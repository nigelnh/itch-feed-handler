#include "itch/order_book.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

int failure_count = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": CHECK failed: " << expression << '\n';
        ++failure_count;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

using itch::BookError;
using itch::MarketState;
using itch::Price;
using itch::Side;

constexpr Price p99_99 = 999'900;
constexpr Price p100_00 = 1'000'000;
constexpr Price p100_01 = 1'000'100;
constexpr Price p100_02 = 1'000'200;

const itch::LimitOrderBook& require_book(const MarketState& market, const std::string& symbol) {
    const auto* book = market.find_book(symbol);
    CHECK(book != nullptr);
    if (book == nullptr) {
        static const itch::LimitOrderBook empty_book;
        return empty_book;
    }
    return *book;
}

void test_add_creates_order_and_level() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);

    const auto order = market.find_order(1);
    const itch::Order expected{"AAPL", Side::Buy, p100_00, 100};
    CHECK(order.has_value());
    CHECK(order == expected);
    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p100_00) == 100);
    CHECK(market.check_invariants());
}

void test_orders_at_same_price_aggregate() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.add_order(2, "AAPL", Side::Buy, p100_00, 250) == BookError::None);

    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p100_00) == 350);
    CHECK(market.order_count() == 2);
    CHECK(market.check_invariants());
}

void test_execute_partial_and_full() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.execute_order(1, 40) == BookError::None);
    CHECK(market.find_order(1)->remaining_quantity == 60);
    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p100_00) == 60);

    CHECK(market.execute_order(1, 60) == BookError::None);
    CHECK(!market.find_order(1).has_value());
    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p100_00) == 0);
    CHECK(market.check_invariants());
}

void test_cancel_partial_and_full() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Sell, p100_01, 125) == BookError::None);
    CHECK(market.cancel_order(1, 25) == BookError::None);
    CHECK(market.find_order(1)->remaining_quantity == 100);
    CHECK(require_book(market, "AAPL").quantity_at(Side::Sell, p100_01) == 100);

    CHECK(market.cancel_order(1, 100) == BookError::None);
    CHECK(!market.find_order(1).has_value());
    CHECK(require_book(market, "AAPL").quantity_at(Side::Sell, p100_01) == 0);
    CHECK(market.check_invariants());
}

void test_delete_removes_remaining_quantity() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Sell, p100_01, 125) == BookError::None);
    CHECK(market.execute_order(1, 25) == BookError::None);
    CHECK(market.delete_order(1) == BookError::None);

    CHECK(!market.find_order(1).has_value());
    CHECK(require_book(market, "AAPL").quantity_at(Side::Sell, p100_01) == 0);
    CHECK(market.check_invariants());
}

void test_replace_changes_id_price_and_quantity() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.replace_order(1, 2, p99_99, 75) == BookError::None);

    CHECK(!market.find_order(1).has_value());
    const auto replacement = market.find_order(2);
    const itch::Order expected{"AAPL", Side::Buy, p99_99, 75};
    CHECK(replacement == expected);
    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p100_00) == 0);
    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p99_99) == 75);
    CHECK(market.check_invariants());
}

void test_symbols_are_isolated() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.add_order(2, "MSFT", Side::Buy, p100_00, 250) == BookError::None);

    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p100_00) == 100);
    CHECK(require_book(market, "MSFT").quantity_at(Side::Buy, p100_00) == 250);
    CHECK(market.symbol_count() == 2);
    CHECK(market.check_invariants());
}

