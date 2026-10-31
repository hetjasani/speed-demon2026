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

#ifndef PF
#define PF 4
#endif
#define MAX_THREADS 64
#define MAX_BUILD_THREADS 16

/* ------------------------------------------------------------------------- */
/* Graph storage: CSR of (head, weight) pairs.                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t v;
    uint32_t w;
} Edge;

typedef struct {
    uint32_t s;
    uint32_t t;
    int64_t ans;
} Query;

/* ------------------------------------------------------------------------- */
/* Radix heap. All keys are non-negative and popped in non-decreasing order, */
/* which is exactly what Dijkstra produces. Push and pop are amortised O(1): */
/* the bucket index is the highest bit where a key differs from the last     */
/* popped key, so no O(log n) sift is ever needed.                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    int64_t d;
    uint32_t u;
} HeapNode;

#define RADIX_BUCKETS 65

typedef struct {
    HeapNode *b[RADIX_BUCKETS];
    uint32_t n[RADIX_BUCKETS];
    uint32_t cap[RADIX_BUCKETS];
    uint32_t total;
    int64_t last;
} RadixHeap;

static void rh_init(RadixHeap *h) {
    memset(h, 0, sizeof(*h));
}

static void rh_free(RadixHeap *h) {
    for (int i = 0; i < RADIX_BUCKETS; i++) free(h->b[i]);
}

static inline void rh_reset(RadixHeap *h) {
    for (int i = 0; i < RADIX_BUCKETS; i++) h->n[i] = 0;
    h->total = 0;
    h->last = 0;
}

static inline unsigned rh_bucket(int64_t key, int64_t last) {
    uint64_t x = (uint64_t)(key ^ last);
    return x ? (unsigned)(64 - __builtin_clzll(x)) : 0u;
}

static inline void rh_push(RadixHeap *h, int64_t key, uint32_t u) {
    unsigned bi = rh_bucket(key, h->last);
    uint32_t k = h->n[bi];
    if (__builtin_expect(k == h->cap[bi], 0)) {
        uint32_t nc = h->cap[bi] ? h->cap[bi] * 2 : 64;
        HeapNode *nb = (HeapNode *)realloc(h->b[bi], (size_t)nc * sizeof(HeapNode));
        if (!nb) {
            fputs("out of memory\n", stderr);
            exit(1);
        }
        h->b[bi] = nb;
        h->cap[bi] = nc;
    }
    h->b[bi][k].d = key;
    h->b[bi][k].u = u;
    h->n[bi] = k + 1;
    h->total++;
}

static inline HeapNode rh_pop(RadixHeap *h) {
    if (__builtin_expect(h->n[0] == 0, 0)) {
        int i = 1;
        while (h->n[i] == 0) i++;
        uint32_t cnt = h->n[i];
        HeapNode *arr = h->b[i];

        uint32_t mi = 0;
        int64_t m = arr[0].d;
        for (uint32_t k = 1; k < cnt; k++) {
            if (arr[k].d < m) {
                m = arr[k].d;
                mi = k;
            }
        }

        HeapNode ret = arr[mi];
        h->n[i] = 0;
        h->total -= cnt;
        h->last = m;

        /* Every remaining key has its highest differing bit with `m` below i,
         * so these pushes never touch bucket i (whose buffer is `arr`). */
        for (uint32_t k = 0; k < cnt; k++) {
            if (k == mi) continue;
            rh_push(h, arr[k].d, arr[k].u);
        }
        return ret;
    }
    h->total--;
    return h->b[0][--h->n[0]];
}

/* ------------------------------------------------------------------------- */
/* Distance table: one 64-bit word per node holding (epoch << shift) | dist.  */
/*                                                                           */
/* A shortest path has at most n-1 edges of weight below 2^32, so every      */
/* distance (and every candidate distance) is below n * 2^32. The remaining  */
/* high bits are free to use as a per-query generation stamp, which turns    */
/* "visited in this query?" plus "current distance" into a single load: the  */
/* neighbour loop touches one cache line per node instead of two.            */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t *tag;
    uint64_t mask;
    int shift;
    uint32_t epoch;
    uint32_t epoch_limit; /* stamps run 1..epoch_limit-1, then the table is cleared */
} DistTable;

