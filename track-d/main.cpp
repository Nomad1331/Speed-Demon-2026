#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <thread>
#include <atomic>
#include <queue>
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

inline char* write_i64_fast(char* p, int64_t val) {
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    return write_u64_fast(p, (uint64_t)val);
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

struct Edge {
    uint32_t v;
    uint32_t w;
};

struct RawEdge {
    uint32_t u;
    uint32_t v;
    uint32_t w;
};

struct Query {
    uint32_t s;
    uint32_t t;
};

struct QuadHeap {
    struct Node {
        uint64_t dist;
        uint32_t u;
    };
    std::vector<Node> c;

    QuadHeap() { c.reserve(65536); }

    void reserve(size_t cap) { c.reserve(cap); }
    void clear() { c.clear(); }
    bool empty() const { return c.empty(); }

    inline void push(uint64_t dist, uint32_t u) {
        c.push_back({dist, u});
        size_t i = c.size() - 1;
        while (i > 0) {
            size_t p = (i - 1) >> 2;
            if (dist < c[p].dist) {
                c[i] = c[p];
                i = p;
            } else {
                break;
            }
        }
        c[i] = {dist, u};
    }

    inline Node pop() {
        Node ret = c[0];
        Node last = c.back();
        c.pop_back();
        if (!c.empty()) {
            size_t i = 0;
            size_t n = c.size();
            while (true) {
                size_t child = (i << 2) + 1;
                if (child >= n) break;
                size_t best = child;
                size_t end_child = std::min(child + 4, n);
                for (size_t k = child + 1; k < end_child; ++k) {
                    if (c[k].dist < c[best].dist) best = k;
                }
                if (c[best].dist < last.dist) {
                    c[i] = c[best];
                    i = best;
                } else {
                    break;
                }
            }
            c[i] = last;
        }
        return ret;
    }
};

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    std::string data_dir = "/data";
    std::string out_dir = "/out";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        }
    }

    std::string graph_path = (std::filesystem::path(data_dir) / "graph.txt").string();
    std::string queries_path = (std::filesystem::path(data_dir) / "queries.txt").string();

    MappedFile gfile;
    if (!gfile.open(graph_path)) {
        std::cerr << "Cannot open " << graph_path << "\n";
        return 1;
    }

    MappedFile qfile;
    if (!qfile.open(queries_path)) {
        std::cerr << "Cannot open " << queries_path << "\n";
        return 1;
    }

    // 1. Parse graph header
    const char* gp = gfile.data;
    const char* gend = gp + gfile.size;

    while (gp < gend && (*gp == ' ' || *gp == '\r' || *gp == '\n')) ++gp;
    uint32_t n = 0;
    while (*gp >= '0') {
        n = n * 10 + (*gp++ - '0');
    }
    while (*gp == ' ') ++gp;
    uint32_t m = 0;
    while (*gp >= '0') {
        m = m * 10 + (*gp++ - '0');
    }
    while (*gp != '\n') ++gp;
    ++gp; // skip '\n'

    // 2. Parse queries.txt
    const char* qp = qfile.data;
    const char* qend = qp + qfile.size;
    std::vector<Query> queries;
    queries.reserve(2048);

    while (qp < qend) {
        while (qp < qend && (*qp == ' ' || *qp == '\r' || *qp == '\n')) ++qp;
        if (qp >= qend) break;
        uint32_t s = 0;
        while (*qp >= '0') {
            s = s * 10 + (*qp++ - '0');
        }
        ++qp; // ' '
        uint32_t t = 0;
        while (*qp >= '0') {
            t = t * 10 + (*qp++ - '0');
        }
        queries.push_back({s, t});
        while (qp < qend && *qp != '\n') ++qp;
        if (qp < qend && *qp == '\n') ++qp;
    }

    const size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries);

    // 3. Spawn workers concurrently while loading graph edges
    uint32_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > num_queries && num_queries > 0) num_threads = (uint32_t)num_queries;

    std::atomic<bool> graph_ready{false};
    std::atomic<size_t> query_cursor{0};
    std::vector<uint32_t> head(n + 1, 0);
    std::unique_ptr<Edge[]> edges;
    std::vector<uint8_t> has_in(n, 0);

    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (uint32_t tid = 0; tid < num_threads; ++tid) {
        workers.emplace_back([&]() {
            QuadHeap pq;
            pq.reserve(65536);
            std::unique_ptr<uint64_t[]> dist(new uint64_t[n]);
            std::vector<uint32_t> seen(n, 0);
            uint32_t epoch = 0;

            // Wait until graph CSR is ready
            graph_ready.wait(false, std::memory_order_acquire);

            while (true) {
                size_t q = query_cursor.fetch_add(1, std::memory_order_relaxed);
                if (q >= num_queries) break;

                uint32_t s = queries[q].s;
                uint32_t t = queries[q].t;

                if (s == t) {
                    answers[q] = 0;
                    continue;
                }
                if (head[s] == head[s + 1] || !has_in[t]) {
                    answers[q] = -1;
                    continue;
                }

                uint32_t cur_epoch = ++epoch;
                if (cur_epoch == 0) {
                    std::fill(seen.begin(), seen.end(), 0);
                    cur_epoch = ++epoch;
                }

                dist[s] = 0;
                seen[s] = cur_epoch;
                pq.clear();
                pq.push(0, s);

                int64_t ans = -1;
                while (!pq.empty()) {
                    auto top = pq.pop();
                    uint64_t d = top.dist;
                    uint32_t u = top.u;

                    if (d > dist[u]) continue;
                    if (u == t) {
                        ans = (int64_t)d;
                        break;
                    }

                    uint32_t start_edge = head[u];
                    uint32_t end_edge = head[u + 1];
                    for (uint32_t e = start_edge; e < end_edge; ++e) {
                        uint32_t v = edges[e].v;
                        uint64_t nd = d + edges[e].w;

                        if (seen[v] != cur_epoch) {
                            seen[v] = cur_epoch;
                            dist[v] = nd;
                            pq.push(nd, v);
                        } else if (nd < dist[v]) {
                            dist[v] = nd;
                            pq.push(nd, v);
                        }
                    }
                }
                answers[q] = ans;
                pq.clear();
            }
        });
    }

    // Main thread parses edges and builds CSR
    auto raw = std::make_unique_for_overwrite<RawEdge[]>(m);
    std::vector<uint32_t> deg(n, 0);

    for (uint32_t i = 0; i < m; ++i) {
        uint32_t u = 0;
        while (*gp >= '0') {
            u = u * 10 + (*gp++ - '0');
        }
        ++gp; // ' '
        uint32_t v = 0;
        while (*gp >= '0') {
            v = v * 10 + (*gp++ - '0');
        }
        ++gp; // ' '
        uint64_t w = 0;
        while (*gp >= '0') {
            w = w * 10 + (*gp++ - '0');
        }
        if (*gp == '\r') ++gp;
        ++gp; // '\n'

        raw[i] = {u, v, (uint32_t)w};
        deg[u]++;
        has_in[v] = 1;
    }

    // Build CSR
    for (uint32_t i = 0; i < n; ++i) {
        head[i + 1] = head[i] + deg[i];
    }
    deg.clear();
    deg.shrink_to_fit();

    std::vector<uint32_t> cur = head;
    edges = std::make_unique_for_overwrite<Edge[]>(m);
    for (uint32_t i = 0; i < m; ++i) {
        uint32_t u = raw[i].u;
        edges[cur[u]++] = {raw[i].v, raw[i].w};
    }
    raw.reset();
    cur.clear();
    cur.shrink_to_fit();

    // Signal workers to begin
    graph_ready.store(true, std::memory_order_release);
    graph_ready.notify_all();

    for (auto& w : workers) {
        w.join();
    }

    // 4. Output Generation
    // Each answer: at most 20 chars + 1 newline
    std::vector<char> out_buf(num_queries * 24 + 64);
    char* op = out_buf.data();
    for (size_t i = 0; i < num_queries; ++i) {
        op = write_i64_fast(op, answers[i]);
        *op++ = '\n';
    }

    std::filesystem::create_directories(out_dir);
    std::string out_path = (std::filesystem::path(out_dir) / "distances.txt").string();
    FILE* fp = fopen(out_path.c_str(), "wb");
    if (fp) {
        fwrite(out_buf.data(), 1, op - out_buf.data(), fp);
        fclose(fp);
    } else {
        std::cerr << "Failed to open output file " << out_path << "\n";
        return 1;
    }

    return 0;
}
