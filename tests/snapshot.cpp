// Snapshot/journal round-trip tests (spec/JOURNAL.md):
//   snap → restore → snap  = byte-identical
//   restore → continue     = byte-identical event stream vs uninterrupted run
//   cmd journal → replay   = identical event journal
#include <cstdint>
#include <fstream>
#include <iostream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <sstream>
#include <string>
#include <vector>

using namespace matcher;

using TaggedCmd = std::pair<Symbol, Command>;

static std::vector<TaggedCmd> load_engine_cmds(const char* dir) {
    std::ifstream in(std::string(dir) + "/engine/001_multisymbol.cmd.jsonl");
    std::vector<TaggedCmd> cmds;
    std::string line;
    std::getline(in, line); // header
    Command c;
    Symbol sym;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (!jsonflat::parse_command(line, c, &sym)) {
            std::cerr << "bad cmd: " << line << "\n";
            std::exit(2);
        }
        cmds.emplace_back(sym, c);
    }
    return cmds;
}

static std::string run_tagged(Engine& e, const std::vector<TaggedCmd>& cmds,
                              std::size_t lo, std::size_t hi) {
    std::string out;
    for (std::size_t i = lo; i < hi; ++i) {
        e.submit_tagged(cmds[i].first, cmds[i].second,
                        [&](Symbol s, std::uint64_t seq, const Event& ev) {
                            write_canonical_sym(seq, s, ev, out);
                            out.push_back('\n');
                        });
    }
    return out;
}

struct StrWriter {
    std::string& s;
    void operator()(const std::string& chunk) { s += chunk; }
};

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "vectors";
    auto cmds = load_engine_cmds(dir);
    BookConfig cfg;
    cfg.price_min = 0;
    cfg.price_max = 1'000'000;
    cfg.max_orders = 65'536;
    cfg.index = IndexKind::Ladder;
    const std::size_t split = cmds.size() / 2;

    int fails = 0;
    auto check = [&](bool ok, const char* name, const std::string& detail = "") {
        if (!ok) {
            ++fails;
            std::cerr << "FAIL " << name << "\n" << detail << "\n";
        }
    };

    // 1. Reference: uninterrupted run.
    Engine ref(cfg);
    std::string expected = run_tagged(ref, cmds, 0, cmds.size());

    // 2. Split run: snapshot at midpoint, restore, continue.
    Engine eng(cfg);
    std::string out = run_tagged(eng, cmds, 0, split);
    std::string snap;
    snapshot::write_engine(eng, snap);
    Engine eng2 = snapshot::restore_engine(snapshot::parse(snap));

    std::string snap2;
    snapshot::write_engine(eng2, snap2);
    check(snap == snap2, "re-snapshot byte-identical",
          "left:\n" + snap + "\nright:\n" + snap2);

    out += run_tagged(eng2, cmds, split, cmds.size());
    check(out == expected, "continuation byte-identical");

    // 3. Journal replay: journal commands + events, replay cmds, compare evts.
    std::string cmd_log, evt_log, scratch;
    Engine eng3(cfg);
    StrWriter cw{cmd_log}, ew{evt_log};
    for (auto& [sym, c] : cmds) {
        CmdJournal<StrWriter> j(cw, &sym);
        j.record(c);
        eng3.submit_tagged(sym, c, [&](Symbol s, std::uint64_t seq, const Event& ev) {
            journal_event(s, seq, ev, ew, scratch);
        });
    }
    Engine eng4(cfg);
    std::string replayed;
    std::istringstream jl(cmd_log);
    std::string line;
    Command c2;
    Symbol s2;
    while (std::getline(jl, line)) {
        if (line.empty()) continue;
        jsonflat::parse_command(line, c2, &s2);
        eng4.submit_tagged(s2, c2, [&](Symbol s, std::uint64_t seq, const Event& ev) {
            write_canonical_sym(seq, s, ev, replayed);
            replayed.push_back('\n');
        });
    }
    check(replayed == evt_log, "journal replay byte-identical");

    // 4. Empty engine snapshot round-trips.
    Engine empty(cfg);
    std::string esnap;
    snapshot::write_engine(empty, esnap);
    auto eparsed = snapshot::parse(esnap);
    check(eparsed.books.empty(), "empty snapshot has no books");

    // 5. Mid-fuzz stream: xorshift64* same as fuzzgen, duplicated locally.
    {
        BookConfig fcfg;
        fcfg.price_min = 0;
        fcfg.price_max = 1000;
        fcfg.max_orders = 4096;
        fcfg.index = IndexKind::Ladder;
        std::uint64_t rng = 0xC0FFEE;
        auto next = [&] {
            rng ^= rng >> 12;
            rng ^= rng << 25;
            rng ^= rng >> 27;
            return rng * 0x2545F4914F6CDD1Dull;
        };
        auto below = [&](std::uint64_t n) { return next() % n; };
        std::vector<TaggedCmd> fcmds;
        for (int i = 0; i < 4000; ++i) {
            Symbol sym = Symbol(below(6));
            OrderId id = below(256);
            Command c;
            switch (below(3)) {
                case 0: {
                    Command n;
                    n.kind = Command::Kind::New;
                    n.order_id = id;
                    n.side = below(2) ? Side::Ask : Side::Bid;
                    n.otype = OType::Limit;
                    n.price = Price(below(999)) + 1;
                    n.qty = below(200) + 1;
                    std::uint64_t t = below(4);
                    n.tif = t == 0 ? Tif::Ioc : t == 1 ? Tif::Fok
                            : t == 2 ? Tif::PostOnly : Tif::Gtc;
                    c = n;
                    break;
                }
                case 1: c = Command::cancel(id); break;
                default: c = Command::replace(id, Price(below(999)) + 1, below(200) + 1); break;
            }
            fcmds.emplace_back(sym, c);
        }
        Engine fref(fcfg);
        std::string fexp = run_tagged(fref, fcmds, 0, fcmds.size());
        Engine feng(fcfg);
        std::string fout = run_tagged(feng, fcmds, 0, 2000);
        std::string fsnap;
        snapshot::write_engine(feng, fsnap);
        Engine feng2 = snapshot::restore_engine(snapshot::parse(fsnap));
        fout += run_tagged(feng2, fcmds, 2000, fcmds.size());
        check(fout == fexp, "mid-fuzz restore byte-identical");
    }

    if (fails == 0) std::cout << "snapshot: all checks passed\n";
    return fails ? 1 : 0;
}
