#include "itch/replay.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Measurement {
    double seconds;
    std::uint64_t messages;
    std::uint64_t checksum;
    std::size_t peak_active_orders;
};

void print_replay_failure(const itch::ReplayResult& result) {
    std::cerr << "Failure at record " << result.record_number << ": "
              << itch::to_string(result.status);
    if (result.status == itch::ReplayStatus::FrameError) {
        std::cerr << " (" << itch::to_string(result.frame_status) << ')';
    } else if (result.status == itch::ReplayStatus::DecodeError) {
        std::cerr << " for type '" << result.message_type << "' ("
                  << itch::to_string(result.decode_error) << ')';
    } else if (result.status == itch::ReplayStatus::BookError) {
        std::cerr << " for type '" << result.message_type << "' ("
                  << itch::to_string(result.book_error) << ')';
    }
    std::cerr << '\n';
}

std::optional<Measurement> measure_decode_only(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open " << path << '\n';
        return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    const itch::DecodeStreamResult decoded = itch::decode_stream(input);
    const auto stop = std::chrono::steady_clock::now();
    if (!decoded.result.ok()) {
        print_replay_failure(decoded.result);
        return std::nullopt;
    }

    return Measurement{
        std::chrono::duration<double>(stop - start).count(),
        decoded.messages_decoded,
        decoded.checksum,
        0,
    };
}

std::optional<Measurement> measure_full_replay(
    const std::filesystem::path& path,
    std::size_t order_reserve = 0) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open " << path << '\n';
        return std::nullopt;
    }

    itch::ReplayEngine engine;
    const auto start = std::chrono::steady_clock::now();
    if (order_reserve > 0) {
        engine.market().reserve_orders(order_reserve);
    }
    const itch::ReplayResult result = itch::replay_stream(input, engine);
    const auto stop = std::chrono::steady_clock::now();
    if (!result.ok()) {
        print_replay_failure(result);
        return std::nullopt;
    }
    if (!engine.market().check_invariants()) {
        std::cerr << "Final market-state invariant check failed\n";
        return std::nullopt;
    }

    const auto& statistics = engine.statistics();
    const std::uint64_t checksum = statistics.orders_added
        ^ (statistics.executions << 1U)
        ^ (statistics.cancels << 2U)
        ^ (statistics.deletes << 3U)
        ^ (statistics.replaces << 4U)
        ^ static_cast<std::uint64_t>(engine.market().order_count());
    return Measurement{
        std::chrono::duration<double>(stop - start).count(),
        statistics.messages_processed,
        checksum,
        statistics.peak_active_orders,
    };
}

std::optional<Measurement> measure_mapped_decode(const std::filesystem::path& path) {
    try {
        const itch::MappedFile file(path);
        const auto start = std::chrono::steady_clock::now();
        const itch::DecodeStreamResult decoded = itch::decode_bytes(file.bytes());
        const auto stop = std::chrono::steady_clock::now();
        if (!decoded.result.ok()) {
            print_replay_failure(decoded.result);
            return std::nullopt;
        }
        return Measurement{
            std::chrono::duration<double>(stop - start).count(),
            decoded.messages_decoded,
            decoded.checksum,
            0,
        };
    } catch (const std::exception& error) {
        std::cerr << "Memory-mapped decode failed: " << error.what() << '\n';
        return std::nullopt;
    }
}

std::optional<Measurement> measure_mapped_replay(const std::filesystem::path& path) {
    try {
        itch::ReplayEngine engine;
        const itch::MappedFile file(path);
        const auto start = std::chrono::steady_clock::now();
        const itch::ReplayResult result = itch::replay_bytes(file.bytes(), engine);
        const auto stop = std::chrono::steady_clock::now();
        if (!result.ok()) {
            print_replay_failure(result);
            return std::nullopt;
        }
        if (!engine.market().check_invariants()) {
            std::cerr << "Final memory-mapped market-state invariant check failed\n";
            return std::nullopt;
        }

        const auto& statistics = engine.statistics();
        const std::uint64_t checksum = statistics.orders_added
            ^ (statistics.executions << 1U)
            ^ (statistics.cancels << 2U)
            ^ (statistics.deletes << 3U)
            ^ (statistics.replaces << 4U)
            ^ static_cast<std::uint64_t>(engine.market().order_count());
        return Measurement{
            std::chrono::duration<double>(stop - start).count(),
            statistics.messages_processed,
            checksum,
            statistics.peak_active_orders,
        };
    } catch (const std::exception& error) {
        std::cerr << "Memory-mapped replay failed: " << error.what() << '\n';
        return std::nullopt;
    }
}

