#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

constexpr int MAX_COUNTRIES = 1024;
constexpr int MAX_THREADS = 16;
constexpr size_t READ_BUF_SZ = 384 * 1024;      // 384 KB buffer per thread
constexpr size_t LOCAL_FILTER_CAP = 2048;       // L1 cache resident
constexpr size_t ARENA_SIZE = 4 * 1024 * 1024;  // 4 MB arena for unique names

struct FilterEntry {
    uint64_t hash;
    uint16_t country_idx;
};

struct GlobalNameTable {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    uint32_t count = 0;
    uint32_t cap = 0;
    uint64_t *slots = nullptr;
};

static uint8_t g_arena_buf[ARENA_SIZE] __attribute__((aligned(64)));
static std::atomic<size_t> g_arena_offset{0};

static inline void *arena_alloc(size_t sz) {
    sz = (sz + 63) & ~63ULL;
    size_t old = g_arena_offset.fetch_add(sz, std::memory_order_relaxed);
    if (__builtin_expect(old + sz > ARENA_SIZE, 0)) return malloc(sz);
    return g_arena_buf + old;
}

static GlobalNameTable g_country_names[MAX_COUNTRIES];

static void insert_global_name(uint16_t c_idx, uint64_t h) {
    auto &g = g_country_names[c_idx];
    while (g.lock.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__)
        _mm_pause();
#endif
    }

    if (__builtin_expect(g.cap == 0, 0)) {
        g.cap = 128;
        g.slots = (uint64_t *)arena_alloc(g.cap * sizeof(uint64_t));
        std::memset(g.slots, 0, g.cap * sizeof(uint64_t));
    }

    uint32_t mask = g.cap - 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & mask;
    while (g.slots[slot] != 0) {
        if (g.slots[slot] == h) {
            g.lock.clear(std::memory_order_release);
            return;
        }
        slot = (slot + 1) & mask;
    }

    if (__builtin_expect((g.count + 1) * 2 >= g.cap, 0)) {
        uint32_t new_cap = g.cap * 2;
        uint64_t *new_slots = (uint64_t *)arena_alloc(new_cap * sizeof(uint64_t));
        std::memset(new_slots, 0, new_cap * sizeof(uint64_t));
        uint32_t new_mask = new_cap - 1;
        for (uint32_t i = 0; i < g.cap; ++i) {
            uint64_t v = g.slots[i];
            if (v != 0) {
                uint32_t s = (uint32_t)(v ^ (v >> 32)) & new_mask;
                while (new_slots[s] != 0) s = (s + 1) & new_mask;
                new_slots[s] = v;
            }
        }
        g.slots = new_slots;
        g.cap = new_cap;
        mask = new_mask;
        slot = (uint32_t)(h ^ (h >> 32)) & mask;
        while (g.slots[slot] != 0) slot = (slot + 1) & mask;
    }

    g.slots[slot] = h;
    g.count++;
    g.lock.clear(std::memory_order_release);
}

static uint16_t *g_region_direct = nullptr;
static uint64_t g_max_region_id = 0;

struct RegionHashEntry {
    uint64_t reg_id = 0;
    uint16_t country_idx = 0;
};
static std::vector<RegionHashEntry> g_region_hash;
static uint32_t g_region_hash_mask = 0;

static std::vector<std::string> g_country_names_list;
static int g_total_countries = 1; // 0 is _UNMATCHED

static inline uint16_t lookup_region(uint64_t reg_id) {
    if (__builtin_expect(g_region_direct != nullptr, 1)) {
        return (reg_id <= g_max_region_id) ? g_region_direct[reg_id] : 0;
    }
    if (g_region_hash_mask != 0) {
        uint32_t slot = (uint32_t)((reg_id * 11400714819323198485ULL) >> 32) & g_region_hash_mask;
        while (g_region_hash[slot].reg_id != 0) {
            if (g_region_hash[slot].reg_id == reg_id)
                return g_region_hash[slot].country_idx;
            slot = (slot + 1) & g_region_hash_mask;
        }
    }
    return 0;
}

