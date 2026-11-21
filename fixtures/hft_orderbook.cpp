#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// 1) Packed Big-Endian wire message (ITCH / OUCH / SBE style)
#pragma pack(push, 1)
struct WireOrderMsg {
    uint16_t msg_type_be;    // 'A' = 0x0041 (big-endian)
    uint64_t timestamp_ns_be;// UTC epoch ns (big-endian)
    uint64_t order_id_be;    // big-endian order id
    int64_t price_e4_be;     // fixed-point 1e-4 price (big-endian)
    uint32_t qty_be;         // big-endian quantity
    char side;               // 'B' or 'S'
    char symbol[8];          // space-padded ticker
};
#pragma pack(pop)

// 2) Intentional false-sharing + padding layout struct for Cache-Line Analyzer
struct MatchingEngineStats {
    uint8_t venue_id;                  // +0 (1B) -> 7B padding hole!
    std::atomic<uint64_t> rx_packets;  // +8 (8B)  CL#0
    std::atomic<uint64_t> tx_orders;   // +16 (8B) CL#0 -> FALSE SHARING with rx_packets!
    uint32_t active_sessions;          // +24 (4B) -> 36B padding before alignas(64)
    alignas(64) std::atomic<uint64_t> fill_count; // +64 (8B) CL#1 (properly isolated)
};

// 3) Lock-free SPSC Ring Buffer
struct OrderEvent {
    uint64_t seq;
    int64_t price_e4;
    uint32_t qty;
    char side;
};

template <typename T, size_t N>
struct SpscRing {
    T slots[N];
    size_t capacity = N;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};

    bool push(const T& ev) {
        size_t t = tail.load(std::memory_order_relaxed);
        if (t - head.load(std::memory_order_acquire) >= N) return false;
        slots[t % N] = ev;
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        size_t h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire)) return false;
        out = slots[h % N];
        head.store(h + 1, std::memory_order_release);
        return true;
    }
};

// 4) L2 Order Book levels
struct PriceLevel {
    int64_t price; // fixed-point 1e-4 ($184.2500 -> 1842500)
    uint32_t qty;
    uint32_t orders;
};

struct OrderBook {
    std::vector<PriceLevel> bids; // descending price
    std::vector<PriceLevel> asks; // ascending price
};

static inline uint64_t to_be64(uint64_t v) { return __builtin_bswap64(v); }
static inline uint32_t to_be32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t to_be16(uint16_t v) { return __builtin_bswap16(v); }

__attribute__((always_inline))
static inline int64_t compute_mid_e4(int64_t bid, int64_t ask) {
    return (bid + ask) / 2;
}

int main() {
    // 2026-11-21 14:30:00.123456789 UTC in nanoseconds
    const uint64_t base_ts_ns = 1795271400123456789ULL;

    WireOrderMsg wire{};
    wire.msg_type_be = to_be16(0x0041);
    wire.timestamp_ns_be = to_be64(base_ts_ns);
    wire.order_id_be = to_be64(900001ULL);
    wire.price_e4_be = static_cast<int64_t>(to_be64(static_cast<uint64_t>(1842500))); // $184.2500
    wire.qty_be = to_be32(500);
    wire.side = 'B';
    std::memcpy(wire.symbol, "AAPL    ", 8);
    WireOrderMsg* wire_ptr = &wire;

    MatchingEngineStats stats{};
    stats.venue_id = 7;
    stats.rx_packets.store(1000);
    stats.tx_orders.store(42);
    stats.active_sessions = 4;
    stats.fill_count.store(12);
    MatchingEngineStats* stats_ptr = &stats;

    SpscRing<OrderEvent, 16> ring{};
    // Pre-advance head & tail so ring demonstrates wrap-around occupancy!
    for (int k = 0; k < 12; ++k) {
        ring.push(OrderEvent{static_cast<uint64_t>(k + 1), 1842000 + k * 50, 100u + k * 10u, 'B'});
    }
    OrderEvent popped{};
    for (int k = 0; k < 10; ++k) {
        ring.pop(popped);
    }
    for (int k = 0; k < 8; ++k) {
        ring.push(OrderEvent{static_cast<uint64_t>(13 + k), 1842500 + k * 25, 250u + k * 25u,
                             (k % 2 == 0) ? 'B' : 'S'});
    }

    OrderBook book;
    book.bids = {
        {1842500, 1200, 4},
        {1842400, 2400, 9},
        {1842300, 1850, 6},
        {1842100, 3100, 11},
        {1841900, 900, 3},
    };
    book.asks = {
        {1842600, 800, 3},
        {1842700, 1950, 7},
        {1842800, 2200, 5},
        {1843000, 3400, 12},
        {1843200, 1500, 4},
    };

    for (int tick = 0; tick < 6; ++tick) {
        stats.rx_packets.fetch_add(1);
        wire.timestamp_ns_be = to_be64(base_ts_ns + static_cast<uint64_t>(tick) * 450000ULL);
        wire.order_id_be = to_be64(900001ULL + static_cast<uint64_t>(tick));

        // On tick 3, intentionally cross the book (bid >= ask) to trigger Book L2 alert!
        if (tick == 3) {
            book.bids[0].price = 1842700;
        } else {
            book.bids[0].price = 1842500;
        }
        book.bids[0].qty += 50;
        int64_t mid = compute_mid_e4(book.bids[0].price, book.asks[0].price);

        ring.pop(popped);
        ring.push(OrderEvent{static_cast<uint64_t>(21 + tick), mid, 300u, 'B'});

        std::printf("tick=%d mid=%.4f best_bid=%.4f best_ask=%.4f ring_used=%zu wire=%p stats=%p\n",
                    tick,
                    static_cast<double>(mid) / 10000.0,
                    static_cast<double>(book.bids[0].price) / 10000.0,
                    static_cast<double>(book.asks[0].price) / 10000.0,
                    ring.tail.load() - ring.head.load(),
                    static_cast<void*>(wire_ptr),
                    static_cast<void*>(stats_ptr));
        std::fflush(stdout);
    }

    return 0;
}
