// Regression: OrderMap backward-shift deletion used to overwrite a live key
// under dense churn. (1) model-based churn on OrderMap; (2) a 400-command
// dense-map stream whose expected events agree with matcher-java/-ts.
//   regress_test <tests/data dir>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <matcher/jsonflat.hpp>
#include <matcher/matcher.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using namespace matcher;

static int failures = 0;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        if (!(c)) { std::cerr << "FAIL: " << msg << "\n"; ++failures; } \
    } while (0)

static void ordermap_model() {
    std::uint64_t x = 0x12345678;
    auto next = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    for (std::size_t live_max : {8u, 32u, 100u, 512u}) {
        detail::OrderMap m(live_max);
        std::unordered_map<std::uint64_t, std::uint32_t> model;
        std::uint64_t ids = live_max * 3;
        for (std::uint32_t step = 0; step < 200000 && failures == 0; ++step) {
            std::uint64_t k = next() % ids;
            if (model.size() < live_max && next() % 2 == 0) {
                m.insert(k, step);
                model[k] = step;
            } else {
                bool had = model.erase(k) > 0;
                CHECK(had == (m.get(k) != nullptr), "presence of " << k << " at step " << step);
                m.remove(k);
            }
            CHECK(m.len == model.size(), "len at step " << step);
            if (step % 997 == 0)
                for (auto& [k2, v] : model) {
                    auto got = m.get(k2);
                    CHECK(got && *got == v, "lost key " << k2 << " at step " << step);
                }
        }
    }
}

static void dense_map_churn(const std::string& dir) {
    std::ifstream in(dir + "/dense_map_churn.cmd.jsonl");
    std::string line;
    std::getline(in, line);
    auto hdr = jsonflat::parse_header(line);
    std::vector<Command> cmds;
    Command c;
    while (std::getline(in, line))
        if (!line.empty() && jsonflat::parse_command(line, c)) cmds.push_back(c);
    std::ifstream ev(dir + "/dense_map_churn.evt.jsonl");
    std::vector<std::string> want;
    std::getline(ev, line);
    while (std::getline(ev, line)) want.push_back(line);
    for (IndexKind kind : {IndexKind::Ladder, IndexKind::Tree}) {
        BookConfig cfg;
        cfg.price_min = hdr.pmin;
        cfg.price_max = hdr.pmax;
        cfg.max_orders = hdr.max_orders;
        cfg.index = kind;
        OrderBook book(cfg);
        LinesSink sink;
        for (auto& cmd : cmds) book.apply(cmd, sink);
        CHECK(sink.lines == want, "dense_map_churn stream differs");
    }
}

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "tests/data";
    ordermap_model();
    dense_map_churn(dir);
    if (failures) return 1;
    std::cout << "regress: ordermap model + dense_map_churn passed\n";
    return 0;
}
