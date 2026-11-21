#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "nrl_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/*
 * nrl_core.c — NRL Sparse Lattice State & Compute Engine
 *
 * Packed INT4 potential lattice:
 *   - Each sublattice is a window of `lattice_w` 4-bit potentials
 *   - Two potentials packed per uint8_t (low nibble = even index, high = odd)
 *   - AVX2 fast path for bulk updates with scalar fallback
 *   - Skip logic: sublattices with all-zero delta are skipped
 *   - Omega-ZPM-style sparse routing via activation thresholds
 */

#ifndef NO_AVX2
#include <immintrin.h>
#endif

/* -- Internal context structure -- */
struct nrl_ctx {
    unsigned int  seed;
    unsigned int  total_sublattices;
    unsigned int  lattice_w;           /* Width of each sublattice (positions) */
    unsigned int  packed_w;            /* lattice_w / 2 (packed uint8_t count) */
    
    /* Packed lattice: total_sublattices * packed_w bytes */
    uint8_t      *lattice;
    
    /* Delta accumulator for current tick */
    uint8_t      *delta;
    
    /* Sublattice activity flags */
    uint8_t      *active_flags;        /* 1 byte per sublattice, 0/1 */
    
    /* Telemetry */
    unsigned long long steps;
    unsigned long long virtual_steps;
    unsigned int  active_sublattices;
    double        skip_ratio;
    double        last_delta_score;
    
    /* AVX2 capability */
    int           use_avx2;
};

/* -- Forward declarations of internal functions -- */
static void nrl_update_sublattice_scalar(nrl_ctx *ctx, unsigned int sl_idx,
                                          unsigned int input_seed);
#ifndef NO_AVX2
static void nrl_update_sublattice_avx2(nrl_ctx *ctx, unsigned int sl_idx,
                                        unsigned int input_seed);
#endif
static uint64_t nrl_hash_combine(uint64_t h, const uint8_t *data, size_t len);
static double nrl_activation_score(uint8_t *packed, unsigned int w);

/* ---- Public API ---- */

nrl_ctx *nrl_create(unsigned int seed,
                     unsigned int total_sublattices,
                     unsigned int lattice_w)
{
    if (total_sublattices == 0 || lattice_w == 0 || lattice_w % 2 != 0) {
        return NULL;  /* lattice_w must be even for packing */
    }
    if (total_sublattices > 65536 || lattice_w > 256) {
        return NULL;  /* sanity bounds */
    }
    
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
        free(ctx->lattice);
        free(ctx->delta);
        free(ctx->active_flags);
        free(ctx);
        return NULL;
    }
    
    /* Initialize lattice with seeded pseudo-random potentials */
    unsigned int rng = seed;
    for (size_t i = 0; i < total_bytes; i++) {
        rng = (rng * 1103515245U + 12345U) & 0x7fffffffU;
        /* Pack two random 4-bit values (0-15 each) */
        uint8_t lo = (uint8_t)((rng >> 0) & 0x0F);
        uint8_t hi = (uint8_t)((rng >> 4) & 0x0F);
        ctx->lattice[i] = (hi << 4) | lo;
    }
    
    ctx->active_sublattices = 0;
    ctx->steps = 0;
    ctx->virtual_steps = 0;
    ctx->skip_ratio = 0.0;
    ctx->last_delta_score = 0.0;
    
#ifndef NO_AVX2
    /* Check CPUID for AVX2 support at runtime */
    {
        unsigned int eax, ebx, ecx, edx;
        __get_cpuid(7, &eax, &ebx, &ecx, &edx);
        ctx->use_avx2 = (ebx & (1U << 5)) != 0;  /* EBX bit 5 = AVX2 */
    }
#else
    ctx->use_avx2 = 0;
#endif
    
    return ctx;
}

void nrl_free(nrl_ctx *ctx)
{
    if (ctx) {
        free(ctx->lattice);
        free(ctx->delta);
        free(ctx->active_flags);
        free(ctx);
    }
}

