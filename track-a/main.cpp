// Speed Demon 2026, Track A (Ingest) - Robust Single-Pass Architecture
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <immintrin.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <utility>

#ifndef O_BINARY
#define O_BINARY 0
#endif

using u32 = uint32_t;
using u64 = uint64_t;
constexpr int HOURS = 72;
constexpr u32 KEY_BITS = 13, ERR = 1u << 12, ID_MASK = ERR - 1, PACKED_MAX = 1u << 19;

static const char DIGITS_LUT[] = 
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline char* write_u32_fast(char* p, uint32_t val) {
    if (val < 10) { *p++ = (char)('0' + val); return p; }
    char buf[12], *b = buf + 12;
    while (val >= 100) {
        uint32_t q = val / 100, r = val - q * 100;
        val = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2);
    }
    if (val < 10) *--b = (char)('0' + val);
    else { b -= 2; memcpy(b, &DIGITS_LUT[val * 2], 2); }
    size_t len = (buf + 12) - b; memcpy(p, b, len); return p + len;
}

inline u32 width(u32 x) {
    static constexpr u32 p10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
    u32 t = (32 - __builtin_clz(x | 1)) * 1233 >> 12;
    return t + ((x | 1) >= p10[t]);
}

inline uint64_t hash_endpoint(const char* s, size_t len) {
    uint64_t h = len * 11400714819323198485ULL; const char* end = s + len;
    while (s + 8 <= end) { uint64_t v; memcpy(&v, s, 8); h = _mm_crc32_u64(h, v); s += 8; }
    while (s < end) h = _mm_crc32_u64(h, (uint8_t)*s++);
    return h ? h : 1;
}

inline int get_hour_id(const char* p) {
    int d = (p[8] - '0') * 10 + (p[9] - '0'), h = (p[11] - '0') * 10 + (p[12] - '0');
    return ((p[6] == '0') ? (d - 30) : (d + 1)) * 24 + h - 18;
}

struct SharedSlot {
    std::atomic<uint32_t> lock{0};
    uint64_t hash = 0;
    uint32_t offset = 0;
    uint16_t len = 0, id = 0;
};

struct EpTable {
    static constexpr size_t CAP = 8192, MASK = CAP - 1;
    SharedSlot slots[CAP];
    std::atomic<uint16_t> ep_count{0};

    inline uint16_t lookup_or_insert(const char* s, uint16_t len, uint64_t h, size_t file_offset, const char* file_base) {
        size_t idx = h & MASK;
        while (true) {
            uint32_t state = slots[idx].lock.load(std::memory_order_acquire);
            if (state == 2) {
                if (slots[idx].hash == h && slots[idx].len == len && memcmp(file_base + slots[idx].offset, s, len) == 0)
                    return slots[idx].id;
            } else if (state == 0) {
                uint32_t expected = 0;
                if (slots[idx].lock.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
                    uint16_t id = ep_count.fetch_add(1, std::memory_order_relaxed);
                    slots[idx].hash = h; slots[idx].offset = (uint32_t)file_offset; slots[idx].len = len; slots[idx].id = id;
                    slots[idx].lock.store(2, std::memory_order_release);
                    return id;
                }
                continue;
            } else if (state == 1) {
                while (slots[idx].lock.load(std::memory_order_acquire) == 1) _mm_pause();
                if (slots[idx].hash == h && slots[idx].len == len && memcmp(file_base + slots[idx].offset, s, len) == 0)
                    return slots[idx].id;
            }
            idx = (idx + 1) & MASK;
        }
    }
};
static EpTable ep_table;

struct LocalEpCache {
    struct Entry { uint64_t hash; uint16_t len, id; } entries[2048]{};
    inline uint16_t get_or_lookup(const char* s, uint16_t len, uint64_t h, size_t off, const char* base) {
        size_t idx = h & 2047;
        if (entries[idx].hash == h && entries[idx].len == len) return entries[idx].id;
        uint16_t id = ep_table.lookup_or_insert(s, len, h, off, base);
        entries[idx] = { h, len, id }; return id;
    }
};

