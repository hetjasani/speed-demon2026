#include <bits/stdc++.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <atomic>
using namespace std;

struct MappedFile {
    int fd = -1;
    size_t size = 0;
    const char* data = nullptr;
    explicit MappedFile(const string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw runtime_error("open failed: " + path);
        struct stat st{};
        if (fstat(fd, &st) != 0) { close(fd); fd = -1; throw runtime_error("fstat failed: " + path); }
        size = (size_t)st.st_size;
        if (size) {
            void* p = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) { close(fd); fd = -1; throw runtime_error("mmap failed: " + path); }
            data = (const char*)p;
        }
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    ~MappedFile() { if (data) munmap((void*)data, size); if (fd >= 0) close(fd); }
};

static uint8_t LOWER[256];
static void init_tables() {
    for (int c = 0; c < 256; ++c) LOWER[c] = 0;
    for (int c = 'a'; c <= 'z'; ++c) LOWER[c] = (uint8_t)c;
    for (int c = 'A'; c <= 'Z'; ++c) LOWER[c] = (uint8_t)(c + 32);
    for (int c = '0'; c <= '9'; ++c) LOWER[c] = (uint8_t)c;
}

static inline uint64_t hash_lower(const char* s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= LOWER[(uint8_t)s[i]]; h *= 1099511628211ull; }
    return h ^ (h >> 29);
}

struct Dict {
    vector<uint32_t> slot; // id + 1; zero means empty
    vector<uint64_t> hashes;
    vector<string> words;
    vector<uint8_t> in_phrase;
    size_t mask = 0;

    void reserve(size_t expected) {
        size_t cap = 1024;
        while (cap < expected + expected / 2) cap <<= 1;
        slot.assign(cap, 0);
        mask = cap - 1;
        words.reserve(expected);
        hashes.reserve(expected);
        in_phrase.reserve(expected);
    }

    uint32_t insert_normalized(string s, bool phrase) {
        for (char& c : s) c = (char)LOWER[(uint8_t)c];
        uint64_t h = hash_lower(s.data(), s.size());
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            if (!slot[i]) {
                uint32_t id = (uint32_t)words.size();
                words.push_back(move(s));
                hashes.push_back(h);
                in_phrase.push_back((uint8_t)phrase);
                slot[i] = id + 1;
                return id;
            }
            uint32_t id = slot[i] - 1;
            if (hashes[id] == h && words[id] == s) {
                if (phrase) in_phrase[id] = 1;
                return id;
            }
        }
    }

    inline int64_t find(const char* s, size_t n, uint64_t h) const {
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            uint32_t x = slot[i];
            if (!x) return -1;
            uint32_t id = x - 1;
            if (hashes[id] == h && words[id].size() == n) {
                if (memcmp(words[id].data(), s, n) == 0) return (int64_t)id;
                bool equal = true;
                for (size_t k = 0; k < n; ++k) {
                    if (LOWER[(uint8_t)s[k]] != (uint8_t)words[id][k]) {
                        equal = false;
                        break;
                    }
                }
                if (equal) return (int64_t)id;
            }
        }
    }
};

struct Query {
    bool phrase = false;
    vector<uint32_t> terms;
    vector<uint8_t> ops; // 0 AND, 1 OR, 2 NOT; evaluated left-to-right
};

static inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static void parse_queries(const MappedFile& file, Dict& dict, vector<Query>& queries) {
    size_t p = 0;
    while (p < file.size) {
        size_t e = p;
        while (e < file.size && file.data[e] != '\n') ++e;
        string_view line(file.data + p, e - p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        Query q;
        if (line.size() >= 8 && line.substr(0, 8) == "PHRASE \"") {
            q.phrase = true;
            size_t begin = 8;
            size_t end = line.size();
            if (end > begin && line[end - 1] == '"') --end;
            size_t i = begin;
            while (i < end) {
                while (i < end && is_space(line[i])) ++i;
                size_t j = i;
                while (j < end && !is_space(line[j])) ++j;
                if (j > i) q.terms.push_back(dict.insert_normalized(string(line.substr(i, j - i)), true));
                i = j;
            }
        } else if (line.size() >= 5 && line.substr(0, 5) == "BOOL ") {
            q.phrase = false;
            size_t i = 5;
            vector<string_view> parts;
            while (i < line.size()) {
                while (i < line.size() && is_space(line[i])) ++i;
                size_t j = i;
                while (j < line.size() && !is_space(line[j])) ++j;
                if (j > i) parts.push_back(line.substr(i, j - i));
                i = j;
            }
            for (size_t k = 0; k < parts.size(); ++k) {
                if ((k & 1) == 0) {
                    q.terms.push_back(dict.insert_normalized(string(parts[k]), false));
                } else {
                    if (parts[k] == "AND") q.ops.push_back(0);
                    else if (parts[k] == "OR") q.ops.push_back(1);
                    else if (parts[k] == "NOT") q.ops.push_back(2);
                    else throw runtime_error("invalid BOOL operator in query file");
                }
            }
            if (q.terms.empty() || q.ops.size() + 1 != q.terms.size())
                throw runtime_error("invalid BOOL query structure");
        } else {
            throw runtime_error("unknown query format");
        }
        if (q.phrase && (q.terms.size() < 2 || q.terms.size() > 8))
            throw runtime_error("PHRASE query term count outside spec");
        if (!q.phrase && (q.terms.empty() || q.terms.size() > 6))
            throw runtime_error("BOOL query term count outside spec");
        queries.push_back(move(q));
        p = e + 1;
    }
}

struct Item { uint32_t term, pos; };
struct DocHdr { uint32_t id; uint64_t start; };
struct Index {
    vector<uint64_t> off;
    vector<uint64_t> key;
    vector<uint64_t> doc_off;
    vector<uint32_t> doc_key;
    const uint64_t* begin(uint32_t t) const { return key.data() + off[t]; }
    const uint64_t* end(uint32_t t) const { return key.data() + off[t + 1]; }
};

template<class F> static void parallel_for(unsigned T, F fn) {
    vector<thread> th;
    th.reserve(T);
    for (unsigned t = 0; t < T; ++t) th.emplace_back(fn, t);
    for (auto& x : th) x.join();
}

static Index build_index(const MappedFile& docs, const Dict& dict, unsigned T) {
    const char* base = docs.data;
    const size_t n = docs.size;
    const size_t V = dict.words.size();
    vector<size_t> bounds(T + 1);
    bounds[0] = 0; bounds[T] = n;
    for (unsigned t = 1; t < T; ++t) {
        size_t x = n * t / T;
        if (x < n) {
            const void* nl = memchr(base + x, '\n', n - x);
            bounds[t] = nl ? (size_t)((const char*)nl - base) + 1 : n;
        } else bounds[t] = n;
        if (bounds[t] < bounds[t - 1]) bounds[t] = bounds[t - 1];
    }

    vector<vector<Item>> items(T);
    vector<vector<DocHdr>> headers(T);
    vector<uint64_t> counts((size_t)T * V, 0);
    vector<uint64_t> doc_counts((size_t)T * V, 0);

    parallel_for(T, [&](unsigned t) {
        auto& rec = items[t];
        auto& hdr = headers[t];
        uint64_t* cnt = counts.data() + (size_t)t * V;
        uint64_t* doc_cnt = doc_counts.data() + (size_t)t * V;
        vector<uint32_t> stamp(V, UINT32_MAX);
        uint32_t seq = 0;
        const char* p = base + bounds[t];
        const char* end = base + bounds[t + 1];
        while (p < end) {
            uint32_t doc_id = 0;
            while (p < end && *p != '\t' && *p != '\n') {
                if (*p >= '0' && *p <= '9') doc_id = doc_id * 10 + (uint32_t)(*p - '0');
                ++p;
            }
            if (p < end && *p == '\t') ++p;
            ++seq;
            uint32_t pos = 0;
            bool has_record = false;
            uint64_t doc_record_start = rec.size();
            while (p < end && *p != '\n') {
                uint8_t c = (uint8_t)*p;
                if (!LOWER[c]) { ++p; continue; }
                const char* tok = p;
                uint64_t h = 1469598103934665603ull;
                size_t len = 0;
                while (p < end) {
                    uint8_t raw = (uint8_t)*p;
                    uint8_t lc = LOWER[raw];
                    if (!lc) break;
                    ++p;
                    h ^= lc;
                    h *= 1099511628211ull;
                    ++len;
                }
                h ^= (h >> 29);
                int64_t tid = dict.find(tok, len, h);
                if (tid >= 0) {
                    uint32_t id = (uint32_t)tid;
                    bool first_in_doc = (stamp[id] != seq);
                    if (first_in_doc) {
                        stamp[id] = seq;
                        ++doc_cnt[id];
                    }
                    if (dict.in_phrase[id] || first_in_doc) {
                        if (!has_record) { hdr.push_back({doc_id, doc_record_start}); has_record = true; }
                        rec.push_back({id, dict.in_phrase[id] ? pos : 0});
                        ++cnt[id];
                    }
                }
                ++pos;
            }
            if (p < end && *p == '\n') ++p;
        }
        hdr.push_back({0, (uint64_t)rec.size()});
    });

    Index ix;
    ix.off.assign(V + 1, 0);
    for (size_t v = 0; v < V; ++v) {
        uint64_t total = 0;
        for (unsigned t = 0; t < T; ++t) total += counts[(size_t)t * V + v];
        ix.off[v + 1] = ix.off[v] + total;
    }
    uint64_t total_keys = ix.off[V];
    ix.key.resize((size_t)max<uint64_t>(1, total_keys));

    vector<uint64_t> cursor((size_t)T * V);
    for (size_t v = 0; v < V; ++v) {
        uint64_t at = ix.off[v];
        for (unsigned t = 0; t < T; ++t) {
            size_t z = (size_t)t * V + v;
            cursor[z] = at;
            at += counts[z];
        }
    }

    parallel_for(T, [&](unsigned t) {
        uint64_t* out = ix.key.data();
        const auto& rec = items[t];
        const auto& hdr = headers[t];
        uint64_t* cur = cursor.data() + (size_t)t * V;
        for (size_t d = 0; d + 1 < hdr.size(); ++d) {
            uint64_t high = (uint64_t)hdr[d].id << 32;
            for (uint64_t k = hdr[d].start; k < hdr[d + 1].start; ++k) {
                const Item& x = rec[(size_t)k];
                out[cur[x.term]++] = high | x.pos;
            }
        }
    });

    ix.doc_off.assign(V + 1, 0);
    for (size_t v = 0; v < V; ++v) {
        uint64_t total = 0;
        for (unsigned t = 0; t < T; ++t)
            total += doc_counts[(size_t)t * V + v];
        ix.doc_off[v + 1] = ix.doc_off[v] + total;
    }

    uint64_t total_docs = ix.doc_off[V];
    ix.doc_key.resize((size_t)max<uint64_t>(1, total_docs));

    vector<uint64_t> doc_cursor((size_t)T * V);
    for (size_t v = 0; v < V; ++v) {
        uint64_t at = ix.doc_off[v];
        for (unsigned t = 0; t < T; ++t) {
            size_t z = (size_t)t * V + v;
            doc_cursor[z] = at;
            at += doc_counts[z];
        }
    }

    parallel_for(T, [&](unsigned t) {
        uint32_t* out = ix.doc_key.data();
        const auto& rec = items[t];
        const auto& hdr = headers[t];
        uint64_t* cur = doc_cursor.data() + (size_t)t * V;
        vector<uint32_t> stamp(V, UINT32_MAX);
        uint32_t seq = 0;

        for (size_t d = 0; d + 1 < hdr.size(); ++d) {
            ++seq;
            uint32_t doc = hdr[d].id;
            for (uint64_t k = hdr[d].start; k < hdr[d + 1].start; ++k) {
                uint32_t term = rec[(size_t)k].term;
                if (stamp[term] != seq) {
                    stamp[term] = seq;
                    out[cur[term]++] = doc;
                }
            }
        }
    });

    return ix;
}

using Ids = vector<uint32_t>;
struct Scratch { Ids a, b, tmp; vector<uint64_t> phrase_a, phrase_b; };

static void doc_set(const Index& ix, uint32_t term, Ids& out) {
    const uint32_t* b = ix.doc_key.data() + ix.doc_off[term];
    const uint32_t* e = ix.doc_key.data() + ix.doc_off[term + 1];
    out.assign(b, e);
}

static void intersect_sets(const Ids& a, const uint32_t* b, size_t bn, Ids& out) {
    out.clear();

    if (a.size() * 16 < bn) {
        const uint32_t* it = b;
        const uint32_t* end = b + bn;
        for (uint32_t x : a) {
            it = lower_bound(it, end, x);
            if (it == end) break;
            if (*it == x) out.push_back(x);
        }
    } else if (bn * 16 < a.size()) {
        auto it = a.begin();
        for (size_t j = 0; j < bn; ++j) {
            it = lower_bound(it, a.end(), b[j]);
            if (it == a.end()) break;
            if (*it == b[j]) out.push_back(b[j]);
        }
    } else {
        size_t i = 0, j = 0;
        while (i < a.size() && j < bn) {
            if (a[i] < b[j]) ++i;
            else if (a[i] > b[j]) ++j;
            else { out.push_back(a[i]); ++i; ++j; }
        }
    }
}

static void union_sets(const Ids& a, const uint32_t* b, size_t bn, Ids& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < bn) {
        if (a[i] < b[j]) out.push_back(a[i++]);
        else if (a[i] > b[j]) out.push_back(b[j++]);
        else { out.push_back(a[i]); ++i; ++j; }
    }
    while (i < a.size()) out.push_back(a[i++]);
    while (j < bn) out.push_back(b[j++]);
}

