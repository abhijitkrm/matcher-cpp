// OrderBook — single-writer deterministic FIFO price-time book.
// Mirrors matcher-rust/src/book.rs.
#pragma once

#include <limits>

#include "detail/internals.hpp"
#include "types.hpp"

namespace matcher {

struct OrderInfo {
    OrderId order_id;
    Side side;
    Price price;
    Qty qty;
};

class OrderBook {
    using Level = detail::Level;
    using Chunk = detail::Chunk;

  public:
    explicit OrderBook(BookConfig cfg)
        : store_(cfg.max_orders), map_(cfg.max_orders),
          bids_(cfg.index, Side::Bid, cfg.price_min, cfg.price_max),
          asks_(cfg.index, Side::Ask, cfg.price_min, cfg.price_max), cfg_(cfg) {}

    /// Touch all order storage now (it is otherwise committed lazily as the
    /// book grows). Call before latency-sensitive use; costs ~128 B of
    /// resident memory per max_orders.
    void prefault() { store_.prefault(); }

    /// apply_batch pipelines prefetches only once this many orders rest: a
    /// cache-resident book gains nothing from look-ahead and pays for the
    /// extra probes. Tests set 0 to force the pipeline on.
    void set_prefetch_threshold(std::size_t live) { prefetch_min_live_ = live; }

    /// Apply one command; events emitted in order via sink.on_event(seq, ev).
    template <class Sink>
    void apply(const Command& cmd, Sink& sink) {
        switch (cmd.kind) {
            case Command::Kind::New:
                new_order(cmd, sink);
                break;
            case Command::Kind::Cancel:
                cancel(cmd.order_id, sink);
                break;
            case Command::Kind::Replace:
                replace(cmd.order_id, cmd.price, cmd.qty, sink);
                break;
        }
    }

    /// Apply a run of commands. Event stream is identical to calling apply()
    /// on each in order; the batch only lets the book look ahead and
    /// software-prefetch the memory upcoming commands will touch (map slot,
    /// then the order's chunk, then its price level), hiding DRAM latency
    /// behind the current command. Prefetches are hints: they read
    /// possibly-stale state and never write.
    template <class Sink>
    void apply_batch(const Command* cmds, std::size_t n, Sink& sink) {
        constexpr std::size_t D_MAP = 16, D_CHUNK = 8, D_LEVEL = 4;
        if (store_.live < prefetch_min_live_) {
            for (std::size_t i = 0; i < n; ++i) apply(cmds[i], sink);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (i + D_MAP < n) map_.prefetch(cmds[i + D_MAP].order_id);
            if (i + D_CHUNK < n) prefetch_chunk(cmds[i + D_CHUNK]);
            if (i + D_LEVEL < n) prefetch_level(cmds[i + D_LEVEL]);
            apply(cmds[i], sink);
        }
    }

    // ---- commands ----

    template <class Sink>
    void new_order(const Command& c, Sink& sink) {
        const OrderId oid = c.order_id;
        // SPEC §4.3 validation precedence.
        if (c.qty == 0) return reject(sink, oid, RejectReason::InvalidQty);
        if (c.otype == OType::Limit && !price_ok(c.price))
            return reject(sink, oid, RejectReason::InvalidPrice);
        if (map_.contains(oid))
            return reject(sink, oid, RejectReason::DuplicateOrderId);
        if (store_.live >= cfg_.max_orders)
            return reject(sink, oid, RejectReason::BookFull);
        if (c.otype == OType::Limit) {
            if (c.tif == Tif::PostOnly && would_cross(c.side, c.price))
                return reject(sink, oid, RejectReason::PostOnlyWouldCross);
            if (c.tif == Tif::Fok && fillable(c.side, c.price) < c.qty)
                return reject(sink, oid, RejectReason::FokCannotFill);
        }

        Qty remaining = c.qty;
        const bool bound = c.otype == OType::Limit;
        cross(c.side, bound, c.price, oid, remaining, sink);

        if (remaining == 0) {
            Event e; e.kind = Event::Kind::Closed; e.order_id = oid;
            e.reason = std::uint8_t(CloseReason::Filled);
            emit(sink, e);
        } else if (c.otype == OType::Limit && (c.tif == Tif::Gtc || c.tif == Tif::PostOnly)) {
            rest(oid, c.side, c.price, remaining, c.tif);
            Event e; e.kind = Event::Kind::Accepted; e.order_id = oid; e.leaves_qty = remaining;
            emit(sink, e);
        } else {
            Event e; e.kind = Event::Kind::Closed; e.order_id = oid;
            e.reason = std::uint8_t(CloseReason::Expired);
            emit(sink, e);
        }
    }

