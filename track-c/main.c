#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define REGION_TABLE_CAP 1048576  // 1M slots for regions (easily fits 2.1MB regions.csv)
#define COUNTRY_TABLE_CAP 65536   // 64k slots for countries

// --- Fast Memory Arena ---
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

static inline uint32_t hash_bytes(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

// --- Regions Hash Table ---
typedef struct {
    int64_t region_id;
    char *country;
    int occupied;
} RegionEntry;

static RegionEntry *region_table = NULL;

static void insert_region(int64_t id, const char *country, Arena *arena) {
    uint32_t mask = REGION_TABLE_CAP - 1;
    uint32_t slot = ((uint64_t)id * 11400714819323198485ULL) & mask;
    while (region_table[slot].occupied) {
        if (region_table[slot].region_id == id) return;
        slot = (slot + 1) & mask;
    }
    region_table[slot].occupied = 1;
    region_table[slot].region_id = id;
    region_table[slot].country = arena_alloc_str(arena, country, strlen(country));
}

static const char* lookup_region(int64_t id) {
    uint32_t mask = REGION_TABLE_CAP - 1;
    uint32_t slot = ((uint64_t)id * 11400714819323198485ULL) & mask;
    while (region_table[slot].occupied) {
        if (region_table[slot].region_id == id) return region_table[slot].country;
        slot = (slot + 1) & mask;
    }
    return "_UNMATCHED";
}

// --- Country Aggregation ---
typedef struct {
    uint32_t len;
    uint32_t hash;
    char *str;
} NameEntry;

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
} Country;

static Country **country_table = NULL;
static Country *country_list = NULL;
static int country_list_count = 0;

static void country_add_name(Country *c, const char *name, size_t len, Arena *arena) {
    if (c->names_cap == 0) {
        c->names_cap = 64;
        c->names_table = (NameEntry *)calloc(c->names_cap, sizeof(NameEntry));
    } else if (c->names_count * 2 >= c->names_cap) {
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

    uint32_t h = hash_bytes(name, len);
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

static Country* get_or_create_country(const char *name, Arena *arena) {
    size_t len = strlen(name);
    uint32_t h = hash_bytes(name, len);
    uint32_t mask = COUNTRY_TABLE_CAP - 1;
    uint32_t slot = h & mask;
    while (country_table[slot]) {
        if (strcmp(country_table[slot]->name, name) == 0) return country_table[slot];
        slot = (slot + 1) & mask;
    }
    Country *c = &country_list[country_list_count++];
    memset(c, 0, sizeof(Country));
    c->name = arena_alloc_str(arena, name, len);
    c->amount_min = INT64_MAX;
    c->amount_max = INT64_MIN;
    country_table[slot] = c;
    return c;
}

static int cmp_countries(const void *a, const void *b) {
    return strcmp(((const Country *)a)->name, ((const Country *)b)->name);
}

static inline int64_t parse_int64(const char *s) {
    int64_t v = 0;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return sign * v;
}

static inline int64_t parse_cents(const char *s) {
    int64_t sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int64_t whole = 0;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (*s - '0');
        s++;
    }
    if (*s == '.') s++;
    int64_t frac = 0;
    if (*s >= '0' && *s <= '9') { frac += (*s - '0') * 10; s++; }
    if (*s >= '0' && *s <= '9') { frac += (*s - '0'); s++; }
    return sign * (whole * 100 + frac);
}

static void format_cents(char *buf, int64_t cents) {
    char sign = (cents < 0) ? '-' : '\0';
    int64_t c = (cents < 0) ? -cents : cents;
    if (sign) sprintf(buf, "-%lld.%02lld", (long long)(c / 100), (long long)(c % 100));
    else sprintf(buf, "%lld.%02lld", (long long)(c / 100), (long long)(c % 100));
}

static void load_regions(const char *path, Arena *arena) {
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
        insert_region(parse_int64(p), comma + 1, arena);
    }
    fclose(f);
}

