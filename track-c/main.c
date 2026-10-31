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

#if defined(__SSE4_2__)
#include <x86intrin.h>
#endif

#define MAX_THREADS       16
#define REGION_TABLE_CAP  1048576   // 1M slots (16 MB static BSS)
#define MAX_COUNTRIES     1024
#define NAME_INIT_CAP     256

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

static inline uint32_t fast_hash_bytes(const char *s, size_t len) {
#if defined(__SSE4_2__)
    uint64_t crc = len;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t v;
        memcpy(&v, s + i, 8);
        crc = _mm_crc32_u64(crc, v);
    }
    if (i + 4 <= len) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        crc = _mm_crc32_u32((uint32_t)crc, v);
        i += 4;
    }
    while (i < len) {
        crc = _mm_crc32_u8((uint32_t)crc, (unsigned char)s[i++]);
    }
    return (uint32_t)crc;
#else
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }
    return h;
#endif
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
} ThreadCountry;

typedef struct {
    char *name;
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
} FinalCountry;

static char *country_names[MAX_COUNTRIES];
static int total_countries = 0;
static int unmatched_country_idx = -1;

static int get_or_create_country_idx(const char *name, Arena *arena) {
    for (int i = 0; i < total_countries; i++) {
        if (strcmp(country_names[i], name) == 0) return i;
    }
    if (total_countries >= MAX_COUNTRIES) return unmatched_country_idx;
    int idx = total_countries++;
    country_names[idx] = arena_alloc_str(arena, name, strlen(name));
    return idx;
}

