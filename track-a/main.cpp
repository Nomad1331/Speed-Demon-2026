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
#include <immintrin.h>
#include <atomic>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

static const char DIGITS_LUT[] = "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
inline char* write_u32_fast(char* p, uint32_t val) {
    if (val < 10) { *p++ = (char)('0' + val); return p; }
    char buf[12], *b = buf + 12;
    while (val >= 100) { uint32_t q = val / 100, r = val - q * 100; val = q; b -= 2; memcpy(b, &DIGITS_LUT[r * 2], 2); }
    if (val < 10) *--b = (char)('0' + val); else { b -= 2; memcpy(b, &DIGITS_LUT[val * 2], 2); }
    size_t len = (buf + 12) - b; memcpy(p, b, len); return p + len;
}
inline int u32_len(uint32_t v) {
    return 1 + (v >= 10) + (v >= 100) + (v >= 1000) + (v >= 10000) + (v >= 100000) + (v >= 1000000) + (v >= 10000000) + (v >= 100000000) + (v >= 1000000000);
}

template<char CH>
inline const char* find_char_avx2(const char* cur, const char* end, __m256i tgt) {
    if (cur >= end) return end;
    while (cur + 32 <= end) {
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)cur), tgt));
        if (m) return cur + __builtin_ctz(m);
        cur += 32;
    }
    while (cur < end && *cur != CH) ++cur; return cur;
}

inline uint64_t hash_endpoint(const char* s, size_t len) {
    uint64_t h = len * 11400714819323198485ULL; const char* end = s + len;
    while (s + 8 <= end) { uint64_t v; memcpy(&v, s, 8); h = _mm_crc32_u64(h, v); s += 8; }
    while (s < end) h = _mm_crc32_u64(h, (uint8_t)*s++);
    return h ? h : 1;
}

static constexpr size_t MAX_EP = 2048, H = 72, RAW_G = H * MAX_EP;
struct EpSlot { std::atomic<uint32_t> lock{0}; uint64_t hash = 0; uint32_t offset = 0; uint16_t len = 0, id = 0; };
static EpSlot ep_table[8192];
static std::atomic<uint16_t> ep_count{0};

inline uint16_t lookup_or_insert(uint16_t len, uint64_t h, uint32_t off) {
    size_t idx = h & 8191;
    while (true) {
        uint32_t state = ep_table[idx].lock.load(std::memory_order_acquire);
        if (state == 2) { if (ep_table[idx].hash == h && ep_table[idx].len == len) return ep_table[idx].id; }
        else if (state == 0) {
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
            if (ep_table[idx].hash == h && ep_table[idx].len == len) return ep_table[idx].id;
        }
        idx = (idx + 1) & 8191;
    }
}

struct LocalEpCache {
    struct Entry { uint64_t hash; uint16_t len, id; } entries[2048]{};
    inline uint16_t get_or_lookup(uint16_t len, uint64_t h, uint32_t off) {
        size_t idx = h & 2047;
        if (entries[idx].hash == h && entries[idx].len == len) return entries[idx].id;
        uint16_t id = lookup_or_insert(len, h, off);
        entries[idx] = { h, len, id }; return id;
    }
};

inline int get_hour_id(const char* p) {
    int d = (p[8] - '0') * 10 + (p[9] - '0'), h = (p[11] - '0') * 10 + (p[12] - '0');
    return ((p[6] == '0') ? (d - 30) : (d + 1)) * 24 + h - 18;
}

struct GroupQuantiles { uint32_t p50 = 0, p95 = 0, p99 = 0; };

