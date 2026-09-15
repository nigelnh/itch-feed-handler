#include "itch/decoder.hpp"
#include "itch/framing.hpp"
#include "itch/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
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

constexpr itch::StockLocate kLocate = 0x1234;
constexpr itch::TrackingNumber kTracking = 0x5678;
constexpr itch::Timestamp kTimestamp = 0x0102'0304'0506ULL;

void append_big_endian(
    std::vector<std::uint8_t>& bytes,
    std::uint64_t value,
    std::size_t width) {
    for (std::size_t index = width; index > 0; --index) {
        const auto shift = static_cast<unsigned>((index - 1) * 8);
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_alpha(std::vector<std::uint8_t>& bytes, std::string value, std::size_t width) {
    value.resize(width, ' ');
    for (std::size_t index = 0; index < width; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value[index]));
    }
}

std::vector<std::uint8_t> header(
    char type,
    itch::StockLocate locate = kLocate,
    itch::TrackingNumber tracking = kTracking,
    itch::Timestamp timestamp = kTimestamp) {
    std::vector<std::uint8_t> bytes;
    bytes.push_back(static_cast<std::uint8_t>(type));
    append_big_endian(bytes, locate, 2);
    append_big_endian(bytes, tracking, 2);
    append_big_endian(bytes, timestamp, 6);
    return bytes;
}

std::vector<std::uint8_t> system_event(char event_code) {
    auto bytes = header(itch::SystemEvent::kType, 0);
    bytes.push_back(static_cast<std::uint8_t>(event_code));
    return bytes;
}

std::vector<std::uint8_t> stock_directory(std::string stock) {
    auto bytes = header(itch::StockDirectory::kType);
    append_alpha(bytes, std::move(stock), 8);
    bytes.push_back('Q');
    bytes.push_back('N');
    append_big_endian(bytes, 100, 4);
    bytes.push_back('N');
    bytes.push_back('C');
    append_alpha(bytes, "  ", 2);
    bytes.push_back('P');
    bytes.push_back('N');
    bytes.push_back('N');
    bytes.push_back('1');
    bytes.push_back('N');
    append_big_endian(bytes, 1, 4);
    bytes.push_back('N');
    return bytes;
}

std::vector<std::uint8_t> trading_action(std::string stock, char state) {
    auto bytes = header(itch::StockTradingAction::kType);
    append_alpha(bytes, std::move(stock), 8);
    bytes.push_back(static_cast<std::uint8_t>(state));
    bytes.push_back(' ');
    append_alpha(bytes, "TEST", 4);
    return bytes;
}

std::vector<std::uint8_t> add_order(
    char type,
    itch::OrderId order_id,
    char side,
    itch::Quantity shares,
    std::string stock,
    itch::Price price,
    std::string attribution = "") {
    auto bytes = header(type);
    append_big_endian(bytes, order_id, 8);
    bytes.push_back(static_cast<std::uint8_t>(side));
    append_big_endian(bytes, shares, 4);
    append_alpha(bytes, std::move(stock), 8);
    append_big_endian(bytes, price, 4);
    if (type == itch::AddOrderWithMpid::kType) {
        append_alpha(bytes, std::move(attribution), 4);
    }
    return bytes;
}

std::vector<std::uint8_t> order_executed(
    itch::OrderId order_id,
    itch::Quantity shares,
    itch::MatchNumber match_number) {
    auto bytes = header(itch::OrderExecuted::kType);
    append_big_endian(bytes, order_id, 8);
    append_big_endian(bytes, shares, 4);
    append_big_endian(bytes, match_number, 8);
    return bytes;
}

std::vector<std::uint8_t> order_executed_with_price(
    itch::OrderId order_id,
    itch::Quantity shares,
    itch::MatchNumber match_number,
    char printable,
    itch::Price execution_price) {
    auto bytes = header(itch::OrderExecutedWithPrice::kType);
    append_big_endian(bytes, order_id, 8);
    append_big_endian(bytes, shares, 4);
    append_big_endian(bytes, match_number, 8);
    bytes.push_back(static_cast<std::uint8_t>(printable));
    append_big_endian(bytes, execution_price, 4);
    return bytes;
}

