#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <thread>
#include <barrier>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>

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

// Fast 2-digit integer formatting lookup table
static const char DIGITS_LUT[] = 
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline char* write_u64_fast(char* p, uint64_t val) {
    if (val < 10) {
        *p++ = (char)('0' + val);
        return p;
    }
    char buf[24];
    char* b = buf + 24;
    while (val >= 100) {
        uint64_t q = val / 100;
        uint32_t r = (uint32_t)(val - q * 100);
        val = q;
        b -= 2;
        memcpy(b, &DIGITS_LUT[r * 2], 2);
    }
    if (val < 10) {
        *--b = (char)('0' + val);
    } else {
        b -= 2;
        memcpy(b, &DIGITS_LUT[val * 2], 2);
    }
    size_t len = (buf + 24) - b;
    memcpy(p, b, len);
    return p + len;
}

inline char* write_i64_fast(char* p, int64_t val) {
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    return write_u64_fast(p, (uint64_t)val);
}

inline char* format_money(char* p, int64_t cents) {
    if (cents < 0) {
        *p++ = '-';
        cents = -cents;
    }
    uint64_t whole = (uint64_t)cents / 100;
    uint32_t frac = (uint32_t)((uint64_t)cents % 100);
    p = write_u64_fast(p, whole);
    *p++ = '.';
    *p++ = (char)('0' + (frac / 10));
    *p++ = (char)('0' + (frac % 10));
    return p;
}

struct MappedFile {
    const char* data = nullptr;
    size_t size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapping = NULL;
#else
    int fd = -1;
#endif

    bool open(const std::string& path) {
#ifdef _WIN32
        hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fs;
        if (!GetFileSizeEx(hFile, &fs)) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false; }
        size = (size_t)fs.QuadPart;
        if (size == 0) return true;
        hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMapping) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; return false; }
        data = (const char*)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        return data != nullptr;
#else
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (fstat(fd, &st) < 0) { ::close(fd); fd = -1; return false; }
        size = (size_t)st.st_size;
        if (size == 0) return true;
        void* p = ::mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); fd = -1; return false; }
        data = (const char*)p;
#ifdef MADV_SEQUENTIAL
        madvise((void*)p, size, MADV_SEQUENTIAL);
#endif
        return true;
#endif
    }

    ~MappedFile() {
#ifdef _WIN32
        if (data) UnmapViewOfFile(data);
        if (hMapping) CloseHandle(hMapping);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
#else
        if (data && data != MAP_FAILED) munmap((void*)data, size);
        if (fd >= 0) ::close(fd);
#endif
    }
};

struct DynamicHashSet {
    uint64_t* entries = nullptr;
    uint32_t cap = 0;
    uint32_t count = 0;

    void init(uint32_t initial_cap = 64) {
        cap = initial_cap;
        count = 0;
        entries = (uint64_t*)calloc(cap, sizeof(uint64_t));
    }

    DynamicHashSet() = default;
    DynamicHashSet(const DynamicHashSet&) = delete;
    DynamicHashSet& operator=(const DynamicHashSet&) = delete;
    DynamicHashSet(DynamicHashSet&& o) noexcept : entries(o.entries), cap(o.cap), count(o.count) {
        o.entries = nullptr; o.cap = 0; o.count = 0;
    }
    DynamicHashSet& operator=(DynamicHashSet&& o) noexcept {
        if (this != &o) {
            if (entries) free(entries);
            entries = o.entries; cap = o.cap; count = o.count;
            o.entries = nullptr; o.cap = 0; o.count = 0;
        }
        return *this;
    }

    ~DynamicHashSet() {
        if (entries) free(entries);
    }

    inline void insert(uint64_t val) {
        if (val == 0) val = 1;
        if (cap == 0) {
            init(64);
        } else if (count * 2 >= cap) {
            grow();
        }
        uint32_t mask = cap - 1;
        uint32_t idx = (uint32_t)(val ^ (val >> 32)) & mask;
        while (entries[idx] != 0) {
            if (entries[idx] == val) return;
            idx = (idx + 1) & mask;
        }
        entries[idx] = val;
        count++;
    }

    void grow() {
        uint32_t new_cap = (cap == 0) ? 64 : cap * 2;
        uint64_t* new_entries = (uint64_t*)calloc(new_cap, sizeof(uint64_t));
        uint32_t mask = new_cap - 1;
        for (uint32_t i = 0; i < cap; ++i) {
            uint64_t val = entries[i];
            if (val != 0) {
                uint32_t idx = (uint32_t)(val ^ (val >> 32)) & mask;
                while (new_entries[idx] != 0) {
                    idx = (idx + 1) & mask;
                }
                new_entries[idx] = val;
            }
        }
        free(entries);
        entries = new_entries;
        cap = new_cap;
    }
};

