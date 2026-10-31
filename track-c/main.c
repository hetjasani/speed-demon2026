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

#define REG_CAP       1048576
#define MAX_COUNTRIES 1024
#define MAX_THREADS   16

typedef struct {
    int64_t id;
    int32_t c_idx;
} RegEntry;
static RegEntry reg_table[REG_CAP];

static char *country_names[MAX_COUNTRIES];
static int total_countries = 0;
static int unmatched_idx = -1;

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    char data[65536];
} ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

static inline char* arena_alloc_str(Arena *a, const char *s, size_t len) {
    if (!a->head || a->head->used + len + 1 > sizeof(a->head->data)) {
        ArenaBlock *b = (ArenaBlock *)malloc(sizeof(ArenaBlock));
        b->next = a->head;
        b->used = 0;
        a->head = b;
    }
    char *dest = a->head->data + a->head->used;
    memcpy(dest, s, len);
    dest[len] = '\0';
    a->head->used += len + 1;
    return dest;
}

static int get_country_idx(const char *name, Arena *arena) {
    for (int i = 0; i < total_countries; i++) {
        if (strcmp(country_names[i], name) == 0) return i;
    }
    if (total_countries >= MAX_COUNTRIES) return unmatched_idx;
    int idx = total_countries++;
    country_names[idx] = arena_alloc_str(arena, name, strlen(name));
    return idx;
}

static inline void reg_put(int64_t id, int32_t c_idx) {
    uint32_t mask = REG_CAP - 1;
    uint32_t slot = ((uint64_t)id * 11400714819323198485ULL) & mask;
    while (reg_table[slot].c_idx != -1) {
        if (reg_table[slot].id == id) {
            reg_table[slot].c_idx = c_idx;
            return;
        }
        slot = (slot + 1) & mask;
    }
    reg_table[slot].id = id;
    reg_table[slot].c_idx = c_idx;
}

static inline int32_t reg_get(int64_t id) {
    uint32_t mask = REG_CAP - 1;
    uint32_t slot = ((uint64_t)id * 11400714819323198485ULL) & mask;
    while (reg_table[slot].c_idx != -1) {
        if (reg_table[slot].id == id) return reg_table[slot].c_idx;
        slot = (slot + 1) & mask;
    }
    return unmatched_idx;
}

static inline uint32_t fast_hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h ? h : 1;
}

typedef struct {
    uint32_t hash;
    uint32_t len;
    const char *str;
} NameEntry;

typedef struct {
    int64_t count;
    int64_t id_sum;
    int64_t amount_sum;
    int64_t amount_min;
    int64_t amount_max;
    uint64_t name_bytes;
    uint64_t note_bytes;
    uint32_t names_cap;
    uint32_t names_count;
    NameEntry *names_table;
    const char *last_name;
    uint32_t last_name_len;
} Agg;

static inline void agg_add_name(Agg *c, const char *name, size_t len, Arena *arena) {
    if (c->last_name_len == len && c->last_name && memcmp(c->last_name, name, len) == 0) return;
    if (!c->names_table) {
        c->names_cap = 256;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
    } else if (c->names_count * 2 >= c->names_cap) {
        uint32_t old_cap = c->names_cap;
        NameEntry *old_t = c->names_table;
        c->names_cap *= 2;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
        uint32_t mask = c->names_cap - 1;
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old_t[i].str) {
                uint32_t slot = old_t[i].hash & mask;
                while (c->names_table[slot].str) slot = (slot + 1) & mask;
                c->names_table[slot] = old_t[i];
            }
        }
        free(old_t);
    }

    uint32_t h = fast_hash(name, len);
    uint32_t mask = c->names_cap - 1;
    uint32_t slot = h & mask;
    while (c->names_table[slot].str) {
        if (c->names_table[slot].hash == h && c->names_table[slot].len == len &&
            memcmp(c->names_table[slot].str, name, len) == 0) {
            c->last_name = c->names_table[slot].str;
            c->last_name_len = (uint32_t)len;
            return;
        }
        slot = (slot + 1) & mask;
    }

    c->names_table[slot].len = (uint32_t)len;
    c->names_table[slot].hash = h;
    c->names_table[slot].str = arena_alloc_str(arena, name, len);
    c->last_name = c->names_table[slot].str;
    c->last_name_len = (uint32_t)len;
    c->names_count++;
}

typedef struct {
    const char *start;
    const char *end;
    Agg agg[MAX_COUNTRIES];
    Arena arena;
} Worker;

