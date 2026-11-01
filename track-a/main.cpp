#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
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

inline char* write_u64_fast(char* p, uint64_t val) {
    if (val < 10) { *p++ = (char)('0' + val); return p; }
    char buf[24], *b = buf + 24;
    while (val >= 100) {
        uint64_t q = val / 100; uint32_t r = (uint32_t)(val - q * 100);
        val = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2);
    }
    if (val < 10) *--b = (char)('0' + val);
    else { b -= 2; memcpy(b, &DIGITS_LUT[val * 2], 2); }
    size_t len = (buf + 24) - b; memcpy(p, b, len); return p + len;
}

inline const char* find_char_avx2(const char* p, const char* end, char target) {
    __m256i tgt = _mm256_set1_epi8(target);
    while (p + 32 <= end) {
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)p), tgt));
        if (mask) return p + __builtin_ctz(mask);
        p += 32;
    }
    while (p < end && *p != target) ++p;
    return p;
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

static constexpr int H = 72;

struct GroupData {
    std::vector<uint32_t> latencies;
    uint32_t errors = 0;
};

struct LocalPaths {
    struct Slot { uint64_t hash = 0; uint32_t off = 0; uint16_t len = 0, id = 0; };
    Slot slots[4096]{};
    std::vector<std::string_view> names;
    uint16_t count = 0;

    inline uint16_t intern(const char* s, uint16_t len, uint64_t h, uint32_t file_off, const char* file_base) {
        size_t idx = h & 4095;
        while (slots[idx].len != 0) {
            if (slots[idx].hash == h && slots[idx].len == len && memcmp(file_base + slots[idx].off, s, len) == 0)
                return slots[idx].id;
            idx = (idx + 1) & 4095;
        }
        slots[idx] = { h, file_off, len, count };
        names.push_back(std::string_view(s, len));
        return count++;
    }
};

struct Shard {
    LocalPaths paths;
    std::vector<GroupData> hour_groups[H];
};

