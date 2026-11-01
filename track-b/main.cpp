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
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/prctl.h>

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

struct MappedFile {
    const char* data = nullptr; size_t size = 0; int fd = -1;
    bool open(const std::string& path) {
        fd = ::open(path.c_str(), O_RDONLY | O_BINARY); if (fd < 0) return false;
        struct stat st; if (fstat(fd, &st) < 0) { ::close(fd); fd = -1; return false; }
        size = (size_t)st.st_size; if (size == 0) return true;
        void* p = ::mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) { ::close(fd); fd = -1; return false; }
        data = (const char*)p;
        return true;
    }
    void close() {
        if (data && data != MAP_FAILED) { ::munmap((void*)data, size); data = nullptr; }
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    ~MappedFile() { close(); }
};

static uint8_t CHAR_MAP[256];
inline void init_char_map() {
    static bool inited = false; if (inited) return; inited = true;
    memset(CHAR_MAP, 0, 256);
    for (int c = '0'; c <= '9'; ++c) CHAR_MAP[c] = (uint8_t)c;
    for (int c = 'a'; c <= 'z'; ++c) CHAR_MAP[c] = (uint8_t)c;
    for (int c = 'A'; c <= 'Z'; ++c) CHAR_MAP[c] = (uint8_t)(c + 32);
}

struct PopcountLUT { uint8_t count; uint32_t sum; };
static PopcountLUT LUT16[65536];
inline void init_lut16() {
    static bool inited = false; if (inited) return; inited = true;
    for (uint32_t i = 0; i < 65536; ++i) {
        uint8_t cnt = 0; uint32_t sm = 0;
        for (int b = 0; b < 16; ++b) { if ((i >> b) & 1) { cnt++; sm += b; } }
        LUT16[i] = { cnt, sm };
    }
}

inline uint32_t compute_crc(const char* s, uint32_t len) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < len; ++i) crc = _mm_crc32_u8(crc, (uint8_t)s[i]);
    return crc;
}

inline bool tokens_equal(const char* t1, uint32_t l1, const char* t2, uint32_t l2) {
    if (l1 != l2) return false;
    for (uint32_t i = 0; i < l1; ++i) if ((char)CHAR_MAP[(unsigned char)t1[i]] != (char)CHAR_MAP[(unsigned char)t2[i]]) return false;
    return true;
}

static uint64_t FILTER_BITMAP[16384];

struct TermSlot {
    uint32_t hash = 0, term_id = UINT32_MAX, len = 0;
    std::string text;
};

struct TermTable {
    std::vector<TermSlot> slots;
    uint32_t mask = 0, num_terms = 0;

    void init(size_t cap = 65536) {
        size_t sz = 1; while (sz < cap) sz <<= 1;
        slots.assign(sz, TermSlot{}); mask = (uint32_t)(sz - 1); num_terms = 0;
    }

    void grow() {
        size_t new_sz = slots.size() * 2;
        std::vector<TermSlot> old_slots = std::move(slots);
        slots.assign(new_sz, TermSlot{});
        mask = (uint32_t)(new_sz - 1);
        for (auto& s : old_slots) {
            if (s.term_id != UINT32_MAX) {
                uint32_t idx = s.hash & mask;
                while (slots[idx].term_id != UINT32_MAX) idx = (idx + 1) & mask;
                slots[idx] = std::move(s);
            }
        }
    }

