
#include "nrl_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

struct nrl_ctx {
    unsigned int seed;
    unsigned int total_sublattices;
    unsigned int lattice_w;
    unsigned int packed_w;
    uint8_t *lattice;
    uint8_t *delta;
    uint8_t *active_flags;
    unsigned long long steps;
    unsigned long long virtual_steps;
    unsigned int active_sublattices;
    double skip_ratio;
    double last_delta_score;
    int use_avx2;
};

static void nrl_update_sublattice_scalar(nrl_ctx *ctx, unsigned int sl_idx, unsigned int input_seed) {
    (void)sl_idx;
    uint8_t *slice = ctx->lattice + (size_t)sl_idx * ctx->packed_w;
    unsigned int rng = input_seed;
    for (unsigned int i = 0; i < ctx->packed_w; i++) {
        rng = (rng * 1103515245U + 12345U) & 0x7fffffffU;
        uint8_t lo = slice[i] & 0x0F;
        uint8_t hi = (slice[i] >> 4) & 0x0F;
        int new_lo = (int)lo + (int)((rng >> 0) & 0x03) - 1;
        int new_hi = (int)hi + (int)((rng >> 2) & 0x03) - 1;
        if (new_lo < 0) new_lo = 0;
        if (new_lo > 15) new_lo = 15;
        if (new_hi < 0) new_hi = 0;
        if (new_hi > 15) new_hi = 15;
        slice[i] = (uint8_t)((new_hi << 4) | new_lo);
    }
}

nrl_ctx *nrl_create(unsigned int seed, unsigned int total_sublattices, unsigned int lattice_w) {
    if (total_sublattices == 0 || lattice_w == 0 || lattice_w % 2 != 0) return NULL;
    if (total_sublattices > 65536 || lattice_w > 256) return NULL;
    nrl_ctx *ctx = (nrl_ctx *)calloc(1, sizeof(nrl_ctx));
    if (!ctx) return NULL;
    ctx->seed = seed;
    ctx->total_sublattices = total_sublattices;
    ctx->lattice_w = lattice_w;
    ctx->packed_w = lattice_w / 2;
    size_t slice_bytes = (size_t)ctx->packed_w;
    size_t total_bytes = (size_t)total_sublattices * slice_bytes;
    ctx->lattice = (uint8_t *)calloc(1, total_bytes);
    ctx->delta = (uint8_t *)calloc(1, total_bytes);
    ctx->active_flags = (uint8_t *)calloc(1, total_sublattices);
    if (!ctx->lattice || !ctx->delta || !ctx->active_flags) {
        free(ctx->lattice); free(ctx->delta); free(ctx->active_flags); free(ctx);
        return NULL;
    }
    unsigned int rng = seed;
    for (size_t i = 0; i < total_bytes; i++) {
        rng = (rng * 1103515245U + 12345U) & 0x7fffffffU;
        uint8_t lo = (uint8_t)((rng >> 0) & 0x0F);
        uint8_t hi = (uint8_t)((rng >> 4) & 0x0F);
        ctx->lattice[i] = (hi << 4) | lo;
    }
    ctx->use_avx2 = 0; /* Scalar fallback for portability */
    return ctx;
}

void nrl_free(nrl_ctx *ctx) {
    if (ctx) {
        free(ctx->lattice); free(ctx->delta); free(ctx->active_flags);
        free(ctx);
    }
}

void nrl_tick(nrl_ctx *ctx, unsigned long long steps, unsigned int input_seed) {
    if (!ctx || steps == 0) return;
    unsigned int active_count = 0;
    unsigned long long virtual_steps_acc = 0;
    for (unsigned long long s = 0; s < steps; s++) {
        unsigned int combined_seed = input_seed ^ (unsigned int)(s & 0xFFFFFFFF);
        for (unsigned int sl = 0; sl < ctx->total_sublattices; sl++) {
            unsigned int sl_seed = combined_seed ^ (sl * 2654435761U);
            uint8_t *slice = ctx->lattice + (size_t)sl * ctx->packed_w;
            memcpy(ctx->delta, slice, ctx->packed_w);
            nrl_update_sublattice_scalar(ctx, sl, sl_seed);
            int changed = 0;
            for (unsigned int i = 0; i < ctx->packed_w; i++) {
                if (ctx->delta[i] != slice[i]) { changed = 1; break; }
            }
            ctx->active_flags[sl] = (uint8_t)changed;
            active_count += (unsigned int)changed;
            virtual_steps_acc += (unsigned long long)(changed ? 1 : 0);
        }
    }
    ctx->steps += steps;
    ctx->virtual_steps += virtual_steps_acc;
    ctx->active_sublattices = active_count;
    ctx->skip_ratio = (ctx->steps > 0) ? (1.0 - (double)ctx->virtual_steps / (double)(ctx->steps * ctx->total_sublattices)) : 0.0;
    double total_delta = 0.0;
    int pairs = 0;
    for (unsigned int i = 0; i < ctx->total_sublattices && pairs < 64; i++) {
        unsigned int j = (i * 2654435761U + 12345U) % ctx->total_sublattices;
        if (i != j) { total_delta += nrl_delta_score(ctx, i, j); pairs++; }
    }
    ctx->last_delta_score = (pairs > 0) ? total_delta / pairs : 0.0;
}