static inline void ensure_cap(char **buf, size_t *cap, size_t needed) {
    if (needed > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 256;
        while (new_cap < needed) new_cap *= 2;
        *buf = (char *)realloc(*buf, new_cap);
        *cap = new_cap;
    }
}

int main(int argc, char **argv) {
    const char *data_dir = "/data";
    const char *out_dir = "/out";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    region_table = (RegionEntry *)calloc(REGION_TABLE_CAP, sizeof(RegionEntry));
    country_table = (Country **)calloc(COUNTRY_TABLE_CAP, sizeof(Country *));
    country_list = (Country *)calloc(COUNTRY_TABLE_CAP, sizeof(Country));

    Arena arena = {0};

    char regions_path[1024];
    snprintf(regions_path, sizeof(regions_path), "%s/regions.csv", data_dir);
    load_regions(regions_path, &arena);

    char records_path[1024];
    snprintf(records_path, sizeof(records_path), "%s/records.csv", data_dir);
    int fd = open(records_path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;
    const char *p = (const char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    const char *end = p + file_size;

    // Skip header line
    if (p + 2 < end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (p < end && *p != '\n') p++;
    if (p < end && *p == '\n') p++;

    size_t f_cap[5] = {256, 256, 256, 256, 256};
    char *f_buf[5] = {malloc(256), malloc(256), malloc(256), malloc(256), malloc(256)};
    size_t f_len[5] = {0};

    while (p < end) {
        if (p + 2 < end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        if (p >= end) break;
        if (*p == '\n' || *p == '\r') { p++; continue; }

        for (int f = 0; f < 5; f++) {
            size_t len = 0;
            if (p < end && *p == '"') {
                p++;
                while (p < end) {
                    if (*p == '"') {
                        if (p + 1 < end && *(p + 1) == '"') {
                            ensure_cap(&f_buf[f], &f_cap[f], len + 1);
                            f_buf[f][len++] = '"';
                            p += 2;
                        } else {
                            p++;
                            break;
                        }
                    } else {
                        ensure_cap(&f_buf[f], &f_cap[f], len + 1);
                        f_buf[f][len++] = *p++;
                    }
                }
                if (p < end && *p == '\r') p++;
                if (p < end && (*p == ',' || *p == '\n')) p++;
            } else {
                const char *start = p;
                if (f == 4) {
                    while (p < end && *p != '\n' && *p != '\r') p++;
                } else {
                    while (p < end && *p != ',') p++;
                }
                len = p - start;
                ensure_cap(&f_buf[f], &f_cap[f], len + 1);
                memcpy(f_buf[f], start, len);
                if (p < end && *p == '\r') p++;
                if (p < end && (*p == ',' || *p == '\n')) p++;
            }
            f_buf[f][len] = '\0';
            f_len[f] = len;
        }

        int64_t id = parse_int64(f_buf[0]);
        int64_t region_id = parse_int64(f_buf[3]);
        int64_t cents = parse_cents(f_buf[4]);
        const char *country_name = lookup_region(region_id);

        Country *c = get_or_create_country(country_name, &arena);
        c->count++;
        c->id_sum += id;
        c->amount_sum += cents;
        if (cents < c->amount_min) c->amount_min = cents;
        if (cents > c->amount_max) c->amount_max = cents;
        c->name_bytes += f_len[1];
        c->note_bytes += f_len[2];
        country_add_name(c, f_buf[1], f_len[1], &arena);
    }

    munmap((void *)p, file_size);
    close(fd);

    qsort(country_list, country_list_count, sizeof(Country), cmp_countries);

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);
    FILE *out = fopen(out_path, "wb");
    if (!out) return 1;

    char min_s[32], max_s[32], sum_s[32];
    for (int i = 0; i < country_list_count; i++) {
        Country *c = &country_list[i];
        format_cents(sum_s, c->amount_sum);
        format_cents(min_s, c->amount_min);
        format_cents(max_s, c->amount_max);
        fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
                c->name, (long long)c->count, (long long)c->id_sum,
                sum_s, min_s, max_s, c->names_count,
                (unsigned long long)c->name_bytes, (unsigned long long)c->note_bytes);
    }
    fclose(out);
    return 0;
}