std::vector<std::uint8_t> order_cancel(
    itch::OrderId order_id,
    itch::Quantity shares) {
    auto bytes = header(itch::OrderCancel::kType);
    append_big_endian(bytes, order_id, 8);
    append_big_endian(bytes, shares, 4);
    return bytes;
}

std::vector<std::uint8_t> order_delete(itch::OrderId order_id) {
    auto bytes = header(itch::OrderDelete::kType);
    append_big_endian(bytes, order_id, 8);
    return bytes;
}

std::vector<std::uint8_t> order_replace(
    itch::OrderId old_order_id,
    itch::OrderId new_order_id,
    itch::Quantity shares,
    itch::Price price) {
    auto bytes = header(itch::OrderReplace::kType);
    append_big_endian(bytes, old_order_id, 8);
    append_big_endian(bytes, new_order_id, 8);
    append_big_endian(bytes, shares, 4);
    append_big_endian(bytes, price, 4);
    return bytes;
}

template <typename MessageType>
std::optional<MessageType> decode_as(const std::vector<std::uint8_t>& payload) {
    const auto result = itch::decode_message(payload);
    CHECK(result.ok());
    if (!result.ok()) {
        return std::nullopt;
    }
    const auto* message = std::get_if<MessageType>(&*result.message);
    CHECK(message != nullptr);
    if (message == nullptr) {
        return std::nullopt;
    }
    return *message;
}

std::string binary_file(
    const std::vector<std::vector<std::uint8_t>>& messages,
    bool include_terminator = true) {
    std::string bytes;
    for (const auto& message : messages) {
        const auto size = static_cast<std::uint16_t>(message.size());
        bytes.push_back(static_cast<char>((size >> 8U) & 0xffU));
        bytes.push_back(static_cast<char>(size & 0xffU));
        for (const std::uint8_t byte : message) {
            bytes.push_back(static_cast<char>(byte));
        }
    }
    if (include_terminator) {
        bytes.push_back('\0');
        bytes.push_back('\0');
    }
    return bytes;
}

std::istringstream input_stream(std::string bytes) {
    return std::istringstream{std::move(bytes), std::ios::in | std::ios::binary};
}

std::vector<std::uint8_t> byte_vector(const std::string& bytes) {
    std::vector<std::uint8_t> result;
    result.reserve(bytes.size());
    for (const char byte : bytes) {
        result.push_back(static_cast<std::uint8_t>(byte));
    }
    return result;
}

void test_common_header_and_administrative_messages() {
    const auto event = decode_as<itch::SystemEvent>(system_event('O'));
    CHECK(event->header.stock_locate == 0);
    CHECK(event->header.tracking_number == kTracking);
    CHECK(event->header.timestamp == kTimestamp);
    CHECK(event->event_code == 'O');

    const auto directory = decode_as<itch::StockDirectory>(stock_directory("AAPL"));
    CHECK(directory->header.stock_locate == kLocate);
    CHECK(directory->stock == "AAPL");
    CHECK(directory->market_category == 'Q');
    CHECK(directory->financial_status_indicator == 'N');
    CHECK(directory->round_lot_size == 100);
    CHECK(directory->issue_sub_type.empty());
    CHECK(directory->etp_leverage_factor == 1);

    const auto action = decode_as<itch::StockTradingAction>(trading_action("AAPL", 'T'));
    CHECK(action->stock == "AAPL");
    CHECK(action->trading_state == 'T');
    CHECK(action->reason == "TEST");
}

void test_add_messages() {
    constexpr itch::OrderId order_id = 0x0102'0304'0506'0708ULL;
    constexpr itch::Quantity shares = 0x0102'0304U;
    constexpr itch::Price price = 2'001'000;

    const auto add = decode_as<itch::AddOrder>(
        add_order(itch::AddOrder::kType, order_id, 'B', shares, "AAPL", price));
    CHECK(add->order_reference_number == order_id);
    CHECK(add->side == itch::Side::Buy);
    CHECK(add->shares == shares);
    CHECK(add->stock == "AAPL");
    CHECK(add->price == price);

    const auto attributed = decode_as<itch::AddOrderWithMpid>(
        add_order(itch::AddOrderWithMpid::kType, 22, 'S', 500, "MSFT", 4'001'000, "ABCD"));
    CHECK(attributed->order_reference_number == 22);
    CHECK(attributed->side == itch::Side::Sell);
    CHECK(attributed->stock == "MSFT");
    CHECK(attributed->attribution == "ABCD");
}

