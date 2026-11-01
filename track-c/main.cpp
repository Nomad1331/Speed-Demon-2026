#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <barrier>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <immintrin.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static const char DIGITS_LUT[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
inline char* write_u64(char* p, uint64_t v) {
    if (v < 10) { *p++ = '0' + v; return p; }
    char buf[24], *b = buf + 24;
    while (v >= 100) { uint64_t q = v / 100; uint32_t r = (uint32_t)(v - q * 100); v = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2); }
    if (v < 10) *--b = '0' + v; else { b -= 2; memcpy(b, &DIGITS_LUT[v * 2], 2); }
    size_t len = (buf + 24) - b; memcpy(p, b, len); return p + len;
}
inline char* write_i64(char* p, int64_t v) { if (v < 0) { *p++ = '-'; v = -v; } return write_u64(p, (uint64_t)v); }
inline char* format_money(char* p, int64_t c) {
    if (c < 0) { *p++ = '-'; c = -c; }
    p = write_u64(p, (uint64_t)c / 100); *p++ = '.';
    uint32_t frac = (uint32_t)((uint64_t)c % 100);
    *p++ = '0' + (frac / 10); *p++ = '0' + (frac % 10); return p;
}

inline uint32_t hash_bytes_crc32(const char* s, size_t len) {
    uint32_t h = (uint32_t)len;
    while (len >= 8) { uint64_t v; memcpy(&v, s, 8); h = (uint32_t)_mm_crc32_u64(h, v); s += 8; len -= 8; }
    if (len >= 4) { uint32_t v; memcpy(&v, s, 4); h = _mm_crc32_u32(h, v); s += 4; len -= 4; }
    while (len > 0) { h = _mm_crc32_u8(h, (uint8_t)*s++); len--; }
    return h;
}

struct MappedFile {
    const char* data = nullptr; size_t size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = NULL;
    bool open(const std::string& path) {
        hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fs; GetFileSizeEx(hFile, &fs); size = (size_t)fs.QuadPart; if (!size) return true;
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        return (data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0)) != nullptr;
    }
    ~MappedFile() { if (data) UnmapViewOfFile(data); if (hMap) CloseHandle(hMap); if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile); }
#else
    int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY); if (fd < 0) return false;
        struct stat st; fstat(fd, &st); size = (size_t)st.st_size; if (!size) return true;
        data = (const char*)::mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0); return data != MAP_FAILED;
    }
    ~MappedFile() { if (data && data != MAP_FAILED) munmap((void*)data, size); if (fd >= 0) ::close(fd); }
#endif
};

struct LocalCountryAgg { uint64_t count = 0, name_bytes = 0, note_bytes = 0; int64_t id_sum = 0, amount_sum = 0, amount_min = 0, amount_max = 0; };
struct ThreadData { LocalCountryAgg countries[241]; std::vector<uint64_t> my_names; uint32_t c_offset[242]; std::string out_buf; };
static uint8_t region_to_country[1000000];
struct Chunk { const char* start = nullptr; const char* end = nullptr; };

