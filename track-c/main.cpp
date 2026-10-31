#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <barrier>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static const char DIGITS_LUT[] = 
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline char* write_u64(char* p, uint64_t val) {
    if (val < 10) { *p++ = '0' + val; return p; }
    char buf[24], *b = buf + 24;
    while (val >= 100) {
        uint64_t q = val / 100;
        uint32_t r = (uint32_t)(val - q * 100);
        val = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2);
    }
    if (val < 10) *--b = '0' + val;
    else { b -= 2; memcpy(b, &DIGITS_LUT[val * 2], 2); }
    size_t len = (buf + 24) - b; memcpy(p, b, len); return p + len;
}

inline char* write_i64(char* p, int64_t val) {
    if (val < 0) { *p++ = '-'; val = -val; }
    return write_u64(p, (uint64_t)val);
}

inline char* format_money(char* p, int64_t cents) {
    if (cents < 0) { *p++ = '-'; cents = -cents; }
    p = write_u64(p, (uint64_t)cents / 100); *p++ = '.';
    uint32_t frac = (uint32_t)((uint64_t)cents % 100);
    *p++ = '0' + (frac / 10); *p++ = '0' + (frac % 10);
    return p;
}

struct MappedFile {
    const char* data = nullptr; size_t size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMapping = NULL;
    bool open(const std::string& path) {
        hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fs; GetFileSizeEx(hFile, &fs); size = (size_t)fs.QuadPart;
        if (size == 0) return true;
        hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        data = (const char*)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        return data != nullptr;
    }
    ~MappedFile() { if (data) UnmapViewOfFile(data); if (hMapping) CloseHandle(hMapping); if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile); }
#else
    int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY); if (fd < 0) return false;
        struct stat st; fstat(fd, &st); size = (size_t)st.st_size; if (size == 0) return true;
        data = (const char*)::mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0); return data != MAP_FAILED;
    }
    ~MappedFile() { if (data && data != MAP_FAILED) munmap((void*)data, size); if (fd >= 0) ::close(fd); }
#endif
};


struct LocalCountryAgg {
    uint64_t count = 0; int64_t id_sum = 0, amount_sum = 0, amount_min = 0, amount_max = 0;
    uint64_t name_bytes = 0, note_bytes = 0;
};

struct ThreadData {
    LocalCountryAgg countries[241];
    std::vector<uint64_t> country_names[241];
    std::string out_buf;
};