    template <class Sink>
    void cancel(OrderId oid, Sink& sink) {
        const std::uint32_t* ploc = map_.get(oid);
        if (!ploc) return reject(sink, oid, RejectReason::UnknownOrderId);
        const std::uint32_t loc = *ploc;
        const Chunk& ch = store_.chunk(loc);
        const Price price = ch.price;
        auto& own = own_index(ch.side);
        if (Level* lv = own.level_mut(price)) store_.unlink(*lv, loc);
        own.unlink_level(price);
        map_.remove(oid);
        Event e; e.kind = Event::Kind::Closed; e.order_id = oid;
        e.reason = std::uint8_t(CloseReason::Cancelled);
        emit(sink, e);
    }

    template <class Sink>
    void replace(OrderId oid, Price price, Qty qty, Sink& sink) {
        std::uint32_t* ploc = const_cast<std::uint32_t*>(map_.get(oid));
        if (!ploc) return reject(sink, oid, RejectReason::UnknownOrderId);
        const std::uint32_t loc = *ploc;
        if (qty == 0) return reject(sink, oid, RejectReason::InvalidQty);
        if (!price_ok(price)) return reject(sink, oid, RejectReason::InvalidPrice);
        Chunk& ch = store_.chunk(loc);
        detail::Slot& o = ch.s[detail::loc_slot(loc)];
        const Price old_price = ch.price;
        const Qty old_qty = o.qty;
        const Side side = ch.side;
        const Tif tif = ch.tif[detail::loc_slot(loc)];
        auto& own = own_index(side);

        if (price == old_price && qty <= old_qty) {
            if (Level* lv = own.level_mut(price)) lv->total -= old_qty - qty;
            o.qty = qty;
            Event e; e.kind = Event::Kind::Replaced; e.order_id = oid; e.price = price; e.qty = qty;
            emit(sink, e);
            return;
        }

        // Leave the book (losing time priority), re-cross at the new price,
        // rest any remainder at the back of the new level. The map entry is
        // kept across cross() so the id stays reserved; it is rewritten (or
        // removed) afterwards because cross() may move map entries.
        if (Level* lv = own.level_mut(old_price)) store_.unlink(*lv, loc);
        own.unlink_level(old_price);

        Qty remaining = qty;
        const std::uint64_t removes = map_.removes;
        cross(side, true, price, oid, remaining, sink);

        if (remaining == 0) {
            map_.remove(oid);
            Event e; e.kind = Event::Kind::Closed; e.order_id = oid;
            e.reason = std::uint8_t(CloseReason::Filled);
            emit(sink, e);
            return;
        }
        const std::uint32_t nloc = store_.push(*own.level_insert(price), side, price, oid, remaining, tif);
        if (map_.removes == removes) *ploc = nloc; // no entry moved: skip the re-probe
        else map_.insert(oid, nloc);
        Event e; e.kind = Event::Kind::Replaced; e.order_id = oid; e.price = price;
        e.qty = remaining;
        emit(sink, e);
    }

    // ---- matching core ----

    template <class Sink>
    void cross(Side side, bool has_bound, Price bound, OrderId taker, Qty& qty, Sink& sink) {
        detail::PriceIndex& opp = side == Side::Bid ? asks_ : bids_;
        for (;;) {
            Price bp;
            if (!opp.best_price(bp)) return;
            if (has_bound) {
                if (side == Side::Bid && bp > bound) return;
                if (side == Side::Ask && bp < bound) return;
            }
            Level* lv = opp.level_mut(bp);
            if (!lv) return;
            while (!lv->empty()) {
                const std::uint32_t loc = store_.front(*lv);
                Chunk& ch = store_.chunk(loc);
                detail::Slot& m = ch.s[detail::loc_slot(loc)];
                const OrderId mid = m.id;
                const Qty mqty = m.qty;
                const Qty q = qty < mqty ? qty : mqty;
                ++seq_;
                {
                    Event e; e.kind = Event::Kind::Trade; e.maker = mid; e.taker = taker;
                    e.price = bp; e.qty = q;
                    sink.on_event(seq_, e);
                }
                lv->total -= q;
                m.qty = mqty - q;
                qty -= q;
                if (mqty == q) {
                    // m.qty == 0: the slot is now dead.
                    --store_.live;
                    if (--ch.live == 0) store_.drop_chunk(*lv, detail::loc_chunk(loc));
                    map_.remove(mid);
                    ++seq_;
                    Event e; e.kind = Event::Kind::Closed; e.order_id = mid;
                    e.reason = std::uint8_t(CloseReason::Filled);
                    sink.on_event(seq_, e);
                }
                if (qty == 0) break;
            }
            if (lv->empty()) opp.unlink_level(bp);
            if (qty == 0) return;
        }
    }

    // ---- query API ----

