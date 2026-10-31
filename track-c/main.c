#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_COUNTRIES 1024
#define REGION_CAP 1048576
#define REGION_MASK (REGION_CAP - 1)

typedef struct {
  uint64_t region_id;
  uint16_t country_idx;
  uint8_t occupied;
} RegionEntry;

static RegionEntry *region_table = NULL;

typedef struct {
  uint32_t hash;
  uint32_t len_plus_1;
  uint32_t offset;
} NameEntry;

typedef struct {
  int64_t count;
  int64_t id_sum;
  int64_t amount_sum;
  int64_t amount_min;
  int64_t amount_max;
  uint64_t name_bytes;
  uint64_t note_bytes;
  uint32_t names_count;
  uint32_t names_cap;
  NameEntry *names;
} CountryAgg;

static char country_names[MAX_COUNTRIES][64];
static int total_countries = 1; // 0 is _UNMATCHED

static char *name_arena = NULL;
static size_t arena_cap = 0;
static size_t arena_used = 0;
static CountryAgg aggs[MAX_COUNTRIES];

static int get_country_idx(const char *name, size_t len) {
  for (int i = 1; i < total_countries; i++) {
    if (strncmp(country_names[i], name, len) == 0 && country_names[i][len] == '\0')
      return i;
  }
  if (total_countries >= MAX_COUNTRIES)
    return 0;
  int idx = total_countries++;
  memcpy(country_names[idx], name, len);
  country_names[idx][len] = '\0';
  return idx;
}

static inline void insert_region(uint64_t reg_id, uint16_t country_idx) {
  uint32_t slot = (uint32_t)((reg_id * 11400714819323198485ULL) >> 32) & REGION_MASK;
  while (region_table[slot].occupied) {
    if (region_table[slot].region_id == reg_id) {
      region_table[slot].country_idx = country_idx;
      return;
    }
    slot = (slot + 1) & REGION_MASK;
  }
  region_table[slot].region_id = reg_id;
  region_table[slot].country_idx = country_idx;
  region_table[slot].occupied = 1;
}

static inline uint16_t lookup_region(uint64_t reg_id) {
  uint32_t slot = (uint32_t)((reg_id * 11400714819323198485ULL) >> 32) & REGION_MASK;
  while (region_table[slot].occupied) {
    if (region_table[slot].region_id == reg_id)
      return region_table[slot].country_idx;
    slot = (slot + 1) & REGION_MASK;
  }
  return 0;
}

