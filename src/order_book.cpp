#include "itch/order_book.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace itch {

namespace {

template <typename Levels>
AggregateQuantity quantity_at_level(const Levels& levels, Price price) noexcept {
    const auto level = levels.find(price);
    return level == levels.end() ? 0 : level->second;
}

template <typename Levels>
void add_to_level(Levels& levels, Price price, Quantity quantity) {
    levels[price] += static_cast<AggregateQuantity>(quantity);
}

template <typename Levels>
void reduce_level(Levels& levels, Price price, Quantity quantity) {
    auto level = levels.find(price);
    const auto amount = static_cast<AggregateQuantity>(quantity);
    level->second -= amount;
    if (level->second == 0) {
        levels.erase(level);
    }
}

template <typename Levels>
std::vector<PriceLevel> copy_levels(const Levels& levels) {
    std::vector<PriceLevel> result;
    result.reserve(levels.size());
    for (const auto& [price, quantity] : levels) {
        result.push_back(PriceLevel{price, quantity});
    }
    return result;
}

}  // namespace

void MarketState::reserve_orders(std::size_t expected_order_count) {
    orders_.reserve(expected_order_count);
}

AggregateQuantity LimitOrderBook::quantity_at(Side side, Price price) const noexcept {
    return side == Side::Buy ? quantity_at_level(bids_, price)
                             : quantity_at_level(asks_, price);
}

std::optional<Price> LimitOrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> LimitOrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<std::int64_t> LimitOrderBook::spread() const noexcept {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*ask) - static_cast<std::int64_t>(*bid);
}

std::vector<PriceLevel> LimitOrderBook::bids() const {
    return copy_levels(bids_);
}

std::vector<PriceLevel> LimitOrderBook::asks() const {
    return copy_levels(asks_);
}

bool LimitOrderBook::empty() const noexcept {
    return bids_.empty() && asks_.empty();
}

void LimitOrderBook::add_quantity(Side side, Price price, Quantity quantity) {
    if (side == Side::Buy) {
        add_to_level(bids_, price, quantity);
    } else {
        add_to_level(asks_, price, quantity);
    }
}

bool LimitOrderBook::can_reduce(Side side, Price price, Quantity quantity) const noexcept {
    return quantity_at(side, price) >= static_cast<AggregateQuantity>(quantity);
}

void LimitOrderBook::reduce_quantity(Side side, Price price, Quantity quantity) {
    if (side == Side::Buy) {
        reduce_level(bids_, price, quantity);
    } else {
        reduce_level(asks_, price, quantity);
    }
}

BookError MarketState::add_order(
    OrderId order_id,
    std::string symbol,
    Side side,
    Price price,
    Quantity quantity) {
    if (quantity == 0) {
        return BookError::InvalidQuantity;
    }
    if (orders_.contains(order_id)) {
        return BookError::DuplicateOrderId;
    }

    SymbolId symbol_id = 0;
    const auto known_symbol = symbol_ids_.find(symbol);
    if (known_symbol == symbol_ids_.end()) {
        symbol_id = books_.size();
        books_.push_back(StoredBook{std::move(symbol), LimitOrderBook{}});
        symbol_ids_.emplace(books_.back().symbol, symbol_id);
    } else {
        symbol_id = known_symbol->second;
    }

    auto [order, inserted] = orders_.emplace(
        order_id,
        StoredOrder{symbol_id, side, price, quantity});
    if (!inserted) {
        return BookError::DuplicateOrderId;
    }

    books_[order->second.symbol_id].book.add_quantity(side, price, quantity);
    return BookError::None;
}

BookError MarketState::execute_order(OrderId order_id, Quantity quantity) {
    return reduce_order(order_id, quantity);
}

BookError MarketState::cancel_order(OrderId order_id, Quantity quantity) {
    return reduce_order(order_id, quantity);
}

BookError MarketState::delete_order(OrderId order_id) {
    const auto order = orders_.find(order_id);
    if (order == orders_.end()) {
        return BookError::UnknownOrderId;
    }

    const StoredOrder current = order->second;
    if (current.symbol_id >= books_.size()) {
        return BookError::InconsistentState;
    }
    auto& book = books_[current.symbol_id].book;
    if (!book.can_reduce(current.side, current.price, current.remaining_quantity)) {
        return BookError::InconsistentState;
    }

    book.reduce_quantity(current.side, current.price, current.remaining_quantity);
    orders_.erase(order);
    return BookError::None;
}

BookError MarketState::replace_order(
    OrderId old_order_id,
    OrderId new_order_id,
    Price new_price,
    Quantity new_quantity) {
    if (new_quantity == 0) {
        return BookError::InvalidQuantity;
    }

    const auto old_order = orders_.find(old_order_id);
    if (old_order == orders_.end()) {
        return BookError::UnknownOrderId;
    }
    if (orders_.contains(new_order_id)) {
        return BookError::DuplicateOrderId;
    }

    const StoredOrder previous = old_order->second;
    if (previous.symbol_id >= books_.size()) {
        return BookError::InconsistentState;
    }
    auto& book = books_[previous.symbol_id].book;
    if (!book.can_reduce(previous.side, previous.price, previous.remaining_quantity)) {
        return BookError::InconsistentState;
    }

    book.reduce_quantity(previous.side, previous.price, previous.remaining_quantity);
    book.add_quantity(previous.side, new_price, new_quantity);
    orders_.erase(old_order);
    orders_.emplace(
        new_order_id,
        StoredOrder{previous.symbol_id, previous.side, new_price, new_quantity});
    return BookError::None;
}