static inline void thread_country_add_name(ThreadCountry *c, const char *name, size_t len, Arena *arena) {
    if (c->last_name_len == len && c->last_name && memcmp(c->last_name, name, len) == 0) {
        return;
    }
    if (__builtin_expect(!c->names_table, 0)) {
        c->names_cap = NAME_INIT_CAP;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
    } else if (__builtin_expect(c->names_count * 2 >= c->names_cap, 0)) {
        uint32_t old_cap = c->names_cap;
        NameEntry *old_table = c->names_table;
        c->names_cap *= 2;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
        uint32_t mask = c->names_cap - 1;
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old_table[i].str) {
                uint32_t slot = old_table[i].hash & mask;
                while (c->names_table[slot].str) slot = (slot + 1) & mask;
                c->names_table[slot] = old_table[i];
            }
        }
        free(old_table);
    }

    uint32_t h = fast_hash_bytes(name, len);
    uint32_t mask = c->names_cap - 1;
    uint32_t slot = h & mask;
    while (c->names_table[slot].str) {
        if (c->names_table[slot].hash == h &&
            c->names_table[slot].len == len &&
            memcmp(c->names_table[slot].str, name, len) == 0) {
            c->last_name = c->names_table[slot].str;
            c->last_name_len = len;
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

static inline void final_country_add_name(FinalCountry *c, const char *name, size_t len, Arena *arena) {
    if (__builtin_expect(!c->names_table, 0)) {
        c->names_cap = NAME_INIT_CAP * 2;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
    } else if (__builtin_expect(c->names_count * 2 >= c->names_cap, 0)) {
        uint32_t old_cap = c->names_cap;
        NameEntry *old_table = c->names_table;
        c->names_cap *= 2;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
        uint32_t mask = c->names_cap - 1;
        for (uint32_t i = 0; i < old_cap; i++) {
            if (old_table[i].str) {
                uint32_t slot = old_table[i].hash & mask;
                while (c->names_table[slot].str) slot = (slot + 1) & mask;
                c->names_table[slot] = old_table[i];
            }
        }
        free(old_table);
    }

    uint32_t h = fast_hash_bytes(name, len);
    uint32_t mask = c->names_cap - 1;
    uint32_t slot = h & mask;
    while (c->names_table[slot].str) {
        if (c->names_table[slot].hash == h &&
            c->names_table[slot].len == len &&
            memcmp(c->names_table[slot].str, name, len) == 0) {
            return;
        }
        slot = (slot + 1) & mask;
    }

    c->names_table[slot].len = (uint32_t)len;
    c->names_table[slot].hash = h;
    c->names_table[slot].str = arena_alloc_str(arena, name, len);
    c->names_count++;
}

typedef struct {
    int64_t region_id;
    int32_t country_idx;
} RegionEntry;

static RegionEntry region_table[REGION_TABLE_CAP];

static inline void insert_region(int64_t id, int32_t country_idx) {
    uint32_t mask = REGION_TABLE_CAP - 1;
    uint32_t slot = ((uint64_t)id * 11400714819323198485ULL) & mask;
    while (region_table[slot].country_idx != -1) {
        if (region_table[slot].region_id == id) {
            region_table[slot].country_idx = country_idx;
            return;
        }
        slot = (slot + 1) & mask;
    }
    region_table[slot].region_id = id;
    region_table[slot].country_idx = country_idx;
}

static inline int32_t lookup_region(int64_t id) {
    uint32_t mask = REGION_TABLE_CAP - 1;
    uint32_t slot = ((uint64_t)id * 11400714819323198485ULL) & mask;
    while (region_table[slot].country_idx != -1) {
        if (region_table[slot].region_id == id) return region_table[slot].country_idx;
        slot = (slot + 1) & mask;
    }
    return unmatched_country_idx;
}

static void load_regions(const char *path, Arena *arena) {
    for (int i = 0; i < REGION_TABLE_CAP; i++) region_table[i].country_idx = -1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[1024];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        size_t l = strlen(p);
        while (l > 0 && (p[l - 1] == '\r' || p[l - 1] == '\n')) p[--l] = '\0';
        if (l == 0) continue;
        char *comma = strchr(p, ',');
        if (!comma) continue;
        *comma = '\0';

        int64_t id = 0;
        char *id_p = p;
        while (*id_p >= '0' && *id_p <= '9') id = id * 10 + (*id_p++ - '0');

        int c_idx = get_or_create_country_idx(comma + 1, arena);
        insert_region(id, c_idx);
    }
    fclose(f);
}

typedef struct {
    const char *start;
    const char *end;
    ThreadCountry country_list[MAX_COUNTRIES];
    Arena arena;
} ThreadContext;

static void* worker_thread(void *arg) {
    ThreadContext *ctx = (ThreadContext *)arg;
    const char *p = ctx->start;
    const char *end = ctx->end;

    for (int i = 0; i < total_countries; i++) {
        ctx->country_list[i].amount_min = INT64_MAX;
        ctx->country_list[i].amount_max = INT64_MIN;
    }

    char name_unescape_buf[65536];
    int64_t last_reg_id = -1;
    int32_t last_c_idx = -1;

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) p++;
        if (p >= end) break;
        if (p + 2 < end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        if (p >= end) break;

        // Field 0: id
        int64_t id = 0;
        while (p < end && *p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0');
        if (p < end && *p == ',') p++;

        // Field 1: name
        const char *name_ptr = "";
        size_t name_len = 0;
        if (p < end && *p == '"') {
            p++;
            const char *start = p;
            int has_escapes = 0;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        has_escapes = 1;
                        p += 2;
                    } else {
                        break;
                    }
                } else {
                    p++;
                }
            }
            if (!has_escapes) {
                name_ptr = start;
                name_len = p - start;
            } else {
                const char *s = start;
                char *d = name_unescape_buf;
                while (s < p) {
                    if (*s == '"' && *(s + 1) == '"') {
                        *d++ = '"';
                        s += 2;
                    } else {
                        *d++ = *s++;
                    }
                }
                name_ptr = name_unescape_buf;
                name_len = d - name_unescape_buf;
            }
            if (p < end && *p == '"') p++;
            if (p < end && *p == ',') p++;
        } else {
            const char *start = p;
            while (p < end && *p != ',') p++;
            name_ptr = start;
            name_len = p - start;
            if (p < end && *p == ',') p++;
        }

        // Field 2: note
        size_t note_len = 0;
        if (p < end && *p == '"') {
            p++;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        note_len++;
                        p += 2;
                    } else {
                        p++;
                        break;
                    }
                } else {
                    note_len++;
                    p++;
                }
            }
            if (p < end && *p == ',') p++;
        } else {
            const char *start = p;
            while (p < end && *p != ',') p++;
            note_len = p - start;
            if (p < end && *p == ',') p++;
        }

        // Field 3: region_id
        int64_t region_id = 0;
        while (p < end && *p >= '0' && *p <= '9') region_id = region_id * 10 + (*p++ - '0');
        if (p < end && *p == ',') p++;

        // Field 4: amount
        int sign = 1;
        if (p < end && *p == '"') p++;
        if (p < end && *p == '-') { sign = -1; p++; }
        int64_t cents = 0;
        while (p < end && *p >= '0' && *p <= '9') cents = cents * 10 + (*p++ - '0');
        if (p < end && *p == '.') p++;
        cents = cents * 100 + (p[0] - '0') * 10 + (p[1] - '0');
        p += 2;
        if (sign < 0) cents = -cents;
        if (p < end && *p == '"') p++;
        while (p < end && (*p == '\r' || *p == '\n')) p++;

        int32_t c_idx;
        if (__builtin_expect(region_id == last_reg_id, 1)) {
            c_idx = last_c_idx;
        } else {
            c_idx = lookup_region(region_id);
            last_reg_id = region_id;
            last_c_idx = c_idx;
        }

        ThreadCountry *c = &ctx->country_list[c_idx];
        c->count++;
        c->id_sum += id;
        c->amount_sum += cents;
        if (cents < c->amount_min) c->amount_min = cents;
        if (cents > c->amount_max) c->amount_max = cents;
        c->name_bytes += name_len;
        c->note_bytes += note_len;
        thread_country_add_name(c, name_ptr, name_len, &ctx->arena);
    }
    return NULL;
}

