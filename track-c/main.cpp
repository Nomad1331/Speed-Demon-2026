#ifdef __linux__
#include <sys/prctl.h>
#endif
#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <immintrin.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct Mapping {
    int fd = -1; size_t size = 0; const char* data = nullptr;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY | O_BINARY); if (fd < 0) return false;
        struct stat st; if (fstat(fd, &st) < 0) { ::close(fd); fd = -1; return false; }
        size = (size_t)st.st_size; if (size == 0) return true;
        data = (const char*)::mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0); return data != MAP_FAILED;
    }
    void close() {
        if (data && data != MAP_FAILED) { ::munmap((void*)data, size); data = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    ~Mapping() { close(); }
};

struct Field { const char* begin; const char* end; size_t pairs; bool quoted; };

static inline const char* find_quote(const char* p, const char* lim) noexcept {
    const __m256i quote = _mm256_set1_epi8('"');
    for (; p + 32 <= lim; p += 32) {
        const uint32_t m = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), quote)));
        if (m) return p + __builtin_ctz(m);
    }
    return static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(lim - p)));
}

static inline Field read_field(const char*& p, const char* lim) noexcept {
    if (*p != '"') {
        const char* b = p;
        while (p < lim && *p != ',' && *p != '\r' && *p != '\n') ++p;
        return {b, p, 0, false};
    }
    const char* b = ++p;
    size_t pairs = 0;
    for (;;) {
        const char* q = find_quote(p, lim);
        if (!q) { p = lim; return {b, lim, pairs, true}; }
        if (q + 1 < lim && q[1] == '"') { ++pairs; p = q + 2; continue; }
        p = q + 1;
        return {b, q, pairs, true};
    }
}

