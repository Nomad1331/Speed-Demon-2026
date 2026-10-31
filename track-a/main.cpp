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

inline char* write_u32_fast(char* p, uint32_t val) {
    if (val < 10) {
        *p++ = (char)('0' + val);
        return p;
    }
    char buf[12];
    char* b = buf + 12;
    while (val >= 100) {
        uint32_t q = val / 100;
        uint32_t r = val - q * 100;
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
    size_t len = (buf + 12) - b;
    memcpy(p, b, len);
    return p + len;
}

inline void append_line(std::string& out, const std::string& hour, const std::string& ep,
                        uint32_t count, uint32_t p50, uint32_t p95, uint32_t p99, uint32_t errors, uint32_t ppm) {
    char buf[256];
    char* p = buf;
    memcpy(p, hour.data(), hour.size());
    p += hour.size();
    *p++ = ' ';
    memcpy(p, ep.data(), ep.size());
    p += ep.size();
    *p++ = ' ';
    p = write_u32_fast(p, count);
    *p++ = ' ';
    p = write_u32_fast(p, p50);
    *p++ = ' ';
    p = write_u32_fast(p, p95);
    *p++ = ' ';
    p = write_u32_fast(p, p99);
    *p++ = ' ';
    p = write_u32_fast(p, errors);
    *p++ = ' ';
    p = write_u32_fast(p, ppm);
    *p++ = '\n';

    out.append(buf, p - buf);
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

inline uint32_t hash_endpoint(const char* s, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL ^ len;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, s, 8);
        h = (h ^ v) * 0x100000001b3ULL;
        s += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        memcpy(&v, s, 4);
        h = (h ^ v) * 0x100000001b3ULL;
        s += 4;
        len -= 4;
    }
    while (len > 0) {
        h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
        ++s;
        --len;
    }
    uint32_t res = (uint32_t)(h ^ (h >> 32));
    return res ? res : 1;
}

static constexpr size_t MAX_EP = 4096;
static constexpr size_t MAX_H = 72;

struct EndpointTable {
    static constexpr size_t CAP = 8192;
    static constexpr size_t MASK = CAP - 1;
    struct Entry {
        uint32_t hash;
        uint16_t ep_id; // 1-indexed, 0 = empty
    };
    Entry entries[CAP];

    EndpointTable() {
        memset(entries, 0, sizeof(entries));
    }

    inline uint16_t lookup_or_insert(const char* s, size_t len, uint32_t h, std::vector<std::string>& local_endpoints) {
        size_t idx = h & MASK;
        while (entries[idx].ep_id != 0) {
            if (entries[idx].hash == h) {
                uint16_t id = entries[idx].ep_id - 1;
                const std::string& stored = local_endpoints[id];
                if (stored.size() == len && memcmp(stored.data(), s, len) == 0) {
                    return id;
                }
            }
            idx = (idx + 1) & MASK;
        }
        uint16_t id = (uint16_t)local_endpoints.size();
        local_endpoints.emplace_back(s, len);
        entries[idx].hash = h;
        entries[idx].ep_id = id + 1;
        return id;
    }
};

struct Record {
    uint32_t latency;
    uint16_t ep_id;
    uint8_t hid;
    uint8_t pad;
};

struct ActiveGroup {
    uint8_t hid;
    uint16_t lep;
};

struct ThreadData {
    EndpointTable ep_table;
    std::vector<std::string> local_endpoints;
    std::vector<Record> records;
    std::vector<ActiveGroup> active_groups;
    std::vector<uint32_t> counts;
    std::vector<uint32_t> errors;
    std::vector<uint32_t> local_ggid_counts;
    std::vector<uint16_t> ep_remap;
    std::string out_buffer;

    ThreadData() : counts(MAX_H * MAX_EP, 0), errors(MAX_H * MAX_EP, 0) {
        local_endpoints.reserve(2048);
        active_groups.reserve(4096);
    }
};

