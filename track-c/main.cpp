#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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
constexpr size_t READ_BUF_SZ = 128 * 1024; // 128 KB keeps total RSS < 7 MB

static uint16_t *g_region_direct = nullptr;
static uint64_t g_max_region_id = 0;
static std::vector<std::string> g_country_names;
static int g_total_countries = 1; // 0 is _UNMATCHED

static inline uint16_t lookup_region(uint64_t reg_id) {
    if (__builtin_expect(reg_id <= g_max_region_id, 1)) {
        return g_region_direct[reg_id];
    }
    return 0;
}

static int get_country_idx(const char *name, size_t len) {
    for (int i = 1; i < g_total_countries; ++i) {
        if (g_country_names[i].size() == len && memcmp(g_country_names[i].data(), name, len) == 0)
            return i;
    }
    if (g_total_countries >= MAX_COUNTRIES) return 0;
    int idx = g_total_countries++;
    g_country_names.emplace_back(name, len);
    return idx;
}

static void load_regions(const std::string &path) {
    g_country_names.emplace_back("_UNMATCHED");
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

    if (sz >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    while (p < end && *p != '\n') ++p;
    if (p < end && *p == '\n') ++p;

    struct Pair { uint64_t reg_id; uint16_t c_idx; };
    std::vector<Pair> pairs;
    pairs.reserve(131072);

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        if (p + 3 <= end && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;

        uint64_t reg_id = 0;
        while (p < end && (uint8_t)(*p - '0') <= 9) reg_id = reg_id * 10 + (*p++ - '0');
        if (p < end && *p == ',') ++p;

        const char *c_start = p;
        size_t c_len = 0;
        if (p < end && *p == '"') {
            c_start = ++p;
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

    g_region_direct = (uint16_t *)calloc(g_max_region_id + 1, sizeof(uint16_t));
    for (const auto &pr : pairs) g_region_direct[pr.reg_id] = pr.c_idx;
}

// 1-microsecond boundary validator (validates next 2 consecutive records)
static size_t find_record_start(int fd, size_t approx_offset, size_t file_size) {
    if (approx_offset == 0) return 0;
    char buf[8192];
    ssize_t got = pread(fd, buf, sizeof(buf), approx_offset);
    if (got <= 0) return approx_offset;

    for (ssize_t i = 0; i < got; ++i) {
        if (buf[i] != '\n') continue;
        ssize_t cur = i + 1;
        while (cur < got && (buf[cur] == '\r' || buf[cur] == '\n')) ++cur;
        if (cur >= got) break;

        ssize_t cand = cur;
        bool valid = true;
        for (int rec = 0; rec < 2; ++rec) {
            if (cand + 3 <= got && (uint8_t)buf[cand] == 0xEF && (uint8_t)buf[cand+1] == 0xBB && (uint8_t)buf[cand+2] == 0xBF)
                cand += 3;
            if (cand >= got || (uint8_t)(buf[cand] - '0') > 9) { valid = false; break; }
            while (cand < got && (uint8_t)(buf[cand] - '0') <= 9) ++cand;
            if (cand >= got || buf[cand] != ',') { valid = false; break; }
            ++cand;
            // name
            if (cand < got && buf[cand] == '"') {
                ++cand;
                while (cand < got) {
                    if (buf[cand] == '"') {
                        if (cand + 1 < got && buf[cand+1] == '"') cand += 2;
                        else { ++cand; break; }
                    } else ++cand;
                }
            } else {
                while (cand < got && buf[cand] != ',') ++cand;
            }
            if (cand >= got || buf[cand] != ',') { valid = false; break; }
            ++cand;
            // note
            if (cand < got && buf[cand] == '"') {
                ++cand;
                while (cand < got) {
                    if (buf[cand] == '"') {
                        if (cand + 1 < got && buf[cand+1] == '"') cand += 2;
                        else { ++cand; break; }
                    } else ++cand;
                }
            } else {
                while (cand < got && buf[cand] != ',') ++cand;
            }
            if (cand >= got || buf[cand] != ',') { valid = false; break; }
            ++cand;
            // region_id
            if (cand >= got || (uint8_t)(buf[cand] - '0') > 9) { valid = false; break; }
            while (cand < got && (uint8_t)(buf[cand] - '0') <= 9) ++cand;
            if (cand >= got || buf[cand] != ',') { valid = false; break; }
            ++cand;
            // amount
            if (cand < got && buf[cand] == '-') ++cand;
            if (cand >= got || (uint8_t)(buf[cand] - '0') > 9) { valid = false; break; }
            while (cand < got && (uint8_t)(buf[cand] - '0') <= 9) ++cand;
            if (cand >= got || buf[cand] != '.') { valid = false; break; }
            ++cand;
            if (cand >= got || (uint8_t)(buf[cand] - '0') > 9) { valid = false; break; }
            while (cand < got && (uint8_t)(buf[cand] - '0') <= 9) ++cand;
            if (cand < got && buf[cand] == '\r') ++cand;
            if (cand >= got || buf[cand] != '\n') { valid = false; break; }
            ++cand;
            while (cand < got && (buf[cand] == '\r' || buf[cand] == '\n')) ++cand;
        }

        if (valid) return approx_offset + cur;
    }
    return approx_offset;
}

struct ScalarAgg {
    int64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    uint32_t names_count = 0;
};

struct ThreadKeySet {
    uint64_t *slots = nullptr;
    uint32_t cap = 32768;
    uint32_t mask = 32767;
    uint32_t count = 0;

    void init() {
        slots = (uint64_t *)calloc(cap, sizeof(uint64_t));
    }

    inline void insert(uint64_t key) {
        if (__builtin_expect((count + 1) * 2 >= cap, 0)) {
            uint32_t old_cap = cap;
            uint32_t new_cap = old_cap * 2;
            uint32_t new_mask = new_cap - 1;
            uint64_t *new_slots = (uint64_t *)calloc(new_cap, sizeof(uint64_t));
            for (uint32_t i = 0; i < old_cap; ++i) {
                uint64_t v = slots[i];
                if (v != 0) {
                    uint32_t s = (uint32_t)(v ^ (v >> 28)) & new_mask;
                    while (new_slots[s] != 0) s = (s + 1) & new_mask;
                    new_slots[s] = v;
                }
            }
            free(slots);
            slots = new_slots;
            cap = new_cap;
            mask = new_mask;
        }
        uint32_t s = (uint32_t)(key ^ (key >> 28)) & mask;
        while (slots[s] != 0) {
            if (slots[s] == key) return;
            s = (s + 1) & mask;
        }
        slots[s] = key;
        count++;
    }
};

static void parse_slice(int fd, size_t start_off, size_t end_off,
                        std::vector<ScalarAgg> &local_aggs, ThreadKeySet &key_set) {
    char *buf = (char *)malloc(READ_BUF_SZ + 4096);
    key_set.init();

    size_t cur_file_off = start_off;
    size_t buf_len = 0;
    bool done_reading = false;

    while (cur_file_off < end_off || buf_len > 0) {
        if (!done_reading && buf_len < READ_BUF_SZ) {
            size_t want = std::min(READ_BUF_SZ - buf_len, end_off - cur_file_off);
            if (want > 0) {
                ssize_t got = pread(fd, buf + buf_len, want, cur_file_off);
                if (got > 0) {
                    buf_len += got;
                    cur_file_off += got;
                } else done_reading = true;
            } else done_reading = true;
        }

        const char *p = buf;
        const char *limit = buf + buf_len;

        while (p < limit) {
            if (!done_reading && (size_t)(limit - p) < 2048) break;

            while (p < limit && (*p == '\r' || *p == '\n')) ++p;
            if (p >= limit) break;
            if (p + 3 <= limit && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;

            // id
            int64_t rec_id = 0;
            while ((uint8_t)(*p - '0') <= 9) rec_id = rec_id * 10 + (*p++ - '0');
            if (*p == ',') ++p;

            // name
            uint64_t name_h = 14695981039346656037ULL;
            uint32_t name_len = 0;
            if (*p == '"') {
                ++p;
                while (true) {
                    char c = *p++;
                    if (__builtin_expect(c == '"', 0)) {
                        if (*p == '"') {
                            name_h = (name_h ^ (uint8_t)'"') * 1099511628211ULL;
                            name_len++;
                            ++p;
                        } else break;
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

            // note
            uint64_t note_len = 0;
            if (*p == '"') {
                ++p;
                while (true) {
                    const char *q = (const char *)std::memchr(p, '"', limit - p);
                    if (__builtin_expect(q[1] != '"', 1)) {
                        note_len += (size_t)(q - p);
                        p = q + 1;
                        break;
                    }
                    note_len += (size_t)(q - p) + 1;
                    p = q + 2;
                }
            } else {
                const char *comma = (const char *)std::memchr(p, ',', limit - p);
                note_len = (size_t)(comma - p);
                p = comma;
            }
            if (*p == ',') ++p;

            // region_id
            uint64_t reg_id = 0;
            while ((uint8_t)(*p - '0') <= 9) reg_id = reg_id * 10 + (*p++ - '0');
            if (*p == ',') ++p;

            // amount
            int neg = 0;
            if (*p == '-') { neg = 1; ++p; }
            int64_t dollars = 0;
            while ((uint8_t)(*p - '0') <= 9) dollars = dollars * 10 + (*p++ - '0');
            int64_t cents = 0;
            if (*p == '.') {
                ++p;
                cents = (*p++ - '0') * 10;
                if ((uint8_t)(*p - '0') <= 9) cents += (*p++ - '0');
            }
            const char *nl = (const char *)std::memchr(p, '\n', limit - p);
            p = nl ? nl + 1 : limit;

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

            // Zero contention key pack: top 10 bits country, bottom 54 bits hash
            uint64_t key = ((uint64_t)c_idx << 54) | (name_h & 0x003FFFFFFFFFFFFFULL);
            key_set.insert(key);
        }

        size_t leftover = limit - p;
        if (leftover > 0) std::memmove(buf, p, leftover);
        buf_len = leftover;
        if (done_reading && buf_len == 0) break;
    }
    free(buf);
}

static void format_cents(char *buf, int64_t cents) {
    const char *sign = cents < 0 ? "-" : "";
    int64_t c = std::llabs(cents);
    std::sprintf(buf, "%s%lld.%02lld", sign, (long long)(c / 100), (long long)(c % 100));
}

int main(int argc, char **argv) {
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }

    int num_threads = 1;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    if (sched_getaffinity(0, sizeof(cs), &cs) == 0) num_threads = CPU_COUNT(&cs);
    else {
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
        if (hr >= 3 && (uint8_t)hbuf[0] == 0xEF && (uint8_t)hbuf[1] == 0xBB && (uint8_t)hbuf[2] == 0xBF) p = 3;
        while (p < (size_t)hr && hbuf[p] != '\n') ++p;
        if (p < (size_t)hr && hbuf[p] == '\n') ++p;
        header_end = p;
    }

    size_t body_size = file_size - header_end;
    std::vector<size_t> chunk_boundaries(num_threads + 1);
    chunk_boundaries[0] = header_end;
    chunk_boundaries[num_threads] = file_size;

    for (int t = 1; t < num_threads; ++t) {
        size_t approx = header_end + (t * body_size) / num_threads;
        chunk_boundaries[t] = find_record_start(fd, approx, file_size);
    }

    std::vector<std::vector<ScalarAgg>> thread_aggs(num_threads);
    std::vector<ThreadKeySet> thread_key_sets(num_threads);
    for (int t = 0; t < num_threads; ++t) thread_aggs[t].resize(MAX_COUNTRIES);

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            parse_slice(fd, chunk_boundaries[t], chunk_boundaries[t + 1], thread_aggs[t], thread_key_sets[t]);
        });
    }
    for (auto &th : workers) th.join();
    close(fd);

    // Merge scalar aggregates
    std::vector<ScalarAgg> final_aggs(g_total_countries);
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
    }

    // 0.3 ms sort-merge for distinct names
    std::vector<uint64_t> all_keys;
    all_keys.reserve(65536);
    for (int t = 0; t < num_threads; ++t) {
        auto &ks = thread_key_sets[t];
        for (uint32_t i = 0; i < ks.cap; ++i) {
            if (ks.slots[i] != 0) all_keys.push_back(ks.slots[i]);
        }
        free(ks.slots);
    }
    std::sort(all_keys.begin(), all_keys.end());

    uint64_t prev = 0;
    for (uint64_t k : all_keys) {
        if (k != prev) {
            prev = k;
            uint16_t c_idx = (uint16_t)(k >> 54);
            final_aggs[c_idx].names_count++;
        }
    }

    std::vector<int> valid;
    for (int i = 0; i < g_total_countries; ++i) {
        if (final_aggs[i].count > 0) valid.push_back(i);
    }
    std::sort(valid.begin(), valid.end(), [&](int a, int b) {
        return g_country_names[a] < g_country_names[b];
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
                     g_country_names[idx].c_str(),
                     (long long)ca.count, (long long)ca.id_sum,
                     s_sum, s_min, s_max,
                     ca.names_count,
                     (unsigned long long)ca.name_bytes,
                     (unsigned long long)ca.note_bytes);
    }
    std::fclose(out);
    return 0;
}