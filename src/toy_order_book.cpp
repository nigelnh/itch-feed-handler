#include "itch/order_book.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

void print_level(const itch::PriceLevel& level) {
    std::cout << '$' << itch::format_price(level.price) << " -> " << level.quantity << '\n';
}

void print_state(const itch::MarketState& market) {
    const auto snapshot = market.snapshot();

    std::cout << "\nOrderId -> Order\n";
    if (snapshot.orders.empty()) {
        std::cout << "  (no active orders)\n";
    }
    for (const auto& entry : snapshot.orders) {
        const auto& order = entry.order;
        std::cout << "  " << entry.order_id << " -> " << order.symbol << ' '
                  << itch::to_string(order.side) << " $" << itch::format_price(order.price)
                  << " x " << order.remaining_quantity << '\n';
    }

    for (const auto& book_snapshot : snapshot.books) {
        std::cout << "\n" << book_snapshot.symbol << " aggregated L2 book\n";
        std::cout << "  BIDS (highest first)\n";
        if (book_snapshot.bids.empty()) {
            std::cout << "    (empty)\n";
        }
        for (const auto& level : book_snapshot.bids) {
            std::cout << "    ";
            print_level(level);
        }

        std::cout << "  ASKS (lowest first)\n";
        if (book_snapshot.asks.empty()) {
            std::cout << "    (empty)\n";
        }
        for (const auto& level : book_snapshot.asks) {
            std::cout << "    ";
            print_level(level);
        }

        const auto* book = market.find_book(book_snapshot.symbol);
        if (book != nullptr && book->best_bid() && book->best_ask()) {
            std::cout << "  Best bid: $" << itch::format_price(*book->best_bid())
                      << " | Best ask: $" << itch::format_price(*book->best_ask())
                      << " | Spread: $"
                      << itch::format_price(static_cast<itch::Price>(*book->spread())) << '\n';
        } else {
            std::cout << "  Best bid/ask/spread: unavailable until both sides exist\n";
        }
    }

    std::cout << "  Invariants: " << (market.check_invariants() ? "valid" : "BROKEN") << "\n\n";
}

template <typename Operation>
void apply_and_print(
    itch::MarketState& market,
    std::string_view description,
    Operation operation) {
    std::cout << "============================================================\n";
    std::cout << description << '\n';
    const auto result = operation();
    std::cout << "Result: " << itch::to_string(result) << '\n';
    print_state(market);
}

}  // namespace

int main() {
    using itch::Price;
    using itch::Side;

    constexpr Price p200_09 = 2'000'900;
    constexpr Price p200_10 = 2'001'000;
    constexpr Price p200_11 = 2'001'100;
    constexpr Price p200_12 = 2'001'200;
    constexpr Price p200_13 = 2'001'300;

    itch::MarketState market;

    std::cout << "Toy order-book reconstruction\n"
              << "Prices are integers scaled by 10,000: 2,001,000 means $200.1000.\n\n";

    apply_and_print(market, "ADD 101 AAPL BUY  $200.1000 x 200", [&] {
        return market.add_order(101, "AAPL", Side::Buy, p200_10, 200);
    });
    apply_and_print(market, "ADD 102 AAPL BUY  $200.1000 x 300", [&] {
        return market.add_order(102, "AAPL", Side::Buy, p200_10, 300);
    });
    apply_and_print(market, "ADD 103 AAPL BUY  $200.0900 x 850", [&] {
        return market.add_order(103, "AAPL", Side::Buy, p200_09, 850);
    });
    apply_and_print(market, "ADD 201 AAPL SELL $200.1100 x 200", [&] {
        return market.add_order(201, "AAPL", Side::Sell, p200_11, 200);
    });
    apply_and_print(market, "ADD 202 AAPL SELL $200.1100 x 120", [&] {
        return market.add_order(202, "AAPL", Side::Sell, p200_11, 120);
    });
    apply_and_print(market, "ADD 203 AAPL SELL $200.1200 x 740", [&] {
        return market.add_order(203, "AAPL", Side::Sell, p200_12, 740);
    });
    apply_and_print(market, "EXECUTE order 101 for 50 shares", [&] {
        return market.execute_order(101, 50);
    });
    apply_and_print(market, "CANCEL 100 shares from order 102", [&] {
        return market.cancel_order(102, 100);
    });
    apply_and_print(market, "DELETE order 103", [&] {
        return market.delete_order(103);
    });
    apply_and_print(
        market,
        "REPLACE order 202 with order 204 at $200.1300 x 90",
        [&] { return market.replace_order(202, 204, p200_13, 90); });
    apply_and_print(market, "EXECUTE the remaining 150 shares of order 101", [&] {
        return market.execute_order(101, 150);
    });

    std::cout << "Final state contains " << market.order_count() << " active orders across "
              << market.symbol_count() << " reconstructed symbol.\n";
    return market.check_invariants() ? 0 : 1;
}
