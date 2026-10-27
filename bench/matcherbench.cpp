// matcherbench — spec/BENCH.md measurement protocol (C++).
//   matcherbench <corpus-prefix> [--tag name] [--mode latency|throughput|scale]
//                [--threads N] [--reps R]
//
//   latency    (default) per-op timed apply(); row as BENCH.md §3.
//   throughput untimed apply_batch() over the whole run, best of R reps.
//   scale      N shard threads, each owning a private book replaying the
//              corpus as its own symbol partition (SCALING.md); aggregate
//              ops/s = N*ops / slowest shard's wall time, best of R reps.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

using namespace matcher;
using Clock = std::chrono::steady_clock;

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

static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

static void high_qos() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); // P-cores (no pinning on macOS)
#endif
}

// One untimed batched replay on a fresh book; returns run wall seconds.
static double replay(const BookConfig& cfg, const std::vector<Command>& setup,
                     const std::vector<Command>& run, std::uint64_t& acc) {
    OrderBook book(cfg);
    book.prefault();
    NullSink sink;
    book.apply_batch(setup.data(), setup.size(), sink);
    auto t0 = Clock::now();
    book.apply_batch(run.data(), run.size(), sink);
    double s = secs(Clock::now() - t0);
    acc += sink.acc;
    return s;
}

static int latency_mode(const std::string& tag, const BookConfig& cfg,
                        const std::vector<Command>& setup, const std::vector<Command>& run) {
    // Warmup.
    {
        OrderBook book(cfg);
        NullSink sink;
        for (auto& c : setup) book.apply(c, sink);
        for (std::size_t i = 0; i < run.size() / 10; ++i) book.apply(run[i], sink);
        asm volatile("" ::"r"(sink.acc) : "memory");
    }

    OrderBook book(cfg);
    book.prefault();
    NullSink sink;
    for (auto& c : setup) book.apply(c, sink);

    std::vector<std::uint64_t> lat(run.size());
    auto wall0 = Clock::now();
    for (std::size_t i = 0; i < run.size(); ++i) {
        auto t0 = Clock::now();
        book.apply(run[i], sink);
        lat[i] = std::uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    auto wall_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - wall0).count();
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
    return 0;
}

static int throughput_mode(const std::string& tag, const BookConfig& cfg,
                           const std::vector<Command>& setup, const std::vector<Command>& run,
                           int reps) {
    high_qos();
    std::uint64_t acc = 0;
    std::vector<double> t;
    replay(cfg, setup, run, acc); // warmup
    for (int r = 0; r < reps; ++r) t.push_back(replay(cfg, setup, run, acc));
    asm volatile("" ::"r"(acc) : "memory");
    std::sort(t.begin(), t.end());
    const double ops = double(run.size());
    std::printf("| %s | throughput | 1 | %zu | %.0f | %.0f | %.2f |\n", tag.c_str(), run.size(),
                ops / t.front(), ops / t[t.size() / 2], 1e9 * t[t.size() / 2] / ops);
    return 0;
}

static int scale_mode(const std::string& tag, const BookConfig& cfg,
                      const std::vector<Command>& setup, const std::vector<Command>& run,
                      int threads, int reps) {
    std::vector<double> agg;
    for (int r = 0; r < reps + 1; ++r) { // rep 0 is warmup
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<double> wall(threads);
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; ++t) {
            ts.emplace_back([&, t] {
                high_qos();
                // Book built on its own thread: first-touch places its pages.
                OrderBook book(cfg);
                book.prefault();
                NullSink sink;
                book.apply_batch(setup.data(), setup.size(), sink);
                ready.fetch_add(1);
                while (!go.load(std::memory_order_acquire)) {}
                auto t0 = Clock::now();
                book.apply_batch(run.data(), run.size(), sink);
                wall[t] = secs(Clock::now() - t0);
                asm volatile("" ::"r"(sink.acc) : "memory");
            });
        }
        while (ready.load() < threads) {}
        go.store(true, std::memory_order_release);
        for (auto& th : ts) th.join();
        if (r > 0)
            agg.push_back(double(threads) * double(run.size()) /
                          *std::max_element(wall.begin(), wall.end()));
    }
    std::sort(agg.begin(), agg.end());
    std::printf("| %s | scale | %d | %zu | %.0f | %.0f | %.0f |\n", tag.c_str(), threads,
                run.size(), agg.back(), agg[agg.size() / 2], agg[agg.size() / 2] / threads);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: matcherbench <corpus-prefix> [--tag name] "
                     "[--mode latency|throughput|scale] [--threads N] [--reps R]\n");
        return 2;
    }
    std::string prefix = argv[1], tag = prefix, mode = "latency";
    int threads = int(std::thread::hardware_concurrency()), reps = 5;
    if (auto p = prefix.rfind('/'); p != std::string::npos) tag = prefix.substr(p + 1);
    for (int i = 2; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--tag") tag = argv[i + 1];
        else if (a == "--mode") mode = argv[i + 1];
        else if (a == "--threads") threads = std::atoi(argv[i + 1]);
        else if (a == "--reps") reps = std::max(1, std::atoi(argv[i + 1]));
    }

    BookConfig cfg;
    std::vector<Command> setup, run;
    load(prefix + ".setup.cmd.jsonl", cfg, setup);
    { BookConfig dummy; load(prefix + ".run.cmd.jsonl", dummy, run); }

    int rc;
    if (mode == "throughput") rc = throughput_mode(tag, cfg, setup, run, reps);
    else if (mode == "scale") rc = scale_mode(tag, cfg, setup, run, threads, reps);
    else rc = latency_mode(tag, cfg, setup, run);
    std::fprintf(stderr, "env: arm64 / darwin / c++ (%s)\n", mode.c_str());
    return rc;
}
