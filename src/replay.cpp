#include "itch/replay.hpp"

#include <algorithm>
#include <string>
#include <variant>

namespace itch {

BookError ReplayEngine::apply(const Message& message) {
    ++statistics_.messages_processed;
    BookError result = BookError::None;

    if (std::holds_alternative<UnsupportedMessage>(message)) {
        ++statistics_.unsupported_messages;
        return result;
    }

    if (const auto* system_event = std::get_if<SystemEvent>(&message)) {
        ++statistics_.system_events;
        last_system_event_ = *system_event;
        return result;
    }

    if (const auto* directory = std::get_if<StockDirectory>(&message)) {
        ++statistics_.stock_directories;
        directories_.insert_or_assign(directory->header.stock_locate, *directory);
        return result;
    }

    if (const auto* action = std::get_if<StockTradingAction>(&message)) {
        ++statistics_.trading_actions;
        trading_actions_.insert_or_assign(action->stock, *action);
        return result;
    }

    if (const auto* add = std::get_if<AddOrder>(&message)) {
        result = market_.add_order(
            add->order_reference_number,
            add->stock,
            add->side,
            add->price,
            add->shares);
        if (result == BookError::None) {
            ++statistics_.orders_added;
            statistics_.peak_active_orders = std::max(
                statistics_.peak_active_orders, market_.order_count());
        }
    } else if (const auto* add = std::get_if<AddOrderWithMpid>(&message)) {
        result = market_.add_order(
            add->order_reference_number,
            add->stock,
            add->side,
            add->price,
            add->shares);
        if (result == BookError::None) {
            ++statistics_.orders_added;
            statistics_.peak_active_orders = std::max(
                statistics_.peak_active_orders, market_.order_count());
        }
    } else if (const auto* execution = std::get_if<OrderExecuted>(&message)) {
        result = market_.execute_order(
            execution->order_reference_number,
            execution->executed_shares);
        if (result == BookError::None) {
            ++statistics_.executions;
        }
    } else if (const auto* execution = std::get_if<OrderExecutedWithPrice>(&message)) {
        // The execution price is trade metadata. The displayed book is reduced at
        // the original order price stored in MarketState.
        result = market_.execute_order(
            execution->order_reference_number,
            execution->executed_shares);
        if (result == BookError::None) {
            ++statistics_.executions;
        }
    } else if (const auto* cancel = std::get_if<OrderCancel>(&message)) {
        result = market_.cancel_order(cancel->order_reference_number, cancel->canceled_shares);
        if (result == BookError::None) {
            ++statistics_.cancels;
        }
    } else if (const auto* deletion = std::get_if<OrderDelete>(&message)) {
        result = market_.delete_order(deletion->order_reference_number);
        if (result == BookError::None) {
            ++statistics_.deletes;
        }
    } else if (const auto* replacement = std::get_if<OrderReplace>(&message)) {
        result = market_.replace_order(
            replacement->original_order_reference_number,
            replacement->new_order_reference_number,
            replacement->price,
            replacement->shares);
        if (result == BookError::None) {
            ++statistics_.replaces;
        }
    }

    if (result != BookError::None) {
        ++statistics_.book_errors;
    }
    return result;
}

const MarketState& ReplayEngine::market() const noexcept {
    return market_;
}

MarketState& ReplayEngine::market() noexcept {
    return market_;
}

const ReplayStatistics& ReplayEngine::statistics() const noexcept {
    return statistics_;
}

std::optional<SystemEvent> ReplayEngine::last_system_event() const {
    return last_system_event_;
}

const StockDirectory* ReplayEngine::find_directory(StockLocate locate) const {
    const auto directory = directories_.find(locate);
    return directory == directories_.end() ? nullptr : &directory->second;
}

const StockTradingAction* ReplayEngine::find_trading_action(std::string_view symbol) const {
    const auto action = trading_actions_.find(std::string{symbol});
    return action == trading_actions_.end() ? nullptr : &action->second;
}

DecodeStreamResult decode_stream(std::istream& input) {
    BinaryFileReader reader(input);
    DecodeStreamResult stream_result;
    bool ended_with_end_message = false;

    while (true) {
        FrameResult frame = reader.next();
        if (frame.status == FrameStatus::EndOfSession) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::Complete,
                .record_number = stream_result.messages_decoded,
            };
            return stream_result;
        }
        if (frame.status == FrameStatus::MissingEndOfSession && ended_with_end_message) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::CompleteWithoutBinaryFileTerminator,
                .record_number = stream_result.messages_decoded,
                .frame_status = frame.status,
            };
            return stream_result;
        }
        if (frame.status != FrameStatus::Message) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::FrameError,
                .record_number = stream_result.messages_decoded + 1,
                .frame_status = frame.status,
            };
            return stream_result;
        }

        const DecodeResult decoded = decode_message(frame.payload);
        if (!decoded.ok()) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::DecodeError,
                .record_number = stream_result.messages_decoded + 1,
                .message_type = decoded.type,
                .frame_status = FrameStatus::Message,
                .decode_error = decoded.error,
                .book_error = BookError::None,
                .expected_size = decoded.expected_size,
                .actual_size = decoded.actual_size,
            };
            return stream_result;
        }

        const auto* system_event = std::get_if<SystemEvent>(&*decoded.message);
        ended_with_end_message = system_event != nullptr && system_event->event_code == 'C';
        ++stream_result.messages_decoded;
        if (std::holds_alternative<UnsupportedMessage>(*decoded.message)) {
            ++stream_result.unsupported_messages;
        }

        // Consuming a tiny deterministic value makes the work observable to the
        // optimizer without adding meaningful per-message benchmark overhead.
        stream_result.checksum = stream_result.checksum * 131U
            + static_cast<unsigned char>(message_type(*decoded.message));
    }
}

