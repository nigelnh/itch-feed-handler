#pragma once

#include "itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace itch {

enum class DecodeError : std::uint8_t {
    None,
    EmptyPayload,
    IncorrectMessageLength,
    InvalidSide,
};

struct DecodeResult {
    std::optional<Message> message;
    DecodeError error{DecodeError::None};
    char type{'\0'};
    std::size_t expected_size{0};
    std::size_t actual_size{0};

    [[nodiscard]] bool ok() const noexcept {
        return error == DecodeError::None && message.has_value();
    }
};

[[nodiscard]] DecodeResult decode_message(std::span<const std::uint8_t> payload);
[[nodiscard]] std::optional<std::size_t> expected_message_size(char type) noexcept;
[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

}  // namespace itch