    inline uint32_t get_or_insert(const char* s, uint32_t len, uint32_t crc) {
        if (num_terms * 2 >= slots.size()) grow();
        uint32_t idx = crc & mask;
        while (true) {
            if (slots[idx].term_id == UINT32_MAX) {
                slots[idx].hash = crc; slots[idx].len = len; slots[idx].term_id = num_terms++;
                slots[idx].text.assign(s, len); return slots[idx].term_id;
            }
            if (slots[idx].hash == crc && tokens_equal(slots[idx].text.data(), slots[idx].len, s, len)) {
                return slots[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
    }
    inline uint32_t lookup(uint32_t crc, const char* s, uint32_t len) const {
        uint32_t idx = crc & mask;
        while (true) {
            if (slots[idx].term_id == UINT32_MAX) return UINT32_MAX;
            if (slots[idx].hash == crc && tokens_equal(slots[idx].text.data(), slots[idx].len, s, len)) {
                return slots[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
    }
};

enum class QueryType : uint8_t { BOOL_QUERY, PHRASE_QUERY };
enum class BoolOp : uint8_t { AND = 1, OR = 2, NOT = 3 };

struct BoolQuery { uint8_t num_terms; uint32_t term_ids[6]; BoolOp ops[5]; };
struct PhraseQuery { uint8_t num_terms; uint32_t term_ids[8]; };

struct Query {
    QueryType type;
    union { BoolQuery b; PhraseQuery p; };
    Query() {}
};

struct TermInfo {
    uint32_t df = 0, tf = 0;
    uint32_t dense_idx = UINT32_MAX;
    uint32_t posting_offset = 0;
    uint32_t doc_offset = 0;
};

struct ThreadAccumulator {
    bool is_dense = false, is_empty = false;
    std::vector<uint32_t> ids, temp_ids;
    std::vector<uint64_t> bitset;
};

int main(int argc, char** argv) {
    prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
    std::ios_base::sync_with_stdio(false);
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

    init_char_map();
    init_lut16();

    std::string queries_path = data_dir + "/queries.txt";
    MappedFile queries_file;
    if (!queries_file.open(queries_path)) return 1;

    TermTable term_table;
    term_table.init(65536);
    std::vector<Query> queries;
    queries.reserve(65536);

    const char* qp = queries_file.data, *qend = queries_file.data + queries_file.size;
    auto register_term = [&](std::string_view tok) -> uint32_t {
        uint32_t crc = compute_crc(tok.data(), (uint32_t)tok.size());
        uint32_t tid = term_table.get_or_insert(tok.data(), (uint32_t)tok.size(), crc);
        uint32_t b1 = crc & 0xFFFFF, b2 = (crc * 0x9E3779B1u) & 0xFFFFF;
        FILTER_BITMAP[b1 >> 6] |= (1ULL << (b1 & 63));
        FILTER_BITMAP[b2 >> 6] |= (1ULL << (b2 & 63));
        return tid;
    };

    while (qp < qend) {
        const char* line_start = qp;
        while (qp < qend && *qp != '\n') qp++;
        const char* line_end = qp;
        if (qp < qend && *qp == '\n') qp++;
        while (line_end > line_start && (*(line_end - 1) == '\r' || *(line_end - 1) == ' ')) line_end--;
        if (line_end <= line_start) continue;

        std::string_view line(line_start, (size_t)(line_end - line_start));
        if (line.starts_with("BOOL ")) {
            Query q; q.type = QueryType::BOOL_QUERY; q.b.num_terms = 0;
            const char* cur = line_start + 5; int pos = 0;
            while (cur < line_end) {
                const char* tok_start = cur;
                while (cur < line_end && *cur != ' ') cur++;
                std::string_view tok(tok_start, (size_t)(cur - tok_start));
                if (cur < line_end && *cur == ' ') cur++;
                if ((pos % 2) == 0) q.b.term_ids[q.b.num_terms++] = register_term(tok);
                else {
                    if (tok == "AND") q.b.ops[q.b.num_terms - 1] = BoolOp::AND;
                    else if (tok == "OR") q.b.ops[q.b.num_terms - 1] = BoolOp::OR;
                    else if (tok == "NOT") q.b.ops[q.b.num_terms - 1] = BoolOp::NOT;
                }
                pos++;
            }
            queries.push_back(q);
        } else if (line.starts_with("PHRASE \"") && line.ends_with("\"")) {
            Query q; q.type = QueryType::PHRASE_QUERY; q.p.num_terms = 0;
            const char* cur = line_start + 8, *p_term_end = line_end - 1;
            while (cur < p_term_end) {
                const char* tok_start = cur;
                while (cur < p_term_end && *cur != ' ') cur++;
                std::string_view tok(tok_start, (size_t)(cur - tok_start));
                if (cur < p_term_end && *cur == ' ') cur++;
                uint32_t tid = register_term(tok);
                q.p.term_ids[q.p.num_terms++] = tid;
            }
            queries.push_back(q);
        }
    }

    uint32_t num_query_terms = term_table.num_terms;

    std::string docs_path = data_dir + "/docs.txt";
    MappedFile docs_file;
    if (!docs_file.open(docs_path)) return 1;

    uint32_t N = 0;
    {
        const char* p = docs_file.data + docs_file.size - 1;
        while (p > docs_file.data && *p == '\n') p--;
        while (p > docs_file.data && *p != '\n') p--;
        if (*p == '\n') p++;
        while (p < docs_file.data + docs_file.size && *p >= '0' && *p <= '9') {
            N = N * 10 + (uint32_t)(*p++ - '0');
        }
        N += 1;
    }

    size_t num_words = (N + 63) / 64;
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 8;
    unsigned int num_threads = std::min(hw_threads, 12u);

    uint32_t DF_THRESHOLD = std::max(256u, (uint32_t)(N / 32));

    std::vector<TermInfo> terms(num_query_terms);
    std::vector<std::vector<uint32_t>> thread_df(num_threads, std::vector<uint32_t>(num_query_terms, 0));
    std::vector<std::vector<uint32_t>> thread_tf(num_threads, std::vector<uint32_t>(num_query_terms, 0));
    std::vector<std::vector<uint32_t>> thread_doc_offsets(num_threads, std::vector<uint32_t>(num_query_terms, 0));
    std::vector<std::vector<uint32_t>> thread_pos_offsets(num_threads, std::vector<uint32_t>(num_query_terms, 0));

    uint64_t* dense_bitsets = nullptr;
    uint32_t* sparse_docs = nullptr;
    uint64_t* positional_postings = nullptr;
    uint32_t num_dense_terms = 0, total_sparse_docs = 0, total_postings = 0;

    size_t chunk_bytes = docs_file.size / num_threads;
    std::vector<std::string> thread_outputs(num_threads);
    std::barrier sync_point(num_threads);

    std::vector<std::thread> workers;
    for (unsigned int tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            size_t b_start = tid * chunk_bytes;
            size_t b_end = (tid == num_threads - 1) ? docs_file.size : (tid + 1) * chunk_bytes;

            const char* file_end = docs_file.data + docs_file.size;
            const char* p = docs_file.data + b_start;
            if (tid > 0) {
                p = find_char_avx2(p, file_end, '\n');
                if (p < file_end && *p == '\n') p++;
            }
            const char* my_start = p;

            uint32_t* my_df = thread_df[tid].data();
            uint32_t* my_tf = thread_tf[tid].data();
            std::vector<uint32_t> last_seen(num_query_terms, 0xFFFFFFFF);

            // ==========================================
            // PASS 1: Count DF and TF (Zero allocations!)
            // ==========================================
            while (p < docs_file.data + b_end && p < file_end) {
                const char* tab_pos = find_char_avx2(p, file_end, '\t');
                if (tab_pos >= file_end) break;
                uint32_t doc_id = 0;
                for (const char* cp = p; cp < tab_pos; ++cp) doc_id = doc_id * 10 + (uint32_t)(*cp - '0');

                const char* dp = tab_pos + 1;
                const char* text_end = find_char_avx2(dp, file_end, '\n');

                while (dp < text_end) {
                    uint8_t c = CHAR_MAP[(unsigned char)*dp++]; if (!c) continue;
                    const char* tok_start = dp - 1;
                    uint32_t crc = _mm_crc32_u8(0, c);
                    while (dp < text_end) {
                        c = CHAR_MAP[(unsigned char)*dp]; if (!c) break;
                        crc = _mm_crc32_u8(crc, c); dp++;
                    }
                    uint32_t tok_len = (uint32_t)(dp - tok_start);
                    uint32_t b1 = crc & 0xFFFFF;
                    if (!((FILTER_BITMAP[b1 >> 6] >> (b1 & 63)) & 1ULL)) continue;
                    uint32_t b2 = (crc * 0x9E3779B1u) & 0xFFFFF;
                    if (!((FILTER_BITMAP[b2 >> 6] >> (b2 & 63)) & 1ULL)) continue;

                    uint32_t term_id = term_table.lookup(crc, tok_start, tok_len);
                    if (term_id != UINT32_MAX) {
                        my_tf[term_id]++;
                        if (last_seen[term_id] != doc_id) {
                            last_seen[term_id] = doc_id;
                            my_df[term_id]++;
                        }
                    }
                }
                p = (text_end < file_end && *text_end == '\n') ? text_end + 1 : file_end;
            }

            sync_point.arrive_and_wait();

            if (tid == 0) {
                uint32_t cur_doc_offset = 0, cur_pos_offset = 0;
                for (uint32_t t = 0; t < num_query_terms; ++t) {
                    uint32_t tot_df = 0, tot_tf = 0;
                    for (unsigned int k = 0; k < num_threads; ++k) {
                        tot_df += thread_df[k][t];
                        tot_tf += thread_tf[k][t];
                    }
                    terms[t].df = tot_df;
                    terms[t].tf = tot_tf;

                    if (tot_df >= DF_THRESHOLD) {
                        terms[t].dense_idx = num_dense_terms++;
                    } else if (tot_df > 0) {
                        terms[t].doc_offset = cur_doc_offset;
                        uint32_t running = cur_doc_offset;
                        for (unsigned int k = 0; k < num_threads; ++k) {
                            thread_doc_offsets[k][t] = running;
                            running += thread_df[k][t];
                        }
                        cur_doc_offset += tot_df;
                    }

                    if (tot_tf > 0) {
                        terms[t].posting_offset = cur_pos_offset;
                        uint32_t running_pos = cur_pos_offset;
                        for (unsigned int k = 0; k < num_threads; ++k) {
                            thread_pos_offsets[k][t] = running_pos;
                            running_pos += thread_tf[k][t];
                        }
                        cur_pos_offset += tot_tf;
                    }
                }
                total_sparse_docs = cur_doc_offset;
                total_postings = cur_pos_offset;

                std::vector<std::vector<uint32_t>>().swap(thread_df);
                std::vector<std::vector<uint32_t>>().swap(thread_tf);

                size_t dense_u64 = (size_t)num_dense_terms * num_words;
                dense_bitsets = (uint64_t*)calloc(dense_u64 > 0 ? dense_u64 : 1, sizeof(uint64_t));
                sparse_docs = (uint32_t*)malloc((size_t)(total_sparse_docs > 0 ? total_sparse_docs : 1) * sizeof(uint32_t));
                positional_postings = (uint64_t*)malloc((size_t)(total_postings > 0 ? total_postings : 1) * sizeof(uint64_t));
            }

            sync_point.arrive_and_wait();

            // ==========================================
            // PASS 2: Populate sparse_docs, dense_bitsets, and positional_postings
            // ==========================================
            uint32_t* my_doc_offsets = thread_doc_offsets[tid].data();
            uint32_t* my_pos_offsets = thread_pos_offsets[tid].data();
            p = my_start;
            std::fill(last_seen.begin(), last_seen.end(), 0xFFFFFFFF);

            while (p < docs_file.data + b_end && p < file_end) {
                const char* tab_pos = find_char_avx2(p, file_end, '\t');
                if (tab_pos >= file_end) break;
                uint32_t doc_id = 0;
                for (const char* cp = p; cp < tab_pos; ++cp) doc_id = doc_id * 10 + (uint32_t)(*cp - '0');

                const char* dp = tab_pos + 1;
                const char* text_end = find_char_avx2(dp, file_end, '\n');
                uint16_t tok_pos = 0;

                while (dp < text_end) {
                    uint8_t c = CHAR_MAP[(unsigned char)*dp++]; if (!c) continue;
                    const char* tok_start = dp - 1;
                    uint32_t crc = _mm_crc32_u8(0, c);
                    while (dp < text_end) {
                        c = CHAR_MAP[(unsigned char)*dp]; if (!c) break;
                        crc = _mm_crc32_u8(crc, c); dp++;
                    }
                    uint32_t tok_len = (uint32_t)(dp - tok_start);
                    uint32_t b1 = crc & 0xFFFFF;
                    if (!((FILTER_BITMAP[b1 >> 6] >> (b1 & 63)) & 1ULL)) { tok_pos++; continue; }
                    uint32_t b2 = (crc * 0x9E3779B1u) & 0xFFFFF;
                    if (!((FILTER_BITMAP[b2 >> 6] >> (b2 & 63)) & 1ULL)) { tok_pos++; continue; }

                    uint32_t term_id = term_table.lookup(crc, tok_start, tok_len);
                    if (term_id != UINT32_MAX) {
                        positional_postings[my_pos_offsets[term_id]++] = ((uint64_t)doc_id << 16) | (uint64_t)tok_pos;
                        if (last_seen[term_id] != doc_id) {
                            last_seen[term_id] = doc_id;
                            const TermInfo& ti = terms[term_id];
                            if (ti.dense_idx != UINT32_MAX) {
                                __atomic_fetch_or((unsigned long long*)&dense_bitsets[(size_t)ti.dense_idx * num_words + (doc_id >> 6)], (1ULL << (doc_id & 63)), __ATOMIC_RELAXED);
                            } else {
                                sparse_docs[my_doc_offsets[term_id]++] = doc_id;
                            }
                        }
                    }
                    tok_pos++;
                }
                p = (text_end < file_end && *text_end == '\n') ? text_end + 1 : file_end;
            }

            sync_point.arrive_and_wait();
            if (tid == 0) {
                std::vector<std::vector<uint32_t>>().swap(thread_doc_offsets);
                std::vector<std::vector<uint32_t>>().swap(thread_pos_offsets);
            }
            sync_point.arrive_and_wait();

            // ==========================================
            // STAGE 3: Answer queries (Pure RAM, Microseconds!)
            // ==========================================
            size_t num_queries = queries.size();
            size_t q_step = (num_queries + num_threads - 1) / num_threads;
            size_t q_start = tid * q_step, q_end = std::min(num_queries, (tid + 1) * q_step);
            std::string& out = thread_outputs[tid];
            out.reserve((q_end > q_start ? (q_end - q_start) : 1) * 32);

            ThreadAccumulator R;
            R.bitset.resize(num_words);
            std::vector<uint32_t> matches;
            matches.reserve(128);
            char line_buf[256];

            auto emit_result = [&](uint32_t count, uint64_t sum, const uint32_t* first_ids, uint32_t num_ids) {
                if (count == 0) { out.append("0 0\n"); return; }
                char* lp = line_buf;
                lp = write_u64_fast(lp, count); *lp++ = ' ';
                lp = write_u64_fast(lp, sum);
                for (uint32_t j = 0; j < num_ids; ++j) { *lp++ = ' '; lp = write_u64_fast(lp, first_ids[j]); }
                *lp++ = '\n';
                out.append(line_buf, lp - line_buf);
            };

            for (size_t qidx = q_start; qidx < q_end; ++qidx) {
                const Query& q = queries[qidx];
                if (q.type == QueryType::BOOL_QUERY) {
                    const TermInfo& info0 = terms[q.b.term_ids[0]];

                    if (info0.df == 0) { R.is_empty = true; R.is_dense = false; R.ids.clear(); }
                    else if (info0.dense_idx != UINT32_MAX) {
                        R.is_empty = false; R.is_dense = true;
                        memcpy(R.bitset.data(), dense_bitsets + (size_t)info0.dense_idx * num_words, num_words * sizeof(uint64_t));
                    } else {
                        R.is_empty = false; R.is_dense = false;
                        R.ids.assign(sparse_docs + info0.doc_offset, sparse_docs + info0.doc_offset + info0.df);
                    }

                    for (int i = 1; i < q.b.num_terms; ++i) {
                        BoolOp op = q.b.ops[i - 1];
                        const TermInfo& ti = terms[q.b.term_ids[i]];

                        if (ti.df == 0) {
                            if (op == BoolOp::AND) { R.is_empty = true; R.is_dense = false; R.ids.clear(); }
                            continue;
                        }

                        if (R.is_empty) {
                            if (op == BoolOp::AND || op == BoolOp::NOT) continue;
                            if (ti.dense_idx != UINT32_MAX) {
                                R.is_empty = false; R.is_dense = true;
                                memcpy(R.bitset.data(), dense_bitsets + (size_t)ti.dense_idx * num_words, num_words * sizeof(uint64_t));
                            } else {
                                R.is_empty = false; R.is_dense = false;
                                R.ids.assign(sparse_docs + ti.doc_offset, sparse_docs + ti.doc_offset + ti.df);
                            }
                            continue;
                        }

                        bool ti_dense = (ti.dense_idx != UINT32_MAX);

                        if (!R.is_dense && !ti_dense) {
                            const uint32_t *a = R.ids.data(), *a_end = a + R.ids.size(), *b = sparse_docs + ti.doc_offset, *b_end = b + ti.df;
                            R.temp_ids.clear();
                            if (op == BoolOp::AND) {
                                while (a < a_end && b < b_end) { if (*a < *b) a++; else if (*b < *a) b++; else { R.temp_ids.push_back(*a++); b++; } }
                            } else if (op == BoolOp::NOT) {
                                while (a < a_end && b < b_end) { if (*a < *b) R.temp_ids.push_back(*a++); else if (*b < *a) b++; else { a++; b++; } }
                                while (a < a_end) R.temp_ids.push_back(*a++);
                            } else if (op == BoolOp::OR) {
                                while (a < a_end && b < b_end) { if (*a < *b) R.temp_ids.push_back(*a++); else if (*b < *a) R.temp_ids.push_back(*b++); else { R.temp_ids.push_back(*a++); b++; } }
                                while (a < a_end) R.temp_ids.push_back(*a++);
                                while (b < b_end) R.temp_ids.push_back(*b++);
                            }
                            R.ids.swap(R.temp_ids);
                            if (R.ids.empty()) R.is_empty = true;
                            else if (op == BoolOp::OR && R.ids.size() >= DF_THRESHOLD) {
                                memset(R.bitset.data(), 0, num_words * sizeof(uint64_t));
                                for (uint32_t id : R.ids) R.bitset[id >> 6] |= 1ULL << (id & 63);
                                R.is_dense = true; R.ids.clear();
                            }
                        } else if (!R.is_dense && ti_dense) {
                            const uint64_t* tb = dense_bitsets + (size_t)ti.dense_idx * num_words;
                            if (op == BoolOp::AND) {
                                uint32_t w = 0; for (uint32_t id : R.ids) if ((tb[id >> 6] >> (id & 63)) & 1ULL) R.ids[w++] = id;
                                R.ids.resize(w); if (w == 0) R.is_empty = true;
                            } else if (op == BoolOp::NOT) {
                                uint32_t w = 0; for (uint32_t id : R.ids) if (!((tb[id >> 6] >> (id & 63)) & 1ULL)) R.ids[w++] = id;
                                R.ids.resize(w); if (w == 0) R.is_empty = true;
                            } else if (op == BoolOp::OR) {
                                memcpy(R.bitset.data(), tb, num_words * sizeof(uint64_t));
                                for (uint32_t id : R.ids) R.bitset[id >> 6] |= 1ULL << (id & 63);
                                R.is_dense = true; R.ids.clear();
                            }
                        } else if (R.is_dense && !ti_dense) {
                            const uint32_t* b = sparse_docs + ti.doc_offset;
                            if (op == BoolOp::AND) {
                                R.ids.clear();
                                for (uint32_t j = 0; j < ti.df; ++j) if ((R.bitset[b[j] >> 6] >> (b[j] & 63)) & 1ULL) R.ids.push_back(b[j]);
                                R.is_dense = false; if (R.ids.empty()) R.is_empty = true;
                            } else if (op == BoolOp::NOT) {
                                for (uint32_t j = 0; j < ti.df; ++j) R.bitset[b[j] >> 6] &= ~(1ULL << (b[j] & 63));
                            } else if (op == BoolOp::OR) {
                                for (uint32_t j = 0; j < ti.df; ++j) R.bitset[b[j] >> 6] |= 1ULL << (b[j] & 63);
                            }
                        } else {
                            const uint64_t* tb = dense_bitsets + (size_t)ti.dense_idx * num_words;
                            if (op == BoolOp::AND) for (size_t w = 0; w < num_words; ++w) R.bitset[w] &= tb[w];
                            else if (op == BoolOp::OR) for (size_t w = 0; w < num_words; ++w) R.bitset[w] |= tb[w];
                            else if (op == BoolOp::NOT) for (size_t w = 0; w < num_words; ++w) R.bitset[w] &= ~tb[w];
                        }
                    }

                    if (R.is_empty) { out.append("0 0\n"); }
                    else if (!R.is_dense) {
                        uint64_t sum = 0; for (uint32_t id : R.ids) sum += id;
                        emit_result((uint32_t)R.ids.size(), sum, R.ids.data(), std::min((uint32_t)R.ids.size(), 10u));
                    } else {
                        uint32_t count = 0, found = 0, first10[10]; uint64_t sum = 0;
                        for (size_t w = 0; w < num_words; ++w) {
                            uint64_t val = R.bitset[w]; if (!val) continue;
                            uint64_t base_id = w * 64;
                            if (__builtin_expect(found < 10, 0)) {
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
                                uint16_t c0 = (uint16_t)val, c1 = (uint16_t)(val >> 16), c2 = (uint16_t)(val >> 32), c3 = (uint16_t)(val >> 48);
                                sum += LUT16[c0].sum + (uint64_t)LUT16[c0].count * base_id;
                                sum += LUT16[c1].sum + (uint64_t)LUT16[c1].count * (base_id + 16);
                                sum += LUT16[c2].sum + (uint64_t)LUT16[c2].count * (base_id + 32);
                                sum += LUT16[c3].sum + (uint64_t)LUT16[c3].count * (base_id + 48);
                                count += (uint32_t)(LUT16[c0].count + LUT16[c1].count + LUT16[c2].count + LUT16[c3].count);
                            }
                        }
                        emit_result(count, sum, first10, found);
                    }
                } else {
                    // ==========================================
                    // PHRASE QUERY via Positional Postings (ZERO Disk I/O, Sub-Microsecond!)
                    // ==========================================
                    int k = q.p.num_terms;
                    bool has_absent = false; int best_idx = 0; uint32_t min_tf = terms[q.p.term_ids[0]].tf;
                    for (int i = 0; i < k; ++i) {
                        uint32_t ctf = terms[q.p.term_ids[i]].tf;
                        if (ctf == 0) { has_absent = true; break; }
                        if (ctf < min_tf) { min_tf = ctf; best_idx = i; }
                    }
                    if (has_absent) { out.append("0 0\n"); continue; }

                    const TermInfo& anchor = terms[q.p.term_ids[best_idx]];
                    const uint64_t* a_start = positional_postings + anchor.posting_offset;
                    const uint64_t* a_end = a_start + anchor.tf;

                    matches.clear(); uint64_t sum = 0;
                    uint32_t last_matched_doc = UINT32_MAX;

                    for (const uint64_t* ap = a_start; ap < a_end; ++ap) {
                        uint64_t a_val = *ap;
                        uint32_t doc_id = (uint32_t)(a_val >> 16);
                        if (doc_id == last_matched_doc) continue;

                        int32_t a_pos = (int32_t)(uint16_t)a_val;
                        bool matched = true;

                        for (int j = 0; j < k; ++j) {
                            if (j == best_idx) continue;
                            int32_t target_pos = a_pos - best_idx + j;
                            if (target_pos < 0 || target_pos > 65535) { matched = false; break; }

                            uint64_t target_val = ((uint64_t)doc_id << 16) | (uint64_t)(uint16_t)target_pos;
                            const TermInfo& tj = terms[q.p.term_ids[j]];
                            const uint64_t* j_start = positional_postings + tj.posting_offset;
                            const uint64_t* j_end = j_start + tj.tf;

                            if (!std::binary_search(j_start, j_end, target_val)) {
                                matched = false; break;
                            }
                        }

                        if (matched) {
                            matches.push_back(doc_id);
                            sum += doc_id;
                            last_matched_doc = doc_id;
                        }
                    }
                    emit_result((uint32_t)matches.size(), sum, matches.data(), std::min((uint32_t)matches.size(), 10u));
                }
            }
        });
    }
    for (auto& w : workers) w.join();

    std::filesystem::create_directories(out_dir);
    std::string out_path = out_dir + "/results.txt";
    FILE* fp = fopen(out_path.c_str(), "wb");
    if (!fp) return 1;

    for (const auto& chunk : thread_outputs) {
        if (!chunk.empty()) fwrite(chunk.data(), 1, chunk.size(), fp);
    }
    fclose(fp);

    _exit(0);
}