int main(int argc, char** argv) {
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (argv[i][0] != '-') {
            if (data_dir == "/data") data_dir = argv[i];
            else if (out_dir == "/out") out_dir = argv[i];
        }
    }
    std::filesystem::path in_path = std::filesystem::path(data_dir) / "requests.log", out_path = std::filesystem::path(out_dir) / "summary.txt";
    std::filesystem::create_directories(out_dir);

    int in_fd = ::open(in_path.string().c_str(), O_RDONLY | O_BINARY);
    if (in_fd < 0) return 0;
    struct stat in_st;
    if (fstat(in_fd, &in_st) < 0 || in_st.st_size == 0) { ::close(in_fd); return 0; }
    size_t file_size = (size_t)in_st.st_size;
    const char* file_data = (const char*)::mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, in_fd, 0);
    if (file_data == MAP_FAILED) { ::close(in_fd); return 0; }
    madvise((void*)file_data, file_size, MADV_WILLNEED | MADV_SEQUENTIAL);

    std::string hour_strings[H];
    {
        size_t idx = 0;
        auto add_h = [&](const char* pr, int s, int e) {
            for (int h = s; h <= e; ++h) {
                char buf[32]; snprintf(buf, sizeof(buf), "%sT%02d:00:00Z", pr, h);
                hour_strings[idx++] = buf;
            }
        };
        add_h("2026-10-30", 18, 23); add_h("2026-10-31", 0, 23); add_h("2026-11-01", 0, 23); add_h("2026-11-02", 0, 17);
    }

    unsigned int nthreads = (file_size < 100000) ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    std::vector<Shard> shards(nthreads);

    auto worker_scan = [&](unsigned int tid) {
        size_t s = (file_size * tid) / nthreads, e = (file_size * (tid + 1)) / nthreads;
        const char* p = file_data + s, *end = file_data + e, *file_end = file_data + file_size;
        if (tid > 0) {
            p = find_char_avx2(p, file_end, '\n');
            if (p < file_end && *p == '\n') p++;
        }
        if (tid + 1 < nthreads) {
            end = find_char_avx2(end, file_end, '\n');
            if (end < file_end && *end == '\n') end++;
        } else end = file_end;

        Shard& shard = shards[tid];
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
            cur = find_char_avx2(cur, end, ' ');
            uint16_t ep_len = (uint16_t)(cur - ep_start);
            uint64_t h = hash_endpoint(ep_start, ep_len);
            uint16_t ep_id = shard.paths.intern(ep_start, ep_len, h, (uint32_t)(ep_start - file_data), file_data);

            bool is_err = (cur[1] == '5');
            cur += 5;
            uint32_t lat = (uint8_t)(*cur++ - '0');
            while ((uint8_t)(*cur - '0') <= 9) lat = lat * 10 + (uint8_t)(*cur++ - '0');

            if (hid >= 0 && hid < H) {
                auto& hgroups = shard.hour_groups[hid];
                if (hgroups.size() <= ep_id) hgroups.resize(ep_id + 1);
                hgroups[ep_id].latencies.push_back(lat);
                hgroups[ep_id].errors += is_err;
            }

            p = (*cur == '\n') ? cur + 1 : find_char_avx2(cur, end, '\n') + 1;
        }
    };

    std::vector<std::thread> workers;
    for (unsigned int tid = 1; tid < nthreads; ++tid) workers.emplace_back(worker_scan, tid);
    worker_scan(0);
    for (auto& w : workers) w.join();

    // Merge unique endpoints across all shards
    std::vector<std::string_view> global_endpoints;
    for (unsigned int i = 0; i < nthreads; ++i) {
        for (const auto& name : shards[i].paths.names) global_endpoints.push_back(name);
    }
    std::sort(global_endpoints.begin(), global_endpoints.end());
    global_endpoints.erase(std::unique(global_endpoints.begin(), global_endpoints.end()), global_endpoints.end());
    size_t num_ep = global_endpoints.size();

    // Map each shard's local endpoint ID to global index
    std::vector<std::vector<int>> shard_ep_map(nthreads, std::vector<int>(num_ep, -1));
    for (unsigned int i = 0; i < nthreads; ++i) {
        for (size_t local_id = 0; local_id < shards[i].paths.names.size(); ++local_id) {
            auto it = std::lower_bound(global_endpoints.begin(), global_endpoints.end(), shards[i].paths.names[local_id]);
            size_t g_idx = it - global_endpoints.begin();
            shard_ep_map[i][g_idx] = (int)local_id;
        }
    }

    // Render output in parallel by hour chunks
    std::vector<std::string> thread_outputs(nthreads);
    auto render_worker = [&](unsigned int tid) {
        std::string& out = thread_outputs[tid];
        out.reserve(num_ep * 72);
        std::vector<uint32_t> merged;
        char line_buf[256];

        int h_start = (H * tid) / nthreads, h_end = (H * (tid + 1)) / nthreads;
        for (int h = h_start; h < h_end; ++h) {
            for (size_t e = 0; e < num_ep; ++e) {
                GroupData* first = nullptr;
                uint32_t errors = 0;
                merged.clear();

                for (unsigned int s = 0; s < nthreads; ++s) {
                    int local_id = shard_ep_map[s][e];
                    if (local_id < 0 || (size_t)local_id >= shards[s].hour_groups[h].size()) continue;
                    auto& part = shards[s].hour_groups[h][local_id];
                    if (part.latencies.empty()) continue;
                    errors += part.errors;
                    if (!first) { first = &part; continue; }
                    if (merged.empty()) merged.insert(merged.end(), first->latencies.begin(), first->latencies.end());
                    merged.insert(merged.end(), part.latencies.begin(), part.latencies.end());
                }
                if (!first) continue;

                auto& values = merged.empty() ? first->latencies : merged;
                size_t n = values.size();
                size_t r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
                uint32_t p50, p95, p99;
                if (n <= 64) {
                    std::sort(values.begin(), values.end());
                    p50 = values[r50]; p95 = values[r95]; p99 = values[r99];
                } else {
                    auto b = values.begin();
                    std::nth_element(b, b + r99, values.end()); p99 = values[r99];
                    std::nth_element(b, b + r95, b + r99); p95 = values[r95];
                    std::nth_element(b, b + r50, b + r95); p50 = values[r50];
                }

                out.append(hour_strings[h]); out.push_back(' ');
                out.append(global_endpoints[e]); out.push_back(' ');
                char* lp = line_buf;
                lp = write_u64_fast(lp, n); *lp++ = ' ';
                lp = write_u64_fast(lp, p50); *lp++ = ' ';
                lp = write_u64_fast(lp, p95); *lp++ = ' ';
                lp = write_u64_fast(lp, p99); *lp++ = ' ';
                lp = write_u64_fast(lp, errors); *lp++ = ' ';
                lp = write_u64_fast(lp, (uint64_t)errors * 1000000ULL / n); *lp++ = '\n';
                out.append(line_buf, lp - line_buf);
            }
        }
    };

    workers.clear();
    for (unsigned int tid = 1; tid < nthreads; ++tid) workers.emplace_back(render_worker, tid);
    render_worker(0);
    for (auto& w : workers) w.join();

    FILE* fp = fopen(out_path.string().c_str(), "wb");
    if (fp) {
        for (const auto& chunk : thread_outputs) {
            if (!chunk.empty()) fwrite(chunk.data(), 1, chunk.size(), fp);
        }
        fclose(fp);
    }
    _exit(0);
}
