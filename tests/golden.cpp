// Golden vector runner: every vectors/**\/*.cmd.jsonl through OrderBook,
// byte-compare canonical event stream to .evt.jsonl.
//   golden_test <vectors-dir>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <matcher/matcher.hpp>
#include <matcher/jsonflat.hpp>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace matcher;

static std::vector<std::string> run_vector(const fs::path& cmd_path, IndexKind kind) {
    std::ifstream in(cmd_path);
    std::string line;
    std::getline(in, line);
    auto hdr = jsonflat::parse_header(line);
    BookConfig cfg;
    cfg.price_min = hdr.pmin;
    cfg.price_max = hdr.pmax;
    cfg.max_orders = hdr.max_orders;
    cfg.index = kind;
    OrderBook book(cfg);
    LinesSink sink;
    Command cmd;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (!jsonflat::parse_command(line, cmd)) {
            std::cerr << "bad command line: " << line << "\n";
            std::exit(2);
        }
        book.apply(cmd, sink);
    }
    return sink.lines;
}

int main(int argc, char** argv) {
    fs::path dir = argc > 1 ? argv[1] : "../vectors";
    std::vector<fs::path> cmd_files;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().filename().string().ends_with(".cmd.jsonl"))
            cmd_files.push_back(e.path());
    }
    std::sort(cmd_files.begin(), cmd_files.end());
    if (cmd_files.empty()) {
        std::cerr << "no vectors found under " << dir << "\n";
        return 2;
    }

    int failures = 0, checked = 0;
    for (auto& cmd_path : cmd_files) {
        fs::path evt_path = cmd_path;
        evt_path.replace_filename(
            cmd_path.filename().string().substr(0, cmd_path.filename().string().size() - 10) +
            ".evt.jsonl");

        // read header
        std::ifstream in(cmd_path);
        std::string hline;
        std::getline(in, hline);
        auto hdr = jsonflat::parse_header(hline);
        in.close();

        IndexKind primary = hdr.index_raw == "tree" ? IndexKind::Tree : IndexKind::Ladder;
        std::vector<IndexKind> modes;
        if (hdr.index_raw == "both") modes = {IndexKind::Ladder, IndexKind::Tree};
        else modes = {primary};

        auto expected = run_vector(cmd_path, primary);
        for (auto k : modes) {
            auto got = run_vector(cmd_path, k);
            if (got != expected) {
                std::cerr << "FAIL " << cmd_path.filename() << " [mode " << int(k)
                          << "]: index divergence\n";
                ++failures;
            }
        }

        std::ifstream ein(evt_path);
        if (!ein) {
            std::cerr << "FAIL " << evt_path.filename() << ": missing evt file\n";
            ++failures;
            continue;
        }
        std::vector<std::string> exp;
        std::string l;
        std::getline(ein, l); // header
        while (std::getline(ein, l)) exp.push_back(l);
        ++checked;
        if (exp != expected) {
            std::cerr << "FAIL " << cmd_path.filename() << ": golden mismatch\n";
            auto n = std::max(exp.size(), expected.size());
            for (std::size_t i = 0; i < n; ++i) {
                std::string e = i < exp.size() ? exp[i] : "<none>";
                std::string a = i < expected.size() ? expected[i] : "<none>";
                if (e != a)
                    std::cerr << "  line " << i + 2 << ":\n    expected " << e
                              << "\n    actual   " << a << "\n";
            }
            ++failures;
        }
    }

    if (failures) {
        std::cerr << failures << " golden vector(s) failed\n";
        return 1;
    }
    std::cout << "golden: " << checked << " vectors passed\n";
    return 0;
}
