#pragma once

#include "itch/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace itch {

using StockLocate = std::uint16_t;
using TrackingNumber = std::uint16_t;
using Timestamp = std::uint64_t;
using MatchNumber = std::uint64_t;

struct MessageHeader {
    StockLocate stock_locate;
    TrackingNumber tracking_number;
    Timestamp timestamp;

    bool operator==(const MessageHeader&) const = default;
};

struct UnsupportedMessage {
    char type;
    std::size_t payload_size;

    bool operator==(const UnsupportedMessage&) const = default;
};

struct SystemEvent {
    static constexpr char kType = 'S';
    static constexpr std::size_t kSize = 12;

    MessageHeader header;
    char event_code;

    bool operator==(const SystemEvent&) const = default;
};

struct StockDirectory {
    static constexpr char kType = 'R';
    static constexpr std::size_t kSize = 39;

    MessageHeader header;
    std::string stock;
    char market_category;
    char financial_status_indicator;
    std::uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    std::string issue_sub_type;
    char authenticity;
    char short_sale_threshold_indicator;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    std::uint32_t etp_leverage_factor;
    char inverse_indicator;

    bool operator==(const StockDirectory&) const = default;
};

struct StockTradingAction {
    static constexpr char kType = 'H';
    static constexpr std::size_t kSize = 25;

    MessageHeader header;
    std::string stock;
    char trading_state;
    char reserved;
    std::string reason;

    bool operator==(const StockTradingAction&) const = default;
};

struct AddOrder {
    static constexpr char kType = 'A';
    static constexpr std::size_t kSize = 36;

    MessageHeader header;
    OrderId order_reference_number;
    Side side;
    Quantity shares;
    std::string stock;
    Price price;

    bool operator==(const AddOrder&) const = default;
};

struct AddOrderWithMpid {
    static constexpr char kType = 'F';
    static constexpr std::size_t kSize = 40;

    MessageHeader header;
    OrderId order_reference_number;
    Side side;
    Quantity shares;
    std::string stock;
    Price price;
    std::string attribution;

    bool operator==(const AddOrderWithMpid&) const = default;
};

struct OrderExecuted {
    static constexpr char kType = 'E';
    static constexpr std::size_t kSize = 31;

    MessageHeader header;
    OrderId order_reference_number;
    Quantity executed_shares;
    MatchNumber match_number;

    bool operator==(const OrderExecuted&) const = default;
};

struct OrderExecutedWithPrice {
    static constexpr char kType = 'C';
    static constexpr std::size_t kSize = 36;

    MessageHeader header;
    OrderId order_reference_number;
    Quantity executed_shares;
    MatchNumber match_number;
    char printable;
    Price execution_price;

    bool operator==(const OrderExecutedWithPrice&) const = default;
};

struct OrderCancel {
    static constexpr char kType = 'X';
    static constexpr std::size_t kSize = 23;

    MessageHeader header;
    OrderId order_reference_number;
    Quantity canceled_shares;

    bool operator==(const OrderCancel&) const = default;
};

struct OrderDelete {
    static constexpr char kType = 'D';
    static constexpr std::size_t kSize = 19;

    MessageHeader header;
    OrderId order_reference_number;

    bool operator==(const OrderDelete&) const = default;
};

struct OrderReplace {
    static constexpr char kType = 'U';
    static constexpr std::size_t kSize = 35;

    MessageHeader header;
    OrderId original_order_reference_number;
    OrderId new_order_reference_number;
    Quantity shares;
    Price price;

    bool operator==(const OrderReplace&) const = default;
};

using Message = std::variant<
    UnsupportedMessage,
    SystemEvent,
    StockDirectory,
    StockTradingAction,
    AddOrder,
    AddOrderWithMpid,
    OrderExecuted,
    OrderExecutedWithPrice,
    OrderCancel,
    OrderDelete,
    OrderReplace>;

[[nodiscard]] char message_type(const Message& message) noexcept;

}  // namespace itch
