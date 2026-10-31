#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_COUNTRIES 1024
#define MAX_THREADS 32
#define CHUNK_NOMINAL_SIZE (1024 * 1024) // 1 MB chunks keep RSS minimal
#define MAX_CHUNKS 4096

#define REGION_CAP 1048576
#define REGION_MASK (REGION_CAP - 1)

typedef struct {
  uint64_t region_id;
  uint16_t country_idx;
  uint8_t occupied;
} RegionEntry;

static RegionEntry *region_table = NULL;

typedef struct {
  uint64_t *slots;
  uint32_t cap;
  uint32_t count;
} Hash64Set;

typedef struct {
  int64_t count;
  int64_t id_sum;
  int64_t amount_sum;
  int64_t amount_min;
  int64_t amount_max;
  uint64_t name_bytes;
  uint64_t note_bytes;
  Hash64Set names;
} LocalCountryAgg;

typedef struct {
  int64_t count;
  int64_t id_sum;
  int64_t amount_sum;
  int64_t amount_min;
  int64_t amount_max;
  uint64_t name_bytes;
  uint64_t note_bytes;
  uint32_t names_count;
} FinalCountryAgg;

static char country_names[MAX_COUNTRIES][64];
static int total_countries = 1; // 0 is _UNMATCHED

static LocalCountryAgg thread_aggs[MAX_THREADS][MAX_COUNTRIES];
static FinalCountryAgg final_aggs[MAX_COUNTRIES];

static int g_records_fd = -1;
static size_t g_file_size = 0;
static int g_num_chunks = 0;
static size_t g_chunk_boundaries[MAX_CHUNKS + 1];
static int g_num_threads = 1;
static atomic_int g_work_counter = 0;

static inline void hash64_grow(Hash64Set *s) {
  uint32_t old_cap = s->cap;
  uint32_t new_cap = old_cap ? (old_cap * 2) : 64;
  uint64_t *new_slots = (uint64_t *)calloc(new_cap, sizeof(uint64_t));
  uint32_t mask = new_cap - 1;
  if (s->slots) {
    for (uint32_t i = 0; i < old_cap; i++) {
      uint64_t v = s->slots[i];
      if (v != 0) {
        uint32_t slot = (uint32_t)(v ^ (v >> 32)) & mask;
        while (new_slots[slot] != 0) slot = (slot + 1) & mask;
        new_slots[slot] = v;
      }
    }
    free(s->slots);
  }
  s->slots = new_slots;
  s->cap = new_cap;
}

static inline void hash64_insert(Hash64Set *s, uint64_t v) {
  if (__builtin_expect((s->count * 2) >= s->cap, 0)) {
    hash64_grow(s);
  }
  uint32_t mask = s->cap - 1;
  uint32_t slot = (uint32_t)(v ^ (v >> 32)) & mask;
  while (s->slots[slot] != 0) {
    if (s->slots[slot] == v) return;
    slot = (slot + 1) & mask;
  }
  s->slots[slot] = v;
  s->count++;
}

