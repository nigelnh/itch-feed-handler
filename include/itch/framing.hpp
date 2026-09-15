#pragma once

#include <cstdint>
#include <istream>
#include <string_view>
#include <vector>

namespace itch {

enum class FrameStatus : std::uint8_t {
    Message,
    EndOfSession,
    MissingEndOfSession,
    TruncatedLength,
    TruncatedPayload,
    IoError,
};

struct FrameResult {
    FrameStatus status;
    std::vector<std::uint8_t> payload;
};

class BinaryFileReader {
public:
    explicit BinaryFileReader(std::istream& input) noexcept;

    [[nodiscard]] FrameResult next();

private:
    std::istream& input_;
};

[[nodiscard]] std::string_view to_string(FrameStatus status) noexcept;

}  // namespace itch
