#include "itch/framing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace itch {

BinaryFileReader::BinaryFileReader(std::istream& input) noexcept : input_(input) {}

FrameResult BinaryFileReader::next() {
    std::array<char, 2> length_bytes{};
    input_.read(length_bytes.data(), static_cast<std::streamsize>(length_bytes.size()));
    const std::streamsize length_bytes_read = input_.gcount();

    if (length_bytes_read == 0) {
        return FrameResult{
            input_.bad() ? FrameStatus::IoError : FrameStatus::MissingEndOfSession,
            {},
        };
    }
    if (length_bytes_read != static_cast<std::streamsize>(length_bytes.size())) {
        return FrameResult{FrameStatus::TruncatedLength, {}};
    }

    const auto high = static_cast<std::uint16_t>(
        static_cast<unsigned char>(length_bytes[0]));
    const auto low = static_cast<std::uint16_t>(
        static_cast<unsigned char>(length_bytes[1]));
    const auto payload_size = static_cast<std::uint16_t>((high << 8U) | low);

    if (payload_size == 0) {
        return FrameResult{FrameStatus::EndOfSession, {}};
    }

    std::vector<std::uint8_t> payload(payload_size);
    input_.read(
        reinterpret_cast<char*>(payload.data()),
        static_cast<std::streamsize>(payload.size()));
    if (input_.gcount() != static_cast<std::streamsize>(payload.size())) {
        return FrameResult{
            input_.bad() ? FrameStatus::IoError : FrameStatus::TruncatedPayload,
            {},
        };
    }

    return FrameResult{FrameStatus::Message, std::move(payload)};
}

std::string_view to_string(FrameStatus status) noexcept {
    switch (status) {
        case FrameStatus::Message:
            return "message";
        case FrameStatus::EndOfSession:
            return "end of session";
        case FrameStatus::MissingEndOfSession:
            return "file ended without the required zero-length session terminator";
        case FrameStatus::TruncatedLength:
            return "truncated two-byte message length";
        case FrameStatus::TruncatedPayload:
            return "truncated message payload";
        case FrameStatus::IoError:
            return "input/output error";
    }
    return "unknown framing status";
}

}  // namespace itch
