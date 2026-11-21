#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

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
#include <sched.h>
#include <grp.h>

/*
 * nrl_sandbox.c — Custom Linux Container / Micro-Jail Runtime
 *
 * Uses native Linux kernel isolation:
 *   - namespaces (CLONE_NEWPID, CLONE_NEWNS, CLONE_NEWUTS, CLONE_NEWIPC)
 *   - pivot_root into minimal rootfs
 *   - cgroups v2 for resource limits
 *   - Bounded stdout/stderr capture
 *   - Process reaping & timeout enforcement
 */

/* -- Internal helpers -- */
static int sandbox_write_cgroup(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = (int)write(fd, value, strlen(value));
    close(fd);
    return rc < 0 ? -1 : 0;
}

static int sandbox_create_cgroup(const char *job_id, unsigned int mem_mb,
                                  unsigned int cpu_quota_pct,
                                  unsigned int pids_max)
{
    char path[512];
    int ret = snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s", job_id);
    if (ret < 0 || (size_t)ret >= sizeof(path)) return -1;

    /* Check if cgroups v2 is available */
    if (access("/sys/fs/cgroup/cgroup.controllers", F_OK) != 0) {
        fprintf(stderr, "[sandbox] cgroups v2 not available, skipping resource limits\n");
        return 0; /* Degrade gracefully */
    }

    /* Create the cgroup directory */
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[sandbox] mkdir cgroup %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Set memory limit */
    if (mem_mb > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%u", mem_mb * 1024 * 1024);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/memory.max", job_id);
        sandbox_write_cgroup(path, val);
    }

    /* Set CPU quota: cpu.max expects "quota period" in us */
    if (cpu_quota_pct > 0 && cpu_quota_pct <= 100) {
        unsigned int quota_us = (unsigned int)((double)cpu_quota_pct * 100.0); /* 100% = 100000 us */
        char val[64];
        snprintf(val, sizeof(val), "%u 100000", quota_us);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/cpu.max", job_id);
        sandbox_write_cgroup(path, val);
    }

    /* Set PIDs limit */
    if (pids_max > 0) {
        char val[64];
        snprintf(val, sizeof(val), "%u", pids_max);
        snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/pids.max", job_id);
        sandbox_write_cgroup(path, val);
    }

    return 0;
}

static int sandbox_add_pid_to_cgroup(const char *job_id, pid_t pid)
{
    char path[512];
    int ret = snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s/cgroup.procs", job_id);
    if (ret < 0 || (size_t)ret >= sizeof(path)) return -1;

    if (access(path, F_OK) != 0) return 0; /* No cgroup, skip */

    char val[32];
    snprintf(val, sizeof(val), "%d", (int)pid);
    return sandbox_write_cgroup(path, val);
}

static void sandbox_remove_cgroup(const char *job_id)
{
    char path[512];
    int ret = snprintf(path, sizeof(path), "/sys/fs/cgroup/nrl-server/%s", job_id);
    if (ret < 0 || (size_t)ret >= sizeof(path)) return;
    rmdir(path); /* Best effort */
}

