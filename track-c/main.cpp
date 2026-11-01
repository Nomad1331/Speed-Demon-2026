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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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

struct MappedFile {
    const char* data = nullptr; size_t size = 0; int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY); if (fd < 0) return false;
        struct stat st; fstat(fd, &st); size = (size_t)st.st_size; if (!size) return true;
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
    uint64_t count = 0, name_bytes = 0, note_bytes = 0;
    int64_t id_sum = 0, amount_sum = 0, amount_min = 0, amount_max = 0;
    uint32_t distinct_names = 0;
};
struct ThreadData { LocalCountryAgg countries[241]; };
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
        uint16_t pack = (uint16_t)(((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A'));
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
        uint32_t rid = (uint8_t)(*p++ - '0');
        while ((uint8_t)(*p - '0') <= 9) rid = rid * 10 + (uint8_t)(*p++ - '0');
        if (p >= rend) break;
        p++;
        uint16_t pack = (uint16_t)(((p[0] - 'A') * 26 + (p[1] - 'A')) * 26 + (p[2] - 'A'));
        if (rid < 1000000) region_to_country[rid] = code_to_cid[pack];
        p += 3; while (p < rend && *p != '\n') p++;
        if (p < rend && *p == '\n') p++;
    }
    mf_regions.close();

    NameSlot* name_table = (NameSlot*)::mmap(NULL, sizeof(NameSlot) * HASH_CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (name_table == MAP_FAILED) return 1;

    MappedFile mf_records; if (!mf_records.open((std::filesystem::path(data_dir) / "records.csv").string())) return 1;
    const char* ptr0 = mf_records.data, *file_end = mf_records.data + mf_records.size;
    if (ptr0 + 3 <= file_end && (uint8_t)ptr0[0] == 0xef && (uint8_t)ptr0[1] == 0xbb && (uint8_t)ptr0[2] == 0xbf) ptr0 += 3;
    while (ptr0 < file_end && *ptr0 != '\n') ptr0++;
    if (ptr0 < file_end && *ptr0 == '\n') ptr0++;
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
    std::vector<ThreadData> tdata(nthreads);
    std::vector<size_t> thread_output_bytes(nthreads, 0), thread_output_offsets(nthreads, 0);
    std::barrier sync_point(nthreads);
    char* out_mmap_ptr = nullptr;
    size_t total_out_size = 0;
    int out_fd = -1;

    auto worker_fn = [&](unsigned int t) {
        const char* ptr = chunks[t].start, *chunk_end = chunks[t].end;
        auto& my_countries = tdata[t].countries;
        __m256i v_comma = _mm256_set1_epi8(','), v_quote = _mm256_set1_epi8('"');

        while (ptr < chunk_end) {
            if (ptr + 3 <= chunk_end && (uint8_t)ptr[0] == 0xef && (uint8_t)ptr[1] == 0xbb && (uint8_t)ptr[2] == 0xbf) ptr += 3;
            uint64_t id = (uint8_t)(*ptr++ - '0');
            while ((uint8_t)(*ptr - '0') <= 9) id = id * 10 + (uint8_t)(*ptr++ - '0');
            ptr++;
            uint32_t crc = 0; size_t name_len = 0;
            if (__builtin_expect(*ptr == '"', 1)) {
                ptr++;
                while (true) {
                    char c = *ptr++;
                    if (__builtin_expect(c != '"', 1)) { crc = _mm_crc32_u8(crc, (uint8_t)c); name_len++; }
                    else { if (*ptr == '"') { ptr++; crc = _mm_crc32_u8(crc, '"'); name_len++; } else break; }
                }
                ptr++;
            } else {
                while (*ptr != ',') { crc = _mm_crc32_u8(crc, (uint8_t)*ptr++); name_len++; }
                ptr++;
            }
            uint64_t name_hash = ((uint64_t)name_len << 32) | crc;

            size_t note_len = 0;
            if (__builtin_expect(*ptr == '"', 1)) {
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

            uint32_t region_id = (uint8_t)(*ptr++ - '0');
            while ((uint8_t)(*ptr - '0') <= 9) region_id = region_id * 10 + (uint8_t)(*ptr++ - '0');
            ptr++; bool neg = (*ptr == '-'); ptr += neg;
            int64_t cents = (uint8_t)(*ptr++ - '0');
            while ((uint8_t)(*ptr - '0') <= 9) cents = cents * 10 + (uint8_t)(*ptr++ - '0');
            ptr++; cents = cents * 100 + (uint8_t)(ptr[0] - '0') * 10 + (uint8_t)(ptr[1] - '0'); ptr += 2;
            if (neg) cents = -cents;
            if (__builtin_expect(ptr < chunk_end, 1)) { ptr += (*ptr == '\r'); ptr += (*ptr == '\n'); }

            uint8_t cid = (region_id < 1000000) ? region_to_country[region_id] : 0;
            LocalCountryAgg& agg = my_countries[cid];
            if (__builtin_expect(agg.count == 0, 0)) { agg.amount_min = agg.amount_max = cents; }
            else { agg.amount_min = std::min(agg.amount_min, cents); agg.amount_max = std::max(agg.amount_max, cents); }
            agg.count++; agg.id_sum += (int64_t)id; agg.amount_sum += cents;
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
            uint32_t word = cid >> 6;
            uint64_t mask = 1ULL << (cid & 63);
            if (!(entry->seen[word].fetch_or(mask, std::memory_order_relaxed) & mask)) {
                agg.distinct_names++;
            }
        }

        sync_point.arrive_and_wait();
        if (t == 0) mf_records.close();

        uint16_t total_c = num_countries + 1, idx_start = (uint16_t)(((uint32_t)total_c * t) / nthreads), idx_end = (uint16_t)(((uint32_t)total_c * (t + 1)) / nthreads);
        char local_buf[4096]; char* p_out = local_buf;
        for (uint16_t idx = idx_start; idx < idx_end; ++idx) {
            uint8_t c = out_order[idx];
            uint64_t count = 0, name_bytes = 0, note_bytes = 0; int64_t id_sum = 0, amt_sum = 0, amt_min = INT64_MAX, amt_max = INT64_MIN;
            uint32_t distinct_names = 0;
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                const auto& o = tdata[ot].countries[c];
                if (!o.count) continue;
                count += o.count; id_sum += o.id_sum; amt_sum += o.amount_sum;
                amt_min = std::min(amt_min, o.amount_min); amt_max = std::max(amt_max, o.amount_max);
                name_bytes += o.name_bytes; note_bytes += o.note_bytes;
                distinct_names += o.distinct_names;
            }
            if (!count) continue;

            const std::string& name = country_names[c];
            memcpy(p_out, name.data(), name.size()); p_out += name.size(); *p_out++ = ' ';
            p_out = write_u64(p_out, count); *p_out++ = ' '; p_out = write_i64(p_out, id_sum); *p_out++ = ' ';
            p_out = format_money(p_out, amt_sum); *p_out++ = ' '; p_out = format_money(p_out, amt_min); *p_out++ = ' ';
            p_out = format_money(p_out, amt_max); *p_out++ = ' '; p_out = write_u64(p_out, distinct_names); *p_out++ = ' ';
            p_out = write_u64(p_out, name_bytes); *p_out++ = ' '; p_out = write_u64(p_out, note_bytes); *p_out++ = '\n';
        }

        size_t my_bytes = (size_t)(p_out - local_buf);
        thread_output_bytes[t] = my_bytes;
        sync_point.arrive_and_wait();

        if (t == 0) {
            size_t total_bytes = 0;
            for (unsigned int ot = 0; ot < nthreads; ++ot) {
                thread_output_offsets[ot] = total_bytes;
                total_bytes += thread_output_bytes[ot];
            }
            total_out_size = total_bytes;
            out_fd = ::open(out_path.string().c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
            if (out_fd >= 0 && total_bytes > 0) {
                ftruncate(out_fd, total_bytes);
                out_mmap_ptr = (char*)::mmap(NULL, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0);
            }
        }
        sync_point.arrive_and_wait();

        if (out_mmap_ptr && my_bytes > 0) {
            memcpy(out_mmap_ptr + thread_output_offsets[t], local_buf, my_bytes);
        }
    };

    std::vector<std::thread> workers; workers.reserve(nthreads - 1);
    for (unsigned int t = 1; t < nthreads; ++t) workers.emplace_back(worker_fn, t);
    worker_fn(0); for (auto& w : workers) w.join();

    if (out_mmap_ptr && out_mmap_ptr != MAP_FAILED) munmap(out_mmap_ptr, total_out_size);
    if (out_fd >= 0) ::close(out_fd);
    munmap(name_table, sizeof(NameSlot) * HASH_CAP);
    return 0;
}
