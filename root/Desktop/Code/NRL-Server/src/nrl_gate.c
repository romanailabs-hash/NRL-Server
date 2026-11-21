#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "nrl_gate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*
 * nrl_gate.c — NRL-Gated Admission, Priority & Placement Policy
 *
 * Uses NRL lattice delta score and host load metrics to score
 * incoming workloads. Produces a deterministic ALLOW/DEFER/REJECT
 * decision plus priority lane.
 */

/* -- Internal gate state -- */
static nrl_ctx *g_nrl = NULL;
static gate_stats_t g_stats;
static unsigned long long g_last_load_check = 0;
static double g_host_load_1min = 0.0;
static unsigned long long g_host_mem_total = 0;
static unsigned long long g_host_mem_avail = 0;

/* -- Internal: sample host load -- */
static void gate_sample_host_load(void)
{
    unsigned long long now = (unsigned long long)time(NULL);
    if (now - g_last_load_check < 2) return;  /* Cache for 2 seconds */
    g_last_load_check = now;
    
    /* Read /proc/loadavg */
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        double l1, l5, l15;
        if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            g_host_load_1min = l1;
        }
        fclose(f);
    }
    
    /* Read /proc/meminfo */
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            unsigned long long val;
            if (sscanf(line, "MemTotal: %llu kB", &val) == 1)
                g_host_mem_total = val * 1024;
            if (sscanf(line, "MemAvailable: %llu kB", &val) == 1)
                g_host_mem_avail = val * 1024;
        }
        fclose(f);
    }
}

/* ---- Public API ---- */

void gate_init(nrl_ctx *nrl)
{
    g_nrl = nrl;
    memset(&g_stats, 0, sizeof(g_stats));
    gate_sample_host_load();
}

gate_result_t gate_evaluate(const gate_workload_t *wl)
{
    gate_result_t result;
    memset(&result, 0, sizeof(result));
    
    if (!g_nrl || !wl) {
        result.decision = GATE_REJECT;
        result.gate_score = 0.0;
        return result;
    }
    
    g_stats.total_evaluated++;
    
    /* Sample current host load */
    gate_sample_host_load();
    
    /* Get NRL telemetry to inform scoring */
    nrl_telemetry tel = nrl_get_telemetry(g_nrl);
    
    /* Compute gate score from NRL delta score and host metrics */
    double delta_score = tel.last_delta_score;
    
    /* Normalize host load: 0.0 = idle, 1.0 = very busy */
    double host_load_factor = fmin(1.0, g_host_load_1min / 8.0);
    
    /* Memory pressure factor */
    double mem_factor = 1.0;
    if (g_host_mem_total > 0) {
        double mem_frac = 1.0 - (double)g_host_mem_avail / (double)g_host_mem_total;
        mem_factor = 1.0 + mem_frac * 2.0;  /* 1.0 to 3.0 */
    }
    
    /* Workload request factor: higher mem/cpu demand = more scrutiny */
    double demand_factor = 1.0;
    if (wl->mem_mb > 0 && g_host_mem_avail > 0) {
        double req_frac = (double)(wl->mem_mb * 1048576ULL) / (double)g_host_mem_avail;
        demand_factor = 1.0 + req_frac * 4.0;
    }
    
    /* Combine into final gate score (higher = more likely to be gated) */
    double raw_score = delta_score * 0.3 +
                       host_load_factor * 2.0 +
                       (mem_factor - 1.0) * 0.5 +
                       (demand_factor - 1.0) * 0.2;
    
    /* Normalize to [0, 100] */
    double normalized = fmin(100.0, raw_score * 10.0);
    result.gate_score = normalized;
    
    /* Deterministic noise from NRL sublattice state adds subtle variation */
    double nrl_noise = 0.0;
    if (tel.active_sublattices > 0) {
        double active_ratio = (double)tel.active_sublattices / (double)tel.total_sublattices;
        nrl_noise = active_ratio * 5.0;
    }
    
    double final_score = normalized + nrl_noise;
    
    /* Decision logic */
    result.priority = (int)fmax(0, 100 - (int)final_score);
    
    if (final_score < 25.0) {
        result.decision = GATE_ALLOW_FAST;
        result.lane = 0;
        g_stats.total_allowed_fast++;
    } else if (final_score < 55.0) {
        result.decision = GATE_ALLOW_THROTTLED;
        result.lane = 1;
        g_stats.total_allowed_throttled++;
    } else if (final_score < 80.0) {
        result.decision = GATE_DEFER;
        result.lane = 2;
        g_stats.total_deferred++;
    } else {
        result.decision = GATE_REJECT;
        result.lane = 3;
        g_stats.total_rejected++;
    }
    
    return result;
}

gate_stats_t gate_get_stats(void)
{
    return g_stats;
}

void gate_reset_stats(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}