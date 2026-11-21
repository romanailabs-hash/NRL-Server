
#include "nrl_sandbox.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sched.h>

#include <time.h>
static int sandbox_write_cgroup(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = (int)write(fd, value, strlen(value));
    close(fd);
    return rc < 0 ? -1 : 0;
}

static int sandbox_create_cgroup(const char *job_id, unsigned int mem_mb, unsigned int cpu_quota_pct, unsigned int pids_max) {
    char path[512];
    if (access("/sys/fs/cgroup/cgroup.controllers", F_OK) != 0) return 0;
    snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s", job_id);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
    if (mem_mb > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%u", mem_mb * 1024 * 1024);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/memory.max", job_id);
        sandbox_write_cgroup(path, val);
    }
    if (cpu_quota_pct > 0 && cpu_quota_pct <= 100) {
        char val[64];
        snprintf(val, sizeof(val), "%u 100000", (unsigned int)(cpu_quota_pct * 100.0));
        snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/cpu.max", job_id);
        sandbox_write_cgroup(path, val);
    }
    if (pids_max > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%u", pids_max);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/pids.max", job_id);
        sandbox_write_cgroup(path, val);
    }
    return 0;
}

static int sandbox_add_pid_to_cgroup(const char *job_id, pid_t pid) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/cgroup.procs", job_id);
    if (access(path, F_OK) != 0) return 0;
    char val[32];
    snprintf(val, sizeof(val), "%d", (int)pid);
    return sandbox_write_cgroup(path, val);
}

static void sandbox_remove_cgroup(const char *job_id) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s", job_id);
    rmdir(path);
}

static int sandbox_child(void *arg) {
    sandbox_handle *handle = (sandbox_handle *)arg;
    mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (handle->rootfs && handle->rootfs[0] != '\0') {
        if (mount(handle->rootfs, handle->rootfs, NULL, MS_BIND | MS_REC, NULL) != 0) return 1;
        mkdir("/oldroot", 0755);
        if (syscall(SYS_pivot_root, handle->rootfs, "/oldroot") != 0) return 1;
        if (chdir("/") != 0) { perror("chdir"); return 1; }
        umount2("/oldroot", MNT_DETACH);
        rmdir("/oldroot");
    }
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount("sysfs", "/sys", "sysfs", MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
    mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=16M");
    if (handle->cmd && handle->cmd[0]) {
        size_t cmdlen __attribute__((unused)) = strlen(handle->cmd);
        char *cmd_copy = strdup(handle->cmd);
        if (!cmd_copy) return 1;
        int argc = 1;
        for (char *p = cmd_copy; *p; p++) if (*p == ' ') argc++;
        char **argv = calloc((size_t)argc + 1, sizeof(char *));
        if (!argv) { free(cmd_copy); return 1; }
        argv[0] = cmd_copy;
        int i = 1;
        for (char *p = cmd_copy; *p; p++) { if (*p == ' ') { *p = '\0'; argv[i++] = p + 1; } }
        argv[argc] = NULL;
        execvp(argv[0], argv);
        free(argv); free(cmd_copy);
        return 1;
    }
    return 0;
}

sandbox_handle *sandbox_spawn(sandbox_job_t *job) {
    if (!job) return NULL;
    sandbox_handle *handle = calloc(1, sizeof(sandbox_handle));
    if (!handle) return NULL;
    static unsigned long long id_counter = 0;
    snprintf(handle->job_id, sizeof(handle->job_id), "job-%llu-%d", (unsigned long long)++id_counter, (int)getpid());
    if (job->cmd) handle->cmd = strdup(job->cmd);
    if (job->rootfs) handle->rootfs = strdup(job->rootfs);
    handle->timeout_ms = job->timeout_ms;
    handle->mem_mb = job->mem_mb;
    handle->cpu_quota_pct = job->cpu_quota_pct;
    handle->pids_max = job->pids_max;
    handle->status = SANDBOX_PENDING;
    sandbox_create_cgroup(handle->job_id, job->mem_mb, job->cpu_quota_pct, job->pids_max);
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) { sandbox_cleanup(handle); return NULL; }
    size_t stack_size = 2 * 1024 * 1024;
    char *stack = (char *)malloc(stack_size);
    if (!stack) { sandbox_cleanup(handle); return NULL; }
    char *stack_top = stack + stack_size;
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
    pid_t pid = (pid_t)syscall(SYS_clone, sandbox_child, stack_top, flags, handle);
    if (pid < 0) { free(stack); sandbox_cleanup(handle); return NULL; }
    handle->pid = pid;
    handle->status = SANDBOX_RUNNING;
    close(stdout_pipe[1]); close(stderr_pipe[1]);
    handle->pipe_stdout = stdout_pipe[0];
    handle->pipe_stderr = stderr_pipe[0];
    sandbox_add_pid_to_cgroup(handle->job_id, pid);
    return handle;
}

