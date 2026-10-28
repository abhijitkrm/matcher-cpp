// Internal machinery: order pool, intrusive levels, order map, price index.
#pragma once

#include <map>
#include <vector>

#include "../types.hpp"

namespace matcher::detail {

inline constexpr std::uint32_t NIL = UINT32_MAX;

// ---- order store: per-level chunked FIFOs ---------------------------------

// Resting orders live inline in their level's queue: an unrolled linked list
// of 128-byte chunks (one M1 cache line), each holding up to 6 {id, qty}
// slots plus the level's price/side. Matching walks slots sequentially; a
// cancel touches one chunk. A dead slot has qty == 0 (live resting qty is
// always > 0). A chunk is released when its last live slot dies, so chunks
// in use never exceed live orders.
//
// An order's location is loc = chunk << 3 | slot — the OrderMap value.
inline constexpr std::uint32_t CHUNK_SLOTS = 6;

struct Slot {
    OrderId id;
    Qty qty;
};

struct alignas(128) Chunk {
    Price price;
    std::uint32_t prev, next;
    std::uint8_t head, tail; // [head, tail) may hold live slots
    std::uint8_t live;
    Side side;
    std::uint32_t pad_;
    Slot s[CHUNK_SLOTS];
    Tif tif[8];
};
static_assert(sizeof(Chunk) == 128);

inline std::uint32_t loc_chunk(std::uint32_t loc) { return loc >> 3; }
inline std::uint32_t loc_slot(std::uint32_t loc) { return loc & 7; }

struct Level {
    std::uint32_t head = NIL, tail = NIL; // chunk indices
    Qty total = 0;
    bool empty() const { return head == NIL; }
};

struct Store {
    std::vector<Chunk> chunks;
    std::uint32_t used = 0; // chunks[0, used) have been handed out
    std::uint32_t free_chunk = NIL;
    std::size_t live = 0;

    explicit Store(std::size_t max_orders) { chunks.reserve(max_orders); }

    // Commit and touch every chunk up front so no page fault lands on the
    // hot path (BENCH.md: no growth inside timed sections).
    void prefault() { chunks.resize(chunks.capacity()); }

    Chunk& chunk(std::uint32_t loc) { return chunks[loc_chunk(loc)]; }
    const Chunk& chunk(std::uint32_t loc) const { return chunks[loc_chunk(loc)]; }

    // Never fails: chunks in use <= live orders <= max_orders.
    [[gnu::always_inline]] std::uint32_t alloc_chunk(Side side, Price price) {
        std::uint32_t c;
        if (free_chunk != NIL) {
            c = free_chunk;
            free_chunk = chunks[c].next;
        } else {
            c = used++;
            if (c == chunks.size()) chunks.emplace_back();
        }
        Chunk& ch = chunks[c];
        ch.price = price;
        ch.prev = ch.next = NIL;
        ch.head = ch.tail = ch.live = 0;
        ch.side = side;
        return c;
    }

    [[gnu::always_inline]] void drop_chunk(Level& l, std::uint32_t c) {
        Chunk& ch = chunks[c];
        if (ch.prev != NIL) chunks[ch.prev].next = ch.next;
        else l.head = ch.next;
        if (ch.next != NIL) chunks[ch.next].prev = ch.prev;
        else l.tail = ch.prev;
        ch.next = free_chunk;
        free_chunk = c;
    }

    [[gnu::always_inline]] std::uint32_t push(Level& l, Side side, Price price, OrderId id, Qty qty, Tif tif) {
        std::uint32_t c = l.tail;
        if (c == NIL || chunks[c].tail == CHUNK_SLOTS) {
            const std::uint32_t n = alloc_chunk(side, price);
            chunks[n].prev = c;
            if (c != NIL) chunks[c].next = n;
            else l.head = n;
            l.tail = c = n;
        }
        Chunk& ch = chunks[c];
        const std::uint32_t slot = ch.tail++;
        ch.s[slot] = Slot{id, qty};
        ch.tif[slot] = tif;
        ++ch.live;
        l.total += qty;
        ++live;
        return c << 3 | slot;
    }

    // Remove a live order from its level.
    [[gnu::always_inline]] void unlink(Level& l, std::uint32_t loc) {
        Chunk& ch = chunk(loc);
        Slot& s = ch.s[loc_slot(loc)];
        l.total -= s.qty;
        s.qty = 0;
        --live;
        if (--ch.live == 0) drop_chunk(l, loc_chunk(loc));
    }

