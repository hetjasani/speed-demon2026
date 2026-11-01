/* Track D - exact Dijkstra with BFS relabelling, a distance-bucket frontier and
 * one 32-bit distance tag per node. C11 + POSIX threads, no dependencies.
 *
 *   graph.txt -> parse (windowed mmap) -> degrees -> CSR (old ids)
 *             -> BFS order -> CSR (new ids) -> 2000 exact searches
 *
 * Every query stops as soon as the target is settled. Weights are below 4096,
 * so the frontier is a Dial bucket queue: push is an array append, pop an array
 * read, no comparisons and no redistribution. Distances above 2^24 are redone
 * on a 64-bit table, so the fast 24-bit path is exact. */
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

#define PF 4          /* relaxation prefetch distance */
#define MT 64         /* max query threads */
#define MB 16         /* max build threads */
#define RB 65         /* radix buckets (wide fallback) */
#define DB 4096       /* Dial buckets: > any weight the fast path uses */
#define FB 24         /* distance bits in the fast tag table */
#define FLIM ((int64_t)1 << FB)
#define GOW INT64_MIN

typedef struct { uint32_t v, w; } Edge;
typedef struct { uint32_t s, t; int64_t ans; } Query;
typedef struct { int64_t d; uint32_t u; } HN;      /* radix entry: carries its key */

/* Both frontiers share one shape; `dial` picks the entry size, and the dial
 * entry needs nothing but an id: every key in the queue lies in [cur, cur + DB),
 * so a bucket holds exactly one key and the bucket index *is* the key. */
typedef struct {
    void **b; uint32_t *n, *cap, nb, total;
    uint64_t bm[DB / 64];        /* dial: bucket-occupancy bitmap */
    int64_t last, cur; int dial;
} Heap;

static void heap_init(Heap *h, int dial) {
    memset(h, 0, sizeof *h);
    h->dial = dial;
    h->nb = dial ? DB : RB;
    h->b = calloc(h->nb, sizeof(void *));
    h->n = calloc(h->nb, sizeof(uint32_t));
    h->cap = calloc(h->nb, sizeof(uint32_t));
    if (!h->b || !h->n || !h->cap) { fputs("oom\n", stderr); exit(1); }
}
static void heap_free(Heap *h) {
    for (uint32_t i = 0; i < h->nb; i++) free(h->b[i]);
    free(h->b); free(h->n); free(h->cap);
}
static inline void heap_reset(Heap *h) {
    memset(h->n, 0, (size_t)h->nb * sizeof(uint32_t));
    memset(h->bm, 0, sizeof h->bm);
    h->total = 0; h->last = 0; h->cur = 0;
}
static void heap_grow(Heap *h, uint32_t bi) {
    uint32_t nc = h->cap[bi] ? h->cap[bi] * 2 : 64;
    void *nb = realloc(h->b[bi], (size_t)nc * (h->dial ? 4 : sizeof(HN)));
    if (!nb) { fputs("oom\n", stderr); exit(1); }
    h->b[bi] = nb; h->cap[bi] = nc;
}
static inline void dpush(Heap *h, int64_t k, uint32_t u) {
    uint32_t bi = (uint32_t)k & (DB - 1), n = h->n[bi];
    if (__builtin_expect(n == h->cap[bi], 0)) heap_grow(h, bi);
    if (!n) h->bm[bi >> 6] |= 1ULL << (bi & 63);
    ((uint32_t *)h->b[bi])[n] = u;
    h->n[bi] = n + 1; h->total++;
}
/* Successive distances are hundreds apart, so scanning empty buckets one at a
 * time costs more than relaxing a node does: the bitmap jumps over the hole. */
