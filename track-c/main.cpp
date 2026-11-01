#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using i64 = std::int64_t;

struct Text {
    std::string_view encoded;
    std::size_t size;
};

// Outer quotes are omitted, but doubled quotes stay in the view. This encoding
// is canonical for a decoded name (an unquoted CSV field cannot contain quotes).
bool read_text(const char*& p, const char* end, Text& field) {
    bool quoted = *p == '"';
    if (quoted) ++p;
    const char* begin = p;
    std::size_t escaped = 0;
    for (;;) {
        const char* stop = static_cast<const char*>(std::memchr(p, quoted ? '"' : ',', end - p));
        if (!stop) return false;
        p = stop + 1;
        if (quoted && *p == '"') {
            ++escaped;
            ++p;
            continue;
        }
        field.encoded = {begin, static_cast<std::size_t>(stop - begin)};
        field.size = field.encoded.size() - escaped;
        return !quoted || *p++ == ',';
    }
}

bool number(const char*& p, const char* end, char delimiter, i64& n) {
    const auto result = std::from_chars(p, end, n);
    p = result.ptr;
    if (result.ec != std::errc{} || *p != delimiter) return false;
    ++p;
    return true;
}

struct Row {
    i64 id, region, amount;
    Text name, note;
};

bool read_row(const char*& p, const char* end, bool final, Row& row) {
    if (std::memcmp(p, "\xef\xbb\xbf", 3) == 0) p += 3;
    if (!number(p, end, ',', row.id) || !read_text(p, end, row.name) ||
        !read_text(p, end, row.note) || !number(p, end, ',', row.region)) return false;
    bool negative = *p == '-';
    if (negative) ++p;
    i64 whole;
    if (!number(p, end, '.', whole) || end - p < 2 || p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9')
        return false;
    row.amount = (negative ? -1 : 1) * (100 * whole + (p[0] - '0') * 10 + (p[1] - '0'));
    p += 2;
    if (*p == '\r') ++p;
    if (p == end) return final;
    return *p++ == '\n';
}

// Nodes are published once and retained until all parsing workers finish.
// The table is an index: compare full keys to resolve every hash collision.
class Distinct {
    struct Name {
        std::string_view text;
        Name* previous;
        std::atomic<std::uint64_t>* seen;
    };
    std::vector<std::atomic<Name*>> buckets;
    std::size_t words;
public:
    explicit Distinct(std::size_t countries) : buckets(1 << 17), words((countries + 63) / 64) {
        for (auto& head : buckets) head.store(nullptr, std::memory_order_relaxed);
    }
    bool insert(std::size_t country, std::string_view name, std::pmr::monotonic_buffer_resource& arena) {
        auto& bucket = buckets[std::hash<std::string_view>{}(name) & (buckets.size() - 1)];
        auto* head = bucket.load(std::memory_order_acquire);
        Name* fresh = nullptr;
        Name* entry;
        for (;;) {
            entry = head;
            while (entry && entry->text != name) entry = entry->previous;
            if (entry) break;
            if (!fresh) {
                // A worker owns its arena throughout parsing; shared keys and
                // bits outlive all workers. Allocation happens only on a miss.
                auto* key = static_cast<char*>(arena.allocate(name.size() + 1, alignof(char)));
                std::memcpy(key, name.data(), name.size());
                auto* storage = arena.allocate(words * sizeof(std::atomic<std::uint64_t>), alignof(std::atomic<std::uint64_t>));
                auto* bits = new(storage) std::atomic<std::uint64_t>[words]();
                for (std::size_t i = 0; i < words; ++i) bits[i].store(0, std::memory_order_relaxed);
                fresh = new(arena.allocate(sizeof(Name), alignof(Name))) Name{{key, name.size()}, nullptr, bits};
            }
            fresh->previous = head;
            if (bucket.compare_exchange_strong(head, fresh, std::memory_order_acq_rel)) {
                entry = fresh;
                break;
            }
        }
        auto mask = std::uint64_t{1} << (country % 64);
        auto& word = entry->seen[country / 64];
        return !(word.load(std::memory_order_relaxed) & mask) &&
               !(word.fetch_or(mask, std::memory_order_relaxed) & mask);
    }
};

struct Totals {
    i64 count = 0, ids = 0, sum = 0;
    i64 low = std::numeric_limits<i64>::max(), high = std::numeric_limits<i64>::min();
    std::size_t n_names = 0, name_bytes = 0, note_bytes = 0;
    void add(i64 id, i64 amount, bool fresh, std::size_t n, std::size_t m) {
        ++count; ids += id; sum += amount;
        low = std::min(amount, low); high = std::max(amount, high);
        name_bytes += n; note_bytes += m;
        n_names += fresh;
    }
    void merge(const Totals& other) {
        count += other.count; ids += other.ids; sum += other.sum;
        low = std::min(low, other.low); high = std::max(high, other.high);
        n_names += other.n_names; name_bytes += other.name_bytes; note_bytes += other.note_bytes;
    }
};


std::string money(i64 value) {
    auto absolute = value < 0 ? -value : value;
    auto cents = absolute % 100;
    return (value < 0 ? "-" : "") + std::to_string(absolute / 100) + (cents < 10 ? ".0" : ".") + std::to_string(cents);
}

void join(const fs::path& data, const fs::path& out) {
    std::ifstream lookup(data / "regions.csv");
    std::string row;
    if (!std::getline(lookup, row) || row != "region_id,country") throw std::runtime_error("invalid regions header");
    std::map<std::string, std::size_t> countries{{"_UNMATCHED", 0}};
    std::vector<std::uint16_t> regions;
    while (std::getline(lookup, row)) {
        std::size_t id = 0;
        auto [pos, ec] = std::from_chars(row.data(), row.data() + row.size(), id);
        if (ec != std::errc{} || pos == row.data() + row.size() || *pos != ',') throw std::runtime_error("bad region id");
        auto country = countries.try_emplace(std::string(pos + 1, row.data() + row.size() - pos - 1), countries.size()).first->second;
        if (regions.size() <= id) regions.resize(id + 1);
        regions[id] = country;
    }
    std::vector<Totals> groups(countries.size());
    Distinct distinct(groups.size());
    const auto records = data / "records.csv";
    std::ifstream input(records, std::ios::binary);
    if (!std::getline(input, row)) throw std::runtime_error("missing records header");
    if (row.compare(0, 3, "\xef\xbb\xbf") == 0) row.erase(0, 3);
    if (!row.empty() && row.back() == '\r') row.pop_back();
    if (row != "id,name,note,region_id,amount") throw std::runtime_error("invalid records header");

    const auto size = fs::file_size(records);
    const std::uint64_t offset = input.eof() ? size : static_cast<std::uint64_t>(input.tellg());
    // Spec 2.2 guarantees that CR occurs only in CRLF record terminators.
    // A bare LF inside a quoted note is therefore never used to split work.
    std::vector<std::uint64_t> boundaries{offset};
    unsigned workers = std::min<std::uint64_t>(8, 1 + size / (4 * 1024 * 1024));
    for (unsigned i = 1; i < workers; ++i) {
        auto cut = offset + (size - offset) * i / workers;
        input.seekg(cut);
        input.ignore(std::numeric_limits<std::streamsize>::max(), '\r');
        if (!input.good()) break;
        if (input.get() != '\n') throw std::runtime_error("invalid record break");
        auto position = static_cast<std::uint64_t>(input.tellg());
        if (position > boundaries.back() && position < size) boundaries.push_back(position);
    }
    boundaries.push_back(size);
    std::vector<std::pmr::monotonic_buffer_resource> arenas(boundaries.size() - 1);
    auto run = [&](std::size_t part) {
        auto begin = boundaries[part], end = boundaries[part + 1];
        std::ifstream input(records, std::ios::binary);
        input.seekg(begin);
        std::vector<Totals> local(countries.size());
        std::uint64_t remaining = end - begin;
        // A sentinel and two padding bytes make the EOF, BOM and quote lookahead
        // safe. Carry an incomplete row to the next block, growing for long fields.
        std::vector<char> buffer(256 * 1024 + 3);
        std::size_t tail = 0;
        for (;;) {
            const auto take = std::min<std::uint64_t>(buffer.size() - 3 - tail, remaining);
            input.read(buffer.data() + tail, take);
            if (static_cast<std::uint64_t>(input.gcount()) != take) throw std::runtime_error("short CSV read");
            remaining -= take;
            if (input.bad()) throw std::runtime_error("records.csv read failed");
            const bool final = remaining == 0;
            const std::size_t size = tail + input.gcount();
            std::memset(buffer.data() + size, 0, 3);
            const char* p = buffer.data();
            const char* end = p + size;
            Row record;
            while (p != end) {
                const char* begin = p;
                if (!read_row(p, end, final, record)) { p = begin; break; }
                auto region = static_cast<std::size_t>(record.region);
                auto country = region < regions.size() ? regions[region] : 0;
                bool fresh = distinct.insert(country, record.name.encoded, arenas[part]);
                local[country].add(record.id, record.amount, fresh, record.name.size, record.note.size);
            }
            tail = end - p;
            if (final) {
                if (tail) throw std::runtime_error("invalid CSV row");
                break;
            }
            std::memmove(buffer.data(), p, tail);
            if (tail == buffer.size() - 3) buffer.resize(2 * tail + 3);
        }
        return local;
    };
    std::vector<std::future<std::vector<Totals>>> jobs;
    for (std::size_t i = 1; i < boundaries.size(); ++i)
        jobs.push_back(std::async(std::launch::async, run, i - 1));
    for (auto& job : jobs) {
        auto local = job.get();
        for (std::size_t i = 0; i < groups.size(); ++i) groups[i].merge(local[i]);
    }
    fs::create_directories(out);
    std::ofstream output(out / "aggregates.txt", std::ios::binary);
    for (const auto& [code, index] : countries) {
        const auto& g = groups[index];
        if (g.count) output << code << ' ' << g.count << ' ' << g.ids << ' ' << money(g.sum) << ' ' << money(g.low)
                            << ' ' << money(g.high) << ' ' << g.n_names << ' ' << g.name_bytes << ' ' << g.note_bytes << '\n';
    }
    output.close();
    if (!output) throw std::runtime_error("aggregates.txt write failed");
}

int main(int argc, char** argv) {
    try {
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
        join(data_dir, out_dir);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