static int get_country_idx(const char *name, size_t len) {
  for (int i = 1; i < total_countries; i++) {
    if (strncmp(country_names[i], name, len) == 0 && country_names[i][len] == '\0')
      return i;
  }
  if (total_countries >= MAX_COUNTRIES) return 0;
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
  return 0; // _UNMATCHED
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
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { close(fd); return; }
  ssize_t rd = pread(fd, buf, sz, 0);
  close(fd);
  if (rd <= 0) { free(buf); return; }

  const char *p = buf;
  const char *end = buf + rd;

  if (rd >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
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
  free(buf);
}

static void parse_chunk(int tid, const char *buf, size_t len) {
  const char *p = buf;
  const char *end = buf + len;

  while (p < end) {
    while (p < end && (*p == '\r' || *p == '\n')) p++;
    if (p >= end) break;
    if (p + 3 <= end && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
      p += 3;

    // Field 1: id
    int64_t rec_id = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      rec_id = rec_id * 10 + (*p - '0');
      p++;
    }
    if (p < end && *p == ',') p++;

    // Field 2: name (hashed on the fly with 64-bit FNV-1a + avalanche)
    uint64_t name_h = 14695981039346656037ULL;
    uint32_t name_len = 0;
    if (p < end && *p == '"') {
      p++;
      while (p < end) {
        if (*p == '"') {
          if (p + 1 < end && p[1] == '"') {
            name_h = (name_h ^ (uint8_t)'"') * 1099511628211ULL;
            name_len++;
            p += 2;
          } else {
            p++;
            break;
          }
        } else {
          name_h = (name_h ^ (uint8_t)*p) * 1099511628211ULL;
          name_len++;
          p++;
        }
      }
    } else {
      while (p < end && *p != ',') {
        name_h = (name_h ^ (uint8_t)*p) * 1099511628211ULL;
        name_len++;
        p++;
      }
    }
    while (p < end && *p != ',') p++;
    if (p < end && *p == ',') p++;

    name_h ^= (uint64_t)name_len * 0x9e3779b97f4a7c15ULL;
    name_h ^= name_h >> 33;
    name_h *= 0xff51afd7ed558ccdULL;
    name_h ^= name_h >> 33;
    if (__builtin_expect(name_h == 0, 0)) name_h = 1;

    // Field 3: note (unescaped length only)
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
    while (p < end && *p >= '0' && *p <= '9') {
      reg_id = reg_id * 10 + (*p - '0');
      p++;
    }
    while (p < end && *p != ',') p++;
    if (p < end && *p == ',') p++;

    // Field 5: amount (cents parsing)
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
    LocalCountryAgg *ca = &thread_aggs[tid][c_idx];
    ca->count++;
    ca->id_sum += rec_id;
    ca->amount_sum += total_cents;
    if (total_cents < ca->amount_min) ca->amount_min = total_cents;
    if (total_cents > ca->amount_max) ca->amount_max = total_cents;
    ca->name_bytes += name_len;
    ca->note_bytes += note_len;
    hash64_insert(&ca->names, name_h);
  }
}

static void *worker_func(void *arg) {
  int tid = *(int *)arg;
  size_t max_buf = CHUNK_NOMINAL_SIZE + 256 * 1024;
  char *buf = (char *)malloc(max_buf);
  if (!buf) return NULL;

  for (;;) {
    int idx = atomic_fetch_add(&g_work_counter, 1);
    if (idx >= g_num_chunks) break;

    size_t start = g_chunk_boundaries[idx];
    size_t end = g_chunk_boundaries[idx + 1];
    if (start >= end) continue;

    size_t len = end - start;
    if (len > max_buf) {
      max_buf = len + 65536;
      buf = (char *)realloc(buf, max_buf);
    }

    ssize_t rd = pread(g_records_fd, buf, len, start);
    if (rd > 0) {
      parse_chunk(tid, buf, (size_t)rd);
    }
  }

  free(buf);
  return NULL;
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

  // Detect CPU cores
  int ncpus = 1;
  cpu_set_t cs;
  CPU_ZERO(&cs);
  if (sched_getaffinity(0, sizeof(cs), &cs) == 0) {
    ncpus = CPU_COUNT(&cs);
  } else {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) ncpus = (int)n;
  }
  if (ncpus > MAX_THREADS) ncpus = MAX_THREADS;
  if (ncpus < 1) ncpus = 1;
  g_num_threads = ncpus;

  // Initialize aggregates
  for (int t = 0; t < MAX_THREADS; t++) {
    for (int i = 0; i < MAX_COUNTRIES; i++) {
      thread_aggs[t][i].amount_min = INT64_MAX;
      thread_aggs[t][i].amount_max = INT64_MIN;
    }
  }

  char path[1024];
  snprintf(path, sizeof(path), "%s/regions.csv", data_dir);
  load_regions(path);

  snprintf(path, sizeof(path), "%s/records.csv", data_dir);
  g_records_fd = open(path, O_RDONLY);
  if (g_records_fd < 0) return 1;
  struct stat st;
  if (fstat(g_records_fd, &st) != 0 || st.st_size <= 0) {
    close(g_records_fd);
    return 1;
  }
  g_file_size = (size_t)st.st_size;

  // Streaming scan to find chunk boundaries without loading file into RSS
  char *scan_buf = (char *)malloc(CHUNK_NOMINAL_SIZE);
  if (!scan_buf) return 1;

  // Locate end of header line
  ssize_t hrd = pread(g_records_fd, scan_buf, 4096, 0);
  size_t p0 = 0;
  if (hrd >= 3 && (unsigned char)scan_buf[0] == 0xEF && (unsigned char)scan_buf[1] == 0xBB && (unsigned char)scan_buf[2] == 0xBF)
    p0 += 3;
  while (p0 < (size_t)hrd && scan_buf[p0] != '\n' && scan_buf[p0] != '\r') p0++;
  while (p0 < (size_t)hrd && (scan_buf[p0] == '\r' || scan_buf[p0] == '\n')) p0++;

  g_chunk_boundaries[0] = p0;
  int chunk_count = 1;
  size_t cur_file_offset = p0;
  size_t next_boundary_target = p0 + CHUNK_NOMINAL_SIZE;
  int in_quote = 0;

  while (cur_file_offset < g_file_size) {
    size_t to_read = (cur_file_offset + CHUNK_NOMINAL_SIZE <= g_file_size) ? CHUNK_NOMINAL_SIZE : (g_file_size - cur_file_offset);
    ssize_t rd = pread(g_records_fd, scan_buf, to_read, cur_file_offset);
    if (rd <= 0) break;

    for (ssize_t i = 0; i < rd; i++) {
      char c = scan_buf[i];
      if (c == '"') {
        in_quote ^= 1;
      } else if (c == '\n' && in_quote == 0) {
        size_t record_end = cur_file_offset + (size_t)i + 1;
        if (record_end >= next_boundary_target && record_end < g_file_size) {
          if (chunk_count < MAX_CHUNKS) {
            g_chunk_boundaries[chunk_count++] = record_end;
            next_boundary_target = record_end + CHUNK_NOMINAL_SIZE;
          }
        }
      }
    }
    cur_file_offset += (size_t)rd;
  }
  g_chunk_boundaries[chunk_count] = g_file_size;
  g_num_chunks = chunk_count;
  free(scan_buf);

  // Launch worker threads to parse chunks in parallel
  pthread_t th[MAX_THREADS];
  int tids[MAX_THREADS];
  for (int i = 0; i < g_num_threads; i++) {
    tids[i] = i;
    pthread_create(&th[i], NULL, worker_func, &tids[i]);
  }
  for (int i = 0; i < g_num_threads; i++) {
    pthread_join(th[i], NULL);
  }
  close(g_records_fd);

  // Merge country aggregates
  for (int ci = 0; ci < total_countries; ci++) {
    FinalCountryAgg *fa = &final_aggs[ci];
    fa->amount_min = INT64_MAX;
    fa->amount_max = INT64_MIN;
    uint32_t est_names = 0;

    for (int t = 0; t < g_num_threads; t++) {
      LocalCountryAgg *la = &thread_aggs[t][ci];
      if (la->count == 0) continue;
      fa->count += la->count;
      fa->id_sum += la->id_sum;
      fa->amount_sum += la->amount_sum;
      if (la->amount_min < fa->amount_min) fa->amount_min = la->amount_min;
      if (la->amount_max > fa->amount_max) fa->amount_max = la->amount_max;
      fa->name_bytes += la->name_bytes;
      fa->note_bytes += la->note_bytes;
      est_names += la->names.count;
    }

    if (fa->count == 0) continue;

    Hash64Set final_set = {NULL, 0, 0};
    uint32_t cap = 64;
    while (cap < est_names * 2) cap *= 2;
    final_set.cap = cap;
    final_set.slots = (uint64_t *)calloc(cap, sizeof(uint64_t));

    for (int t = 0; t < g_num_threads; t++) {
      Hash64Set *s = &thread_aggs[t][ci].names;
      if (s->slots) {
        for (uint32_t i = 0; i < s->cap; i++) {
          if (s->slots[i] != 0) {
            hash64_insert(&final_set, s->slots[i]);
          }
        }
        free(s->slots);
        s->slots = NULL;
      }
    }
    fa->names_count = final_set.count;
    free(final_set.slots);
  }

  // Sort countries alphabetically
  int valid[MAX_COUNTRIES], valid_cnt = 0;
  for (int i = 0; i < total_countries; i++) {
    if (final_aggs[i].count > 0)
      valid[valid_cnt++] = i;
  }
  qsort(valid, (size_t)valid_cnt, sizeof(int), cmp_country);

  // Write aggregates.txt
  mkdir(out_dir, 0777);
  char out_file[1024];
  snprintf(out_file, sizeof(out_file), "%s/aggregates.txt", out_dir);
  FILE *out = fopen(out_file, "wb");
  if (!out) return 1;
  setvbuf(out, NULL, _IOFBF, 256 * 1024);

  char s_sum[32], s_min[32], s_max[32];
  for (int i = 0; i < valid_cnt; i++) {
    int ci = valid[i];
    FinalCountryAgg *ca = &final_aggs[ci];
    format_cents(s_sum, ca->amount_sum);
    format_cents(s_min, ca->amount_min);
    format_cents(s_max, ca->amount_max);
    fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
            country_names[ci], (long long)ca->count, (long long)ca->id_sum,
            s_sum, s_min, s_max,
            ca->names_count, (unsigned long long)ca->name_bytes,
            (unsigned long long)ca->note_bytes);
  }
  fclose(out);
  if (region_table) free(region_table);

  return 0;
}