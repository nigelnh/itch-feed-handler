#include "itch/decoder.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace itch {

namespace {

template <typename Integer>
Integer read_big_endian(std::span<const std::uint8_t> payload, std::size_t offset, std::size_t size) {
    Integer value{0};
    for (std::size_t index = 0; index < size; ++index) {
        value = static_cast<Integer>(
            (value << 8U) | static_cast<Integer>(payload[offset + index]));
    }
    return value;
}

MessageHeader decode_header(std::span<const std::uint8_t> payload) {
    return MessageHeader{
        read_big_endian<StockLocate>(payload, 1, 2),
        read_big_endian<TrackingNumber>(payload, 3, 2),
        read_big_endian<Timestamp>(payload, 5, 6),
    };
}

std::string read_alpha(
    std::span<const std::uint8_t> payload,
    std::size_t offset,
    std::size_t size) {
    std::size_t trimmed_size = size;
    while (trimmed_size > 0 && payload[offset + trimmed_size - 1] == ' ') {
        --trimmed_size;
    }

    // char may inspect any object's byte representation. The string constructor
    // copies exactly the trimmed ASCII bytes, so the result owns its storage and
    // does not depend on the input frame's lifetime.
    const auto* first = reinterpret_cast<const char*>(payload.data() + offset);
    return std::string{first, trimmed_size};
}

std::optional<Side> decode_side(std::uint8_t value) noexcept {
    if (value == static_cast<std::uint8_t>('B')) {
        return Side::Buy;
    }
    if (value == static_cast<std::uint8_t>('S')) {
        return Side::Sell;
    }
    return std::nullopt;
}

DecodeResult wrong_length(char type, std::size_t expected, std::size_t actual) {
    return DecodeResult{
        .message = std::nullopt,
        .error = DecodeError::IncorrectMessageLength,
        .type = type,
        .expected_size = expected,
        .actual_size = actual,
    };
}

DecodeResult invalid_side(char type, std::size_t size) {
    return DecodeResult{
        .message = std::nullopt,
        .error = DecodeError::InvalidSide,
        .type = type,
        .expected_size = size,
        .actual_size = size,
    };
}

DecodeResult success(Message message, char type, std::size_t size) {
    return DecodeResult{
        .message = std::move(message),
        .error = DecodeError::None,
        .type = type,
        .expected_size = size,
        .actual_size = size,
    };
}

}  // namespace

DecodeResult decode_message(std::span<const std::uint8_t> payload) {
    if (payload.empty()) {
        return DecodeResult{
            .message = std::nullopt,
            .error = DecodeError::EmptyPayload,
            .type = '\0',
            .expected_size = 0,
            .actual_size = 0,
        };
    }

    const char type = static_cast<char>(payload.front());
    const auto expected_size = expected_message_size(type);
    if (expected_size && payload.size() != *expected_size) {
        return wrong_length(type, *expected_size, payload.size());
    }

    if (!expected_size) {
        return success(UnsupportedMessage{type, payload.size()}, type, payload.size());
    }

    const MessageHeader header = decode_header(payload);
    switch (type) {
        case SystemEvent::kType:
            return success(SystemEvent{header, static_cast<char>(payload[11])}, type, payload.size());

        case StockDirectory::kType:
            return success(
                StockDirectory{
                    header,
                    read_alpha(payload, 11, 8),
                    static_cast<char>(payload[19]),
                    static_cast<char>(payload[20]),
                    read_big_endian<std::uint32_t>(payload, 21, 4),
                    static_cast<char>(payload[25]),
                    static_cast<char>(payload[26]),
                    read_alpha(payload, 27, 2),
                    static_cast<char>(payload[29]),
                    static_cast<char>(payload[30]),
                    static_cast<char>(payload[31]),
                    static_cast<char>(payload[32]),
                    static_cast<char>(payload[33]),
                    read_big_endian<std::uint32_t>(payload, 34, 4),
                    static_cast<char>(payload[38]),
                },
                type,
                payload.size());

        case StockTradingAction::kType:
            return success(
                StockTradingAction{
                    header,
                    read_alpha(payload, 11, 8),
                    static_cast<char>(payload[19]),
                    static_cast<char>(payload[20]),
                    read_alpha(payload, 21, 4),
                },
                type,
                payload.size());

        case AddOrder::kType: {
            const auto side = decode_side(payload[19]);
            if (!side) {
                return invalid_side(type, payload.size());
            }
            return success(
                AddOrder{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                    *side,
                    read_big_endian<Quantity>(payload, 20, 4),
                    read_alpha(payload, 24, 8),
                    read_big_endian<Price>(payload, 32, 4),
                },
                type,
                payload.size());
        }

        case AddOrderWithMpid::kType: {
            const auto side = decode_side(payload[19]);
            if (!side) {
                return invalid_side(type, payload.size());
            }
            return success(
                AddOrderWithMpid{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                    *side,
                    read_big_endian<Quantity>(payload, 20, 4),
                    read_alpha(payload, 24, 8),
                    read_big_endian<Price>(payload, 32, 4),
                    read_alpha(payload, 36, 4),
                },
                type,
                payload.size());
        }

        case OrderExecuted::kType:
            return success(
                OrderExecuted{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                    read_big_endian<Quantity>(payload, 19, 4),
                    read_big_endian<MatchNumber>(payload, 23, 8),
                },
                type,
                payload.size());

        case OrderExecutedWithPrice::kType:
            return success(
                OrderExecutedWithPrice{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                    read_big_endian<Quantity>(payload, 19, 4),
                    read_big_endian<MatchNumber>(payload, 23, 8),
                    static_cast<char>(payload[31]),
                    read_big_endian<Price>(payload, 32, 4),
                },
                type,
                payload.size());

        case OrderCancel::kType:
            return success(
                OrderCancel{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                    read_big_endian<Quantity>(payload, 19, 4),
                },
                type,
                payload.size());

        case OrderDelete::kType:
            return success(
                OrderDelete{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                },
                type,
                payload.size());

        case OrderReplace::kType:
            return success(
                OrderReplace{
                    header,
                    read_big_endian<OrderId>(payload, 11, 8),
                    read_big_endian<OrderId>(payload, 19, 8),
                    read_big_endian<Quantity>(payload, 27, 4),
                    read_big_endian<Price>(payload, 31, 4),
                },
                type,
                payload.size());

        default:
            break;
    }

    return success(UnsupportedMessage{type, payload.size()}, type, payload.size());
}

