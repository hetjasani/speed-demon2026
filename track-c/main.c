#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

#define MAX_COUNTRIES 1024
#define MAX_THREADS 32
#define MAX_CHUNKS 256
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
} NameTable;

typedef struct {
  int64_t count;
  int64_t id_sum;
  int64_t amount_sum;
  int64_t amount_min;
  int64_t amount_max;
  uint64_t name_bytes;
  uint64_t note_bytes;
  NameTable names;
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

static const char *g_file_base = NULL;
static size_t g_file_size = 0;
static size_t g_chunk_boundaries[MAX_CHUNKS + 1];
static int g_num_chunks = 0;
static int g_num_threads = 1;
static atomic_int g_chunk_counter = 0;

static inline void add_name(NameTable *nt, uint64_t hash) {
  if (__builtin_expect((nt->count * 2) >= nt->cap, 0)) {
    uint32_t old_cap = nt->cap;
    uint32_t new_cap = old_cap ? (old_cap * 2) : 256;
    uint64_t *new_slots = (uint64_t *)calloc(new_cap, sizeof(uint64_t));
    uint32_t new_mask = new_cap - 1;
    if (nt->slots) {
      for (uint32_t i = 0; i < old_cap; i++) {
        uint64_t v = nt->slots[i];
        if (v != 0) {
          uint32_t s = (uint32_t)(v ^ (v >> 32)) & new_mask;
          while (new_slots[s] != 0) s = (s + 1) & new_mask;
          new_slots[s] = v;
        }
      }
      free(nt->slots);
    }
    nt->slots = new_slots;
    nt->cap = new_cap;
  }
  uint32_t mask = nt->cap - 1;
  uint32_t slot = (uint32_t)(hash ^ (hash >> 32)) & mask;
  while (nt->slots[slot] != 0) {
    if (nt->slots[slot] == hash) return;
    slot = (slot + 1) & mask;
  }
  nt->slots[slot] = hash;
  nt->count++;
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
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }
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

static void parse_slice(int tid, const char *p, const char *end) {
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

    // Field 2: name (hashed on the fly with FNV-1a)
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

    // Field 3: note (length only)
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
    LocalCountryAgg *ca = &thread_aggs[tid][c_idx];
    ca->count++;
    ca->id_sum += rec_id;
    ca->amount_sum += total_cents;
    if (total_cents < ca->amount_min) ca->amount_min = total_cents;
    if (total_cents > ca->amount_max) ca->amount_max = total_cents;
    ca->name_bytes += name_len;
    ca->note_bytes += note_len;
    add_name(&ca->names, name_h);
  }
}

static void *worker_thread(void *arg) {
  int tid = *(int *)arg;
  for (;;) {
    int idx = atomic_fetch_add(&g_chunk_counter, 1);
    if (idx >= g_num_chunks) break;

    size_t s = g_chunk_boundaries[idx];
    size_t e = g_chunk_boundaries[idx + 1];
    if (s < e) {
      parse_slice(tid, g_file_base + s, g_file_base + e);
    }
  }
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

  // Detect available CPUs
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
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 1;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 1; }
  g_file_size = (size_t)st.st_size;

  g_file_base = (const char *)mmap(NULL, g_file_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (g_file_base == MAP_FAILED) return 1;

  madvise((void *)g_file_base, g_file_size, MADV_WILLNEED | MADV_SEQUENTIAL);

  // SIMD Quote-Parity Record Boundary Detection (Runs in ~2ms)
  int num_chunks = g_num_threads * 4;
  if (num_chunks > MAX_CHUNKS) num_chunks = MAX_CHUNKS;
  size_t nominal_chunk = g_file_size / num_chunks;
  if (nominal_chunk < 65536) nominal_chunk = 65536;

  const char *p = g_file_base;
  const char *end = g_file_base + g_file_size;

  if (g_file_size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
    p += 3;
  while (p < end && *p != '\n' && *p != '\r') p++;
  while (p < end && (*p == '\r' || *p == '\n')) p++;
  g_chunk_boundaries[0] = (size_t)(p - g_file_base);

  int cur_chunk = 1;
  size_t target_offset = nominal_chunk;
  int in_quote = 0;
  size_t i = g_chunk_boundaries[0];

#if defined(__AVX2__)
  __m256i q_vec = _mm256_set1_epi8('"');
  for (; i + 32 <= g_file_size && cur_chunk < num_chunks; ) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(g_file_base + i));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, q_vec));
    if (mask == 0 && in_quote == 0 && i < target_offset) {
      i += 32;
      continue;
    }
    for (int k = 0; k < 32; k++) {
      char c = g_file_base[i + k];
      if (c == '"') {
        in_quote ^= 1;
      } else if (c == '\n' && in_quote == 0) {
        size_t next_record = i + k + 1;
        while (next_record < g_file_size && (g_file_base[next_record] == '\r' || g_file_base[next_record] == '\n'))
          next_record++;
        if (next_record >= target_offset && cur_chunk < num_chunks) {
          g_chunk_boundaries[cur_chunk++] = next_record;
          target_offset = (size_t)cur_chunk * nominal_chunk;
        }
      }
    }
    i += 32;
  }