void test_early_symbol_survives_symbol_storage_growth() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);

    for (std::uint64_t index = 0; index < 256; ++index) {
        CHECK(market.add_order(
                  10 + index,
                  "SYM" + std::to_string(index),
                  Side::Sell,
                  p100_01,
                  10)
              == BookError::None);
    }

    CHECK(market.execute_order(1, 40) == BookError::None);
    CHECK(market.replace_order(1, 1'000, p99_99, 55) == BookError::None);
    CHECK(market.find_order(1'000)->symbol == "AAPL");
    CHECK(require_book(market, "AAPL").quantity_at(Side::Buy, p99_99) == 55);
    CHECK(market.check_invariants());
}

void test_invalid_events_do_not_mutate_state() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.add_order(2, "AAPL", Side::Sell, p100_01, 100) == BookError::None);
    const auto before = market.snapshot();

    CHECK(market.add_order(1, "NEW", Side::Buy, p100_00, 50)
          == BookError::DuplicateOrderId);
    CHECK(market.snapshot() == before);
    CHECK(market.add_order(3, "NEW", Side::Buy, p100_00, 0)
          == BookError::InvalidQuantity);
    CHECK(market.snapshot() == before);
    CHECK(market.execute_order(99, 1) == BookError::UnknownOrderId);
    CHECK(market.snapshot() == before);
    CHECK(market.cancel_order(1, 0) == BookError::InvalidQuantity);
    CHECK(market.snapshot() == before);
    CHECK(market.delete_order(99) == BookError::UnknownOrderId);
    CHECK(market.snapshot() == before);
    CHECK(market.replace_order(99, 3, p100_02, 50) == BookError::UnknownOrderId);
    CHECK(market.snapshot() == before);
    CHECK(market.replace_order(1, 2, p100_02, 50) == BookError::DuplicateOrderId);
    CHECK(market.snapshot() == before);
    CHECK(market.replace_order(1, 3, p100_02, 0) == BookError::InvalidQuantity);
    CHECK(market.snapshot() == before);
    CHECK(market.check_invariants());
}

void test_excessive_reductions_do_not_mutate_state() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    const auto before = market.snapshot();

    CHECK(market.execute_order(1, 101) == BookError::QuantityExceedsRemaining);
    CHECK(market.snapshot() == before);
    CHECK(market.cancel_order(1, 101) == BookError::QuantityExceedsRemaining);
    CHECK(market.snapshot() == before);
    CHECK(market.find_order(1)->remaining_quantity == 100);
    CHECK(market.check_invariants());
}

bool snapshot_aggregates_match_orders(const itch::MarketSnapshot& snapshot) {
    using Key = std::pair<Side, Price>;
    std::map<std::string, std::map<Key, itch::AggregateQuantity>> expected;
    for (const auto& entry : snapshot.orders) {
        expected[entry.order.symbol][{entry.order.side, entry.order.price}]
            += entry.order.remaining_quantity;
    }

    for (const auto& book : snapshot.books) {
        std::map<Key, itch::AggregateQuantity> actual;
        for (const auto& level : book.bids) {
            actual[{Side::Buy, level.price}] = level.quantity;
        }
        for (const auto& level : book.asks) {
            actual[{Side::Sell, level.price}] = level.quantity;
        }
        if (actual != expected[book.symbol]) {
            return false;
        }
        expected.erase(book.symbol);
    }
    return expected.empty();
}

void test_aggregates_equal_sum_of_active_orders() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.add_order(2, "AAPL", Side::Buy, p100_00, 250) == BookError::None);
    CHECK(market.add_order(3, "AAPL", Side::Sell, p100_01, 80) == BookError::None);
    CHECK(market.add_order(4, "MSFT", Side::Sell, p100_01, 40) == BookError::None);
    CHECK(market.execute_order(2, 50) == BookError::None);
    CHECK(market.cancel_order(3, 10) == BookError::None);
    CHECK(snapshot_aggregates_match_orders(market.snapshot()));
    CHECK(market.check_invariants());
}

void apply_deterministic_sequence(MarketState& market) {
    CHECK(market.add_order(1, "AAPL", Side::Buy, p100_00, 100) == BookError::None);
    CHECK(market.add_order(2, "AAPL", Side::Buy, p99_99, 200) == BookError::None);
    CHECK(market.add_order(3, "AAPL", Side::Sell, p100_01, 150) == BookError::None);
    CHECK(market.execute_order(1, 25) == BookError::None);
    CHECK(market.cancel_order(3, 50) == BookError::None);
    CHECK(market.replace_order(2, 4, p100_00, 120) == BookError::None);
    CHECK(market.delete_order(1) == BookError::None);
}