void print_measurement(std::string_view label, std::size_t iteration, const Measurement& value) {
    const double messages = static_cast<double>(value.messages);
    const double messages_per_second = messages / value.seconds;
    const double nanoseconds_per_message = value.seconds * 1'000'000'000.0 / messages;
    std::cout << "  " << label << " iteration " << (iteration + 1) << ": "
              << std::fixed << std::setprecision(3)
              << messages_per_second / 1'000'000.0 << " M msg/s, "
              << nanoseconds_per_message << " ns/msg, "
              << value.seconds << " s\n" << std::flush;
}

bool same_work(
    const Measurement& reference,
    const Measurement& candidate,
    std::string_view comparison) {
    if (candidate.messages != reference.messages) {
        std::cerr << comparison << " message counts differ.\n";
        return false;
    }
    if (candidate.checksum != reference.checksum) {
        std::cerr << comparison << " checksums differ.\n";
        return false;
    }
    if (candidate.peak_active_orders != reference.peak_active_orders) {
        std::cerr << comparison << " peak active-order counts differ.\n";
        return false;
    }
    return true;
}

void print_summary(std::string_view label, std::vector<Measurement> measurements) {
    std::ranges::sort(measurements, {}, &Measurement::seconds);
    const Measurement& median = measurements[measurements.size() / 2];
    const Measurement& fastest = measurements.front();

    const auto rate = [](const Measurement& value) {
        return static_cast<double>(value.messages) / value.seconds;
    };
    const auto latency = [](const Measurement& value) {
        return value.seconds * 1'000'000'000.0 / static_cast<double>(value.messages);
    };

    std::cout << label << " summary (median): " << std::fixed << std::setprecision(3)
              << rate(median) / 1'000'000.0 << " M msg/s, " << latency(median)
              << " ns/msg\n"
              << label << " fastest:          " << rate(fastest) / 1'000'000.0
              << " M msg/s, " << latency(fastest) << " ns/msg\n";
}