// Fast RFC 3339 hour parser for the 72-hour competition window (Oct 30 18:00 to Nov 02 17:59)
inline int get_hour_id(const char* p) {
    if (p[0] == '2' && p[1] == '0' && p[2] == '2' && p[3] == '6' && p[4] == '-') {
        int m = (p[5] - '0') * 10 + (p[6] - '0');
        int d = (p[8] - '0') * 10 + (p[9] - '0');
        int h = (p[11] - '0') * 10 + (p[12] - '0');
        int day = -1;
        if (m == 10) {
            if (d == 30) day = 0;
            else if (d == 31) day = 1;
        } else if (m == 11) {
            if (d == 1) day = 2;
            else if (d == 2) day = 3;
        }
        if (day >= 0) {
            int hid = day * 24 + h - 18;
            if (hid >= 0 && hid < 72) return hid;
        }
    }
    return -1;
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);

    std::string data_dir = "/data";
    std::string out_dir = "/out";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        }
    }

    std::filesystem::path in_path = std::filesystem::path(data_dir) / "requests.log";
    std::filesystem::path out_path = std::filesystem::path(out_dir) / "summary.txt";
    std::filesystem::create_directories(out_dir);

    MappedFile mf;
#ifdef PROFILE
    auto t0 = std::chrono::steady_clock::now();
#endif
    if (!mf.open(in_path.string())) {
        FILE* f = fopen(out_path.string().c_str(), "wb");
        if (f) fclose(f);
        return 0;
    }
#ifdef PROFILE
    auto t_open = std::chrono::steady_clock::now();
#endif

    if (mf.size == 0) {
        FILE* f = fopen(out_path.string().c_str(), "wb");
        if (f) fclose(f);
        return 0;
    }

    static const size_t H = 72;
    std::string hour_strings[H];
    {
        size_t idx = 0;
        for (int h = 18; h <= 23; ++h) {
            char buf[32]; snprintf(buf, sizeof(buf), "2026-10-30T%02d:00:00Z", h);
            hour_strings[idx++] = buf;
        }
        for (int h = 0; h <= 23; ++h) {
            char buf[32]; snprintf(buf, sizeof(buf), "2026-10-31T%02d:00:00Z", h);
            hour_strings[idx++] = buf;
        }
        for (int h = 0; h <= 23; ++h) {
            char buf[32]; snprintf(buf, sizeof(buf), "2026-11-01T%02d:00:00Z", h);
            hour_strings[idx++] = buf;
        }
        for (int h = 0; h <= 17; ++h) {
            char buf[32]; snprintf(buf, sizeof(buf), "2026-11-02T%02d:00:00Z", h);
            hour_strings[idx++] = buf;
        }
    }

    unsigned int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    if (mf.size < 100000) nthreads = 1;
    if (nthreads > 16) nthreads = 16;

    struct Chunk {
        size_t start = 0;
        size_t end = 0;
    };
    std::vector<Chunk> chunks(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t s = (mf.size * i) / nthreads;
        if (i > 0) {
            while (s < mf.size && mf.data[s] != '\n') ++s;
            if (s < mf.size) ++s;
        }
        size_t e = (mf.size * (i + 1)) / nthreads;
        if (i + 1 < nthreads) {
            while (e < mf.size && mf.data[e] != '\n') ++e;
            if (e < mf.size) ++e;
        } else {
            e = mf.size;
        }
        chunks[i] = {s, e};
    }

    std::vector<ThreadData> tdata(nthreads);
    std::vector<std::string> global_endpoints;
    size_t N = 0;
    size_t total_groups = 0;
    std::vector<uint32_t> global_counts;
    std::vector<uint32_t> global_errors;
    std::vector<size_t> global_offsets;
    std::vector<uint32_t> all_latencies;
    std::vector<uint32_t> write_pos;

    std::barrier sync_point(nthreads);

#ifdef PROFILE
    std::chrono::steady_clock::time_point t_p1, t_p2, t_p3, t_p4, t_p5;