static void difference_sets(const Ids& a, const uint32_t* b, size_t bn, Ids& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < bn) {
        if (a[i] < b[j]) out.push_back(a[i++]);
        else if (a[i] > b[j]) ++j;
        else { ++i; ++j; }
    }
    while (i < a.size()) out.push_back(a[i++]);
}

static void run_query(const Index& ix, const Query& q, Scratch& sc, Ids& result) {
    result.clear();
    if (q.phrase) {
        const size_t n = q.terms.size();
        size_t anchor = 0;
        for (size_t i = 1; i < n; ++i)
            if (ix.off[q.terms[i] + 1] - ix.off[q.terms[i]] < ix.off[q.terms[anchor] + 1] - ix.off[q.terms[anchor]]) anchor = i;
        const uint64_t* candidates = ix.begin(q.terms[anchor]);
        const uint64_t* candidates_end = ix.end(q.terms[anchor]);

        array<const uint64_t*, 8> cursor{};
        array<const uint64_t*, 8> finish{};
        for (size_t j = 0; j < n; ++j) {
            cursor[j] = ix.begin(q.terms[j]);
            finish[j] = ix.end(q.terms[j]);
        }

        array<size_t, 8> order{};
        size_t order_n = 0;
        for (size_t j = 0; j < n; ++j)
            if (j != anchor) order[order_n++] = j;
        sort(order.begin(), order.begin() + order_n, [&](size_t a, size_t b) {
            return (ix.off[q.terms[a] + 1] - ix.off[q.terms[a]]) <
                   (ix.off[q.terms[b] + 1] - ix.off[q.terms[b]]);
        });

        uint32_t last_doc = UINT32_MAX;
        for (const uint64_t* c = candidates; c != candidates_end; ++c) {
            uint64_t packed = *c;
            uint32_t doc = (uint32_t)(packed >> 32);
            uint32_t pos = (uint32_t)packed;
            if (pos < anchor) continue;
            uint32_t start = pos - (uint32_t)anchor;
            bool found = true;
            for (size_t oi = 0; oi < order_n; ++oi) {
                size_t j = order[oi];
                uint64_t target = ((uint64_t)doc << 32) | (uint64_t)(start + (uint32_t)j);
                const uint64_t* pcur = cursor[j];
                const uint64_t* e = finish[j];
                pcur = lower_bound(pcur, e, target);
                cursor[j] = pcur;
                if (pcur == e || *pcur != target) {
                    found = false;
                    break;
                }
            }
            if (found && doc != last_doc) { result.push_back(doc); last_doc = doc; }
        }
        return;
    }

    if (q.terms.empty()) return;
    doc_set(ix, q.terms[0], sc.a);
    for (size_t i = 0; i < q.ops.size(); ++i) {
        uint32_t term = q.terms[i + 1];
        const uint32_t* b = ix.doc_key.data() + ix.doc_off[term];
        size_t bn = (size_t)(ix.doc_off[term + 1] - ix.doc_off[term]);
        sc.tmp.clear();
        switch (q.ops[i]) {
            case 0: intersect_sets(sc.a, b, bn, sc.tmp); break;
            case 1: union_sets(sc.a, b, bn, sc.tmp); break;
            default: difference_sets(sc.a, b, bn, sc.tmp); break;
        }
        sc.a.swap(sc.tmp);
    }
    result.swap(sc.a);
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) { fprintf(stderr, "usage: %s data_dir out_dir [threads]\n", argv[0]); return 2; }
        init_tables();
        unsigned T = argc > 3 ? (unsigned)max(1, atoi(argv[3])) : max(1u, thread::hardware_concurrency());
        T = min(T, 16u);
        using Clock = chrono::steady_clock;
        auto lap_start = Clock::now();
        auto lap = [&](const char* label) {
            auto now = Clock::now();
            cerr << label << ": " << chrono::duration<double>(now - lap_start).count() << " sec\n";
            lap_start = now;
        };

        string data_dir = argv[1], out_dir = argv[2];
        MappedFile qfile(data_dir + "/queries.txt");
        MappedFile dfile(data_dir + "/docs.txt");
        Dict dict; dict.reserve(1u << 17);
        vector<Query> queries;
        queries.reserve(50000);
        parse_queries(qfile, dict, queries);
        lap("QUERY PARSING");

        Index ix = build_index(dfile, dict, T);
        lap("CSR INDEX BUILD");

        vector<string> lines(queries.size());
        atomic<size_t> next{0};
        parallel_for(T, [&](unsigned) {
            Scratch sc;
            Ids result;
            char buf[64];
            for (;;) {
                size_t i = next.fetch_add(128, memory_order_relaxed);
                if (i >= queries.size()) break;
                size_t stop = min(queries.size(), i + 128);
                for (size_t j = i; j < stop; ++j) {
                    run_query(ix, queries[j], sc, result);
                    uint64_t sum = 0;
                    for (uint32_t id : result) sum += id;
                    string& out = lines[j];
                    int n = snprintf(buf, sizeof(buf), "%zu %llu", result.size(), (unsigned long long)sum);
                    out.assign(buf, (size_t)n);
                    for (size_t k = 0; k < min<size_t>(10, result.size()); ++k) {
                        n = snprintf(buf, sizeof(buf), " %u", result[k]);
                        out.append(buf, (size_t)n);
                    }
                    out.push_back('\n');
                }
            }
        });
        lap("QUERY EXECUTION");

        mkdir(out_dir.c_str(), 0755);
        ofstream out(out_dir + "/results.txt", ios::binary);
        if (!out) throw runtime_error("cannot open output file");
        for (const string& line : lines) out.write(line.data(), (streamsize)line.size());
        out.close();
        lap("OUTPUT WRITE");
        return 0;
    } catch (const exception& e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