static int get_country_idx(const char *name, size_t len) {
    for (int i = 1; i < g_total_countries; ++i) {
        if (g_country_names_list[i].size() == len &&
            memcmp(g_country_names_list[i].data(), name, len) == 0)
            return i;
    }
    if (g_total_countries >= MAX_COUNTRIES) return 0;
    int idx = g_total_countries++;
    g_country_names_list.emplace_back(name, len);
    return idx;
}

static void load_regions(const std::string &path) {
    g_country_names_list.emplace_back("_UNMATCHED");
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }
    size_t sz = (size_t)st.st_size;

    std::vector<char> buf(sz + 1);
    ssize_t r = pread(fd, buf.data(), sz, 0);
    close(fd);
    if (r <= 0) return;

    const char *p = buf.data();
    const char *end = buf.data() + r;

    if (sz >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
        p += 3;
    while (p < end && *p != '\n') ++p;
    if (p < end && *p == '\n') ++p;

    struct Pair { uint64_t reg_id; uint16_t c_idx; };
    std::vector<Pair> pairs;
    pairs.reserve(131072);

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        if (p + 3 <= end && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
            p += 3;

        uint64_t reg_id = 0;
        while (p < end && (uint8_t)(*p - '0') <= 9) {
            reg_id = reg_id * 10 + (*p - '0');
            ++p;
        }
        if (p < end && *p == ',') ++p;

        const char *c_start = p;
        size_t c_len = 0;
        if (p < end && *p == '"') {
            ++p;
            c_start = p;
            while (p < end && *p != '"') ++p;
            c_len = (size_t)(p - c_start);
            if (p < end && *p == '"') ++p;
        } else {
            while (p < end && *p != '\r' && *p != '\n') ++p;
            c_len = (size_t)(p - c_start);
        }
        while (p < end && (*p == '\r' || *p == '\n')) ++p;

        if (c_len > 0) {
            uint16_t c_idx = (uint16_t)get_country_idx(c_start, c_len);
            pairs.push_back({reg_id, c_idx});
            if (reg_id > g_max_region_id) g_max_region_id = reg_id;
        }
    }

    if (g_max_region_id < 2500000) {
        g_region_direct = (uint16_t *)calloc(g_max_region_id + 1, sizeof(uint16_t));
        for (const auto &pr : pairs) {
            g_region_direct[pr.reg_id] = pr.c_idx;
        }
    } else {
        uint32_t cap = 262144;
        while (cap < pairs.size() * 2) cap *= 2;
        g_region_hash.resize(cap);
        g_region_hash_mask = cap - 1;
        for (const auto &pr : pairs) {
            uint32_t slot = (uint32_t)((pr.reg_id * 11400714819323198485ULL) >> 32) & g_region_hash_mask;
            while (g_region_hash[slot].reg_id != 0) {
                if (g_region_hash[slot].reg_id == pr.reg_id) break;
                slot = (slot + 1) & g_region_hash_mask;
            }
            g_region_hash[slot].reg_id = pr.reg_id;
            g_region_hash[slot].country_idx = pr.c_idx;
        }
    }
}

static inline size_t count_quotes_simd(const char *buf, size_t len) {
    size_t count = 0;
    size_t i = 0;
#if defined(__AVX2__)
    __m256i q = _mm256_set1_epi8('"');
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(buf + i));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, q));
        count += __builtin_popcount(mask);
    }
#endif
    for (; i < len; ++i) {
        if (buf[i] == '"') count++;
    }
    return count;
}

