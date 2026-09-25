// matcherrun — e2e runner: apply an engine command stream, emit the canonical
// tagged event journal to stdout, optionally write a snapshot at the end.
//   matcherrun <engine.cmd.jsonl> [--snap <path>]
#include <fstream>
#include <iostream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <string>

using namespace matcher;

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: matcherrun <cmd.jsonl> [--snap <path>]\n"; return 2; }
    const char* path = argv[1];
    std::string snap_path;
    for (int i = 2; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--snap") snap_path = argv[i + 1];

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    auto h = jsonflat::parse_header(line);
    BookConfig cfg;
    cfg.price_min = h.pmin; cfg.price_max = h.pmax;
    cfg.max_orders = h.max_orders; cfg.index = h.index;
    Engine eng(cfg);

    std::string scratch;
    Command c; Symbol sym;
    int lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        if (!jsonflat::parse_command(line, c, &sym)) {
            std::cerr << path << ":" << lineno << ": malformed command: " << line << "\n";
            return 2;
        }
        eng.submit_tagged(sym, c, [&](Symbol s, std::uint64_t seq, const Event& ev) {
            scratch.clear();
            write_canonical_sym(seq, s, ev, scratch);
            scratch.push_back('\n');
            std::cout << scratch;
        });
    }
    if (!snap_path.empty()) {
        std::string snap;
        snapshot::write_engine(eng, snap);
        std::ofstream(snap_path) << snap;
    }
}
