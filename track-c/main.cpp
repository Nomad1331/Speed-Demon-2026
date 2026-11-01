#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <immintrin.h>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
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

// SWAR 8-digit parser
static inline uint32_t digits8(uint64_t w) noexcept {
    w = ((w & 0x0F0F0F0F0F0F0F0FULL) * 2561) >> 8;
    w = ((w & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;
    return static_cast<uint32_t>(((w & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32);
}

static inline uint32_t read_uint32(const char*& p) noexcept {
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
    for (p += 8; *p >= '0' && *p <= '9'; ++p) n = n * 10u + static_cast<uint32_t>(*p - '0');
    return n;
}

static inline int64_t read_cents(const char*& p) noexcept {
    const bool neg = (*p == '-'); if (neg) ++p;
    const int64_t whole = read_uint32(p);
    ++p; // skip '.'
    const int64_t cents = whole * 100 + int64_t(p[0] - '0') * 10 + int64_t(p[1] - '0');
    p += 2;
    return neg ? -cents : cents;
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

// 262,144 slots (2 MB table) -> 38% load factor, 99% hit on first probe!
static constexpr u32 BITS = 18, SLOTS = 1 << BITS;
alignas(64) static std::atomic<u64> g_slots[SLOTS];

// Country bitsets: 256 countries * 4096 words (8 MB total)
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

struct ThreadData { LocalCountryAgg countries[241]; };
static ThreadData tdata[16];
static uint8_t region_to_country[1000000];
struct Chunk { const char* start = nullptr; const char* end = nullptr; };

int main(int argc, char* argv[]) {
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);

    std::string data_dir = "/data", out_arg = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_arg = argv[++i];
    }
    std::filesystem::path out_path = out_arg.ends_with(".txt") ? std::filesystem::path(out_arg) : std::filesystem::path(out_arg) / "aggregates.txt";
    std::filesystem::create_directories(out_path.parent_path());

    MappedFile mf_regions;
    if (!mf_regions.open((std::filesystem::path(data_dir) / "regions.csv").string())) return 1;
    const char* rptr = mf_regions.data, *rend = mf_regions.data + mf_regions.size;
    while (rptr < rend && *rptr != '\n') rptr++;
    if (rptr < rend && *rptr == '\n') rptr++;

    struct RegPair { uint32_t rid; uint16_t pack; };
    std::vector<RegPair> r_pairs; r_pairs.reserve(200000);
    bool seen[26 * 26 * 26] = {false};
    uint32_t unique_codes[256]; uint16_t num_countries = 0;
    const char* p = rptr;
    while (p < rend) {
        uint32_t rid = read_uint32(p);
        if (p < rend && *p == ',') p++;
        if (p >= rend) break;
        uint16_t pack = (uint16_t)(((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A'));
        if (!seen[pack]) {
            seen[pack] = true;
            unique_codes[num_countries++] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        }
        r_pairs.push_back({rid, pack});
        p += 3;
        while (p < rend && *p != '\n') p++;
        if (p < rend && *p == '\n') p++;
    }
    mf_regions.close();

    std::sort(unique_codes, unique_codes + num_countries);
    uint8_t pack_to_id[26 * 26 * 26]; memset(pack_to_id, 0, sizeof(pack_to_id));
    for (uint16_t i = 0; i < num_countries; ++i) {
        uint32_t c = unique_codes[i];
        uint16_t pack = (uint16_t)((((c >> 16) & 0xff) - 'A') * 26 * 26 + (((c >> 8) & 0xff) - 'A') * 26 + ((c & 0xff) - 'A'));
        pack_to_id[pack] = (uint8_t)(i + 1);
    }
    for (const auto& rp : r_pairs) {
        if (rp.rid < 1000000) region_to_country[rp.rid] = pack_to_id[rp.pack];
    }

    MappedFile mf_records;
    if (!mf_records.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;
    const char* ptr0 = mf_records.data, *file_end = mf_records.data + mf_records.size;
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) ptr0 += 3;
    while (ptr0 < file_end && *ptr0 != '\n') ptr0++;
    if (ptr0 < file_end && *ptr0 == '\n') ptr0++;

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

    uint8_t out_order[241];
    for (uint16_t i = 0; i < num_countries; ++i) out_order[i] = (uint8_t)(i + 1);
    out_order[num_countries] = 0;

    std::atomic<unsigned int> piece_idx{0};

    auto worker_fn = [&](unsigned int t) {
        auto& my_countries = tdata[t].countries;
        __m256i v_comma = _mm256_set1_epi8(','), v_quote = _mm256_set1_epi8('"');

        for (;;) {
            unsigned int p_idx = piece_idx.fetch_add(1, std::memory_order_relaxed);
            if (p_idx >= num_pieces) break;

            const char* ptr = chunks[p_idx].start, *chunk_end = chunks[p_idx].end;

            while (ptr < chunk_end) {
                _mm_prefetch(ptr + 2048, _MM_HINT_T0);
                if (__builtin_expect((uint8_t)*ptr == 0xef, 0)) {
                    if (ptr + 3 <= chunk_end && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) ptr += 3;
                }
                uint64_t id = read_uint32(ptr);
                if (ptr < chunk_end && *ptr == ',') ptr++;

                uint32_t crc = 0; size_t name_len = 0;
                if (__builtin_expect(*ptr == '"', 1)) {
                    ptr++;
                    if (ptr + 33 <= chunk_end) {
                        __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
                        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_quote));
                        if (mask) {
                            int idx = __builtin_ctz(mask);
                            if (ptr[idx + 1] == ',') {
                                name_len = (size_t)idx;
                                const char* cp = ptr;
                                while (idx >= 8) {
                                    crc = (uint32_t)_mm_crc32_u64(crc, *(const uint64_t*)cp);
                                    cp += 8; idx -= 8;
                                }
                                if (idx >= 4) {
                                    crc = _mm_crc32_u32(crc, *(const uint32_t*)cp);
                                    cp += 4; idx -= 4;
                                }
                                while (idx > 0) {
                                    crc = _mm_crc32_u8(crc, (uint8_t)*cp++);
                                    idx--;
                                }
                                ptr += name_len + 2;
                                goto name_done;
                            }
                        }
                    }
                    while (true) {
                        char c = *ptr++;
                        if (__builtin_expect(c != '"', 1)) { crc = _mm_crc32_u8(crc, (uint8_t)c); name_len++; }
                        else if (*ptr == '"') { ptr++; crc = _mm_crc32_u8(crc, '"'); name_len++; }
                        else break;
                    }
                    ptr++;
                } else {
                    while (*ptr != ',') { crc = _mm_crc32_u8(crc, (uint8_t)*ptr++); name_len++; }
                    ptr++;
                }
            name_done:
                uint64_t name_hash = ((uint64_t)name_len << 32) | crc;

                size_t note_len = 0;
                if (__builtin_expect(*ptr == '"', 1)) {
                    ptr++;
                    if (ptr + 33 <= chunk_end) {
                        __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
                        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_quote));
                        if (m) {
                            int idx = __builtin_ctz(m);
                            if (ptr[idx + 1] == ',') {
                                note_len = (size_t)idx;
                                ptr += idx + 2;
                                goto note_done;
                            }
                        }
                    }
                    while (true) {
                        while (ptr + 32 <= chunk_end) {
                            __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
                            uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_quote));
                            if (m) { note_len += __builtin_ctz(m); ptr += __builtin_ctz(m); break; }
                            note_len += 32; ptr += 32;
                        }
                        char c = *ptr++;
                        if (__builtin_expect(c != '"', 1)) note_len++;
                        else if (*ptr == '"') { ptr++; note_len++; }
                        else break;
                    }
                    ptr++;
                } else {
                    const char* start = ptr;
                    while (ptr + 32 <= chunk_end) {
                        __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
                        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_comma));
                        if (m) { ptr += __builtin_ctz(m); break; }
                        ptr += 32;
                    }
                    while (*ptr != ',') ptr++;
                    note_len = (size_t)(ptr - start); ptr++;
                }
            note_done:

                uint32_t region_id = read_uint32(ptr);
                if (ptr < chunk_end && *ptr == ',') ptr++;

                int64_t cents = read_cents(ptr);
                if (__builtin_expect(ptr < chunk_end, 1)) { ptr += (*ptr == '\r'); ptr += (*ptr == '\n'); }

                uint8_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0;
                LocalCountryAgg& agg = my_countries[cid];
                if (__builtin_expect(agg.count == 0, 0)) { agg.amount_min = agg.amount_max = cents; }
                else { agg.amount_min = std::min(agg.amount_min, cents); agg.amount_max = std::max(agg.amount_max, cents); }
                agg.count++; agg.id_sum += id; agg.amount_sum += cents;
                agg.name_bytes += name_len; agg.note_bytes += note_len;

                uint64_t nh = name_hash ? name_hash : 1;
                u32 name_id = get_name_id(nh);
                if (mark_seen(cid, name_id)) {
                    agg.distinct_names++;
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) workers.emplace_back(worker_fn, i);
    for (auto& w : workers) w.join();

    LocalCountryAgg final_countries[241];
    for (unsigned int t = 0; t < nthreads; ++t) {
        for (int c = 0; c < 241; ++c) {
            auto& src = tdata[t].countries[c];
            if (!src.count) continue;
            auto& dst = final_countries[c];
            if (dst.count == 0) { dst.amount_min = src.amount_min; dst.amount_max = src.amount_max; }
            else { dst.amount_min = std::min(dst.amount_min, src.amount_min); dst.amount_max = std::max(dst.amount_max, src.amount_max); }
            dst.count += src.count; dst.id_sum += src.id_sum; dst.amount_sum += src.amount_sum;
            dst.distinct_names += src.distinct_names;
            dst.name_bytes += src.name_bytes; dst.note_bytes += src.note_bytes;
        }
    }

    char* out_buf = (char*)malloc(256 * 1024);
    char* optr = out_buf;
    for (uint16_t i = 0; i <= num_countries; ++i) {
        uint8_t cid = out_order[i];
        const auto& c = final_countries[cid];
        if (!c.count) continue;
        if (cid == 0) { memcpy(optr, "_UNMATCHED ", 11); optr += 11; }
        else {
            uint32_t code = unique_codes[cid - 1];
            optr[0] = (char)((code >> 16) & 0xff); optr[1] = (char)((code >> 8) & 0xff); optr[2] = (char)(code & 0xff); optr[3] = ' ';
            optr += 4;
        }
        optr = write_u64(optr, c.count); *optr++ = ' ';
        optr = write_u64(optr, c.id_sum); *optr++ = ' ';
        optr = format_money(optr, c.amount_sum); *optr++ = ' ';
        optr = format_money(optr, c.amount_min); *optr++ = ' ';
        optr = format_money(optr, c.amount_max); *optr++ = ' ';
        optr = write_u64(optr, c.distinct_names); *optr++ = ' ';
        optr = write_u64(optr, c.name_bytes); *optr++ = ' ';
        optr = write_u64(optr, c.note_bytes); *optr++ = '\n';
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
