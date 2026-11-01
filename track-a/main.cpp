#include <algorithm>
#include <atomic>
#include <barrier>
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
static constexpr size_t MAX_EP = 2048;
static constexpr size_t TOTAL_GROUPS = H * MAX_EP;

struct EpSlot {
    std::atomic<uint32_t> lock{0};
    uint64_t hash = 0;
    uint32_t offset = 0;
    uint16_t len = 0, id = 0;
};
static EpSlot ep_table[8192];
static std::atomic<uint16_t> ep_count{0};

inline uint16_t lookup_or_insert_ep(uint16_t len, uint64_t h, uint32_t off, const char* file_base) {
    size_t idx = h & 8191;
    while (true) {
        uint32_t state = ep_table[idx].lock.load(std::memory_order_acquire);
        if (state == 2) {
            if (ep_table[idx].hash == h && ep_table[idx].len == len && memcmp(file_base + ep_table[idx].offset, file_base + off, len) == 0)
                return ep_table[idx].id;
        } else if (state == 0) {
            uint32_t exp = 0;
            if (ep_table[idx].lock.compare_exchange_strong(exp, 1, std::memory_order_relaxed)) {
                uint16_t id = ep_count.fetch_add(1, std::memory_order_relaxed);
                ep_table[idx].hash = h; ep_table[idx].offset = off; ep_table[idx].len = len; ep_table[idx].id = id;
                ep_table[idx].lock.store(2, std::memory_order_release);
                return id;
            }
            continue;
        } else if (state == 1) {
            while (ep_table[idx].lock.load(std::memory_order_acquire) == 1) _mm_pause();
            if (ep_table[idx].hash == h && ep_table[idx].len == len && memcmp(file_base + ep_table[idx].offset, file_base + off, len) == 0)
                return ep_table[idx].id;
        }
        idx = (idx + 1) & 8191;
    }
}

struct LocalEpCache {
    struct Entry { uint64_t hash; uint16_t len, id; } entries[2048]{};
    inline uint16_t get_or_lookup(uint16_t len, uint64_t h, uint32_t off, const char* file_base) {
        size_t idx = h & 2047;
        if (entries[idx].hash == h && entries[idx].len == len) return entries[idx].id;
        uint16_t id = lookup_or_insert_ep(len, h, off, file_base);
        entries[idx] = { h, len, id }; return id;
    }
};

