// matcherfuzz <corpus.cmd.jsonl> — replay a fuzzgen corpus, print the
// canonical event stream (symbol-tagged for engine corpora) to stdout.
// scripts/diffuzz.sh byte-diffs this output across implementations.
#include <fstream>
#include <iostream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <string>

using namespace matcher;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: matcherfuzz <cmd.jsonl>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }
    std::string line;
    std::getline(in, line);
    auto hdr = jsonflat::parse_header(line);
    BookConfig cfg;
    cfg.price_min = hdr.pmin;
    cfg.price_max = hdr.pmax;
    cfg.max_orders = hdr.max_orders;
    cfg.index = IndexKind::Ladder;

    std::string out;
    out.reserve(1 << 20);
    auto flush = [&] {
        std::fwrite(out.data(), 1, out.size(), stdout);
        out.clear();
    };

    if (hdr.engine) {
        Engine eng(cfg);
        Command cmd;
        Symbol sym = 0;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (!jsonflat::parse_command(line, cmd, &sym)) {
                std::cerr << "bad command line: " << line << "\n";
                return 2;
            }
            eng.submit_tagged(sym, cmd, [&](Symbol s, std::uint64_t seq, const Event& ev) {
                write_canonical_sym(seq, s, ev, out);
                out.push_back('\n');
            });
            if (out.size() > (1 << 20)) flush();
        }
    } else {
        OrderBook book(cfg);
        struct OutSink {
            std::string& out;
            void on_event(std::uint64_t seq, const Event& ev) {
                write_canonical(seq, ev, out);
                out.push_back('\n');
            }
        } sink{out};
        Command cmd;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            if (!jsonflat::parse_command(line, cmd)) {
                std::cerr << "bad command line: " << line << "\n";
                return 2;
            }
            book.apply(cmd, sink);
            if (out.size() > (1 << 20)) flush();
        }
    }
    flush();
    return 0;
}