int main(int argc, char* argv[]) {
    std::string data_dir = "/data", out_arg = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_arg = argv[++i];
    }
    std::filesystem::path out_path = out_arg.ends_with(".txt") ? std::filesystem::path(out_arg) : std::filesystem::path(out_arg) / "aggregates.txt";
    std::filesystem::create_directories(out_path.parent_path());
    MappedFile mf_regions; if (!mf_regions.open((std::filesystem::path(data_dir) / "regions.csv").string())) return 1;
    const char* rptr = mf_regions.data, *rend = mf_regions.data + mf_regions.size;
    while (rptr < rend && *rptr != '\n') rptr++;
    if (rptr < rend && *rptr == '\n') rptr++;

    bool seen[26 * 26 * 26] = {false};
    std::vector<uint32_t> unique_codes; unique_codes.reserve(256);
    const char* p = rptr;
    while (p < rend) {
        while (p < rend && *p != ',') p++;
        if (p >= rend) break;
        p++;
        char c0 = p[0], c1 = p[1], c2 = p[2];
        uint16_t pack = (uint16_t)(((c0 - 'A') * 26 + (c1 - 'A')) * 26 + (c2 - 'A'));
        if (!seen[pack]) { seen[pack] = true; unique_codes.push_back(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2]); }
        p += 3; while (p < rend && *p != '\n') p++;
        if (p < rend && *p == '\n') p++;
    }
    std::sort(unique_codes.begin(), unique_codes.end());
    uint16_t num_countries = (uint16_t)unique_codes.size();
    std::string country_names[241];
    country_names[0] = "_UNMATCHED";
    uint8_t code_to_cid[26 * 26 * 26] = {0};
    for (uint16_t i = 0; i < num_countries; ++i) {
        uint32_t cd = unique_codes[i]; char c0 = (char)(cd >> 16), c1 = (char)((cd >> 8) & 0xFF), c2 = (char)(cd & 0xFF);
        country_names[i + 1] = std::string({c0, c1, c2});
        code_to_cid[(uint16_t)(((c0 - 'A') * 26 + (c1 - 'A')) * 26 + (c2 - 'A'))] = (uint8_t)(i + 1);
    }
    p = rptr;
    while (p < rend) {
        uint32_t rid = 0; while (p < rend && *p >= '0') rid = rid * 10 + (*p++ - '0');
        if (p >= rend) break;
        p++;
        uint16_t pack = (uint16_t)(((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A'));
        if (rid < 1000000) region_to_country[rid] = code_to_cid[pack];
        p += 3; while (p < rend && *p != '\n') p++;
        if (p < rend && *p == '\n') p++;
    }
#ifndef _WIN32
    if (mf_regions.size > 0) madvise((void*)mf_regions.data, mf_regions.size, MADV_DONTNEED);
#endif

    MappedFile mf_records; if (!mf_records.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;
    const char* ptr0 = mf_records.data, *file_end = mf_records.data + mf_records.size;
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) ptr0 += 3;
    while (ptr0 < file_end && *ptr0 != '\n') ptr0++; if (ptr0 < file_end && *ptr0 == '\n') ptr0++;

    unsigned int nthreads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u); if (mf_records.size < 200000) nthreads = 1;
    std::vector<Chunk> chunks(nthreads);
    chunks[0].start = ptr0;
    if (nthreads == 1) chunks[0].end = file_end;
    else {
        __m256i v_cr = _mm256_set1_epi8('\r');
        for (unsigned int i = 1; i < nthreads; ++i) {
            size_t approx = (mf_records.size * i) / nthreads;
            const char* cur = mf_records.data + approx;
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

    uint8_t out_order[241]; for (uint16_t i = 0; i < num_countries; ++i) out_order[i] = (uint8_t)(i + 1); out_order[num_countries] = 0;
    std::vector<ThreadData> tdata(nthreads); std::barrier sync_point(nthreads);

    auto worker_fn = [&](unsigned int t) {
        const char* ptr = chunks[t].start, *chunk_end = chunks[t].end, *last_evicted = ptr;
        auto& my_countries = tdata[t].countries;
        auto& my_names = tdata[t].my_names;
        my_names.reserve((chunk_end - ptr) / 45 + 1024);
        char unesc[256];
        __m256i v_comma = _mm256_set1_epi8(','), v_quote = _mm256_set1_epi8('"');

        while (ptr < chunk_end) {
            if (__builtin_expect(ptr - last_evicted >= 524288, 0)) {
#ifndef _WIN32
                uintptr_t s = ((uintptr_t)last_evicted + 4095ULL) & ~4095ULL, e = ((uintptr_t)ptr) & ~4095ULL;
                if (e > s) madvise((void*)s, e - s, MADV_DONTNEED);
#endif
                last_evicted = ptr;
            }
            if (ptr + 3 <= chunk_end && (uint8_t)ptr[0] == 0xef && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) ptr += 3;

            uint64_t id = 0; while (*ptr >= '0') id = id * 10 + (*ptr++ - '0'); ptr++;
            uint32_t name_hash; size_t name_len;
            if (__builtin_expect(*ptr != '"', 1)) {
                const char* start = ptr;
                while (*ptr != ',') ptr++;
                name_len = (size_t)(ptr - start); ptr++;
                name_hash = hash_bytes_crc32(start, name_len);
            } else {
                ptr++; char* up = unesc;
                while (true) {
                    char c = *ptr++;
                    if (__builtin_expect(c != '"', 1)) *up++ = c;
                    else { if (*ptr == '"') { ptr++; *up++ = '"'; } else break; }
                }
                ptr++;
                name_len = (size_t)(up - unesc);
                name_hash = hash_bytes_crc32(unesc, name_len);
            }

            size_t note_len = 0;
            if (__builtin_expect(*ptr != '"', 1)) {
                const char* start = ptr;
                while (ptr + 32 <= chunk_end) {
                    __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
                    uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_comma));
                    if (m) { ptr += __builtin_ctz(m); break; }
                    ptr += 32;
                }
                while (*ptr != ',') ptr++;
                note_len = (size_t)(ptr - start); ptr++;
            } else {
                ptr++;
                while (true) {
                    while (ptr + 32 <= chunk_end) {
                        __m256i v = _mm256_loadu_si256((const __m256i*)ptr);
                        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_quote));
                        if (m) { note_len += __builtin_ctz(m); ptr += __builtin_ctz(m); break; }
                        note_len += 32; ptr += 32;
                    }
                    char c = *ptr++;
                    if (__builtin_expect(c != '"', 1)) note_len++;
                    else { if (*ptr == '"') { ptr++; note_len++; } else break; }
                }
                ptr++;
            }

            uint32_t region_id = 0; while (*ptr >= '0') region_id = region_id * 10 + (*ptr++ - '0');
            ptr++; bool neg = (*ptr == '-'); if (neg) ptr++;
            int64_t cents = 0; while (*ptr >= '0') cents = cents * 10 + (*ptr++ - '0');
            ptr++; cents = cents * 100 + (ptr[0] - '0') * 10 + (ptr[1] - '0'); ptr += 2; if (neg) cents = -cents;
            if (ptr < chunk_end && *ptr == '\r') ptr++; if (ptr < chunk_end && *ptr == '\n') ptr++;

            uint8_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0;
            LocalCountryAgg& agg = my_countries[cid];
            agg.count++; agg.id_sum += (int64_t)id; agg.amount_sum += cents;
            if (agg.count == 1) { agg.amount_min = agg.amount_max = cents; }
            else { agg.amount_min = std::min(agg.amount_min, cents); agg.amount_max = std::max(agg.amount_max, cents); }
            agg.name_bytes += name_len; agg.note_bytes += note_len;
            my_names.push_back(((uint64_t)cid << 32) | name_hash);
        }

        std::sort(my_names.begin(), my_names.end());
        my_names.erase(std::unique(my_names.begin(), my_names.end()), my_names.end());
        my_names.shrink_to_fit();
        size_t ucount = my_names.size();

        auto& c_offset = tdata[t].c_offset; memset(c_offset, 0, sizeof(c_offset));
        for (size_t i = 0; i < ucount; ++i) c_offset[(my_names[i] >> 32) + 1]++;
        for (int i = 1; i <= 241; ++i) c_offset[i] += c_offset[i - 1];

#ifndef _WIN32
        uintptr_t s = ((uintptr_t)last_evicted + 4095ULL) & ~4095ULL, e = ((uintptr_t)chunk_end) & ~4095ULL;
        if (e > s) madvise((void*)s, e - s, MADV_DONTNEED);
#endif
        sync_point.arrive_and_wait();

        uint16_t total_c = num_countries + 1, idx_start = (uint16_t)(((uint32_t)total_c * t) / nthreads), idx_end = (uint16_t)(((uint32_t)total_c * (t + 1)) / nthreads);
        char line_buf[256]; auto& out_buf = tdata[t].out_buf; out_buf.reserve((idx_end - idx_start) * 64);
        std::vector<uint32_t> all_names; all_names.reserve(2048);

        for (uint16_t idx = idx_start; idx < idx_end; ++idx) {
            uint8_t c = out_order[idx];
            uint64_t count = 0, name_bytes = 0, note_bytes = 0; int64_t id_sum = 0, amt_sum = 0, amt_min = INT64_MAX, amt_max = INT64_MIN;
            all_names.clear();
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                const auto& o = tdata[ot].countries[c];
                if (!o.count) continue;
                count += o.count; id_sum += o.id_sum; amt_sum += o.amount_sum;
                amt_min = std::min(amt_min, o.amount_min); amt_max = std::max(amt_max, o.amount_max);
                name_bytes += o.name_bytes; note_bytes += o.note_bytes;
                size_t start = tdata[ot].c_offset[c], end = tdata[ot].c_offset[c + 1];
                for (size_t k = start; k < end; ++k) all_names.push_back((uint32_t)tdata[ot].my_names[k]);
            }
            if (!count) continue;

            std::sort(all_names.begin(), all_names.end());
            uint32_t distinct_names = (uint32_t)(std::unique(all_names.begin(), all_names.end()) - all_names.begin());

            char* p_out = line_buf; const std::string& name = country_names[c];
            memcpy(p_out, name.data(), name.size()); p_out += name.size(); *p_out++ = ' ';
            p_out = write_u64(p_out, count); *p_out++ = ' '; p_out = write_i64(p_out, id_sum); *p_out++ = ' ';
            p_out = format_money(p_out, amt_sum); *p_out++ = ' '; p_out = format_money(p_out, amt_min); *p_out++ = ' ';
            p_out = format_money(p_out, amt_max); *p_out++ = ' '; p_out = write_u64(p_out, distinct_names); *p_out++ = ' ';
            p_out = write_u64(p_out, name_bytes); *p_out++ = ' '; p_out = write_u64(p_out, note_bytes); *p_out++ = '\n';
            out_buf.append(line_buf, p_out - line_buf);
        }
    };

    std::vector<std::thread> workers; workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) workers.emplace_back(worker_fn, t);
    worker_fn(0); for (auto& w : workers) w.join();
    FILE* f = fopen(out_path.string().c_str(), "wb"); if (!f) return 1;
    for (unsigned int t = 0; t < nthreads; ++t) if (!tdata[t].out_buf.empty()) fwrite(tdata[t].out_buf.data(), 1, tdata[t].out_buf.size(), f);
    fclose(f); return 0;
}
