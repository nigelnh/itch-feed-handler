#include "itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class SyntheticWriter {
public:
    explicit SyntheticWriter(std::ostream& output) : output_(output) {}

    void write_system_event(char event_code) {
        auto message = header(itch::SystemEvent::kType, 0);
        append_byte(message, event_code);
        write_frame(message);
    }

    void write_directory(itch::StockLocate locate, const std::string& symbol) {
        auto message = header(itch::StockDirectory::kType, locate);
        append_alpha(message, symbol, 8);
        append_byte(message, 'Q');
        append_byte(message, 'N');
        append_big_endian(message, 100, 4);
        append_byte(message, 'N');
        append_byte(message, 'C');
        append_alpha(message, "", 2);
        append_byte(message, 'P');
        append_byte(message, 'N');
        append_byte(message, 'N');
        append_byte(message, '1');
        append_byte(message, 'N');
        append_big_endian(message, 1, 4);
        append_byte(message, 'N');
        write_frame(message);
    }

    void write_trading_action(itch::StockLocate locate, const std::string& symbol) {
        auto message = header(itch::StockTradingAction::kType, locate);
        append_alpha(message, symbol, 8);
        append_byte(message, 'T');
        append_byte(message, ' ');
        append_alpha(message, "", 4);
        write_frame(message);
    }

    void write_add(
        itch::StockLocate locate,
        itch::OrderId order_id,
        itch::Side side,
        itch::Quantity shares,
        const std::string& symbol,
        itch::Price price,
        bool attributed) {
        const char type = attributed ? itch::AddOrderWithMpid::kType : itch::AddOrder::kType;
        auto message = header(type, locate);
        append_big_endian(message, order_id, 8);
        append_byte(message, side == itch::Side::Buy ? 'B' : 'S');
        append_big_endian(message, shares, 4);
        append_alpha(message, symbol, 8);
        append_big_endian(message, price, 4);
        if (attributed) {
            append_alpha(message, "TEST", 4);
        }
        write_frame(message);
    }

    void write_execution(
        itch::StockLocate locate,
        itch::OrderId order_id,
        itch::Quantity shares,
        itch::MatchNumber match_number) {
        auto message = header(itch::OrderExecuted::kType, locate);
        append_big_endian(message, order_id, 8);
        append_big_endian(message, shares, 4);
        append_big_endian(message, match_number, 8);
        write_frame(message);
    }

    void write_cancel(
        itch::StockLocate locate,
        itch::OrderId order_id,
        itch::Quantity shares) {
        auto message = header(itch::OrderCancel::kType, locate);
        append_big_endian(message, order_id, 8);
        append_big_endian(message, shares, 4);
        write_frame(message);
    }

    void write_replace(
        itch::StockLocate locate,
        itch::OrderId old_order_id,
        itch::OrderId new_order_id,
        itch::Quantity shares,
        itch::Price price) {
        auto message = header(itch::OrderReplace::kType, locate);
        append_big_endian(message, old_order_id, 8);
        append_big_endian(message, new_order_id, 8);
        append_big_endian(message, shares, 4);
        append_big_endian(message, price, 4);
        write_frame(message);
    }

    void write_execution_with_price(
        itch::StockLocate locate,
        itch::OrderId order_id,
        itch::Quantity shares,
        itch::MatchNumber match_number,
        itch::Price execution_price) {
        auto message = header(itch::OrderExecutedWithPrice::kType, locate);
        append_big_endian(message, order_id, 8);
        append_big_endian(message, shares, 4);
        append_big_endian(message, match_number, 8);
        append_byte(message, 'Y');
        append_big_endian(message, execution_price, 4);
        write_frame(message);
    }

    void finish() {
        const char end_marker[2]{'\0', '\0'};
        output_.write(end_marker, 2);
    }

    [[nodiscard]] std::uint64_t message_count() const noexcept {
        return message_count_;
    }

