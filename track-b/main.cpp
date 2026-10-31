#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <thread>
#include <atomic>
#include <barrier>
#include <bit>
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

// 256-byte static LUT mapping ASCII A-Z to a-z, and non-alnum to 0
alignas(64) static uint8_t CHAR_MAP[256];

static void init_char_map() {
    memset(CHAR_MAP, 0, sizeof(CHAR_MAP));
    for (int c = '0'; c <= '9'; ++c) CHAR_MAP[c] = (uint8_t)c;
    for (int c = 'a'; c <= 'z'; ++c) CHAR_MAP[c] = (uint8_t)c;
    for (int c = 'A'; c <= 'Z'; ++c) CHAR_MAP[c] = (uint8_t)(c + ('a' - 'A'));
}

// 16-bit LUT for popcount and bit index sum
struct Lut16Entry {
    uint8_t count;
    uint16_t sum;
};
alignas(64) static Lut16Entry LUT16[65536];

static void init_lut16() {
    for (uint32_t i = 0; i < 65536; ++i) {
        uint8_t c = 0;
        uint16_t s = 0;
        for (int b = 0; b < 16; ++b) {
            if ((i >> b) & 1) {
                c++;
                s += (uint16_t)b;
            }
        }
        LUT16[i].count = c;
        LUT16[i].sum = s;
    }
}

// FNV-1a 64-bit hash
inline uint64_t fnv1a(const char* s, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h = (h ^ (unsigned char)s[i]) * 1099511628211ULL;
    }
    return h;
}

inline bool tokens_equal(const char* query_str, const char* doc_tok, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        if ((char)CHAR_MAP[(unsigned char)doc_tok[i]] != query_str[i]) return false;
    }
    return true;
}

struct TermSlot {
    uint64_t hash = 0;
    uint32_t term_id = 0;
    uint32_t len = 0;
    const char* str = nullptr;
};

struct TermTable {
    TermSlot* slots = nullptr;
    uint32_t capacity = 0;
    uint32_t mask = 0;
    uint32_t num_terms = 0;
    std::vector<std::string_view> term_strings;

    void init(uint32_t expected_terms) {
        capacity = 16;
        while (capacity < expected_terms * 4) capacity *= 2;
        if (capacity < 1024) capacity = 1024;
        mask = capacity - 1;
        slots = (TermSlot*)calloc(capacity, sizeof(TermSlot));
    }

    ~TermTable() {
        if (slots) free(slots);
    }