struct Chunk {
    static constexpr u32 CAP = (16384 - 16) / 4;
    Chunk* prev = nullptr;
    u32 n = 0, hour = 0;
    u32 rec[CAP];
};

struct Parser {
    Chunk* last[HOURS] = {};
    Chunk* next_chunk = nullptr;
    Chunk* kept = nullptr;
    std::vector<Chunk> arena;
    std::vector<std::pair<u32, u32>> wide[HOURS];

    void init(size_t file_size, int nthreads) {
        size_t est_chunks = (file_size / 60 / nthreads / Chunk::CAP) + 128;
        if (est_chunks < 64) est_chunks = 64;
        arena.resize(est_chunks);
        next_chunk = kept = arena.data();
    }

    inline void add_record(int h, u32 lat, u32 key) {
        if (__builtin_expect(lat >= PACKED_MAX, 0)) {
            wide[h].push_back({lat, key});
            return;
        }
        Chunk*& c = last[h];
        if (!c || c->n == Chunk::CAP) {
            next_chunk->prev = c;
            next_chunk->n = 0;
            next_chunk->hour = h;
            c = next_chunk++;
        }
        c->rec[c->n++] = (lat << KEY_BITS) | key;
    }

    template <class F> void each(int h, const u32* rank, F&& f) const {
        for (const Chunk* c = last[h]; c; c = c->prev) {
            for (u32 i = 0; i < c->n; ++i) {
                u32 val = c->rec[i];
                f(rank[val & ID_MASK], val & ERR, val >> KEY_BITS);
            }
        }
        for (auto [lat, key] : wide[h]) {
            f(rank[key & ID_MASK], key & ERR, lat);
        }
    }
};

inline void select_val(u32 *a, u32 n, u32 k) {
    for (int round = 0; n > 1; ++round) {
        if (round == 40) { std::nth_element(a, a + k, a + n); return; }
        u32 m = n / 2;
        if (a[m] < a[0]) std::swap(a[m], a[0]);
        if (a[n - 1] < a[m]) std::swap(a[n - 1], a[m]);
        if (a[m] < a[0]) std::swap(a[m], a[0]);
        std::swap(a[m], a[n - 1]);
        u32 pivot = a[n - 1], lt = 0;
        for (u32 i = 0; i + 1 < n; ++i) {
            u32 x = a[i];
            a[i] = a[lt]; a[lt] = x; lt += (x < pivot);
        }
        std::swap(a[lt], a[n - 1]);
        if (k == lt) return;
        if (k < lt) n = lt;
        else { a += lt + 1; k -= lt + 1; n -= lt + 1; }
    }
}

