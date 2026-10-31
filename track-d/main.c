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

#define MAX_THREADS 64
#define MAX_BUILD_THREADS 8
#define PF 4 /* prefetch distance in the relaxation loops */

/* ------------------------------------------------------------------------- */
/* Graph storage: CSR of (head, weight) pairs, forward and reverse.          */
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
/* Distance tables.                                                          */
/*                                                                           */
/* A shortest path has at most n-1 edges of weight below 2^32, so every      */
/* distance (and every candidate) is below n * 2^32. The remaining high bits */
/* are free to serve as a per-query generation stamp, so "visited in this    */
/* query?" and "current distance" become one load from one cache line.       */
/*                                                                           */
/* Both directions live in the same 16-byte record, so the meeting test in   */
/* the relaxation loop hits the line already being touched.                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint64_t f, b;
} TagPair;

/* Number of low bits that hold a distance. */
static int dist_bits(uint32_t n) {
    uint64_t ub = (uint64_t)n * 4294967296ULL;
    int d = 64 - __builtin_clzll(ub) + 2; /* +2 for slack */
    if (d > 62) d = 62;
    return d;
}

/* ------------------------------------------------------------------------- */
/* Bidirectional Dijkstra: grow both frontiers and stop as soon as their      */
/* radii plus the best complete path found so far prove optimality.          */
/* ------------------------------------------------------------------------- */

static int64_t shortest(uint32_t s, uint32_t t, uint32_t n,
                        const uint32_t *head, const Edge *fedges,
                        const uint32_t *rhead, const Edge *redges,
                        TagPair *tag, int shift, uint32_t *epoch_ptr,
                        RadixHeap *fh, RadixHeap *bh) {
    uint32_t cur = *epoch_ptr + 1;
    if (__builtin_expect(cur >= (1u << shift), 0)) {
        memset(tag, 0, (size_t)n * sizeof(TagPair));
        cur = 1;
    }
    *epoch_ptr = cur;

    const uint64_t stamp = (uint64_t)cur << shift;
    const uint64_t mask = ((uint64_t)1 << shift) - 1;

    rh_reset(fh);
    rh_reset(bh);
    tag[s].f = stamp;
    tag[t].b = stamp;
    rh_push(fh, 0, s);
    rh_push(bh, 0, t);

    int64_t best = INT64_MAX;

    for (;;) {
        if (!fh->total || !bh->total) break;
        /* last is a lower bound on each heap's minimum, and every unexplored
         * s-t path costs at least the sum of the two minima. */
        if (fh->last + bh->last >= best) break;

        if (fh->last <= bh->last) {
            HeapNode top = rh_pop(fh);
            int64_t d = top.d;
            uint32_t u = top.u;
            TagPair *tu = &tag[u];
            if (__builtin_expect((int64_t)(tu->f & mask) != d, 0)) continue; /* stale */

            uint64_t y = tu->b;
            if ((y >> shift) == cur) {
                int64_t cand = d + (int64_t)(y & mask);
                if (cand < best) best = cand;
            }

            uint32_t e = head[u];
            uint32_t e_end = head[u + 1];
            for (; e < e_end; e++) {
                if (PF && e + PF < e_end) __builtin_prefetch(&tag[fedges[e + PF].v].f, 1, 3);
                uint32_t v = fedges[e].v;
                int64_t nd = d + (int64_t)fedges[e].w;
                TagPair *tv = &tag[v];
                uint64_t x = tv->f;
                uint64_t y2 = tv->b;
                if ((y2 >> shift) == cur) {
                    int64_t cand = nd + (int64_t)(y2 & mask);
                    if (cand < best) best = cand;
                }
                if ((x >> shift) != cur) {
                    tv->f = stamp | (uint64_t)nd;
                    rh_push(fh, nd, v);
                } else if (nd < (int64_t)(x & mask)) {
                    tv->f = stamp | (uint64_t)nd;
                    rh_push(fh, nd, v);
                }
            }
        } else {
            HeapNode top = rh_pop(bh);
            int64_t d = top.d;
            uint32_t u = top.u;
            TagPair *tu = &tag[u];
            if (__builtin_expect((int64_t)(tu->b & mask) != d, 0)) continue; /* stale */

            uint64_t y = tu->f;
            if ((y >> shift) == cur) {
                int64_t cand = d + (int64_t)(y & mask);
                if (cand < best) best = cand;
            }

            uint32_t e = rhead[u];
            uint32_t e_end = rhead[u + 1];
            for (; e < e_end; e++) {
                if (PF && e + PF < e_end) __builtin_prefetch(&tag[redges[e + PF].v].b, 1, 3);
                uint32_t v = redges[e].v; /* a predecessor of u */
                int64_t nd = d + (int64_t)redges[e].w;
                TagPair *tv = &tag[v];
                uint64_t x = tv->b;
                uint64_t y2 = tv->f;
                if ((y2 >> shift) == cur) {
                    int64_t cand = nd + (int64_t)(y2 & mask);
                    if (cand < best) best = cand;
                }
                if ((x >> shift) != cur) {
                    tv->b = stamp | (uint64_t)nd;
                    rh_push(bh, nd, v);
                } else if (nd < (int64_t)(x & mask)) {
                    tv->b = stamp | (uint64_t)nd;
                    rh_push(bh, nd, v);
                }
            }
        }
    }

    return best == INT64_MAX ? -1 : best;
}