    // First live order of a non-empty level (advances the head chunk's
    // cursor past dead slots).
    std::uint32_t front(const Level& l) {
        Chunk& ch = chunks[l.head];
        while (ch.s[ch.head].qty == 0) ++ch.head;
        return l.head << 3 | ch.head;
    }

    // Visit live orders in FIFO order: f(loc).
    template <class F>
    void for_each(const Level& l, F&& f) const {
        for (std::uint32_t c = l.head; c != NIL; c = chunks[c].next) {
            const Chunk& ch = chunks[c];
            for (std::uint32_t i = ch.head; i < ch.tail; ++i)
                if (ch.s[i].qty) f(c << 3 | i);
        }
    }
};

// ---- order_id -> pool index (open-addressed) --------------------------------

// Linear probing over two arrays: a 1-byte tag per slot (0 = empty, else 7
// hash bits | 0x80) and the {key, val} entries. Absent-key probes — every
// New's duplicate check — touch only the dense tag array; a hit costs one
// tag byte plus one 16-byte entry.
struct OrderMap {
    struct Entry {
        OrderId key;
        std::uint32_t val;
    };
    std::vector<std::uint8_t> tags;
    std::vector<Entry> entries;
    std::size_t mask, len = 0;

    explicit OrderMap(std::size_t live_max) {
        std::size_t cap = 16;
        while (cap < live_max * 2) cap <<= 1;
        tags.assign(cap, 0);
        entries.assign(cap, Entry{0, 0});
        mask = cap - 1;
    }

    static std::uint64_t mix(std::uint64_t k) {
        std::uint64_t h = k * 0x9E3779B97F4A7C15ull;
        return h ^ (h >> 32);
    }
    // Slot from the low bits, tag from the top 7 (independent of the slot).
    static std::uint8_t tag_of(std::uint64_t h) { return std::uint8_t((h >> 57) | 0x80); }

    void prefetch(OrderId k) const {
        const std::size_t i = mix(k) & mask;
        __builtin_prefetch(&tags[i]);
        __builtin_prefetch(&entries[i]);
    }

    const std::uint32_t* get(OrderId k) const {
        const std::uint64_t h = mix(k);
        const std::uint8_t t = tag_of(h);
        std::size_t i = h & mask;
        while (std::uint8_t ti = tags[i]) {
            if (ti == t && entries[i].key == k) return &entries[i].val;
            i = (i + 1) & mask;
        }
        return nullptr;
    }

    bool contains(OrderId k) const { return get(k) != nullptr; }

    // Bumped by every remove(): a value pointer from get() stays valid until
    // the next remove (backward shift may relocate entries; insert of a new
    // key never moves existing ones).
    std::uint64_t removes = 0;

    [[gnu::always_inline]] void insert(OrderId k, std::uint32_t v) {
        const std::uint64_t h = mix(k);
        const std::uint8_t t = tag_of(h);
        std::size_t i = h & mask;
        while (std::uint8_t ti = tags[i]) {
            if (ti == t && entries[i].key == k) { entries[i].val = v; return; }
            i = (i + 1) & mask;
        }
        tags[i] = t;
        entries[i] = Entry{k, v};
        ++len;
    }

    [[gnu::always_inline]] void remove(OrderId k) {
        const std::uint64_t h = mix(k);
        const std::uint8_t t = tag_of(h);
        std::size_t i = h & mask;
        for (;;) {
            const std::uint8_t ti = tags[i];
            if (!ti) return;
            if (ti == t && entries[i].key == k) break;
            i = (i + 1) & mask;
        }
        tags[i] = 0;
        --len;
        ++removes;
        // Backward-shift deletion (Knuth 6.4R) — no tombstones. Scan forward
        // from the hole; move back any entry whose probe chain crosses it.
        // An entry that cannot move leaves the hole where it is — only the
        // scan advances.
        std::size_t hole = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (!tags[j]) return;
            std::size_t home = mix(entries[j].key) & mask;
            bool in_interval = hole < j ? (home > hole && home <= j)
                                        : (home > hole || home <= j);
            if (!in_interval) {
                tags[hole] = tags[j]; entries[hole] = entries[j];
                tags[j] = 0;
                hole = j;
            }
        }
    }
};

// ---- price index ------------------------------------------------------------

struct LevelDepth {
    Price price;
    Qty qty;
};