/* -- Namespace isolation function (runs in child after clone) -- */
static int sandbox_child(void *arg)
{
    sandbox_job_t *job = (sandbox_job_t *)arg;

    /* Make mount propagation private */
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        fprintf(stderr, "[sandbox] MS_PRIVATE mount: %s\n", strerror(errno));
        return 1;
    }

    /* Bind-mount new rootfs if specified */
    if (job->rootfs && job->rootfs[0] != '\0') {
        /* Prepare: bind mount rootfs to a temp location for pivot */
        char newroot[512];
        snprintf(newroot, sizeof(newroot), "%s", job->rootfs);

        /* Bind mount rootfs over itself to make it a mount point */
        if (mount(newroot, newroot, NULL, MS_BIND | MS_REC, NULL) != 0) {
            fprintf(stderr, "[sandbox] bind mount rootfs: %s\n", strerror(errno));
            return 1;
        }

        /* Create old root directory */
        if (mkdir("/oldroot", 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[sandbox] mkdir /oldroot: %s\n", strerror(errno));
            return 1;
        }

        /* pivot_root: swap to new rootfs */
        if (syscall(SYS_pivot_root, newroot, "/oldroot") != 0) {
            fprintf(stderr, "[sandbox] pivot_root: %s\n", strerror(errno));
            return 1;
        }

        /* chdir to new root */
        if (chdir("/") != 0) {
            fprintf(stderr, "[sandbox] chdir /: %s\n", strerror(errno));
            return 1;
        }

        /* Unmount old root */
        if (umount2("/oldroot", MNT_DETACH) != 0) {
            fprintf(stderr, "[sandbox] umount oldroot: %s\n", strerror(errno));
            /* Non-fatal, continue */
        }
        rmdir("/oldroot");
    }

    /* Mount /proc */
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        fprintf(stderr, "[sandbox] mount /proc: %s\n", strerror(errno));
        /* Non-fatal for some commands */
    }

    /* Mount /sys read-only */
    if (mount("sysfs", "/sys", "sysfs", MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        /* Optional, ignore failure */
    }

    /* Mount /tmp as tmpfs */
    if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV,
              "mode=1777,size=16M") != 0) {
        /* Optional */
    }

    /* Set hostname */
    if (job->hostname && job->hostname[0]) {
        sethostname(job->hostname, strlen(job->hostname));
    }

    /* Drop capabilities (bounding set) */
    {
        struct __user_cap_header_struct cap_hdr = {
            .version = _LINUX_CAPABILITY_VERSION_3,
            .pid = 0
        };
        struct __user_cap_data_struct cap_data[2] = {{0}};
        capset(&cap_hdr, cap_data);
    }

    /* Become nobody:nogroup if possible */
    {
        struct passwd *pw = getpwnam("nobody");
        if (pw) {
            setgroups(0, NULL);
            setgid(pw->pw_gid);
            setuid(pw->pw_uid);
        }
    }

    /* Execute the command */
    if (job->cmd && job->cmd[0]) {
        /* Build argv from cmd string (space-separated) */
        char *cmd_copy = strdup(job->cmd);
        if (!cmd_copy) return 1;

        /* Count arguments */
        int argc = 1;
        for (char *p = cmd_copy; *p; p++) if (*p == ' ') argc++;

        char **argv = calloc((size_t)argc + 1, sizeof(char *));
        if (!argv) { free(cmd_copy); return 1; }

        argv[0] = cmd_copy;
        int i = 1;
        for (char *p = cmd_copy; *p; p++) {
            if (*p == ' ') {
                *p = '\0';
                argv[i++] = p + 1;
            }
        }
        argv[argc] = NULL;

        execvp(argv[0], argv);
        /* If we get here, exec failed */
        fprintf(stderr, "[sandbox] exec %s: %s\n", argv[0], strerror(errno));
        free(argv);
        free(cmd_copy);
        return 1;
    }

    return 0;
}

/* ---- Public API ---- */

sandbox_handle *sandbox_spawn(sandbox_job_t *job)
{
    if (!job) return NULL;

    sandbox_handle *handle = calloc(1, sizeof(sandbox_handle));
    if (!handle) return NULL;

    /* Generate a unique job ID */
    static unsigned long long id_counter = 0;
    snprintf(handle->job_id, sizeof(handle->job_id), "job-%llu-%d",
             (unsigned long long)++id_counter, (int)getpid());

    /* Capture job parameters */
    if (job->cmd) handle->cmd = strdup(job->cmd);
    if (job->rootfs) handle->rootfs = strdup(job->rootfs);
    handle->timeout_ms = job->timeout_ms;
    handle->mem_mb = job->mem_mb;
    handle->cpu_quota_pct = job->cpu_quota_pct;
    handle->pids_max = job->pids_max;
    handle->status = SANDBOX_PENDING;

    /* Create cgroup for resource limits */
    sandbox_create_cgroup(handle->job_id, job->mem_mb, job->cpu_quota_pct, job->pids_max);

    /* Create stdout/stderr capture pipes */
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        sandbox_cleanup(handle);
        return NULL;
    }

    /* Allocate stack for clone */
    size_t stack_size = 2 * 1024 * 1024; /* 2 MB */
    char *stack = (char *)malloc(stack_size);
    if (!stack) {
        sandbox_cleanup(handle);
        return NULL;
    }
    char *stack_top = stack + stack_size;

    /* Clone with namespaces */
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
    pid_t pid = (pid_t)syscall(SYS_clone, sandbox_child, stack_top, flags, handle);
    if (pid < 0) {
        fprintf(stderr, "[sandbox] clone: %s\n", strerror(errno));
        free(stack);
        sandbox_cleanup(handle);
        return NULL;
    }

    handle->pid = pid;
    handle->status = SANDBOX_RUNNING;

    /* Close write ends in parent */
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    handle->pipe_stdout = stdout_pipe[0];
    handle->pipe_stderr = stderr_pipe[0];

    /* Add PID to cgroup */
    sandbox_add_pid_to_cgroup(handle->job_id, pid);

    return handle;
}

