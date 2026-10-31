#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#define MAX_COUNTRIES 1024
#define MAX_THREADS   64
#define ARENA_BLOCK   (1u << 20)
#define UNMATCHED_IDX 0
#define UNMATCHED_KEY "_UNMATCHED"
#define MAX_CHUNKS    1024
typedef struct ArenaBlock { struct ArenaBlock *next; size_t used, cap; char data[]; } ArenaBlock;
typedef struct { ArenaBlock *head; } Arena;
static inline char *arena_alloc(Arena *a, const char *s, size_t len) {
    if (!a->head || a->head->used + len + 1 > a->head->cap) {
        size_t cap = len + 1 > ARENA_BLOCK ? len + 1 : ARENA_BLOCK;
        ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock) + cap);
        if (!b) { fputs("out of memory\n", stderr); exit(1); }
        b->next = a->head; b->used = 0; b->cap = cap; a->head = b;
    }
    char *dest = a->head->data + a->head->used;
    if (len) memcpy(dest, s, len);
    dest[len] = '\0'; a->head->used += len + 1; return dest;
}
static inline uint32_t hash_bytes(const char *s, size_t len) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)len << 32); size_t i = 0;
    for (; i + 8 <= len; i += 8) { uint64_t k; memcpy(&k, s + i, 8); h ^= k; h *= 0x9E3779B97F4A7C15ULL; h ^= h >> 32; }
    if (i < len) { uint64_t k = 0; memcpy(&k, s + i, len - i); h ^= k; h *= 0x9E3779B97F4A7C15ULL; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; return (uint32_t)h;
}
typedef struct { uint32_t hash, len; const char *str; } NameEntry;
typedef struct { int64_t count, id_sum, amount_sum, amount_min, amount_max; uint64_t name_bytes, note_bytes; uint32_t names_cap, names_count; NameEntry *names_table; } Agg;
static void agg_grow(Agg *c) {
    uint32_t old_cap = c->names_cap, new_cap = old_cap ? old_cap * 2 : 64, mask = new_cap - 1;
    NameEntry *old_t = c->names_table, *new_t = (NameEntry *)calloc(new_cap, sizeof(NameEntry));
    if (!new_t) { fputs("out of memory\n", stderr); exit(1); }
    for (uint32_t i = 0; i < old_cap; i++) if (old_t[i].str) {
        uint32_t slot = old_t[i].hash & mask;
        while (new_t[slot].str) slot = (slot + 1) & mask;
        new_t[slot] = old_t[i];
    }
    free(old_t); c->names_table = new_t; c->names_cap = new_cap;
}
static inline int name_update(NameEntry *t, uint32_t mask, uint32_t hash, uint32_t len, const char *str) {
    uint32_t slot = hash & mask;
    for (;; slot = (slot + 1) & mask) {
        NameEntry *e = &t[slot];
        if (!e->str) { e->hash = hash; e->len = len; e->str = str; return 1; }
        if (e->hash == hash && e->len == len && memcmp(e->str, str, len) == 0) return 0;
    }
}
static inline void agg_add_name(Agg *c, const char *name, uint32_t len, uint32_t hash, Arena *arena) {
    if (__builtin_expect(c->names_cap == 0 || (c->names_count + 1) * 4 > c->names_cap * 3, 0)) agg_grow(c);
    uint32_t mask = c->names_cap - 1, slot = hash & mask;
    for (;; slot = (slot + 1) & mask) {
        NameEntry *e = &c->names_table[slot];
        if (!e->str) { e->hash = hash; e->len = len; e->str = arena_alloc(arena, name, len); c->names_count++; return; }
        if (e->hash == hash && e->len == len && memcmp(e->str, name, len) == 0) return;
    }
}
static uint16_t *region_map = NULL; static uint64_t region_max = 0;
static char *country_names[MAX_COUNTRIES]; static int total_countries = 1, code_index[26 * 26 * 26];
static int country_index(const char *s, size_t len, Arena *arena) {
    if (len == 3) {
        unsigned c0 = (unsigned char)s[0] - 'A', c1 = (unsigned char)s[1] - 'A', c2 = (unsigned char)s[2] - 'A';
        if (c0 < 26 && c1 < 26 && c2 < 26) {
            unsigned k = (c0 * 26 + c1) * 26 + c2; int idx = code_index[k];
            if (idx) return idx;
            if (total_countries >= MAX_COUNTRIES) return UNMATCHED_IDX;
            idx = total_countries++; country_names[idx] = arena_alloc(arena, s, len); code_index[k] = idx; return idx;
        }
    }
    for (int i = 1; i < total_countries; i++) if (strlen(country_names[i]) == len && memcmp(country_names[i], s, len) == 0) return i;
    if (total_countries >= MAX_COUNTRIES) return UNMATCHED_IDX;
    int idx = total_countries++; country_names[idx] = arena_alloc(arena, s, len); return idx;
}
typedef struct { int64_t id; uint16_t country; } RegionPair;
static void load_regions(const char *path, Arena *arena) {
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    struct stat st; if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }
    size_t size = (size_t)st.st_size; const char *data = (const char *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { close(fd); return; }
    const char *p = data, *end = data + size;
    if (size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p)); p = nl ? nl + 1 : end;
    size_t cap = 4096, n = 0; RegionPair *pairs = (RegionPair *)malloc(cap * sizeof(*pairs)); uint64_t max_id = 0;
    while (p < end) {
        int64_t id = 0; while (p < end && (unsigned)(*p - '0') <= 9) id = id * 10 + (*p++ - '0');
        if (p >= end || *p != ',') { while (p < end && *p != '\n') p++; if (p < end) p++; continue; }
        p++; const char *cs = p; while (p < end && *p != '\n' && *p != '\r') p++;
        size_t clen = (size_t)(p - cs); while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (clen == 0) continue;
        if (n == cap) { cap *= 2; pairs = (RegionPair *)realloc(pairs, cap * sizeof(*pairs)); if (!pairs) { fputs("out of memory\n", stderr); exit(1); } }
        pairs[n].id = id; pairs[n++].country = (uint16_t)country_index(cs, clen, arena);
        if ((uint64_t)id > max_id) max_id = (uint64_t)id;
    }
    munmap((void *)data, size); close(fd); region_max = max_id;
    region_map = (uint16_t *)calloc((size_t)max_id + 1, sizeof(uint16_t));
    if (!region_map) { fputs("out of memory\n", stderr); exit(1); }
    for (size_t i = 0; i < n; i++) region_map[pairs[i].id] = pairs[i].country;
    free(pairs);
}
typedef struct { Agg *agg; Arena arena; char *esc; size_t esc_cap; } Worker;
typedef struct { const char *start, *end; } Chunk;
static Chunk chunks[MAX_CHUNKS]; static int total_chunks; static atomic_int next_chunk;
static inline const char *find_close(const char *p, const char *end, unsigned *pairs) {
    unsigned n = 0;
    for (;;) {
        const char *q = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!q) { *pairs = n; return end; }
        if (q + 1 < end && q[1] == '"') { n++; p = q + 2; } else { *pairs = n; return q; }
    }
}
static void *worker_func(void *arg) {
    Worker *w = (Worker *)arg; Agg *aggs = w->agg; Arena *arena = &w->arena; char *esc = w->esc; size_t esc_cap = w->esc_cap;
    for (;;) {
        int ci = atomic_fetch_add_explicit(&next_chunk, 1, memory_order_relaxed);
        if (ci >= total_chunks) break;
        const char *p = chunks[ci].start, *end = chunks[ci].end;
        while (p < end) {
            if ((unsigned char)*p == 0xEF && p + 2 < end && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) { p += 3; if (p >= end) break; }
            int64_t id = 0; while (p < end && (unsigned)(*p - '0') <= 9) id = id * 10 + (*p++ - '0');
            if (p < end && *p == ',') p++;
            const char *name; uint32_t nlen;
            if (p < end && *p == '"') {
                const char *st = ++p; unsigned pairs; const char *q = find_close(p, end, &pairs);
                nlen = (uint32_t)((size_t)(q - st) - pairs);
                if (__builtin_expect(pairs == 0, 1)) { name = st; }
                else {
                    size_t raw = (size_t)(q - st);
                    if (raw > esc_cap) { esc_cap = raw + 256; esc = (char *)realloc(esc, esc_cap); if (!esc) { fputs("out of memory\n", stderr); exit(1); } }
                    char *d = esc; for (const char *s = st; s < q;) { if (*s == '"') { *d++ = '"'; s += 2; } else *d++ = *s++; }
                    name = esc;
                }
                p = (q < end) ? q + 1 : end; if (p < end && *p == ',') p++;
            } else { const char *st = p; while (p < end && *p != ',') p++; name = st; nlen = (uint32_t)(p - st); if (p < end) p++; }
            size_t note_len;
            if (p < end && *p == '"') { const char *st = ++p; unsigned pairs; const char *q = find_close(p, end, &pairs); note_len = (size_t)(q - st) - pairs; p = (q < end) ? q + 1 : end; if (p < end && *p == ',') p++; }
            else { const char *st = p; while (p < end && *p != ',') p++; note_len = (size_t)(p - st); if (p < end) p++; }
            int64_t reg = 0; while (p < end && (unsigned)(*p - '0') <= 9) reg = reg * 10 + (*p++ - '0');
            if (p < end && *p == ',') p++;
            int neg = (p < end && *p == '-') ? (p++, 1) : 0; int64_t cents = 0;
            while (p < end && (unsigned)(*p - '0') <= 9) cents = cents * 10 + (*p++ - '0');
            if (p + 2 < end && p[0] == '.') { cents = cents * 100 + (p[1] - '0') * 10 + (p[2] - '0'); p += 3; }
            else if (p < end && *p == '.') { p++; int64_t frac = 0; if (p < end) frac = (*p++ - '0') * 10; if (p < end) frac += *p++ - '0'; cents = cents * 100 + frac; }
            if (neg) cents = -cents;
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
            uint16_t c_idx = ((uint64_t)reg <= region_max) ? region_map[reg] : (uint16_t)UNMATCHED_IDX;
            Agg *c = &aggs[c_idx]; c->count++; c->id_sum += id; c->amount_sum += cents;
            if (cents < c->amount_min) c->amount_min = cents;
            if (cents > c->amount_max) c->amount_max = cents;
            c->name_bytes += nlen; c->note_bytes += note_len;
            agg_add_name(c, name, nlen, hash_bytes(name, nlen), arena);
        }
    }
    w->esc = esc; w->esc_cap = esc_cap; return NULL;
}
__attribute__((unused)) static inline void scan_block(const char *base, unsigned mask, int *in_quote, const char **target, int *t, int n, const char *start, size_t stride, Chunk *out) {
    while (mask) {
        unsigned i = (unsigned)__builtin_ctz(mask); mask &= mask - 1; char c = base[i];
        if (c == '"') *in_quote ^= 1;
        else if (c == '\n' && !*in_quote) {
            const char *next = base + i + 1;
            if (next >= *target && *t < n) { int tt = *t; out[tt - 1].end = next; out[tt].start = next; *t = ++tt; *target = start + (size_t)tt * stride; }
        }
    }
}
static int split_ranges(const char *start, const char *end, int n, Chunk *out) {
    if (n <= 1) { out[0].start = start; out[0].end = end; return 1; }
    const size_t stride = (size_t)(end - start) / (size_t)n; const char *target = start + stride, *p = start;
    int t = 1, in_quote = 0; out[0].start = start;
#if defined(__AVX2__)
    const __m256i vq = _mm256_set1_epi8('"'), vl = _mm256_set1_epi8('\n');
    while (p + 32 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)p);
        unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpeq_epi8(v, vq), _mm256_cmpeq_epi8(v, vl)));
        if (m) scan_block(p, m, &in_quote, &target, &t, n, start, stride, out);
        p += 32;
    }
