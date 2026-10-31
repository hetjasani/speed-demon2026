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

#define MAX_THREADS 16

typedef struct {
    uint32_t v;
    uint32_t w;
} Edge;

typedef struct {
    uint32_t s;
    uint32_t t;
    int64_t ans;
} Query;

typedef struct {
    int64_t d;
    uint32_t u;
} HeapNode;

typedef struct {
    HeapNode *data;
    uint32_t size;
    uint32_t cap;
} MinHeap;

static inline void heap_push(MinHeap *h, int64_t d, uint32_t u) {
    if (__builtin_expect(h->size >= h->cap, 0)) {
        h->cap *= 2;
        h->data = (HeapNode *)realloc(h->data, h->cap * sizeof(HeapNode));
    }
    uint32_t i = h->size++;
    while (i > 0) {
        uint32_t p = (i - 1) >> 2;
        if (h->data[p].d <= d) break;
        h->data[i] = h->data[p];
        i = p;
    }
    h->data[i].d = d;
    h->data[i].u = u;
}

static inline HeapNode heap_pop(MinHeap *h) {
    HeapNode top = h->data[0];
    uint32_t n = --h->size;
    if (n > 0) {
        HeapNode last = h->data[n];
        uint32_t i = 0;
        while (1) {
            uint32_t child = (i << 2) + 1;
            if (child >= n) break;
            uint32_t min_c = child;
            int64_t min_d = h->data[child].d;

            if (child + 1 < n && h->data[child + 1].d < min_d) {
                min_c = child + 1;
                min_d = h->data[child + 1].d;
            }
            if (child + 2 < n && h->data[child + 2].d < min_d) {
                min_c = child + 2;
                min_d = h->data[child + 2].d;
            }
            if (child + 3 < n && h->data[child + 3].d < min_d) {
                min_c = child + 3;
                min_d = h->data[child + 3].d;
            }

            if (last.d <= min_d) break;
            h->data[i] = h->data[min_c];
            i = min_c;
        }
        h->data[i] = last;
    }
    return top;
}

static int64_t dijkstra(uint32_t s, uint32_t t, uint32_t n,
                        const uint32_t *head, const Edge *edges,
                        int64_t *dist, uint32_t *epoch, uint32_t *cur_epoch_ptr,
                        MinHeap *heap) {
    if (s == t) return 0;
    if (s >= n || t >= n) return -1;

    uint32_t cur_epoch = ++(*cur_epoch_ptr);
    if (__builtin_expect(cur_epoch == 0, 0)) {
        memset(epoch, 0, n * sizeof(uint32_t));
        cur_epoch = ++(*cur_epoch_ptr);
    }

    heap->size = 0;
    epoch[s] = cur_epoch;
    dist[s] = 0;
    heap_push(heap, 0, s);

    while (heap->size > 0) {
        HeapNode top = heap_pop(heap);
        int64_t d = top.d;
        uint32_t u = top.u;

        if (d > dist[u]) continue;
        if (u == t) return d;

        uint32_t start_edge = head[u];
        uint32_t end_edge = head[u + 1];

        for (uint32_t e = start_edge; e < end_edge; e++) {
            uint32_t v = edges[e].v;
            int64_t nd = d + edges[e].w;

            if (epoch[v] != cur_epoch) {
                epoch[v] = cur_epoch;
                dist[v] = nd;
                heap_push(heap, nd, v);
            } else if (nd < dist[v]) {
                dist[v] = nd;
                heap_push(heap, nd, v);
            }
        }
    }
    return -1;
}

typedef struct {
    uint32_t n;
    const uint32_t *head;
    const Edge *edges;
    Query *queries;
    int num_queries;
} WorkerContext;

static atomic_int global_query_idx = 0;

static void* worker_func(void *arg) {
    WorkerContext *ctx = (WorkerContext *)arg;
    uint32_t n = ctx->n;

    int64_t *dist = (int64_t *)malloc(n * sizeof(int64_t));
    uint32_t *epoch = (uint32_t *)calloc(n, sizeof(uint32_t));
    uint32_t cur_epoch = 0;

    MinHeap heap;
    heap.size = 0;
    heap.cap = 65536;
    heap.data = (HeapNode *)malloc(heap.cap * sizeof(HeapNode));

    while (1) {
        int q = atomic_fetch_add_explicit(&global_query_idx, 1, memory_order_relaxed);
        if (q >= ctx->num_queries) break;

        ctx->queries[q].ans = dijkstra(ctx->queries[q].s, ctx->queries[q].t, n,
                                      ctx->head, ctx->edges,
                                      dist, epoch, &cur_epoch, &heap);
    }

    free(dist);
    free(epoch);
    free(heap.data);
    return NULL;
}

