#ifndef NRL_SANDBOX_H
#define NRL_SANDBOX_H

#include <stdint.h>
#include <sys/types.h>

typedef enum {
    SANDBOX_PENDING   = 0,
    SANDBOX_RUNNING   = 1,
    SANDBOX_COMPLETED = 2,
    SANDBOX_FAILED    = 3,
    SANDBOX_TIMEOUT   = 4,
    SANDBOX_CLEANED   = 5
} sandbox_status_t;

typedef struct {
    char job_id[64];
    char *cmd;
    char *rootfs;
    char *hostname;
    unsigned int timeout_ms;
    unsigned int mem_mb;
    unsigned int cpu_quota_pct;
    unsigned int pids_max;
} sandbox_job_t;

typedef struct {
    char job_id[64];
    pid_t pid;
    char *cmd;
    char *rootfs;
    unsigned int timeout_ms;
    unsigned int mem_mb;
    unsigned int cpu_quota_pct;
    unsigned int pids_max;
    int pipe_stdout;
    int pipe_stderr;
    sandbox_status_t status;
    int exit_status;
    unsigned int duration_ms;
    char *stdout;
    char *stderr;
} sandbox_handle;

sandbox_handle *sandbox_spawn(sandbox_job_t *job);
int sandbox_wait(sandbox_handle *handle, unsigned int timeout_ms);
void sandbox_cleanup(sandbox_handle *handle);

#endif
