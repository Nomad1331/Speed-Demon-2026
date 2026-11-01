#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <immintrin.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef O_BINARY
#define O_BINARY 0
#endif

using u32 = uint32_t;
using u64 = uint64_t;

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

static inline uint32_t digits8(uint64_t w) noexcept {
    w = ((w & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
    w = ((w & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    return static_cast<uint32_t>(((w & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32);
}

static inline uint32_t read_uint32_swar(const char*& p, const char* lim) noexcept {
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

static inline int64_t read_cents_swar(const char*& p, const char* lim) noexcept {
    const bool neg = (*p == '-'); if (neg) ++p;
    const int64_t whole = read_uint32_swar(p, lim);
    if (p < lim && *p == '.') ++p;
    int64_t cents = whole * 100;
    if (p + 1 < lim && p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9') {
        cents += int64_t(p[0] - '0') * 10 + int64_t(p[1] - '0');
        p += 2;
    }
    return neg ? -cents : cents;
}

struct Field { const char* begin; const char* end; size_t pairs; bool quoted; };

static inline Field read_field(const char*& p, const char* lim) noexcept {
    if (*p != '"') {
        const char* b = p;
        while (p < lim && *p != ',' && *p != '\r' && *p != '\n') ++p;
        return {b, p, 0, false};
    }
    const char* b = ++p;
    size_t pairs = 0;
    for (;;) {
        const char* q = static_cast<const char*>(std::memchr(p, '"', lim - p));
        if (!q) { p = lim; return {b, lim, pairs, true}; }
        if (q + 1 < lim && q[1] == '"') { ++pairs; p = q + 2; continue; }
        p = q + 1;
        return {b, q, pairs, true};
    }
}

static inline uint64_t hash_name(const Field& f, std::string& scratch) {
    const char* p; size_t n;
    if (!f.pairs) { p = f.begin; n = f.end - f.begin; }
    else {
        scratch.clear();
        for (const char* cp = f.begin; cp < f.end; ++cp) {
            scratch.push_back(*cp);
            if (*cp == '"') ++cp;
        }
        p = scratch.data(); n = scratch.size();
    }
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
    return (h ^ (h >> 29)) | 1;
}

struct MappedFile {
    const char* data = nullptr; size_t size = 0; int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY | O_BINARY); if (fd < 0) return false;
        struct stat st; if (fstat(fd, &st) < 0) { ::close(fd); fd = -1; return false; }
        size = (size_t)st.st_size; if (!size) return true;
        data = (const char*)::mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0); return data != MAP_FAILED;
    }
    void close() {
        if (data && data != MAP_FAILED) { ::munmap((void*)data, size); data = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    ~MappedFile() { close(); }
};

static constexpr u32 BITS = 17, SLOTS = 1 << BITS;
alignas(64) static std::atomic<u64> g_slots[SLOTS];

static constexpr u32 WORDS_PER_COUNTRY = SLOTS / 64;
alignas(64) static std::atomic<u64> g_seen_bits[256 * WORDS_PER_COUNTRY];

inline u32 get_name_id(u64 k) {
    for (u32 i = (u32)((k ^ (k >> 16)) & (SLOTS - 1)) & ~3u;; i = (i + 4) & (SLOTS - 1)) {
        __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i*>(&g_slots[i]));
        u32 hit = (u32)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v, _mm256_set1_epi64x(k))));
        if (hit) return i + __builtin_ctz(hit);
        u32 free = (u32)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(v, _mm256_setzero_si256())));
        while (free) {
            u32 j = i + __builtin_ctz(free);
            u64 expected = 0;
            if (g_slots[j].compare_exchange_strong(expected, k, std::memory_order_relaxed) || expected == k) {
                return j;
            }
            free &= free - 1;
        }
    }
}

inline bool mark_seen(u32 cid, u32 name_id) {
    size_t idx = (size_t)cid * WORDS_PER_COUNTRY + (name_id >> 6);
    u64 bit = 1ULL << (name_id & 63);
    u64 cur = g_seen_bits[idx].load(std::memory_order_relaxed);
    if (!(cur & bit)) {
        return !(g_seen_bits[idx].fetch_or(bit, std::memory_order_relaxed) & bit);
    }
    return false;
}