static uint32_t dnext(Heap *h) {
    uint32_t ci = (uint32_t)h->cur & (DB - 1), w = ci >> 6;
    uint64_t m = h->bm[w] & (~0ULL << (ci & 63));
    while (!m) { w = (w + 1) & (DB / 64 - 1); m = h->bm[w]; }
    uint32_t idx = w * 64 + (uint32_t)__builtin_ctzll(m);
    h->cur += (uint32_t)(idx - ci) & (DB - 1);
    return idx;
}
static inline uint32_t dpop(Heap *h) {
    uint32_t ci = (uint32_t)h->cur & (DB - 1);
    if (h->n[ci] == 0) ci = dnext(h);
    h->total--;
    uint32_t n = --h->n[ci];
    if (!n) h->bm[ci >> 6] &= ~(1ULL << (ci & 63));
    return ((uint32_t *)h->b[ci])[n];
}
static inline unsigned rbuck(int64_t k, int64_t last) {
    uint64_t x = (uint64_t)(k ^ last);
    return x ? (unsigned)(64 - __builtin_clzll(x)) : 0u;
}
static inline void rpush(Heap *h, int64_t k, uint32_t u) {
    unsigned bi = rbuck(k, h->last);
    uint32_t n = h->n[bi];
    if (__builtin_expect(n == h->cap[bi], 0)) heap_grow(h, bi);
    ((HN *)h->b[bi])[n].d = k; ((HN *)h->b[bi])[n].u = u;
    h->n[bi] = n + 1; h->total++;
}
static inline HN rpop(Heap *h) {
    if (__builtin_expect(h->n[0] == 0, 0)) {
        unsigned i = 1;
        while (h->n[i] == 0) i++;
        uint32_t cnt = h->n[i], mi = 0; HN *arr = (HN *)h->b[i];
        int64_t m = arr[0].d;
        for (uint32_t k = 1; k < cnt; k++) if (arr[k].d < m) { m = arr[k].d; mi = k; }
        HN ret = arr[mi];
        h->n[i] = 0; h->total -= cnt; h->last = m;
        /* Every remaining key differs from m in a bit below i, so these pushes
         * never touch bucket i (whose buffer is arr). */
        for (uint32_t k = 0; k < cnt; k++)
            if (k != mi) rpush(h, arr[k].d, arr[k].u);
        return ret;
    }
    h->total--;
    return ((HN *)h->b[0])[--h->n[0]];
}

/* 24-bit distance + 8-bit generation stamp in one word: "seen in this query?"
 * and "current distance" are a single load, one cache line per neighbour. */
typedef struct { uint32_t *tag, mask, epoch, limit; } DT;
typedef struct { uint64_t *tag, mask; int shift; uint32_t epoch, limit; } WT;

/* Every candidate is below n * 2^32, so that many bits plus one of slack. */
static int dbits(uint32_t n) {
    uint64_t ub = (uint64_t)n * 4294967296ULL;
    if (ub == 0) ub = 1;
    int d = 64 - __builtin_clzll(ub) + 1;
    return d > 62 ? 62 : d;
}

static int64_t shortest(uint32_t s, uint32_t t, uint32_t n, const uint32_t *head,
                        const Edge *e, DT *dt, Heap *h) {
    uint32_t cur = dt->epoch + 1;
    if (__builtin_expect(cur >= dt->limit, 0)) { memset(dt->tag, 0, (size_t)n * 4); cur = 1; }
    dt->epoch = cur;
    const uint32_t stamp = cur << FB;
    const uint32_t mask = dt->mask;
    uint32_t *tag = dt->tag;
    heap_reset(h);
    tag[s] = stamp;
    dpush(h, 0, s);
    int dropped = 0;
    while (h->total) {
        uint32_t u = dpop(h), d = (uint32_t)h->cur;   /* the bucket index is the key */
        uint32_t ee = head[u], e_end = head[u + 1];
        if (__builtin_expect((tag[u] & mask) != d, 0)) continue;  /* stale */
        if (u == t) return d;
        for (; ee < e_end; ee++) {
            if (PF && ee + PF < e_end) __builtin_prefetch(&tag[e[ee + PF].v], 1, 3);
            uint32_t v = e[ee].v, w = e[ee].w;
            /* A candidate at or above FLIM cannot lie on a path shorter than
             * FLIM, so dropping it keeps every distance below FLIM exact. The
             * unsigned compare also catches d + w overflowing 32 bits. */
            if (__builtin_expect(w >= (uint32_t)(FLIM - d), 0)) { dropped = 1; continue; }
            uint32_t nd = d + w;
            uint32_t x = tag[v];
            if ((x >> FB) != cur || nd < (x & mask)) {
                tag[v] = stamp | nd;
                __builtin_prefetch(&head[v], 0, 3);
                dpush(h, nd, v);
            }
        }
    }
    /* Nothing dropped: every path below FLIM was explored, so t is unreachable.
     * Otherwise a longer path exists and the 64-bit table has to decide. */
    return dropped ? GOW : -1;
}

