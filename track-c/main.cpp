#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
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
constexpr int MAX_THREADS = 32;
constexpr uint32_t REGION_CAP = 1048576;
constexpr uint32_t REGION_MASK = REGION_CAP - 1;

struct RegionEntry {
    uint64_t region_id = 0;
    uint16_t country_idx = 0;
    uint8_t occupied = 0;
};

static std::vector<RegionEntry> region_table(REGION_CAP);
static std::vector<std::string> country_names;
static int total_countries = 1; // 0 is _UNMATCHED

struct LocalCountryAgg {
    int64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    std::vector<uint64_t> name_hashes;
};

struct FinalCountryAgg {
    int64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = INT64_MAX;
    int64_t amount_max = INT64_MIN;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    uint32_t names_count = 0;
};

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

static int get_country_idx(const char *name, size_t len) {
    for (int i = 1; i < total_countries; ++i) {
        if (country_names[i].size() == len && memcmp(country_names[i].data(), name, len) == 0)
            return i;
    }
    if (total_countries >= MAX_COUNTRIES) return 0;
    int idx = total_countries++;
    country_names.emplace_back(name, len);
    return idx;
}

static void load_regions(const std::string &path) {
    country_names.emplace_back("_UNMATCHED");
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return; }
    size_t sz = (size_t)st.st_size;
    char *buf = (char *)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) return;

    const char *p = buf;
    const char *end = buf + sz;

    if (sz >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
        p += 3;
    while (p < end && *p != '\n' && *p != '\r') ++p;
    while (p < end && (*p == '\r' || *p == '\n')) ++p;

    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        if (p + 3 <= end && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
            p += 3;

        uint64_t reg_id = 0;
        while (p < end && *p != ',') {
            if (*p >= '0' && *p <= '9') reg_id = reg_id * 10 + (*p - '0');
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
            insert_region(reg_id, c_idx);
        }
    }
    munmap(buf, sz);
}

static std::vector<size_t> find_chunk_boundaries(const char *base, size_t size, int num_threads) {
    std::vector<size_t> boundaries(num_threads + 1);
    size_t p = 0;
    if (size >= 3 && (uint8_t)base[0] == 0xEF && (uint8_t)base[1] == 0xBB && (uint8_t)base[2] == 0xBF) p += 3;
    while (p < size && base[p] != '\n' && base[p] != '\r') ++p;
    while (p < size && (base[p] == '\r' || base[p] == '\n')) ++p;
    boundaries[0] = p;

    size_t nominal_chunk = size / num_threads;
    int cur_chunk = 1;
    size_t target = nominal_chunk;
    int in_quote = 0;
    size_t i = p;

#if defined(__AVX2__)
    __m256i q_vec = _mm256_set1_epi8('"');
    while (i + 32 <= size && cur_chunk < num_threads) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(base + i));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, q_vec));
        if (mask == 0 && in_quote == 0 && i < target) {
            i += 32;
            continue;
        }
        for (int k = 0; k < 32; ++k) {
            char c = base[i + k];
            if (c == '"') {
                in_quote ^= 1;
            } else if (c == '\n' && in_quote == 0) {
                size_t next_record = i + k + 1;
                while (next_record < size && (base[next_record] == '\r' || base[next_record] == '\n'))
                    next_record++;
                if (next_record >= target && cur_chunk < num_threads) {
                    boundaries[cur_chunk++] = next_record;
                    target = (size_t)cur_chunk * nominal_chunk;
                }
            }
        }
        i += 32;
    }
#endif

    while (i < size && cur_chunk < num_threads) {
        char c = base[i];
        if (c == '"') {
            in_quote ^= 1;
        } else if (c == '\n' && in_quote == 0) {
            size_t next_record = i + 1;
            while (next_record < size && (base[next_record] == '\r' || base[next_record] == '\n'))
                next_record++;
            if (next_record >= target && cur_chunk < num_threads) {
                boundaries[cur_chunk++] = next_record;
                target = (size_t)cur_chunk * nominal_chunk;
            }
        }
        ++i;
    }
    boundaries[cur_chunk] = size;
    return boundaries;
}

