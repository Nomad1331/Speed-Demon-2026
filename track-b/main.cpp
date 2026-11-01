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

struct TermInfo {
    uint32_t df = 0;
    uint32_t dense_idx = UINT32_MAX; // if dense, index in dense_bitsets
    uint32_t sparse_offset = 0;      // if sparse, start index in sparse_postings
};

struct ThreadAccumulator {
    bool is_dense = false;
    bool is_empty = false;
    std::vector<uint32_t> ids;    // Used when is_dense == false (sorted doc IDs)
    std::vector<uint64_t> bitset; // Used when is_dense == true (num_words uint64_t)
    std::vector<uint32_t> temp_ids;
};

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

    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 8;
    unsigned int num_threads = std::min(hw_threads, 16u);

    // Optimal threshold: N / 32 (3.125% of N)
    // Squeezes dense bitsets down to ~10 MB on full!
    uint32_t DF_THRESHOLD = std::max(256u, (uint32_t)(N / 32));

    std::vector<TermInfo> terms(num_query_terms);
    uint64_t* dense_bitsets = nullptr;
    uint32_t* sparse_postings = nullptr;
    uint32_t num_dense_terms = 0;
    uint32_t total_sparse_postings = 0;

    // Per-thread count and write offset tables
    std::vector<std::vector<uint32_t>> thread_df(num_threads, std::vector<uint32_t>(num_query_terms, 0));
    std::vector<std::vector<uint32_t>> thread_write_offsets(num_threads, std::vector<uint32_t>(num_query_terms, 0));

    size_t chunk_bytes = docs_file.size / num_threads;
    size_t step = ((N + num_threads - 1) / num_threads + 63) & ~63ULL;
    if (step == 0) step = 64;

    size_t num_queries = queries.size();
    size_t q_step = (num_queries + num_threads - 1) / num_threads;
    std::vector<std::string> thread_outputs(num_threads);

    std::barrier sync_point(num_threads);

    std::chrono::high_resolution_clock::time_point t_stage0;
    std::chrono::high_resolution_clock::time_point t_pass1;
    std::chrono::high_resolution_clock::time_point t_pass2;
    std::chrono::high_resolution_clock::time_point t_queries_done;

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            // ==========================================
            // Stage 0: Document boundary scan across byte chunks
            // ==========================================
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
            if (tid == 0 && profile) t_stage0 = std::chrono::high_resolution_clock::now();

            // ==========================================
            // Stage 1 (Pass 1): Count query term df for assigned docs
            // ==========================================
            size_t d_start = tid * step;
            size_t d_end = std::min((size_t)N, (tid + 1) * step);
            uint32_t* my_df = thread_df[tid].data();
            uint32_t doc_terms_buf[1024];

            for (size_t d = d_start; d < d_end; ++d) {
                const char* dp = docs_file.data + doc_offsets[d];
                const char* text_end = dp + doc_lens[d];
                uint32_t num_doc_terms = 0;

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
                    if (term_id != UINT32_MAX && num_doc_terms < 1024) {
                        doc_terms_buf[num_doc_terms++] = term_id;
                    }
                }

                if (num_doc_terms > 0) {
                    std::sort(doc_terms_buf, doc_terms_buf + num_doc_terms);
                    uint32_t unique_count = (uint32_t)(std::unique(doc_terms_buf, doc_terms_buf + num_doc_terms) - doc_terms_buf);
                    for (uint32_t j = 0; j < unique_count; ++j) {
                        my_df[doc_terms_buf[j]]++;
                    }
                }
            }

            sync_point.arrive_and_wait();

            // Thread 0 allocates dense & sparse pools
            if (tid == 0) {
                if (profile) t_pass1 = std::chrono::high_resolution_clock::now();
                uint32_t cur_sparse_offset = 0;
                for (uint32_t t = 0; t < num_query_terms; ++t) {
                    uint32_t tot = 0;
                    for (unsigned int k = 0; k < num_threads; ++k) {
                        tot += thread_df[k][t];
                    }
                    terms[t].df = tot;

                    if (tot >= DF_THRESHOLD) {
                        terms[t].dense_idx = num_dense_terms++;
                    } else if (tot > 0) {
                        terms[t].sparse_offset = cur_sparse_offset;
                        uint32_t running = cur_sparse_offset;
                        for (unsigned int k = 0; k < num_threads; ++k) {
                            thread_write_offsets[k][t] = running;
                            running += thread_df[k][t];
                        }
                        cur_sparse_offset += tot;
                    }
                }
                total_sparse_postings = cur_sparse_offset;

                size_t dense_u64 = (size_t)num_dense_terms * num_words;
                dense_bitsets = (uint64_t*)calloc(dense_u64 > 0 ? dense_u64 : 1, sizeof(uint64_t));
                sparse_postings = (uint32_t*)malloc((size_t)(total_sparse_postings > 0 ? total_sparse_postings : 1) * sizeof(uint32_t));
            }

            sync_point.arrive_and_wait();

            // ==========================================
            // Stage 2 (Pass 2): Populate dense bitsets and sorted sparse postings
            // ==========================================
            uint32_t* my_write_offsets = thread_write_offsets[tid].data();

            for (size_t d = d_start; d < d_end; ++d) {
                const char* dp = docs_file.data + doc_offsets[d];
                const char* text_end = dp + doc_lens[d];
                uint32_t num_doc_terms = 0;
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
                    if (term_id != UINT32_MAX && num_doc_terms < 1024) {
                        doc_terms_buf[num_doc_terms++] = term_id;
                    }
                }

                if (num_doc_terms > 0) {
                    std::sort(doc_terms_buf, doc_terms_buf + num_doc_terms);
                    uint32_t unique_count = (uint32_t)(std::unique(doc_terms_buf, doc_terms_buf + num_doc_terms) - doc_terms_buf);
                    for (uint32_t j = 0; j < unique_count; ++j) {
                        uint32_t term_id = doc_terms_buf[j];
                        const TermInfo& ti = terms[term_id];
                        if (ti.dense_idx != UINT32_MAX) {
                            dense_bitsets[ti.dense_idx * num_words + word_idx] |= bit_mask;
                        } else {
                            sparse_postings[my_write_offsets[term_id]++] = (uint32_t)d;
                        }
                    }
                }
            }

            sync_point.arrive_and_wait();
            if (tid == 0) {
                if (profile) t_pass2 = std::chrono::high_resolution_clock::now();
#ifndef _WIN32
                // Drop page cache on Linux to ensure <40MB RAM
                madvise((void*)docs_file.data, docs_file.size, MADV_DONTNEED);
#endif
            }

            // ==========================================
            // Stage 3: Ultra-Fast Hybrid Query Execution
            // ==========================================
            size_t q_start = tid * q_step;
            size_t q_end = std::min(num_queries, (tid + 1) * q_step);

            std::string& out = thread_outputs[tid];
            out.reserve((q_end > q_start ? (q_end - q_start) : 1) * 32);

            ThreadAccumulator R;
            R.ids.reserve(DF_THRESHOLD * 2 + 64);
            R.temp_ids.reserve(DF_THRESHOLD * 2 + 64);
            R.bitset.resize(num_words > 0 ? num_words : 1);

            std::vector<uint32_t> candidates;
            candidates.reserve(128);
            std::vector<uint32_t> matches;
            matches.reserve(128);
            TokenRef window[8];
            char line_buf[256];

            for (size_t qidx = q_start; qidx < q_end; ++qidx) {
                const Query& q = queries[qidx];
                if (q.type == QueryType::BOOL_QUERY) {
                    const TermInfo& info0 = terms[q.b.term_ids[0]];

                    if (info0.df == 0) {
                        R.is_empty = true;
                        R.is_dense = false;
                        R.ids.clear();
                    } else if (info0.dense_idx != UINT32_MAX) {
                        R.is_empty = false;
                        R.is_dense = true;
                        memcpy(R.bitset.data(), dense_bitsets + (size_t)info0.dense_idx * num_words, num_words * sizeof(uint64_t));
                    } else {
                        R.is_empty = false;
                        R.is_dense = false;
                        R.ids.assign(sparse_postings + info0.sparse_offset, sparse_postings + info0.sparse_offset + info0.df);
                    }

                    for (int i = 1; i < q.b.num_terms; ++i) {
                        BoolOp op = q.b.ops[i - 1];
                        const TermInfo& ti = terms[q.b.term_ids[i]];

                        if (ti.df == 0) {
                            if (op == BoolOp::AND) {
                                R.is_empty = true;
                                R.is_dense = false;
                                R.ids.clear();
                            }
                            continue;
                        }

                        if (R.is_empty) {
                            if (op == BoolOp::AND || op == BoolOp::NOT) continue;
                            // empty OR t = t
                            if (ti.dense_idx != UINT32_MAX) {
                                R.is_empty = false;
                                R.is_dense = true;
                                memcpy(R.bitset.data(), dense_bitsets + (size_t)ti.dense_idx * num_words, num_words * sizeof(uint64_t));
                            } else {
                                R.is_empty = false;
                                R.is_dense = false;
                                R.ids.assign(sparse_postings + ti.sparse_offset, sparse_postings + ti.sparse_offset + ti.df);
                            }
                            continue;
                        }

                        bool ti_dense = (ti.dense_idx != UINT32_MAX);

                        if (!R.is_dense && !ti_dense) {
                            // Sparse OP Sparse: two-pointer linear scan
                            const uint32_t* a = R.ids.data();
                            const uint32_t* a_end = a + R.ids.size();
                            const uint32_t* b = sparse_postings + ti.sparse_offset;
                            const uint32_t* b_end = b + ti.df;

                            R.temp_ids.clear();
                            if (op == BoolOp::AND) {
                                while (a < a_end && b < b_end) {
                                    if (*a < *b) a++;
                                    else if (*b < *a) b++;
                                    else { R.temp_ids.push_back(*a++); b++; }
                                }
                                R.ids.swap(R.temp_ids);
                                if (R.ids.empty()) R.is_empty = true;
                            } else if (op == BoolOp::NOT) {
                                while (a < a_end && b < b_end) {
                                    if (*a < *b) R.temp_ids.push_back(*a++);
                                    else if (*b < *a) b++;
                                    else { a++; b++; }
                                }
                                while (a < a_end) R.temp_ids.push_back(*a++);
                                R.ids.swap(R.temp_ids);
                                if (R.ids.empty()) R.is_empty = true;
                            } else if (op == BoolOp::OR) {
                                while (a < a_end && b < b_end) {
                                    if (*a < *b) R.temp_ids.push_back(*a++);
                                    else if (*b < *a) R.temp_ids.push_back(*b++);
                                    else { R.temp_ids.push_back(*a++); b++; }
                                }
                                while (a < a_end) R.temp_ids.push_back(*a++);
                                while (b < b_end) R.temp_ids.push_back(*b++);
                                R.ids.swap(R.temp_ids);

                                if (R.ids.size() >= DF_THRESHOLD) {
                                    memset(R.bitset.data(), 0, num_words * sizeof(uint64_t));
                                    for (uint32_t id : R.ids) R.bitset[id >> 6] |= 1ULL << (id & 63);
                                    R.is_dense = true;
                                    R.ids.clear();
                                }
                            }
                        } else if (!R.is_dense && ti_dense) {
                            // Sparse OP Dense
                            const uint64_t* tb = dense_bitsets + (size_t)ti.dense_idx * num_words;
                            if (op == BoolOp::AND) {
                                uint32_t w = 0;
                                for (uint32_t id : R.ids) {
                                    if ((tb[id >> 6] >> (id & 63)) & 1ULL) R.ids[w++] = id;
                                }
                                R.ids.resize(w);
                                if (w == 0) R.is_empty = true;
                            } else if (op == BoolOp::NOT) {
                                uint32_t w = 0;
                                for (uint32_t id : R.ids) {
                                    if (!((tb[id >> 6] >> (id & 63)) & 1ULL)) R.ids[w++] = id;
                                }
                                R.ids.resize(w);
                                if (w == 0) R.is_empty = true;
                            } else if (op == BoolOp::OR) {
                                memcpy(R.bitset.data(), tb, num_words * sizeof(uint64_t));
                                for (uint32_t id : R.ids) R.bitset[id >> 6] |= 1ULL << (id & 63);
                                R.is_dense = true;
                                R.ids.clear();
                            }
                        } else if (R.is_dense && !ti_dense) {
                            // Dense OP Sparse
                            const uint32_t* b = sparse_postings + ti.sparse_offset;
                            if (op == BoolOp::AND) {
                                R.ids.clear();
                                for (uint32_t j = 0; j < ti.df; ++j) {
                                    uint32_t id = b[j];
                                    if ((R.bitset[id >> 6] >> (id & 63)) & 1ULL) R.ids.push_back(id);
                                }
                                R.is_dense = false;
                                if (R.ids.empty()) R.is_empty = true;
                            } else if (op == BoolOp::NOT) {
                                for (uint32_t j = 0; j < ti.df; ++j) {
                                    uint32_t id = b[j];
                                    R.bitset[id >> 6] &= ~(1ULL << (id & 63));
                                }
                            } else if (op == BoolOp::OR) {
                                for (uint32_t j = 0; j < ti.df; ++j) {
                                    uint32_t id = b[j];
                                    R.bitset[id >> 6] |= 1ULL << (id & 63);
                                }
                            }
                        } else {
                            // Dense OP Dense: AVX2 SIMD bitwise ops
                            const uint64_t* tb = dense_bitsets + (size_t)ti.dense_idx * num_words;
                            if (op == BoolOp::AND) {
                                for (size_t w = 0; w < num_words; ++w) R.bitset[w] &= tb[w];
                            } else if (op == BoolOp::OR) {
                                for (size_t w = 0; w < num_words; ++w) R.bitset[w] |= tb[w];
                            } else if (op == BoolOp::NOT) {
                                for (size_t w = 0; w < num_words; ++w) R.bitset[w] &= ~tb[w];
                            }
                        }
                    }

                    if (R.is_empty) {
                        out.append("0 0\n");
                    } else if (!R.is_dense) {
                        // Sparse result: ALREADY SORTED! Instant O(1) count and first 10!
                        uint32_t count = (uint32_t)R.ids.size();
                        if (count == 0) {
                            out.append("0 0\n");
                        } else {
                            uint64_t sum = 0;
                            for (uint32_t id : R.ids) sum += id;
                            char* p = line_buf;
                            p = write_u64_fast(p, count);
                            *p++ = ' ';
                            p = write_u64_fast(p, sum);
                            uint32_t m = std::min(count, 10u);
                            for (uint32_t j = 0; j < m; ++j) {
                                *p++ = ' ';
                                p = write_u64_fast(p, R.ids[j]);
                            }
                            *p++ = '\n';
                            out.append(line_buf, p - line_buf);
                        }
                    } else {
                        // Dense result: LUT16 sum & count
                        uint32_t count = 0;
                        uint64_t sum = 0;
                        uint32_t first10[10];
                        uint32_t found = 0;

                        for (size_t w = 0; w < num_words; ++w) {
                            uint64_t val = R.bitset[w];
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
                            for (uint32_t j = 0; j < found; ++j) {
                                *p++ = ' ';
                                p = write_u64_fast(p, first10[j]);
                            }
                            *p++ = '\n';
                            out.append(line_buf, p - line_buf);
                        }
                    }
                } else { // PHRASE_QUERY
                    bool has_absent = false;
                    int best_idx = 0;
                    uint32_t min_df = terms[q.p.term_ids[0]].df;

                    for (int i = 0; i < q.p.num_terms; ++i) {
                        uint32_t cdf = terms[q.p.term_ids[i]].df;
                        if (cdf == 0) {
                            has_absent = true;
                            break;
                        }
                        if (cdf < min_df) {
                            min_df = cdf;
                            best_idx = i;
                        }
                    }

                    if (has_absent) {
                        out.append("0 0\n");
                        continue;
                    }

                    candidates.clear();
                    const TermInfo& rarest = terms[q.p.term_ids[best_idx]];

                    if (rarest.dense_idx != UINT32_MAX) {
                        // All terms in phrase are dense
                        memcpy(R.bitset.data(), dense_bitsets + (size_t)rarest.dense_idx * num_words, num_words * sizeof(uint64_t));
                        for (int i = 0; i < q.p.num_terms; ++i) {
                            if (i == best_idx) continue;
                            const TermInfo& ti = terms[q.p.term_ids[i]];
                            const uint64_t* tb = dense_bitsets + (size_t)ti.dense_idx * num_words;
                            for (size_t w = 0; w < num_words; ++w) R.bitset[w] &= tb[w];
                        }
                        for (size_t w = 0; w < num_words; ++w) {
                            uint64_t val = R.bitset[w];
                            while (val) {
                                int bit = std::countr_zero(val);
                                candidates.push_back((uint32_t)(w * 64 + bit));
                                val &= val - 1;
                            }
                        }
                    } else {
                        // Rarest term is SPARSE (sorted doc IDs)
                        const uint32_t* r_ids = sparse_postings + rarest.sparse_offset;
                        for (uint32_t i = 0; i < rarest.df; ++i) {
                            uint32_t doc_id = r_ids[i];
                            bool in_all = true;
                            for (int j = 0; j < q.p.num_terms; ++j) {
                                if (j == best_idx) continue;
                                const TermInfo& ti = terms[q.p.term_ids[j]];
                                if (ti.dense_idx != UINT32_MAX) {
                                    if (!((dense_bitsets[(size_t)ti.dense_idx * num_words + (doc_id >> 6)] >> (doc_id & 63)) & 1ULL)) {
                                        in_all = false;
                                        break;
                                    }
                                } else {
                                    const uint32_t* s_ids = sparse_postings + ti.sparse_offset;
                                    if (!std::binary_search(s_ids, s_ids + ti.df, doc_id)) {
                                        in_all = false;
                                        break;
                                    }
                                }
                            }
                            if (in_all) {
                                candidates.push_back(doc_id);
                            }
                        }
                    }

                    matches.clear();
                    uint64_t sum = 0;
                    uint8_t k = q.p.num_terms;

                    // Streaming In-Place Phrase Token Matching with Instant Early Exit
                    for (uint32_t doc_id : candidates) {
                        const char* dp = docs_file.data + doc_offsets[doc_id];
                        const char* text_end = dp + doc_lens[doc_id];
                        uint32_t tok_count = 0;
                        bool matched = false;

                        while (dp < text_end) {
                            uint8_t c = CHAR_MAP[(unsigned char)*dp++];
                            if (!c) continue;
                            const char* tok_start = dp - 1;
                            while (dp < text_end && CHAR_MAP[(unsigned char)*dp]) dp++;
                            uint32_t tok_len = (uint32_t)(dp - tok_start);

                            window[tok_count % k] = {tok_start, tok_len};
                            tok_count++;

                            if (tok_count >= k) {
                                bool ok = true;
                                uint32_t start_idx = tok_count - k;
                                for (uint32_t j = 0; j < k; ++j) {
                                    const TokenRef& tok = window[(start_idx + j) % k];
                                    if (!phrase_tokens_match(q.p.terms[j], tok)) {
                                        ok = false;
                                        break;
                                    }
                                }
                                if (ok) {
                                    matched = true;
                                    break; // EARLY EXIT: Immediately proceed to next candidate
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
                        for (uint32_t j = 0; j < m; ++j) {
                            *p++ = ' ';
                            p = write_u64_fast(p, matches[j]);
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
        auto d_stage0 = std::chrono::duration<double, std::milli>(t_stage0 - t_queries).count();
        auto d_pass1 = std::chrono::duration<double, std::milli>(t_pass1 - t_stage0).count();
        auto d_pass2 = std::chrono::duration<double, std::milli>(t_pass2 - t_pass1).count();
        auto d_query_exec = std::chrono::duration<double, std::milli>(t_queries_done - t_pass2).count();
        auto d_output = std::chrono::duration<double, std::milli>(t_done - t_queries_done).count();
        auto d_total = std::chrono::duration<double, std::milli>(t_done - t_start).count();

        std::cerr << "[Profile Breakdown]\n"
                  << "  Query parse:       " << d_queries << " ms\n"
                  << "  Stage 0 (scan):    " << d_stage0 << " ms\n"
                  << "  Pass 1 (df count): " << d_pass1 << " ms\n"
                  << "  Pass 2 (indexing): " << d_pass2 << " ms\n"
                  << "  Query exec:        " << d_query_exec << " ms\n"
                  << "  Output write:      " << d_output << " ms\n"
                  << "  Total wall:        " << d_total << " ms\n"
                  << "  Dense terms:       " << num_dense_terms << " ("
                  << (num_dense_terms * num_words * 8) / (1024.0 * 1024.0) << " MB)\n"
                  << "  Sparse postings:   " << total_sparse_postings << " ("
                  << (total_sparse_postings * 4) / (1024.0 * 1024.0) << " MB)\n";
    }

    if (dense_bitsets) free(dense_bitsets);
    if (sparse_postings) free(sparse_postings);
    return 0;
}