int sandbox_wait(sandbox_handle *handle, unsigned int timeout_ms)
{
    if (!handle || handle->pid <= 0) return -1;

    /* Read stdout/stderr into bounded buffers */
    #define CAPTURE_MAX 262144  /* 256 KiB max per stream */
    char *stdout_buf = calloc(1, CAPTURE_MAX + 1);
    char *stderr_buf = calloc(1, CAPTURE_MAX + 1);
    size_t stdout_len = 0, stderr_len = 0;

    /* Set pipes non-blocking for timeout handling */
    int flags_stdout = fcntl(handle->pipe_stdout, F_GETFL, 0);
    int flags_stderr = fcntl(handle->pipe_stderr, F_GETFL, 0);
    fcntl(handle->pipe_stdout, F_SETFL, flags_stdout | O_NONBLOCK);
    fcntl(handle->pipe_stderr, F_SETFL, flags_stderr | O_NONBLOCK);

    /* Wait with timeout */
    int status;
    pid_t wpid;
    unsigned long long deadline = (unsigned long long)time(NULL) * 1000ULL +
                                   (unsigned long long)timeout_ms;

    /* Read pipe data while process runs */
    do {
        /* Check timeout */
        if (timeout_ms > 0) {
            unsigned long long now = (unsigned long long)time(NULL) * 1000ULL;
            if (now >= deadline) {
                kill(handle->pid, SIGKILL);
                handle->exit_status = -1;
                handle->status = SANDBOX_TIMEOUT;
                break;
            }
        }

        /* Read from pipes */
        if (handle->pipe_stdout >= 0) {
            ssize_t n = read(handle->pipe_stdout, stdout_buf + stdout_len,
                             CAPTURE_MAX - stdout_len);
            if (n > 0) stdout_len += (size_t)n;
        }
        if (handle->pipe_stderr >= 0) {
            ssize_t n = read(handle->pipe_stderr, stderr_buf + stderr_len,
                             CAPTURE_MAX - stderr_len);
            if (n > 0) stderr_len += (size_t)n;
        }

        /* Try non-blocking waitpid */
        wpid = waitpid(handle->pid, &status, WNOHANG);
        if (wpid == handle->pid) {
            if (WIFEXITED(status)) handle->exit_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) handle->exit_status = 128 + WTERMSIG(status);
            else handle->exit_status = -1;
            break;
        }
        usleep(1000); /* 1 ms polling */
    } while (1);

    /* Final read of any remaining data */
    if (handle->pipe_stdout >= 0) {
        ssize_t n;
        do {
            n = read(handle->pipe_stdout, stdout_buf + stdout_len,
                     CAPTURE_MAX - stdout_len);
            if (n > 0) stdout_len += (size_t)n;
        } while (n > 0);
    }
    if (handle->pipe_stderr >= 0) {
        ssize_t n;
        do {
            n = read(handle->pipe_stderr, stderr_buf + stderr_len,
                     CAPTURE_MAX - stderr_len);
            if (n > 0) stderr_len += (size_t)n;
        } while (n > 0);
    }

    /* Store captured output */
    if (stdout_len > 0) handle->stdout = stdout_buf;
    else free(stdout_buf);
    if (stderr_len > 0) handle->stderr = stderr_buf;
    else free(stderr_buf);

    handle->duration_ms = (unsigned int)(deadline - ((unsigned long long)time(NULL) * 1000ULL));

    if (handle->status != SANDBOX_TIMEOUT) {
        handle->status = (handle->exit_status == 0) ? SANDBOX_COMPLETED : SANDBOX_FAILED;
    }

    /* Close pipes */
    if (handle->pipe_stdout >= 0) close(handle->pipe_stdout);
    if (handle->pipe_stderr >= 0) close(handle->pipe_stderr);
    handle->pipe_stdout = -1;
    handle->pipe_stderr = -1;

    return 0;
}

void sandbox_cleanup(sandbox_handle *handle)
{
    if (!handle) return;
    if (handle->pid > 0) {
        kill(handle->pid, SIGKILL);
        waitpid(handle->pid, NULL, WNOHANG);
    }
    sandbox_remove_cgroup(handle->job_id);
    if (handle->pipe_stdout >= 0) close(handle->pipe_stdout);
    if (handle->pipe_stderr >= 0) close(handle->pipe_stderr);
    free(handle->stdout);
    free(handle->stderr);
    free(handle->cmd);
    free(handle->rootfs);
    handle->pid = 0;
    handle->status = SANDBOX_CLEANED;
}