DecodeStreamResult decode_bytes(std::span<const std::uint8_t> bytes) {
    MappedFrameCursor reader(bytes);
    DecodeStreamResult stream_result;
    bool ended_with_end_message = false;

    while (true) {
        const FrameViewResult frame = reader.next();
        if (frame.status == FrameStatus::EndOfSession) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::Complete,
                .record_number = stream_result.messages_decoded,
            };
            return stream_result;
        }
        if (frame.status == FrameStatus::MissingEndOfSession && ended_with_end_message) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::CompleteWithoutBinaryFileTerminator,
                .record_number = stream_result.messages_decoded,
                .frame_status = frame.status,
            };
            return stream_result;
        }
        if (frame.status != FrameStatus::Message) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::FrameError,
                .record_number = stream_result.messages_decoded + 1,
                .frame_status = frame.status,
            };
            return stream_result;
        }

        const DecodeResult decoded = decode_message(frame.payload);
        if (!decoded.ok()) {
            stream_result.result = ReplayResult{
                .status = ReplayStatus::DecodeError,
                .record_number = stream_result.messages_decoded + 1,
                .message_type = decoded.type,
                .frame_status = FrameStatus::Message,
                .decode_error = decoded.error,
                .book_error = BookError::None,
                .expected_size = decoded.expected_size,
                .actual_size = decoded.actual_size,
            };
            return stream_result;
        }

        const auto* system_event = std::get_if<SystemEvent>(&*decoded.message);
        ended_with_end_message = system_event != nullptr && system_event->event_code == 'C';
        ++stream_result.messages_decoded;
        if (std::holds_alternative<UnsupportedMessage>(*decoded.message)) {
            ++stream_result.unsupported_messages;
        }
        stream_result.checksum = stream_result.checksum * 131U
            + static_cast<unsigned char>(message_type(*decoded.message));
    }
}

