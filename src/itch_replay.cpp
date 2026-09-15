#include "itch/replay.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

void print_statistics(
    const itch::ReplayEngine& engine,
    std::chrono::duration<double> elapsed) {
    const auto& statistics = engine.statistics();
    const double seconds = elapsed.count();
    const double messages_per_second =
        seconds > 0.0 ? static_cast<double>(statistics.messages_processed) / seconds : 0.0;
    const double nanoseconds_per_message = statistics.messages_processed > 0
        ? seconds * 1'000'000'000.0 / static_cast<double>(statistics.messages_processed)
        : 0.0;

    std::cout << "Messages processed:     " << statistics.messages_processed << '\n'
              << "Unsupported skipped:    " << statistics.unsupported_messages << '\n'
              << "System events:          " << statistics.system_events << '\n'
              << "Stock directories:      " << statistics.stock_directories << '\n'
              << "Trading actions:        " << statistics.trading_actions << '\n'
              << "Orders added:           " << statistics.orders_added << '\n'
              << "Executions:             " << statistics.executions << '\n'
              << "Cancels:                " << statistics.cancels << '\n'
              << "Deletes:                " << statistics.deletes << '\n'
              << "Replaces:               " << statistics.replaces << '\n'
              << "Symbols reconstructed:  " << engine.market().symbol_count() << '\n'
              << "Active orders:          " << engine.market().order_count() << '\n'
              << "Peak active orders:     " << statistics.peak_active_orders << '\n'
              << std::fixed << std::setprecision(6)
              << "Elapsed time:           " << seconds << " s\n"
              << "Throughput:             " << messages_per_second / 1'000'000.0
              << " million messages/s\n"
              << "Average:                " << nanoseconds_per_message << " ns/message\n";
}

void print_failure(const itch::ReplayResult& result) {
    std::cerr << "Replay failed at BinaryFILE record " << result.record_number << ": "
              << itch::to_string(result.status);

    if (result.status == itch::ReplayStatus::FrameError) {
        std::cerr << " (" << itch::to_string(result.frame_status) << ')';
    } else if (result.status == itch::ReplayStatus::DecodeError) {
        std::cerr << " for message '" << result.message_type << "' ("
                  << itch::to_string(result.decode_error);
        if (result.decode_error == itch::DecodeError::IncorrectMessageLength) {
            std::cerr << ", expected " << result.expected_size << " bytes, received "
                      << result.actual_size;
        }
        std::cerr << ')';
    } else if (result.status == itch::ReplayStatus::BookError) {
        std::cerr << " for message '" << result.message_type << "' ("
                  << itch::to_string(result.book_error) << ')';
    }
    std::cerr << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    const bool use_mmap = argc == 3 && std::string_view{argv[1]} == "--mmap";
    if ((argc != 2 && !use_mmap) || (argc == 2 && std::string_view{argv[1]} == "--mmap")) {
        std::cerr << "Usage: itch_replay [--mmap] path/to/uncompressed-itch-file\n";
        return 2;
    }

    const std::filesystem::path input_path{argv[use_mmap ? 2 : 1]};
    if (input_path.extension() == ".gz") {
        std::cerr << "Compressed input is intentionally not accepted. Decompress the .gz file "
                     "first, then benchmark/replay the raw BinaryFILE.\n";
        return 2;
    }

    itch::ReplayEngine engine;
    itch::ReplayResult result;
    std::chrono::duration<double> elapsed;

    if (use_mmap) {
        try {
            const itch::MappedFile input(input_path);
            const auto start = std::chrono::steady_clock::now();
            result = itch::replay_bytes(input.bytes(), engine);
            const auto stop = std::chrono::steady_clock::now();
            elapsed = stop - start;
        } catch (const std::exception& error) {
            std::cerr << "Could not map input file: " << error.what() << '\n';
            return 2;
        }
    } else {
        std::ifstream input(input_path, std::ios::binary);
        if (!input) {
            std::cerr << "Could not open input file: " << input_path << '\n';
            return 2;
        }
        const auto start = std::chrono::steady_clock::now();
        result = itch::replay_stream(input, engine);
        const auto stop = std::chrono::steady_clock::now();
        elapsed = stop - start;
    }

    std::cout << "Input mode:             " << (use_mmap ? "mmap" : "buffered") << '\n';
    print_statistics(engine, elapsed);

    if (!result.ok()) {
        print_failure(result);
        return 1;
    }
    if (result.status == itch::ReplayStatus::CompleteWithoutBinaryFileTerminator) {
        std::cerr << "Warning: accepted EOF because the final record was the ITCH End of Messages "
                     "system event; no zero-length BinaryFILE terminator was present.\n";
    }
    if (!engine.market().check_invariants()) {
        std::cerr << "Replay completed, but the final order and price-level states disagree.\n";
        return 1;
    }

    return 0;
}
