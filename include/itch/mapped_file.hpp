#pragma once

#include "itch/framing.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace itch {

class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&&) = delete;
    MappedFile& operator=(MappedFile&&) = delete;

    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
};

struct FrameViewResult {
    FrameStatus status;
    std::span<const std::uint8_t> payload;
};

class MappedFrameCursor {
public:
    explicit MappedFrameCursor(std::span<const std::uint8_t> bytes) noexcept;

    [[nodiscard]] FrameViewResult next() noexcept;

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

}  // namespace itch