struct LocalCountryAgg {
    uint64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    DynamicHashSet names;
};

struct MergedCountryAgg {
    uint64_t count = 0;
    int64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    DynamicHashSet names;
};

// 0 is UNMATCHED, 1..num_countries are valid countries
static uint16_t region_to_country[10000000];

struct Chunk {
    const char* start = nullptr;
    const char* end = nullptr;
};

struct ThreadData {
    std::vector<LocalCountryAgg> local_countries;
    std::string out_buf;
};

struct RegionRecord {
    uint32_t rid;
    uint16_t pack;
};

// Speed Demon 2026 - Track C: Join
// High-performance zero-dependency C++20 engine

int main(int argc, char* argv[]) {
#ifdef PROFILE
    auto t0 = std::chrono::steady_clock::now();
#endif
    std::ios_base::sync_with_stdio(false);

    std::string data_dir = "/data";
    std::string out_arg = "/out";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_arg = argv[++i];
        }
    }

    std::filesystem::path out_path;
    if (out_arg.ends_with(".txt")) {
        out_path = out_arg;
        std::filesystem::create_directories(out_path.parent_path());
    } else {
        out_path = std::filesystem::path(out_arg) / "aggregates.txt";
        std::filesystem::create_directories(out_arg);
    }

    std::filesystem::path data_p(data_dir);
    std::filesystem::path records_path = data_p / "records.csv";
    std::filesystem::path regions_path = data_p / "regions.csv";

    MappedFile mf_regions;
    if (!mf_regions.open(regions_path.string())) {
        std::cerr << "Failed to open " << regions_path << "\n";
        return 1;
    }

    // Single-pass parse of regions.csv
    const char* rptr = mf_regions.data;
    const char* rend = mf_regions.data + mf_regions.size;

    // Skip header line "region_id,country\n"
    while (rptr < rend && *rptr != '\n') rptr++;
    if (rptr < rend && *rptr == '\n') rptr++;

    std::vector<RegionRecord> reg_records;
    reg_records.reserve(200000);

    bool seen[26 * 26 * 26] = {false};
    std::vector<uint32_t> unique_codes;
    unique_codes.reserve(300);

    const char* p = rptr;
    while (p < rend) {
        uint32_t rid = 0;
        while (p < rend && *p >= '0' && *p <= '9') {
            rid = rid * 10 + (*p++ - '0');
        }
        if (p >= rend) break;
        p++; // skip ','
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

    // 0 is _UNMATCHED, 1..num_countries are sorted country codes
    std::vector<std::string> country_names(num_countries + 1);
    country_names[0] = "_UNMATCHED";

    uint16_t code_to_cid[26 * 26 * 26];
    memset(code_to_cid, 0, sizeof(code_to_cid));

    for (uint16_t i = 0; i < num_countries; ++i) {
        uint32_t code = unique_codes[i];
        char c0 = (char)(code >> 16);
        char c1 = (char)((code >> 8) & 0xFF);
        char c2 = (char)(code & 0xFF);
        uint16_t cid = i + 1;
        country_names[cid] = std::string({c0, c1, c2});
        uint16_t pack = (uint16_t)(((c0 - 'A') * 26 + (c1 - 'A')) * 26 + (c2 - 'A'));
        code_to_cid[pack] = cid;
    }

    for (const auto& reg : reg_records) {
        if (reg.rid < 10000000) {
            region_to_country[reg.rid] = code_to_cid[reg.pack];
        }
    }

#ifdef PROFILE
    auto t_regions = std::chrono::steady_clock::now();
#endif

    // Step 2: Parse records.csv
    MappedFile mf_records;
    if (!mf_records.open(records_path.string())) {
        std::cerr << "Failed to open " << records_path << "\n";
        return 1;
    }

    const char* ptr0 = mf_records.data;
    const char* file_end = mf_records.data + mf_records.size;

    // Optional leading BOM
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) {
        ptr0 += 3;
    }

    // Skip header line
    while (ptr0 < file_end && *ptr0 != '\n') ptr0++;
    if (ptr0 < file_end && *ptr0 == '\n') ptr0++;

    unsigned int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (mf_records.size < 200000) nthreads = 1;
    if (nthreads > 16) nthreads = 16;

    std::vector<Chunk> chunks(nthreads);
    chunks[0].start = ptr0;

    if (nthreads == 1) {
        chunks[0].end = file_end;
    } else {
        for (unsigned int i = 1; i < nthreads; ++i) {
            size_t approx = (mf_records.size * i) / nthreads;
            const char* cur = mf_records.data + approx;
            while (cur + 1 < file_end && !(cur[0] == '\r' && cur[1] == '\n')) {
                cur++;
            }
            if (cur + 1 < file_end) {
                cur += 2; // skip CRLF
            } else {
                cur = file_end;
            }
            chunks[i].start = cur;
            chunks[i - 1].end = cur;
        }
        chunks[nthreads - 1].end = file_end;
    }

    // Output order: 1..num_countries (alphabetical), then 0 (_UNMATCHED)
    std::vector<uint16_t> output_order(num_countries + 1);
    for (uint16_t i = 0; i < num_countries; ++i) {
        output_order[i] = i + 1;
    }
    output_order[num_countries] = 0;

    std::vector<ThreadData> tdata(nthreads);
    for (unsigned int t = 0; t < nthreads; ++t) {
        tdata[t].local_countries.resize(num_countries + 1);
    }

    std::vector<MergedCountryAgg> merged_countries(num_countries + 1);
    std::barrier sync_point(nthreads);

    auto worker_fn = [&](unsigned int t) {
        // Phase 1: Parse assigned chunk
        const char* ptr = chunks[t].start;
        const char* chunk_end = chunks[t].end;
        auto& my_countries = tdata[t].local_countries;

        while (ptr < chunk_end) {
            // Optional BOM at record start
            if (ptr + 3 <= chunk_end && (uint8_t)ptr[0] == 0xef && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) {
                ptr += 3;
            }

            // Field 0: id
            uint64_t id = 0;
            while (ptr < chunk_end && *ptr >= '0' && *ptr <= '9') {
                id = id * 10 + (*ptr++ - '0');
            }
            if (ptr < chunk_end && *ptr == ',') ptr++;

            // Field 1: name
            uint64_t name_hash = 0xcbf29ce484222325ULL;
            size_t name_len = 0;
            if (ptr < chunk_end && *ptr == '"') {
                ptr++; // skip opening quote
                while (ptr < chunk_end && (*ptr != '"' || (ptr + 1 < chunk_end && *(ptr + 1) == '"'))) {
                    if (*ptr == '"') {
                        name_hash = (name_hash ^ '"') * 0x100000001b3ULL;
                        name_len++;
                        ptr += 2;
                    } else {
                        name_hash = (name_hash ^ (uint8_t)*ptr) * 0x100000001b3ULL;
                        name_len++;
                        ptr++;
                    }
                }
                if (ptr < chunk_end && *ptr == '"') ptr++; // skip closing quote
                if (ptr < chunk_end && *ptr == ',') ptr++; // skip comma
            } else {
                while (ptr < chunk_end && *ptr != ',') {
                    name_hash = (name_hash ^ (uint8_t)*ptr) * 0x100000001b3ULL;
                    name_len++;
                    ptr++;
                }
                if (ptr < chunk_end && *ptr == ',') ptr++; // skip comma
            }

            // Field 2: note
            size_t note_len = 0;
            if (ptr < chunk_end && *ptr == '"') {
                ptr++; // skip opening quote
                while (ptr < chunk_end && (*ptr != '"' || (ptr + 1 < chunk_end && *(ptr + 1) == '"'))) {
                    if (*ptr == '"') {
                        note_len++;
                        ptr += 2;
                    } else {
                        note_len++;
                        ptr++;
                    }
                }
                if (ptr < chunk_end && *ptr == '"') ptr++; // skip closing quote
                if (ptr < chunk_end && *ptr == ',') ptr++; // skip comma
            } else {
                while (ptr < chunk_end && *ptr != ',') {
                    note_len++;
                    ptr++;
                }
                if (ptr < chunk_end && *ptr == ',') ptr++; // skip comma
            }

            // Field 3: region_id
            uint32_t region_id = 0;
            while (ptr < chunk_end && *ptr >= '0' && *ptr <= '9') {
                region_id = region_id * 10 + (*ptr++ - '0');
            }
            if (ptr < chunk_end && *ptr == ',') ptr++; // skip comma

            // Field 4: amount
            bool neg = false;
            if (ptr < chunk_end && *ptr == '-') {
                neg = true;
                ptr++;
            }
            int64_t cents = 0;
            while (ptr < chunk_end && *ptr >= '0' && *ptr <= '9') {
                cents = cents * 10 + (*ptr++ - '0');
            }
            if (ptr < chunk_end && *ptr == '.') ptr++; // skip '.'
            cents = cents * 100 + (*ptr - '0') * 10 + (*(ptr + 1) - '0');
            ptr += 2;
            if (neg) cents = -cents;

            // Skip record terminator (CRLF or LF)
            if (ptr < chunk_end && *ptr == '\r') ptr++;
            if (ptr < chunk_end && *ptr == '\n') ptr++;

            // Lookup country
            uint16_t cid = (region_id < 10000000) ? region_to_country[region_id] : 0;

            LocalCountryAgg& agg = my_countries[cid];
            agg.count++;
            agg.id_sum += (int64_t)id;
            agg.amount_sum += cents;
            if (agg.count == 1) {
                agg.amount_min = cents;
                agg.amount_max = cents;
            } else {
                if (cents < agg.amount_min) agg.amount_min = cents;
                if (cents > agg.amount_max) agg.amount_max = cents;
            }
            agg.name_bytes += name_len;
            agg.note_bytes += note_len;

            // Insert name_hash into local country hash table
            agg.names.insert(name_hash);
        }

        sync_point.arrive_and_wait();

        // Phase 2: Parallel merge across countries partitioned by output_order
        uint16_t total_c = num_countries + 1;
        uint16_t idx_start = (uint16_t)(((uint32_t)total_c * t) / nthreads);
        uint16_t idx_end = (uint16_t)(((uint32_t)total_c * (t + 1)) / nthreads);

        for (uint16_t idx = idx_start; idx < idx_end; ++idx) {
            uint16_t c = output_order[idx];
            MergedCountryAgg& merged = merged_countries[c];
            merged.names.init(4096);
            bool first = true;
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                const auto& other = tdata[ot].local_countries[c];
                if (other.count == 0) continue;
                if (first) {
                    merged.count = other.count;
                    merged.id_sum = other.id_sum;
                    merged.amount_sum = other.amount_sum;
                    merged.amount_min = other.amount_min;
                    merged.amount_max = other.amount_max;
                    merged.name_bytes = other.name_bytes;
                    merged.note_bytes = other.note_bytes;
                    first = false;
                } else {
                    merged.count += other.count;
                    merged.id_sum += other.id_sum;
                    merged.amount_sum += other.amount_sum;
                    if (other.amount_min < merged.amount_min) merged.amount_min = other.amount_min;
                    if (other.amount_max > merged.amount_max) merged.amount_max = other.amount_max;
                    merged.name_bytes += other.name_bytes;
                    merged.note_bytes += other.note_bytes;
                }
                if (other.names.entries) {
                    for (uint32_t k = 0; k < other.names.cap; ++k) {
                        if (other.names.entries[k] != 0) {
                            merged.names.insert(other.names.entries[k]);
                        }
                    }
                }
            }
        }

        sync_point.arrive_and_wait();

        // Phase 3: Format output lines in parallel in exact output_order
        char line_buf[256];
        auto& out_buf = tdata[t].out_buf;
        out_buf.reserve((idx_end - idx_start) * 64);

        for (uint16_t idx = idx_start; idx < idx_end; ++idx) {
            uint16_t c = output_order[idx];
            const auto& agg = merged_countries[c];
            if (agg.count == 0) continue;

            char* p_out = line_buf;
            const std::string& name = country_names[c];
            memcpy(p_out, name.data(), name.size());
            p_out += name.size();
            *p_out++ = ' ';
            p_out = write_u64_fast(p_out, agg.count);
            *p_out++ = ' ';
            p_out = write_i64_fast(p_out, agg.id_sum);
            *p_out++ = ' ';
            p_out = format_money(p_out, agg.amount_sum);
            *p_out++ = ' ';
            p_out = format_money(p_out, agg.amount_min);
            *p_out++ = ' ';
            p_out = format_money(p_out, agg.amount_max);
            *p_out++ = ' ';
            p_out = write_u64_fast(p_out, agg.names.count);
            *p_out++ = ' ';
            p_out = write_u64_fast(p_out, agg.name_bytes);
            *p_out++ = ' ';
            p_out = write_u64_fast(p_out, agg.note_bytes);
            *p_out++ = '\n';

            out_buf.append(line_buf, p_out - line_buf);
        }
    };