static void* worker_func(void *arg) {
    Worker *w = (Worker *)arg;
    const char *p = w->start;
    const char *end = w->end;

    for (int i = 0; i < total_countries; i++) {
        w->agg[i].amount_min = INT64_MAX;
        w->agg[i].amount_max = INT64_MIN;
    }

    char ubuf[65536];
    int64_t last_reg = -1;
    int32_t last_c = -1;

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (p >= end) break;
        if (p + 2 < end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        if (p >= end) break;

        // 1. id
        int64_t id = 0;
        while (p < end && *p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0');
        if (p < end && *p == ',') p++;

        // 2. name
        const char *np = "";
        size_t nl = 0;
        if (p < end && *p == '"') {
            p++;
            const char *st = p;
            int esc = 0;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') { esc = 1; p += 2; }
                    else break;
                } else p++;
            }
            if (!esc) {
                np = st; nl = p - st;
            } else {
                char *d = ubuf;
                for (const char *s = st; s < p;) {
                    if (*s == '"' && *(s + 1) == '"') { *d++ = '"'; s += 2; }
                    else *d++ = *s++;
                }
                np = ubuf; nl = d - ubuf;
            }
            if (p < end && *p == '"') p++;
            if (p < end && *p == ',') p++;
        } else {
            const char *st = p;
            while (p < end && *p != ',') p++;
            np = st; nl = p - st;
            if (p < end && *p == ',') p++;
        }

        // 3. note
        size_t note_l = 0;
        if (p < end && *p == '"') {
            p++;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') { note_l++; p += 2; }
                    else { p++; break; }
                } else { note_l++; p++; }
            }
            if (p < end && *p == ',') p++;
        } else {
            const char *st = p;
            while (p < end && *p != ',') p++;
            note_l = p - st;
            if (p < end && *p == ',') p++;
        }

        // 4. region_id
        int64_t reg = 0;
        while (p < end && *p >= '0' && *p <= '9') reg = reg * 10 + (*p++ - '0');
        if (p < end && *p == ',') p++;

        // 5. amount
        int sgn = 1;
        if (p < end && *p == '"') p++;
        if (p < end && *p == '-') { sgn = -1; p++; }
        int64_t cents = 0;
        while (p < end && *p >= '0' && *p <= '9') cents = cents * 10 + (*p++ - '0');
        if (p < end && *p == '.') p++;
        cents = cents * 100 + (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
        if (sgn < 0) cents = -cents;
        if (p < end && *p == '"') p++;
        while (p < end && (*p == '\r' || *p == '\n')) p++;

        int32_t ci = (reg == last_reg) ? last_c : (last_c = reg_get(reg), last_reg = reg, last_c);
        Agg *c = &w->agg[ci];
        c->count++;
        c->id_sum += id;
        c->amount_sum += cents;
        if (cents < c->amount_min) c->amount_min = cents;
        if (cents > c->amount_max) c->amount_max = cents;
        c->name_bytes += nl;
        c->note_bytes += note_l;
        agg_add_name(c, np, nl, &w->arena);
    }
    return NULL;
}

static int cmp_country(const void *a, const void *b) {
    return strcmp(country_names[*(const int *)a], country_names[*(const int *)b]);
}