    bool best_bid(Price& out) const { return bids_.best_price(out); }
    bool best_ask(Price& out) const { return asks_.best_price(out); }
    std::size_t order_count() const { return store_.live; }
    std::uint64_t seq() const { return seq_; }
    std::size_t level_count(Side s) { return own_index(s).len(); }
    std::vector<detail::LevelDepth> depth(Side s, std::size_t n) {
        return own_index(s).depth(n);
    }
    bool order(OrderId oid, OrderInfo& out) const {
        const std::uint32_t* ploc = map_.get(oid);
        if (!ploc) return false;
        const Chunk& ch = store_.chunk(*ploc);
        const detail::Slot& o = ch.s[detail::loc_slot(*ploc)];
        out = OrderInfo{o.id, ch.side, ch.price, o.qty};
        return true;
    }

    /// One live order, for snapshot serialization (spec/JOURNAL.md).
    struct RestingOrder {
        OrderId order_id;
        Side side;
        Price price;
        Qty qty;
        Tif tif;
    };

    /// All live orders in book order: bids best→worst then asks best→worst,
    /// FIFO within each level.
    std::vector<RestingOrder> resting_orders() {
        std::vector<RestingOrder> out;
        out.reserve(store_.live);
        for (Side s : {Side::Bid, Side::Ask}) {
            auto& idx = own_index(s);
            for (const auto& d : idx.depth(std::numeric_limits<std::size_t>::max())) {
                store_.for_each(*idx.level_mut(d.price), [&](std::uint32_t loc) {
                    const Chunk& ch = store_.chunk(loc);
                    const auto i = detail::loc_slot(loc);
                    out.push_back(RestingOrder{ch.s[i].id, s, ch.price, ch.s[i].qty, ch.tif[i]});
                });
            }
        }
        return out;
    }

    /// Rebuild a book from a snapshot: same config, explicit seq, resting
    /// orders replayed in snapshot order (bids then asks, FIFO per level).
    static OrderBook restore(BookConfig cfg, std::uint64_t seq,
                             const std::vector<RestingOrder>& orders) {
        OrderBook b(cfg);
        b.seq_ = seq;
        for (const auto& o : orders) {
            if (b.store_.live >= cfg.max_orders) break;
            b.rest(o.order_id, o.side, o.price, o.qty, o.tif);
        }
        return b;
    }

    const BookConfig& config() const { return cfg_; }

  private:
    detail::PriceIndex& own_index(Side s) { return s == Side::Bid ? bids_ : asks_; }
    const detail::PriceIndex& own_index(Side s) const {
        return s == Side::Bid ? bids_ : asks_;
    }

    void prefetch_chunk(const Command& c) const {
        if (c.kind == Command::Kind::New) {
            if (c.otype == OType::Limit) own_index(c.side).prefetch(c.price);
            return;
        }
        if (const std::uint32_t* p = map_.get(c.order_id))
            __builtin_prefetch(&store_.chunk(*p), 1);
    }

    void prefetch_level(const Command& c) const {
        if (c.kind == Command::Kind::New) {
            // Resting appends to the level's tail chunk.
            if (c.otype == OType::Limit)
                if (const Level* lv = own_index(c.side).peek(c.price))
                    if (lv->tail != detail::NIL) __builtin_prefetch(&store_.chunks[lv->tail], 1);
            return;
        }
        const std::uint32_t* p = map_.get(c.order_id);
        if (!p) return;
        const Chunk& ch = store_.chunk(*p);
        own_index(ch.side).prefetch(ch.price);
        if (c.kind == Command::Kind::Replace && c.price != ch.price)
            own_index(ch.side).prefetch(c.price);
    }

    bool price_ok(Price p) const {
        if (cfg_.index == IndexKind::Tree) return p > 0;
        return p >= cfg_.price_min && p <= cfg_.price_max;
    }

    bool would_cross(Side side, Price price) const {
        Price bp;
        if (side == Side::Bid) return asks_.best_price(bp) && price >= bp;
        return bids_.best_price(bp) && price <= bp;
    }

    Qty fillable(Side side, Price price) const {
        const Price lo = cfg_.index == IndexKind::Tree ? INT64_MIN / 2 : cfg_.price_min;
        const Price hi = cfg_.index == IndexKind::Tree ? INT64_MAX / 2 : cfg_.price_max;
        return side == Side::Bid ? asks_.sum_range(lo, price) : bids_.sum_range(price, hi);
    }

    void rest(OrderId oid, Side side, Price price, Qty qty, Tif tif) {
        map_.insert(oid, store_.push(*own_index(side).level_insert(price), side, price, oid, qty, tif));
    }

    template <class Sink>
    void emit(Sink& sink, const Event& e) {
        ++seq_;
        sink.on_event(seq_, e);
    }
    template <class Sink>
    void reject(Sink& sink, OrderId oid, RejectReason r) {
        Event e; e.kind = Event::Kind::Rejected; e.order_id = oid;
        e.reason = std::uint8_t(r);
        emit(sink, e);
    }

    detail::Store store_;
    detail::OrderMap map_;
    detail::PriceIndex bids_, asks_;
    std::uint64_t seq_ = 0;
    std::size_t prefetch_min_live_ = std::size_t(1) << 16;
    BookConfig cfg_;
};

} // namespace matcher