static inline uint32_t digits8(uint64_t w) noexcept {
    w = ((w & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
    w = ((w & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    return static_cast<uint32_t>(((w & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32);
}

static inline uint32_t read_uint32(const char*& p, const char* lim) noexcept {
    if (p + 8 <= lim) {
        uint64_t w; std::memcpy(&w, p, 8);
        w ^= 0x3030303030303030ULL;
        const uint64_t stop = ((w + 0x7676767676767676ULL) | w) & 0x8080808080808080ULL;
        if (stop) {
            const unsigned digits = static_cast<unsigned>(__builtin_ctzll(stop)) >> 3;
            if (!digits) return 0;
            p += digits;
            return digits8(w << ((8 - digits) * 8));
        }
        uint32_t n = digits8(w);
        for (p += 8; p < lim && *p >= '0' && *p <= '9'; ++p) n = n * 10u + static_cast<uint32_t>(*p - '0');
        return n;
    }
    uint32_t n = 0;
    while (p < lim && *p >= '0' && *p <= '9') n = n * 10u + static_cast<uint32_t>(*p++ - '0');
    return n;
}

static inline int64_t read_cents(const char*& p, const char* lim) noexcept {
    const bool neg = (*p == '-'); if (neg) ++p;
    const int64_t whole = read_uint32(p, lim);
    if (p < lim && *p == '.') ++p;
    int64_t cents = whole * 100;
    if (p + 1 < lim && p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
        cents += int64_t(p[0] - '0') * 10 + int64_t(p[1] - '0');
        p += 2;
    }
    return neg ? -cents : cents;
}

static inline uint64_t hash_bytes(const char* p, size_t n) noexcept {
    uint64_t h = (n + 1) * 0x9E3779B97F4A7C15ULL;
    for (; n >= 8; p += 8, n -= 8) {
        uint64_t w; std::memcpy(&w, p, 8);
        h = (h ^ w) * 0xD6E8FEB86659FD93ULL; h ^= h >> 32;
    }
    if (n) {
        uint64_t w = 0; std::memcpy(&w, p, n);
        h = (h ^ w) * 0xD6E8FEB86659FD93ULL; h ^= h >> 32;
    }
    h *= 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 29);
}

static inline std::pair<uint64_t, size_t> name_hash_and_len(const Field& f, std::string& scratch) {
    if (!f.pairs) return {hash_bytes(f.begin, static_cast<size_t>(f.end - f.begin)), static_cast<size_t>(f.end - f.begin)};
    scratch.clear();
    for (const char* p = f.begin; p < f.end; ++p) {
        scratch.push_back(*p);
        if (*p == '"') ++p;
    }
    return {hash_bytes(scratch.data(), scratch.size()), scratch.size()};
}

static const char DIGITS_LUT[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline char* write_u64(char* p, uint64_t v) {
    if (v < 10) { *p++ = '0' + v; return p; }
    char buf[24], *b = buf + 24;
    while (v >= 100) {
        uint64_t q = v / 100; uint32_t r = (uint32_t)(v - q * 100);
        v = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2);
    }
    if (v < 10) *--b = '0' + v;
    else { b -= 2; memcpy(b, &DIGITS_LUT[v * 2], 2); }
    size_t len = (buf + 24) - b; memcpy(p, b, len); return p + len;
}

inline char* format_money(char* p, int64_t c) {
    if (c < 0) { *p++ = '-'; c = -c; }
    p = write_u64(p, (uint64_t)c / 100); *p++ = '.';
    uint32_t frac = (uint32_t)((uint64_t)c % 100);
    *p++ = '0' + (frac / 10); *p++ = '0' + (frac % 10); return p;
}

struct WorkerCountryAgg {
    uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
    int64_t amount_sum = 0, minimum = 0, maximum = 0;
};

static const char* find_boundary(const char* p, const char* data_end, const char* data_start) {
    while (p < data_end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(data_end - p)));
        if (!nl) return data_end;
        const char* next_rec = nl + 1;
        if (next_rec < data_end && *next_rec >= '0' && *next_rec <= '9') {
            const char* q = next_rec;
            while (q < data_end && *q >= '0' && *q <= '9') ++q;
            if (q < data_end && *q == ',') {
                const char* b = (nl > data_start && *(nl - 1) == '\r') ? nl - 2 : nl - 1;
                if (b - 3 >= data_start && b[0] >= '0' && b[0] <= '9' && b[-1] >= '0' && b[-1] <= '9' && b[-2] == '.' && b[-3] >= '0' && b[-3] <= '9') {
                    return next_rec;
                }
            }
        }
        p = nl + 1;
    }
    return data_end;
}

int main(int argc, char* argv[]) {
#ifdef __linux__
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
#endif
    std::string data_dir = "/data", out_arg = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_arg = argv[++i];
    }
    std::filesystem::path out_path = out_arg.ends_with(".txt") ? std::filesystem::path(out_arg) : std::filesystem::path(out_arg) / "aggregates.txt";
    std::filesystem::create_directories(out_path.parent_path());

    Mapping reg_file;
    if (!reg_file.open((std::filesystem::path(data_dir) / "regions.csv").string())) return 1;

    std::vector<uint16_t> code_to_cid(26 * 26 * 26, 0xFFFF);
    std::vector<std::string> countries;
    std::vector<uint8_t> region_to_country(1'000'001, 0);

    const char* rp = reg_file.data, *rend = rp + reg_file.size;
    while (rp < rend && *rp++ != '\n') {}
    while (rp < rend) {
        const uint32_t region = read_uint32(rp, rend);
        if (rp < rend && *rp == ',') ++rp;
        if (rend - rp < 3) break;
        uint16_t& cid = code_to_cid[static_cast<size_t>(rp[0] - 'A') * 676 + static_cast<size_t>(rp[1] - 'A') * 26 + static_cast<size_t>(rp[2] - 'A')];
        if (cid == 0xFFFF) { cid = static_cast<uint16_t>(countries.size()); countries.emplace_back(rp, 3); }
        rp += 3;
        while (rp < rend && (*rp == '\r' || *rp == '\n')) ++rp;
        if (region < region_to_country.size()) region_to_country[region] = static_cast<uint8_t>(cid + 1);
    }

    const uint16_t unmatched_cid = static_cast<uint16_t>(countries.size());
    countries.push_back("_UNMATCHED");
    const size_t n_countries = countries.size();

    Mapping rec_file;
    if (!rec_file.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;
    madvise((void*)rec_file.data, rec_file.size, MADV_WILLNEED | MADV_SEQUENTIAL);

    const char* recs = rec_file.data, *recs_end = recs + rec_file.size;
    if (recs_end - recs >= 3 && static_cast<unsigned char>(recs[0]) == 0xef && static_cast<unsigned char>(recs[1]) == 0xbb && static_cast<unsigned char>(recs[2]) == 0xbf) recs += 3;
    while (recs < recs_end && *recs++ != '\n') {}
    const char* data_start = recs;
    const size_t total_data_size = static_cast<size_t>(recs_end - data_start);

    unsigned int WORKERS = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    if (rec_file.size < 200000) WORKERS = 1;

    std::vector<const char*> split_points(WORKERS + 1);
    split_points[0] = data_start; split_points[WORKERS] = recs_end;
    for (unsigned w = 1; w < WORKERS; ++w) {
        split_points[w] = find_boundary(data_start + total_data_size * w / WORKERS, recs_end, data_start);
    }

    std::vector<std::vector<WorkerCountryAgg>> partial(WORKERS, std::vector<WorkerCountryAgg>(n_countries));

    constexpr uint32_t NAME_SLOTS = 1u << 17, ROW_WORDS = NAME_SLOTS / 64;
    auto* const name_table = static_cast<std::atomic<uint64_t>*>(std::calloc(NAME_SLOTS, sizeof(uint64_t)));
    auto* const seen = static_cast<std::atomic<uint64_t>*>(std::calloc(n_countries * ROW_WORDS, sizeof(uint64_t)));

    auto parse_range = [&](unsigned w) {
        const char* cur = split_points[w], *lim = split_points[w + 1];
        auto& aggs = partial[w];
        std::string scratch;

        struct Rec { uint64_t hash; int64_t amount; uint32_t id, region, name_len, note_len; uint32_t group, slot; };
        constexpr unsigned BATCH = 32;
        Rec batch[BATCH];
        unsigned nb = 0;

        auto drain = [&]() {
            for (unsigned i = 0; i < nb; ++i) {
                Rec& r = batch[i];
                const uint8_t mapped = (r.region < region_to_country.size()) ? region_to_country[r.region] : 0;
                r.group = mapped ? static_cast<uint32_t>(mapped - 1) : unmatched_cid;
                uint32_t slot = static_cast<uint32_t>(r.hash) & (NAME_SLOTS - 1);
                for (;;) {
                    uint64_t key = name_table[slot].load(std::memory_order_relaxed);
                    if (key == 0 && name_table[slot].compare_exchange_strong(key, r.hash, std::memory_order_relaxed)) break;
                    if (key == r.hash) break;
                    slot = (slot + 1) & (NAME_SLOTS - 1);
                }
                r.slot = slot;
                __builtin_prefetch(&seen[static_cast<size_t>(r.group) * ROW_WORDS + (slot >> 6)], 1);
            }
            for (unsigned i = 0; i < nb; ++i) {
                const Rec& r = batch[i];
                WorkerCountryAgg& agg = aggs[r.group];
                if (!agg.count) { agg.minimum = agg.maximum = r.amount; }
                else {
                    if (r.amount < agg.minimum) agg.minimum = r.amount;
                    if (r.amount > agg.maximum) agg.maximum = r.amount;
                }
                ++agg.count; agg.id_sum += r.id; agg.amount_sum += r.amount;
                agg.name_bytes += r.name_len; agg.note_bytes += r.note_len;
                const uint64_t bit = 1ULL << (r.slot & 63);
                std::atomic<uint64_t>& word = seen[static_cast<size_t>(r.group) * ROW_WORDS + (r.slot >> 6)];
                if (!(word.load(std::memory_order_relaxed) & bit)) word.fetch_or(bit, std::memory_order_relaxed);
            }
            nb = 0;
        };

        while (cur < lim) {
            if (lim - cur >= 3 && static_cast<unsigned char>(cur[0]) == 0xef && static_cast<unsigned char>(cur[1]) == 0xbb && static_cast<unsigned char>(cur[2]) == 0xbf) cur += 3;
            const uint32_t rec_id = read_uint32(cur, lim);
            if (cur < lim && *cur == ',') ++cur;
            const Field name = read_field(cur, lim);
            if (cur < lim && *cur == ',') ++cur;
            const Field note = read_field(cur, lim);
            if (cur < lim && *cur == ',') ++cur;
            const uint32_t region = read_uint32(cur, lim);
            if (cur < lim && *cur == ',') ++cur;
            const int64_t amount = read_cents(cur, lim);
            if (cur < lim && *cur == '\r') ++cur;
            if (cur < lim && *cur == '\n') ++cur;

            auto [h, n] = name_hash_and_len(name, scratch);
            if (!h) h = 1;
            __builtin_prefetch(&name_table[static_cast<uint32_t>(h) & (NAME_SLOTS - 1)], 1);
            if (region < region_to_country.size()) __builtin_prefetch(&region_to_country[region]);
            batch[nb++] = {h, amount, rec_id, region, static_cast<uint32_t>(n),
                           static_cast<uint32_t>(static_cast<uint64_t>(note.end - note.begin) - note.pairs), 0, 0};
            if (nb == BATCH) drain();
        }
        drain();
    };

    std::vector<std::thread> pool; pool.reserve(WORKERS);
    for (unsigned i = 0; i < WORKERS; ++i) pool.emplace_back(parse_range, i);
    for (auto& t : pool) t.join();
    pool.clear();

    struct FinalResult {
        std::string country;
        uint64_t count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
        int64_t amount_sum = 0, amount_min = 0, amount_max = 0;
        size_t distinct_names = 0;
    };
    std::vector<FinalResult> results(n_countries);

    for (size_t c = 0; c < n_countries; ++c) {
        FinalResult& res = results[c]; res.country = countries[c];
        bool first_set = true;
        for (unsigned w = 0; w < WORKERS; ++w) {
            const auto& src = partial[w][c];
            if (!src.count) continue;
            if (first_set) { res.amount_min = src.minimum; res.amount_max = src.maximum; first_set = false; }
            else {
                if (src.minimum < res.amount_min) res.amount_min = src.minimum;
                if (src.maximum > res.amount_max) res.amount_max = src.maximum;
            }
            res.count += src.count; res.id_sum += src.id_sum; res.amount_sum += src.amount_sum;
            res.name_bytes += src.name_bytes; res.note_bytes += src.note_bytes;
        }
        for (uint32_t i = 0; i < ROW_WORDS; ++i) res.distinct_names += static_cast<size_t>(__builtin_popcountll(seen[c * ROW_WORDS + i].load(std::memory_order_relaxed)));
    }

    std::vector<size_t> order; order.reserve(n_countries);
    for (size_t c = 0; c < n_countries; ++c) if (results[c].count > 0) order.push_back(c);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return results[a].country < results[b].country; });

    char* out_buf = (char*)malloc(256 * 1024);
    char* optr = out_buf;
    for (size_t c : order) {
        const auto& r = results[c];
        memcpy(optr, r.country.data(), r.country.size()); optr += r.country.size(); *optr++ = ' ';
        optr = write_u64(optr, r.count); *optr++ = ' ';
        optr = write_u64(optr, r.id_sum); *optr++ = ' ';
        optr = format_money(optr, r.amount_sum); *optr++ = ' ';
        optr = format_money(optr, r.amount_min); *optr++ = ' ';
        optr = format_money(optr, r.amount_max); *optr++ = ' ';
        optr = write_u64(optr, r.distinct_names); *optr++ = ' ';
        optr = write_u64(optr, r.name_bytes); *optr++ = ' ';
        optr = write_u64(optr, r.note_bytes); *optr++ = '\n';
    }

    int out_fd = ::open(out_path.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (out_fd >= 0) {
        size_t written = 0, total = optr - out_buf;
        while (written < total) {
            ssize_t w = ::write(out_fd, out_buf + written, total - written);
            if (w <= 0) break;
            written += w;
        }
        ::close(out_fd);
    }
    _exit(0);
}