static void parse_slice(const char *p, const char *end, std::vector<LocalCountryAgg> &local_aggs) {
    while (p < end) {
        while (p < end && (*p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        if (p + 3 <= end && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
            p += 3;

        // Field 1: id
        int64_t rec_id = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            rec_id = rec_id * 10 + (*p - '0');
            ++p;
        }
        if (p < end && *p == ',') ++p;

        // Field 2: name (hashed on the fly with FNV-1a + Murmur avalanche)
        uint64_t name_h = 14695981039346656037ULL;
        uint32_t name_len = 0;
        if (p < end && *p == '"') {
            ++p;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && p[1] == '"') {
                        name_h = (name_h ^ (uint8_t)'"') * 1099511628211ULL;
                        name_len++;
                        p += 2;
                    } else {
                        ++p;
                        break;
                    }
                } else {
                    name_h = (name_h ^ (uint8_t)*p) * 1099511628211ULL;
                    name_len++;
                    ++p;
                }
            }
        } else {
            while (p < end && *p != ',') {
                name_h = (name_h ^ (uint8_t)*p) * 1099511628211ULL;
                name_len++;
                ++p;
            }
        }
        while (p < end && *p != ',') ++p;
        if (p < end && *p == ',') ++p;

        name_h ^= (uint64_t)name_len * 0x9e3779b97f4a7c15ULL;
        name_h ^= name_h >> 33;
        name_h *= 0xff51afd7ed558ccdULL;
        name_h ^= name_h >> 33;

        // Field 3: note (unescaped length only)
        uint64_t note_len = 0;
        if (p < end && *p == '"') {
            ++p;
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && p[1] == '"') {
                        note_len++;
                        p += 2;
                    } else {
                        ++p;
                        break;
                    }
                } else {
                    note_len++;
                    ++p;
                }
            }
        } else {
            const char *ns = p;
            while (p < end && *p != ',') ++p;
            note_len = (size_t)(p - ns);
        }
        while (p < end && *p != ',') ++p;
        if (p < end && *p == ',') ++p;

        // Field 4: region_id
        uint64_t reg_id = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            reg_id = reg_id * 10 + (*p - '0');
            ++p;
        }
        while (p < end && *p != ',') ++p;
        if (p < end && *p == ',') ++p;

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
                ++p;
                int d = 0;
                while (p < end && *p >= '0' && *p <= '9' && d < 2) {
                    cents = cents * 10 + (*p - '0');
                    d++;
                    ++p;
                }
                if (d == 1) cents *= 10;
                while (p < end && *p != '\n' && *p != '\r') ++p;
                break;
            }
            ++p;
        }
        while (p < end && (*p == '\r' || *p == '\n')) ++p;

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
        ca.name_hashes.push_back(name_h);
    }
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

    const char *file_base = (const char *)mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file_base == MAP_FAILED) return 1;

    madvise((void *)file_base, file_size, MADV_WILLNEED | MADV_SEQUENTIAL);

    auto boundaries = find_chunk_boundaries(file_base, file_size, num_threads);

    std::vector<std::vector<LocalCountryAgg>> all_thread_aggs(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        all_thread_aggs[t].resize(MAX_COUNTRIES);
    }

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, t]() {
            parse_slice(file_base + boundaries[t], file_base + boundaries[t + 1], all_thread_aggs[t]);
        });
    }
    for (auto &th : workers) th.join();
    munmap((void *)file_base, file_size);

    std::vector<FinalCountryAgg> final_aggs(total_countries);
    std::vector<std::thread> merge_threads;
    std::atomic<int> country_cursor{0};

    for (int t = 0; t < num_threads; ++t) {
        merge_threads.emplace_back([&]() {
            std::vector<uint64_t> merged_names;
            while (true) {
                int ci = country_cursor.fetch_add(1, std::memory_order_relaxed);
                if (ci >= total_countries) break;

                auto &fa = final_aggs[ci];
                size_t total_names_est = 0;
                for (int thr = 0; thr < num_threads; ++thr) {
                    const auto &la = all_thread_aggs[thr][ci];
                    if (la.count == 0) continue;
                    fa.count += la.count;
                    fa.id_sum += la.id_sum;
                    fa.amount_sum += la.amount_sum;
                    if (la.amount_min < fa.amount_min) fa.amount_min = la.amount_min;
                    if (la.amount_max > fa.amount_max) fa.amount_max = la.amount_max;
                    fa.name_bytes += la.name_bytes;
                    fa.note_bytes += la.note_bytes;
                    total_names_est += la.name_hashes.size();
                }

                if (fa.count == 0) continue;

                merged_names.clear();
                merged_names.reserve(total_names_est);
                for (int thr = 0; thr < num_threads; ++thr) {
                    const auto &la = all_thread_aggs[thr][ci];
                    if (!la.name_hashes.empty()) {
                        merged_names.insert(merged_names.end(), la.name_hashes.begin(), la.name_hashes.end());
                    }
                }
                std::sort(merged_names.begin(), merged_names.end());
                fa.names_count = (uint32_t)(std::unique(merged_names.begin(), merged_names.end()) - merged_names.begin());
            }
        });
    }
    for (auto &th : merge_threads) th.join();

    std::vector<int> valid;
    for (int i = 0; i < total_countries; ++i) {
        if (final_aggs[i].count > 0) valid.push_back(i);
    }
    std::sort(valid.begin(), valid.end(), [&](int a, int b) {
        return country_names[a] < country_names[b];
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
                     country_names[idx].c_str(),
                     (long long)ca.count, (long long)ca.id_sum,
                     s_sum, s_min, s_max,
                     ca.names_count,
                     (unsigned long long)ca.name_bytes,
                     (unsigned long long)ca.note_bytes);
    }
    std::fclose(out);

    return 0;
}