int main(int argc, char** argv) {
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);

    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (arg.starts_with("--data=")) data_dir = arg.substr(7);
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg.starts_with("--out=")) out_dir = arg.substr(6);
        else if (arg[0] != '-') {
            if (data_dir == "/data") data_dir = argv[i];
            else if (out_dir == "/out") out_dir = argv[i];
        }
    }

    std::filesystem::path in_path = std::filesystem::path(data_dir) / "requests.log";
    std::filesystem::path out_path = std::filesystem::path(out_dir) / "summary.txt";
    std::filesystem::create_directories(out_dir);

    int in = open(in_path.string().c_str(), O_RDONLY | O_BINARY);
    if (in < 0) return 0;
    struct stat in_st;
    if (fstat(in, &in_st) < 0 || in_st.st_size == 0) { close(in); return 0; }
    size_t size = (size_t)in_st.st_size;

    const char* log = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, in, 0);
    if (log == MAP_FAILED) { close(in); return 0; }
    madvise((void*)log, size, MADV_WILLNEED | MADV_SEQUENTIAL);

    const int nt = (size < 100000) ? 1 : std::clamp<int>(std::thread::hardware_concurrency(), 1, 12);
    const int pieces = 8 * nt;
    std::vector<size_t> cut(pieces + 1, size);
    cut[0] = 0;
    for (int k = 1; k < pieces; ++k) {
        size_t pos = std::max(cut[k - 1], size * k / pieces);
        while (pos < size && log[pos - 1] != '\n') ++pos;
        cut[k] = pos;
    }

    std::atomic<int> claim_piece{0}, claim_hour{0}, claim_text{0};
    std::vector<Parser> parsers(nt);
    for (int t = 0; t < nt; ++t) parsers[t].init(size, nt);

    std::vector<std::string_view> names;
    std::vector<u32> rank(8192, 0);
    u32 most = 0;

    std::barrier ranked(nt, [&]() noexcept {
        for (int h = 0; h < HOURS; ++h) {
            u32 n = 0;
            for (auto& ps : parsers) ps.each(h, rank.data(), [&](u32, u32, u32) { ++n; });
            most = std::max(most, n);
        }
        uint16_t N = ep_table.ep_count.load(std::memory_order_relaxed);
        struct EpRef { std::string_view sv; uint16_t id; };
        std::vector<EpRef> refs; refs.reserve(N);
        for (size_t i = 0; i < EpTable::CAP; ++i) {
            if (ep_table.slots[i].lock.load(std::memory_order_relaxed) == 2) {
                uint16_t id = ep_table.slots[i].id;
                refs.push_back({ std::string_view(log + ep_table.slots[i].offset, ep_table.slots[i].len), id });
            }
        }
        std::sort(refs.begin(), refs.end(), [](const EpRef& a, const EpRef& b) { return a.sv < b.sv; });
        names.resize(refs.size());
        for (size_t i = 0; i < refs.size(); ++i) {
            names[i] = refs[i].sv;
            rank[refs[i].id] = (u32)i;
        }
    });

    struct Line { u32 e, n, p50, p95, p99, errors; };
    auto ppm = [](const Line& l) { return u32(u64(l.errors) * 1000000 / l.n); };
    std::vector<std::vector<Line>> lines(HOURS);
    std::vector<u64> at(HOURS + 1);
    std::vector<std::string> hour_texts(HOURS);

    std::barrier sized(nt, [&]() noexcept {
        u64 total = 0;
        for (int h = 0; h < HOURS; ++h) total += std::exchange(at[h], total);
        at[HOURS] = total;
    });

    std::vector<std::thread> pool;
    for (int t = 0; t < nt; ++t) {
        pool.emplace_back([&, t] {
            LocalEpCache cache;
            for (int k; (k = claim_piece++) < pieces;) {
                const char* p = log + cut[k], *end = log + cut[k + 1];
                uint64_t last_tag = 0; int last_hid = -1;

                while (p < end) {
                    if (__builtin_expect(p + 35 > end, 0)) break;
                    uint64_t cur_tag; memcpy(&cur_tag, p + 5, 8);
                    int hid = (cur_tag == last_tag) ? last_hid : get_hour_id(p);
                    if (cur_tag != last_tag) { last_tag = cur_tag; last_hid = hid; }

                    const char* cur = p + 28;
                    int off = (cur[3] == ' ') ? 4 : (cur[4] == ' ') ? 5 : (cur[5] == ' ') ? 6 : (cur[6] == ' ') ? 7 : (cur[7] == ' ') ? 8 : 0;
                    if (off) cur += off; else { while (*cur != ' ') ++cur; ++cur; }

                    const char* ep_start = cur;
                    while (*cur != ' ') ++cur;
                    uint16_t ep_len = (uint16_t)(cur - ep_start);
                    uint64_t h = hash_endpoint(ep_start, ep_len);
                    uint16_t ep_id = cache.get_or_lookup(ep_start, ep_len, h, (size_t)(ep_start - log), log);

                    bool is_err = (cur[1] == '5');
                    cur += 5;
                    uint32_t lat = (uint8_t)(*cur++ - '0');
                    while ((uint8_t)(*cur - '0') <= 9) lat = lat * 10 + (uint8_t)(*cur++ - '0');

                    u32 key = (u32)ep_id | (is_err ? ERR : 0);
                    parsers[t].add_record(hid, lat, key);

                    if (*cur == '\n') p = cur + 1;
                    else { while (cur < end && *cur != '\n') ++cur; p = cur + 1; }
                }

                uintptr_t a = (uintptr_t(log + cut[k]) + 4095) & ~uintptr_t(4095);
                uintptr_t b = uintptr_t(log + cut[k + 1]) & ~uintptr_t(4095);
                if (b > a) madvise((void*)a, b - a, MADV_DONTNEED);
            }

            ranked.arrive_and_wait();

            {
                const u32 ne = (u32)names.size();
                std::vector<u32> start(ne + 1), errors(ne), cursor(ne);
                std::unique_ptr<u32[]> latency(new u32[most ? most : 1]);

                for (int h; (h = claim_hour++) < HOURS;) {
                    std::fill(start.begin(), start.end(), 0);
                    std::fill(errors.begin(), errors.end(), 0);

                    for (auto& ps : parsers)
                        ps.each(h, rank.data(), [&](u32 e, u32 err, u32) {
                            ++start[e + 1]; errors[e] += (err != 0);
                        });

                    for (u32 e = 0; e < ne; ++e) {
                        cursor[e] = start[e];
                        start[e + 1] += start[e];
                    }

                    for (auto& ps : parsers)
                        ps.each(h, rank.data(), [&](u32 e, u32, u32 l) {
                            latency[cursor[e]++] = l;
                        });

                    u64 sz = 0;
                    for (u32 e = 0; e < ne; ++e) {
                        u32 n = start[e + 1] - start[e];
                        if (!n) continue;
                        u32* a = latency.get() + start[e];
                        u32 r50 = (50 * n + 99) / 100 - 1;
                        u32 r95 = (95 * n + 99) / 100 - 1;
                        u32 r99 = (99 * n + 99) / 100 - 1;
                        for (u32 from = 0; u32 r : {r50, r95, r99}) {
                            if (r >= from) {
                                select_val(a + from, n - from, r - from);
                                from = r + 1;
                            }
                        }
                        const Line& l = lines[h].emplace_back(e, n, a[r50], a[r95], a[r99], errors[e]);
                        sz += 28 + names[e].size();
                        for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)}) sz += width(x);
                    }
                    at[h] = sz;
                }
            }

            sized.arrive_and_wait();

            for (int h; (h = claim_text++) < HOURS;) {
                char label[32];
                int day = (h + 18) / 24;
                snprintf(label, sizeof(label), "2026-%02d-%02dT%02d:00:00Z ",
                         day < 2 ? 10 : 11,
                         day < 2 ? 30 + day : day - 1,
                         (h + 18) % 24);
                std::string& s = hour_texts[h];
                s.reserve(at[h]);
                for (const Line& l : lines[h]) {
                    s.append(label, 21);
                    s.append(names[l.e]);
                    for (u32 x : {l.n, l.p50, l.p95, l.p99, l.errors, ppm(l)}) {
                        s.push_back(' ');
                        char num_buf[16];
                        char* end_ptr = write_u32_fast(num_buf, x);
                        s.append(num_buf, end_ptr - num_buf);
                    }
                    s.push_back('\n');
                }
                std::vector<Line>().swap(lines[h]);
            }
        });
    }
    for (auto& th : pool) th.join();

    FILE* out_f = fopen(out_path.string().c_str(), "wb");
    if (out_f) {
        for (int h = 0; h < HOURS; ++h) {
            if (!hour_texts[h].empty()) {
                fwrite(hour_texts[h].data(), 1, hour_texts[h].size(), out_f);
            }
        }
        fclose(out_f);
    }

    munmap((void*)log, size);
    close(in);
    _exit(0);
}
