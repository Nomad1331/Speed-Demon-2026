#include <algorithm>
#include <thread>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using Id = uint32_t;
using Ids = std::vector<Id>;

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

struct Query {
    bool phrase;
    Ids terms;
    std::vector<char> ops;
};

// Each ASCII letter and digit maps to one non-zero base-37 digit.
constexpr auto symbols = [] {
    std::array<unsigned char,256> codes{};
    for(int i=0;i<26;++i) codes['A'+i]=codes['a'+i]=i+1;
    for(int i=0;i<10;++i) codes['0'+i]=i+27;
    return codes;
}();

class Vocabulary {
    struct Slot { uint64_t key; const std::string* word; Id id; };
    std::vector<Slot> slots;
    std::array<Id,37*37*37> short_words{};
    size_t slot(uint64_t key) const {
        key ^= key >> 29;
        key *= 0x9e3779b97f4a7c15ULL;
        return (key ^ (key >> 32)) & (slots.size()-1);
    }
    std::unordered_map<std::string, Id, StringHash, std::equal_to<>> dictionary;


    Id intern(const std::string& word) {
        auto [it, inserted] = dictionary.try_emplace(word, dictionary.size()+1);
        return it->second;
    }

public:
    std::vector<Query> read_queries(std::istream& file) {
        std::vector<Query> result;
        std::string line;
        while (std::getline(file, line)) {
            const bool phrase = line.starts_with("PHRASE ");
            std::istringstream fields(phrase ? line.substr(8, line.size()-9) : line.substr(5));
            Query q{phrase, {}, {}};
            std::string word;
            while (fields >> word) {
                q.terms.push_back(intern(word));
                if (!phrase && fields >> word) q.ops.push_back(word.at(0));
            }
            result.push_back(std::move(q));
        }
        size_t nslots=1;
        while(nslots<2*dictionary.size()) nslots*=2;
        slots.resize(nslots);
        for(const auto& [word,id] : dictionary) {
            uint64_t key=0;
            for(unsigned char c:word) key=key*37+symbols[c];
            if (word.size() <= 3) {short_words[key] = id; continue;}
            auto at=slot(key);
            while(slots[at].word) at=(at+1)&(slots.size()-1);
            slots[at]={key,&word,id};
        }
        return result;
    }

    size_t size() const { return dictionary.size()+1; }
    Id find(uint64_t key, std::string_view text) const {
        if (text.size() <= 3) return short_words[key];
        for(auto at=slot(key);slots[at].word;at=(at+1)&(slots.size()-1)) {
            const auto& e=slots[at];
            if(e.key!=key || e.word->size()!=text.size()) continue;
            // A base-37 key is exact through length 12; check longer words for overflow collisions.
            if(text.size()<=12 || std::equal(text.begin(),text.end(),e.word->begin(),
               [](unsigned char a,unsigned char b){ return symbols[a]==symbols[b]; })) return e.id;
        }
        return 0;
    }
};

struct Summary {
    uint64_t sum = 0;
    Id count = 0;
    std::array<Id,10> first{};
    void append(Id doc) {
        if (count < first.size()) first[count] = doc;
        ++count;
        sum += doc;
    }
    void merge(const Summary& next) {
        for (size_t i=0; i<next.count && count+i<first.size(); ++i) first[count+i]=next.first[i];
        count+=next.count;
        sum+=next.sum;
    }
};

class Index {
    std::vector<Ids> postings;
    Ids tokens;
    std::vector<size_t> offsets{0};
    Id first_doc = 0;
public:
    explicit Index(size_t vocabulary_size) : postings(vocabulary_size) {}

