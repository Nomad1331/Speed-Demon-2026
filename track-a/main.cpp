#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <barrier>
#include <cstring>
#include <cstdint>
#include <cstdio>
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

static const char DIGITS_LUT[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

inline char* write_u32_fast(char* p, uint32_t val) {
    if (val < 10) { *p++ = (char)('0' + val); return p; }
    char buf[12], *b = buf + 12;
    while (val >= 100) { uint32_t q = val / 100, r = val - q * 100; val = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2); }
    if (val < 10) *--b = (char)('0' + val); else { b -= 2; memcpy(b, &DIGITS_LUT[val * 2], 2); }
    size_t len = (buf + 12) - b; memcpy(p, b, len); return p + len;
}

inline void append_line(std::string& out, const std::string& h, const std::string& ep,
                        uint32_t count, uint32_t p50, uint32_t p95, uint32_t p99, uint32_t errs, uint32_t ppm) {
    char buf[256], *p = buf;
    memcpy(p, h.data(), h.size()); p += h.size(); *p++ = ' '; memcpy(p, ep.data(), ep.size()); p += ep.size(); *p++ = ' ';
    p = write_u32_fast(p, count); *p++ = ' '; p = write_u32_fast(p, p50); *p++ = ' ';
    p = write_u32_fast(p, p95); *p++ = ' '; p = write_u32_fast(p, p99); *p++ = ' ';
    p = write_u32_fast(p, errs); *p++ = ' '; p = write_u32_fast(p, ppm); *p++ = '\n'; out.append(buf, p - buf);
}

struct MappedFile {
    const char* data = nullptr; size_t size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = NULL;
    bool open(const std::string& path) {
        hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fs; if (!GetFileSizeEx(hFile, &fs) || (size = (size_t)fs.QuadPart) == 0) { CloseHandle(hFile); return false; }
        hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        return hMap && (data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0)) != nullptr;
    }
    ~MappedFile() { if (data) UnmapViewOfFile(data); if (hMap) CloseHandle(hMap); if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile); }
#else
    int fd = -1;
    bool open(const std::string& path) {
        if ((fd = ::open(path.c_str(), O_RDONLY)) < 0) return false;
        struct stat st; if (fstat(fd, &st) < 0 || (size = (size_t)st.st_size) == 0) { ::close(fd); return false; }
        data = (const char*)::mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0); return data != MAP_FAILED;
    }
    ~MappedFile() { if (data && data != MAP_FAILED) munmap((void*)data, size); if (fd >= 0) ::close(fd); }
#endif
};

inline uint32_t hash_endpoint(const char* s, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL ^ len;
    while (len >= 8) { uint64_t v; memcpy(&v, s, 8); h = (h ^ v) * 0x100000001b3ULL; s += 8; len -= 8; }
    if (len >= 4) { uint32_t v; memcpy(&v, s, 4); h = (h ^ v) * 0x100000001b3ULL; s += 4; len -= 4; }
    while (len > 0) { h = (h ^ (uint8_t)*s) * 0x100000001b3ULL; ++s; --len; }
    uint32_t res = (uint32_t)(h ^ (h >> 32)); return res ? res : 1;
}

static constexpr size_t MAX_EP = 2048, MAX_H = 72;
struct EpEntry { uint32_t hash = 0; uint16_t local_id = 0, global_id = 0; };
struct EndpointTable {
    static constexpr size_t CAP = 4096, MASK = CAP - 1;
    EpEntry entries[CAP];
    EndpointTable() { memset(entries, 0, sizeof(entries)); }
    inline uint16_t lookup_or_insert(const char* s, size_t len, uint32_t h, std::vector<std::string>& eps) {
        size_t idx = h & MASK;
        while (entries[idx].hash != 0) {
            if (entries[idx].hash == h && eps[entries[idx].local_id].size() == len && !memcmp(eps[entries[idx].local_id].data(), s, len))
                return entries[idx].local_id;
            idx = (idx + 1) & MASK;
        }
        uint16_t lid = (uint16_t)eps.size(); eps.emplace_back(s, len); entries[idx] = {h, lid, 0}; return lid;
    }
    inline uint16_t lookup(const char* s, size_t len, uint32_t h, const std::vector<std::string>& eps) const {
        size_t idx = h & MASK;
        while (true) {
            if (entries[idx].hash == h && eps[entries[idx].local_id].size() == len && !memcmp(eps[entries[idx].local_id].data(), s, len))
                return entries[idx].global_id;
            idx = (idx + 1) & MASK;
        }
    }
};

struct ActiveGroup { uint8_t hid; uint16_t lep; };
struct ThreadData {
    EndpointTable ep_table;
    std::vector<std::string> local_endpoints;
    std::vector<ActiveGroup> active_groups;
    std::vector<uint32_t> counts, errors;
    std::vector<uint16_t> ep_remap;
    std::string out_buffer;
    ThreadData() : counts(MAX_H * MAX_EP, 0), errors(MAX_H * MAX_EP, 0) {
        local_endpoints.reserve(2048); active_groups.reserve(4096);
    }
};