#endif

  for (; i < g_file_size && cur_chunk < num_chunks; i++) {
    char c = g_file_base[i];
    if (c == '"') {
      in_quote ^= 1;
    } else if (c == '\n' && in_quote == 0) {
      size_t next_record = i + 1;
      while (next_record < g_file_size && (g_file_base[next_record] == '\r' || g_file_base[next_record] == '\n'))
        next_record++;
      if (next_record >= target_offset && cur_chunk < num_chunks) {
        g_chunk_boundaries[cur_chunk++] = next_record;
        target_offset = (size_t)cur_chunk * nominal_chunk;
      }
    }
  }
  g_chunk_boundaries[cur_chunk] = g_file_size;
  g_num_chunks = cur_chunk;

  // Run multithreaded parse
  pthread_t th[MAX_THREADS];
  int tids[MAX_THREADS];
  for (int t = 0; t < g_num_threads; t++) {
    tids[t] = t;
    pthread_create(&th[t], NULL, worker_thread, &tids[t]);
  }
  for (int t = 0; t < g_num_threads; t++) {
    pthread_join(th[t], NULL);
  }

  munmap((void *)g_file_base, g_file_size);

  // Fast country aggregation
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

    NameTable final_table = {NULL, 0, 0};
    uint32_t cap = 256;
    while (cap < est_names * 2) cap *= 2;
    final_table.cap = cap;
    final_table.slots = (uint64_t *)calloc(cap, sizeof(uint64_t));

    for (int t = 0; t < g_num_threads; t++) {
      NameTable *s = &thread_aggs[t][ci].names;
      if (s->slots) {
        for (uint32_t k = 0; k < s->cap; k++) {
          if (s->slots[k] != 0) {
            add_name(&final_table, s->slots[k]);
          }
        }
        free(s->slots);
        s->slots = NULL;
      }
    }
    fa->names_count = final_table.count;
    free(final_table.slots);
  }

  // Sort and emit output
  int valid[MAX_COUNTRIES], valid_cnt = 0;
  for (int ci = 0; ci < total_countries; ci++) {
    if (final_aggs[ci].count > 0)
      valid[valid_cnt++] = ci;
  }
  qsort(valid, (size_t)valid_cnt, sizeof(int), cmp_country);

  mkdir(out_dir, 0777);
  char out_file[1024];
  snprintf(out_file, sizeof(out_file), "%s/aggregates.txt", out_dir);
  FILE *out = fopen(out_file, "wb");
  if (!out) return 1;
  setvbuf(out, NULL, _IOFBF, 256 * 1024);

  char s_sum[32], s_min[32], s_max[32];
  for (int ci = 0; ci < valid_cnt; ci++) {
    int idx = valid[ci];
    FinalCountryAgg *ca = &final_aggs[idx];
    format_cents(s_sum, ca->amount_sum);
    format_cents(s_min, ca->amount_min);
    format_cents(s_max, ca->amount_max);
    fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
            country_names[idx], (long long)ca->count, (long long)ca->id_sum,
            s_sum, s_min, s_max,
            ca->names_count, (unsigned long long)ca->name_bytes,
            (unsigned long long)ca->note_bytes);
  }
  fclose(out);
  if (region_table) free(region_table);

  return 0;
}