static void load_regions(const char *path) {
  strcpy(country_names[0], "_UNMATCHED");
  region_table = (RegionEntry *)calloc(REGION_CAP, sizeof(RegionEntry));

  int fd = open(path, O_RDONLY);
  if (fd < 0) return;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return;
  }
  size_t sz = (size_t)st.st_size;
  char *buf = (char *)mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (buf == MAP_FAILED) return;

  const char *p = buf;
  const char *end = buf + sz;

  if (sz >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
    p += 3;
  while (p < end && *p != '\n' && *p != '\r') p++;
  while (p < end && (*p == '\r' || *p == '\n')) p++;

  while (p < end) {
    while (p < end && (*p == '\r' || *p == '\n')) p++;
    if (p >= end) break;

    if (p + 3 <= end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
      p += 3;

    uint64_t reg_id = 0;
    while (p < end && *p != ',') {
      if (*p >= '0' && *p <= '9') reg_id = reg_id * 10 + (*p - '0');
      p++;
    }
    if (p < end && *p == ',') p++;

    const char *c_start = p;
    size_t c_len = 0;
    if (p < end && *p == '"') {
      p++;
      c_start = p;
      while (p < end && *p != '"') p++;
      c_len = (size_t)(p - c_start);
      if (p < end && *p == '"') p++;
    } else {
      while (p < end && *p != '\r' && *p != '\n') p++;
      c_len = (size_t)(p - c_start);
    }
    while (p < end && (*p == '\r' || *p == '\n')) p++;

    if (c_len > 0) {
      uint16_t c_idx = (uint16_t)get_country_idx(c_start, c_len);
      insert_region(reg_id, c_idx);
    }
  }
  munmap(buf, sz);
}

static void resize_names_table(CountryAgg *agg) {
  uint32_t old_cap = agg->names_cap;
  uint32_t new_cap = old_cap ? (old_cap * 2) : 512;
  NameEntry *new_names = (NameEntry *)calloc(new_cap, sizeof(NameEntry));
  uint32_t new_mask = new_cap - 1;

  if (agg->names) {
    for (uint32_t i = 0; i < old_cap; i++) {
      if (agg->names[i].len_plus_1 != 0) {
        uint32_t slot = agg->names[i].hash & new_mask;
        while (new_names[slot].len_plus_1 != 0)
          slot = (slot + 1) & new_mask;
        new_names[slot] = agg->names[i];
      }
    }
    free(agg->names);
  }
  agg->names = new_names;
  agg->names_cap = new_cap;
}

static inline void add_name(CountryAgg *agg, const char *str, uint32_t len) {
  if (__builtin_expect((agg->names_count * 2) >= agg->names_cap, 0)) {
    resize_names_table(agg);
  }

  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; i++) {
    h ^= (uint8_t)str[i];
    h *= 16777619u;
  }

  uint32_t mask = agg->names_cap - 1;
  uint32_t slot = h & mask;
  uint32_t target_len_plus_1 = len + 1;

  while (agg->names[slot].len_plus_1 != 0) {
    if (agg->names[slot].len_plus_1 == target_len_plus_1 &&
        agg->names[slot].hash == h &&
        memcmp(name_arena + agg->names[slot].offset, str, len) == 0) {
      return;
    }
    slot = (slot + 1) & mask;
  }

  if (arena_used + len > arena_cap) {
    size_t new_cap = arena_cap ? (arena_cap * 2) : (32 * 1024 * 1024);
    while (arena_used + len > new_cap) new_cap *= 2;
    name_arena = (char *)realloc(name_arena, new_cap);
    arena_cap = new_cap;
  }

  memcpy(name_arena + arena_used, str, len);
  agg->names[slot].hash = h;
  agg->names[slot].offset = (uint32_t)arena_used;
  agg->names[slot].len_plus_1 = target_len_plus_1;
  arena_used += len;
  agg->names_count++;
}

static void format_cents(char *buf, int64_t cents) {
  const char *sign = cents < 0 ? "-" : "";
  int64_t c = llabs(cents);
  sprintf(buf, "%s%lld.%02lld", sign, (long long)(c / 100), (long long)(c % 100));
}

static int cmp_country(const void *a, const void *b) {
  return strcmp(country_names[*(const int *)a], country_names[*(const int *)b]);
}