#endif

    auto worker_fn = [&](unsigned int t) {
        // Phase 1: Parse chunks
        auto& td = tdata[t];
        const char* p = mf.data + chunks[t].start;
        const char* end = mf.data + chunks[t].end;
        if (p < end) {
            td.records.reserve((chunks[t].end - chunks[t].start) / 60);
            char last_hour[13] = {0};
            int last_hid = -1;

            while (p < end) {
                if (p + 27 > end) break;
                int hid;
                if (last_hid >= 0 && memcmp(p, last_hour, 13) == 0) {
                    hid = last_hid;
                } else {
                    hid = get_hour_id(p);
                    memcpy(last_hour, p, 13);
                    last_hid = hid;
                }

                const char* cur = p + 28;
                while (cur < end && *cur != ' ') ++cur;
                if (cur >= end) break;
                const char* ep_start = ++cur;

                while (cur < end && *cur != ' ') ++cur;
                if (cur >= end) break;
                size_t ep_len = cur - ep_start;
                uint32_t ep_hash = hash_endpoint(ep_start, ep_len);
                uint16_t ep_id = td.ep_table.lookup_or_insert(ep_start, ep_len, ep_hash, td.local_endpoints);

                const char* st_p = ++cur;
                bool is_err = (st_p < end && st_p[0] == '5');

                cur = st_p + 4;
                uint32_t lat = 0;
                while (cur < end && *cur >= '0' && *cur <= '9') {
                    lat = lat * 10 + (*cur - '0');
                    ++cur;
                }

                if (hid >= 0 && ep_id < (uint16_t)MAX_EP) {
                    size_t idx = (size_t)hid * MAX_EP + ep_id;
                    if (td.counts[idx] == 0) {
                        td.active_groups.push_back({(uint8_t)hid, ep_id});
                    }
                    td.counts[idx]++;
                    if (is_err) td.errors[idx]++;
                    td.records.push_back({lat, ep_id, (uint8_t)hid, 0});
                }

                if (cur < end && *cur == '\r') ++cur;
                if (cur < end && *cur == '\n') ++cur;
                p = cur;
            }
        }

        sync_point.arrive_and_wait();

        // Phase 2A: Single-thread merges endpoints
        if (t == 0) {
#ifdef PROFILE
            t_p1 = std::chrono::steady_clock::now();
#endif
            global_endpoints = tdata[0].local_endpoints;
            std::sort(global_endpoints.begin(), global_endpoints.end());
            for (unsigned int ot = 1; ot < nthreads; ++ot) {
                for (const auto& ep : tdata[ot].local_endpoints) {
                    auto it = std::lower_bound(global_endpoints.begin(), global_endpoints.end(), ep);
                    if (it == global_endpoints.end() || *it != ep) {
                        global_endpoints.insert(it, ep);
                    }
                }
            }
            N = global_endpoints.size();
            total_groups = H * N;
        }

        sync_point.arrive_and_wait();

        if (N == 0) return;

        // Phase 2B: Parallel Remap & Local ggid counts
        {
            size_t lep_count = td.local_endpoints.size();
            td.ep_remap.resize(lep_count);
            for (size_t i = 0; i < lep_count; ++i) {
                auto it = std::lower_bound(global_endpoints.begin(), global_endpoints.end(), td.local_endpoints[i]);
                td.ep_remap[i] = (uint16_t)(it - global_endpoints.begin());
            }

            td.local_ggid_counts.assign(total_groups, 0);
            for (const auto& ag : td.active_groups) {
                uint32_t c = td.counts[(size_t)ag.hid * MAX_EP + ag.lep];
                uint16_t gep = td.ep_remap[ag.lep];
                td.local_ggid_counts[(size_t)ag.hid * N + gep] = c;
            }
        }

        sync_point.arrive_and_wait();

        // Phase 3: Global offsets & write_pos on thread 0
        if (t == 0) {
#ifdef PROFILE
            t_p2 = std::chrono::steady_clock::now();
#endif
            global_counts.assign(total_groups, 0);
            global_errors.assign(total_groups, 0);
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                for (const auto& ag : tdata[ot].active_groups) {
                    size_t idx = (size_t)ag.hid * MAX_EP + ag.lep;
                    uint32_t c = tdata[ot].counts[idx];
                    uint16_t gep = tdata[ot].ep_remap[ag.lep];
                    size_t ggid = (size_t)ag.hid * N + gep;
                    global_counts[ggid] += c;
                    global_errors[ggid] += tdata[ot].errors[idx];
                }
            }

            global_offsets.resize(total_groups + 1, 0);
            for (size_t g = 0; g < total_groups; ++g) {
                global_offsets[g + 1] = global_offsets[g] + global_counts[g];
            }
            size_t total_latencies = global_offsets[total_groups];
            all_latencies.resize(total_latencies);

            write_pos.resize(nthreads * total_groups);
            for (size_t g = 0; g < total_groups; ++g) {
                write_pos[0 * total_groups + g] = (uint32_t)global_offsets[g];
            }
            for (unsigned int ot = 1; ot < nthreads; ++ot) {
                uint32_t* wp_cur = &write_pos[ot * total_groups];
                const uint32_t* wp_prev = &write_pos[(ot - 1) * total_groups];
                const uint32_t* counts_prev = tdata[ot - 1].local_ggid_counts.data();
                for (size_t g = 0; g < total_groups; ++g) {
                    wp_cur[g] = wp_prev[g] + counts_prev[g];
                }
            }
#ifdef PROFILE
            t_p3 = std::chrono::steady_clock::now();
#endif
        }

        sync_point.arrive_and_wait();

        // Phase 4: Scatter latencies in parallel
        {
            uint32_t* wp = &write_pos[t * total_groups];
            const auto& ep_map = td.ep_remap;
            for (const auto& rec : td.records) {
                uint16_t gep = ep_map[rec.ep_id];
                size_t ggid = (size_t)rec.hid * N + gep;
                all_latencies[wp[ggid]++] = rec.latency;
            }
        }

        sync_point.arrive_and_wait();

        if (t == 0) {
#ifdef PROFILE
            t_p4 = std::chrono::steady_clock::now();
#endif
        }

        // Phase 5: Percentiles and output generation in parallel
        {
            auto& out = td.out_buffer;
            out.reserve(1024 * 1024);

            size_t h_start = (H * t) / nthreads;
            size_t h_end = (H * (t + 1)) / nthreads;

            for (size_t h = h_start; h < h_end; ++h) {
                const std::string& h_str = hour_strings[h];
                for (size_t ep = 0; ep < N; ++ep) {
                    size_t g = h * N + ep;
                    size_t n = global_counts[g];
                    if (n == 0) continue;

                    uint32_t* begin = &all_latencies[global_offsets[g]];
                    uint32_t* end = begin + n;

                    uint32_t p50, p95, p99;
                    if (n == 1) {
                        p50 = p95 = p99 = begin[0];
                    } else if (n == 2) {
                        if (begin[0] > begin[1]) std::swap(begin[0], begin[1]);
                        p50 = begin[0];
                        p95 = p99 = begin[1];
                    } else {
                        size_t r50 = (50 * n + 99) / 100 - 1;
                        size_t r95 = (95 * n + 99) / 100 - 1;
                        size_t r99 = (99 * n + 99) / 100 - 1;

                        if (n <= 64) {
                            std::sort(begin, end);
                            p50 = begin[r50];
                            p95 = begin[r95];
                            p99 = begin[r99];
                        } else {
                            std::nth_element(begin, begin + r99, end);
                            p99 = begin[r99];
                            std::nth_element(begin, begin + r95, begin + r99);
                            p95 = begin[r95];
                            std::nth_element(begin, begin + r50, begin + r95);
                            p50 = begin[r50];
                        }
                    }

                    uint32_t errs = global_errors[g];
                    uint64_t ppm = ((uint64_t)errs * 1000000ULL) / n;

                    append_line(out, h_str, global_endpoints[ep], (uint32_t)n, p50, p95, p99, errs, (uint32_t)ppm);
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) {
        workers.emplace_back(worker_fn, t);
    }
    worker_fn(0);

    for (auto& w : workers) w.join();

#ifdef PROFILE
    t_p5 = std::chrono::steady_clock::now();
#endif

    // Phase 6: Write output
    FILE* f = fopen(out_path.string().c_str(), "wb");
    if (!f) return 1;
    for (unsigned int t = 0; t < nthreads; ++t) {
        if (!tdata[t].out_buffer.empty()) {
            fwrite(tdata[t].out_buffer.data(), 1, tdata[t].out_buffer.size(), f);
        }
    }
    fclose(f);
#ifdef PROFILE
    auto t_p6 = std::chrono::steady_clock::now();
    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    std::cerr << "Open:    " << ms(t_open - t0) << " ms\n"
              << "Phase 1: " << ms(t_p1 - t_open) << " ms\n"
              << "Phase 2: " << ms(t_p2 - t_p1) << " ms\n"
              << "Phase 3: " << ms(t_p3 - t_p2) << " ms\n"
              << "Phase 4: " << ms(t_p4 - t_p3) << " ms\n"
              << "Phase 5: " << ms(t_p5 - t_p4) << " ms\n"
              << "Phase 6: " << ms(t_p6 - t_p5) << " ms\n"
              << "Total:   " << ms(t_p6 - t0) << " ms\n";
#endif

    return 0;
}