// Direct-indexed ladder + occupancy bitmap + top-of-book cursor. summary bit
// w is set iff bits[w] != 0, so rescan skips 4096 empty ticks per word.
struct LadderIndex {
    Price base;
    Side side;
    std::vector<Level> levels;
    std::vector<std::uint64_t> bits;
    std::vector<std::uint64_t> summary;
    std::uint32_t count = 0;
    std::uint32_t best = NIL;

    LadderIndex(Side s, Price pmin, Price pmax) : base(pmin), side(s) {
        std::size_t span = std::size_t(pmax - pmin) + 1;
        levels.assign(span, Level{});
        bits.assign((span + 63) / 64, 0);
        summary.assign((bits.size() + 63) / 64, 0);
    }

    std::size_t idx(Price p) const { return std::size_t(p - base); }

    void prefetch(Price p) const {
        auto i = idx(p);
        if (i < levels.size()) __builtin_prefetch(&levels[i]);
    }
    const Level* peek(Price p) const {
        auto i = idx(p);
        return i < levels.size() ? &levels[i] : nullptr;
    }

    bool best_price(Price& out) const {
        if (best == NIL) return false;
        out = base + best;
        return true;
    }

    [[gnu::always_inline]] Level* level_mut(Price p) {
        auto i = idx(p);
        if (i >= levels.size()) return nullptr;
        Level& l = levels[i];
        return l.empty() ? nullptr : &l;
    }

    [[gnu::always_inline]] Level* level_insert(Price p) {
        auto i = idx(p);
        Level& l = levels[i];
        if (l.empty()) {
            bits[i / 64] |= 1ull << (i % 64);
            summary[i / 4096] |= 1ull << ((i / 64) % 64);
            ++count;
            if (best == NIL || (side == Side::Bid && std::uint32_t(i) > best) ||
                (side == Side::Ask && std::uint32_t(i) < best)) {
                best = std::uint32_t(i);
            }
        }
        return &l;
    }

    [[gnu::always_inline]] void unlink_level(Price p) {
        auto i = idx(p);
        if (!levels[i].empty()) return;
        std::size_t w = i / 64;
        bits[w] &= ~(1ull << (i % 64));
        if (!bits[w]) summary[w / 64] &= ~(1ull << (w % 64));
        --count;
        if (count == 0) best = NIL;
        else if (std::uint32_t(i) == best) best = rescan(i);
    }

    // Next occupied index moving inward from `from` (inclusive): higher for
    // asks, lower for bids.
    std::uint32_t rescan(std::size_t from) const {
        std::size_t w = from / 64;
        if (side == Side::Ask) {
            if (std::uint64_t word = bits[w] & (~0ull << (from % 64)))
                return std::uint32_t(w * 64 + __builtin_ctzll(word));
            // next non-empty word above w, via the summary
            for (std::size_t s = w + 1; s < bits.size();) {
                std::size_t sw = s / 64;
                if (std::uint64_t sword = summary[sw] & (~0ull << (s % 64))) {
                    std::size_t nw = sw * 64 + __builtin_ctzll(sword);
                    return std::uint32_t(nw * 64 + __builtin_ctzll(bits[nw]));
                }
                s = sw * 64 + 64;
            }
            return NIL;
        }
        if (std::uint64_t word = bits[w] & (~0ull >> (63 - from % 64)))
            return std::uint32_t(w * 64 + 63 - __builtin_clzll(word));
        // next non-empty word below w, via the summary
        for (std::int64_t s = std::int64_t(w) - 1; s >= 0;) {
            std::size_t sw = std::size_t(s) / 64;
            if (std::uint64_t sword = summary[sw] & (~0ull >> (63 - std::size_t(s) % 64))) {
                std::size_t nw = sw * 64 + 63 - __builtin_clzll(sword);
                return std::uint32_t(nw * 64 + 63 - __builtin_clzll(bits[nw]));
            }
            s = std::int64_t(sw) * 64 - 1;
        }
        return NIL;
    }

    Qty sum_range(Price lo, Price hi) const {
        long loI = long(idx(lo)) < 0 ? 0 : long(idx(lo));
        long hiI = long(idx(hi)) > long(levels.size()) - 1 ? long(levels.size()) - 1 : long(idx(hi));
        if (loI > hiI) return 0;
        Qty sum = 0;
        for (std::size_t i = std::size_t(loI); i <= std::size_t(hiI);) {
            std::size_t w = i / 64;
            std::size_t hiBit = std::min(w * 64 + 63, std::size_t(hiI));
            std::uint64_t word = bits[w] & (~0ull << (i % 64)) & (~0ull >> (63 - hiBit % 64));
            while (word) {
                std::size_t b = __builtin_ctzll(word);
                sum += levels[w * 64 + b].total;
                word &= word - 1;
            }
            i = w * 64 + 64;
        }
        return sum;
    }