int main(int argc, char **argv) {
    const char *data_dir = "/data";
    const char *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    char graph_path[1024];
    snprintf(graph_path, sizeof(graph_path), "%s/graph.txt", data_dir);
    int fd = open(graph_path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    const char *p = (const char *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    const char *end = p + file_size;
    posix_madvise((void *)p, file_size, POSIX_MADV_WILLNEED | POSIX_MADV_SEQUENTIAL);

    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    uint32_t n = 0;
    while (p < end && *p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    uint32_t m = 0;
    while (p < end && *p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0');
    while (p < end && *p != '\n') p++;
    if (p < end && *p == '\n') p++;

    const char *p_edges_start = p;
    uint32_t *deg = (uint32_t *)calloc(n + 1, sizeof(uint32_t));

    // Pass 1: compute node degrees
    for (uint32_t i = 0; i < m; i++) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= end) break;
        uint32_t u = 0;
        while (p < end && *p >= '0' && *p <= '9') u = u * 10 + (*p++ - '0');
        while (p < end && *p != '\n') p++;
        if (p < end && *p == '\n') p++;
        if (u < n) deg[u]++;
    }

    uint32_t *head = (uint32_t *)malloc((n + 2) * sizeof(uint32_t));
    head[0] = 0;
    for (uint32_t i = 0; i < n; i++) head[i + 1] = head[i] + deg[i];
    head[n + 1] = head[n];
    memcpy(deg, head, (n + 1) * sizeof(uint32_t));

    // Pass 2: populate CSR edge array
    p = p_edges_start;
    Edge *edges = (Edge *)malloc(m * sizeof(Edge));
    for (uint32_t i = 0; i < m; i++) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (p >= end) break;
        uint32_t u = 0;
        while (p < end && *p >= '0' && *p <= '9') u = u * 10 + (*p++ - '0');
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        uint32_t v = 0;
        while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        uint32_t w = 0;
        while (p < end && *p >= '0' && *p <= '9') w = w * 10 + (*p++ - '0');
        while (p < end && *p != '\n') p++;
        if (p < end && *p == '\n') p++;

        if (u < n) {
            uint32_t pos = deg[u]++;
            edges[pos].v = v;
            edges[pos].w = w;
        }
    }
    free(deg);
    munmap((void *)p_edges_start - (p_edges_start - (const char *)p), file_size);
    close(fd);

    // Read queries
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);
    int qfd = open(queries_path, O_RDONLY);
    if (qfd < 0) return 1;

    struct stat qst;
    fstat(qfd, &qst);
    const char *qp = (const char *)mmap(NULL, qst.st_size, PROT_READ, MAP_SHARED, qfd, 0);
    const char *qend = qp + qst.st_size;

    uint32_t q_cap = 4096;
    Query *queries = (Query *)malloc(q_cap * sizeof(Query));
    int num_queries = 0;

    while (qp < qend) {
        while (qp < qend && (*qp == ' ' || *qp == '\t' || *qp == '\r' || *qp == '\n')) qp++;
        if (qp >= qend) break;
        uint32_t s = 0;
        while (qp < qend && *qp >= '0' && *qp <= '9') s = s * 10 + (*qp++ - '0');
        while (qp < qend && (*qp == ' ' || *qp == '\t')) qp++;
        uint32_t t = 0;
        while (qp < qend && *qp >= '0' && *qp <= '9') t = t * 10 + (*qp++ - '0');
        while (qp < qend && *qp != '\n') qp++;
        if (qp < qend && *qp == '\n') qp++;

        if (num_queries >= (int)q_cap) {
            q_cap *= 2;
            queries = (Query *)realloc(queries, q_cap * sizeof(Query));
        }
        queries[num_queries].s = s;
        queries[num_queries].t = t;
        queries[num_queries].ans = -1;
        num_queries++;
    }
    munmap((void *)qp, qst.st_size);
    close(qfd);

    // Run parallel Dijkstra
    int num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads > 14) num_threads = 14;
    if (num_threads < 1) num_threads = 1;

    pthread_t threads[MAX_THREADS];
    WorkerContext ctx = {
        .n = n,
        .head = head,
        .edges = edges,
        .queries = queries,
        .num_queries = num_queries
    };

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], &attr, worker_func, &ctx);
    }
    pthread_attr_destroy(&attr);

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

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
    free(queries);
    return 0;
}