    uint32_t get_or_insert(const char* s, uint32_t len) {
        uint64_t h = fnv1a(s, len);
        uint32_t idx = (uint32_t)(h & mask);
        while (slots[idx].hash != 0) {
            if (slots[idx].hash == h && slots[idx].len == len && memcmp(slots[idx].str, s, len) == 0) {
                return slots[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        uint32_t id = num_terms++;
        slots[idx].hash = h;
        slots[idx].len = len;
        slots[idx].str = s; // Zero-copy pointing directly to queries.txt
        slots[idx].term_id = id;
        term_strings.push_back(std::string_view(s, len));
        return id;
    }

    inline uint32_t lookup(uint64_t h, const char* s, uint32_t len) const {
        uint32_t idx = (uint32_t)(h & mask);
        while (slots[idx].hash != 0) {
            if (slots[idx].hash == h && slots[idx].len == len && tokens_equal(slots[idx].str, s, len)) {
                return slots[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        return UINT32_MAX;
    }
};

enum class QueryType : uint8_t {
    BOOL_QUERY,
    PHRASE_QUERY
};

enum class BoolOp : uint8_t {
    AND = 1,
    OR = 2,
    NOT = 3
};

struct BoolQuery {
    uint8_t num_terms;
    uint32_t term_ids[6];
    BoolOp ops[5];
};

struct PhraseQuery {
    uint8_t num_terms;
    uint32_t term_ids[8];
    std::string_view terms[8];
};

struct Query {
    QueryType type;
    union {
        BoolQuery b;
        PhraseQuery p;
    };
    Query() {}
};

struct TokenRef {
    const char* ptr;
    uint32_t len;
};

inline bool phrase_tokens_match(std::string_view phrase_term, const TokenRef& tok) {
    if (phrase_term.size() != tok.len) return false;
    for (size_t i = 0; i < tok.len; ++i) {
        if ((char)CHAR_MAP[(unsigned char)tok.ptr[i]] != phrase_term[i]) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::ios_base::sync_with_stdio(false);
    std::string data_dir = "/data";
    std::string out_dir = "/out";
    bool profile = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (arg == "--profile") profile = true;
    }

    auto t_start = std::chrono::high_resolution_clock::now();
    init_char_map();
    init_lut16();

    // 1. Memory-map queries.txt and parse all queries
    std::string queries_path = data_dir + "/queries.txt";
    MappedFile queries_file;
    if (!queries_file.open(queries_path)) {
        std::cerr << "Failed to open " << queries_path << "\n";
        return 1;
    }

    TermTable term_table;
    term_table.init(32768);

    std::vector<Query> queries;
    queries.reserve(65536);

    const char* qp = queries_file.data;
    const char* qend = queries_file.data + queries_file.size;

    while (qp < qend) {
        const char* line_start = qp;
        while (qp < qend && *qp != '\n') qp++;
        const char* line_end = qp;
        if (qp < qend && *qp == '\n') qp++;
        while (line_end > line_start && (*(line_end - 1) == '\r' || *(line_end - 1) == ' ')) line_end--;
        if (line_end <= line_start) continue;

        std::string_view line(line_start, (size_t)(line_end - line_start));
        if (line.starts_with("BOOL ")) {
            Query q;
            q.type = QueryType::BOOL_QUERY;
            q.b.num_terms = 0;

            const char* cur = line_start + 5;
            int pos = 0;
            while (cur < line_end) {
                const char* tok_start = cur;
                while (cur < line_end && *cur != ' ') cur++;
                std::string_view tok(tok_start, (size_t)(cur - tok_start));
                if (cur < line_end && *cur == ' ') cur++;

                if ((pos % 2) == 0) {
                    uint32_t tid = term_table.get_or_insert(tok.data(), (uint32_t)tok.size());
                    q.b.term_ids[q.b.num_terms++] = tid;
                } else {
                    if (tok == "AND") q.b.ops[q.b.num_terms - 1] = BoolOp::AND;
                    else if (tok == "OR") q.b.ops[q.b.num_terms - 1] = BoolOp::OR;
                    else if (tok == "NOT") q.b.ops[q.b.num_terms - 1] = BoolOp::NOT;
                }
                pos++;
            }
            queries.push_back(q);
        } else if (line.starts_with("PHRASE \"") && line.ends_with("\"")) {
            Query q;
            q.type = QueryType::PHRASE_QUERY;
            q.p.num_terms = 0;

            const char* cur = line_start + 8;
            const char* p_term_end = line_end - 1;

            while (cur < p_term_end) {
                const char* tok_start = cur;
                while (cur < p_term_end && *cur != ' ') cur++;
                std::string_view tok(tok_start, (size_t)(cur - tok_start));
                if (cur < p_term_end && *cur == ' ') cur++;

                uint32_t tid = term_table.get_or_insert(tok.data(), (uint32_t)tok.size());
                q.p.term_ids[q.p.num_terms] = tid;
                q.p.terms[q.p.num_terms] = term_table.term_strings[tid];
                q.p.num_terms++;
            }
            queries.push_back(q);
        }
    }

    uint32_t num_query_terms = term_table.num_terms;
    auto t_queries = std::chrono::high_resolution_clock::now();

    // 2. Memory-map docs.txt and determine N
    std::string docs_path = data_dir + "/docs.txt";
    MappedFile docs_file;
    if (!docs_file.open(docs_path)) {
        std::cerr << "Failed to open " << docs_path << "\n";
        return 1;
    }

    // Determine N quickly from end of docs.txt
    uint32_t N = 0;
    if (docs_file.size > 0) {
        const char* end_ptr = docs_file.data + docs_file.size;
        while (end_ptr > docs_file.data && (*(end_ptr - 1) == '\n' || *(end_ptr - 1) == '\r')) end_ptr--;
        const char* last_line = end_ptr;
        while (last_line > docs_file.data && *(last_line - 1) != '\n') last_line--;
        uint32_t max_id = 0;
        const char* p_id = last_line;
        while (p_id < end_ptr && *p_id >= '0' && *p_id <= '9') {
            max_id = max_id * 10 + (*p_id++ - '0');
        }
        N = max_id + 1;
    }

    std::vector<uint32_t> doc_offsets(N);
    std::vector<uint32_t> doc_lens(N);
    size_t num_words = (N + 63) / 64;

    // 3. Allocate Inverted Index Bitsets
    size_t total_u64 = (size_t)num_query_terms * num_words;
    uint64_t* bitsets = (uint64_t*)calloc(total_u64 > 0 ? total_u64 : 1, sizeof(uint64_t));

    // Determine thread count
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 8;
    unsigned int num_threads = std::min(hw_threads, 16u);

    std::barrier sync_point(num_threads);

    size_t chunk_bytes = docs_file.size / num_threads;
    size_t step = ((N + num_threads - 1) / num_threads + 63) & ~63ULL;
    if (step == 0) step = 64;

    size_t num_queries = queries.size();
    size_t q_step = (num_queries + num_threads - 1) / num_threads;
    std::vector<std::string> thread_outputs(num_threads);

    std::chrono::high_resolution_clock::time_point t_docs_scan;
    std::chrono::high_resolution_clock::time_point t_index;
    std::chrono::high_resolution_clock::time_point t_queries_done;

    // Persistent Worker Thread Pool executing all 3 stages
    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            // Stage 1: Parallel document boundary scan
            size_t b_start = tid * chunk_bytes;
            size_t b_end = (tid == num_threads - 1) ? docs_file.size : (tid + 1) * chunk_bytes;

            const char* p = docs_file.data + b_start;
            const char* file_end = docs_file.data + docs_file.size;
            if (tid > 0) {
                while (p < file_end && *p != '\n') p++;
                if (p < file_end && *p == '\n') p++;
            }

            while (p < docs_file.data + b_end && p < file_end) {
                uint32_t doc_id = 0;
                while (p < file_end && *p >= '0' && *p <= '9') {
                    doc_id = doc_id * 10 + (*p++ - '0');
                }
                if (p < file_end && *p == '\t') p++;
                const char* text_start = p;
                while (p < file_end && *p != '\n') p++;
                const char* text_end = p;
                if (p < file_end && *p == '\n') p++;

                if (doc_id < N) {
                    doc_offsets[doc_id] = (uint32_t)(text_start - docs_file.data);
                    doc_lens[doc_id] = (uint32_t)(text_end - text_start);
                }
            }

            sync_point.arrive_and_wait();
            if (tid == 0 && profile) t_docs_scan = std::chrono::high_resolution_clock::now();

            // Stage 2: Parallel document indexing into bitsets
            size_t d_start = tid * step;
            size_t d_end = std::min((size_t)N, (tid + 1) * step);

            for (size_t d = d_start; d < d_end; ++d) {
                const char* dp = docs_file.data + doc_offsets[d];
                const char* text_end = dp + doc_lens[d];
                size_t word_idx = d >> 6;
                uint64_t bit_mask = 1ULL << (d & 63);

                while (dp < text_end) {
                    uint8_t c = CHAR_MAP[(unsigned char)*dp++];
                    if (!c) continue;

                    uint64_t hash = 14695981039346656037ULL;
                    const char* tok_start = dp - 1;
                    hash = (hash ^ c) * 1099511628211ULL;

                    while (dp < text_end) {
                        c = CHAR_MAP[(unsigned char)*dp];
                        if (!c) break;
                        hash = (hash ^ c) * 1099511628211ULL;
                        dp++;
                    }
                    uint32_t tok_len = (uint32_t)(dp - tok_start);

                    uint32_t term_id = term_table.lookup(hash, tok_start, tok_len);
                    if (term_id != UINT32_MAX) {
                        bitsets[term_id * num_words + word_idx] |= bit_mask;
                    }
                }
            }

            sync_point.arrive_and_wait();
            if (tid == 0 && profile) t_index = std::chrono::high_resolution_clock::now();

            // Stage 3: Parallel query execution
            size_t q_start = tid * q_step;
            size_t q_end = std::min(num_queries, (tid + 1) * q_step);

            std::string& out = thread_outputs[tid];
            out.reserve((q_end > q_start ? (q_end - q_start) : 1) * 32);

            std::vector<uint64_t> accum(num_words > 0 ? num_words : 1);
            std::vector<uint32_t> candidates;
            std::vector<uint32_t> matches;
            TokenRef doc_tokens[1024];
            char line_buf[256];

            for (size_t qidx = q_start; qidx < q_end; ++qidx) {
                const Query& q = queries[qidx];
                if (q.type == QueryType::BOOL_QUERY) {
                    uint32_t t0 = q.b.term_ids[0];
                    memcpy(accum.data(), bitsets + (size_t)t0 * num_words, num_words * sizeof(uint64_t));

                    for (int i = 1; i < q.b.num_terms; ++i) {
                        BoolOp op = q.b.ops[i - 1];
                        uint32_t t = q.b.term_ids[i];
                        const uint64_t* term_bits = bitsets + (size_t)t * num_words;

                        if (op == BoolOp::AND) {
                            for (size_t w = 0; w < num_words; ++w) accum[w] &= term_bits[w];
                        } else if (op == BoolOp::OR) {
                            for (size_t w = 0; w < num_words; ++w) accum[w] |= term_bits[w];
                        } else if (op == BoolOp::NOT) {
                            for (size_t w = 0; w < num_words; ++w) accum[w] &= ~term_bits[w];
                        }
                    }

                    uint32_t count = 0;
                    uint64_t sum = 0;
                    uint32_t first10[10];
                    uint32_t found = 0;

                    for (size_t w = 0; w < num_words; ++w) {
                        uint64_t val = accum[w];
                        if (!val) continue;

                        uint64_t base_id = w * 64;
                        if (found < 10) {
                            uint64_t temp = val;
                            while (temp) {
                                int bit = std::countr_zero(temp);
                                uint32_t doc_id = (uint32_t)(base_id + bit);
                                sum += doc_id;
                                if (found < 10) first10[found++] = doc_id;
                                temp &= temp - 1;
                            }
                            count += (uint32_t)std::popcount(val);
                        } else {
                            uint16_t c0 = (uint16_t)(val);
                            uint16_t c1 = (uint16_t)(val >> 16);
                            uint16_t c2 = (uint16_t)(val >> 32);
                            uint16_t c3 = (uint16_t)(val >> 48);

                            sum += LUT16[c0].sum + (uint64_t)LUT16[c0].count * base_id;
                            sum += LUT16[c1].sum + (uint64_t)LUT16[c1].count * (base_id + 16);
                            sum += LUT16[c2].sum + (uint64_t)LUT16[c2].count * (base_id + 32);
                            sum += LUT16[c3].sum + (uint64_t)LUT16[c3].count * (base_id + 48);

                            count += (uint32_t)(LUT16[c0].count + LUT16[c1].count + LUT16[c2].count + LUT16[c3].count);
                        }
                    }

                    if (count == 0) {
                        out.append("0 0\n");
                    } else {
                        char* p = line_buf;
                        p = write_u64_fast(p, count);
                        *p++ = ' ';
                        p = write_u64_fast(p, sum);
                        for (uint32_t i = 0; i < found; ++i) {
                            *p++ = ' ';
                            p = write_u64_fast(p, first10[i]);
                        }
                        *p++ = '\n';
                        out.append(line_buf, p - line_buf);
                    }
                } else { // PHRASE_QUERY
                    uint32_t t0 = q.p.term_ids[0];
                    memcpy(accum.data(), bitsets + (size_t)t0 * num_words, num_words * sizeof(uint64_t));

                    for (int i = 1; i < q.p.num_terms; ++i) {
                        uint32_t t = q.p.term_ids[i];
                        const uint64_t* term_bits = bitsets + (size_t)t * num_words;
                        for (size_t w = 0; w < num_words; ++w) accum[w] &= term_bits[w];
                    }

                    candidates.clear();
                    for (size_t w = 0; w < num_words; ++w) {
                        uint64_t val = accum[w];
                        while (val) {
                            int bit = std::countr_zero(val);
                            candidates.push_back((uint32_t)(w * 64 + bit));
                            val &= val - 1;
                        }
                    }

                    matches.clear();
                    uint64_t sum = 0;
                    uint8_t k = q.p.num_terms;

                    for (uint32_t doc_id : candidates) {
                        const char* dp = docs_file.data + doc_offsets[doc_id];
                        const char* text_end = dp + doc_lens[doc_id];
                        uint32_t num_tok = 0;

                        while (dp < text_end && num_tok < 1024) {
                            uint8_t c = CHAR_MAP[(unsigned char)*dp++];
                            if (!c) continue;
                            const char* tok_start = dp - 1;
                            while (dp < text_end && CHAR_MAP[(unsigned char)*dp]) dp++;
                            doc_tokens[num_tok++] = {tok_start, (uint32_t)(dp - tok_start)};
                        }

                        bool matched = false;
                        if (num_tok >= k) {
                            for (uint32_t i = 0; i + k <= num_tok; ++i) {
                                bool ok = true;
                                for (uint32_t j = 0; j < k; ++j) {
                                    if (!phrase_tokens_match(q.p.terms[j], doc_tokens[i + j])) {
                                        ok = false;
                                        break;
                                    }
                                }
                                if (ok) {
                                    matched = true;
                                    break;
                                }
                            }
                        }

                        if (matched) {
                            matches.push_back(doc_id);
                            sum += doc_id;
                        }
                    }

                    if (matches.empty()) {
                        out.append("0 0\n");
                    } else {
                        char* p = line_buf;
                        p = write_u64_fast(p, (uint64_t)matches.size());
                        *p++ = ' ';
                        p = write_u64_fast(p, sum);
                        uint32_t m = std::min((uint32_t)matches.size(), 10u);
                        for (uint32_t i = 0; i < m; ++i) {
                            *p++ = ' ';
                            p = write_u64_fast(p, matches[i]);
                        }
                        *p++ = '\n';
                        out.append(line_buf, p - line_buf);
                    }
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    if (profile) t_queries_done = std::chrono::high_resolution_clock::now();

    // 5. Output results.txt
    std::filesystem::create_directories(out_dir);
    std::string out_path = out_dir + "/results.txt";
    FILE* fp = fopen(out_path.c_str(), "wb");
    if (!fp) {
        std::cerr << "Failed to open output file " << out_path << "\n";
        return 1;
    }

    for (const auto& chunk : thread_outputs) {
        if (!chunk.empty()) {
            fwrite(chunk.data(), 1, chunk.size(), fp);
        }
    }
    fclose(fp);
    auto t_done = std::chrono::high_resolution_clock::now();

    if (profile) {
        auto d_queries = std::chrono::duration<double, std::milli>(t_queries - t_start).count();
        auto d_docs_scan = std::chrono::duration<double, std::milli>(t_docs_scan - t_queries).count();
        auto d_index = std::chrono::duration<double, std::milli>(t_index - t_docs_scan).count();
        auto d_query_exec = std::chrono::duration<double, std::milli>(t_queries_done - t_index).count();
        auto d_output = std::chrono::duration<double, std::milli>(t_done - t_queries_done).count();
        auto d_total = std::chrono::duration<double, std::milli>(t_done - t_start).count();

        std::cerr << "[Profile Breakdown]\n"
                  << "  Query parse:       " << d_queries << " ms\n"
                  << "  Docs scan (16 th): " << d_docs_scan << " ms\n"
                  << "  Doc index (16 th): " << d_index << " ms\n"
                  << "  Query exec (16 th):" << d_query_exec << " ms\n"
                  << "  Output write:      " << d_output << " ms\n"
                  << "  Total wall:        " << d_total << " ms\n";
    }

    free(bitsets);
    return 0;
}
