#ifndef NRL_GATE_H
#define NRL_GATE_H

#include <stdint.h>
#include "nrl_core.h"

#define GATE_WORKLOAD_CMD_MAX 4096
#define GATE_WORKLOAD_ROOTFS_MAX 256
#define GATE_WORKLOAD_HASH_SIZE 32

typedef enum {
    GATE_ALLOW_FAST      = 0,
    GATE_ALLOW_THROTTLED = 1,
    GATE_DEFER           = 2,
    GATE_REJECT          = 3
} gate_decision_t;

typedef struct {
    char cmd[GATE_WORKLOAD_CMD_MAX];
    char rootfs[GATE_WORKLOAD_ROOTFS_MAX];
    unsigned int timeout_ms;
    unsigned int mem_mb;
    unsigned int cpu_quota_pct;
    unsigned int pids_max;
    uint64_t payload_hash;
} gate_workload_t;

typedef struct {
    gate_decision_t decision;
    int priority;
    int lane;
    double gate_score;
} gate_result_t;

typedef struct {
    unsigned long long total_evaluated;
    unsigned long long total_allowed_fast;
    unsigned long long total_allowed_throttled;
    unsigned long long total_deferred;
    unsigned long long total_rejected;
} gate_stats_t;

void gate_init(nrl_ctx *nrl);
gate_result_t gate_evaluate(const gate_workload_t *wl);
gate_stats_t gate_get_stats(void);
void gate_reset_stats(void);

#endif