int main(int argc, char **argv) {
  const char *data_dir = "/data", *out_dir = "/out";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data") && i + 1 < argc)
      data_dir = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc)
      out_dir = argv[++i];
  }

  arena_cap = 32 * 1024 * 1024;
  name_arena = (char *)malloc(arena_cap);

  for (int i = 0; i < MAX_COUNTRIES; i++) {
    aggs[i].amount_min = INT64_MAX;
    aggs[i].amount_max = INT64_MIN;
  }

  char path[1024];
  snprintf(path, sizeof(path), "%s/regions.csv", data_dir);
  load_regions(path);

  snprintf(path, sizeof(path), "%s/records.csv", data_dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 1;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return 1;
  }
  size_t total_size = (size_t)st.st_size;
  char *buf = (char *)mmap(NULL, total_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (buf == MAP_FAILED) return 1;

  madvise(buf, total_size, MADV_SEQUENTIAL);

  const char *p = buf;
  const char *end = buf + total_size;

  if (total_size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
    p += 3;
  while (p < end && *p != '\n' && *p != '\r') p++;
  while (p < end && (*p == '\r' || *p == '\n')) p++;

  char name_scratch[65536];

  while (p < end) {
    while (p < end && (*p == '\r' || *p == '\n')) p++;
    if (p >= end) break;

    if (p + 3 <= end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
      p += 3;

    // Field 1: id
    int64_t rec_id = 0;
    while (p < end && *p != ',') {
      if (*p >= '0' && *p <= '9') rec_id = rec_id * 10 + (*p - '0');
      p++;
    }
    if (p < end && *p == ',') p++;

    // Field 2: name
    uint32_t name_len = 0;
    if (p < end && *p == '"') {
      p++;
      while (p < end) {
        if (*p == '"') {
          if (p + 1 < end && p[1] == '"') {
            if (name_len < sizeof(name_scratch)) name_scratch[name_len++] = '"';
            p += 2;
          } else {
            p++;
            break;
          }
        } else {
          if (name_len < sizeof(name_scratch)) name_scratch[name_len++] = *p;
          p++;
        }
      }
    } else {
      while (p < end && *p != ',') {
        if (name_len < sizeof(name_scratch)) name_scratch[name_len++] = *p;
        p++;
      }
    }
    while (p < end && *p != ',') p++;
    if (p < end && *p == ',') p++;

    // Field 3: note
    uint64_t note_len = 0;
    if (p < end && *p == '"') {
      p++;
      while (p < end) {
        if (*p == '"') {
          if (p + 1 < end && p[1] == '"') {
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
    } else {
      const char *ns = p;
      while (p < end && *p != ',') p++;
      note_len = (size_t)(p - ns);
    }
    while (p < end && *p != ',') p++;
    if (p < end && *p == ',') p++;

    // Field 4: region_id
    uint64_t reg_id = 0;
    while (p < end && *p != ',') {
      if (*p >= '0' && *p <= '9') reg_id = reg_id * 10 + (*p - '0');
      p++;
    }
    if (p < end && *p == ',') p++;

    // Field 5: amount
    int neg = 0;
    int64_t dollars = 0;
    int64_t cents = 0;
    while (p < end && *p != '\n' && *p != '\r') {
      if (*p == '-') {
        neg = 1;
      } else if (*p >= '0' && *p <= '9') {
        dollars = dollars * 10 + (*p - '0');
      } else if (*p == '.') {
        p++;
        int d = 0;
        while (p < end && *p >= '0' && *p <= '9' && d < 2) {
          cents = cents * 10 + (*p - '0');
          d++;
          p++;
        }
        if (d == 1) cents *= 10;
        while (p < end && *p != '\n' && *p != '\r') p++;
        break;
      }
      p++;
    }
    while (p < end && (*p == '\r' || *p == '\n')) p++;

    int64_t total_cents = dollars * 100 + cents;
    if (neg) total_cents = -total_cents;

    uint16_t c_idx = lookup_region(reg_id);
    CountryAgg *ca = &aggs[c_idx];
    ca->count++;
    ca->id_sum += rec_id;
    ca->amount_sum += total_cents;
    if (total_cents < ca->amount_min) ca->amount_min = total_cents;
    if (total_cents > ca->amount_max) ca->amount_max = total_cents;
    ca->name_bytes += name_len;
    ca->note_bytes += note_len;
    add_name(ca, name_scratch, name_len);
  }

  munmap(buf, total_size);

  int valid[MAX_COUNTRIES], valid_cnt = 0;
  for (int i = 0; i < total_countries; i++) {
    if (aggs[i].count > 0)
      valid[valid_cnt++] = i;
  }
  qsort(valid, (size_t)valid_cnt, sizeof(int), cmp_country);

  mkdir(out_dir, 0777);
  char out_file[1024];
  snprintf(out_file, sizeof(out_file), "%s/aggregates.txt", out_dir);
  FILE *out = fopen(out_file, "wb");
  if (!out) return 1;
  setvbuf(out, NULL, _IOFBF, 256 * 1024);

  char s_sum[32], s_min[32], s_max[32];
  for (int i = 0; i < valid_cnt; i++) {
    int ci = valid[i];
    CountryAgg *ca = &aggs[ci];
    format_cents(s_sum, ca->amount_sum);
    format_cents(s_min, ca->amount_min);
    format_cents(s_max, ca->amount_max);
    fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
            country_names[ci], (long long)ca->count, (long long)ca->id_sum,
            s_sum, s_min, s_max,
            ca->names_count, (unsigned long long)ca->name_bytes,
            (unsigned long long)ca->note_bytes);
    if (ca->names) free(ca->names);
  }
  fclose(out);
  free(name_arena);
  if (region_table) free(region_table);
  return 0;
}