void test_modify_messages() {
    constexpr itch::MatchNumber match = 0x0102'0304'0506'0708ULL;

    const auto executed = decode_as<itch::OrderExecuted>(order_executed(10, 25, match));
    CHECK(executed->order_reference_number == 10);
    CHECK(executed->executed_shares == 25);
    CHECK(executed->match_number == match);

    const auto with_price = decode_as<itch::OrderExecutedWithPrice>(
        order_executed_with_price(11, 30, match + 1, 'Y', 1'000'050));
    CHECK(with_price->order_reference_number == 11);
    CHECK(with_price->executed_shares == 30);
    CHECK(with_price->match_number == match + 1);
    CHECK(with_price->printable == 'Y');
    CHECK(with_price->execution_price == 1'000'050);

    const auto cancel = decode_as<itch::OrderCancel>(order_cancel(12, 35));
    CHECK(cancel->order_reference_number == 12);
    CHECK(cancel->canceled_shares == 35);

    const auto deletion = decode_as<itch::OrderDelete>(order_delete(13));
    CHECK(deletion->order_reference_number == 13);

    const auto replacement = decode_as<itch::OrderReplace>(
        order_replace(14, 15, 40, 1'000'200));
    CHECK(replacement->original_order_reference_number == 14);
    CHECK(replacement->new_order_reference_number == 15);
    CHECK(replacement->shares == 40);
    CHECK(replacement->price == 1'000'200);
}

void test_decoder_rejects_bad_supported_messages() {
    auto short_add = add_order(itch::AddOrder::kType, 1, 'B', 100, "AAPL", 1'000'000);
    short_add.pop_back();
    const auto wrong_length = itch::decode_message(short_add);
    CHECK(wrong_length.error == itch::DecodeError::IncorrectMessageLength);
    CHECK(wrong_length.expected_size == itch::AddOrder::kSize);
    CHECK(wrong_length.actual_size == itch::AddOrder::kSize - 1);

    const auto bad_side = itch::decode_message(
        add_order(itch::AddOrder::kType, 1, 'Z', 100, "AAPL", 1'000'000));
    CHECK(bad_side.error == itch::DecodeError::InvalidSide);

    const std::vector<std::uint8_t> empty;
    CHECK(itch::decode_message(empty).error == itch::DecodeError::EmptyPayload);
}

void test_unknown_message_is_preserved_for_safe_skipping() {
    const std::vector<std::uint8_t> payload{'P', 1, 2, 3};
    const auto result = itch::decode_message(payload);
    CHECK(result.ok());
    const auto* unknown = std::get_if<itch::UnsupportedMessage>(&*result.message);
    CHECK(unknown != nullptr);
    CHECK(unknown->type == 'P');
    CHECK(unknown->payload_size == 4);
}

void test_binary_file_framing() {
    auto input = input_stream(binary_file({system_event('O'), system_event('C')}));
    itch::BinaryFileReader reader(input);

    const auto first = reader.next();
    CHECK(first.status == itch::FrameStatus::Message);
    CHECK(first.payload == system_event('O'));
    const auto second = reader.next();
    CHECK(second.status == itch::FrameStatus::Message);
    CHECK(second.payload == system_event('C'));
    CHECK(reader.next().status == itch::FrameStatus::EndOfSession);

    const auto mapped_bytes = byte_vector(binary_file({system_event('O'), system_event('C')}));
    itch::MappedFrameCursor cursor(mapped_bytes);
    const auto mapped_first = cursor.next();
    CHECK(mapped_first.status == itch::FrameStatus::Message);
    CHECK(std::vector<std::uint8_t>(mapped_first.payload.begin(), mapped_first.payload.end())
          == system_event('O'));
    CHECK(cursor.next().status == itch::FrameStatus::Message);
    CHECK(cursor.next().status == itch::FrameStatus::EndOfSession);
}

