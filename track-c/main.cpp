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

static const char DIGITS_LUT[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline char* write_u64(char* p, uint64_t v) {
    if (v < 10) { *p++ = '0' + v; return p; }
    char buf[24], *b = buf + 24;
    while (v >= 100) {
        uint64_t q = v / 100; uint32_t r = (uint32_t)(v - q * 100);
        val_assign: v = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2);
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

struct alignas(8) NameSlot {
    std::atomic<uint64_t> hash{0};
    std::atomic<uint64_t> seen[4]{{0}, {0}, {0}, {0}};
};

static constexpr size_t HASH_CAP = 1 << 18;
static constexpr size_t HASH_MASK = HASH_CAP - 1;

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
    bool seen[26 * 26 * 26] = {false};
    uint32_t unique_codes[256]; uint16_t num_countries = 0;
    const char* p = rptr;
    while (p < rend) {
        while (p < rend && *p != ',') p++;
        if (p >= rend) break;
        p++;
        uint16_t pack = (uint16_t)(((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A'));
        if (!seen[pack]) {
            seen[pack] = true;
            unique_codes[num_countries++] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        }
        p += 3; while (p < rend && *p != '\n') p++;
        if (p < rend && *p == '\n') p++;
    }
    std::sort(unique_codes, unique_codes + num_countries);
    uint8_t pack_to_id[26 * 26 * 26]; memset(pack_to_id, 0, sizeof(pack_to_id));
    for (uint16_t i = 0; i < num_countries; ++i) {
        uint32_t c = unique_codes[i];
        uint16_t pack = (uint16_t)((((c >> 16) & 0xff) - 'A') * 26 * 26 + (((c >> 8) & 0xff) - 'A') * 26 + ((c & 0xff) - 'A'));
        pack_to_id[pack] = (uint8_t)(i + 1);
    }
    p = rptr;
    while (p < rend) {
        uint32_t id = (uint8_t)(*p++ - '0');
        while ((uint8_t)(*p - '0') <= 9) id = id * 10 + (uint8_t)(*p++ - '0');
        p++;
        uint16_t pack = (uint16_t)(((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A'));
        if (id < 1000000) region_to_country[id] = pack_to_id[pack];
        p += 3; while (p < rend && *p != '\n') p++;
        if (p < rend && *p == '\n') p++;
    }
    mf_regions.close();

    NameSlot* name_table = (NameSlot*)::mmap(NULL, sizeof(NameSlot) * HASH_CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (name_table == MAP_FAILED) return 1;

    MappedFile mf_records;
    if (!mf_records.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;
    madvise((void*)mf_records.data, mf_records.size, MADV_WILLNEED | MADV_SEQUENTIAL);
    const char* ptr0 = mf_records.data, *file_end = mf_records.data + mf_records.size;
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) ptr0 += 3;
    while (ptr0 < file_end && *ptr0 != '\n') ptr0++;
    if (ptr0 < file_end && *ptr0 == '\n') ptr0++;

    unsigned int nthreads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (mf_records.size < 200000) nthreads = 1;
    Chunk chunks[16]; chunks[0].start = ptr0;
    if (nthreads == 1) chunks[0].end = file_end;
    else {
        __m256i v_cr = _mm256_set1_epi8('\r');
        for (unsigned int i = 1; i < nthreads; ++i) {
            const char* cur = mf_records.data + (mf_records.size * i) / nthreads;
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
        chunks[nthreads - 1].end = file_end;
    }

    uint8_t out_order[241];
    for (uint16_t i = 0; i < num_countries; ++i) out_order[i] = (uint8_t)(i + 1);
    out_order[num_countries] = 0;

    auto worker_fn = [&](unsigned int t) {
        const char* ptr = chunks[t].start, *chunk_end = chunks[t].end;
        auto& my_countries = tdata[t].countries;
        __m256i v_comma = _mm256_set1_epi8(','), v_quote = _mm256_set1_epi8('"');

        while (ptr < chunk_end) {
            if (__builtin_expect((uint8_t)*ptr == 0xef, 0)) {
                if (ptr + 3 <= chunk_end && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) ptr += 3;
            }
            uint64_t id = (uint8_t)(*ptr++ - '0');
            while ((uint8_t)(*ptr - '0') <= 9) id = id * 10 + (uint8_t)(*ptr++ - '0');
            ptr++;
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

            uint32_t region_id = (uint8_t)(*ptr++ - '0');
            while ((uint8_t)(*ptr - '0') <= 9) region_id = region_id * 10 + (uint8_t)(*ptr++ - '0');
            ptr++;
            bool neg = (*ptr == '-'); ptr += neg;
            int64_t cents = (uint8_t)(*ptr++ - '0');
            while ((uint8_t)(*ptr - '0') <= 9) cents = cents * 10 + (uint8_t)(*ptr++ - '0');
            ptr++; cents = cents * 100 + (uint8_t)(ptr[0] - '0') * 10 + (uint8_t)(ptr[1] - '0'); ptr += 2;
            if (neg) cents = -cents;
            if (__builtin_expect(ptr < chunk_end, 1)) { ptr += (*ptr == '\r'); ptr += (*ptr == '\n'); }

            uint8_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0;
            LocalCountryAgg& agg = my_countries[cid];
            if (__builtin_expect(agg.count == 0, 0)) { agg.amount_min = agg.amount_max = cents; }
            else { agg.amount_min = std::min(agg.amount_min, cents); agg.amount_max = std::max(agg.amount_max, cents); }
            agg.count++; agg.id_sum += id; agg.amount_sum += cents;
            agg.name_bytes += name_len; agg.note_bytes += note_len;

            uint64_t nh = name_hash ? name_hash : 1;
            size_t idx = (nh ^ (nh >> 16)) & HASH_MASK;
            NameSlot* entry = nullptr;
            while (true) {
                uint64_t cur = name_table[idx].hash.load(std::memory_order_relaxed);
                if (cur == nh) { entry = &name_table[idx]; break; }
                if (cur == 0) {
                    uint64_t expected = 0;
                    if (name_table[idx].hash.compare_exchange_strong(expected, nh, std::memory_order_relaxed)) {
                        entry = &name_table[idx]; break;
                    }
                    if (expected == nh) { entry = &name_table[idx]; break; }
                }
                idx = (idx + 1) & HASH_MASK;
            }
            uint32_t w = cid >> 6;
            uint64_t bit = 1ULL << (cid & 63);
            if (!(entry->seen[w].fetch_or(bit, std::memory_order_relaxed) & bit)) {
                agg.distinct_names++;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) workers.emplace_back(worker_fn, i);
    for (auto& w : workers) w.join();

    FILE* fp = fopen(out_path.string().c_str(), "wb");
    if (!fp) return 1;
    char out_buf[1024];
    for (uint16_t i = 0; i <= num_countries; ++i) {
        uint8_t cid = out_order[i];
        LocalCountryAgg total;
        bool any = false;
        for (unsigned int t = 0; t < nthreads; ++t) {
            const auto& src = tdata[t].countries[cid];
            if (src.count > 0) {
                if (!any) { total.amount_min = src.amount_min; total.amount_max = src.amount_max; any = true; }
                else { total.amount_min = std::min(total.amount_min, src.amount_min); total.amount_max = std::max(total.amount_max, src.amount_max); }
                total.count += src.count; total.id_sum += src.id_sum; total.amount_sum += src.amount_sum;
                total.name_bytes += src.name_bytes; total.note_bytes += src.note_bytes;
                total.distinct_names += src.distinct_names;
            }
        }
        if (!any) continue;
        char* cur = out_buf;
        if (cid == 0) { memcpy(cur, "_UNMATCHED ", 11); cur += 11; }
        else {
            uint32_t code = unique_codes[cid - 1];
            *cur++ = (char)((code >> 16) & 0xff); *cur++ = (char)((code >> 8) & 0xff); *cur++ = (char)(code & 0xff); *cur++ = ' ';
        }
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