private:
    static void append_byte(std::vector<std::uint8_t>& message, char value) {
        message.push_back(static_cast<std::uint8_t>(value));
    }

    static void append_big_endian(
        std::vector<std::uint8_t>& message,
        std::uint64_t value,
        std::size_t width) {
        for (std::size_t index = width; index > 0; --index) {
            const auto shift = static_cast<unsigned>((index - 1) * 8);
            message.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
        }
    }

    static void append_alpha(
        std::vector<std::uint8_t>& message,
        std::string value,
        std::size_t width) {
        value.resize(width, ' ');
        for (std::size_t index = 0; index < width; ++index) {
            append_byte(message, value[index]);
        }
    }

    std::vector<std::uint8_t> header(char type, itch::StockLocate locate) {
        std::vector<std::uint8_t> message;
        message.reserve(itch::AddOrderWithMpid::kSize);
        append_byte(message, type);
        append_big_endian(message, locate, 2);
        append_big_endian(message, tracking_number_++, 2);
        append_big_endian(message, timestamp_++, 6);
        return message;
    }

    void write_frame(const std::vector<std::uint8_t>& message) {
        const auto size = static_cast<std::uint16_t>(message.size());
        const char length[2]{
            static_cast<char>((size >> 8U) & 0xffU),
            static_cast<char>(size & 0xffU),
        };
        output_.write(length, 2);
        output_.write(
            reinterpret_cast<const char*>(message.data()),
            static_cast<std::streamsize>(message.size()));
        ++message_count_;
    }

    std::ostream& output_;
    itch::TrackingNumber tracking_number_{0};
    itch::Timestamp timestamp_{34'200'000'000'000ULL};
    std::uint64_t message_count_{0};
};

std::string symbol_for(std::uint64_t index) {
    std::ostringstream symbol;
    symbol << 'S' << std::setfill('0') << std::setw(7) << index;
    return symbol.str();
}

std::optional<std::uint64_t> parse_cycles(const char* text) {
    try {
        const std::string value{text};
        std::size_t position = 0;
        const auto parsed = std::stoull(value, &position);
        if (position != value.size() || parsed == 0) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: itch_generate_synthetic output-file cycle-count\n"
                  << "Each cycle writes A/F, E, X, U, and C (five messages).\n";
        return 2;
    }

    const auto cycles = parse_cycles(argv[2]);
    if (!cycles) {
        std::cerr << "Cycle count must be a positive integer.\n";
        return 2;
    }

    const std::filesystem::path output_path{argv[1]};
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not open output file: " << output_path << '\n';
        return 2;
    }

    constexpr std::uint64_t symbol_count = 64;
    constexpr itch::OrderId replacement_id_offset = 1'000'000'000'000ULL;
    SyntheticWriter writer(output);
    writer.write_system_event('O');

    for (std::uint64_t index = 1; index <= symbol_count; ++index) {
        const auto locate = static_cast<itch::StockLocate>(index);
        const std::string symbol = symbol_for(index);
        writer.write_directory(locate, symbol);
        writer.write_trading_action(locate, symbol);
    }

    for (std::uint64_t cycle = 0; cycle < *cycles; ++cycle) {
        const auto symbol_index = cycle % symbol_count + 1;
        const auto locate = static_cast<itch::StockLocate>(symbol_index);
        const std::string symbol = symbol_for(symbol_index);
        const itch::OrderId order_id = cycle + 1;
        const itch::OrderId replacement_id = replacement_id_offset + order_id;
        const itch::Side side = cycle % 2 == 0 ? itch::Side::Buy : itch::Side::Sell;
        const itch::Price price = static_cast<itch::Price>(
            1'000'000U + static_cast<itch::Price>((cycle % 1'000U) * 100U));

        writer.write_add(locate, order_id, side, 1'000, symbol, price, cycle % 2 != 0);
        writer.write_execution(locate, order_id, 100, cycle * 2 + 1);
        writer.write_cancel(locate, order_id, 100);
        writer.write_replace(locate, order_id, replacement_id, 600, price + 100U);
        writer.write_execution_with_price(
            locate,
            replacement_id,
            600,
            cycle * 2 + 2,
            price + 50U);
    }

    writer.write_system_event('C');
    writer.finish();
    output.close();
    if (!output) {
        std::cerr << "Failed while writing output file: " << output_path << '\n';
        return 1;
    }

    std::error_code error;
    const auto bytes = std::filesystem::file_size(output_path, error);
    std::cout << "Synthetic dataset written: " << output_path << '\n'
              << "Messages: " << writer.message_count() << '\n';
    if (!error) {
        std::cout << "Bytes: " << bytes << '\n';
    }
    std::cout << "This fixture is deterministic but is not representative of real market traffic.\n";
    return 0;
}