ReplayResult replay_stream(std::istream& input, ReplayEngine& engine) {
    BinaryFileReader reader(input);
    std::uint64_t record_number = 0;
    bool ended_with_end_message = false;

    while (true) {
        FrameResult frame = reader.next();
        if (frame.status == FrameStatus::EndOfSession) {
            return ReplayResult{
                .status = ReplayStatus::Complete,
                .record_number = record_number,
            };
        }
        if (frame.status == FrameStatus::MissingEndOfSession && ended_with_end_message) {
            return ReplayResult{
                .status = ReplayStatus::CompleteWithoutBinaryFileTerminator,
                .record_number = record_number,
                .frame_status = frame.status,
            };
        }
        if (frame.status != FrameStatus::Message) {
            return ReplayResult{
                .status = ReplayStatus::FrameError,
                .record_number = record_number + 1,
                .frame_status = frame.status,
            };
        }

        ++record_number;
        const DecodeResult decoded = decode_message(frame.payload);
        if (!decoded.ok()) {
            return ReplayResult{
                .status = ReplayStatus::DecodeError,
                .record_number = record_number,
                .message_type = decoded.type,
                .frame_status = FrameStatus::Message,
                .decode_error = decoded.error,
                .book_error = BookError::None,
                .expected_size = decoded.expected_size,
                .actual_size = decoded.actual_size,
            };
        }

        const auto* system_event = std::get_if<SystemEvent>(&*decoded.message);
        ended_with_end_message = system_event != nullptr && system_event->event_code == 'C';
        const BookError book_error = engine.apply(*decoded.message);
        if (book_error != BookError::None) {
            return ReplayResult{
                .status = ReplayStatus::BookError,
                .record_number = record_number,
                .message_type = decoded.type,
                .frame_status = FrameStatus::Message,
                .decode_error = DecodeError::None,
                .book_error = book_error,
                .expected_size = decoded.expected_size,
                .actual_size = decoded.actual_size,
            };
        }
    }
}

ReplayResult replay_bytes(std::span<const std::uint8_t> bytes, ReplayEngine& engine) {
    MappedFrameCursor reader(bytes);
    std::uint64_t record_number = 0;
    bool ended_with_end_message = false;

    while (true) {
        const FrameViewResult frame = reader.next();
        if (frame.status == FrameStatus::EndOfSession) {
            return ReplayResult{
                .status = ReplayStatus::Complete,
                .record_number = record_number,
            };
        }
        if (frame.status == FrameStatus::MissingEndOfSession && ended_with_end_message) {
            return ReplayResult{
                .status = ReplayStatus::CompleteWithoutBinaryFileTerminator,
                .record_number = record_number,
                .frame_status = frame.status,
            };
        }
        if (frame.status != FrameStatus::Message) {
            return ReplayResult{
                .status = ReplayStatus::FrameError,
                .record_number = record_number + 1,
                .frame_status = frame.status,
            };
        }

        ++record_number;
        const DecodeResult decoded = decode_message(frame.payload);
        if (!decoded.ok()) {
            return ReplayResult{
                .status = ReplayStatus::DecodeError,
                .record_number = record_number,
                .message_type = decoded.type,
                .frame_status = FrameStatus::Message,
                .decode_error = decoded.error,
                .book_error = BookError::None,
                .expected_size = decoded.expected_size,
                .actual_size = decoded.actual_size,
            };
        }

        const auto* system_event = std::get_if<SystemEvent>(&*decoded.message);
        ended_with_end_message = system_event != nullptr && system_event->event_code == 'C';
        const BookError book_error = engine.apply(*decoded.message);
        if (book_error != BookError::None) {
            return ReplayResult{
                .status = ReplayStatus::BookError,
                .record_number = record_number,
                .message_type = decoded.type,
                .frame_status = FrameStatus::Message,
                .decode_error = DecodeError::None,
                .book_error = book_error,
                .expected_size = decoded.expected_size,
                .actual_size = decoded.actual_size,
            };
        }
    }
}

std::string_view to_string(ReplayStatus status) noexcept {
    switch (status) {
        case ReplayStatus::Complete:
            return "complete";
        case ReplayStatus::CompleteWithoutBinaryFileTerminator:
            return "complete via ITCH End of Messages; BinaryFILE terminator absent";
        case ReplayStatus::FrameError:
            return "BinaryFILE framing error";
        case ReplayStatus::DecodeError:
            return "ITCH decode error";
        case ReplayStatus::BookError:
            return "order-book update error";
    }
    return "unknown replay status";
}

}  // namespace itch