void test_binary_file_framing_errors() {
    auto missing_end = input_stream(binary_file({system_event('O')}, false));
    itch::BinaryFileReader missing_end_reader(missing_end);
    CHECK(missing_end_reader.next().status == itch::FrameStatus::Message);
    CHECK(missing_end_reader.next().status == itch::FrameStatus::MissingEndOfSession);

    auto short_length = input_stream(std::string(1, '\0'));
    itch::BinaryFileReader short_length_reader(short_length);
    CHECK(short_length_reader.next().status == itch::FrameStatus::TruncatedLength);

    std::string short_payload;
    short_payload.push_back('\0');
    short_payload.push_back(static_cast<char>(5));
    short_payload.append("ABC");
    auto truncated = input_stream(std::move(short_payload));
    itch::BinaryFileReader truncated_reader(truncated);
    CHECK(truncated_reader.next().status == itch::FrameStatus::TruncatedPayload);

    const std::vector<std::uint8_t> one_byte{0};
    itch::MappedFrameCursor mapped_short_length(one_byte);
    CHECK(mapped_short_length.next().status == itch::FrameStatus::TruncatedLength);

    const std::vector<std::uint8_t> mapped_short_payload{0, 5, 'A', 'B', 'C'};
    itch::MappedFrameCursor mapped_truncated(mapped_short_payload);
    CHECK(mapped_truncated.next().status == itch::FrameStatus::TruncatedPayload);
}

void test_end_to_end_binary_replay() {
    constexpr itch::Price bid_price = 1'000'000;
    constexpr itch::Price ask_price = 1'000'100;
    constexpr itch::Price replacement_price = 1'000'200;

    const std::vector<std::vector<std::uint8_t>> messages{
        system_event('O'),
        stock_directory("AAPL"),
        trading_action("AAPL", 'T'),
        add_order(itch::AddOrder::kType, 1, 'B', 100, "AAPL", bid_price),
        add_order(itch::AddOrderWithMpid::kType, 2, 'S', 80, "AAPL", ask_price, "ABCD"),
        order_executed(1, 40, 1001),
        order_executed_with_price(2, 30, 1002, 'Y', 1'000'050),
        order_cancel(1, 10),
        order_replace(2, 3, 60, replacement_price),
        order_delete(1),
        {'P', 1, 2, 3},
    };

    auto decode_input = input_stream(binary_file(messages));
    const auto decode_result = itch::decode_stream(decode_input);
    CHECK(decode_result.result.ok());
    CHECK(decode_result.messages_decoded == messages.size());
    CHECK(decode_result.unsupported_messages == 1);
    CHECK(decode_result.checksum != 0);

    const auto mapped_bytes = byte_vector(binary_file(messages));
    const auto mapped_decode_result = itch::decode_bytes(mapped_bytes);
    CHECK(mapped_decode_result.result.ok());
    CHECK(mapped_decode_result.messages_decoded == decode_result.messages_decoded);
    CHECK(mapped_decode_result.unsupported_messages == decode_result.unsupported_messages);
    CHECK(mapped_decode_result.checksum == decode_result.checksum);

    auto input = input_stream(binary_file(messages));
    itch::ReplayEngine engine;
    const auto result = itch::replay_stream(input, engine);

    CHECK(result.ok());
    CHECK(result.record_number == messages.size());
    CHECK(engine.market().order_count() == 1);
    CHECK(!engine.market().find_order(1).has_value());
    CHECK(!engine.market().find_order(2).has_value());
    const auto replacement = engine.market().find_order(3);
    CHECK(replacement.has_value());
    CHECK(replacement->symbol == "AAPL");
    CHECK(replacement->side == itch::Side::Sell);
    CHECK(replacement->price == replacement_price);
    CHECK(replacement->remaining_quantity == 60);

    const auto* book = engine.market().find_book("AAPL");
    CHECK(book != nullptr);
    CHECK(book->quantity_at(itch::Side::Buy, bid_price) == 0);
    CHECK(book->quantity_at(itch::Side::Sell, ask_price) == 0);
    CHECK(book->quantity_at(itch::Side::Sell, replacement_price) == 60);
    CHECK(engine.market().check_invariants());

    const auto& statistics = engine.statistics();
    CHECK(statistics.messages_processed == messages.size());
    CHECK(statistics.unsupported_messages == 1);
    CHECK(statistics.system_events == 1);
    CHECK(statistics.stock_directories == 1);
    CHECK(statistics.trading_actions == 1);
    CHECK(statistics.orders_added == 2);
    CHECK(statistics.executions == 2);
    CHECK(statistics.cancels == 1);
    CHECK(statistics.deletes == 1);
    CHECK(statistics.replaces == 1);
    CHECK(statistics.peak_active_orders == 2);

    CHECK(engine.last_system_event()->event_code == 'O');
    CHECK(engine.find_directory(kLocate)->stock == "AAPL");
    CHECK(engine.find_trading_action("AAPL")->trading_state == 'T');

    itch::ReplayEngine mapped_engine;
    const auto mapped_result = itch::replay_bytes(mapped_bytes, mapped_engine);
    CHECK(mapped_result.ok());
    CHECK(mapped_engine.market().snapshot() == engine.market().snapshot());
    CHECK(mapped_engine.statistics().messages_processed == statistics.messages_processed);
    CHECK(mapped_engine.market().check_invariants());
}

