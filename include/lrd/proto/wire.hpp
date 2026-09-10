#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lrd::proto {

/// std::byte rather than char or unsigned char: it is the type that says "raw
/// memory, not text and not a number". It has no arithmetic operators, so a
/// stray `buffer[i] + 1` is a compile error rather than a silent sign-extension
/// bug - which is exactly the discipline you want in wire-format code.
using ByteBuffer = std::vector<std::byte>;

/// A non-owning view of bytes. std::span is C++20's answer to passing
/// (pointer, length) pairs around: it keeps them together, carries no
/// ownership, and costs nothing at runtime. The `const` is in the element type,
/// so a ByteView cannot be used to modify what it points at.
using ByteView = std::span<const std::byte>;

/// Appends big-endian integers and length-prefixed strings to a buffer.
///
/// Big-endian because it is the network byte order convention, and because a
/// hexdump of a frame then reads left to right in the order the fields are
/// written - which matters more than it sounds when debugging a protocol.
class ByteWriter {
public:
    explicit ByteWriter(ByteBuffer& out) noexcept : out_(out) {}

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);

    /// Raw bytes, with no length prefix.
    void bytes(ByteView value);

    /// uint32 length prefix followed by the bytes.
    void string(std::string_view value);

    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }

private:
    ByteBuffer& out_;
};

/// Reads big-endian integers and length-prefixed strings out of a buffer.
///
/// Failure is *sticky*: the first read that runs out of input sets a flag, and
/// every subsequent read returns a zero value without touching memory. That
/// lets a decoder be written as straight-line code with a single check at the
/// end, instead of an `if` after every field. std::istream works the same way,
/// for the same reason.
///
/// The pattern only stays safe because failed reads return defaults rather than
/// garbage - a decoder that ignores failed() gets zeros and empty strings, not
/// a buffer overrun.
class ByteReader {
public:
    explicit ByteReader(ByteView in) noexcept : data_(in) {}

    [[nodiscard]] std::uint8_t u8() noexcept;
    [[nodiscard]] std::uint16_t u16() noexcept;
    [[nodiscard]] std::uint32_t u32() noexcept;
    [[nodiscard]] std::uint64_t u64() noexcept;

    /// Reads a uint32-prefixed string.
    ///
    /// The length is checked against the bytes actually remaining *before* the
    /// string is allocated. Skipping that check is the classic remote
    /// denial-of-service: a 40-byte frame whose string claims 900 KB would
    /// otherwise cause a 900 KB allocation on the daemon's behalf.
    [[nodiscard]] std::string string();

    /// Reads exactly `count` raw bytes, or fails.
    [[nodiscard]] ByteView bytes(std::size_t count) noexcept;

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] bool exhausted() const noexcept { return remaining() == 0; }

private:
    /// Returns true if `count` bytes are available, otherwise sets the sticky
    /// failure flag and returns false.
    bool need(std::size_t count) noexcept;

    ByteView data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

}  // namespace lrd::proto
