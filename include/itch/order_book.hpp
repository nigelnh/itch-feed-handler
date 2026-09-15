#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace itch {

using OrderId = std::uint64_t;
using Price = std::uint32_t;
using Quantity = std::uint32_t;
using AggregateQuantity = std::uint64_t;

inline constexpr Price kPriceScale = 10'000;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class BookError : std::uint8_t {
    None,
    DuplicateOrderId,
    UnknownOrderId,
    InvalidQuantity,
    QuantityExceedsRemaining,
    InconsistentState,
};

struct Order {
    std::string symbol;
    Side side;
    Price price;
    Quantity remaining_quantity;

    bool operator==(const Order&) const = default;
};

struct PriceLevel {
    Price price;
    AggregateQuantity quantity;

    bool operator==(const PriceLevel&) const = default;
};

struct OrderSnapshot {
    OrderId order_id;
    Order order;

    bool operator==(const OrderSnapshot&) const = default;
};

struct SymbolBookSnapshot {
    std::string symbol;
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;

    bool operator==(const SymbolBookSnapshot&) const = default;
};

struct MarketSnapshot {
    std::vector<OrderSnapshot> orders;
    std::vector<SymbolBookSnapshot> books;

    bool operator==(const MarketSnapshot&) const = default;
};

class LimitOrderBook {
public:
    [[nodiscard]] AggregateQuantity quantity_at(Side side, Price price) const noexcept;
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> spread() const noexcept;
    [[nodiscard]] std::vector<PriceLevel> bids() const;
    [[nodiscard]] std::vector<PriceLevel> asks() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    using BidLevels = std::map<Price, AggregateQuantity, std::greater<Price>>;
    using AskLevels = std::map<Price, AggregateQuantity>;

    [[nodiscard]] bool can_reduce(Side side, Price price, Quantity quantity) const noexcept;
    void add_quantity(Side side, Price price, Quantity quantity);
    void reduce_quantity(Side side, Price price, Quantity quantity);

    BidLevels bids_;
    AskLevels asks_;

    friend class MarketState;
};

class MarketState {
public:
    // Preallocates hash-table buckets without creating any orders. This is an
    // optional startup hint; correctness never depends on the estimate.
    void reserve_orders(std::size_t expected_order_count);

    [[nodiscard]] BookError add_order(
        OrderId order_id,
        std::string symbol,
        Side side,
        Price price,
        Quantity quantity);
    [[nodiscard]] BookError execute_order(OrderId order_id, Quantity quantity);
    [[nodiscard]] BookError cancel_order(OrderId order_id, Quantity quantity);
    [[nodiscard]] BookError delete_order(OrderId order_id);
    [[nodiscard]] BookError replace_order(
        OrderId old_order_id,
        OrderId new_order_id,
        Price new_price,
        Quantity new_quantity);

    [[nodiscard]] std::optional<Order> find_order(OrderId order_id) const;
    [[nodiscard]] const LimitOrderBook* find_book(std::string_view symbol) const;
    [[nodiscard]] std::size_t order_count() const noexcept;
    [[nodiscard]] std::size_t symbol_count() const noexcept;
    [[nodiscard]] MarketSnapshot snapshot() const;
    [[nodiscard]] bool check_invariants() const;

private:
    using SymbolId = std::size_t;

    struct StoredOrder {
        SymbolId symbol_id;
        Side side;
        Price price;
        Quantity remaining_quantity;
    };

    struct StoredBook {
        std::string symbol;
        LimitOrderBook book;
    };

    [[nodiscard]] BookError reduce_order(OrderId order_id, Quantity quantity);

    std::unordered_map<OrderId, StoredOrder> orders_;
    std::unordered_map<std::string, SymbolId> symbol_ids_;
    std::vector<StoredBook> books_;
};

[[nodiscard]] std::string format_price(Price price);
[[nodiscard]] std::string_view to_string(Side side) noexcept;
[[nodiscard]] std::string_view to_string(BookError error) noexcept;

}  // namespace itch