std::optional<std::size_t> expected_message_size(char type) noexcept {
    switch (type) {
        case SystemEvent::kType:
            return SystemEvent::kSize;
        case StockDirectory::kType:
            return StockDirectory::kSize;
        case StockTradingAction::kType:
            return StockTradingAction::kSize;
        case AddOrder::kType:
            return AddOrder::kSize;
        case AddOrderWithMpid::kType:
            return AddOrderWithMpid::kSize;
        case OrderExecuted::kType:
            return OrderExecuted::kSize;
        case OrderExecutedWithPrice::kType:
            return OrderExecutedWithPrice::kSize;
        case OrderCancel::kType:
            return OrderCancel::kSize;
        case OrderDelete::kType:
            return OrderDelete::kSize;
        case OrderReplace::kType:
            return OrderReplace::kSize;
        default:
            return std::nullopt;
    }
}

std::string_view to_string(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::None:
            return "no error";
        case DecodeError::EmptyPayload:
            return "empty ITCH payload";
        case DecodeError::IncorrectMessageLength:
            return "incorrect message length";
        case DecodeError::InvalidSide:
            return "invalid buy/sell indicator";
    }
    return "unknown decode error";
}

char message_type(const Message& message) noexcept {
    if (const auto* value = std::get_if<UnsupportedMessage>(&message)) {
        return value->type;
    }
    if (std::holds_alternative<SystemEvent>(message)) {
        return SystemEvent::kType;
    }
    if (std::holds_alternative<StockDirectory>(message)) {
        return StockDirectory::kType;
    }
    if (std::holds_alternative<StockTradingAction>(message)) {
        return StockTradingAction::kType;
    }
    if (std::holds_alternative<AddOrder>(message)) {
        return AddOrder::kType;
    }
    if (std::holds_alternative<AddOrderWithMpid>(message)) {
        return AddOrderWithMpid::kType;
    }
    if (std::holds_alternative<OrderExecuted>(message)) {
        return OrderExecuted::kType;
    }
    if (std::holds_alternative<OrderExecutedWithPrice>(message)) {
        return OrderExecutedWithPrice::kType;
    }
    if (std::holds_alternative<OrderCancel>(message)) {
        return OrderCancel::kType;
    }
    if (std::holds_alternative<OrderDelete>(message)) {
        return OrderDelete::kType;
    }
    return OrderReplace::kType;
}

}  // namespace itch