static uint8_t region_to_country[1000000];
struct Chunk { const char* start = nullptr; const char* end = nullptr; };
struct RegionRecord { uint32_t rid; uint16_t pack; };

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::string data_dir = "/data", out_arg = "/out";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_arg = argv[++i];
    }

    std::filesystem::path out_path = out_arg.ends_with(".txt") ? std::filesystem::path(out_arg) : std::filesystem::path(out_arg) / "aggregates.txt";
    std::filesystem::create_directories(out_path.parent_path());

    MappedFile mf_regions;
    if (!mf_regions.open((std::filesystem::path(data_dir) / "regions.csv").string())) return 1;

    const char* rptr = mf_regions.data, *rend = mf_regions.data + mf_regions.size;
    while (rptr < rend && *rptr != '\n') rptr++;
    if (rptr < rend && *rptr == '\n') rptr++;

    std::vector<RegionRecord> reg_records; reg_records.reserve(200000);
    bool seen[26 * 26 * 26] = {false};
    std::vector<uint32_t> unique_codes; unique_codes.reserve(256);

    const char* p = rptr;
    while (p < rend) {
        uint32_t rid = 0; while (p < rend && *p >= '0') rid = rid * 10 + (*p++ - '0');
        if (p >= rend) break;
        p++;
        char c0 = p[0], c1 = p[1], c2 = p[2];
        uint16_t pack = (uint16_t)(((c0 - 'A') * 26 + (c1 - 'A')) * 26 + (c2 - 'A'));
        if (!seen[pack]) {
            seen[pack] = true;
            unique_codes.push_back(((uint32_t)c0 << 16) | ((uint32_t)c1 << 8) | (uint32_t)c2);
        }
        reg_records.push_back({rid, pack});
        p += 3;
        if (p < rend && *p == '\r') p++;
        if (p < rend && *p == '\n') p++;
    }

    std::sort(unique_codes.begin(), unique_codes.end());
    uint16_t num_countries = (uint16_t)unique_codes.size();
    std::string country_names[241];
    country_names[0] = "_UNMATCHED";
    uint8_t code_to_cid[26 * 26 * 26] = {0};

    for (uint16_t i = 0; i < num_countries; ++i) {
        uint32_t code = unique_codes[i];
        char c0 = (char)(code >> 16), c1 = (char)((code >> 8) & 0xFF), c2 = (char)(code & 0xFF);
        uint8_t cid = (uint8_t)(i + 1);
        country_names[cid] = std::string({c0, c1, c2});
        code_to_cid[(uint16_t)(((c0 - 'A') * 26 + (c1 - 'A')) * 26 + (c2 - 'A'))] = cid;
    }
    for (const auto& reg : reg_records) {
        if (reg.rid < 1000000) region_to_country[reg.rid] = code_to_cid[reg.pack];
    }

    MappedFile mf_records;
    if (!mf_records.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;

    const char* ptr0 = mf_records.data, *file_end = mf_records.data + mf_records.size;
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) ptr0 += 3;
    while (ptr0 < file_end && *ptr0 != '\n') ptr0++;
    if (ptr0 < file_end && *ptr0 == '\n') ptr0++;

    unsigned int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (mf_records.size < 200000) nthreads = 1;
    if (nthreads > 16) nthreads = 16;

    std::vector<Chunk> chunks(nthreads);
    chunks[0].start = ptr0;
    if (nthreads == 1) chunks[0].end = file_end;
    else {
        for (unsigned int i = 1; i < nthreads; ++i) {
            size_t approx = (mf_records.size * i) / nthreads;
            const char* cur = mf_records.data + approx;
            while (cur + 1 < file_end && !(cur[0] == '\r' && cur[1] == '\n')) cur++;
            cur = (cur + 1 < file_end) ? cur + 2 : file_end;
            chunks[i].start = cur; chunks[i - 1].end = cur;
        }
        chunks[nthreads - 1].end = file_end;
    }

    uint8_t out_order[241];
    for (uint16_t i = 0; i < num_countries; ++i) out_order[i] = (uint8_t)(i + 1);
    out_order[num_countries] = 0;

    std::vector<ThreadData> tdata(nthreads);
    std::barrier sync_point(nthreads);

    auto worker_fn = [&](unsigned int t) {
        const char* ptr = chunks[t].start, *chunk_end = chunks[t].end;
        auto& my_countries = tdata[t].countries;
        auto& my_names = tdata[t].country_names;

        while (ptr < chunk_end) {
            if (ptr + 3 <= chunk_end && (uint8_t)ptr[0] == 0xef && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) ptr += 3;

            uint64_t id = 0; while (*ptr >= '0') id = id * 10 + (*ptr++ - '0');
            ptr++;

            uint64_t name_hash = 0xcbf29ce484222325ULL;
            size_t name_len = 0;
            if (*ptr == '"') {
                ptr++;
                while (true) {
                    char c = *ptr++;
                    if (c == '"') {
                        if (*ptr == '"') { ptr++; name_hash = (name_hash ^ '"') * 0x100000001b3ULL; name_len++; }
                        else break;
                    } else { name_hash = (name_hash ^ (uint8_t)c) * 0x100000001b3ULL; name_len++; }
                }
                ptr++;
            } else {
                while (*ptr != ',') { name_hash = (name_hash ^ (uint8_t)*ptr++) * 0x100000001b3ULL; name_len++; }
                ptr++;
            }

            size_t note_len = 0;
            if (*ptr == '"') {
                ptr++;
                while (true) {
                    char c = *ptr++;
                    if (c == '"') {
                        if (*ptr == '"') { ptr++; note_len++; }
                        else break;
                    } else note_len++;
                }
                ptr++;
            } else {
                while (*ptr != ',') { note_len++; ptr++; }
                ptr++;
            }

            uint32_t region_id = 0; while (*ptr >= '0') region_id = region_id * 10 + (*ptr++ - '0');
            ptr++;

            bool neg = (*ptr == '-'); if (neg) ptr++;
            int64_t cents = 0; while (*ptr >= '0') cents = cents * 10 + (*ptr++ - '0');
            ptr++; cents = cents * 100 + (ptr[0] - '0') * 10 + (ptr[1] - '0');
            ptr += 2; if (neg) cents = -cents;

            if (ptr < chunk_end && *ptr == '\r') ptr++;
            if (ptr < chunk_end && *ptr == '\n') ptr++;

            uint8_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0;
            LocalCountryAgg& agg = my_countries[cid];
            agg.count++; agg.id_sum += (int64_t)id; agg.amount_sum += cents;
            if (agg.count == 1) { agg.amount_min = cents; agg.amount_max = cents; }
            else {
                if (cents < agg.amount_min) agg.amount_min = cents;
                if (cents > agg.amount_max) agg.amount_max = cents;
            }
            agg.name_bytes += name_len; agg.note_bytes += note_len;

            my_names[cid].push_back(name_hash);
        }

        sync_point.arrive_and_wait();

        uint16_t total_c = num_countries + 1;
        uint16_t idx_start = (uint16_t)(((uint32_t)total_c * t) / nthreads);
        uint16_t idx_end = (uint16_t)(((uint32_t)total_c * (t + 1)) / nthreads);

        char line_buf[256]; auto& out_buf = tdata[t].out_buf;
        out_buf.reserve((idx_end - idx_start) * 64);
        std::vector<uint64_t> all_names; all_names.reserve(4096);

        for (uint16_t idx = idx_start; idx < idx_end; ++idx) {
            uint8_t c = out_order[idx];
            uint64_t count = 0; int64_t id_sum = 0, amt_sum = 0, amt_min = 0, amt_max = 0;
            uint64_t name_bytes = 0, note_bytes = 0;
            bool first = true; all_names.clear();

            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                const auto& other = tdata[ot].countries[c];
                if (other.count == 0) continue;
                if (first) {
                    count = other.count; id_sum = other.id_sum; amt_sum = other.amount_sum;
                    amt_min = other.amount_min; amt_max = other.amount_max;
                    name_bytes = other.name_bytes; note_bytes = other.note_bytes; first = false;
                } else {
                    count += other.count; id_sum += other.id_sum; amt_sum += other.amount_sum;
                    if (other.amount_min < amt_min) amt_min = other.amount_min;
                    if (other.amount_max > amt_max) amt_max = other.amount_max;
                    name_bytes += other.name_bytes; note_bytes += other.note_bytes;
                }
                const auto& nms = tdata[ot].country_names[c];
                all_names.insert(all_names.end(), nms.begin(), nms.end());
            }
            if (count == 0) continue;

            std::sort(all_names.begin(), all_names.end());
            uint32_t distinct_names = (uint32_t)(std::unique(all_names.begin(), all_names.end()) - all_names.begin());

            char* p_out = line_buf;
            const std::string& name = country_names[c];
            memcpy(p_out, name.data(), name.size()); p_out += name.size(); *p_out++ = ' ';
            p_out = write_u64(p_out, count); *p_out++ = ' ';
            p_out = write_i64(p_out, id_sum); *p_out++ = ' ';
            p_out = format_money(p_out, amt_sum); *p_out++ = ' ';
            p_out = format_money(p_out, amt_min); *p_out++ = ' ';
            p_out = format_money(p_out, amt_max); *p_out++ = ' ';
            p_out = write_u64(p_out, distinct_names); *p_out++ = ' ';
            p_out = write_u64(p_out, name_bytes); *p_out++ = ' ';
            p_out = write_u64(p_out, note_bytes); *p_out++ = '\n';
            out_buf.append(line_buf, p_out - line_buf);
        }
    };

    std::vector<std::thread> workers; workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) workers.emplace_back(worker_fn, t);
    worker_fn(0);
    for (auto& w : workers) w.join();

    FILE* f = fopen(out_path.string().c_str(), "wb");
    if (!f) return 1;
    for (unsigned int t = 0; t < nthreads; ++t) {
        if (!tdata[t].out_buf.empty()) fwrite(tdata[t].out_buf.data(), 1, tdata[t].out_buf.size(), f);
    }
    fclose(f);
    return 0;
}
