#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_COUNTRIES 512
#define MAX_THREADS 16

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
  uint64_t *names_table;
} CountryAgg;

typedef struct {
  size_t start_off;
  size_t end_off;
} ChunkRange;

typedef struct {
  int fd;
  size_t start_off;
  size_t end_off;
  CountryAgg aggs[MAX_COUNTRIES];
} Worker;

static uint16_t *region_map = NULL;
static uint32_t region_max = 0;
static char country_names[MAX_COUNTRIES][64];
static int total_countries = 1; // 0 = _UNMATCHED

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

static void load_regions(const char *path) {
  strcpy(country_names[0], "_UNMATCHED");
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return;
  }
  size_t sz = (size_t)st.st_size;
  char *buf = (char *)malloc(sz + 1);
  if (!buf) {
    close(fd);
    return;
  }
  ssize_t rd = pread(fd, buf, sz, 0);
  close(fd);
  if (rd <= 0) {
    free(buf);
    return;
  }
  buf[rd] = '\0';

  char *p = buf;
  if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
    p += 3;
  while (*p && *p != '\n')
    p++;
  if (*p == '\n')
    p++;

  uint32_t max_id = 0;
  char *scan = p;
  while (*scan) {
    uint32_t id = 0;
    while (*scan >= '0' && *scan <= '9')
      id = id * 10 + (*scan++ - '0');
    if (id > max_id)
      max_id = id;
    while (*scan && *scan != '\n')
      scan++;
    if (*scan == '\n')
      scan++;
  }

  region_max = max_id;
  region_map = (uint16_t *)calloc((size_t)max_id + 1, sizeof(uint16_t));
  if (!region_map) {
    free(buf);
    return;
  }

  while (*p) {
    uint32_t reg_id = 0;
    while (*p >= '0' && *p <= '9')
      reg_id = reg_id * 10 + (*p++ - '0');
    if (*p == ',')
      p++;
    char *c_start = p;
    while (*p && *p != '\n' && *p != '\r')
      p++;
    size_t c_len = p - c_start;
    while (*p == '\r' || *p == '\n')
      p++;
    if (c_len > 0) {
      uint16_t c_idx = (uint16_t)get_country_idx(c_start, c_len);
      if (reg_id <= region_max)
        region_map[reg_id] = c_idx;
    }
  }
  free(buf);
}

static inline void add_name_hash(CountryAgg *agg, uint64_t hash) {
  if (hash == 0)
    hash = 1;
  if (__builtin_expect(agg->names_cap == 0, 0)) {
    agg->names_cap = 256;
    agg->names_table = (uint64_t *)calloc(agg->names_cap, sizeof(uint64_t));
  } else if (__builtin_expect((agg->names_count * 2) >= agg->names_cap, 0)) {
    uint32_t old_cap = agg->names_cap;
    uint32_t new_cap = old_cap * 2;
    uint64_t *new_tbl = (uint64_t *)calloc(new_cap, sizeof(uint64_t));
    uint32_t mask = new_cap - 1;
    for (uint32_t i = 0; i < old_cap; i++) {
      if (agg->names_table[i] != 0) {
        uint32_t s = (uint32_t)(agg->names_table[i] & mask);
        while (new_tbl[s] != 0)
          s = (s + 1) & mask;
        new_tbl[s] = agg->names_table[i];
      }
    }
    free(agg->names_table);
    agg->names_table = new_tbl;
    agg->names_cap = new_cap;
  }
  uint32_t mask = agg->names_cap - 1;
  uint32_t s = (uint32_t)(hash & mask);
  while (agg->names_table[s] != 0) {
    if (agg->names_table[s] == hash)
      return;
    s = (s + 1) & mask;
  }
  agg->names_table[s] = hash;
  agg->names_count++;
}

