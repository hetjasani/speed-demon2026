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

#define MAX_THREADS 64

/* Small, dynamic per-thread country hash: key = (uint32_t)country id.
 * Only ~150 countries expected; table grows with 2x load factor. */

typedef struct {
    uint32_t key;
    int64_t count, id_sum, amount_sum;
    int64_t min, max;
} HashEntry;

typedef struct {
    HashEntry *table;
    uint32_t cap;
    uint32_t used;
} HashMap;

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x85ebca6b;
    x ^= x >> 13;
    x *= 0xc2b2ae35;
    x ^= x >> 16;
    return x;
}

static void hm_init(HashMap *h) {
    h->cap = 64;
    h->used = 0;
    h->table = (HashEntry *)calloc(h->cap, sizeof(HashEntry));
}

static void hm_ensure(HashMap *h) {
    if (h->used < h->cap * 3 / 4) return;
    uint32_t nc = h->cap * 2;
    HashEntry *old = h->table;
    HashEntry *nt = (HashEntry *)calloc(nc, sizeof(HashEntry));
    for (uint32_t i = 0; i < h->cap; i++) {
        if (old[i].key == 0) continue;
        uint32_t k = old[i].key;
        uint32_t idx = hash_u32(k) & (nc - 1);
        while (nt[idx].key != 0) idx = (idx + 1) & (nc - 1);
        nt[idx] = old[i];
    }
    free(old);
    h->table = nt;
    h->cap = nc;
}

static void hm_insert(HashMap *h, uint32_t country, int64_t id, int64_t amount) {
    hm_ensure(h);
    uint32_t idx = hash_u32(country) & (h->cap - 1);
    while (h->table[idx].key != 0) {
        if (h->table[idx].key == country) break;
        idx = (idx + 1) & (h->cap - 1);
    }
    if (h->table[idx].key == 0) {
        h->table[idx].key = country;
        h->table[idx].count = 0;
        h->table[idx].id_sum = 0;
        h->table[idx].amount_sum = 0;
        h->table[idx].min = id;
        h->table[idx].max = id;
        h->used++;
    }
    HashEntry *e = &h->table[idx];
    e->count++;
    e->id_sum += id;
    e->amount_sum += amount;
    if (id < e->min) e->min = id;
    if (id > e->max) e->max = id;
}

static void hm_free(HashMap *h) {
    free(h->table);
    h->table = NULL; h->cap = 0; h->used = 0;
}

/* ------------------------------------------------------------------------- */
/* Chunk processing: reads a byte range [start, end) from fd, parses CSV   */
/* lines. Returns the number of rows parsed (for progress tracking).         */
/* ------------------------------------------------------------------------- */

typedef struct {
    int fd;
    off_t off;
    size_t len;
    HashMap *map;
} ChunkCtx;

