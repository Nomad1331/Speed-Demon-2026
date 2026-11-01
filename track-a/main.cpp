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
#include <memory>
#include <fcntl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
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

inline uint32_t u32_len(uint32_t v) {
    if (v < 10) return 1; if (v < 100) return 2; if (v < 1000) return 3; if (v < 10000) return 4;
    if (v < 100000) return 5; if (v < 1000000) return 6; if (v < 10000000) return 7;
    return (v < 100000000) ? 8 : (v < 1000000000 ? 9 : 10);
}

struct MappedFile {
    const char* data = nullptr; size_t size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = NULL;
    bool open(const std::string& p) {
        hFile = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER fs; if (!GetFileSizeEx(hFile, &fs) || (size = (size_t)fs.QuadPart) == 0) { CloseHandle(hFile); return false; }
        return (hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL)) && (data = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0));
    }
    void close() { if (data) UnmapViewOfFile(data); if (hMap) CloseHandle(hMap); if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile); data = nullptr; }
#else
    int fd = -1;
    bool open(const std::string& p) {
        if ((fd = ::open(p.c_str(), O_RDONLY)) < 0) return false;
        struct stat st; if (fstat(fd, &st) < 0 || (size = (size_t)st.st_size) == 0) { ::close(fd); return false; }
        data = (const char*)::mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data != MAP_FAILED) madvise((void*)data, size, MADV_SEQUENTIAL);
        return data != MAP_FAILED;
    }
    void close() { if (data && data != MAP_FAILED) { madvise((void*)data, size, MADV_DONTNEED); munmap((void*)data, size); } if (fd >= 0) ::close(fd); data = nullptr; }
#endif
    ~MappedFile() { close(); }
};

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

static constexpr size_t MAX_EP = 2048, MAX_H = 72;
struct SharedSlot { std::atomic<uint32_t> lock{0}; uint64_t hash = 0; uint32_t offset = 0; uint16_t len = 0, id = 0; };
struct SharedEpTable {
    static constexpr size_t CAP = 8192, MASK = CAP - 1;
    SharedSlot slots[CAP];
    std::atomic<uint16_t> ep_count{0};
    inline uint16_t lookup_or_insert(uint16_t len, uint64_t h, size_t file_offset) {
        size_t idx = h & MASK;
        while (true) {
            uint32_t state = slots[idx].lock.load(std::memory_order_acquire);
            if (state == 2) { if (slots[idx].hash == h && slots[idx].len == len) return slots[idx].id; }
            else if (state == 0) {
                uint32_t expected = 0;
                if (slots[idx].lock.compare_exchange_strong(expected, 1, std::memory_order_relaxed)) {
                    uint16_t id = ep_count.fetch_add(1, std::memory_order_relaxed);
                    slots[idx].hash = h; slots[idx].offset = (uint32_t)file_offset; slots[idx].len = len; slots[idx].id = id;
                    slots[idx].lock.store(2, std::memory_order_release);
                    return id;
                }
                continue;
            } else if (state == 1) {
                while (slots[idx].lock.load(std::memory_order_acquire) == 1) _mm_pause();
                if (slots[idx].hash == h && slots[idx].len == len) return slots[idx].id;
            }
            idx = (idx + 1) & MASK;
        }
    }
};

struct LocalEpCache {
    struct Entry { uint64_t hash; uint16_t len, id; };
    Entry entries[512]{};
    inline uint16_t get_or_lookup(SharedEpTable& tbl, uint16_t len, uint64_t h, size_t off) {
        size_t idx = h & 511;
        if (entries[idx].hash == h && entries[idx].len == len) return entries[idx].id;
        uint16_t id = tbl.lookup_or_insert(len, h, off);
        entries[idx] = { h, len, id };
        return id;
    }
};

inline int get_hour_id(const char* p) {
    int d = (p[8] - '0') * 10 + (p[9] - '0'), h = (p[11] - '0') * 10 + (p[12] - '0');
    return ((p[6] == '0') ? (d - 30) : (d + 1)) * 24 + h - 18;
}

struct FastLine { uint16_t ep_id; uint16_t lat_offset; };
struct GroupQuantiles { uint32_t p50 = 0, p95 = 0, p99 = 0; };