static inline void format_cents(char *buf, int64_t cents) {
    int64_t c = cents;
    if (c < 0) {
        c = -c;
        sprintf(buf, "-%lld.%02lld", (long long)(c / 100), (long long)(c % 100));
    } else {
        sprintf(buf, "%lld.%02lld", (long long)(c / 100), (long long)(c % 100));
    }
}

static int cmp_country_ptrs(const void *a, const void *b) {
    return strcmp((*(const FinalCountry **)a)->name, (*(const FinalCountry **)b)->name);
}

int main(int argc, char **argv) {
    const char *data_dir = "/data";
    const char *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    Arena main_arena = {0};
    unmatched_country_idx = get_or_create_country_idx("_UNMATCHED", &main_arena);

    char regions_path[1024];
    snprintf(regions_path, sizeof(regions_path), "%s/regions.csv", data_dir);
    load_regions(regions_path, &main_arena);

    char records_path[1024];
    snprintf(records_path, sizeof(records_path), "%s/records.csv", data_dir);
    int fd = open(records_path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    const char *p = (const char *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    const char *end = p + file_size;

    posix_madvise((void *)p, file_size, POSIX_MADV_WILLNEED | POSIX_MADV_SEQUENTIAL);

    const char *p_rec = p;
    if (p_rec + 2 < end && (unsigned char)p_rec[0] == 0xEF && (unsigned char)p_rec[1] == 0xBB && (unsigned char)p_rec[2] == 0xBF) p_rec += 3;
    while (p_rec < end && *p_rec != '\n') p_rec++;
    if (p_rec < end && *p_rec == '\n') p_rec++;

    int num_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_threads > 12) num_threads = 12;
    if (num_threads < 1) num_threads = 1;

    const char *chunk_starts[MAX_THREADS];
    const char *chunk_ends[MAX_THREADS];

    chunk_starts[0] = p_rec;
    size_t total_bytes = end - p_rec;
    size_t stride = total_bytes / num_threads;

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
                chunk_starts[t] = next;
                chunk_ends[t - 1] = next;
                t++;
                target = p_rec + (size_t)t * stride;
            }
        }
        scan++;
    }
    num_threads = t;
    chunk_ends[num_threads - 1] = end;

    pthread_t threads[MAX_THREADS];
    ThreadContext *contexts = (ThreadContext *)calloc(num_threads, sizeof(ThreadContext));

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);

    for (int i = 0; i < num_threads; i++) {
        contexts[i].start = chunk_starts[i];
        contexts[i].end = chunk_ends[i];
        pthread_create(&threads[i], &attr, worker_thread, &contexts[i]);
    }
    pthread_attr_destroy(&attr);

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    munmap((void *)p, file_size);
    close(fd);

    FinalCountry final_countries[MAX_COUNTRIES];
    memset(final_countries, 0, sizeof(final_countries));

    for (int i = 0; i < total_countries; i++) {
        FinalCountry *final = &final_countries[i];
        final->name = country_names[i];
        final->amount_min = INT64_MAX;
        final->amount_max = INT64_MIN;

        for (int j = 0; j < num_threads; j++) {
            ThreadCountry *src = &contexts[j].country_list[i];
            if (src->count == 0) continue;
            final->count += src->count;
            final->id_sum += src->id_sum;
            final->amount_sum += src->amount_sum;
            if (src->amount_min < final->amount_min) final->amount_min = src->amount_min;
            if (src->amount_max > final->amount_max) final->amount_max = src->amount_max;
            final->name_bytes += src->name_bytes;
            final->note_bytes += src->note_bytes;

            if (src->names_table) {
                for (uint32_t s = 0; s < src->names_cap; s++) {
                    if (src->names_table[s].str) {
                        final_country_add_name(final, src->names_table[s].str, src->names_table[s].len, &main_arena);
                    }
                }
            }
        }
    }

    FinalCountry *valid_countries[MAX_COUNTRIES];
    int valid_count = 0;
    for (int i = 0; i < total_countries; i++) {
        if (final_countries[i].count > 0) {
            valid_countries[valid_count++] = &final_countries[i];
        }
    }

    qsort(valid_countries, valid_count, sizeof(FinalCountry *), cmp_country_ptrs);

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);
    FILE *out = fopen(out_path, "wb");
    if (!out) return 1;
    setvbuf(out, NULL, _IOFBF, 65536);

    char min_s[32], max_s[32], sum_s[32];
    for (int i = 0; i < valid_count; i++) {
        FinalCountry *c = valid_countries[i];
        format_cents(sum_s, c->amount_sum);
        format_cents(min_s, c->amount_min);
        format_cents(max_s, c->amount_max);
        fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
                c->name, (long long)c->count, (long long)c->id_sum,
                sum_s, min_s, max_s, c->names_count,
                (unsigned long long)c->name_bytes, (unsigned long long)c->note_bytes);
    }
    fclose(out);
    free(contexts);
    return 0;
}