struct LocalScalarAgg {
    int64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

static void parse_slice_worker(int fd, size_t start_off, size_t end_off,
                              std::vector<LocalScalarAgg> &local_aggs,
                              char *read_buf) {
    FilterEntry local_filter[LOCAL_FILTER_CAP];
    std::memset(local_filter, 0, sizeof(local_filter));

    size_t cur_file_off = start_off;
    size_t buf_len = 0;
    bool done_reading = false;

    while (cur_file_off < end_off || buf_len > 0) {
        if (!done_reading && buf_len < READ_BUF_SZ) {
            size_t want = std::min(READ_BUF_SZ - buf_len, end_off - cur_file_off);
            if (want > 0) {
                ssize_t got = pread(fd, read_buf + buf_len, want, cur_file_off);
                if (got > 0) {
                    buf_len += got;
                    cur_file_off += got;
                } else {
                    done_reading = true;
                }
            } else {
                done_reading = true;
            }
        }

        const char *p = read_buf;
        const char *limit = read_buf + buf_len;

        while (p < limit) {
            if (!done_reading && (size_t)(limit - p) < 2048) {
                break;
            }

            while (p < limit && (*p == '\r' || *p == '\n')) ++p;
            if (p >= limit) break;

            if (p + 3 <= limit && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
                p += 3;

            // Field 1: id (unbounded fast path)
            int64_t rec_id = 0;
            while ((uint8_t)(*p - '0') <= 9) {
                rec_id = rec_id * 10 + (*p++ - '0');
            }
            if (*p == ',') ++p;

            // Field 2: name
            uint64_t name_h = 14695981039346656037ULL;
            uint32_t name_len = 0;
            if (*p == '"') {
                ++p;
                while (true) {
                    char c = *p++;
                    if (c == '"') {
                        if (*p == '"') {
                            name_h = (name_h ^ (uint8_t)'"') * 1099511628211ULL;
                            name_len++;
                            ++p;
                        } else {
                            break;
                        }
                    } else {
                        name_h = (name_h ^ (uint8_t)c) * 1099511628211ULL;
                        name_len++;
                    }
                }
            } else {
                while (*p != ',') {
                    name_h = (name_h ^ (uint8_t)*p++) * 1099511628211ULL;
                    name_len++;
                }
            }
            if (*p == ',') ++p;

            name_h ^= (uint64_t)name_len * 0x9e3779b97f4a7c15ULL;
            name_h ^= name_h >> 33;
            name_h *= 0xff51afd7ed558ccdULL;
            name_h ^= name_h >> 33;
            if (__builtin_expect(name_h == 0, 0)) name_h = 1;

            // Field 3: note
            uint64_t note_len = 0;
            if (*p == '"') {
                ++p;
                while (true) {
                    const char *qpos = (const char *)std::memchr(p, '"', limit - p);
                    if (!qpos) {
                        note_len += (limit - p);
                        p = limit;
                        break;
                    }
                    note_len += (qpos - p);
                    p = qpos;
                    if (p + 1 < limit && p[1] == '"') {
                        note_len++;
                        p += 2;
                    } else {
                        ++p;
                        break;
                    }
                }
            } else {
                const char *ns = p;
                while (*p != ',') ++p;
                note_len = (size_t)(p - ns);
            }
            if (*p == ',') ++p;

            // Field 4: region_id
            uint64_t reg_id = 0;
            while ((uint8_t)(*p - '0') <= 9) {
                reg_id = reg_id * 10 + (*p++ - '0');
            }
            if (*p == ',') ++p;

            // Field 5: amount
            int neg = 0;
            if (*p == '-') {
                neg = 1;
                ++p;
            }
            int64_t dollars = 0;
            while ((uint8_t)(*p - '0') <= 9) {
                dollars = dollars * 10 + (*p++ - '0');
            }
            int64_t cents = 0;
            if (*p == '.') {
                ++p;
                if ((uint8_t)(*p - '0') <= 9) {
                    cents = (*p++ - '0') * 10;
                    if ((uint8_t)(*p - '0') <= 9) {
                        cents += (*p++ - '0');
                    }
                }
            }
            while (p < limit && *p != '\n') ++p;
            if (p < limit && *p == '\n') ++p;

            int64_t total_cents = dollars * 100 + cents;
            if (neg) total_cents = -total_cents;

            uint16_t c_idx = lookup_region(reg_id);
            auto &ca = local_aggs[c_idx];
            ca.count++;
            ca.id_sum += rec_id;
            ca.amount_sum += total_cents;
            if (total_cents < ca.amount_min) ca.amount_min = total_cents;
            if (total_cents > ca.amount_max) ca.amount_max = total_cents;
            ca.name_bytes += name_len;
            ca.note_bytes += note_len;

            uint32_t f_slot = (uint32_t)(name_h ^ (c_idx * 0x9e3779b97f4a7c15ULL)) & (LOCAL_FILTER_CAP - 1);
            if (local_filter[f_slot].hash != name_h || local_filter[f_slot].country_idx != c_idx) {
                local_filter[f_slot] = {name_h, c_idx};
                insert_global_name(c_idx, name_h);
            }
        }

        size_t leftover = limit - p;
        if (leftover > 0) {
            std::memmove(read_buf, p, leftover);
        }
        buf_len = leftover;
        if (done_reading && buf_len == 0) break;
    }
}

static void format_cents(char *buf, int64_t cents) {
    const char *sign = cents < 0 ? "-" : "";
    int64_t c = std::llabs(cents);
    std::sprintf(buf, "%s%lld.%02lld", sign, (long long)(c / 100), (long long)(c % 100));
}

// Lightweight atomic synchronization barrier
struct SpinBarrier {
    std::atomic<int> count{0};
    std::atomic<int> step{0};
    int total = 0;

    void init(int t) { total = t; count.store(0); step.store(0); }

    void wait() {
        int s = step.load(std::memory_order_relaxed);
        if (count.fetch_add(1, std::memory_order_acq_rel) == total - 1) {
            count.store(0, std::memory_order_relaxed);
            step.fetch_add(1, std::memory_order_release);
        } else {
            while (step.load(std::memory_order_acquire) == s) {
#if defined(__x86_64__)
                _mm_pause();
#endif
            }
        }
    }
};

int main(int argc, char **argv) {
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }

    int num_threads = 1;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    if (sched_getaffinity(0, sizeof(cs), &cs) == 0) {
        num_threads = CPU_COUNT(&cs);
    } else {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n > 0) num_threads = (int)n;
    }
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;
    if (num_threads < 1) num_threads = 1;