static int64_t shortest_wide(uint32_t s, uint32_t t, uint32_t n, const uint32_t *head,
                             const Edge *e, WT *dt, Heap *h) {
    uint32_t cur = dt->epoch + 1;
    if (__builtin_expect(cur >= dt->limit, 0)) { memset(dt->tag, 0, (size_t)n * 8); cur = 1; }
    dt->epoch = cur;
    const int sh = dt->shift;
    const uint64_t stamp = (uint64_t)cur << sh, mask = dt->mask;
    uint64_t *tag = dt->tag;
    heap_reset(h);
    tag[s] = stamp;
    rpush(h, 0, s);
    while (h->total) {
        HN top = rpop(h);
        int64_t d = top.d;
        uint32_t u = top.u, ee = head[u], e_end = head[u + 1];
        if (__builtin_expect((int64_t)(tag[u] & mask) != d, 0)) continue;
        if (u == t) return d;
        for (; ee < e_end; ee++) {
            if (PF && ee + PF < e_end) __builtin_prefetch(&tag[e[ee + PF].v], 1, 3);
            uint32_t v = e[ee].v;
            int64_t nd = d + (int64_t)e[ee].w;
            uint64_t x = tag[v];
            if ((x >> sh) != cur || nd < (int64_t)(x & mask)) {
                tag[v] = stamp | (uint64_t)nd;
                rpush(h, nd, v);
            }
        }
    }
    return -1;
}

typedef struct {
    uint32_t n; const uint32_t *head; const Edge *edges;
    const uint64_t *has_in; Query *q; int nq, dbits, dial;
} WC;

static atomic_int gq = 0;
/* The wide table is shared and only used under the lock: overflowing queries
 * are rare, so serialising them beats a 64-bit table per thread. */
static WT wide;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;
static int wready;

static int64_t shortest_whole(uint32_t s, uint32_t t, const WC *c, Heap *h) {
    pthread_mutex_lock(&wmu);
    if (!wready) {
        wide.tag = (uint64_t *)calloc((size_t)c->n + 1, sizeof(uint64_t));
        if (!wide.tag) { fputs("oom\n", stderr); exit(1); }
        wide.mask = ((uint64_t)1 << c->dbits) - 1;
        wide.shift = c->dbits;
        wide.epoch = 0;
        uint64_t lim = (uint64_t)1 << c->dbits;
        wide.limit = lim > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)lim;
        wready = 1;
    }
    int64_t ans = shortest_wide(s, t, c->n, c->head, c->edges, &wide, h);
    pthread_mutex_unlock(&wmu);
    return ans;
}

static void *worker(void *arg) {
    WC *c = (WC *)arg;
    DT dt;
    dt.tag = (uint32_t *)calloc((size_t)c->n + 1, sizeof(uint32_t));
    dt.mask = (uint32_t)FLIM - 1;
    dt.epoch = 0;
    dt.limit = 1u << (32 - FB);
    if (!dt.tag) { fputs("oom\n", stderr); exit(1); }
    Heap h, wh;
    heap_init(&h, 1);
    heap_init(&wh, 0);   /* the wide search keeps the radix heap: huge keys */
    for (;;) {
        int q = atomic_fetch_add_explicit(&gq, 1, memory_order_relaxed);
        if (q >= c->nq) break;
        uint32_t s = c->q[q].s, t = c->q[q].t;
        int64_t ans;
        if (s == t) ans = 0;
        else if (s >= c->n || t >= c->n) ans = -1;
        else if (!c->dial) ans = shortest_whole(s, t, c, &wh);
        else if ((c->has_in[t >> 6] & (1ULL << (t & 63))) == 0) ans = -1;
        else if (c->head[s] == c->head[s + 1]) ans = -1;
        else {
            ans = shortest(s, t, c->n, c->head, c->edges, &dt, &h);
            if (__builtin_expect(ans == GOW, 0)) ans = shortest_whole(s, t, c, &wh);
        }
        c->q[q].ans = ans;
    }
    heap_free(&h); heap_free(&wh); free(dt.tag);
    return NULL;
}