/* Number of low bits that hold a distance: every candidate is below
 * n * 2^32, so that many bits plus one of slack always suffice. */
static int dist_bits(uint32_t n) {
    uint64_t ub = (uint64_t)n * 4294967296ULL;
    if (ub == 0) ub = 1;
    int d = 64 - __builtin_clzll(ub) + 1;
    if (d > 62) d = 62;
    return d;
}

/* ------------------------------------------------------------------------- */
/* Dijkstra, stopping when the target is settled.                            */
/* ------------------------------------------------------------------------- */

static int64_t shortest(uint32_t s, uint32_t t, uint32_t n,
                        const uint32_t *head, const Edge *edges,
                        DistTable *dt, RadixHeap *heap) {
    uint32_t cur = dt->epoch + 1;
    if (__builtin_expect(cur >= dt->epoch_limit, 0)) {
        memset(dt->tag, 0, (size_t)n * sizeof(uint64_t));
        cur = 1;
    }
    dt->epoch = cur;

    const uint64_t stamp = (uint64_t)cur << dt->shift;
    const int shift = dt->shift;
    const uint64_t mask = dt->mask;
    uint64_t *tag = dt->tag;

    rh_reset(heap);
    tag[s] = stamp; /* distance 0 */
    rh_push(heap, 0, s);

    while (heap->total) {
        HeapNode top = rh_pop(heap);
        int64_t d = top.d;
        uint32_t u = top.u;

        if (__builtin_expect((int64_t)(tag[u] & mask) != d, 0)) continue; /* stale */
        if (u == t) return d;

        uint32_t e = head[u];
        uint32_t e_end = head[u + 1];
        for (; e < e_end; e++) {
            if (PF && e + PF < e_end) __builtin_prefetch(&tag[edges[e + PF].v], 1, 3);
            uint32_t v = edges[e].v;
            int64_t nd = d + (int64_t)edges[e].w;
            uint64_t x = tag[v];
            if ((x >> shift) != cur) {
                tag[v] = stamp | (uint64_t)nd;
                rh_push(heap, nd, v);
            } else if (nd < (int64_t)(x & mask)) {
                tag[v] = stamp | (uint64_t)nd;
                rh_push(heap, nd, v);
            }
        }
    }
    return -1;
}

typedef struct {
    uint32_t n;
    const uint32_t *head;
    const Edge *edges;
    const uint64_t *has_in;
    Query *queries;
    int num_queries;
    int dbits;
} WorkerContext;

static atomic_int global_query_idx = 0;

static void *worker_func(void *arg) {
    WorkerContext *ctx = (WorkerContext *)arg;
    uint32_t n = ctx->n;

    DistTable dt;
    dt.tag = (uint64_t *)calloc((size_t)n + 1, sizeof(uint64_t));
    dt.mask = ((uint64_t)1 << ctx->dbits) - 1;
    dt.shift = ctx->dbits;
    dt.epoch = 0;
    {
        uint64_t lim = (uint64_t)1 << ctx->dbits;
        dt.epoch_limit = lim > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)lim;
    }
    if (!dt.tag) {
        fputs("out of memory\n", stderr);
        exit(1);
    }

    RadixHeap heap;
    rh_init(&heap);

    for (;;) {
        int q = atomic_fetch_add_explicit(&global_query_idx, 1, memory_order_relaxed);
        if (q >= ctx->num_queries) break;

        uint32_t s = ctx->queries[q].s;
        uint32_t t = ctx->queries[q].t;
        int64_t ans;

        if (s == t) {
            ans = 0; /* a zero-edge path always exists */
        } else if (s >= n || t >= n) {
            ans = -1;
        } else if ((ctx->has_in[t >> 6] & (1ULL << (t & 63))) == 0) {
            /* No edge enters t, so nothing else can reach it. */
            ans = -1;
        } else if (ctx->head[s] == ctx->head[s + 1]) {
            /* s has no out-edges, so it reaches nothing else. */
            ans = -1;
        } else {
            ans = shortest(s, t, n, ctx->head, ctx->edges, &dt, &heap);
        }
        ctx->queries[q].ans = ans;
    }

    rh_free(&heap);
    free(dt.tag);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Node relabelling.                                                         */
