// snapdump — apply an engine command stream, print the snapshot to stdout.
// Cross-language snapshot parity check (spec/JOURNAL.md).
//   matchersnap <engine.cmd.jsonl>
#include <fstream>
#include <iostream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <string>

using namespace matcher;

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: matchersnap <cmd.jsonl>\n"; return 2; }
    std::ifstream in(argv[1]);
    std::string line;
    std::getline(in, line);
    auto h = jsonflat::parse_header(line);
    BookConfig cfg;
    cfg.price_min = h.pmin; cfg.price_max = h.pmax;
    cfg.max_orders = h.max_orders; cfg.index = h.index;
    Engine eng(cfg);
    Command c; Symbol sym;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        jsonflat::parse_command(line, c, &sym);
        eng.submit_tagged(sym, c, [](Symbol, std::uint64_t, const Event&) {});
    }
    std::string snap;
    snapshot::write_engine(eng, snap);
    std::cout << snap;
}