int main(int argc, char* argv[]) {
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
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

    std::string hour_strings[H];
    { size_t idx = 0; auto add_h = [&](const char* pr, int s, int e) { for (int h = s; h <= e; ++h) { char buf[32]; snprintf(buf, sizeof(buf), "%sT%02d:00:00Z", pr, h); hour_strings[idx++] = buf; } };
      add_h("2026-10-30", 18, 23); add_h("2026-10-31", 0, 23); add_h("2026-11-01", 0, 23); add_h("2026-11-02", 0, 17); }

    unsigned int nthreads = (file_size < 100000) ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    struct Chunk { size_t start = 0, end = 0; };
    std::vector<Chunk> chunks(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t s = (file_size * i) / nthreads, e = (file_size * (i + 1)) / nthreads;
        if (i > 0) { while (s < file_size && file_data[s] != '\n') ++s; if (s < file_size) ++s; }
        if (i + 1 < nthreads) { while (e < file_size && file_data[e] != '\n') ++e; if (e < file_size) ++e; } else e = file_size;
        chunks[i] = {s, e};
    }

    size_t td_bytes = RAW_G * nthreads * sizeof(uint16_t);
    uint16_t* thread_data = (uint16_t*)::mmap(NULL, td_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t err_bytes = RAW_G * sizeof(std::atomic<uint32_t>);
    auto* atomic_errors = (std::atomic<uint32_t>*)::mmap(NULL, err_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    std::vector<std::string_view> sorted_endpoints;
    std::vector<uint16_t> ep_remap;
    size_t N = 0, G = 0, total_requests = 0, total_out_size = 0;
    std::vector<uint32_t> global_counts, global_errors;
    uint32_t* all_latencies = nullptr;
    std::vector<size_t> group_offsets, thread_output_bytes(nthreads, 0), thread_output_offsets(nthreads, 0);
    std::vector<GroupQuantiles> quantiles;
    std::barrier sync_point(nthreads);
    char* out_mmap_ptr = nullptr;
    int out_fd = -1;

    auto worker_fn = [&](unsigned int t) {
        LocalEpCache cache;
        const char* p = file_data + chunks[t].start, *end = file_data + chunks[t].end;
        uint64_t last_tag = 0; int last_hid = -1;
        __m256i sp = _mm256_set1_epi8(' ');

        while (p < end) {
            if (p + 35 > end) break;
            uint64_t cur_tag; memcpy(&cur_tag, p + 5, 8);
            int hid = (cur_tag == last_tag) ? last_hid : get_hour_id(p);
            if (cur_tag != last_tag) { last_tag = cur_tag; last_hid = hid; }
            const char* cur = p + 28;
            int off = (cur[3] == ' ') ? 4 : (cur[4] == ' ') ? 5 : (cur[5] == ' ') ? 6 : (cur[6] == ' ') ? 7 : (cur[7] == ' ') ? 8 : 0;
            if (off) cur += off; else { while (*cur != ' ') ++cur; ++cur; }
            const char* ep_start = cur; cur = find_char_avx2<' '>(cur, end, sp);
            uint16_t ep_len = (uint16_t)(cur - ep_start);
            uint64_t h = hash_endpoint(ep_start, ep_len);
            uint16_t ep_id = cache.get_or_lookup(ep_len, h, (uint32_t)(ep_start - file_data));
            size_t cell = (size_t)hid * MAX_EP + ep_id;
            thread_data[cell * nthreads + t]++;
            if (cur[1] == '5') atomic_errors[cell].fetch_add(1, std::memory_order_relaxed);
            cur += 5;
            while (*cur != '\n') ++cur;
            p = cur + 1;
        }
        sync_point.arrive_and_wait();

        if (t == 0) {
            N = ep_count.load(std::memory_order_relaxed); G = H * N;
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
            std::vector<uint16_t> sorted_to_orig(N);
            for (uint16_t i = 0; i < N; ++i) {
                sorted_endpoints[i] = refs[i].sv;
                ep_remap[refs[i].orig_id] = i;
                sorted_to_orig[i] = refs[i].orig_id;
            }
            global_counts.assign(G, 0); global_errors.assign(G, 0); group_offsets.resize(G + 1, 0);

            uint32_t total = 0;
            for (size_t h = 0; h < H; ++h) {
                for (size_t ep = 0; ep < N; ++ep) {
                    uint16_t orig_id = sorted_to_orig[ep];
                    size_t cell = h * MAX_EP + orig_id, g = h * N + ep;
                    group_offsets[g] = total;
                    uint16_t* cell_row = thread_data + cell * nthreads;
                    uint32_t g_count = 0;
                    for (unsigned int ot = 0; ot < nthreads; ++ot) {
                        uint16_t c = cell_row[ot];
                        cell_row[ot] = (uint16_t)g_count;
                        g_count += c;
                    }
                    global_counts[g] = g_count;
                    global_errors[g] = atomic_errors[cell].load(std::memory_order_relaxed);
                    total += g_count;
                }
            }
            group_offsets[G] = total; total_requests = total;
            all_latencies = (uint32_t*)::mmap(NULL, total * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            quantiles.resize(G);
        }
        sync_point.arrive_and_wait();

        if (N > 0) {
            p = file_data + chunks[t].start; last_tag = 0; last_hid = -1;
            while (p < end) {
                if (p + 35 > end) break;
                uint64_t cur_tag; memcpy(&cur_tag, p + 5, 8);
                int hid = (cur_tag == last_tag) ? last_hid : get_hour_id(p);
                if (cur_tag != last_tag) { last_tag = cur_tag; last_hid = hid; }
                const char* cur = p + 28;
                int off = (cur[3] == ' ') ? 4 : (cur[4] == ' ') ? 5 : (cur[5] == ' ') ? 6 : (cur[6] == ' ') ? 7 : (cur[7] == ' ') ? 8 : 0;
                if (off) cur += off; else { while (*cur != ' ') ++cur; ++cur; }
                const char* ep_start = cur; cur = find_char_avx2<' '>(cur, end, sp);
                uint16_t ep_len = (uint16_t)(cur - ep_start);
                uint64_t h = hash_endpoint(ep_start, ep_len);
                uint16_t ep_id = cache.get_or_lookup(ep_len, h, (uint32_t)(ep_start - file_data));
                size_t cell = (size_t)hid * MAX_EP + ep_id;
                size_t g = (size_t)hid * N + ep_remap[ep_id];
                cur += 5;
                uint32_t lat = (uint8_t)(*cur++ - '0');
                while ((uint8_t)(*cur - '0') <= 9) lat = lat * 10 + (uint8_t)(*cur++ - '0');
                all_latencies[group_offsets[g] + thread_data[cell * nthreads + t]++] = lat;
                while (*cur != '\n') ++cur;
                p = cur + 1;
            }
        }
        sync_point.arrive_and_wait();

        if (t == 0) {
            ::munmap(thread_data, td_bytes);
            ::munmap(atomic_errors, err_bytes);
        }

        size_t total_lines = group_offsets[G];
        size_t t_s = (total_lines * t) / nthreads, t_e = (total_lines * (t + 1)) / nthreads;
        size_t g_s = std::lower_bound(group_offsets.begin(), group_offsets.end(), t_s) - group_offsets.begin();
        size_t g_e = (t == nthreads - 1) ? G : (size_t)(std::lower_bound(group_offsets.begin(), group_offsets.end(), t_e) - group_offsets.begin());

        size_t my_bytes = 0;
        for (size_t g = g_s; g < g_e; ++g) {
            size_t n = global_counts[g]; if (n == 0) continue;
            uint32_t* begin = all_latencies + group_offsets[g], *end_lat = begin + n, p50, p95, p99;
            if (n == 1) { p50 = p95 = p99 = begin[0]; }
            else if (n == 2) { if (begin[0] > begin[1]) std::swap(begin[0], begin[1]); p50 = begin[0]; p95 = p99 = begin[1]; }
            else {
                size_t r50 = (50 * n + 99) / 100 - 1, r95 = (95 * n + 99) / 100 - 1, r99 = (99 * n + 99) / 100 - 1;
                if (n <= 64) { std::sort(begin, end_lat); p50 = begin[r50]; p95 = begin[r95]; p99 = begin[r99]; }
                else {
                    std::nth_element(begin, begin + r99, end_lat); p99 = begin[r99];
                    std::nth_element(begin, begin + r95, begin + r99); p95 = begin[r95];
                    std::nth_element(begin, begin + r50, begin + r95); p50 = begin[r50];
                }
            }
            quantiles[g] = { p50, p95, p99 };
            size_t gep = g % N; uint32_t errs = global_errors[g], ppm = (uint32_t)(((uint64_t)errs * 1000000ULL) / n);
            my_bytes += 21 + sorted_endpoints[gep].size() + 1 + u32_len((uint32_t)n) + 1 +
                        u32_len(p50) + 1 + u32_len(p95) + 1 + u32_len(p99) + 1 +
                        u32_len(errs) + 1 + u32_len(ppm) + 1;
        }
        thread_output_bytes[t] = my_bytes;
        sync_point.arrive_and_wait();

        if (t == 0) {
            ::munmap(all_latencies, total_requests * sizeof(uint32_t));
            size_t total_bytes = 0;
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                thread_output_offsets[ot] = total_bytes; total_bytes += thread_output_bytes[ot];
            }
            total_out_size = total_bytes;
            out_fd = ::open(out_path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
            if (out_fd >= 0 && total_bytes > 0) {
                ftruncate(out_fd, total_bytes);
                out_mmap_ptr = (char*)::mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0);
            }
        }
        sync_point.arrive_and_wait();

        if (out_mmap_ptr && out_mmap_ptr != MAP_FAILED) {
            char* p_out = out_mmap_ptr + thread_output_offsets[t];
            for (size_t g = g_s; g < g_e; ++g) {
                uint32_t count = global_counts[g]; if (count == 0) continue;
                size_t h = g / N, gep = g % N;
                const auto& h_str = hour_strings[h]; const auto& ep = sorted_endpoints[gep];
                const auto& q = quantiles[g];
                uint32_t errs = global_errors[g], ppm = (uint32_t)(((uint64_t)errs * 1000000ULL) / count);
                memcpy(p_out, h_str.data(), 20); p_out += 20; *p_out++ = ' ';
                memcpy(p_out, ep.data(), ep.size()); p_out += ep.size(); *p_out++ = ' ';
                p_out = write_u32_fast(p_out, count); *p_out++ = ' ';
                p_out = write_u32_fast(p_out, q.p50); *p_out++ = ' ';
                p_out = write_u32_fast(p_out, q.p95); *p_out++ = ' ';
                p_out = write_u32_fast(p_out, q.p99); *p_out++ = ' ';
                p_out = write_u32_fast(p_out, errs); *p_out++ = ' ';
                p_out = write_u32_fast(p_out, ppm); *p_out++ = '\n';
            }
        }
    };

    std::vector<std::thread> workers; workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) workers.emplace_back(worker_fn, t);
    worker_fn(0);
    for (auto& w : workers) w.join();

    if (out_mmap_ptr && out_mmap_ptr != MAP_FAILED) munmap(out_mmap_ptr, total_out_size);
    if (out_fd >= 0) ::close(out_fd);
    munmap((void*)file_data, file_size);
    ::close(in_fd);
    return 0;
}