nrl_telemetry nrl_get_telemetry(const nrl_ctx *ctx) {
    nrl_telemetry t = {0,0,0,0,0.0,0,0.0};
    if (!ctx) return t;
    t.steps = ctx->steps;
    t.virtual_steps = ctx->virtual_steps;
    t.active_sublattices = ctx->active_sublattices;
    t.total_sublattices = ctx->total_sublattices;
    t.skip_ratio = ctx->skip_ratio;
    t.state_hash = nrl_state_hash(ctx);
    t.last_delta_score = ctx->last_delta_score;
    return t;
}

uint64_t nrl_state_hash(const nrl_ctx *ctx) {
    if (!ctx || !ctx->lattice) return 0;
    size_t total_bytes = (size_t)ctx->total_sublattices * ctx->packed_w;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < total_bytes; i++) { h ^= (uint64_t)ctx->lattice[i]; h *= 1099511628211ULL; }
    h ^= (uint64_t)ctx->steps; h *= 1099511628211ULL;
    h ^= (uint64_t)ctx->virtual_steps; h *= 1099511628211ULL;
    h ^= (uint64_t)ctx->active_sublattices; h *= 1099511628211ULL;
    h ^= (uint64_t)ctx->seed; h *= 1099511628211ULL;
    return h;
}

nrl_state *nrl_save_state(const nrl_ctx *ctx, size_t *out_size) {
    if (!ctx) { if (out_size) *out_size = 0; return NULL; }
    size_t lattice_bytes = (size_t)ctx->total_sublattices * ctx->packed_w;
    size_t hdr_size = 8;
    size_t state_size = sizeof(nrl_state) + hdr_size + lattice_bytes;
    if (state_size > 4*1024*1024) { if (out_size) *out_size = 0; return NULL; }
    nrl_state *state = (nrl_state *)malloc(state_size);
    if (!state) { if (out_size) *out_size = 0; return NULL; }
    state->size = hdr_size + lattice_bytes;
    state->data[0] = (uint8_t)(ctx->total_sublattices & 0xFF);
    state->data[1] = (uint8_t)((ctx->total_sublattices >> 8) & 0xFF);
    state->data[2] = (uint8_t)((ctx->total_sublattices >> 16) & 0xFF);
    state->data[3] = (uint8_t)((ctx->total_sublattices >> 24) & 0xFF);
    state->data[4] = (uint8_t)(ctx->lattice_w & 0xFF);
    state->data[5] = (uint8_t)((ctx->lattice_w >> 8) & 0xFF);
    state->data[6] = (uint8_t)((ctx->lattice_w >> 16) & 0xFF);
    state->data[7] = (uint8_t)((ctx->lattice_w >> 24) & 0xFF);
    memcpy(state->data + 8, ctx->lattice, lattice_bytes);
    if (out_size) *out_size = state_size;
    return state;
}

nrl_ctx *nrl_load_state(const nrl_state *state) {
    if (!state || state->size < 8) return NULL;
    unsigned int total_sublattices = (unsigned int)(state->data[0] | ((unsigned int)state->data[1] << 8) | ((unsigned int)state->data[2] << 16) | ((unsigned int)state->data[3] << 24));
    unsigned int lattice_w = (unsigned int)(state->data[4] | ((unsigned int)state->data[5] << 8) | ((unsigned int)state->data[6] << 16) | ((unsigned int)state->data[7] << 24));
    size_t expected = (size_t)total_sublattices * (lattice_w / 2);
    if (state->size != 8 + expected) return NULL;
    nrl_ctx *ctx = nrl_create(0, total_sublattices, lattice_w);
    if (!ctx) return NULL;
    memcpy(ctx->lattice, state->data + 8, state->size - 8);
    return ctx;
}

double nrl_delta_score(const nrl_ctx *ctx, unsigned int pos_a, unsigned int pos_b) {
    if (!ctx || pos_a >= ctx->total_sublattices || pos_b >= ctx->total_sublattices) return 0.0;
    uint8_t *sl_a = ctx->lattice + (size_t)pos_a * ctx->packed_w;
    uint8_t *sl_b = ctx->lattice + (size_t)pos_b * ctx->packed_w;
    double score = 0.0;
    for (unsigned int i = 0; i < ctx->packed_w; i++) {
        int diff = (int)(sl_a[i] & 0x0F) - (int)(sl_b[i] & 0x0F);
        score += (double)(diff * diff);
        diff = (int)((sl_a[i] >> 4) & 0x0F) - (int)((sl_b[i] >> 4) & 0x0F);
        score += (double)(diff * diff);
    }
    return sqrt(score);
}