/* BFS order: a search only touches the ball around s, and with BFS ids that
 * ball is a narrow band of consecutive ids, so the tag table, the offsets and
 * the edge blocks of a query stay in cache. Only a permutation of the ids. */
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
        uint32_t hi = qt;   /* end of this level */
        for (uint32_t i = lo; i < hi; i++) {
            /* The queue is in BFS order but a node's out-edges still live at
             * its old offset, so each node costs two dependent cache misses. */
            if (i + 32 < hi) __builtin_prefetch(&head[order[i + 32]], 0, 1);
            if (i + 8 < hi) {
                uint32_t f = order[i + 8];
                if (head[f] < head[f + 1]) __builtin_prefetch(&edges[head[f]], 0, 1);
            }
            uint32_t u = order[i], e = head[u], e_end = head[u + 1];
            head2[i + 1] = e_end - e;   /* out-degree in the new order, free here */
            for (; e < e_end; e++) {
                uint32_t v = edges[e].v;
                if (v >= n) continue;
                /* A 1-bit-per-node bitmap stays in L2, unlike the 7 MB pos[]. */
                uint64_t m = 1ULL << (v & 63);
                if ((seen[v >> 6] & m) == 0) {
                    seen[v >> 6] |= m;
                    order[qt++] = v;
                }
            }
        }
        lo = hi;
    }
    for (uint32_t u = 0; u < n; u++)   /* nodes the BFS never reached go last */
        if ((seen[u >> 6] & (1ULL << (u & 63))) == 0) {
            seen[u >> 6] |= 1ULL << (u & 63);
            order[qt] = u;
            head2[qt + 1] = head[u + 1] - head[u];
            qt++;
        }
}

typedef struct { const uint32_t *order, *lo, *hi; uint32_t *pos; } PosC;
static void pos_range(void *v, int idx) {
    PosC *c = (PosC *)v;
    uint32_t lo = c->lo[idx], hi = c->hi[idx];
    for (uint32_t i = lo; i < hi; i++) {
        c->pos[c->order[i]] = i;
        if (i + 32 < hi) __builtin_prefetch(&c->pos[c->order[i + 32]], 1, 3);
    }
}

typedef struct {
    const uint32_t *head, *pos, *head2, *order, *lo, *hi;
    const Edge *edges; Edge *edges2;
    const uint64_t *target_mask; uint64_t *has_in; uint32_t n;
} PermC;

/* Rewrite the CSR in the new order. Offsets do not change, so each part writes
 * a disjoint slice of the edge array and no coordination is needed. */
static void permute_range(void *v, int idx) {
    PermC *c = (PermC *)v;
    uint32_t lo = c->lo[idx], hi = c->hi[idx];
    uint32_t e = c->head2[lo];
    for (uint32_t i = lo; i < hi; i++) {
        uint32_t u = c->order[i];
        uint32_t eu = c->head[u], eu_end = c->head[u + 1];
        /* Two-stage pipeline over the random order[i] -> head[u] lookups. */
        if (i + 48 < hi) __builtin_prefetch(&c->head[c->order[i + 48]], 0, 1);
        if (i + 12 < hi) __builtin_prefetch(&c->edges[c->head[c->order[i + 12]]], 0, 1);
        for (; eu < eu_end; eu++) {
            uint32_t v2 = c->edges[eu].v, nv = v2 < c->n ? c->pos[v2] : v2;
            c->edges2[e].v = nv;
            c->edges2[e].w = c->edges[eu].w;
            e++;
            if (__builtin_expect(v2 < c->n && (c->target_mask[v2 >> 6] & (1ULL << (v2 & 63))) != 0, 0))
                c->has_in[nv >> 6] |= 1ULL << (nv & 63);
        }
    }
}

