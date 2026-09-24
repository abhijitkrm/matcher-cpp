// matcherbench — spec/BENCH.md measurement protocol (C++).
//   matcherbench <corpus-prefix> [--tag name]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <string>
#include <vector>

using namespace matcher;

static void load(const std::string& path, BookConfig& cfg, std::vector<Command>& out) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    if (std::getline(in, line)) {
        auto h = jsonflat::parse_header(line);
        cfg.price_min = h.pmin;
        cfg.price_max = h.pmax;
        cfg.max_orders = h.max_orders;
        cfg.index = h.index;
    }
    Command c;
    while (std::getline(in, line)) {
        if (!line.empty() && jsonflat::parse_command(line, c)) out.push_back(c);
    }
}

static std::uint64_t pct(const std::vector<std::uint64_t>& s, double p) {
    if (s.empty()) return 0;
    auto i = std::size_t((double(s.size()) - 1.0) * p + 0.999999);
    return s[std::min(i, s.size() - 1)];
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: matcherbench <corpus-prefix> [--tag name]\n");
        return 2;
    }
    std::string prefix = argv[1], tag = prefix;
    if (auto p = prefix.rfind('/'); p != std::string::npos) tag = prefix.substr(p + 1);
    for (int i = 2; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--tag") tag = argv[i + 1];

    BookConfig cfg;
    std::vector<Command> setup, run;
    load(prefix + ".setup.cmd.jsonl", cfg, setup);
    { BookConfig dummy; load(prefix + ".run.cmd.jsonl", dummy, run); }

    // Warmup.
    {
        OrderBook book(cfg);
        NullSink sink;
        for (auto& c : setup) book.apply(c, sink);
        for (std::size_t i = 0; i < run.size() / 10; ++i) book.apply(run[i], sink);
        asm volatile("" ::"r"(sink.acc) : "memory");
    }

    OrderBook book(cfg);
    NullSink sink;
    for (auto& c : setup) book.apply(c, sink);

    std::vector<std::uint64_t> lat(run.size());
    auto wall0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < run.size(); ++i) {
        auto t0 = std::chrono::steady_clock::now();
        book.apply(run[i], sink);
        lat[i] = std::uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
    }
    auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - wall0)
                       .count();
    asm volatile("" ::"r"(sink.acc) : "memory");

    std::sort(lat.begin(), lat.end());
    std::size_t ops = run.size();
    double ops_s = ops / (double(wall_ns) / 1e9);
    unsigned __int128 sum = 0;
    for (auto v : lat) sum += v;
    auto mean = std::uint64_t(sum / ops);

    std::printf("| %s | %zu | %.0f | %llu | %llu | %llu | %llu | %llu | %llu |\n",
                tag.c_str(), ops, ops_s, (unsigned long long)mean,
                (unsigned long long)pct(lat, .50), (unsigned long long)pct(lat, .90),
                (unsigned long long)pct(lat, .99), (unsigned long long)pct(lat, .999),
                (unsigned long long)lat.back());
    std::fprintf(stderr, "env: arm64 / darwin / c++\n");
    return 0;
}
