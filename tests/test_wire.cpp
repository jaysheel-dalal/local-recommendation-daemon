#include "lrd/proto/wire.hpp"

#include "test_harness.hpp"

#include <cstdint>
#include <string>

using lrd::proto::ByteBuffer;
using lrd::proto::ByteReader;
using lrd::proto::ByteView;
using lrd::proto::ByteWriter;

namespace {

/// Renders a buffer as hex, so a byte-order failure prints something readable
/// instead of a wall of unprintable characters.
std::string hex(const ByteBuffer& buffer) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(buffer.size() * 2);
    for (const std::byte b : buffer) {
        const auto value = static_cast<unsigned>(b);
        out.push_back(kDigits[value >> 4]);
        out.push_back(kDigits[value & 0x0F]);
    }
    return out;
}

ByteView view(const ByteBuffer& buffer) {
    return ByteView(buffer);
}

}  // namespace

LRD_TEST("integers are written big-endian") {
    // The single most important assertion in this file. If this silently
    // became little-endian, every round-trip test would still pass - the bug
    // only appears when talking to something that is not this build.
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(0x01020304);
    LRD_CHECK_EQ(hex(buffer), std::string("01020304"));

    buffer.clear();
    writer.u16(0xABCD);
    LRD_CHECK_EQ(hex(buffer), std::string("abcd"));

    buffer.clear();
    writer.u64(0x0102030405060708ULL);
    LRD_CHECK_EQ(hex(buffer), std::string("0102030405060708"));
}

LRD_TEST("integers round-trip through writer and reader") {
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u8(0x7F);
    writer.u16(0xBEEF);
    writer.u32(0xDEADBEEF);
    writer.u64(0xFEEDFACECAFEBEEFULL);

    ByteReader reader(view(buffer));
    LRD_CHECK_EQ(static_cast<unsigned>(reader.u8()), 0x7Fu);
    LRD_CHECK_EQ(static_cast<unsigned>(reader.u16()), 0xBEEFu);
    LRD_CHECK_EQ(reader.u32(), 0xDEADBEEFu);
    LRD_CHECK_EQ(reader.u64(), 0xFEEDFACECAFEBEEFULL);
    LRD_CHECK(!reader.failed());
    LRD_CHECK(reader.exhausted());
}

LRD_TEST("extreme integer values survive the round trip") {
    // Catches sign-extension bugs: a value with the top bit set is the one that
    // breaks if an intermediate step uses a signed type.
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u64(0xFFFFFFFFFFFFFFFFULL);
    writer.u32(0xFFFFFFFFu);
    writer.u64(0);

    ByteReader reader(view(buffer));
    LRD_CHECK_EQ(reader.u64(), 0xFFFFFFFFFFFFFFFFULL);
    LRD_CHECK_EQ(reader.u32(), 0xFFFFFFFFu);
    LRD_CHECK_EQ(reader.u64(), 0ULL);
    LRD_CHECK(!reader.failed());
}

LRD_TEST("strings round-trip, including empty and binary content") {
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.string("hello");
    writer.string("");
    // Embedded NULs are the reason the format is length-prefixed rather than
    // NUL-terminated. A C-string encoding would truncate this to one byte.
    writer.string(std::string("a\0b\0c", 5));

    ByteReader reader(view(buffer));
    LRD_CHECK_EQ(reader.string(), std::string("hello"));
    LRD_CHECK_EQ(reader.string(), std::string(""));
    LRD_CHECK_EQ(reader.string(), std::string("a\0b\0c", 5));
    LRD_CHECK(!reader.failed());
    LRD_CHECK(reader.exhausted());
}

LRD_TEST("reading past the end sets the failure flag") {
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u16(0x1234);

    ByteReader reader(view(buffer));
    LRD_CHECK_EQ(static_cast<unsigned>(reader.u16()), 0x1234u);
    LRD_CHECK(!reader.failed());

    const std::uint32_t overrun = reader.u32();
    LRD_CHECK(reader.failed());
    // A failed read must return a default, not whatever happened to be nearby.
    LRD_CHECK_EQ(overrun, 0u);
}

LRD_TEST("failure is sticky") {
    ByteBuffer buffer;  // empty
    ByteReader reader(view(buffer));

    LRD_CHECK_EQ(reader.u32(), 0u);
    LRD_REQUIRE(reader.failed());

    // Once broken, every subsequent read must stay broken and stay safe. This
    // is what lets a decoder run straight through and check failed() once at
    // the end rather than after every field.
    LRD_CHECK_EQ(reader.u64(), 0ULL);
    LRD_CHECK_EQ(reader.string(), std::string(""));
    LRD_CHECK(reader.failed());
}

LRD_TEST("a string length larger than the buffer is rejected before allocating") {
    // The denial-of-service case, hand-built: a 4-byte length prefix claiming
    // 4 GiB, followed by nothing. A decoder that trusts the prefix would try to
    // allocate 4 GiB on a remote peer's say-so.
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(0xFFFFFFFFu);  // claimed string length
    writer.u8(0x41);          // one solitary byte of actual content

    ByteReader reader(view(buffer));
    const std::string value = reader.string();

    LRD_CHECK(reader.failed());
    LRD_CHECK(value.empty());
}

LRD_TEST("a string length just past the end is rejected") {
    // The off-by-one version of the same check, which a naive `length > 1MB`
    // guard would let through.
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(6);
    writer.bytes(ByteView(reinterpret_cast<const std::byte*>("hello"), 5));

    ByteReader reader(view(buffer));
    LRD_CHECK(reader.string().empty());
    LRD_CHECK(reader.failed());
}

LRD_TEST("remaining and exhausted track consumption") {
    ByteBuffer buffer;
    ByteWriter writer(buffer);
    writer.u32(1);
    writer.u32(2);

    ByteReader reader(view(buffer));
    LRD_CHECK_EQ(reader.remaining(), std::size_t{8});
    (void)reader.u32();
    LRD_CHECK_EQ(reader.remaining(), std::size_t{4});
    LRD_CHECK(!reader.exhausted());
    (void)reader.u32();
    LRD_CHECK(reader.exhausted());
}