    load_regions(data_dir + "/regions.csv");

    std::string rec_path = data_dir + "/records.csv";
    int fd = open(rec_path.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return 1; }
    size_t file_size = (size_t)st.st_size;

    char hbuf[1024];
    ssize_t hr = pread(fd, hbuf, sizeof(hbuf) - 1, 0);
    size_t header_end = 0;
    if (hr > 0) {
        hbuf[hr] = '\0';
        size_t p = 0;
        if (hr >= 3 && (uint8_t)hbuf[0] == 0xEF && (uint8_t)hbuf[1] == 0xBB && (uint8_t)hbuf[2] == 0xBF)
            p = 3;
        while (p < (size_t)hr && hbuf[p] != '\n') ++p;
        if (p < (size_t)hr && hbuf[p] == '\n') ++p;
        header_end = p;
    }

    size_t body_size = file_size - header_end;
    std::vector<size_t> slice_offsets(num_threads + 1);
    for (int i = 0; i <= num_threads; ++i) {
        slice_offsets[i] = header_end + (i * body_size) / num_threads;
    }

    std::vector<size_t> slice_quotes(num_threads, 0);
    std::vector<size_t> chunk_boundaries(num_threads + 1);
    chunk_boundaries[0] = header_end;
    chunk_boundaries[num_threads] = file_size;

    std::vector<std::vector<LocalScalarAgg>> thread_aggs(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        thread_aggs[t].resize(MAX_COUNTRIES);
    }

    SpinBarrier barrier;
    barrier.init(num_threads);

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            char *buf = (char *)malloc(READ_BUF_SZ + 4096);