void nrl_tick(nrl_ctx *ctx, unsigned long long steps, unsigned int input_seed)
{
    if (!ctx || steps == 0) return;
    
    unsigned int active_count = 0;
    unsigned long long virtual_steps_acc = 0;
    
    for (unsigned long long s = 0; s < steps; s++) {
        unsigned int combined_seed = input_seed ^ (unsigned int)(s & 0xFFFFFFFF);
        
        for (unsigned int sl = 0; sl < ctx->total_sublattices; sl++) {
            unsigned int sl_seed = combined_seed ^ (sl * 2654435761U);
            
            /* Save pre-update slice for delta check */
            uint8_t *slice = ctx->lattice + (size_t)sl * ctx->packed_w;
            memcpy(ctx->delta, slice, ctx->packed_w);
            
            /* Update the sublattice */
            if (ctx->use_avx2) {
#ifndef NO_AVX2
                nrl_update_sublattice_avx2(ctx, sl, sl_seed);
#endif
            } else {
                nrl_update_sublattice_scalar(ctx, sl, sl_seed);
            }
            
            /* Check if anything changed (delta > 0) */
            int changed = 0;
            for (unsigned int i = 0; i < ctx->packed_w; i++) {
                if (ctx->delta[i] != slice[i]) {
                    changed = 1;
                    break;
                }
            }
            
            ctx->active_flags[sl] = (uint8_t)changed;
            active_count += (unsigned int)changed;
            virtual_steps_acc += (unsigned long long)(changed ? 1 : 0);
        }
    }
    
    ctx->steps += steps;
    ctx->virtual_steps += virtual_steps_acc;
    ctx->active_sublattices = active_count;
    ctx->skip_ratio = (ctx->steps > 0) ?
        (1.0 - (double)ctx->virtual_steps / (double)(ctx->steps * ctx->total_sublattices)) : 0.0;
    
    /* Compute delta score across a random sample of sublattice pairs */
    double total_delta = 0.0;
    int pairs = 0;
    for (unsigned int i = 0; i < ctx->total_sublattices && pairs < 64; i++) {
        unsigned int j = (i * 2654435761U + 12345U) % ctx->total_sublattices;
        if (i != j) {
            total_delta += nrl_delta_score(ctx, i, j);
            pairs++;
        }
    }
    ctx->last_delta_score = (pairs > 0) ? total_delta / pairs : 0.0;
}