void test_replay_is_deterministic() {
    MarketState first;
    MarketState second;
    apply_deterministic_sequence(first);
    apply_deterministic_sequence(second);

    CHECK(first.snapshot() == second.snapshot());
    CHECK(first.check_invariants());
    CHECK(second.check_invariants());
}

void test_reserve_does_not_change_results() {
    MarketState baseline;
    MarketState reserved;
    reserved.reserve_orders(1'000);

    apply_deterministic_sequence(baseline);
    apply_deterministic_sequence(reserved);

    CHECK(reserved.snapshot() == baseline.snapshot());
    CHECK(reserved.check_invariants());
}

void test_best_prices_and_signed_spread() {
    MarketState market;
    CHECK(market.add_order(1, "AAPL", Side::Buy, p99_99, 100) == BookError::None);
    auto& one_sided = require_book(market, "AAPL");
    CHECK(one_sided.best_bid() == p99_99);
    CHECK(!one_sided.best_ask().has_value());
    CHECK(!one_sided.spread().has_value());

    CHECK(market.add_order(2, "AAPL", Side::Buy, p100_00, 50) == BookError::None);
    CHECK(market.add_order(3, "AAPL", Side::Sell, p100_02, 75) == BookError::None);
    auto& normal = require_book(market, "AAPL");
    CHECK(normal.best_bid() == p100_00);
    CHECK(normal.best_ask() == p100_02);
    CHECK(normal.spread() == 200);

    CHECK(market.add_order(4, "AAPL", Side::Buy, p100_02, 10) == BookError::None);
    CHECK(market.add_order(5, "AAPL", Side::Sell, p100_00, 10) == BookError::None);
    auto& crossed = require_book(market, "AAPL");
    CHECK(crossed.spread() == -200);
    CHECK(market.check_invariants());
}

}  // namespace

int main() {
    using Test = std::pair<std::string, std::function<void()>>;
    const std::vector<Test> tests{
        {"add creates order and level", test_add_creates_order_and_level},
        {"same-price orders aggregate", test_orders_at_same_price_aggregate},
        {"execute partial and full", test_execute_partial_and_full},
        {"cancel partial and full", test_cancel_partial_and_full},
        {"delete removes remaining quantity", test_delete_removes_remaining_quantity},
        {"replace changes id, price, and quantity", test_replace_changes_id_price_and_quantity},
        {"symbols are isolated", test_symbols_are_isolated},
        {"early symbol survives storage growth", test_early_symbol_survives_symbol_storage_growth},
        {"invalid events do not mutate", test_invalid_events_do_not_mutate_state},
        {"excessive reductions do not mutate", test_excessive_reductions_do_not_mutate_state},
        {"aggregates equal active orders", test_aggregates_equal_sum_of_active_orders},
        {"replay is deterministic", test_replay_is_deterministic},
        {"reserving order capacity preserves results", test_reserve_does_not_change_results},
        {"best prices and signed spread", test_best_prices_and_signed_spread},
    };

    for (const auto& [name, test] : tests) {
        const int failures_before = failure_count;
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << "Unexpected exception in " << name << ": " << error.what() << '\n';
            ++failure_count;
        } catch (...) {
            std::cerr << "Unexpected non-standard exception in " << name << '\n';
            ++failure_count;
        }

        if (failure_count == failures_before) {
            std::cout << "[PASS] " << name << '\n';
        } else {
            std::cout << "[FAIL] " << name << '\n';
        }
    }

    std::cout << '\n' << tests.size() << " tests run, " << failure_count << " failure(s).\n";
    return failure_count == 0 ? 0 : 1;
}