void test_replay_reports_decode_and_book_errors() {
    auto short_add = add_order(itch::AddOrder::kType, 1, 'B', 100, "AAPL", 1'000'000);
    short_add.pop_back();
    auto bad_decode_input = input_stream(binary_file({short_add}));
    itch::ReplayEngine decode_engine;
    const auto decode_result = itch::replay_stream(bad_decode_input, decode_engine);
    CHECK(decode_result.status == itch::ReplayStatus::DecodeError);
    CHECK(decode_result.record_number == 1);
    CHECK(decode_result.message_type == 'A');

    auto bad_book_input = input_stream(binary_file({
        add_order(itch::AddOrder::kType, 1, 'B', 100, "AAPL", 1'000'000),
        order_cancel(1, 101),
    }));
    itch::ReplayEngine book_engine;
    const auto book_result = itch::replay_stream(bad_book_input, book_engine);
    CHECK(book_result.status == itch::ReplayStatus::BookError);
    CHECK(book_result.record_number == 2);
    CHECK(book_result.message_type == 'X');
    CHECK(book_result.book_error == itch::BookError::QuantityExceedsRemaining);
    CHECK(book_engine.market().find_order(1)->remaining_quantity == 100);
    CHECK(book_engine.market().check_invariants());
}

void test_replay_requires_end_of_session_marker() {
    auto input = input_stream(binary_file({system_event('O')}, false));
    itch::ReplayEngine engine;
    const auto result = itch::replay_stream(input, engine);
    CHECK(result.status == itch::ReplayStatus::FrameError);
    CHECK(result.record_number == 2);
    CHECK(result.frame_status == itch::FrameStatus::MissingEndOfSession);

    auto ended_input = input_stream(binary_file({system_event('O'), system_event('C')}, false));
    itch::ReplayEngine ended_engine;
    const auto ended_result = itch::replay_stream(ended_input, ended_engine);
    CHECK(ended_result.ok());
    CHECK(ended_result.status == itch::ReplayStatus::CompleteWithoutBinaryFileTerminator);
    CHECK(ended_result.record_number == 2);

    const auto ended_bytes = byte_vector(binary_file({system_event('C')}, false));
    const auto mapped_decode = itch::decode_bytes(ended_bytes);
    CHECK(mapped_decode.result.ok());
    CHECK(mapped_decode.result.status
          == itch::ReplayStatus::CompleteWithoutBinaryFileTerminator);
}

}  // namespace

int main() {
    using Test = std::pair<std::string, std::function<void()>>;
    const std::vector<Test> tests{
        {"decode common header and administrative messages",
         test_common_header_and_administrative_messages},
        {"decode add messages", test_add_messages},
        {"decode modify messages", test_modify_messages},
        {"reject malformed supported messages", test_decoder_rejects_bad_supported_messages},
        {"preserve unsupported messages", test_unknown_message_is_preserved_for_safe_skipping},
        {"read BinaryFILE framing", test_binary_file_framing},
        {"report BinaryFILE framing errors", test_binary_file_framing_errors},
        {"replay binary messages end to end", test_end_to_end_binary_replay},
        {"report decode and book errors", test_replay_reports_decode_and_book_errors},
        {"require BinaryFILE terminator", test_replay_requires_end_of_session_marker},
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

        std::cout << (failure_count == failures_before ? "[PASS] " : "[FAIL] ")
                  << name << '\n';
    }

    std::cout << '\n' << tests.size() << " tests run, " << failure_count << " failure(s).\n";
    return failure_count == 0 ? 0 : 1;
}