int main(int argc, char* argv[]) {
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
    }
    std::filesystem::path in_path = std::filesystem::path(data_dir) / "requests.log", out_path = std::filesystem::path(out_dir) / "summary.txt";
    std::filesystem::create_directories(out_dir);
    MappedFile mf;
    if (!mf.open(in_path.string()) || mf.size == 0) { FILE* f = fopen(out_path.string().c_str(), "wb"); if (f) fclose(f); return 0; }

    static const size_t H = 72;
    std::string hour_strings[H];
    { size_t idx = 0; auto add_h = [&](const char* pr, int s, int e) { for (int h = s; h <= e; ++h) { char buf[32]; snprintf(buf, sizeof(buf), "%sT%02d:00:00Z", pr, h); hour_strings[idx++] = buf; } };
      add_h("2026-10-30", 18, 23); add_h("2026-10-31", 0, 23); add_h("2026-11-01", 0, 23); add_h("2026-11-02", 0, 17); }

    unsigned int nthreads = (mf.size < 100000) ? 1u : std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    struct Chunk { size_t start = 0, end = 0; };
    std::vector<Chunk> chunks(nthreads);
    for (unsigned int i = 0; i < nthreads; ++i) {
        size_t s = (mf.size * i) / nthreads, e = (mf.size * (i + 1)) / nthreads;
        if (i > 0) { while (s < mf.size && mf.data[s] != '\n') ++s; if (s < mf.size) ++s; }
        if (i + 1 < nthreads) { while (e < mf.size && mf.data[e] != '\n') ++e; if (e < mf.size) ++e; } else e = mf.size;
        chunks[i] = {s, e};
    }
    SharedEpTable shared_ep;
    static constexpr size_t RAW_G = H * MAX_EP;
    auto atomic_counts = std::make_unique<std::atomic<uint32_t>[]>(RAW_G);
    auto atomic_errors = std::make_unique<std::atomic<uint32_t>[]>(RAW_G);
    for (size_t i = 0; i < RAW_G; ++i) { atomic_counts[i].store(0, std::memory_order_relaxed); atomic_errors[i].store(0, std::memory_order_relaxed); }

    std::vector<std::vector<FastLine>> thread_lines(nthreads);
    std::vector<std::string_view> sorted_endpoints;
    std::vector<uint16_t> ep_remap;
    size_t N = 0, G = 0, total_out_size = 0;
    std::vector<uint32_t> global_counts, global_errors, all_latencies;
    std::vector<size_t> group_offsets, thread_output_bytes(nthreads, 0), thread_output_offsets(nthreads, 0);
    std::vector<GroupQuantiles> quantiles;
    std::unique_ptr<std::atomic<uint32_t>[]> atomic_write_offsets;
    std::barrier sync_point(nthreads);
    char* out_mmap_ptr = nullptr;
#ifdef _WIN32
    HANDLE hOutFile = INVALID_HANDLE_VALUE, hOutMap = NULL;
#else
    int out_fd = -1;