static inline const char *pu32(const char *p, const char *end, uint32_t *out) {
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

/* First offset at or after pos that starts a line, so a text window never
 * splits an edge line in two. */
static size_t line_after(int fd, size_t pos, size_t size) {
    char buf[4096];
    while (pos < size) {
        size_t want = size - pos;
        if (want > sizeof buf) want = sizeof buf;
        ssize_t got = pread(fd, buf, want, (off_t)pos);
        if (got <= 0) return size;
        const char *nl = (const char *)memchr(buf, '\n', (size_t)got);
        if (nl) return pos + (size_t)(nl - buf) + 1;
        pos += (size_t)got;
    }
    return size;
}

/* CPUs this process may run on (respects a cgroup / affinity mask). */
static int cpus(void) {
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

typedef struct { const char *start, *end; } Range;

/* One parse part: its own rows, grown on demand. A fixed reservation of
 * m/parts + slack rows per part wastes parts * slack rows of RAM, which on a
 * many-core box is tens of megabytes of pure slack. */
typedef struct { uint32_t *tu, *tv, *tw, count, cap; uint64_t *bits; } Part;

typedef struct {
    Part part[MB];
    Range range[MB];
    uint32_t n, m;
    const uint64_t *target_mask;
    Edge *edges;
    uint32_t *head, *loc, *base, loc_stride;
    uint32_t maxw[MB];
    int parts;
} BC;

static inline uint32_t plo(const BC *b, int i) {
    return (uint32_t)(((uint64_t)b->n * (uint64_t)i) / (uint64_t)b->parts);
}
static inline uint32_t phi(const BC *b, int i) {
    return (uint32_t)(((uint64_t)b->n * (uint64_t)(i + 1)) / (uint64_t)b->parts);
}

typedef void (*RangeFn)(void *, int);
typedef struct { RangeFn fn; void *ctx; int idx; } RA;

static void *range_thread(void *v) {
    RA *a = (RA *)v;
    a->fn(a->ctx, a->idx);
    return NULL;
}
/* Runs fn(ctx, 0..count-1); the caller handles index 0 itself. */
static void run_ranges(int count, RangeFn fn, void *ctx) {
    pthread_t th[MB];
    RA args[MB];
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    int started = 0;
    for (int i = 1; i < count; i++) {
        args[i].fn = fn; args[i].ctx = ctx; args[i].idx = i;
        if (pthread_create(&th[started], &at, range_thread, &args[i]) != 0) {
            fn(ctx, i);   /* could not start it: do this range inline */
            continue;
        }
        started++;
    }
    pthread_attr_destroy(&at);
    fn(ctx, 0);
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
}

static void part_grow(Part *p) {
    uint32_t nc = p->cap ? p->cap + p->cap / 2 : 4096;
    uint32_t *a = (uint32_t *)realloc(p->tu, (size_t)nc * 4);
    uint32_t *b = (uint32_t *)realloc(p->tv, (size_t)nc * 4);
    uint32_t *c = (uint32_t *)realloc(p->tw, (size_t)nc * 4);
    if (!a || !b || !c) { fputs("oom\n", stderr); exit(1); }
    p->tu = a; p->tv = b; p->tw = c; p->cap = nc;
}

static void build_range(void *v, int idx) {
    BC *b = (BC *)v;
    Part *pr = &b->part[idx];
    const char *p = b->range[idx].start, *end = b->range[idx].end;
    uint32_t n = b->n, i = pr->count, mx = b->maxw[idx];
    uint64_t *bits = pr->bits;
    const uint64_t *tm = b->target_mask;
    for (; p < end; i++) {
        while (p < end && (unsigned)(*p - '0') > 9) p++;
        if (p >= end) break;
        uint32_t u, v2, w;
        p = pu32(p, end, &u);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        p = pu32(p, end, &v2);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        p = pu32(p, end, &w);
        p = skip_line(p, end);
        if (__builtin_expect(i == pr->cap, 0)) part_grow(pr);
        pr->tu[i] = u; pr->tv[i] = v2; pr->tw[i] = w;
        /* Edges at or above FLIM can never take part in a fast candidate, so
         * they do not constrain the bucket count either. */
        if (w < FLIM && w > mx) mx = w;
        if (__builtin_expect(v2 < n && (tm[v2 >> 6] & (1ULL << (v2 & 63))) != 0, 0))
            bits[v2 >> 6] |= 1ULL << (v2 & 63);
    }
    pr->count = i;
    b->maxw[idx] = mx;
}

/* Out-degrees of this part's id range, in a private cache-resident counter.
 * Counting into one shared array costs a random access into 7 MB per edge. */
static void count_range(void *v, int idx) {
    BC *b = (BC *)v;
    uint32_t lo = plo(b, idx), hi = phi(b, idx), span = hi - lo;
    uint32_t *loc = b->loc + (size_t)idx * b->loc_stride;
    memset(loc, 0, (size_t)span * 4);
    for (int r = 0; r < b->parts; r++) {
        const Part *pr = &b->part[r];
        const uint32_t *src = pr->tu;
        uint32_t k = pr->count;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t u = src[i];
            if (u - lo < span) loc[u - lo]++;   /* true iff lo <= u < hi */
        }
    }
    uint32_t acc = 0;
    for (uint32_t k = 0; k < span; k++) { uint32_t c = loc[k]; loc[k] = acc; acc += c; }
    b->base[idx + 1] = acc;
}

/* Write this part's offsets and scatter its edges; loc already holds the local
 * prefix sums, so it doubles as the cursor array: private and cache-hot. */
static void fill_range(void *v, int idx) {
    BC *b = (BC *)v;
    uint32_t lo = plo(b, idx), hi = phi(b, idx), span = hi - lo;
    uint32_t *loc = b->loc + (size_t)idx * b->loc_stride;
    uint32_t *head = b->head, base = b->base[idx];
    Edge *edges = b->edges;
    for (uint32_t k = 0; k < span; k++) {
        uint32_t p = base + loc[k];
        loc[k] = p;
        head[lo + k] = p;
    }
    if (hi == b->n) head[b->n] = b->base[b->parts];
    for (int r = 0; r < b->parts; r++) {
        const Part *pr = &b->part[r];
        const uint32_t *su = pr->tu, *sv = pr->tv, *sw = pr->tw;
        uint32_t k = pr->count;
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
    const char *data = "/data", *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }
    char path[1024];
    /* Queries first: their target ids decide which in-degrees we must know. */
    snprintf(path, sizeof path, "%s/queries.txt", data);
    int qfd = open(path, O_RDONLY);
    if (qfd < 0) return 1;
    struct stat qst;
    if (fstat(qfd, &qst) != 0 || qst.st_size <= 0) { close(qfd); return 1; }
    size_t qsize = (size_t)qst.st_size;
    const char *qp = (const char *)mmap(NULL, qsize, PROT_READ, MAP_PRIVATE, qfd, 0);
    if (qp == MAP_FAILED) { close(qfd); return 1; }
    const char *qend = qp + qsize;
    uint32_t qcap = 4096;
    Query *queries = (Query *)malloc((size_t)qcap * sizeof(Query));
    if (!queries) return 1;
    int nq = 0;
    while (qp < qend) {
        while (qp < qend && (unsigned)(*qp - '0') > 9) qp++;
        if (qp >= qend) break;
        uint32_t s, t;
        qp = pu32(qp, qend, &s);
        while (qp < qend && (*qp == ' ' || *qp == '\t')) qp++;
        qp = pu32(qp, qend, &t);
        qp = skip_line(qp, qend);
        if (nq >= (int)qcap) {
            qcap *= 2;
            Query *nqq = (Query *)realloc(queries, (size_t)qcap * sizeof(Query));
            if (!nqq) return 1;
            queries = nqq;
        }
        queries[nq].s = s; queries[nq].t = t; queries[nq].ans = -1;
        nq++;
    }
    munmap((void *)qp, qsize);
    close(qfd);

    snprintf(path, sizeof path, "%s/graph.txt", data);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 1; }
    size_t fsize = (size_t)st.st_size;
    /* Map only the header: the edge text is consumed a window at a time, since
     * mapping the whole ~90 MB at once would set the peak RSS for the run. */
    size_t hlen = fsize < 4096 ? fsize : 4096;
    const char *hdr = (const char *)mmap(NULL, hlen, PROT_READ, MAP_PRIVATE, fd, 0);
    if (hdr == MAP_FAILED) { close(fd); return 1; }
    const char *hend = hdr + hlen;
    uint32_t n = 0, m = 0;
    const char *p = pu32(hdr, hend, &n);
    while (p < hend && (*p == ' ' || *p == '\t')) p++;
    p = pu32(p, hend, &m);
    p = skip_line(p, hend);
    size_t text_off = (size_t)(p - hdr);
    munmap((void *)hdr, hlen);

    size_t words = (size_t)((n + 63) >> 6);
    if (!words) words = 1;
    uint64_t *tmask = (uint64_t *)calloc(words, 8);
    uint64_t *has_in = (uint64_t *)calloc(words, 8);
    if (!tmask || !has_in) return 1;
    for (int i = 0; i < nq; i++) {
        uint32_t t = queries[i].t;
        if (t < n) tmask[t >> 6] |= 1ULL << (t & 63);
    }

    int bt = cpus();
    if (bt > MB) bt = MB;
    if (bt < 1) bt = 1;
    if ((uint32_t)bt > m && m) bt = (int)m;
    if (bt < 1) bt = 1;

    BC b;
    memset(&b, 0, sizeof b);
    b.n = n; b.m = m; b.target_mask = tmask; b.parts = bt;
    /* Byte ranges are equal in size and edge lines have nearly constant length,
     * so a small proportional reservation covers the digit-length variance and
     * the grow path is only a safety net. */
    uint32_t per = m / (uint32_t)bt, reserve = per + per / 16 + 1024u;
    for (int i = 0; i < bt; i++) {
        b.part[i].cap = reserve;
        b.part[i].tu = (uint32_t *)malloc((size_t)reserve * 4);
        b.part[i].tv = (uint32_t *)malloc((size_t)reserve * 4);
        b.part[i].tw = (uint32_t *)malloc((size_t)reserve * 4);
        b.part[i].bits = (uint64_t *)calloc(words, 8);
        if (!b.part[i].tu || !b.part[i].tv || !b.part[i].tw || !b.part[i].bits) return 1;
    }
    b.loc_stride = n / (uint32_t)bt + 2;
    b.loc = (uint32_t *)malloc((size_t)bt * b.loc_stride * 4);
    b.base = (uint32_t *)calloc((size_t)bt + 1, 4);
    b.head = (uint32_t *)malloc(((size_t)n + 1) * 4);
    if (!b.loc || !b.base || !b.head) return 1;

    {   /* Pass 1: parse the graph text one line-aligned window at a time. */
        const size_t WIN = (size_t)32 << 20;
        size_t off = text_off, page = (size_t)sysconf(_SC_PAGESIZE);
        while (off < fsize) {
            size_t wend = off + WIN;
            if (wend >= fsize) wend = fsize;
            else wend = line_after(fd, wend, fsize);
            size_t wlen = wend - off, map_off = off & ~(page - 1), delta = off - map_off;
            const char *wbase = (const char *)mmap(NULL, wlen + delta, PROT_READ,
                                                   MAP_PRIVATE, fd, (off_t)map_off);
            if (wbase == MAP_FAILED) return 1;
            const char *w = wbase + delta, *wendp = w + wlen;
            b.range[0].start = w;
            const char *prev = w;
            for (int i = 1; i < bt; i++) {
                const char *q = w + (wlen * (size_t)i) / (size_t)bt;
                if (q < prev) q = prev;
                q = skip_line(q, wendp);   /* chunks start on a line boundary */
                if (q < prev) q = prev;
                b.range[i - 1].end = q;
                b.range[i].start = q;
                prev = q;
            }
            b.range[bt - 1].end = wendp;
            run_ranges(bt, build_range, &b);
            munmap((void *)wbase, wlen + delta);
            off = wend;
        }
    }
    close(fd);

    /* Pass 2: out-degrees per node, counted inside each part's id range. */
    run_ranges(bt, count_range, &b);
    b.base[0] = 0;
    for (int i = 1; i <= bt; i++) b.base[i] += b.base[i - 1];

    /* Pass 3: write the offsets and fill the CSR. */
    Edge *edges = (Edge *)malloc((size_t)m * sizeof(Edge));
    if (!edges) return 1;
    b.edges = edges;
    run_ranges(bt, fill_range, &b);

    for (int i = 0; i < bt; i++) {
        const uint64_t *src = b.part[i].bits;
        for (size_t k = 0; k < words; k++) has_in[k] |= src[k];
        free(b.part[i].bits); free(b.part[i].tu); free(b.part[i].tv); free(b.part[i].tw);
    }
    free(b.loc); free(b.base);
    uint32_t *head = b.head;

    /* If every weight the fast path can use is below the bucket count, the
     * frontier can be bucketed by distance instead of keyed in a radix heap. */
    uint32_t maxw = 0;
    for (int i = 0; i < bt; i++) if (b.maxw[i] > maxw) maxw = b.maxw[i];
    int dial = maxw < DB;

    /* Relabel in BFS order and rewrite the CSR in it: only a permutation. */
    if (n >= 2) {
        uint32_t *pos = (uint32_t *)malloc((size_t)n * 4);
        uint32_t *order = (uint32_t *)malloc((size_t)n * 4);
        uint32_t *head2 = (uint32_t *)malloc(((size_t)n + 1) * 4);
        Edge *edges2 = (Edge *)malloc((size_t)m * sizeof(Edge));
        uint64_t *seen = (uint64_t *)calloc(((size_t)n + 63) / 64, 8);
        uint32_t *clo = (uint32_t *)malloc((size_t)bt * 4);
        uint32_t *chi = (uint32_t *)malloc((size_t)bt * 4);
        if (!pos || !order || !head2 || !edges2 || !seen || !clo || !chi) return 1;
        bfs_order(n, head, edges, seen, order, head2);
        free(seen);
        for (int i = 0; i < bt; i++) {
            clo[i] = (uint32_t)(((uint64_t)n * (uint64_t)i) / (uint64_t)bt);
            chi[i] = (uint32_t)(((uint64_t)n * (uint64_t)(i + 1)) / (uint64_t)bt);
        }
        PosC pc;
        pc.order = order; pc.pos = pos; pc.lo = clo; pc.hi = chi;
        run_ranges(bt, pos_range, &pc);
        head2[0] = 0;
        for (uint32_t nu = 0; nu < n; nu++) head2[nu + 1] += head2[nu];
        memset(has_in, 0, words * 8);
        PermC mc;
        mc.head = head; mc.edges = edges; mc.pos = pos; mc.head2 = head2;
        mc.edges2 = edges2; mc.order = order; mc.lo = clo; mc.hi = chi;
        mc.target_mask = tmask; mc.has_in = has_in; mc.n = n;
        run_ranges(bt, permute_range, &mc);
        for (int i = 0; i < nq; i++) {
            uint32_t qs = queries[i].s, qt = queries[i].t;
            queries[i].s = qs < n ? pos[qs] : qs;
            queries[i].t = qt < n ? pos[qt] : qt;
        }
        free(head); free(edges); free(pos); free(order); free(clo); free(chi);
        head = head2;
        edges = edges2;
    }
    free(tmask);

    int nt = cpus();
    if (nt > MT) nt = MT;
    if (nt < 1) nt = 1;
    if (nt > nq && nq > 0) nt = nq;
    pthread_t th[MT];
    WC c;
    c.n = n; c.head = head; c.edges = edges; c.has_in = has_in;
    c.q = queries; c.nq = nq; c.dbits = dbits(n); c.dial = dial;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    int started = 0;
    for (int i = 0; i < nt; i++) {
        if (pthread_create(&th[i], &at, worker, &c) != 0) break;
        started++;
    }
    pthread_attr_destroy(&at);
    if (started == 0) worker(&c);   /* no thread could be created: do it here */
    for (int i = 0; i < started; i++) pthread_join(th[i], NULL);

    mkdir(out_dir, 0777);
    snprintf(path, sizeof path, "%s/distances.txt", out_dir);
    FILE *out = fopen(path, "wb");
    if (!out) return 1;
    char obuf[65536];
    setvbuf(out, obuf, _IOFBF, sizeof obuf);
    for (int i = 0; i < nq; i++) fprintf(out, "%lld\n", (long long)queries[i].ans);
    fclose(out);
    free(head); free(edges); free(has_in); free(queries);
    return 0;
}