static void *worker_func(void *arg) {
  Worker *w = (Worker *)arg;
  for (int i = 0; i < MAX_COUNTRIES; i++) {
    w->aggs[i].amount_min = INT64_MAX;
    w->aggs[i].amount_max = INT64_MIN;
  }

  size_t buf_cap = 512 * 1024;
  char *buf = (char *)malloc(buf_cap + 65536);
  size_t cur = w->start_off;
  size_t leftover = 0;

  while (cur < w->end_off || leftover > 0) {
    size_t want = buf_cap - leftover;
    if (cur + want > w->end_off)
      want = (w->end_off > cur) ? (w->end_off - cur) : 0;
    ssize_t n = 0;
    if (want > 0) {
      n = pread(w->fd, buf + leftover, want, cur);
      if (n > 0)
        cur += n;
    }
    size_t total = leftover + (n > 0 ? n : 0);
    if (total == 0)
      break;

    char *p = buf;
    char *end = buf + total;

    while (p < end) {
      if (cur < w->end_off) {
        char *nl = (char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl || (size_t)(end - p) < 8192) {
          leftover = end - p;
          memmove(buf, p, leftover);
          break;
        }
      }

      while (p < end && (*p == '\r' || *p == '\n'))
        p++;
      if (p >= end)
        break;

      if ((unsigned char)p[0] == 0xEF && p + 2 < end &&
          (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

      int64_t rec_id = 0;
      while (p < end && *p >= '0' && *p <= '9')
        rec_id = rec_id * 10 + (*p++ - '0');
      if (p < end && *p == ',')
        p++;

      uint64_t name_hash = 0xcbf29ce484222325ULL;
      size_t name_len = 0;
      if (p < end && *p == '"') {
        p++;
        while (p < end) {
          if (*p == '"') {
            if (p + 1 < end && p[1] == '"') {
              name_hash = (name_hash ^ '"') * 0x100000001b3ULL;
              name_len++;
              p += 2;
            } else {
              p++;
              break;
            }
          } else {
            name_hash = (name_hash ^ (uint8_t)*p) * 0x100000001b3ULL;
            name_len++;
            p++;
          }
        }
      } else {
        while (p < end && *p != ',') {
          name_hash = (name_hash ^ (uint8_t)*p) * 0x100000001b3ULL;
          name_len++;
          p++;
        }
      }
      if (p < end && *p == ',')
        p++;

      size_t note_len = 0;
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
        while (p < end && *p != ',') {
          note_len++;
          p++;
        }
      }
      if (p < end && *p == ',')
        p++;

      uint32_t reg_id = 0;
      while (p < end && *p >= '0' && *p <= '9')
        reg_id = reg_id * 10 + (*p++ - '0');
      if (p < end && *p == ',')
        p++;

      int neg = 0;
      if (p < end && *p == '-') {
        neg = 1;
        p++;
      }
      int64_t cents = 0;
      while (p < end && *p >= '0' && *p <= '9')
        cents = cents * 10 + (*p++ - '0');
      if (p < end && *p == '.') {
        p++;
        if (p < end && *p >= '0' && *p <= '9')
          cents = cents * 10 + (*p++ - '0');
        if (p < end && *p >= '0' && *p <= '9')
          cents = cents * 10 + (*p++ - '0');
      }
      if (neg)
        cents = -cents;

      while (p < end && *p != '\n')
        p++;
      if (p < end && *p == '\n')
        p++;

      uint16_t c_idx = (reg_id <= region_max) ? region_map[reg_id] : 0;
      CountryAgg *ca = &w->aggs[c_idx];
      ca->count++;
      ca->id_sum += rec_id;
      ca->amount_sum += cents;
      if (cents < ca->amount_min)
        ca->amount_min = cents;
      if (cents > ca->amount_max)
        ca->amount_max = cents;
      ca->name_bytes += name_len;
      ca->note_bytes += note_len;
      add_name_hash(ca, name_hash);
    }
    if (cur >= w->end_off)
      leftover = 0;
  }
  free(buf);
  return NULL;
}

static int split_file(int fd, size_t total_size, size_t data_start, int nchunks, ChunkRange *chunks) {
  if (nchunks <= 1 || total_size <= data_start) {
    chunks[0].start_off = data_start;
    chunks[0].end_off = total_size;
    return 1;
  }

  size_t body = total_size - data_start;
  size_t stride = body / nchunks;
  size_t target = data_start + stride;
  int t = 1;
  chunks[0].start_off = data_start;

  size_t cur = data_start;
  int in_quote = 0;
  char buf[256 * 1024];

  while (cur < total_size && t < nchunks) {
    size_t want = sizeof(buf);
    if (cur + want > total_size)
      want = total_size - cur;
    ssize_t n = pread(fd, buf, want, cur);
    if (n <= 0)
      break;

    for (ssize_t i = 0; i < n; i++) {
      char c = buf[i];
      if (c == '"') {
        in_quote ^= 1;
      } else if (c == '\n' && !in_quote) {
        size_t file_pos = cur + i + 1;
        if (file_pos >= target) {
          chunks[t - 1].end_off = file_pos;
          chunks[t].start_off = file_pos;
          t++;
          target = data_start + (size_t)t * stride;
          if (t >= nchunks)
            break;
        }
      }
    }
    cur += n;
  }
  chunks[t - 1].end_off = total_size;
  return t;
}

static int cmp_country(const void *a, const void *b) {
  return strcmp(country_names[*(const int *)a], country_names[*(const int *)b]);
}

static void format_cents(char *buf, int64_t cents) {
  const char *sign = cents < 0 ? "-" : "";
  int64_t c = llabs(cents);
  sprintf(buf, "%s%lld.%02lld", sign, (long long)(c / 100), (long long)(c % 100));
}

int main(int argc, char **argv) {
  const char *data_dir = "/data", *out_dir = "/out";
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--data") && i + 1 < argc)
      data_dir = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc)
      out_dir = argv[++i];
  }

  char path[1024];
  snprintf(path, sizeof(path), "%s/regions.csv", data_dir);
  load_regions(path);

  snprintf(path, sizeof(path), "%s/records.csv", data_dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 1;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return 1;
  }
  size_t total_size = (size_t)st.st_size;

  char hbuf[1024];
  ssize_t hn = pread(fd, hbuf, sizeof(hbuf), 0);
  size_t data_start = 0;
  for (ssize_t i = 0; i < hn; i++) {
    if (hbuf[i] == '\n') {
      data_start = i + 1;
      break;
    }
  }

  int nthreads = 12;
  cpu_set_t cpuset;
  if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0) {
    int count = CPU_COUNT(&cpuset);
    if (count > 0 && count < nthreads)
      nthreads = count;
  }

  ChunkRange chunks[MAX_THREADS];
  int actual_chunks = split_file(fd, total_size, data_start, nthreads, chunks);

  Worker *workers = (Worker *)calloc((size_t)MAX_THREADS, sizeof(Worker));
  if (!workers)
    return 1;

  pthread_t threads[MAX_THREADS];

  for (int i = 0; i < actual_chunks; i++) {
    workers[i].fd = fd;
    workers[i].start_off = chunks[i].start_off;
    workers[i].end_off = chunks[i].end_off;
    pthread_create(&threads[i], NULL, worker_func, &workers[i]);
  }
  for (int i = 0; i < actual_chunks; i++) {
    pthread_join(threads[i], NULL);
  }
  close(fd);

  CountryAgg *final_aggs = (CountryAgg *)calloc((size_t)MAX_COUNTRIES, sizeof(CountryAgg));
  if (!final_aggs)
    return 1;

  for (int c = 0; c < total_countries; c++) {
    final_aggs[c].amount_min = INT64_MAX;
    final_aggs[c].amount_max = INT64_MIN;
    for (int t = 0; t < actual_chunks; t++) {
      CountryAgg *src = &workers[t].aggs[c];
      if (src->count == 0)
        continue;
      final_aggs[c].count += src->count;
      final_aggs[c].id_sum += src->id_sum;
      final_aggs[c].amount_sum += src->amount_sum;
      if (src->amount_min < final_aggs[c].amount_min)
        final_aggs[c].amount_min = src->amount_min;
      if (src->amount_max > final_aggs[c].amount_max)
        final_aggs[c].amount_max = src->amount_max;
      final_aggs[c].name_bytes += src->name_bytes;
      final_aggs[c].note_bytes += src->note_bytes;

      if (src->names_table) {
        for (uint32_t s = 0; s < src->names_cap; s++) {
          if (src->names_table[s] != 0)
            add_name_hash(&final_aggs[c], src->names_table[s]);
        }
        free(src->names_table);
      }
    }
  }

  int valid[MAX_COUNTRIES], valid_cnt = 0;
  for (int i = 0; i < total_countries; i++) {
    if (final_aggs[i].count > 0)
      valid[valid_cnt++] = i;
  }
  qsort(valid, (size_t)valid_cnt, sizeof(int), cmp_country);

  mkdir(out_dir, 0777);
  char out_file[1024];
  snprintf(out_file, sizeof(out_file), "%s/aggregates.txt", out_dir);
  FILE *out = fopen(out_file, "wb");
  if (!out)
    return 1;
  setvbuf(out, NULL, _IOFBF, 256 * 1024);

  char s_sum[32], s_min[32], s_max[32];
  for (int i = 0; i < valid_cnt; i++) {
    int ci = valid[i];
    CountryAgg *ca = &final_aggs[ci];
    format_cents(s_sum, ca->amount_sum);
    format_cents(s_min, ca->amount_min);
    format_cents(s_max, ca->amount_max);
    fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
            country_names[ci], (long long)ca->count, (long long)ca->id_sum,
            s_sum, s_min, s_max,
            ca->names_count, (unsigned long long)ca->name_bytes,
            (unsigned long long)ca->note_bytes);
    if (ca->names_table)
      free(ca->names_table);
  }
  fclose(out);
  free(workers);
  free(final_aggs);
  if (region_map)
    free(region_map);
  return 0;
}