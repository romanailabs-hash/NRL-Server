
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "nrl_core.h"
#include "nrl_gate.h"
#include "nrl_sandbox.h"
#include "nrl_http.h"
#include "nrl_server.h"

static http_server_t *g_server = NULL;
static nrl_ctx *g_nrl_ctx = NULL;

static void handle_signal(int sig) {
    fprintf(stderr, "[nrl-serverd] Signal %d received, shutting down...\n", sig);
    if (g_server) http_server_stop(g_server);
}

static char *on_health(void *data) {
    (void)data;
    char buf[256];
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"service\":\"nrl-server\",\"version\":\"%s\",\"uptime_sec\":%llu}",
        NRL_SERVER_VERSION,
        (unsigned long long)(ts.tv_sec));
    return strdup(buf);
}

static char *on_stats(void *data) {
    (void)data;
    if (!g_nrl_ctx) return strdup("{\"error\":\"nrl not initialized\"}");
    nrl_telemetry tel = nrl_get_telemetry(g_nrl_ctx);
    gate_stats_t gs = gate_get_stats();
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"nrl\":{\"steps\":%llu,\"virtual_steps\":%llu,\"active_sublattices\":%u,\"total_sublattices\":%u,\"skip_ratio\":%.4f,\"state_hash\":\"%016llx\",\"last_delta_score\":%.4f},"
        "\"gate\":{\"total_evaluated\":%llu,\"allowed_fast\":%llu,\"allowed_throttled\":%llu,\"deferred\":%llu,\"rejected\":%llu}}",
        (unsigned long long)tel.steps, (unsigned long long)tel.virtual_steps,
        tel.active_sublattices, tel.total_sublattices,
        tel.skip_ratio, (unsigned long long)tel.state_hash, tel.last_delta_score,
        (unsigned long long)gs.total_evaluated,
        (unsigned long long)gs.total_allowed_fast,
        (unsigned long long)gs.total_allowed_throttled,
        (unsigned long long)gs.total_deferred,
        (unsigned long long)gs.total_rejected);
    return strdup(buf);
}

static char *on_tick(void *data) {
    (void)data;
    if (!g_nrl_ctx) return strdup("{\"error\":\"nrl not initialized\"}");
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    nrl_tick(g_nrl_ctx, 10000, 42);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    unsigned long long elapsed_us = (unsigned long long)(t2.tv_sec - t1.tv_sec) * 1000000ULL + (unsigned long long)(t2.tv_nsec - t1.tv_nsec) / 1000ULL;
    nrl_telemetry tel = nrl_get_telemetry(g_nrl_ctx);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"steps\":10000,\"virtual_steps\":%llu,\"exec_time_us\":%llu,\"skip_ratio\":%.4f,\"state_hash\":\"%016llx\",\"active_sublattices\":%u}",
        (unsigned long long)tel.virtual_steps, elapsed_us,
        tel.skip_ratio, (unsigned long long)tel.state_hash, tel.active_sublattices);
    return strdup(buf);
}

static char *on_run(void *data) {
    (void)data;
    if (!g_nrl_ctx) return strdup("{\"error\":\"nrl not initialized\"}");
    sandbox_job_t job;
    memset(&job, 0, sizeof(job));
    job.cmd = "/bin/echo hello";
    job.rootfs = "";
    job.timeout_ms = 5000;
    job.mem_mb = 128;
    job.cpu_quota_pct = 50;
    job.pids_max = 64;
    gate_workload_t wl;
    memset(&wl, 0, sizeof(wl));
    strncpy(wl.cmd, job.cmd, sizeof(wl.cmd)-1);
    wl.mem_mb = job.mem_mb;
    wl.cpu_quota_pct = job.cpu_quota_pct;
    wl.pids_max = job.pids_max;
    wl.timeout_ms = job.timeout_ms;
    gate_result_t gate_res = gate_evaluate(&wl);
    sandbox_handle *h = sandbox_spawn(&job);
    if (!h) return strdup("{\"error\":\"sandbox_spawn failed\"}");
    sandbox_wait(h, 5000);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"job_id\":\"%s\",\"gate_decision\":%d,\"exit_code\":%d,\"duration_ms\":%u,\"stdout\":\"%s\",\"stderr\":\"%s\"}",
        h->job_id, gate_res.decision, h->exit_status, h->duration_ms,
        h->stdout ? h->stdout : "", h->stderr ? h->stderr : "");
    sandbox_cleanup(h); free(h);
    return strdup(buf);
}

static char *on_jobs(void *data) {
    (void)data;
    return strdup("{\"jobs\":[],\"message\":\"job tracking not yet implemented\"}");
}

int main(int argc, char **argv) {
    unsigned short port = 8080;
    const char *bind_addr = "127.0.0.1";
    const char *token = NULL;
    unsigned int seed = 42;
    unsigned int sublattices = 1024;
    unsigned int lattice_w = 64;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--port <port>] [--bind <addr>] [--token <tok>]\n", argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--port") == 0 && i+1 < argc) port = (unsigned short)atoi(argv[++i]);
        else if (strcmp(argv[i], "--bind") == 0 && i+1 < argc) bind_addr = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i+1 < argc) token = argv[++i];
        else if (strcmp(argv[i], "--seed") == 0 && i+1 < argc) seed = (unsigned int)atoi(argv[++i]);
        else if (strcmp(argv[i], "--sublattices") == 0 && i+1 < argc) sublattices = (unsigned int)atoi(argv[++i]);
        else if (strcmp(argv[i], "--lattice-width") == 0 && i+1 < argc) lattice_w = (unsigned int)atoi(argv[++i]);
    }
    if (!token) token = getenv("NRL_SERVER_TOKEN");
    
    printf("NRL-Server v1 starting...\n");
    printf("  Bind: %s:%d\n", bind_addr, port);
    
    g_nrl_ctx = nrl_create(seed, sublattices, lattice_w);
    if (!g_nrl_ctx) { fprintf(stderr, "Failed to create NRL context\n"); return 1; }
    printf("  NRL context created, hash=%016llx\n", (unsigned long long)nrl_state_hash(g_nrl_ctx));
    
    gate_init(g_nrl_ctx);
    signal(SIGINT, handle_signal); signal(SIGTERM, handle_signal); signal(SIGPIPE, SIG_IGN);
    
    g_server = http_server_create(bind_addr, port, token);
    if (!g_server) { fprintf(stderr, "Failed to create HTTP server\n"); nrl_free(g_nrl_ctx); return 1; }
    
    http_server_handlers_t handlers = { .on_health = on_health, .on_stats = on_stats, .on_tick = on_tick, .on_run = on_run, .on_jobs = on_jobs };
    http_server_set_callbacks(g_server, &handlers, NULL);
    
    printf("  Ready on %s:%d\n", bind_addr, port);
    int rc = http_server_run(g_server);
    
    printf("\nShutting down...\n");
    http_server_destroy(g_server);
    nrl_free(g_nrl_ctx);
    return rc;
}
