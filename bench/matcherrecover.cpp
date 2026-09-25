// matcherrecover — e2e recovery: load a matcher-snap/1 snapshot, replay a
// command journal tail, emit the canonical tagged event journal to stdout.
// Exits nonzero on a malformed journal line (truncated tail write).
//   matcherrecover <snap.jsonl> <cmd-tail.jsonl>
#include <fstream>
#include <iostream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <sstream>
#include <string>

using namespace matcher;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: matcherrecover <snap.jsonl> <cmd-tail.jsonl>\n";
        return 2;
    }
    std::ifstream sin(argv[1]);
    std::stringstream ss;
    ss << sin.rdbuf();
    Engine eng = snapshot::restore_engine(snapshot::parse(ss.str()));

    std::ifstream in(argv[2]);
    std::string line, scratch;
    Command c; Symbol sym;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line.find("\"format\"") != std::string::npos) continue;
        if (!jsonflat::parse_command(line, c, &sym)) {
            std::cerr << argv[2] << ":" << lineno << ": malformed journal line: " << line << "\n";
            return 2;
        }
        eng.submit_tagged(sym, c, [&](Symbol s, std::uint64_t seq, const Event& ev) {
            scratch.clear();
            write_canonical_sym(seq, s, ev, scratch);
            scratch.push_back('\n');
            std::cout << scratch;
        });
    }
}
