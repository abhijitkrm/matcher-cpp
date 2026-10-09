// The ladder's cursor agrees with a sorted reference under random churn on a
// wide ladder, including whole summary words emptying and refilling.
#include <cstdio>
#include <iterator>
#include <set>

#include "matcher/book.hpp"

using namespace matcher;
using detail::LadderIndex;
using detail::NIL;

int main() {
    const Price span = 1 << 20;
    for (Side side : {Side::Bid, Side::Ask}) {
        LadderIndex lad(side, 0, span - 1);
        std::set<Price> set;
        std::uint64_t x = 0x9E3779B97F4A7C15ull;
        for (int step = 0; step < 200000; ++step) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            Price p;
            switch (x % 8) {
                case 0: p = 0; break;
                case 1: p = span - 1; break;
                case 2: p = Price((x >> 8) % std::uint64_t(span)); break;
                default: p = span / 2 - 6000 + Price((x >> 8) % 12000);
            }
            if (step % 1000 < 520 && !set.empty()) {
                Price q;
                if (!(x & 16)) {
                    q = side == Side::Bid ? *set.rbegin() : *set.begin();
                } else {
                    auto it = set.lower_bound(p);
                    q = it == set.end() ? *set.rbegin() : *it;
                }
                set.erase(q);
                lad.level_mut(q)->head = NIL;
                lad.unlink_level(q);
            } else if (set.insert(p).second) {
                lad.level_insert(p)->head = 0;
            }
            Price got = 0;
            bool ok = lad.best_price(got);
            bool want_ok = !set.empty();
            Price want = want_ok ? (side == Side::Bid ? *set.rbegin() : *set.begin()) : 0;
            if (ok != want_ok || got != want || lad.count != set.size()) {
                std::fprintf(stderr, "side %d step %d: best %lld/%d want %lld/%d\n", int(side), step,
                             (long long)got, ok, (long long)want, want_ok);
                return 1;
            }
        }
    }
    std::puts("ladder: ok");
    return 0;
}
