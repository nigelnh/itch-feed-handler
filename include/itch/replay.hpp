#pragma once

#include "itch/decoder.hpp"
#include "itch/framing.hpp"
#include "itch/messages.hpp"
#include "itch/mapped_file.hpp"
#include "itch/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace itch {

struct ReplayStatistics {
    std::uint64_t messages_processed{0};
    std::uint64_t unsupported_messages{0};
    std::uint64_t system_events{0};
    std::uint64_t stock_directories{0};
    std::uint64_t trading_actions{0};
    std::uint64_t orders_added{0};
    std::uint64_t executions{0};
    std::uint64_t cancels{0};
    std::uint64_t deletes{0};
    std::uint64_t replaces{0};
    std::uint64_t book_errors{0};
    std::size_t peak_active_orders{0};
};

class ReplayEngine {
public:
    [[nodiscard]] BookError apply(const Message& message);

    [[nodiscard]] const MarketState& market() const noexcept;
    [[nodiscard]] MarketState& market() noexcept;
    [[nodiscard]] const ReplayStatistics& statistics() const noexcept;
    [[nodiscard]] std::optional<SystemEvent> last_system_event() const;
    [[nodiscard]] const StockDirectory* find_directory(StockLocate locate) const;
    [[nodiscard]] const StockTradingAction* find_trading_action(std::string_view symbol) const;

private:
    MarketState market_;
    ReplayStatistics statistics_;
    std::optional<SystemEvent> last_system_event_;
    std::unordered_map<StockLocate, StockDirectory> directories_;
    std::unordered_map<std::string, StockTradingAction> trading_actions_;
};

enum class ReplayStatus : std::uint8_t {
    Complete,
    CompleteWithoutBinaryFileTerminator,
    FrameError,
    DecodeError,
    BookError,
};

struct ReplayResult {
    ReplayStatus status{ReplayStatus::Complete};
    std::uint64_t record_number{0};
    char message_type{'\0'};
    FrameStatus frame_status{FrameStatus::EndOfSession};
    DecodeError decode_error{DecodeError::None};
    BookError book_error{BookError::None};
    std::size_t expected_size{0};
    std::size_t actual_size{0};

    [[nodiscard]] bool ok() const noexcept {
        return status == ReplayStatus::Complete
            || status == ReplayStatus::CompleteWithoutBinaryFileTerminator;
    }
};

struct DecodeStreamResult {
    ReplayResult result;
    std::uint64_t messages_decoded{0};
    std::uint64_t unsupported_messages{0};
    std::uint64_t checksum{0};
};

[[nodiscard]] DecodeStreamResult decode_stream(std::istream& input);
[[nodiscard]] DecodeStreamResult decode_bytes(std::span<const std::uint8_t> bytes);
[[nodiscard]] ReplayResult replay_stream(std::istream& input, ReplayEngine& engine);
[[nodiscard]] ReplayResult replay_bytes(
    std::span<const std::uint8_t> bytes,
    ReplayEngine& engine);
[[nodiscard]] std::string_view to_string(ReplayStatus status) noexcept;

}  // namespace itch