int main(int argc, char** argv) {
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (arg.starts_with("--data=")) data_dir = arg.substr(7);
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg.starts_with("--out=")) out_dir = arg.substr(6);
        else if (arg[0] != '-') {
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
    struct Chunk { size_t start = 0, end = 0; };
    std::vector<Chunk> chunks(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t s = (file_size * i) / nthreads, e = (file_size * (i + 1)) / nthreads;
        if (i > 0) { while (s < file_size && file_data[s] != '\n') ++s; if (s < file_size) ++s; }
        if (i + 1 < nthreads) { while (e < file_size && file_data[e] != '\n') ++e; if (e < file_size) ++e; } else e = file_size;
        chunks[i] = {s, e};
    }

    // Per-thread storage: single-pass latency ingestion
    size_t max_thread_items = (file_size / 60 / nthreads) + 10000;
    if (max_thread_items < 1000) max_thread_items = 1000;

    std::vector<std::vector<uint32_t>> thread_latencies(nthreads);
    std::vector<std::vector<uint32_t>> thread_cells(nthreads);
    std::vector<std::vector<uint32_t>> thread_cell_counts(nthreads);
    std::vector<uint32_t> global_errors(TOTAL_GROUPS, 0);

    for (unsigned int i = 0; i < nthreads; ++i) {
        thread_latencies[i].reserve(max_thread_items);
        thread_cells[i].reserve(max_thread_items);
        thread_cell_counts[i].assign(TOTAL_GROUPS, 0);
    }

    std::barrier sync_point(nthreads);
    size_t N = 0;
    std::vector<std::string_view> sorted_endpoints;
    std::vector<uint16_t> ep_remap;
    std::vector<uint32_t> global_group_offsets;
    std::vector<uint32_t> all_latencies;
    std::vector<std::string> thread_outputs(nthreads);

    auto worker_fn = [&](unsigned int tid) {
        LocalEpCache cache;
        const char* p = file_data + chunks[tid].start, *end = file_data + chunks[tid].end;
        uint64_t last_tag = 0; int last_hid = -1;

        auto& my_lats = thread_latencies[tid];
        auto& my_cells = thread_cells[tid];
        auto& my_counts = thread_cell_counts[tid];

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
            uint16_t ep_id = cache.get_or_lookup(ep_len, h, (uint32_t)(ep_start - file_data), file_data);

            bool is_err = (cur[1] == '5');
            cur += 5;
            uint32_t lat = (uint8_t)(*cur++ - '0');
            while ((uint8_t)(*cur - '0') <= 9) lat = lat * 10 + (uint8_t)(*cur++ - '0');

            uint32_t cell = (uint32_t)hid * (uint32_t)MAX_EP + ep_id;
            my_lats.push_back(lat);
            my_cells.push_back(cell);
            my_counts[cell]++;
            if (is_err) __atomic_fetch_add(&global_errors[cell], 1, __ATOMIC_RELAXED);

            p = (*cur == '\n') ? cur + 1 : find_char_avx2(cur, end, '\n') + 1;
        }

        sync_point.arrive_and_wait();

        // Single thread sorts endpoints and sets up global offsets
        if (tid == 0) {
            N = ep_count.load(std::memory_order_relaxed);
            struct EpRef { std::string_view sv; uint16_t orig_id; };
            std::vector<EpRef> refs(N);
            for (size_t i = 0; i < 8192; ++i) {
                if (ep_table[i].lock.load(std::memory_order_relaxed) == 2) {
                    uint16_t id = ep_table[i].id;
                    refs[id] = { std::string_view(file_data + ep_table[i].offset, ep_table[i].len), id };
                }
            }
            std::sort(refs.begin(), refs.end(), [](const EpRef& a, const EpRef& b) { return a.sv < b.sv; });
            sorted_endpoints.resize(N); ep_remap.resize(N);
            for (uint16_t i = 0; i < N; ++i) {
                sorted_endpoints[i] = refs[i].sv;
                ep_remap[refs[i].orig_id] = i;
            }

            // Remap cells to compact groups: group = hid * N + ep_remap[orig_ep]
            size_t G = H * N;
            global_group_offsets.assign(G + 1, 0);

            // Compute total counts per group and per-thread write cursors
            uint32_t total = 0;
            for (int h = 0; h < H; ++h) {
                for (size_t ep = 0; ep < N; ++ep) {
                    uint16_t orig_id = refs[ep].orig_id;
                    uint32_t cell = (uint32_t)h * (uint32_t)MAX_EP + orig_id;
                    size_t g = (size_t)h * N + ep;
                    global_group_offsets[g] = total;

                    uint32_t g_count = 0;
                    for (unsigned int ot = 0; ot < nthreads; ++ot) {
                        uint32_t cnt = thread_cell_counts[ot][cell];
                        thread_cell_counts[ot][cell] = total + g_count;
                        g_count += cnt;
                    }
                    total += g_count;
                }
            }
            global_group_offsets[G] = total;
            all_latencies.resize(total);
        }

        sync_point.arrive_and_wait();

        // Phase 2: Scatter thread-local latencies directly into global all_latencies array
        size_t n_items = my_lats.size();
        for (size_t i = 0; i < n_items; ++i) {
            uint32_t cell = my_cells[i];
            uint32_t write_pos = thread_cell_counts[tid][cell]++;
            all_latencies[write_pos] = my_lats[i];
        }

        sync_point.arrive_and_wait();

        // Phase 3: Compute quantiles and render output in parallel
        size_t G = H * N;
        size_t g_start = (G * tid) / nthreads, g_end = (G * (tid + 1)) / nthreads;
        std::string& out = thread_outputs[tid];
        out.reserve(N * 72);
        char line_buf[256];

        for (size_t g = g_start; g < g_end; ++g) {
            uint32_t off_start = global_group_offsets[g];
            uint32_t off_end = global_group_offsets[g + 1];
            uint32_t n = off_end - off_start;
            if (n == 0) continue;

            int h = (int)(g / N);
            size_t ep = g % N;

            uint32_t* begin = all_latencies.data() + off_start;
            uint32_t* end_lat = begin + n;
            size_t r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
            uint32_t p50, p95, p99;

            if (n <= 64) {
                std::sort(begin, end_lat);
                p50 = begin[r50]; p95 = begin[r95]; p99 = begin[r99];
            } else {
                std::nth_element(begin, begin + r99, end_lat); p99 = begin[r99];
                std::nth_element(begin, begin + r95, begin + r99); p95 = begin[r95];
                std::nth_element(begin, begin + r50, begin + r95); p50 = begin[r50];
            }

            uint16_t orig_id = 0;
            for (size_t k = 0; k < 8192; ++k) {
                if (ep_table[k].lock.load(std::memory_order_relaxed) == 2 && ep_remap[ep_table[k].id] == ep) {
                    orig_id = ep_table[k].id; break;
                }
            }
            uint32_t cell = (uint32_t)h * (uint32_t)MAX_EP + orig_id;
            uint32_t errors = global_errors[cell];

            out.append(hour_strings[h]); out.push_back(' ');
            out.append(sorted_endpoints[ep]); out.push_back(' ');
            char* lp = line_buf;
            lp = write_u64_fast(lp, n); *lp++ = ' ';
            lp = write_u64_fast(lp, p50); *lp++ = ' ';
            lp = write_u64_fast(lp, p95); *lp++ = ' ';
            lp = write_u64_fast(lp, p99); *lp++ = ' ';
            lp = write_u64_fast(lp, errors); *lp++ = ' ';
            lp = write_u64_fast(lp, (uint64_t)errors * 1000000ULL / n); *lp++ = '\n';
            out.append(line_buf, lp - line_buf);
        }
    };

    std::vector<std::thread> workers;
    for (unsigned int tid = 1; tid < nthreads; ++tid) workers.emplace_back(worker_fn, tid);
    worker_fn(0);
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