typedef struct {
    uint32_t n;
    const uint32_t *head;
    const Edge *edges;
    const uint32_t *rhead;
    const Edge *redges;
    const uint64_t *has_in;
    Query *queries;
    int num_queries;
    int dbits;
} WorkerContext;

static atomic_int global_query_idx = 0;

static void *worker_func(void *arg) {
    WorkerContext *ctx = (WorkerContext *)arg;
    uint32_t n = ctx->n;

    TagPair *tag = (TagPair *)calloc(n, sizeof(TagPair));
    if (!tag) {
        fputs("out of memory\n", stderr);
        exit(1);
    }

    RadixHeap fh, bh;
    rh_init(&fh);
    rh_init(&bh);
    uint32_t epoch = 0;

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
            ans = shortest(s, t, n, ctx->head, ctx->edges, ctx->rhead, ctx->redges,
                           tag, ctx->dbits, &epoch, &fh, &bh);
        }
        ctx->queries[q].ans = ans;
    }

    rh_free(&fh);
    rh_free(&bh);
    free(tag);
    return NULL;
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
/* Parallel helpers.                                                         */
/* ------------------------------------------------------------------------- */

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

/* Runs fn(ctx, 0..count-1) across threads; index 0 runs on the caller. */
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
            fn(ctx, i); /* could not start a thread: do the range inline */
            continue;
        }
        started++;
    }
    pthread_attr_destroy(&attr);
    fn(ctx, 0);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
}

/* ------------------------------------------------------------------------- */
/* Parallel CSR construction from the text file.                             */
/*                                                                           */
/* The edge lines are split into line-aligned chunks. Pass 1 counts each      */
/* chunk's out-degrees into a private array; the counts are summed into the   */
/* CSR offsets. Pass 2 scatters edges into their final slots, taking a slot   */
/* from a shared cursor with a relaxed atomic add (contention is rare         */
/* because tails are spread over the whole node set).                         */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *start;
    const char *end;
} Range;

typedef struct {
    Range *ranges;
    uint32_t n;
    uint32_t m;
    uint32_t **cnt;          /* per-range out-degree counters */
    uint64_t **inbits;       /* per-range "has an incoming edge" bitmaps */
    const uint64_t *target_mask;
    Edge *edges;
    uint32_t *cursor;
    int pass;
} BuildCtx;

static void build_range(void *vctx, int idx) {
    BuildCtx *b = (BuildCtx *)vctx;
    const char *p = b->ranges[idx].start;
    const char *end = b->ranges[idx].end;
    uint32_t n = b->n;

    if (b->pass == 1) {
        uint32_t *cnt = b->cnt[idx];
        for (uint32_t i = 0; i < b->m && p < end; i++) {
            while (p < end && (unsigned)(*p - '0') > 9) p++;
            if (p >= end) break;
            uint32_t u;
            p = parse_u32(p, end, &u);
            p = skip_line(p, end);
            if (u < n) cnt[u]++;
        }
        return;
    }

    uint64_t *bits = b->inbits[idx];
    const uint64_t *tmask = b->target_mask;
    Edge *edges = b->edges;
    uint32_t *cursor = b->cursor;
    for (uint32_t i = 0; i < b->m && p < end; i++) {
        while (p < end && (unsigned)(*p - '0') > 9) p++;
        if (p >= end) break;
        uint32_t u, v, w;
        p = parse_u32(p, end, &u);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        p = parse_u32(p, end, &v);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        p = parse_u32(p, end, &w);
        p = skip_line(p, end);

        if (u < n) {
            uint32_t pos = __atomic_fetch_add(&cursor[u], 1u, __ATOMIC_RELAXED);
            edges[pos].v = v;
            edges[pos].w = w;
        }
        if (__builtin_expect(v < n && (tmask[v >> 6] & (1ULL << (v & 63))) != 0, 0))
            bits[v >> 6] |= 1ULL << (v & 63);
    }
}