            // Phase 1: Parallel quote scan in-place
            size_t off = slice_offsets[t];
            size_t end_off = slice_offsets[t + 1];
            size_t qcount = 0;
            while (off < end_off) {
                size_t want = std::min(READ_BUF_SZ, end_off - off);
                ssize_t got = pread(fd, buf, want, off);
                if (got <= 0) break;
                qcount += count_quotes_simd(buf, got);
                off += got;
            }
            slice_quotes[t] = qcount;

            barrier.wait();

            // Thread 0 computes chunk boundaries without spawning new threads
            if (t == 0) {
                int cur_p = 0;
                for (int i = 1; i < num_threads; ++i) {
                    cur_p ^= (slice_quotes[i - 1] & 1);
                    size_t s_off = slice_offsets[i];
                    char scan_buf[65536];
                    ssize_t got = pread(fd, scan_buf, sizeof(scan_buf), s_off);
                    int in_q = cur_p;
                    size_t record_start = s_off;
                    for (ssize_t j = 0; j < got; ++j) {
                        char c = scan_buf[j];
                        if (c == '"') {
                            in_q ^= 1;
                        } else if (c == '\n' && in_q == 0) {
                            ssize_t k = j + 1;
                            while (k < got && (scan_buf[k] == '\r' || scan_buf[k] == '\n')) {
                                k++;
                            }
                            record_start = s_off + k;
                            break;
                        }
                    }
                    chunk_boundaries[i] = record_start;
                }
            }

            barrier.wait();

            // Phase 2: High-speed record parsing using same buffer
            parse_slice_worker(fd, chunk_boundaries[t], chunk_boundaries[t + 1], thread_aggs[t], buf);
            free(buf);
        });
    }

    for (auto &th : workers) th.join();
    close(fd);

    struct FinalAgg {
        int64_t count = 0;
        int64_t id_sum = 0;
        int64_t amount_sum = 0;
        int64_t amount_min = INT64_MAX;
        int64_t amount_max = INT64_MIN;
        uint64_t name_bytes = 0;
        uint64_t note_bytes = 0;
        uint32_t names_count = 0;
    };
    std::vector<FinalAgg> final_aggs(g_total_countries);

    for (int ci = 0; ci < g_total_countries; ++ci) {
        auto &fa = final_aggs[ci];
        for (int t = 0; t < num_threads; ++t) {
            const auto &la = thread_aggs[t][ci];
            if (la.count == 0) continue;
            fa.count += la.count;
            fa.id_sum += la.id_sum;
            fa.amount_sum += la.amount_sum;
            if (la.amount_min < fa.amount_min) fa.amount_min = la.amount_min;
            if (la.amount_max > fa.amount_max) fa.amount_max = la.amount_max;
            fa.name_bytes += la.name_bytes;
            fa.note_bytes += la.note_bytes;
        }
        fa.names_count = g_country_names[ci].count;
    }

    std::vector<int> valid;
    for (int i = 0; i < g_total_countries; ++i) {
        if (final_aggs[i].count > 0) valid.push_back(i);
    }
    std::sort(valid.begin(), valid.end(), [&](int a, int b) {
        return g_country_names_list[a] < g_country_names_list[b];
    });

    mkdir(out_dir.c_str(), 0777);
    std::string out_path = out_dir + "/aggregates.txt";
    FILE *out = std::fopen(out_path.c_str(), "wb");
    if (!out) return 1;
    std::setvbuf(out, nullptr, _IOFBF, 256 * 1024);

    char s_sum[32], s_min[32], s_max[32];
    for (int idx : valid) {
        const auto &ca = final_aggs[idx];
        format_cents(s_sum, ca.amount_sum);
        format_cents(s_min, ca.amount_min);
        format_cents(s_max, ca.amount_max);
        std::fprintf(out, "%s %lld %lld %s %s %s %u %llu %llu\n",
                     g_country_names_list[idx].c_str(),
                     (long long)ca.count, (long long)ca.id_sum,
                     s_sum, s_min, s_max,
                     ca.names_count,
                     (unsigned long long)ca.name_bytes,
                     (unsigned long long)ca.note_bytes);
    }
    std::fclose(out);
    return 0;
}