    std::size_t len() const { return count; }

    std::vector<LevelDepth> depth(std::size_t n) const {
        std::vector<LevelDepth> out;
        if (side == Side::Ask) {
            for (std::size_t w = 0; w < bits.size() && out.size() < n; ++w) {
                std::uint64_t word = bits[w];
                while (word && out.size() < n) {
                    std::size_t b = __builtin_ctzll(word);
                    std::size_t i = w * 64 + b;
                    out.push_back({base + Price(i), levels[i].total});
                    word &= word - 1;
                }
            }
        } else {
            for (std::size_t w = bits.size(); w-- > 0 && out.size() < n;) {
                std::uint64_t word = bits[w];
                while (word && out.size() < n) {
                    std::size_t b = 63 - __builtin_clzll(word);
                    std::size_t i = w * 64 + b;
                    out.push_back({base + Price(i), levels[i].total});
                    word &= ~(1ull << b);
                }
            }
        }
        return out;
    }
};

// std::map fallback for unbounded domains.
struct TreeIndex {
    Side side;
    std::map<Price, Level> map;

    explicit TreeIndex(Side s) : side(s) {}

    bool best_price(Price& out) const {
        if (map.empty()) return false;
        out = side == Side::Bid ? map.rbegin()->first : map.begin()->first;
        return true;
    }

    [[gnu::always_inline]] Level* level_mut(Price p) {
        auto it = map.find(p);
        if (it == map.end() || it->second.empty()) return nullptr;
        return &it->second;
    }

    [[gnu::always_inline]] Level* level_insert(Price p) { return &map[p]; }

    [[gnu::always_inline]] void unlink_level(Price p) {
        auto it = map.find(p);
        if (it != map.end() && it->second.empty()) map.erase(it);
    }

    Qty sum_range(Price lo, Price hi) const {
        Qty sum = 0;
        for (auto it = map.lower_bound(lo); it != map.end() && it->first <= hi; ++it)
            sum += it->second.total;
        return sum;
    }

    std::size_t len() const { return map.size(); }

    std::vector<LevelDepth> depth(std::size_t n) const {
        std::vector<LevelDepth> out;
        if (side == Side::Ask) {
            for (auto it = map.begin(); it != map.end() && out.size() < n; ++it)
                out.push_back({it->first, it->second.total});
        } else {
            for (auto it = map.rbegin(); it != map.rend() && out.size() < n; ++it)
                out.push_back({it->first, it->second.total});
        }
        return out;
    }
};


// Enum-dispatched union of the two indexes (mirrors Rust `enum PriceIndex`):
// branch-predictable, no virtuals, fully inlinable. Holds both impls — the
// unused one is ~48 bytes.
struct PriceIndex {
    IndexKind kind;
    LadderIndex lad;
    TreeIndex tree;

    PriceIndex(IndexKind k, Side side, Price pmin, Price pmax)
        : kind(k), lad(side, pmin, pmax), tree(side) {}

    void prefetch(Price p) const {
        if (kind == IndexKind::Ladder) lad.prefetch(p);
    }
    const Level* peek(Price p) const {
        return kind == IndexKind::Ladder ? lad.peek(p) : nullptr;
    }
    bool best_price(Price& out) const {
        return kind == IndexKind::Ladder ? lad.best_price(out) : tree.best_price(out);
    }
    [[gnu::always_inline]] Level* level_mut(Price p) {
        return kind == IndexKind::Ladder ? lad.level_mut(p) : tree.level_mut(p);
    }
    [[gnu::always_inline]] Level* level_insert(Price p) {
        return kind == IndexKind::Ladder ? lad.level_insert(p) : tree.level_insert(p);
    }
    [[gnu::always_inline]] void unlink_level(Price p) {
        kind == IndexKind::Ladder ? lad.unlink_level(p) : tree.unlink_level(p);
    }
    Qty sum_range(Price lo, Price hi) const {
        return kind == IndexKind::Ladder ? lad.sum_range(lo, hi) : tree.sum_range(lo, hi);
    }
    std::size_t len() const {
        return kind == IndexKind::Ladder ? lad.len() : tree.len();
    }
    std::vector<LevelDepth> depth(std::size_t n) const {
        return kind == IndexKind::Ladder ? lad.depth(n) : tree.depth(n);
    }
};

} // namespace matcher::detail