#ifdef PROFILE
    auto t_spawn = std::chrono::steady_clock::now();
#endif
    std::vector<std::thread> workers;
    workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) {
        workers.emplace_back(worker_fn, t);
    }
    worker_fn(0);
    for (auto& w : workers) w.join();

#ifdef PROFILE
    auto t_workers = std::chrono::steady_clock::now();
#endif

    // Step 4: Write output
    FILE* f = fopen(out_path.string().c_str(), "wb");
    if (!f) {
        std::cerr << "Failed to open output file " << out_path << "\n";
        return 1;
    }
    for (unsigned int t = 0; t < nthreads; ++t) {
        if (!tdata[t].out_buf.empty()) {
            fwrite(tdata[t].out_buf.data(), 1, tdata[t].out_buf.size(), f);
        }
    }
    fclose(f);

#ifdef PROFILE
    auto t_end = std::chrono::steady_clock::now();
    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    std::cerr << "Regions: " << ms(t_regions - t0) << " ms\n"
              << "Setup:   " << ms(t_spawn - t_regions) << " ms\n"
              << "Workers: " << ms(t_workers - t_spawn) << " ms\n"
              << "Write:   " << ms(t_end - t_workers) << " ms\n"
              << "Total:   " << ms(t_end - t0) << " ms\n";
#endif

    return 0;
}