inline int get_hour_id(const char* p) {
    int d = (p[8] - '0') * 10 + (p[9] - '0'), h = (p[11] - '0') * 10 + (p[12] - '0');
    return ((p[6] == '0') ? (d - 30) : (d + 1)) * 24 + h - 18;
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }
    std::filesystem::path in_path = std::filesystem::path(data_dir) / "requests.log", out_path = std::filesystem::path(out_dir) / "summary.txt";
    std::filesystem::create_directories(out_dir);

    MappedFile mf;
    if (!mf.open(in_path.string()) || mf.size == 0) {
        FILE* f = fopen(out_path.string().c_str(), "wb"); if (f) fclose(f); return 0;
    }

    static const size_t H = 72;
    std::string hour_strings[H];
    {
        size_t idx = 0;
        auto add_h = [&](const char* pr, int s, int e) {
            for (int h = s; h <= e; ++h) {
                char buf[32]; snprintf(buf, sizeof(buf), "%sT%02d:00:00Z", pr, h); hour_strings[idx++] = buf;
            }
        };
        add_h("2026-10-30", 18, 23); add_h("2026-10-31", 0, 23); add_h("2026-11-01", 0, 23); add_h("2026-11-02", 0, 17);
    }

    unsigned int nthreads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (mf.size < 100000) nthreads = 1;

    struct Chunk { size_t start = 0, end = 0; };
    std::vector<Chunk> chunks(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t s = (mf.size * i) / nthreads, e = (mf.size * (i + 1)) / nthreads;
        if (i > 0) { while (s < mf.size && mf.data[s] != '\n') ++s; if (s < mf.size) ++s; }
        if (i + 1 < nthreads) { while (e < mf.size && mf.data[e] != '\n') ++e; if (e < mf.size) ++e; } else e = mf.size;
        chunks[i] = {s, e};
    }

    std::vector<ThreadData> tdata(nthreads);
    std::vector<std::string> global_endpoints;
    size_t N = 0, G = 0;
    std::vector<uint32_t> global_counts, global_errors, all_latencies, write_offsets;
    std::vector<size_t> group_offsets;
    std::barrier sync_point(nthreads);

    auto worker_fn = [&](unsigned int t) {
        auto& td = tdata[t];
        const char* p = mf.data + chunks[t].start, *end = mf.data + chunks[t].end;
        char last_hour[13] = {0}; int last_hid = -1;

        // Pass 1: Parse endpoints, hours, counts, and errors (zero latency parsing)
        while (p < end) {
            if (p + 27 > end) break;
            int hid = (!memcmp(p, last_hour, 13)) ? last_hid : get_hour_id(p);
            if (hid != last_hid) { memcpy(last_hour, p, 13); last_hid = hid; }
            const char* cur = p + 28; while (*cur != ' ') ++cur;
            const char* ep_start = ++cur; while (*cur != ' ') ++cur;
            size_t ep_len = cur - ep_start;
            uint16_t ep_id = td.ep_table.lookup_or_insert(ep_start, ep_len, hash_endpoint(ep_start, ep_len), td.local_endpoints);
            const char* st = ++cur; size_t idx = (size_t)hid * MAX_EP + ep_id;
            if (td.counts[idx]++ == 0) td.active_groups.push_back({(uint8_t)hid, ep_id});
            if (st[0] == '5') td.errors[idx]++;
            cur = st + 4; while (*cur != '\n') ++cur; p = cur + 1;
        }
        sync_point.arrive_and_wait();

        if (t == 0) {
            global_endpoints = tdata[0].local_endpoints;
            std::sort(global_endpoints.begin(), global_endpoints.end());
            for (unsigned int ot = 1; ot < nthreads; ++ot) {
                for (const auto& ep : tdata[ot].local_endpoints) {
                    auto it = std::lower_bound(global_endpoints.begin(), global_endpoints.end(), ep);
                    if (it == global_endpoints.end() || *it != ep) global_endpoints.insert(it, ep);
                }
            }
            N = global_endpoints.size(); G = H * N;
        }
        sync_point.arrive_and_wait();
        if (N == 0) return;

        size_t lep_count = td.local_endpoints.size();
        td.ep_remap.resize(lep_count);
        for (size_t i = 0; i < lep_count; ++i) {
            td.ep_remap[i] = (uint16_t)(std::lower_bound(global_endpoints.begin(), global_endpoints.end(), td.local_endpoints[i]) - global_endpoints.begin());
        }
        for (auto& entry : td.ep_table.entries) { if (entry.hash) entry.global_id = td.ep_remap[entry.local_id]; }
        sync_point.arrive_and_wait();

        if (t == 0) {
            global_counts.assign(G, 0); global_errors.assign(G, 0);
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                for (const auto& ag : tdata[ot].active_groups) {
                    size_t idx = (size_t)ag.hid * MAX_EP + ag.lep, ggid = (size_t)ag.hid * N + tdata[ot].ep_remap[ag.lep];
                    global_counts[ggid] += tdata[ot].counts[idx]; global_errors[ggid] += tdata[ot].errors[idx];
                }
            }
            group_offsets.resize(G + 1, 0);
            for (size_t g = 0; g < G; ++g) group_offsets[g + 1] = group_offsets[g] + global_counts[g];
            all_latencies.resize(group_offsets[G]);

            write_offsets.resize(nthreads * G);
            for (size_t g = 0; g < G; ++g) write_offsets[0 * G + g] = (uint32_t)group_offsets[g];
            for (unsigned int ot = 1; ot < nthreads; ++ot) {
                memcpy(&write_offsets[ot * G], &write_offsets[(ot - 1) * G], G * sizeof(uint32_t));
                for (const auto& ag : tdata[ot - 1].active_groups) {
                    write_offsets[ot * G + (size_t)ag.hid * N + tdata[ot - 1].ep_remap[ag.lep]] += tdata[ot - 1].counts[(size_t)ag.hid * MAX_EP + ag.lep];
                }
            }
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                tdata[ot].counts.clear(); tdata[ot].counts.shrink_to_fit();
                tdata[ot].errors.clear(); tdata[ot].errors.shrink_to_fit();
                tdata[ot].active_groups.clear(); tdata[ot].active_groups.shrink_to_fit();
            }
        }
        sync_point.arrive_and_wait();

        // Pass 2: Stream latencies directly into flat all_latencies array
        p = mf.data + chunks[t].start; last_hid = -1;
        uint32_t* wp = &write_offsets[t * G];
        while (p < end) {
            if (p + 27 > end) break;
            int hid = (!memcmp(p, last_hour, 13)) ? last_hid : get_hour_id(p);
            if (hid != last_hid) { memcpy(last_hour, p, 13); last_hid = hid; }
            const char* cur = p + 28; while (*cur != ' ') ++cur;
            const char* ep_start = ++cur; while (*cur != ' ') ++cur;
            size_t ep_len = cur - ep_start;
            uint16_t gep = td.ep_table.lookup(ep_start, ep_len, hash_endpoint(ep_start, ep_len), td.local_endpoints);
            cur += 5; uint32_t lat = 0;
            while (*cur >= '0' && *cur <= '9') { lat = lat * 10 + (*cur - '0'); ++cur; }
            all_latencies[wp[(size_t)hid * N + gep]++] = lat;
            if (*cur == '\r') ++cur; if (*cur == '\n') ++cur; p = cur;
        }
        sync_point.arrive_and_wait();

        // Pass 3: Parallel Percentiles & Output Formatting
        auto& out = td.out_buffer; out.reserve(512 * 1024);
        size_t h_start = (H * t) / nthreads, h_end = (H * (t + 1)) / nthreads;
        for (size_t h = h_start; h < h_end; ++h) {
            const std::string& h_str = hour_strings[h];
            for (size_t gep = 0; gep < N; ++gep) {
                size_t g = h * N + gep, n = global_counts[g];
                if (n == 0) continue;
                uint32_t* begin = &all_latencies[group_offsets[g]], *end = begin + n, p50, p95, p99;
                if (n == 1) { p50 = p95 = p99 = begin[0]; }
                else if (n == 2) {
                    if (begin[0] > begin[1]) std::swap(begin[0], begin[1]);
                    p50 = begin[0]; p95 = p99 = begin[1];
                } else {
                    size_t r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
                    if (n <= 64) {
                        std::sort(begin, end); p50 = begin[r50]; p95 = begin[r95]; p99 = begin[r99];
                    } else {
                        std::nth_element(begin, begin + r99, end); p99 = begin[r99];
                        std::nth_element(begin, begin + r95, begin + r99); p95 = begin[r95];
                        std::nth_element(begin, begin + r50, begin + r95); p50 = begin[r50];
                    }
                }
                uint32_t errs = global_errors[g], ppm = ((uint64_t)errs * 1000000ULL) / n;
                append_line(out, h_str, global_endpoints[gep], (uint32_t)n, p50, p95, p99, errs, (uint32_t)ppm);
            }
        }
    };

    std::vector<std::thread> workers; workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) workers.emplace_back(worker_fn, t);
    worker_fn(0);
    for (auto& w : workers) w.join();

    FILE* f = fopen(out_path.string().c_str(), "wb");
    if (!f) return 1;
    for (unsigned int t = 0; t < nthreads; ++t) if (!tdata[t].out_buffer.empty()) fwrite(tdata[t].out_buffer.data(), 1, tdata[t].out_buffer.size(), f);
    fclose(f); return 0;
}