    void read_documents(std::ifstream& file, size_t begin, size_t end, const Vocabulary& vocab) {
        tokens.reserve((end-begin)/5);
        std::string line;
        // A document belongs to the unique byte interval containing its line start.
        if (begin) {
            file.seekg(begin-1);
            std::getline(file,line);
            const auto first = file.tellg();
            if (first < 0) return;
            begin = static_cast<size_t>(first);
        }
        while (begin < end && std::getline(file,line)) {
            begin += line.size()+1;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) throw std::runtime_error("Missing document delimiter");
            Id doc_id{};
            auto [id_end, ec] = std::from_chars(line.data(), line.data()+tab, doc_id);
            if (offsets.size() == 1) first_doc = doc_id;
            doc_id -= first_doc;
            if (ec != std::errc{} || id_end != line.data()+tab || doc_id != offsets.size()-1)
                throw std::runtime_error("Unexpected document id");
            for (size_t pos=tab+1; pos<line.size();) {
                if (!symbols[static_cast<unsigned char>(line[pos])]) { ++pos; continue; }
                const size_t start = pos;
                uint64_t key=0;
                while(pos<line.size()) {
                    const auto digit=symbols[static_cast<unsigned char>(line[pos])];
                    if (!digit) break;
                    key=key*37+digit;
                    ++pos;
                }
                const Id term=vocab.find(key,std::string_view(line).substr(start,pos-start));
                // Zero records a token outside the query vocabulary, preserving adjacency.
                tokens.push_back(term);
                if (term) {
                    auto& docs = postings[term];
                    if (docs.empty() || docs.back()!=doc_id) docs.push_back(doc_id);
                }
            }
            offsets.push_back(tokens.size());
        }
    }

    void answer(const Query& q, Summary& result, Ids& active, Ids& next) const {
        if (q.phrase) {
            const Id anchor = *std::min_element(q.terms.begin(),q.terms.end(),
                [&](Id a,Id b){return postings[a].size()<postings[b].size();});

            for (Id doc : postings[anchor]) {
                auto start = tokens.begin()+offsets[doc], stop = tokens.begin()+offsets[doc+1];
                if (std::search(start,stop,q.terms.begin(),q.terms.end())!=stop) result.append(doc+first_doc);
            }
            return;
        }
        const auto& first = postings[q.terms.front()];
        if (q.ops.empty()) {
            for (Id doc : first) result.append(doc+first_doc);
            return;
        }
        active.assign(first.begin(),first.end());
        for (size_t i=0; i<q.ops.size(); ++i) {
            next.clear();
            const auto& right = postings[q.terms[i+1]];
            if (active.empty()) {
                if (q.ops[i]=='O') active.assign(right.begin(),right.end());
                continue;
            }
            if (right.empty()) {
                if (q.ops[i]=='A') active.clear();
                continue;
            }
            auto out = std::back_inserter(next);
            switch (q.ops[i]) {
                case 'A': std::set_intersection(active.begin(),active.end(),right.begin(),right.end(),out); break;
                case 'O': std::set_union(active.begin(),active.end(),right.begin(),right.end(),out); break;
                case 'N': std::set_difference(active.begin(),active.end(),right.begin(),right.end(),out); break;
                default: throw std::runtime_error("Unknown boolean operator");
            }
            active.swap(next);
        }
        for (Id doc : active) result.append(doc+first_doc);
    }
};

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::string data_dir = "/data", out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (argv[i][0] != '-') {
            if (data_dir == "/data") data_dir = argv[i];
            else if (out_dir == "/out") out_dir = argv[i];
        }
    }
    const fs::path data = data_dir;
    const fs::path out = out_dir;
    const auto docs_path = data/"docs.txt";
    std::ifstream queries_file(data/"queries.txt");
    if (!queries_file) { std::cerr<<"Missing queries\n"; return 1; }
    Vocabulary vocabulary;
    const auto queries = vocabulary.read_queries(queries_file);
    const auto bytes = fs::file_size(docs_path);
    // Keep only one shard's postings per worker; the complete corpus never resides in RAM.
    constexpr size_t shard_bytes = 5*1024*1024;
    const size_t shards = (bytes+shard_bytes-1)/shard_bytes;
    const size_t cores = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    const size_t workers = std::min<size_t>({12,shards,cores});
    std::vector<std::vector<Summary>> partial(workers,std::vector<Summary>(queries.size()));
    auto worker = [&](size_t id) {
        Ids active, next;
        for (size_t shard=shards*id/workers; shard<shards*(id+1)/workers; ++shard) {
            std::ifstream docs_file(docs_path);
            if (!docs_file) throw std::runtime_error("Cannot read documents");
            Index index(vocabulary.size());
            index.read_documents(docs_file,shard*shard_bytes,std::min(bytes,(shard+1)*shard_bytes),vocabulary);
            for (size_t i=0;i<queries.size();++i) index.answer(queries[i],partial[id][i],active,next);
        }
    };
    std::vector<std::thread> threads;
    for (size_t i=1;i<workers;++i) threads.emplace_back(worker,i);
    if(workers) worker(0);
    for (auto& thread : threads) thread.join();
    fs::create_directories(out);
    std::ofstream results(out/"results.txt", std::ios::binary);
    results.exceptions(std::ios::failbit | std::ios::badbit);
    for (size_t i=0;i<queries.size();++i) {
        Summary r;
        for (const auto& shard : partial) r.merge(shard[i]);
        results<<r.count<<' '<<r.sum;
        for (size_t j=0;j<std::min<size_t>(10,r.count);++j) results<<' '<<r.first[j];
        results<<'\n';
    }
    results.close();
}
