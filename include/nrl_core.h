#ifndef NRL_CORE_H
#define NRL_CORE_H

#include <stdint.h>
#include <stddef.h>

typedef struct nrl_ctx nrl_ctx;

typedef struct {
    unsigned long long steps;
    unsigned long long virtual_steps;
    unsigned int active_sublattices;
    unsigned int total_sublattices;
    double skip_ratio;
    uint64_t state_hash;
    double last_delta_score;
} nrl_telemetry;

typedef struct {
    size_t size;
    uint8_t data[];
} nrl_state;

nrl_ctx *nrl_create(unsigned int seed, unsigned int total_sublattices, unsigned int lattice_w);
void nrl_free(nrl_ctx *ctx);
void nrl_tick(nrl_ctx *ctx, unsigned long long steps, unsigned int input_seed);
nrl_telemetry nrl_get_telemetry(const nrl_ctx *ctx);
uint64_t nrl_state_hash(const nrl_ctx *ctx);
nrl_state *nrl_save_state(const nrl_ctx *ctx, size_t *out_size);
nrl_ctx *nrl_load_state(const nrl_state *state);
double nrl_delta_score(const nrl_ctx *ctx, unsigned int pos_a, unsigned int pos_b);

#endif