/* ------------------------------------------------------------------------- */
/* Reverse CSR, built from the forward one: one sequential pass over the      */
/* edges to count in-degrees, one to scatter.                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    const uint32_t *bounds;   /* count+1 node-range bounds */
    const uint32_t *head;
    const Edge *edges;
    uint32_t **icnt;
    uint32_t *rcursor;
    Edge *redges;
    int pass;
} RevCtx;

static void rev_range(void *vctx, int idx) {
    RevCtx *r = (RevCtx *)vctx;
    uint32_t lo = r->bounds[idx], hi = r->bounds[idx + 1];

    if (r->pass == 1) {
        uint32_t *cnt = r->icnt[idx];
        for (uint32_t u = lo; u < hi; u++) {
            uint32_t e = r->head[u], e_end = r->head[u + 1];
            for (; e < e_end; e++) cnt[r->edges[e].v]++;
        }
        return;
    }

    Edge *redges = r->redges;
    uint32_t *rcursor = r->rcursor;
    for (uint32_t u = lo; u < hi; u++) {
        uint32_t e = r->head[u], e_end = r->head[u + 1];
        for (; e < e_end; e++) {
            uint32_t v = r->edges[e].v;
            uint32_t pos = __atomic_fetch_add(&rcursor[v], 1u, __ATOMIC_RELAXED);
            redges[pos].v = u;
            redges[pos].w = r->edges[e].w;
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

    b.cnt = (uint32_t **)calloc((size_t)build_threads, sizeof(uint32_t *));
    b.inbits = (uint64_t **)calloc((size_t)build_threads, sizeof(uint64_t *));
    if (!b.cnt || !b.inbits) return 1;
    for (int i = 0; i < build_threads; i++) {
        b.cnt[i] = (uint32_t *)calloc((size_t)n, sizeof(uint32_t));
        b.inbits[i] = (uint64_t *)calloc(bitmap_words, sizeof(uint64_t));
        if (!b.cnt[i] || !b.inbits[i]) return 1;
    }

    /* Pass 1: out-degree of every node. */
    b.pass = 1;
    run_ranges(build_threads, build_range, &b);

    /* Prefix sums give the CSR offsets. */
    uint32_t *head = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *cursor = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    if (!head || !cursor) return 1;
    head[0] = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint32_t s = 0;
        for (int i = 0; i < build_threads; i++) s += b.cnt[i][u];
        head[u + 1] = head[u] + s;
        cursor[u] = head[u];
    }

    /* Pass 2: scatter edges into CSR, and note incoming edges to targets. */
    Edge *edges = (Edge *)malloc((size_t)m * sizeof(Edge));
    if (!edges) return 1;
    b.edges = edges;
    b.cursor = cursor;
    b.pass = 2;
    run_ranges(build_threads, build_range, &b);

    for (int i = 0; i < build_threads; i++) {
        const uint64_t *src = b.inbits[i];
        for (size_t k = 0; k < bitmap_words; k++) has_in[k] |= src[k];
        free(b.inbits[i]);
    }
    free(b.inbits);
    free(target_mask);
    munmap((void *)base, file_size);
    close(fd);

    /* Reverse CSR, counted from the forward CSR instead of the text. */
    uint32_t *rhead = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *rcursor = (uint32_t *)malloc(((size_t)n + 1) * sizeof(uint32_t));
    Edge *redges = (Edge *)malloc((size_t)m * sizeof(Edge));
    if (!rhead || !rcursor || !redges) return 1;

    uint32_t nbounds[MAX_BUILD_THREADS + 1];
    for (int i = 0; i <= build_threads; i++)
        nbounds[i] = (uint32_t)(((uint64_t)n * (uint64_t)i) / (uint64_t)build_threads);

    for (int i = 0; i < build_threads; i++)
        memset(b.cnt[i], 0, (size_t)n * sizeof(uint32_t));

    RevCtx r;
    memset(&r, 0, sizeof(r));
    r.bounds = nbounds;
    r.head = head;
    r.edges = edges;
    r.icnt = b.cnt;
    r.redges = redges;

    r.pass = 1;
    run_ranges(build_threads, rev_range, &r);

    rhead[0] = 0;
    for (uint32_t u = 0; u < n; u++) {
        uint32_t s = 0;
        for (int i = 0; i < build_threads; i++) s += b.cnt[i][u];
        rhead[u + 1] = rhead[u] + s;
        rcursor[u] = rhead[u];
    }

    r.pass = 2;
    r.rcursor = rcursor;
    run_ranges(build_threads, rev_range, &r);

    for (int i = 0; i < build_threads; i++) free(b.cnt[i]);
    free(b.cnt);

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
    ctx.rhead = rhead;
    ctx.redges = redges;
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
    free(rhead);
    free(redges);
    free(has_in);
    free(queries);
    return 0;
}