#endif

    auto worker_fn = [&](unsigned int t) {
        LocalEpCache cache;
        auto& my_lines = thread_lines[t];
        my_lines.reserve(std::min(size_t(280000), (chunks[t].end - chunks[t].start) / 50 + 100));
        const char* p = mf.data + chunks[t].start, *end = mf.data + chunks[t].end;
        uint64_t last_tag = 0; int last_hid = -1;
        __m256i sp = _mm256_set1_epi8(' ');

        while (p < end) {
            if (p + 35 > end) break;
            const char* line_start = p;
            uint64_t cur_tag; memcpy(&cur_tag, p + 5, 8);
            int hid = (cur_tag == last_tag) ? last_hid : get_hour_id(p);
            if (cur_tag != last_tag) { last_tag = cur_tag; last_hid = hid; }
            const char* cur = p + 28;
            int off = (cur[3] == ' ') ? 4 : (cur[4] == ' ') ? 5 : (cur[5] == ' ') ? 6 : (cur[6] == ' ') ? 7 : (cur[7] == ' ') ? 8 : 0;
            if (off) cur += off; else { while (*cur != ' ') ++cur; ++cur; }
            const char* ep_start = cur; cur = find_char_avx2<' '>(cur, end, sp);
            uint16_t ep_len = (uint16_t)(cur - ep_start);
            uint64_t h = hash_endpoint(ep_start, ep_len);
            uint16_t ep_id = cache.get_or_lookup(shared_ep, ep_len, h, ep_start - mf.data);
            size_t cell = (size_t)hid * MAX_EP + ep_id;
            atomic_counts[cell].fetch_add(1, std::memory_order_relaxed);
            if (cur[1] == '5') atomic_errors[cell].fetch_add(1, std::memory_order_relaxed);
            cur += 5;
            my_lines.push_back({ep_id, (uint16_t)(cur - line_start)});
            while (*cur != '\n') ++cur; ++cur; p = cur;
        }
        sync_point.arrive_and_wait();

        if (t == 0) {
            N = shared_ep.ep_count.load(std::memory_order_relaxed); G = H * N;
            struct EpRef { std::string_view sv; uint16_t orig_id; };
            std::vector<EpRef> refs(N);
            for (size_t i = 0; i < SharedEpTable::CAP; ++i) {
                if (shared_ep.slots[i].lock.load(std::memory_order_relaxed) == 2) {
                    uint16_t id = shared_ep.slots[i].id;
                    refs[id] = { std::string_view(mf.data + shared_ep.slots[i].offset, shared_ep.slots[i].len), id };
                }
            }
            std::sort(refs.begin(), refs.end(), [](const EpRef& a, const EpRef& b) { return a.sv < b.sv; });
            sorted_endpoints.resize(N); ep_remap.resize(N);
            for (uint16_t i = 0; i < N; ++i) { sorted_endpoints[i] = refs[i].sv; ep_remap[refs[i].orig_id] = i; }
            global_counts.assign(G, 0); global_errors.assign(G, 0);
        }
        sync_point.arrive_and_wait();

        if (N > 0) {
            size_t h_start = (H * t) / nthreads, h_end = (H * (t + 1)) / nthreads;
            for (size_t h = h_start; h < h_end; ++h) {
                size_t base = h * MAX_EP;
                for (size_t ep = 0; ep < N; ++ep) {
                    uint32_t c = atomic_counts[base + ep].load(std::memory_order_relaxed);
                    if (c) {
                        size_t g = h * N + ep_remap[ep];
                        global_counts[g] = c; global_errors[g] = atomic_errors[base + ep].load(std::memory_order_relaxed);
                    }
                }
            }
        }
        sync_point.arrive_and_wait();

        if (t == 0) {
            atomic_counts.reset(); atomic_errors.reset();
            group_offsets.resize(G + 1, 0); atomic_write_offsets = std::make_unique<std::atomic<uint32_t>[]>(G);
            size_t total = 0;
            for (size_t g = 0; g < G; ++g) {
                group_offsets[g] = (uint32_t)total; atomic_write_offsets[g].store((uint32_t)total, std::memory_order_relaxed); total += global_counts[g];
            }
            group_offsets[G] = (uint32_t)total; all_latencies.resize(total); quantiles.resize(G);
        }
        sync_point.arrive_and_wait();

        if (N == 0) return;

        p = mf.data + chunks[t].start; last_tag = 0; last_hid = -1;
        for (const auto& line : my_lines) {
            uint64_t cur_tag; memcpy(&cur_tag, p + 5, 8);
            int hid = (cur_tag == last_tag) ? last_hid : get_hour_id(p);
            if (cur_tag != last_tag) { last_tag = cur_tag; last_hid = hid; }
            const char* lat_ptr = p + line.lat_offset;
            uint32_t lat = (uint8_t)(*lat_ptr++ - '0');
            while ((uint8_t)(*lat_ptr - '0') <= 9) lat = lat * 10 + (uint8_t)(*lat_ptr++ - '0');
            uint32_t ggid = (uint32_t)hid * N + ep_remap[line.ep_id];
            all_latencies[atomic_write_offsets[ggid].fetch_add(1, std::memory_order_relaxed)] = lat;
            while (*lat_ptr != '\n') ++lat_ptr;
            p = lat_ptr + 1;
        }
        my_lines.clear(); my_lines.shrink_to_fit();
        sync_point.arrive_and_wait();

        if (t == 0) atomic_write_offsets.reset();

        size_t total_lines = group_offsets[G];
        size_t t_s = (total_lines * t) / nthreads, t_e = (total_lines * (t + 1)) / nthreads;
        size_t g_s = std::lower_bound(group_offsets.begin(), group_offsets.end(), t_s) - group_offsets.begin();
        size_t g_e = (t == nthreads - 1) ? G : (size_t)(std::lower_bound(group_offsets.begin(), group_offsets.end(), t_e) - group_offsets.begin());

        size_t my_bytes = 0;
        for (size_t g = g_s; g < g_e; ++g) {
            size_t n = global_counts[g]; if (n == 0) continue;
            uint32_t* begin = &all_latencies[group_offsets[g]], *end_lat = begin + n, p50, p95, p99;
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
            all_latencies.clear(); all_latencies.shrink_to_fit();
            size_t total_bytes = 0;
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                thread_output_offsets[ot] = total_bytes; total_bytes += thread_output_bytes[ot];
            }
            total_out_size = total_bytes;

#ifdef _WIN32
            hOutFile = CreateFileA(out_path.string().c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hOutFile != INVALID_HANDLE_VALUE && total_bytes > 0) {
                LARGE_INTEGER li; li.QuadPart = (LONGLONG)total_bytes;
                SetFilePointerEx(hOutFile, li, NULL, FILE_BEGIN);
                SetEndOfFile(hOutFile);
                hOutMap = CreateFileMappingA(hOutFile, NULL, PAGE_READWRITE, 0, 0, NULL);
                if (hOutMap) out_mmap_ptr = (char*)MapViewOfFile(hOutMap, FILE_MAP_WRITE, 0, 0, 0);
            }
#else
            out_fd = ::open(out_path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
            if (out_fd >= 0 && total_bytes > 0) {
                ftruncate(out_fd, total_bytes);
                out_mmap_ptr = (char*)::mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0);
            }
#endif
        }
        sync_point.arrive_and_wait();

        if (out_mmap_ptr) {
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

#ifdef _WIN32
    if (out_mmap_ptr) UnmapViewOfFile(out_mmap_ptr);
    if (hOutMap) CloseHandle(hOutMap);
    if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
#else
    if (out_mmap_ptr && out_mmap_ptr != MAP_FAILED) munmap(out_mmap_ptr, total_out_size);
    if (out_fd >= 0) ::close(out_fd);
#endif
    return 0;
}