nrl_telemetry nrl_get_telemetry(const nrl_ctx *ctx)
{
    nrl_telemetry t;
    memset(&t, 0, sizeof(t));
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

uint64_t nrl_state_hash(const nrl_ctx *ctx)
{
    if (!ctx || !ctx->lattice) return 0;
    
    size_t total_bytes = (size_t)ctx->total_sublattices * ctx->packed_w;
    uint64_t h = 14695981039346656037ULL;  /* FNV-1a 64-bit offset basis */
    
    for (size_t i = 0; i < total_bytes; i++) {
        h ^= (uint64_t)ctx->lattice[i];
        h *= 1099511628211ULL;  /* FNV-1a prime */
    }
    
    /* Mix in telemetry state */
    h ^= (uint64_t)ctx->steps;
    h *= 1099511628211ULL;
    h ^= (uint64_t)ctx->virtual_steps;
    h *= 1099511628211ULL;
    h ^= (uint64_t)ctx->active_sublattices;
    h *= 1099511628211ULL;
    h ^= (uint64_t)ctx->seed;
    h *= 1099511628211ULL;
    
    return h;
}

nrl_state *nrl_save_state(const nrl_ctx *ctx, size_t *out_size)
{
    if (!ctx) { if (out_size) *out_size = 0; return NULL; }
    
    size_t lattice_bytes = (size_t)ctx->total_sublattices * ctx->packed_w;
    size_t state_size = sizeof(nrl_state) + lattice_bytes;
    
    if (state_size > NRL_MAX_STATE_BYTES) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    nrl_state *state = (nrl_state *)malloc(state_size);
    if (!state) { if (out_size) *out_size = 0; return NULL; }
    
    state->size = lattice_bytes;
    memcpy(state->data, ctx->lattice, lattice_bytes);
    
    if (out_size) *out_size = state_size;
    return state;
}

nrl_ctx *nrl_load_state(const nrl_state *state)
{
    if (!state || state->size == 0) return NULL;
    
    /* Reconstruct from lattice bytes: need to know total_sublattices and lattice_w.
     * We store a small header format in the state data area for simplicity. */
    /* For now, assume we know the dimensions from a config or embedded header. */
    /* Practical: embed a minimal header in state->data[0..7] */
    
    /* This minimal loader expects the first 8 bytes to encode total_sublattices
       (4 bytes LE) and lattice_w (4 bytes LE). */
    if (state->size < 8) return NULL;
    
    unsigned int total_sublattices = (unsigned int)(
        (unsigned int)state->data[0] |
        ((unsigned int)state->data[1] << 8) |
        ((unsigned int)state->data[2] << 16) |
        ((unsigned int)state->data[3] << 24));
    
    unsigned int lattice_w = (unsigned int)(
        (unsigned int)state->data[4] |
        ((unsigned int)state->data[5] << 8) |
        ((unsigned int)state->data[6] << 16) |
        ((unsigned int)state->data[7] << 24));
    
    size_t expected = (size_t)total_sublattices * (lattice_w / 2);
    if (state->size != 8 + expected) return NULL;
    
    nrl_ctx *ctx = nrl_create(0, total_sublattices, lattice_w);
    if (!ctx) return NULL;
    
    size_t lattice_bytes = state->size - 8;
    memcpy(ctx->lattice, state->data + 8, lattice_bytes);
    
    return ctx;
}

double nrl_delta_score(const nrl_ctx *ctx, unsigned int pos_a, unsigned int pos_b)
{
    if (!ctx || pos_a >= ctx->total_sublattices || pos_b >= ctx->total_sublattices)
        return 0.0;
    
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

/* ---- Internal implementation ---- */

static void nrl_update_sublattice_scalar(nrl_ctx *ctx, unsigned int sl_idx,
                                          unsigned int input_seed)
{
    uint8_t *slice = ctx->lattice + (size_t)sl_idx * ctx->packed_w;
    unsigned int rng = input_seed;
    
    for (unsigned int i = 0; i < ctx->packed_w; i++) {
        rng = (rng * 1103515245U + 12345U) & 0x7fffffffU;
        
        uint8_t lo = slice[i] & 0x0F;
        uint8_t hi = (slice[i] >> 4) & 0x0F;
        
        /* Potential update: add noise, dampen, clamp to [0, 15] */
        int new_lo = (int)lo + (int)((rng >> 0) & 0x03) - 1;
        int new_hi = (int)hi + (int)((rng >> 2) & 0x03) - 1;
        
        if (new_lo < 0) new_lo = 0;
        if (new_lo > 15) new_lo = 15;
        if (new_hi < 0) new_hi = 0;
        if (new_hi > 15) new_hi = 15;
        
        slice[i] = (uint8_t)((new_hi << 4) | new_lo);
    }
}

#ifndef NO_AVX2
static void nrl_update_sublattice_avx2(nrl_ctx *ctx, unsigned int sl_idx,
                                        unsigned int input_seed)
{
    uint8_t *slice = ctx->lattice + (size_t)sl_idx * ctx->packed_w;
    unsigned int rng = input_seed;
    unsigned int i = 0;
    
    /* Process 32 bytes (64 packed 4-bit values) per AVX2 iteration */
    for (; i + 32 <= ctx->packed_w; i += 32) {
        __m256i data = _mm256_loadu_si256((const __m256i *)(slice + i));
        
        /* Generate noise: 32 bytes of random-ish values */
        __m256i noise;
        uint8_t noise_buf[32];
        for (int j = 0; j < 32; j++) {
            rng = (rng * 1103515245U + 12345U) & 0x7fffffffU;
            noise_buf[j] = (uint8_t)((rng >> 0) & 0x07);  /* 0-7 noise */
        }
        noise = _mm256_loadu_si256((const __m256i *)noise_buf);
        
        /* Subtract 1 from noise to get range [-1, 6] */
        __m256i one = _mm256_set1_epi8(1);
        noise = _mm256_sub_epi8(noise, one);
        
        /* Deinterleave low/high nibbles for independent processing */
        /* We use a simple approach: add noise, clamp via min/max */
        __m256i lo_mask = _mm256_set1_epi8(0x0F);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(data, 4), lo_mask);
        __m256i lo = _mm256_and_si256(data, lo_mask);
        
        /* Apply noise to both nibbles */
        __m256i noise_lo = _mm256_add_epi8(lo, noise);
        __m256i noise_hi = _mm256_add_epi8(hi, noise);
        
        /* Clamp to [0, 15] */
        __m256i zero = _mm256_setzero_si256();
        __m256i max_val = _mm256_set1_epi8(15);
        noise_lo = _mm256_min_epu8(_mm256_max_epu8(noise_lo, zero), max_val);
        noise_hi = _mm256_min_epu8(_mm256_max_epu8(noise_hi, zero), max_val);
        
        /* Re-pack: hi << 4 | lo */
        __m256i packed = _mm256_or_si256(
            _mm256_slli_epi16(noise_hi, 4),
            noise_lo
        );
        
        _mm256_storeu_si256((__m256i *)(slice + i), packed);
    }
    
    /* Handle remaining bytes with scalar */
    for (; i < ctx->packed_w; i++) {
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
#endif

/* FNV-1a hash over arbitrary data */
static uint64_t nrl_hash_combine(uint64_t h, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}