std::optional<Order> MarketState::find_order(OrderId order_id) const {
    const auto order = orders_.find(order_id);
    if (order == orders_.end()) {
        return std::nullopt;
    }
    const StoredOrder& stored = order->second;
    if (stored.symbol_id >= books_.size()) {
        return std::nullopt;
    }
    return Order{
        books_[stored.symbol_id].symbol,
        stored.side,
        stored.price,
        stored.remaining_quantity,
    };
}

const LimitOrderBook* MarketState::find_book(std::string_view symbol) const {
    const auto symbol_id = symbol_ids_.find(std::string{symbol});
    if (symbol_id == symbol_ids_.end() || symbol_id->second >= books_.size()) {
        return nullptr;
    }
    return &books_[symbol_id->second].book;
}

std::size_t MarketState::order_count() const noexcept {
    return orders_.size();
}

std::size_t MarketState::symbol_count() const noexcept {
    return books_.size();
}

MarketSnapshot MarketState::snapshot() const {
    MarketSnapshot result;
    result.orders.reserve(orders_.size());
    for (const auto& [order_id, order] : orders_) {
        if (order.symbol_id >= books_.size()) {
            continue;
        }
        result.orders.push_back(OrderSnapshot{
            order_id,
            Order{
                books_[order.symbol_id].symbol,
                order.side,
                order.price,
                order.remaining_quantity,
            },
        });
    }
    std::ranges::sort(result.orders, {}, &OrderSnapshot::order_id);

    result.books.reserve(books_.size());
    for (const auto& stored_book : books_) {
        result.books.push_back(SymbolBookSnapshot{
            stored_book.symbol,
            stored_book.book.bids(),
            stored_book.book.asks(),
        });
    }
    std::ranges::sort(result.books, {}, &SymbolBookSnapshot::symbol);
    return result;
}

bool MarketState::check_invariants() const {
    using ExpectedBids = std::map<Price, AggregateQuantity, std::greater<Price>>;
    using ExpectedAsks = std::map<Price, AggregateQuantity>;

    std::vector<ExpectedBids> expected_bids(books_.size());
    std::vector<ExpectedAsks> expected_asks(books_.size());

    if (symbol_ids_.size() != books_.size()) {
        return false;
    }
    for (SymbolId symbol_id = 0; symbol_id < books_.size(); ++symbol_id) {
        const auto symbol = symbol_ids_.find(books_[symbol_id].symbol);
        if (symbol == symbol_ids_.end() || symbol->second != symbol_id) {
            return false;
        }
    }

    for (const auto& [order_id, order] : orders_) {
        static_cast<void>(order_id);
        if (order.symbol_id >= books_.size() || order.remaining_quantity == 0) {
            return false;
        }
        if (order.side == Side::Buy) {
            expected_bids[order.symbol_id][order.price] += order.remaining_quantity;
        } else {
            expected_asks[order.symbol_id][order.price] += order.remaining_quantity;
        }
    }

    for (SymbolId symbol_id = 0; symbol_id < books_.size(); ++symbol_id) {
        const auto& book = books_[symbol_id].book;
        if (book.bids_ != expected_bids[symbol_id]
            || book.asks_ != expected_asks[symbol_id]) {
            return false;
        }
    }

    return true;
}

BookError MarketState::reduce_order(OrderId order_id, Quantity quantity) {
    if (quantity == 0) {
        return BookError::InvalidQuantity;
    }

    const auto order = orders_.find(order_id);
    if (order == orders_.end()) {
        return BookError::UnknownOrderId;
    }
    if (quantity > order->second.remaining_quantity) {
        return BookError::QuantityExceedsRemaining;
    }

    const StoredOrder current = order->second;
    if (current.symbol_id >= books_.size()) {
        return BookError::InconsistentState;
    }
    auto& book = books_[current.symbol_id].book;
    if (!book.can_reduce(current.side, current.price, quantity)) {
        return BookError::InconsistentState;
    }

    book.reduce_quantity(current.side, current.price, quantity);
    if (quantity == current.remaining_quantity) {
        orders_.erase(order);
    } else {
        order->second.remaining_quantity -= quantity;
    }
    return BookError::None;
}

std::string format_price(Price price) {
    std::ostringstream output;
    output << (price / kPriceScale) << '.' << std::setfill('0') << std::setw(4)
           << (price % kPriceScale);
    return output.str();
}

std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "BUY";
        case Side::Sell:
            return "SELL";
    }
    return "UNKNOWN";
}

std::string_view to_string(BookError error) noexcept {
    switch (error) {
        case BookError::None:
            return "OK";
        case BookError::DuplicateOrderId:
            return "duplicate order id";
        case BookError::UnknownOrderId:
            return "unknown order id";
        case BookError::InvalidQuantity:
            return "quantity must be greater than zero";
        case BookError::QuantityExceedsRemaining:
            return "quantity exceeds remaining shares";
        case BookError::InconsistentState:
            return "internal order/book state is inconsistent";
    }
    return "unknown error";
}

}  // namespace itch
