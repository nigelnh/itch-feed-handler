#include "itch/mapped_file.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itch {

namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}

    ~FileDescriptor() {
        if (value_ >= 0) {
            static_cast<void>(::close(value_));
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept {
        return value_;
    }

private:
    int value_;
};

}  // namespace

MappedFile::MappedFile(const std::filesystem::path& path) {
    const FileDescriptor descriptor(::open(path.c_str(), O_RDONLY));
    if (descriptor.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path.string());
    }

    struct stat file_status {};
    if (::fstat(descriptor.get(), &file_status) != 0) {
        throw std::system_error(errno, std::generic_category(), "fstat " + path.string());
    }
    if (file_status.st_size < 0
        || static_cast<std::uintmax_t>(file_status.st_size)
            > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("file is too large to map: " + path.string());
    }

    size_ = static_cast<std::size_t>(file_status.st_size);
    if (size_ == 0) {
        return;
    }

    void* mapping = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor.get(), 0);
    if (mapping == MAP_FAILED) {
        size_ = 0;
        throw std::system_error(errno, std::generic_category(), "mmap " + path.string());
    }
    data_ = static_cast<const std::uint8_t*>(mapping);
}

MappedFile::~MappedFile() {
    if (data_ != nullptr) {
        static_cast<void>(::munmap(const_cast<std::uint8_t*>(data_), size_));
    }
}

std::span<const std::uint8_t> MappedFile::bytes() const noexcept {
    return {data_, size_};
}

MappedFrameCursor::MappedFrameCursor(std::span<const std::uint8_t> bytes) noexcept
    : bytes_(bytes) {}

FrameViewResult MappedFrameCursor::next() noexcept {
    const std::size_t remaining = bytes_.size() - offset_;
    if (remaining == 0) {
        return FrameViewResult{FrameStatus::MissingEndOfSession, {}};
    }
    if (remaining == 1) {
        return FrameViewResult{FrameStatus::TruncatedLength, {}};
    }

    const auto high = static_cast<std::uint16_t>(bytes_[offset_]);
    const auto low = static_cast<std::uint16_t>(bytes_[offset_ + 1]);
    const auto payload_size = static_cast<std::uint16_t>((high << 8U) | low);
    offset_ += 2;

    if (payload_size == 0) {
        return FrameViewResult{FrameStatus::EndOfSession, {}};
    }
    if (bytes_.size() - offset_ < payload_size) {
        return FrameViewResult{FrameStatus::TruncatedPayload, {}};
    }

    const auto payload = bytes_.subspan(offset_, payload_size);
    offset_ += payload_size;
    return FrameViewResult{FrameStatus::Message, payload};
}

}  // namespace itch