/*                                                                           */
/* A Dijkstra search only ever touches the nodes of a ball around s. With    */
/* ids in BFS order that ball is a narrow band of consecutive ids, so the    */
/* distance table, the offsets and the edge lists a query reads stay in      */
/* cache instead of being spread over the whole arrays. The answers are      */
/* unchanged: this is just a permutation of the node ids.                    */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Node relabelling by BFS order.                                            */
/*                                                                           */
/* A Dijkstra search only ever touches the nodes of a ball around s. With    */
/* ids in BFS order that ball is a narrow band of consecutive ids, so the    */
/* distance table, the offsets and the edge lists a query reads stay in      */
/* cache instead of being spread over the whole arrays. The answers are      */
/* unchanged: this is just a permutation of the node ids.                    */
/*                                                                           */
/* The order has to be a genuine breadth-first order, level by level, with   */
/* the nodes of a level in discovery order. A parallel work-stealing BFS     */
/* does not give that: a thread that dives deep numbers far-away nodes early,*/
/* which scrambles every level and destroys exactly the locality we want.    */
/* So the BFS is serial (it costs a few tens of milliseconds) while the      */
/* expensive rewriting of the CSR is parallel.                               */
/* ------------------------------------------------------------------------- */

static void bfs_order(uint32_t n, const uint32_t *head, const Edge *edges,
                      uint64_t *seen, uint32_t *order, uint32_t *head2) {
    uint32_t start = 0, best = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint32_t d = head[u + 1] - head[u];
        if (d > best) { best = d; start = u; }
    }
    uint32_t lo = 0, qt = 1;
    seen[start >> 6] = 1ULL << (start & 63);
    order[0] = start;
    while (lo < qt) {
        uint32_t hi = qt; /* end of the current level */
        for (uint32_t i = lo; i < hi; i++) {
            /* The queue is walked in BFS order, but the out-edges of a node
             * still live at its old offset, so every node costs two dependent
             * cache misses. Warm both a few nodes ahead. */
            if (i + 32 < hi) __builtin_prefetch(&head[order[i + 32]], 0, 1);
            if (i + 8 < hi) {
                uint32_t f = order[i + 8];
                if (head[f] < head[f + 1]) __builtin_prefetch(&edges[head[f]], 0, 1);
            }
            uint32_t u = order[i];
            uint32_t e = head[u], e_end = head[u + 1];
            /* The BFS already reads this node's offsets to walk its edges, so
             * its out-degree in the new order is free here: no second random
             * pass over head[] later. */
            head2[i + 1] = e_end - e;
            for (; e < e_end; e++) {
                uint32_t v = edges[e].v;
                if (v >= n) continue;
                /* A bitmap, not the 7 MB id table: at 1 bit per node it stays
                 * in L2, so the "have I seen v?" probe is not a cache miss. */
                uint64_t m = 1ULL << (v & 63);
                if ((seen[v >> 6] & m) == 0) {
                    seen[v >> 6] |= m;
                    order[qt++] = v;
                }
            }
        }
        lo = hi;
    }
    /* Nodes the BFS never reached go last; they are rare (isolated parts). */
    for (uint32_t u = 0; u < n; u++)
        if ((seen[u >> 6] & (1ULL << (u & 63))) == 0) {
            seen[u >> 6] |= 1ULL << (u & 63);
            order[qt] = u;
            head2[qt + 1] = head[u + 1] - head[u];
            qt++;
        }
}

/* pos[order[i]] = i, in parallel: the BFS no longer builds this table. */
typedef struct {
    const uint32_t *order;
    uint32_t *pos;
    const uint32_t *part_lo;
    const uint32_t *part_hi;
} PosCtx;