std::optional<std::size_t> parse_positive_size(const char* text) {
    try {
        const std::string value{text};
        std::size_t position = 0;
        const unsigned long parsed = std::stoul(value, &position);
        if (position != value.size() || parsed == 0
            || parsed > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::string_view operating_system() noexcept {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(_WIN32)
    return "Windows";
#else
    return "Unknown";
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 5) {
        std::cerr << "Usage: itch_benchmark path/to/uncompressed-itch-file [iterations] "
                     "[[order-reserve] [--replay-only] | --decode-only]\n";
        return 2;
    }

    const std::filesystem::path path{argv[1]};
    if (path.extension() == ".gz") {
        std::cerr << "Benchmark only uncompressed BinaryFILE input; decompression is a separate step.\n";
        return 2;
    }

    const auto iterations = argc >= 3
        ? parse_positive_size(argv[2])
        : std::optional<std::size_t>{3};
    if (!iterations) {
        std::cerr << "Iterations must be a positive integer.\n";
        return 2;
    }
    const bool decode_only = argc == 4 && std::string_view{argv[3]} == "--decode-only";
    const auto order_reserve = argc >= 4 && !decode_only
        ? parse_positive_size(argv[3])
        : std::optional<std::size_t>{};
    if (argc >= 4 && !decode_only && !order_reserve) {
        std::cerr << "Order reserve must be a positive integer.\n";
        return 2;
    }
    if (argc == 5 && !order_reserve) {
        std::cerr << "Replay-only mode requires an order reserve.\n";
        return 2;
    }
    const bool replay_only = argc == 5 && std::string_view{argv[4]} == "--replay-only";
    if (argc == 5 && !replay_only) {
        std::cerr << "Unknown option: " << argv[4] << '\n';
        return 2;
    }

    std::error_code file_error;
    const auto file_size = std::filesystem::file_size(path, file_error);
    if (file_error) {
        std::cerr << "Could not inspect input file: " << path << '\n';
        return 2;
    }

    std::cout << "Dataset:             " << path << '\n'
              << "Dataset bytes:       " << file_size << '\n'
              << "Iterations:          " << *iterations << '\n'
              << "OS:                  " << operating_system() << '\n'
              << "Compiler:            " << __VERSION__ << '\n'
              << "Hardware threads:    " << std::thread::hardware_concurrency() << '\n'
              << "Build type:          " << ITCH_BUILD_TYPE << '\n'
              << "Order reserve test:  ";
    if (order_reserve) {
        std::cout << *order_reserve << " elements requested; startup reserve is timed\n";
    } else {
        std::cout << "disabled\n";
    }
    if (replay_only) {
        std::cout << "Methodology:         compare buffered full replay without/with reserve\n\n";
    } else if (decode_only) {
        std::cout << "Methodology:         compare buffered and mmap decode-only paths\n\n";
    } else {
        std::cout << "Methodology:         compare buffered and mmap framing/decode"
                     " (full replay also applies book state)\n\n";
    }
    std::cout << std::flush;

    std::vector<Measurement> decode_measurements;
    std::vector<Measurement> replay_measurements;
    std::vector<Measurement> mapped_decode_measurements;
    std::vector<Measurement> mapped_replay_measurements;
    std::vector<Measurement> reserved_replay_measurements;
    decode_measurements.reserve(*iterations);
    replay_measurements.reserve(*iterations);
    mapped_decode_measurements.reserve(*iterations);
    mapped_replay_measurements.reserve(*iterations);
    reserved_replay_measurements.reserve(order_reserve ? *iterations : 0);

    if (!replay_only) {
        for (std::size_t iteration = 0; iteration < *iterations; ++iteration) {
            const auto measurement = measure_decode_only(path);
            if (!measurement) {
                return 1;
            }
            if (!decode_measurements.empty()
                && !same_work(
                    decode_measurements.front(), *measurement, "Decode-only iterations")) {
                return 1;
            }
            print_measurement("decode-only", iteration, *measurement);
            decode_measurements.push_back(*measurement);
        }
    }

    if (!decode_only) {
        std::cout << '\n';
        for (std::size_t iteration = 0; iteration < *iterations; ++iteration) {
            const auto measurement = measure_full_replay(path);
            if (!measurement) {
                return 1;
            }
            if (!decode_measurements.empty()
                && measurement->messages != decode_measurements.front().messages) {
                std::cerr << "Decode-only and full replay message counts differ.\n";
                return 1;
            }
            if (!replay_measurements.empty()
                && !same_work(
                    replay_measurements.front(), *measurement, "Full-replay iterations")) {
                return 1;
            }
            print_measurement("full-replay", iteration, *measurement);
            replay_measurements.push_back(*measurement);
        }
    }

    if (!replay_only) {
        std::cout << '\n';
        for (std::size_t iteration = 0; iteration < *iterations; ++iteration) {
            const auto measurement = measure_mapped_decode(path);
            if (!measurement) {
                return 1;
            }
            if (!same_work(
                    decode_measurements.front(), *measurement, "Buffered and mmap decode")) {
                return 1;
            }
            print_measurement("mmap-decode", iteration, *measurement);
            mapped_decode_measurements.push_back(*measurement);
        }

        if (!decode_only) {
            std::cout << '\n';
            for (std::size_t iteration = 0; iteration < *iterations; ++iteration) {
                const auto measurement = measure_mapped_replay(path);
                if (!measurement) {
                    return 1;
                }
                if (!same_work(
                        replay_measurements.front(), *measurement, "Buffered and mmap replay")) {
                    return 1;
                }
                print_measurement("mmap-replay", iteration, *measurement);
                mapped_replay_measurements.push_back(*measurement);
            }
        }
    }

    if (order_reserve) {
        std::cout << '\n';
        for (std::size_t iteration = 0; iteration < *iterations; ++iteration) {
            const auto measurement = measure_full_replay(path, *order_reserve);
            if (!measurement) {
                return 1;
            }
            if (!same_work(
                    replay_measurements.front(),
                    *measurement,
                    "Buffered baseline and reserved replay")) {
                return 1;
            }
            print_measurement("reserved-replay", iteration, *measurement);
            reserved_replay_measurements.push_back(*measurement);
        }
    }

    std::cout << '\n';
    const std::size_t observed_peak_orders =
        decode_only ? 0 : replay_measurements.front().peak_active_orders;
    if (!replay_only) {
        print_summary("Decode-only", std::move(decode_measurements));
    }
    if (!decode_only) {
        print_summary("Full replay", std::move(replay_measurements));
    }
    if (!replay_only) {
        print_summary("Mmap decode", std::move(mapped_decode_measurements));
        if (!decode_only) {
            print_summary("Mmap replay", std::move(mapped_replay_measurements));
        }
    }
    if (order_reserve) {
        print_summary("Reserved replay", std::move(reserved_replay_measurements));
    }
    if (!decode_only) {
        std::cout << "Observed peak active orders: " << observed_peak_orders << '\n';
    }
    return 0;
}
