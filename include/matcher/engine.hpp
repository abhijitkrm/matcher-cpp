// Engine — thin multi-symbol router: one book per symbol.
#pragma once

#include <functional>
#include <unordered_map>

#include "book.hpp"

namespace matcher {

class Engine {
  public:
    explicit Engine(BookConfig default_cfg) : default_cfg_(default_cfg) {}

    void add_symbol(Symbol sym, BookConfig cfg) { books_.emplace(sym, OrderBook(cfg)); }

    OrderBook* book(Symbol sym) {
        auto it = books_.find(sym);
        return it == books_.end() ? nullptr : &it->second;
    }

    /// Route a command to sym's book (created with default config on first use).
    template <class S>
    void submit(Symbol sym, const Command& cmd, S& sink) {
        auto [it, _] = books_.try_emplace(sym, OrderBook(default_cfg_));
        it->second.apply(cmd, sink);
    }

    /// submit with symbol-tagged delivery: f(symbol, seq, event).
    void submit_tagged(Symbol sym, const Command& cmd,
                       const std::function<void(Symbol, std::uint64_t, const Event&)>& f) {
        auto [it, _] = books_.try_emplace(sym, OrderBook(default_cfg_));
        auto& book = it->second;
        struct Adaptor {
            Symbol sym;
            const std::function<void(Symbol, std::uint64_t, const Event&)>& f;
            void on_event(std::uint64_t seq, const Event& ev) { f(sym, seq, ev); }
        } ad{sym, f};
        book.apply(cmd, ad);
    }

  private:
    BookConfig default_cfg_;
    std::unordered_map<Symbol, OrderBook> books_;
};

} // namespace matcher
