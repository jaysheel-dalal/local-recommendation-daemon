#include "lrd/common/fd.hpp"
#include "lrd/net/io.hpp"
#include "lrd/net/unix_socket.hpp"
#include "lrd/proto/codec.hpp"
#include "lrd/proto/framing.hpp"
#include "lrd/rank/item.hpp"

#include "test_harness.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>

using lrd::net::UnixStream;
using namespace lrd::proto;
namespace rank = lrd::rank;

namespace {

/// A connected pair of UnixStreams, for exercising the framing layer without a
/// listener, a path, or an accept.
struct StreamPair {
    UnixStream a;
    UnixStream b;

    static StreamPair create() {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
            throw std::runtime_error("socketpair failed");
        }
        return StreamPair{UnixStream(lrd::Fd(fds[0])), UnixStream(lrd::Fd(fds[1]))};
    }
};

/// Writes raw bytes straight to the socket, bypassing write_frame - the only
/// way to construct the malformed frames a hostile or buggy peer would send.
void write_raw(UnixStream& stream, const ByteBuffer& bytes) {
    const auto result = stream.write_all(bytes.data(), bytes.size());
    if (!result) {
        throw std::runtime_error("raw write failed");
    }
}

ByteBuffer length_prefix(std::uint32_t length) {
    ByteBuffer buffer;
    for (int shift = 24; shift >= 0; shift -= 8) {
        buffer.push_back(static_cast<std::byte>((length >> shift) & 0xFF));
    }
    return buffer;
}

/// A valid frame body whose length varies with `padding`.
///
/// Framing does not care what is inside a frame, but the *codec* does, and these
/// bodies get decoded to prove the boundaries were right - so they have to be
/// messages the codec accepts. `padding` therefore grows the number of signal
/// affinities in a Recommend rather than stretching a single field past its
/// limit: kMaxCategoryLength caps a category at 64 bytes, which an earlier
/// version of this helper cheerfully exceeded.
ByteBuffer encode_get(std::size_t padding, std::uint64_t id) {
    const BinaryCodec codec;
    Request request;
    request.request_id = id;

    if (padding == 0) {
        request.body = GetItem{id};
    } else {
        Recommend recommend;
        recommend.count = 1;
        const std::size_t affinities = std::min<std::size_t>(padding, kMaxAffinities);
        for (std::size_t i = 0; i < affinities; ++i) {
            recommend.signal.affinities.push_back(
                rank::CategoryAffinity{"category-" + std::to_string(i), 0.5});
        }
        request.body = std::move(recommend);
    }

    ByteBuffer buffer;
    codec.encode(request, buffer);
    return buffer;
}

}  // namespace

LRD_TEST("a frame round-trips over a socket") {
    StreamPair pair = StreamPair::create();
    const ByteBuffer body = encode_get(4, 1);

    LRD_REQUIRE(write_frame(pair.a, body).ok());

    ByteBuffer received;
    const FrameResult result = read_frame(pair.b, received);
    LRD_REQUIRE(result.ok());
    LRD_CHECK_EQ(result.length, static_cast<std::uint32_t>(body.size()));
    LRD_CHECK(received == body);
}

LRD_TEST("back-to-back frames keep their boundaries") {
    // The test that actually justifies the framing layer. All three frames are
    // written before any is read, so they arrive coalesced in the socket buffer
    // as one undifferentiated run of bytes. Only the length prefixes separate
    // them - a one-byte error in any prefix would corrupt every frame after it.
    StreamPair pair = StreamPair::create();

    const ByteBuffer first = encode_get(0, 1);
    const ByteBuffer second = encode_get(kMaxAffinities, 2);  // much larger frame
    const ByteBuffer third = encode_get(0, 3);

    LRD_REQUIRE(write_frame(pair.a, first).ok());
    LRD_REQUIRE(write_frame(pair.a, second).ok());
    LRD_REQUIRE(write_frame(pair.a, third).ok());

    const BinaryCodec codec;
    ByteBuffer body;

    for (const std::uint64_t expected_id : {1ULL, 2ULL, 3ULL}) {
        LRD_REQUIRE(read_frame(pair.b, body).ok());
        Request decoded;
        LRD_REQUIRE(codec.decode(body, decoded) == DecodeError::None);
        LRD_CHECK_EQ(decoded.request_id, expected_id);
    }
}

LRD_TEST("a frame at the maximum size is accepted") {
    StreamPair pair = StreamPair::create();

    // Big enough that the kernel must split it across many reads, which is
    // where read_exact earns its keep inside read_frame.
    ByteBuffer body = encode_get(kMaxAffinities, 1);
    body.resize(kMaxFrameSize);  // pad out to the cap

    std::thread writer([&] { (void)write_frame(pair.a, body); });

    ByteBuffer received;
    const FrameResult result = read_frame(pair.b, received);
    writer.join();

    LRD_REQUIRE(result.ok());
    LRD_CHECK_EQ(received.size(), std::size_t{kMaxFrameSize});
}

LRD_TEST("an oversized length prefix is rejected without allocating") {
    // Four bytes of 0xFF is a request to reserve 4 GiB. The check has to happen
    // between reading the prefix and sizing the buffer, which is exactly why
    // read_frame issues two reads rather than one.
    StreamPair pair = StreamPair::create();
    write_raw(pair.a, length_prefix(0xFFFFFFFFu));

    ByteBuffer body;
    const FrameResult result = read_frame(pair.b, body);
    LRD_CHECK(result.status == FrameStatus::Oversized);
    LRD_CHECK_EQ(result.length, 0xFFFFFFFFu);
    // Nothing was read into the body, so nothing was allocated on the peer's
    // say-so.
    LRD_CHECK(body.empty());
}

