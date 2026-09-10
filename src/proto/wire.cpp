#include "lrd/proto/wire.hpp"

#include <cstring>

namespace lrd::proto {

namespace {

// Explicit shifts rather than htonl/memcpy of a native integer.
//
// htonl only covers 32 bits - the 64-bit spelling (htobe64) is a BSD/glibc
// extension rather than standard C or C++. And the "fast" alternative,
// *reinterpret_cast<const std::uint32_t*>(ptr), is undefined behaviour when ptr
// is not suitably aligned; a pointer into the middle of a frame usually is not,
// and on some architectures that is a hardware fault rather than a slow path.
//
// Shifts are portable, alignment-agnostic, and every compiler worth using folds
// them into a single bswap instruction. This is one of the rare cases where the
// obviously-correct version is also the fast one.
template <typename T>
void append_be(ByteBuffer& out, T value) {
    constexpr std::size_t width = sizeof(T);
    for (std::size_t i = 0; i < width; ++i) {
        const unsigned shift = static_cast<unsigned>((width - 1 - i) * 8);
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

template <typename T>
T load_be(ByteView data, std::size_t pos) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<T>((value << 8) | static_cast<T>(data[pos + i]));
    }
    return value;
}

}  // namespace

// --------------------------------------------------------------------------
// ByteWriter
// --------------------------------------------------------------------------

void ByteWriter::u8(std::uint8_t value) {
    out_.push_back(static_cast<std::byte>(value));
}

void ByteWriter::u16(std::uint16_t value) {
    append_be(out_, value);
}

void ByteWriter::u32(std::uint32_t value) {
    append_be(out_, value);
}

void ByteWriter::u64(std::uint64_t value) {
    append_be(out_, value);
}

void ByteWriter::bytes(ByteView value) {
    out_.insert(out_.end(), value.begin(), value.end());
}

void ByteWriter::string(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    out_.insert(out_.end(), first, first + value.size());
}

// --------------------------------------------------------------------------
// ByteReader
// --------------------------------------------------------------------------

bool ByteReader::need(std::size_t count) noexcept {
    if (failed_) {
        return false;  // sticky: once broken, stay broken
    }
    if (remaining() < count) {
        failed_ = true;
        return false;
    }
    return true;
}

std::uint8_t ByteReader::u8() noexcept {
    if (!need(1)) {
        return 0;
    }
    const auto value = static_cast<std::uint8_t>(data_[pos_]);
    pos_ += 1;
    return value;
}

std::uint16_t ByteReader::u16() noexcept {
    if (!need(2)) {
        return 0;
    }
    const auto value = load_be<std::uint16_t>(data_, pos_);
    pos_ += 2;
    return value;
}

std::uint32_t ByteReader::u32() noexcept {
    if (!need(4)) {
        return 0;
    }
    const auto value = load_be<std::uint32_t>(data_, pos_);
    pos_ += 4;
    return value;
}

std::uint64_t ByteReader::u64() noexcept {
    if (!need(8)) {
        return 0;
    }
    const auto value = load_be<std::uint64_t>(data_, pos_);
    pos_ += 8;
    return value;
}

ByteView ByteReader::bytes(std::size_t count) noexcept {
    if (!need(count)) {
        return {};
    }
    const ByteView view = data_.subspan(pos_, count);
    pos_ += count;
    return view;
}

std::string ByteReader::string() {
    const std::uint32_t length = u32();
    if (failed_) {
        return {};
    }

    // The check that matters. `length` is a number a remote peer chose, and the
    // next line would otherwise size a std::string to it. Validating against
    // what is actually left in the frame means a hostile length can never
    // allocate more than the frame we already received.
    if (!need(length)) {
        return {};
    }

    const auto* first = reinterpret_cast<const char*>(data_.data() + pos_);
    pos_ += length;
    return std::string(first, length);
}

}  // namespace lrd::proto