static void pos_range(void *vctx, int idx) {
    PosCtx *c = (PosCtx *)vctx;
    uint32_t lo = c->part_lo[idx], hi = c->part_hi[idx];
    for (uint32_t i = lo; i < hi; i++) {
        c->pos[c->order[i]] = i;
        if (i + 32 < hi) __builtin_prefetch(&c->pos[c->order[i + 32]], 1, 3);
    }
}

typedef struct {
    const uint32_t *head;
    const Edge *edges;
    const uint32_t *pos;
    const uint32_t *head2;
    Edge *edges2;
    const uint32_t *order;
    const uint32_t *part_lo;
    const uint32_t *part_hi;
    const uint64_t *target_mask;
    uint64_t *has_in;
    uint32_t n;
} PermuteCtx;

/* Rewrite the CSR in the new order. Offsets do not change, so every part
 * writes a disjoint slice of the edge array: no coordination needed. */
static void permute_range(void *vctx, int idx) {
    PermuteCtx *c = (PermuteCtx *)vctx;
    uint32_t n = c->n;
    uint32_t lo = c->part_lo[idx], hi = c->part_hi[idx];
    uint32_t e = c->head2[lo];
    /* Two-stage software pipeline over the random `order[i] -> head[u]`
     * lookups: prefetch the offset far ahead, then the edge block of a nearer
     * one. Reading the edges of a node is otherwise two dependent misses. */
    for (uint32_t i = lo; i < hi; i++) {
        uint32_t u = c->order[i];
        uint32_t eu = c->head[u], eu_end = c->head[u + 1];
        if (i + 48 < hi) __builtin_prefetch(&c->head[c->order[i + 48]], 0, 1);
        if (i + 12 < hi) __builtin_prefetch(&c->edges[c->head[c->order[i + 12]]], 0, 1);
        for (; eu < eu_end; eu++) {
            uint32_t v = c->edges[eu].v;
            uint32_t nv = v < n ? c->pos[v] : v;
            c->edges2[e].v = nv;
            c->edges2[e].w = c->edges[eu].w;
            e++;
            if (__builtin_expect(v < n && (c->target_mask[v >> 6] & (1ULL << (v & 63))) != 0, 0))
                c->has_in[nv >> 6] |= 1ULL << (nv & 63);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Parsing.                                                                  */
/* ------------------------------------------------------------------------- */

static inline const char *parse_u32(const char *p, const char *end, uint32_t *out) {
    uint32_t x = 0;
    while (p < end) {
        unsigned d = (unsigned)(*p - '0');
        if (d > 9) break;
        x = x * 10 + d;
        p++;
    }
    *out = x;
    return p;
}

static inline const char *skip_line(const char *p, const char *end) {
    while (p < end && *p != '\n') p++;
    return p < end ? p + 1 : p;
}

/* Number of CPUs this process may run on (respects a cpuset/affinity mask). */
static int available_cpus(void) {
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

/* ------------------------------------------------------------------------- */
/* Parallel CSR construction.                                                */
/*                                                                           */
/* The edge lines are split into line-aligned chunks and parsed once into     */
/* three flat arrays (one per field). The degrees are then counted, and the    */
/* CSR filled, by *node range*: a thread re-reads the parsed rows and keeps    */
/* only those whose tail falls in its own slice of the ids. Every write of a   */
/* thread is then inside a contiguous, cache-resident slice of the CSR, so no  */
/* cursor needs an atomic and no counter is written by two threads.            */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *start;
    const char *end;
} Range;

typedef struct {
    Range *ranges;
    uint32_t n;
    uint32_t m;
    uint64_t **inbits;       /* per-range "has an incoming edge" bitmaps */
    const uint64_t *target_mask;
    Edge *edges;
    uint32_t *head;          /* CSR offsets */
    uint32_t *loc;           /* per-part local degrees, later per-part cursors */
    uint32_t loc_stride;
    uint32_t *base;          /* first edge slot of each part's node range */
    uint32_t *tu, *tv, *tw;  /* the parsed edges, one array per field */
    uint32_t *tmp_count;     /* rows filled by each range */
    uint32_t tmp_cap;        /* rows reserved per range */
    int parts;
} BuildCtx;

static inline uint32_t part_lo(const BuildCtx *b, int idx) {
    return (uint32_t)(((uint64_t)b->n * (uint64_t)idx) / (uint64_t)b->parts);
}

static inline uint32_t part_hi(const BuildCtx *b, int idx) {
    return (uint32_t)(((uint64_t)b->n * (uint64_t)(idx + 1)) / (uint64_t)b->parts);
}

typedef void (*RangeFn)(void *ctx, int idx);

typedef struct {
    RangeFn fn;
    void *ctx;
    int idx;
} RangeArg;

static void *range_thread(void *arg) {
    RangeArg *a = (RangeArg *)arg;
    a->fn(a->ctx, a->idx);
    return NULL;
}

/* Runs fn(ctx, 0..count-1) across threads; caller handles index 0. */
static void run_ranges(int count, RangeFn fn, void *ctx) {
    pthread_t th[MAX_BUILD_THREADS];
    RangeArg args[MAX_BUILD_THREADS];
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);

    int started = 0;
    for (int i = 1; i < count; i++) {
        args[i].fn = fn;
        args[i].ctx = ctx;
        args[i].idx = i;
        if (pthread_create(&th[started], &attr, range_thread, &args[i]) != 0) {
            /* Could not start a thread: do that range inline. */
            fn(ctx, i);
            continue;
        }
        started++;
    }
    pthread_attr_destroy(&attr);
    fn(ctx, 0);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
}

static void build_range(void *vctx, int idx) {
    BuildCtx *b = (BuildCtx *)vctx;
    const char *p = b->ranges[idx].start;
    const char *end = b->ranges[idx].end;
    uint32_t n = b->n;
    uint64_t *bits = b->inbits[idx];
    const uint64_t *tmask = b->target_mask;
    uint32_t *tu = b->tu + (size_t)idx * b->tmp_cap;
    uint32_t *tv = b->tv + (size_t)idx * b->tmp_cap;
    uint32_t *tw = b->tw + (size_t)idx * b->tmp_cap;
    uint32_t cap = b->tmp_cap;
    uint32_t i = 0;
    for (; i < b->m && p < end; i++) {
        while (p < end && (unsigned)(*p - '0') > 9) p++;
        if (p >= end) break;
        uint32_t u, v, w;
        p = parse_u32(p, end, &u);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        p = parse_u32(p, end, &v);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        p = parse_u32(p, end, &w);
        p = skip_line(p, end);
        if (i < cap) { tu[i] = u; tv[i] = v; tw[i] = w; }
        if (__builtin_expect(v < n && (tmask[v >> 6] & (1ULL << (v & 63))) != 0, 0))
            bits[v >> 6] |= 1ULL << (v & 63);
    }
    b->tmp_count[idx] = i < cap ? i : cap;
}

/* Out-degree of the nodes in this part's id range. Counting into a local
 * array keeps every increment in L1/L2; the alternative -- one shared counter
 * array bumped during the parse -- costs a random access into 7 MB per edge. */
static void count_range(void *vctx, int idx) {
    BuildCtx *b = (BuildCtx *)vctx;
    uint32_t lo = part_lo(b, idx), hi = part_hi(b, idx);
    uint32_t span = hi - lo;
    uint32_t *loc = b->loc + (size_t)idx * b->loc_stride;
    memset(loc, 0, (size_t)span * sizeof(uint32_t));
    for (int r = 0; r < b->parts; r++) {
        const uint32_t *src = b->tu + (size_t)r * b->tmp_cap;
        uint32_t k = b->tmp_count[r];
        for (uint32_t i = 0; i < k; i++) {
            uint32_t u = src[i];
            if (u - lo < span) loc[u - lo]++; /* true iff lo <= u < hi */
        }
    }
    uint32_t acc = 0;
    for (uint32_t k = 0; k < span; k++) {
        uint32_t c = loc[k];
        loc[k] = acc;
        acc += c;
    }
    b->base[idx + 1] = acc;
}

/* Write this part's offsets and scatter its edges. `loc` already holds the
 * local prefix sums, so it becomes the cursor array: cache-hot, private. */
static void fill_range(void *vctx, int idx) {
    BuildCtx *b = (BuildCtx *)vctx;
    uint32_t lo = part_lo(b, idx), hi = part_hi(b, idx);
    uint32_t span = hi - lo;
    uint32_t *loc = b->loc + (size_t)idx * b->loc_stride;
    uint32_t *head = b->head;
    uint32_t base = b->base[idx];
    Edge *edges = b->edges;

    for (uint32_t k = 0; k < span; k++) {
        uint32_t p = base + loc[k];
        loc[k] = p;
        head[lo + k] = p;
    }
    if (hi == b->n) head[b->n] = b->base[b->parts];

    for (int r = 0; r < b->parts; r++) {
        const uint32_t *su = b->tu + (size_t)r * b->tmp_cap;
        const uint32_t *sv = b->tv + (size_t)r * b->tmp_cap;
        const uint32_t *sw = b->tw + (size_t)r * b->tmp_cap;
        uint32_t k = b->tmp_count[r];
        for (uint32_t i = 0; i < k; i++) {
            uint32_t u = su[i];
            if (u - lo >= span) continue;
            uint32_t slot = loc[u - lo]++;
            edges[slot].v = sv[i];
            edges[slot].w = sw[i];
        }
    }
}

int main(int argc, char **argv) {
    const char *data_dir = "/data";
    const char *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    /* Queries first: their target ids decide which in-degrees we must know. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/queries.txt", data_dir);
    int qfd = open(path, O_RDONLY);
    if (qfd < 0) return 1;

    struct stat qst;
    if (fstat(qfd, &qst) != 0 || qst.st_size <= 0) {
        close(qfd);
        return 1;
    }
    size_t qsize = (size_t)qst.st_size;
    const char *qp = (const char *)mmap(NULL, qsize, PROT_READ, MAP_PRIVATE, qfd, 0);
    if (qp == MAP_FAILED) {
        close(qfd);
        return 1;
    }
    const char *qend = qp + qsize;

    uint32_t q_cap = 4096;
    Query *queries = (Query *)malloc((size_t)q_cap * sizeof(Query));
    if (!queries) return 1;
    int num_queries = 0;

    while (qp < qend) {
        while (qp < qend && (unsigned)(*qp - '0') > 9) qp++;
        if (qp >= qend) break;
        uint32_t s, t;
        qp = parse_u32(qp, qend, &s);
        while (qp < qend && (*qp == ' ' || *qp == '\t')) qp++;
        qp = parse_u32(qp, qend, &t);
        qp = skip_line(qp, qend);

        if (num_queries >= (int)q_cap) {
            q_cap *= 2;
            Query *nq = (Query *)realloc(queries, (size_t)q_cap * sizeof(Query));
            if (!nq) return 1;
            queries = nq;
        }
        queries[num_queries].s = s;
        queries[num_queries].t = t;
        queries[num_queries].ans = -1;
        num_queries++;
    }
    munmap((void *)qp, qsize);
    close(qfd);

    /* Graph. */
    snprintf(path, sizeof(path), "%s/graph.txt", data_dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 1;
    }
    size_t file_size = (size_t)st.st_size;
    const char *base = (const char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return 1;
    }
    posix_madvise((void *)base, file_size, POSIX_MADV_SEQUENTIAL);
    const char *end = base + file_size;

    uint32_t n = 0, m = 0;
    const char *p = parse_u32(base, end, &n);
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    p = parse_u32(p, end, &m);
    p = skip_line(p, end);
    const char *edges_start = p;

    size_t bitmap_words = (size_t)((n + 63) >> 6);
    if (bitmap_words == 0) bitmap_words = 1;

    /* In-degree is only needed for query targets, so track those in a small
     * bitmap instead of touching an n-entry array once per edge. */
    uint64_t *target_mask = (uint64_t *)calloc(bitmap_words, sizeof(uint64_t));
    uint64_t *has_in = (uint64_t *)calloc(bitmap_words, sizeof(uint64_t));
    if (!target_mask || !has_in) return 1;
    for (int i = 0; i < num_queries; i++) {
        uint32_t t = queries[i].t;
        if (t < n) target_mask[t >> 6] |= 1ULL << (t & 63);
    }

    int build_threads = available_cpus();
    if (build_threads > MAX_BUILD_THREADS) build_threads = MAX_BUILD_THREADS;
    if (build_threads < 1) build_threads = 1;
    if ((uint32_t)build_threads > m && m > 0) build_threads = (int)m;
    if (build_threads < 1) build_threads = 1;

    Range ranges[MAX_BUILD_THREADS];
    {
        size_t len = (size_t)(end - edges_start);
        const char *prev = edges_start;
        ranges[0].start = edges_start;
        for (int i = 1; i < build_threads; i++) {
            const char *q = edges_start + (len * (size_t)i) / (size_t)build_threads;
            if (q < prev) q = prev;
            q = skip_line(q, end); /* start chunks on a line boundary */
            if (q < prev) q = prev;
            ranges[i - 1].end = q;
            ranges[i].start = q;
            prev = q;
        }
        ranges[build_threads - 1].end = end;
    }

    BuildCtx b;
    memset(&b, 0, sizeof(b));
    b.ranges = ranges;
    b.n = n;
    b.m = m;
    b.target_mask = target_mask;

    b.inbits = (uint64_t **)calloc((size_t)build_threads, sizeof(uint64_t *));
    b.tmp_count = (uint32_t *)calloc((size_t)build_threads, sizeof(uint32_t));
    b.parts = build_threads;
    /* Byte ranges are equal in size and the edge lines have nearly constant
     * length, so a generous fixed slack per range is enough; it only exists to
     * absorb the variance in digits per line. */
    b.tmp_cap = (uint32_t)(m / (uint32_t)build_threads + 262144u);
    size_t tmp_rows = (size_t)build_threads * b.tmp_cap;
    b.tu = (uint32_t *)malloc(tmp_rows * sizeof(uint32_t));
    b.tv = (uint32_t *)malloc(tmp_rows * sizeof(uint32_t));
    b.tw = (uint32_t *)malloc(tmp_rows * sizeof(uint32_t));
    b.loc_stride = n / (uint32_t)build_threads + 2;
    b.loc = (uint32_t *)malloc((size_t)build_threads * b.loc_stride * sizeof(uint32_t));
    b.base = (uint32_t *)calloc((size_t)build_threads + 1, sizeof(uint32_t));
    b.head = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!b.inbits || !b.tmp_count || !b.tu || !b.tv || !b.tw || !b.loc || !b.base || !b.head)
        return 1;
    for (int i = 0; i < build_threads; i++) {
        b.inbits[i] = (uint64_t *)calloc(bitmap_words, sizeof(uint64_t));
        if (!b.inbits[i]) return 1;
    }

    /* Pass 1: parse the graph once. */
    run_ranges(build_threads, build_range, &b);

    /* The text is no longer needed: everything comes from the parsed rows. */
    munmap((void *)base, file_size);
    close(fd);

    /* Pass 2: out-degrees per node, counted inside each part's id range. */
    run_ranges(build_threads, count_range, &b);
    b.base[0] = 0;
    for (int i = 1; i <= build_threads; i++) b.base[i] += b.base[i - 1];

    /* Pass 3: write the offsets and fill the CSR. */
    Edge *edges = (Edge *)malloc((size_t)m * sizeof(Edge));
    if (!edges) return 1;
    b.edges = edges;
    run_ranges(build_threads, fill_range, &b);

    for (int i = 0; i < build_threads; i++) {
        const uint64_t *src = b.inbits[i];
        for (size_t k = 0; k < bitmap_words; k++) has_in[k] |= src[k];
        free(b.inbits[i]);
    }
    free(b.tu);
    free(b.tv);
    free(b.tw);
    free(b.loc);
    free(b.base);
    free(b.inbits);
    free(b.tmp_count);
    uint32_t *head = b.head;

    /* Relabel the nodes in BFS order so that the ball of nodes a query
     * settles occupies a narrow band of ids, then rewrite the CSR in that
     * order. This is only a permutation: the answers do not change. */
    if (n >= 2) {
        uint32_t *pos = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        uint32_t *order = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        uint32_t *head2 = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
        Edge *edges2 = (Edge *)malloc((size_t)m * sizeof(Edge));
        uint64_t *seen = (uint64_t *)calloc(((size_t)n + 63) / 64, sizeof(uint64_t));
        if (!pos || !order || !head2 || !edges2 || !seen) return 1;

        bfs_order(n, head, edges, seen, order, head2);
        free(seen);

        uint32_t *part_lo = (uint32_t *)malloc((size_t)build_threads * sizeof(uint32_t));
        uint32_t *part_hi = (uint32_t *)malloc((size_t)build_threads * sizeof(uint32_t));
        if (!part_lo || !part_hi) return 1;
        for (int i = 0; i < build_threads; i++) {
            part_lo[i] = (uint32_t)(((uint64_t)n * (uint64_t)i) / (uint64_t)build_threads);
            part_hi[i] = (uint32_t)(((uint64_t)n * (uint64_t)(i + 1)) / (uint64_t)build_threads);
        }

        PosCtx xc;
        xc.order = order;
        xc.pos = pos;
        xc.part_lo = part_lo;
        xc.part_hi = part_hi;
        run_ranges(build_threads, pos_range, &xc);

        head2[0] = 0;
        for (uint32_t nu = 0; nu < n; nu++) head2[nu + 1] += head2[nu];

        memset(has_in, 0, bitmap_words * sizeof(uint64_t));
        PermuteCtx pc;
        pc.head = head;
        pc.edges = edges;
        pc.pos = pos;
        pc.head2 = head2;
        pc.edges2 = edges2;
        pc.order = order;
        pc.part_lo = part_lo;
        pc.part_hi = part_hi;
        pc.target_mask = target_mask;
        pc.has_in = has_in;
        pc.n = n;
        run_ranges(build_threads, permute_range, &pc);

        for (int i = 0; i < num_queries; i++) {
            uint32_t qs = queries[i].s, qt = queries[i].t;
            queries[i].s = qs < n ? pos[qs] : qs;
            queries[i].t = qt < n ? pos[qt] : qt;
        }

        free(head);
        free(edges);
        free(pos);
        free(order);
        free(part_lo);
        free(part_hi);
        head = head2;
        edges = edges2;
    }
    free(target_mask);

    /* Parallel Dijkstra from a shared queue. */
    int num_threads = available_cpus();
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
    if (num_threads < 1) num_threads = 1;
    if (num_threads > num_queries && num_queries > 0) num_threads = num_queries;

    pthread_t threads[MAX_THREADS];
    WorkerContext ctx;
    ctx.n = n;
    ctx.head = head;
    ctx.edges = edges;
    ctx.has_in = has_in;
    ctx.queries = queries;
    ctx.num_queries = num_queries;
    ctx.dbits = dist_bits(n);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    int started = 0;
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], &attr, worker_func, &ctx) != 0) break;
        started++;
    }
    pthread_attr_destroy(&attr);
    /* If no thread could be created, do all the work here. */
    if (started == 0) worker_func(&ctx);
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/distances.txt", out_dir);
    FILE *out = fopen(out_path, "wb");
    if (!out) return 1;
    char out_buf[65536];
    setvbuf(out, out_buf, _IOFBF, sizeof(out_buf));

    for (int i = 0; i < num_queries; i++) {
        fprintf(out, "%lld\n", (long long)queries[i].ans);
    }
    fclose(out);

    free(head);
    free(edges);
    free(has_in);
    free(queries);
    return 0;
}