struct LocalCountryAgg {
    uint64_t count = 0, name_bytes = 0, note_bytes = 0, id_sum = 0;
    int64_t amount_sum = 0, amount_min = 0, amount_max = 0;
    uint32_t distinct_names = 0;
};

struct ThreadData { LocalCountryAgg countries[256]; };
static ThreadData tdata[16];
static uint8_t region_to_country[1000000];
struct Chunk { const char* start = nullptr; const char* end = nullptr; };

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

    MappedFile mf_regions;
    if (!mf_regions.open((std::filesystem::path(data_dir) / "regions.csv").string())) return 1;

    std::vector<uint16_t> code_to_cid(26 * 26 * 26, 0xFFFF);
    std::vector<std::string> country_names;
    const char* rp = mf_regions.data, *rend = rp + mf_regions.size;
    while (rp < rend && *rp++ != '\n') {}
    while (rp < rend) {
        const uint32_t region = read_uint32_swar(rp, rend);
        if (rp < rend && *rp == ',') ++rp;
        if (rend - rp < 3) break;
        uint16_t& cid = code_to_cid[(rp[0] - 'A') * 676 + (rp[1] - 'A') * 26 + (rp[2] - 'A')];
        if (cid == 0xFFFF) { cid = (uint16_t)country_names.size(); country_names.emplace_back(rp, 3); }
        rp += 3;
        while (rp < rend && (*rp == '\r' || *rp == '\n')) ++rp;
        if (region < 1000000) region_to_country[region] = (uint8_t)(cid + 1);
    }
    const uint8_t unmatched_cid = (uint8_t)(country_names.size() + 1);
    const size_t total_cids = country_names.size() + 2;

    MappedFile mf_records;
    if (!mf_records.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;
#ifdef __linux__
    madvise((void*)mf_records.data, mf_records.size, MADV_WILLNEED | MADV_SEQUENTIAL);
#endif
    const char* ptr0 = mf_records.data, *file_end = mf_records.data + mf_records.size;
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) ptr0 += 3;
    while (ptr0 < file_end && *ptr0++ != '\n') {}

    unsigned int nthreads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (mf_records.size < 200000) nthreads = 1;

    const unsigned int num_pieces = (nthreads == 1) ? 1 : 96;
    std::vector<Chunk> chunks(num_pieces);
    chunks[0].start = ptr0;
    if (num_pieces == 1) chunks[0].end = file_end;
    else {
        __m256i v_cr = _mm256_set1_epi8('\r');
        for (unsigned int i = 1; i < num_pieces; ++i) {
            const char* cur = mf_records.data + (mf_records.size * i) / num_pieces;
            while (cur + 32 < file_end) {
                __m256i v = _mm256_loadu_si256((const __m256i*)cur);
                uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_cr));
                while (mask) {
                    int idx = __builtin_ctz(mask);
                    if (cur[idx + 1] == '\n') { cur += idx + 2; goto found_chunk; }
                    mask &= mask - 1;
                }
                cur += 32;
            }
            while (cur + 1 < file_end && !(cur[0] == '\r' && cur[1] == '\n')) cur++;
            cur = (cur + 1 < file_end) ? cur + 2 : file_end;
        found_chunk: chunks[i].start = cur; chunks[i - 1].end = cur;
        }
        chunks[num_pieces - 1].end = file_end;
    }

    std::atomic<unsigned int> piece_idx{0};

    auto worker_fn = [&](unsigned int t) {
        auto& my_countries = tdata[t].countries;
        std::string scratch;

        for (;;) {
            unsigned int p_idx = piece_idx.fetch_add(1, std::memory_order_relaxed);
            if (p_idx >= num_pieces) break;

            const char* ptr = chunks[p_idx].start, *chunk_end = chunks[p_idx].end;

            while (ptr < chunk_end) {
                _mm_prefetch(ptr + 2048, _MM_HINT_T0);
                if (__builtin_expect((uint8_t)*ptr == 0xef, 0)) {
                    if (ptr + 3 <= chunk_end && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) ptr += 3;
                }
                uint64_t id = read_uint32_swar(ptr, chunk_end);
                if (ptr < chunk_end && *ptr == ',') ptr++;

                const Field name = read_field(ptr, chunk_end);
                if (ptr < chunk_end && *ptr == ',') ptr++;

                const Field note = read_field(ptr, chunk_end);
                if (ptr < chunk_end && *ptr == ',') ptr++;

                uint32_t region_id = read_uint32_swar(ptr, chunk_end);
                if (ptr < chunk_end && *ptr == ',') ptr++;

                int64_t cents = read_cents_swar(ptr, chunk_end);
                if (ptr < chunk_end && *ptr == '\r') ptr++;
                if (ptr < chunk_end && *ptr == '\n') ptr++;

                uint8_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0;
                if (!cid) cid = unmatched_cid;

                LocalCountryAgg& agg = my_countries[cid];
                if (__builtin_expect(agg.count == 0, 0)) { agg.amount_min = agg.amount_max = cents; }
                else { agg.amount_min = std::min(agg.amount_min, cents); agg.amount_max = std::max(agg.amount_max, cents); }
                agg.count++; agg.id_sum += id; agg.amount_sum += cents;
                agg.name_bytes += (size_t)(name.end - name.begin - name.pairs);
                agg.note_bytes += (size_t)(note.end - note.begin - note.pairs);

                uint64_t nh = hash_name(name, scratch);
                u32 name_id = get_name_id(nh);
                if (mark_seen(cid, name_id)) {
                    agg.distinct_names++;
                }
            }
        }
    };

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < nthreads; ++i) workers.emplace_back(worker_fn, i);
    for (auto& w : workers) w.join();

    struct OrderEntry { std::string name; uint8_t cid; };
    std::vector<OrderEntry> order;
    for (size_t i = 0; i < country_names.size(); ++i) order.push_back({country_names[i], (uint8_t)(i + 1)});
    std::sort(order.begin(), order.end(), [](const OrderEntry& a, const OrderEntry& b) { return a.name < b.name; });
    order.push_back({"_UNMATCHED", unmatched_cid});

    FILE* fp = fopen(out_path.string().c_str(), "wb");
    char out_buf[1024];

    for (const auto& entry : order) {
        uint8_t cid = entry.cid;
        LocalCountryAgg total;
        bool any = false;
        for (unsigned int t = 0; t < nthreads; ++t) {
            const auto& src = tdata[t].countries[cid];
            if (src.count) {
                if (!any) { total.amount_min = src.amount_min; total.amount_max = src.amount_max; any = true; }
                else { total.amount_min = std::min(total.amount_min, src.amount_min); total.amount_max = std::max(total.amount_max, src.amount_max); }
                total.count += src.count; total.id_sum += src.id_sum;
                total.amount_sum += src.amount_sum; total.name_bytes += src.name_bytes;
                total.note_bytes += src.note_bytes; total.distinct_names += src.distinct_names;
            }
        }
        if (!any) continue;
        char* cur = out_buf;
        memcpy(cur, entry.name.data(), entry.name.size()); cur += entry.name.size(); *cur++ = ' ';
        cur = write_u64(cur, total.count); *cur++ = ' ';
        cur = write_u64(cur, total.id_sum); *cur++ = ' ';
        cur = format_money(cur, total.amount_sum); *cur++ = ' ';
        cur = format_money(cur, total.amount_min); *cur++ = ' ';
        cur = format_money(cur, total.amount_max); *cur++ = ' ';
        cur = write_u64(cur, total.distinct_names); *cur++ = ' ';
        cur = write_u64(cur, total.name_bytes); *cur++ = ' ';
        cur = write_u64(cur, total.note_bytes); *cur++ = '\n';
        fwrite(out_buf, 1, cur - out_buf, fp);
    }
    fclose(fp);
    _exit(0);
}