LRD_TEST("a length just over the cap is rejected") {
    StreamPair pair = StreamPair::create();
    write_raw(pair.a, length_prefix(kMaxFrameSize + 1));

    ByteBuffer body;
    const FrameResult result = read_frame(pair.b, body);
    LRD_CHECK(result.status == FrameStatus::Oversized);
}

LRD_TEST("a zero-length frame is rejected, but a short one is not") {
    // The layering fix from step 8. Framing used to reject anything shorter
    // than binary/v1's 16-byte header - a codec-specific constant in a
    // codec-agnostic layer. It stayed invisible until protobuf arrived, whose
    // PutResponse is 12 bytes and StatsRequest is 4: framing was refusing valid
    // frames as Oversized.
    //
    // Now framing only enforces "at least one byte, at most the cap", and
    // whether the contents mean anything is the codec's call.
    {
        StreamPair pair = StreamPair::create();
        write_raw(pair.a, length_prefix(0));
        ByteBuffer body;
        LRD_CHECK(read_frame(pair.b, body).status == FrameStatus::Oversized);
    }
    {
        StreamPair pair = StreamPair::create();
        // Exactly binary/v1's magic and nothing else: four bytes, valid as far
        // as they go. Chosen so the codec gets past the magic check and fails on
        // running out of bytes, which is the rejection this case is about.
        // (A body of arbitrary bytes would fail earlier, on BadMagic, and prove
        // less.)
        ByteBuffer short_body;
        short_body.push_back(std::byte{0x4C});
        short_body.push_back(std::byte{0x52});
        short_body.push_back(std::byte{0x44});
        short_body.push_back(std::byte{0x31});

        write_raw(pair.a, length_prefix(4));
        write_raw(pair.a, short_body);

        ByteBuffer body;
        const FrameResult result = read_frame(pair.b, body);
        LRD_REQUIRE(result.ok());
        LRD_CHECK_EQ(body.size(), std::size_t{4});

        // And the binary codec - not the framing layer - is what rejects it.
        const BinaryCodec codec;
        Request decoded;
        LRD_CHECK(codec.decode(body, decoded) == DecodeError::Truncated);
    }
}

LRD_TEST("a clean disconnect between frames is not an error") {
    StreamPair pair = StreamPair::create();
    pair.a.close();

    ByteBuffer body;
    const FrameResult result = read_frame(pair.b, body);
    LRD_CHECK(result.status == FrameStatus::PeerClosed);
}

LRD_TEST("a peer that dies mid-frame is reported as truncated, not closed") {
    // The distinction the daemon acts on: PeerClosed is a normal goodbye,
    // Truncated means the stream is unusable and the connection must be dropped.
    StreamPair pair = StreamPair::create();

    const ByteBuffer body = encode_get(4, 1);
    write_raw(pair.a, length_prefix(static_cast<std::uint32_t>(body.size())));
    // Send only half the promised body, then hang up.
    ByteBuffer half(body.begin(), body.begin() + static_cast<long>(body.size() / 2));
    write_raw(pair.a, half);
    pair.a.close();

    ByteBuffer received;
    const FrameResult result = read_frame(pair.b, received);
    LRD_CHECK(result.status == FrameStatus::Truncated);
}

LRD_TEST("a length prefix with no body at all is truncated") {
    StreamPair pair = StreamPair::create();
    write_raw(pair.a, length_prefix(kHeaderSize));
    pair.a.close();

    ByteBuffer body;
    const FrameResult result = read_frame(pair.b, body);
    // The peer promised a body and delivered none. Read returns a clean EOF
    // with zero bytes, but in this position that is a truncation, not a
    // goodbye - read_frame has to translate it.
    LRD_CHECK(result.status == FrameStatus::Truncated);
}

LRD_TEST("writing to a closed peer reports PeerClosed") {
    StreamPair pair = StreamPair::create();
    pair.b.close();

    ByteBuffer body = encode_get(4, 1);
    body.resize(64 * 1024);  // enough that the send buffer cannot swallow it

    const FrameResult result = write_frame(pair.a, body);
    LRD_CHECK(result.status == FrameStatus::PeerClosed);
}

LRD_TEST("write_frame refuses a body that cannot be framed") {
    StreamPair pair = StreamPair::create();

    const ByteBuffer too_small;  // empty
    LRD_CHECK(write_frame(pair.a, too_small).status == FrameStatus::Oversized);

    const ByteBuffer too_big(kMaxFrameSize + 1);
    LRD_CHECK(write_frame(pair.a, too_big).status == FrameStatus::Oversized);
}

LRD_TEST("write_message encodes and frames in one step") {
    StreamPair pair = StreamPair::create();
    const BinaryCodec codec;

    rank::Item item;
    item.id = 99;
    item.category = "tech";
    item.advertiser = "acme";
    item.base_score = 0.5;

    Response response;
    response.request_id = 99;
    response.body = GetItemResult{StatusCode::Ok, item};

    ByteBuffer scratch;
    LRD_REQUIRE(write_message(pair.a, codec, response, scratch).ok());

    ByteBuffer body;
    LRD_REQUIRE(read_frame(pair.b, body).ok());

    Response decoded;
    LRD_REQUIRE(codec.decode(body, decoded) == DecodeError::None);
    LRD_CHECK_EQ(decoded.request_id, std::uint64_t{99});
    const auto* result = std::get_if<GetItemResult>(&decoded.body);
    LRD_REQUIRE(result != nullptr);
    LRD_CHECK_EQ(result->item.category, std::string("tech"));
}