#elif defined(__SSE2__)
    const __m128i vq = _mm_set1_epi8('"'), vl = _mm_set1_epi8('\n');
    while (p + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)p);
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v, vq), _mm_cmpeq_epi8(v, vl)));
        if (m) scan_block(p, m, &in_quote, &target, &t, n, start, stride, out);
        p += 16;
    }
#endif
    while (p + 8 <= end) {
        uint64_t v, q, l, bits; memcpy(&v, p, 8); q = v ^ 0x2222222222222222ULL; l = v ^ 0x0A0A0A0A0A0A0A0AULL;
        bits = ((q - 0x0101010101010101ULL) & ~q & 0x8080808080808080ULL) | ((l - 0x0101010101010101ULL) & ~l & 0x8080808080808080ULL);
        while (bits) {
            unsigned i = (unsigned)__builtin_ctzll(bits) >> 3; bits &= bits - 1;
            if (p[i] == '"') in_quote ^= 1;
            else if (p[i] == '\n' && !in_quote && p + i + 1 >= target && t < n) {
                out[t - 1].end = p + i + 1; out[t].start = p + i + 1; target = start + (size_t)++t * stride;
            }
        }
        p += 8;
    }
    while (p < end) {
        char c = *p++;
        if (c == '"') in_quote ^= 1;
        else if (c == '\n' && !in_quote && p >= target && t < n) { out[t - 1].end = p; out[t].start = p; target = start + (size_t)++t * stride; }
    }
    out[t - 1].end = end; return t;
}
typedef struct { int tid, nthreads, nworkers; Agg *final_agg, **worker_agg; } MergeArg;
static void *merge_func(void *arg) {
    MergeArg *m = (MergeArg *)arg;
    for (int i = m->tid; i < total_countries; i += m->nthreads) {
        Agg *dst = &m->final_agg[i]; dst->amount_min = INT64_MAX; dst->amount_max = INT64_MIN;
        uint32_t total_names = 0; int any = 0;
        for (int j = 0; j < m->nworkers; j++) {
            Agg *src = &m->worker_agg[j][i]; if (!src->count) continue;
            any = 1; dst->count += src->count; dst->id_sum += src->id_sum; dst->amount_sum += src->amount_sum;
            if (src->amount_min < dst->amount_min) dst->amount_min = src->amount_min;
            if (src->amount_max > dst->amount_max) dst->amount_max = src->amount_max;
            dst->name_bytes += src->name_bytes; dst->note_bytes += src->note_bytes; total_names += src->names_count;
        }
        if (!any || total_names == 0) continue;
        uint32_t cap = 64, need = total_names + (total_names >> 1) + 1;
        while (cap < need) cap <<= 1;
        dst->names_cap = cap; dst->names_table = (NameEntry *)calloc(cap, sizeof(NameEntry));
        if (!dst->names_table) { fputs("out of memory\n", stderr); exit(1); }
        uint32_t mask = cap - 1;
        for (int j = 0; j < m->nworkers; j++) {
            Agg *src = &m->worker_agg[j][i]; if (!src->names_table) continue;
            for (uint32_t s = 0; s < src->names_cap; s++) {
                NameEntry *e = &src->names_table[s];
                if (e->str && name_update(dst->names_table, mask, e->hash, e->len, e->str)) dst->names_count++;
            }
        }
    }
    return NULL;
}
static int cmp_country(const void *a, const void *b) { return strcmp(country_names[*(const int *)a], country_names[*(const int *)b]); }
static int available_cpus(void) {
    cpu_set_t set; if (sched_getaffinity(0, sizeof(set), &set) == 0) { int n = CPU_COUNT(&set); if (n > 0) return n; }
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? n : 1;
}
int main(int argc, char **argv) {
    const char *data_dir = "/data", *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }
    Arena main_arena = {0}; country_names[UNMATCHED_IDX] = (char *)UNMATCHED_KEY;
    char path[1024]; snprintf(path, sizeof(path), "%s/regions.csv", data_dir); load_regions(path, &main_arena);
    if (!region_map) { region_max = 0; region_map = (uint16_t *)calloc(1, sizeof(uint16_t)); if (!region_map) return 1; }
    snprintf(path, sizeof(path), "%s/records.csv", data_dir);
    int fd = open(path, O_RDONLY); if (fd < 0) return 1;
    struct stat st; if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 1; }
    size_t fsize = (size_t)st.st_size; const char *data = (const char *)mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) { close(fd); return 1; }
    posix_madvise((void *)data, fsize, POSIX_MADV_SEQUENTIAL);
    const char *end = data + fsize, *p_rec = data;
    if (p_rec + 2 < end && (unsigned char)p_rec[0] == 0xEF && (unsigned char)p_rec[1] == 0xBB && (unsigned char)p_rec[2] == 0xBF) p_rec += 3;
    while (p_rec < end && *p_rec != '\n') p_rec++;
    if (p_rec < end) p_rec++;
    int nthreads = available_cpus(); if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;
    size_t body = (size_t)(end - p_rec);
    int byte_cap = (int)(body / (1u << 20)) + 1; if (nthreads > byte_cap) nthreads = byte_cap; if (nthreads < 1) nthreads = 1;
    int nchunks = nthreads * 2, chunk_cap = (int)(body / (256u << 10)) + 1;
    if (nchunks > chunk_cap) nchunks = chunk_cap; if (nchunks > MAX_CHUNKS) nchunks = MAX_CHUNKS; if (nchunks < 1) nchunks = 1;
    total_chunks = split_ranges(p_rec, end, nchunks, chunks); atomic_store(&next_chunk, 0);
    Worker *workers = (Worker *)calloc((size_t)nthreads, sizeof(Worker));
    pthread_t *threads = (pthread_t *)malloc((size_t)nthreads * sizeof(*threads));
    if (!workers || !threads) return 1;
    for (int i = 0; i < nthreads; i++) {
        workers[i].agg = (Agg *)calloc((size_t)total_countries, sizeof(Agg));
        if (!workers[i].agg) return 1;
        for (int j = 0; j < total_countries; j++) { workers[i].agg[j].amount_min = INT64_MAX; workers[i].agg[j].amount_max = INT64_MIN; }
        if (pthread_create(&threads[i], NULL, worker_func, &workers[i]) != 0) {
            for (int j = 0; j < i; j++) pthread_join(threads[j], NULL);
            munmap((void *)data, fsize); close(fd); fputs("could not start worker thread\n", stderr); return 1;
        }
    }
    for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);
    munmap((void *)data, fsize); close(fd);
    Agg *final_agg = (Agg *)calloc((size_t)total_countries, sizeof(Agg));
    Agg **worker_agg = (Agg **)malloc((size_t)nthreads * sizeof(*worker_agg));
    MergeArg *merge_args = (MergeArg *)calloc((size_t)nthreads, sizeof(MergeArg));
    if (!final_agg || !worker_agg || !merge_args) return 1;
    for (int i = 0; i < nthreads; i++) worker_agg[i] = workers[i].agg;
    int merge_started = 0;
    for (int i = 0; i < nthreads; i++) {
        merge_args[i] = (MergeArg){ .tid = i, .nthreads = nthreads, .nworkers = nthreads, .final_agg = final_agg, .worker_agg = worker_agg };
        if (pthread_create(&threads[i], NULL, merge_func, &merge_args[i]) != 0) break;
        merge_started++;
    }
    for (int i = merge_started; i < nthreads; i++) merge_func(&merge_args[i]);
    for (int i = 0; i < merge_started; i++) pthread_join(threads[i], NULL);
    int valid[MAX_COUNTRIES], valid_cnt = 0;
    for (int i = 0; i < total_countries; i++) if (final_agg[i].count > 0) valid[valid_cnt++] = i;
    qsort(valid, (size_t)valid_cnt, sizeof(int), cmp_country);
    mkdir(out_dir, 0777);
    char out_file[1024]; snprintf(out_file, sizeof(out_file), "%s/aggregates.txt", out_dir);
    FILE *out = fopen(out_file, "wb"); if (!out) return 1;
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    char s_sum[32], s_min[32], s_max[32];
    for (int i = 0; i < valid_cnt; i++) {
        int ci = valid[i]; Agg *c = &final_agg[ci];
        int64_t v = llabs(c->amount_sum);
        sprintf(s_sum, "%s%lld.%02lld", c->amount_sum < 0 ? "-" : "", (long long)(v / 100), (long long)(v % 100));
        v = llabs(c->amount_min);
        sprintf(s_min, "%s%lld.%02lld", c->amount_min < 0 ? "-" : "", (long long)(v / 100), (long long)(v % 100));
        v = llabs(c->amount_max);
        sprintf(s_max, "%s%lld.%02lld", c->amount_max < 0 ? "-" : "", (long long)(v / 100), (long long)(v % 100));
        fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n", country_names[ci], (long long)c->count, (long long)c->id_sum, s_sum, s_min, s_max, c->names_count, (unsigned long long)c->name_bytes, (unsigned long long)c->note_bytes);
    }
    fclose(out); return 0;
}