int main(int argc, char **argv) {
    const char *data_dir = "/data", *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }

    Arena main_arena = {0};
    for (int i = 0; i < REG_CAP; i++) reg_table[i].c_idx = -1;
    unmatched_idx = get_country_idx("_UNMATCHED", &main_arena);

    char path[1024];
    snprintf(path, sizeof(path), "%s/regions.csv", data_dir);
    FILE *rf = fopen(path, "rb");
    if (rf) {
        char l[1024];
        if (fgets(l, sizeof(l), rf)) {}
        while (fgets(l, sizeof(l), rf)) {
            char *p = l;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) p[--len] = '\0';
            if (len == 0) continue;
            char *comma = strchr(p, ',');
            if (!comma) continue;
            *comma = '\0';
            int64_t id = 0;
            char *ip = p;
            while (*ip >= '0' && *ip <= '9') id = id * 10 + (*ip++ - '0');
            reg_put(id, get_country_idx(comma + 1, &main_arena));
        }
        fclose(rf);
    }

    snprintf(path, sizeof(path), "%s/records.csv", data_dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    fstat(fd, &st);
    size_t fsize = st.st_size;
    const char *data = (const char *)mmap(NULL, fsize, PROT_READ, MAP_SHARED, fd, 0);

    const char *p_rec = data;
    const char *end = data + fsize;
    if (p_rec + 2 < end && (unsigned char)p_rec[0] == 0xEF && (unsigned char)p_rec[1] == 0xBB && (unsigned char)p_rec[2] == 0xBF) p_rec += 3;
    while (p_rec < end && *p_rec != '\n') p_rec++;
    if (p_rec < end) p_rec++;

    int num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
    if (num_threads < 1) num_threads = 1;

    const char *starts[MAX_THREADS], *ends[MAX_THREADS];
    starts[0] = p_rec;
    size_t stride = (end - p_rec) / num_threads;
    int in_quote = 0;
    const char *scan = p_rec;
    int t = 1;
    const char *target = p_rec + stride;

    while (scan < end && t < num_threads) {
        if (*scan == '"') {
            in_quote = !in_quote;
        } else if (*scan == '\n' && !in_quote) {
            if (scan >= target) {
                const char *next = scan + 1;
                if (next + 2 < end && (unsigned char)next[0] == 0xEF && (unsigned char)next[1] == 0xBB && (unsigned char)next[2] == 0xBF)
                    next += 3;
                starts[t] = next;
                ends[t - 1] = next;
                t++;
                target = p_rec + (size_t)t * stride;
            }
        }
        scan++;
    }
    num_threads = t;
    ends[num_threads - 1] = end;

    pthread_t th[MAX_THREADS];
    Worker workers[MAX_THREADS];
    memset(workers, 0, sizeof(workers));

    for (int i = 0; i < num_threads; i++) {
        workers[i].start = starts[i];
        workers[i].end = ends[i];
        pthread_create(&th[i], NULL, worker_func, &workers[i]);
    }
    for (int i = 0; i < num_threads; i++) pthread_join(th[i], NULL);

    munmap((void *)data, fsize);
    close(fd);

    Agg final_agg[MAX_COUNTRIES];
    memset(final_agg, 0, sizeof(final_agg));

    for (int i = 0; i < total_countries; i++) {
        final_agg[i].amount_min = INT64_MAX;
        final_agg[i].amount_max = INT64_MIN;
        for (int j = 0; j < num_threads; j++) {
            Agg *src = &workers[j].agg[i];
            if (src->count == 0) continue;
            final_agg[i].count += src->count;
            final_agg[i].id_sum += src->id_sum;
            final_agg[i].amount_sum += src->amount_sum;
            if (src->amount_min < final_agg[i].amount_min) final_agg[i].amount_min = src->amount_min;
            if (src->amount_max > final_agg[i].amount_max) final_agg[i].amount_max = src->amount_max;
            final_agg[i].name_bytes += src->name_bytes;
            final_agg[i].note_bytes += src->note_bytes;
            if (src->names_table) {
                for (uint32_t s = 0; s < src->names_cap; s++) {
                    if (src->names_table[s].str) {
                        agg_add_name(&final_agg[i], src->names_table[s].str, src->names_table[s].len, &main_arena);
                    }
                }
                free(src->names_table);
            }
        }
    }

    int valid[MAX_COUNTRIES], valid_cnt = 0;
    for (int i = 0; i < total_countries; i++) {
        if (final_agg[i].count > 0) valid[valid_cnt++] = i;
    }
    qsort(valid, valid_cnt, sizeof(int), cmp_country);

    mkdir(out_dir, 0777);
    char out_file[1024];
    snprintf(out_file, sizeof(out_file), "%s/aggregates.txt", out_dir);
    FILE *out = fopen(out_file, "wb");
    setvbuf(out, NULL, _IOFBF, 65536);

    char s_sum[32], s_min[32], s_max[32];
    for (int i = 0; i < valid_cnt; i++) {
        int ci = valid[i];
        Agg *c = &final_agg[ci];
        int64_t v = llabs(c->amount_sum);
        sprintf(s_sum, "%s%lld.%02lld", c->amount_sum < 0 ? "-" : "", (long long)(v / 100), (long long)(v % 100));
        v = llabs(c->amount_min);
        sprintf(s_min, "%s%lld.%02lld", c->amount_min < 0 ? "-" : "", (long long)(v / 100), (long long)(v % 100));
        v = llabs(c->amount_max);
        sprintf(s_max, "%s%lld.%02lld", c->amount_max < 0 ? "-" : "", (long long)(v / 100), (long long)(v % 100));

        fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
                country_names[ci], (long long)c->count, (long long)c->id_sum,
                s_sum, s_min, s_max, c->names_count,
                (unsigned long long)c->name_bytes, (unsigned long long)c->note_bytes);
    }
    fclose(out);
    return 0;
}