static void *chunk_worker(void *arg) {
    ChunkCtx *c = (ChunkCtx *)arg;
    char *buf = (char *)malloc(c->len + 1);
    if (!buf) return (void *)(intptr_t)-1;
    ssize_t got = pread(c->fd, buf, c->len, c->off);
    if (got < 0) { free(buf); return (void *)(intptr_t)-1; }
    buf[got] = '\0';

    size_t i = 0;
    while (i < (size_t)got) {
        /* Skip whitespace / newlines between records */
        while (i < (size_t)got && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
        if (i >= (size_t)got) break;

        /* Find end of line */
        size_t j = i;
        while (j < (size_t)got && buf[j] != '\n') j++;
        char *line = buf + i;
        size_t llen = j - i;

        /* Simple split by commas (handles basic CSV, no quoted commas here) */
        char *parts[8];
        int pc = 0;
        size_t p = 0;
        while (p < llen && pc < 8) {
            size_t k = p;
            while (k < llen && line[k] != ',') k++;
            parts[pc++] = line + p;
            line[k] = '\0';
            p = k + 1;
        }
        if (pc >= 4) {
            uint32_t country = (uint32_t)atoi(parts[0]);
            int64_t id = atoll(parts[1]);
            int64_t amount = atoll(parts[3]);
            hm_insert(c->map, country, id, amount);
        }
        i = j + 1;
    }
    /* Drop processed chunk pages from RSS per blueprint */
    if (got > 0) madvise(buf, (size_t)got, MADV_DONTNEED);
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Main: split file into chunks, launch threads, merge results.               */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *data_dir = "/data";
    const char *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/records.csv", data_dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 1; }
    off_t file_size = st.st_size;

    int num_threads = 8;
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    /* Divide into equal byte chunks; each thread scans to a newline */
    off_t chunk_size = file_size / num_threads;
    off_t offsets[MAX_THREADS];
    size_t lengths[MAX_THREADS];
    for (int t = 0; t < num_threads; t++) {
        off_t off = (off_t)((int64_t)t * chunk_size);
        if (t > 0) {
            /* Adjust to line boundary: scan forward from previous chunk end */
            char ch;
            while (pread(fd, &ch, 1, off) == 1 && ch != '\n') off++;
            if (pread(fd, &ch, 1, off) == 1 && ch == '\n') off++; /* skip newline */
        }
        offsets[t] = off;
    }
    for (int t = 0; t < num_threads; t++) {
        off_t start = offsets[t];
        off_t end = (t == num_threads - 1) ? file_size : offsets[t + 1];
        lengths[t] = (size_t)(end > start ? end - start : 0);
    }

    HashMap *maps = (HashMap *)calloc(num_threads, sizeof(HashMap));
    ChunkCtx *contexts = (ChunkCtx *)calloc(num_threads, sizeof(ChunkCtx));
    pthread_t threads[MAX_THREADS];

    for (int t = 0; t < num_threads; t++) {
        hm_init(&maps[t]);
        contexts[t].fd = fd;
        contexts[t].off = offsets[t];
        contexts[t].len = lengths[t];
        contexts[t].map = &maps[t];
        if (lengths[t] > 0) {
            pthread_create(&threads[t], NULL, chunk_worker, &contexts[t]);
        }
    }

    for (int t = 0; t < num_threads; t++) {
        if (lengths[t] > 0) pthread_join(threads[t], NULL);
    }
    close(fd);

    /* Merge: sum all per-thread results into one output */
    /* In a real full version this would write aggregates.txt; kept minimal. */
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);
    FILE *out = fopen(out_path, "wb");
    if (!out) return 1;

    /* Full per-country merge: collect from all threads, write aggregates */
    HashMap final;
    hm_init(&final);
    for (int t = 0; t < num_threads; t++) {
        HashMap *h = &maps[t];
        for (uint32_t i = 0; i < h->cap; i++) {
            if (h->table[i].key == 0) continue;
            uint32_t country = h->table[i].key;
            int64_t id_sum = h->table[i].id_sum;
            int64_t amount_sum = h->table[i].amount_sum;
            int64_t count = h->table[i].count;
            int64_t mn = h->table[i].min;
            int64_t mx = h->table[i].max;
            /* Merge into final: find or create */
            uint32_t idx = hash_u32(country) & (final.cap - 1);
            while (final.table[idx].key != 0 && final.table[idx].key != country) idx = (idx + 1) & (final.cap - 1);
            if (final.table[idx].key == 0) {
                final.table[idx].key = country;
                final.table[idx].count = 0;
                final.table[idx].id_sum = 0;
                final.table[idx].amount_sum = 0;
                final.table[idx].min = id_sum;
                final.table[idx].max = id_sum;
                final.used++;
            }
            final.table[idx].count += count;
            final.table[idx].id_sum += id_sum;
            final.table[idx].amount_sum += amount_sum;
            if (mn < final.table[idx].min) final.table[idx].min = mn;
            if (mx > final.table[idx].max) final.table[idx].max = mx;
        }
    }
    fprintf(out, "country\tcount\tid_sum\tamount_sum\tmin_id\tmax_id\n");
    for (uint32_t i = 0; i < final.cap; i++) {
        if (final.table[i].key == 0) continue;
        fprintf(out, "%u\t%lld\t%lld\t%lld\t%lld\t%lld\n",
                final.table[i].key,
                (long long)final.table[i].count,
                (long long)final.table[i].id_sum,
                (long long)final.table[i].amount_sum,
                (long long)final.table[i].min,
                (long long)final.table[i].max);
    }
    fclose(out);

    hm_free(&final);
    for (int t = 0; t < num_threads; t++) hm_free(&maps[t]);
    free(contexts);
    free(maps);
    return 0;
}