int sandbox_wait(sandbox_handle *handle, unsigned int timeout_ms) {
    if (!handle || handle->pid <= 0) return -1;
    #define CAPTURE_MAX 262144
    char *stdout_buf = calloc(1, CAPTURE_MAX + 1);
    char *stderr_buf = calloc(1, CAPTURE_MAX + 1);
    size_t stdout_len = 0, stderr_len = 0;
    int flags_stdout = fcntl(handle->pipe_stdout, F_GETFL, 0);
    int flags_stderr = fcntl(handle->pipe_stderr, F_GETFL, 0);
    fcntl(handle->pipe_stdout, F_SETFL, flags_stdout | O_NONBLOCK);
    fcntl(handle->pipe_stderr, F_SETFL, flags_stderr | O_NONBLOCK);
    int status;
    pid_t wpid;
    unsigned long long start = (unsigned long long)time(NULL) * 1000ULL;
    unsigned long long deadline = start + (unsigned long long)timeout_ms;
    do {
        unsigned long long now = (unsigned long long)time(NULL) * 1000ULL;
        if (timeout_ms > 0 && now >= deadline) {
            kill(handle->pid, SIGKILL);
            handle->exit_status = -1;
            handle->status = SANDBOX_TIMEOUT;
            handle->duration_ms = (unsigned int)(now - start);
            break;
        }
        if (handle->pipe_stdout >= 0) {
            ssize_t n = read(handle->pipe_stdout, stdout_buf + stdout_len, CAPTURE_MAX - stdout_len);
            if (n > 0) stdout_len += (size_t)n;
        }
        if (handle->pipe_stderr >= 0) {
            ssize_t n = read(handle->pipe_stderr, stderr_buf + stderr_len, CAPTURE_MAX - stderr_len);
            if (n > 0) stderr_len += (size_t)n;
        }
        wpid = waitpid(handle->pid, &status, WNOHANG);
        if (wpid == handle->pid) {
            if (WIFEXITED(status)) handle->exit_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) handle->exit_status = 128 + WTERMSIG(status);
            else handle->exit_status = -1;
            handle->duration_ms = (unsigned int)((unsigned long long)time(NULL) * 1000ULL - start);
            break;
        }
        usleep(1000);
    } while (1);
    if (handle->pipe_stdout >= 0) { ssize_t n; do { n = read(handle->pipe_stdout, stdout_buf + stdout_len, CAPTURE_MAX - stdout_len); if (n > 0) stdout_len += (size_t)n; } while (n > 0); close(handle->pipe_stdout); }
    if (handle->pipe_stderr >= 0) { ssize_t n; do { n = read(handle->pipe_stderr, stderr_buf + stderr_len, CAPTURE_MAX - stderr_len); if (n > 0) stderr_len += (size_t)n; } while (n > 0); close(handle->pipe_stderr); }
    handle->pipe_stdout = -1; handle->pipe_stderr = -1;
    if (stdout_len > 0) handle->stdout = stdout_buf; else free(stdout_buf);
    if (stderr_len > 0) handle->stderr = stderr_buf; else free(stderr_buf);
    if (handle->status != SANDBOX_TIMEOUT) handle->status = (handle->exit_status == 0) ? SANDBOX_COMPLETED : SANDBOX_FAILED;
    return 0;
}

void sandbox_cleanup(sandbox_handle *handle) {
    if (!handle) return;
    if (handle->pid > 0) { kill(handle->pid, SIGKILL); waitpid(handle->pid, NULL, WNOHANG); }
    sandbox_remove_cgroup(handle->job_id);
    if (handle->pipe_stdout >= 0) close(handle->pipe_stdout);
    if (handle->pipe_stderr >= 0) close(handle->pipe_stderr);
    free(handle->stdout); free(handle->stderr);
    free(handle->cmd); free(handle->rootfs);
    handle->pid = 0; handle->status = SANDBOX_CLEANED;
}
