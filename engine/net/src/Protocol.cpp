#include "px/net/Protocol.hpp"

#include "px/net/Transport.hpp"

#include <format>

namespace px::net {

namespace {

ByteWriter start(MessageType type) {
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(type));
    return w;
}

Error malformed(std::string_view what) {
    return Error{ErrorCode::InvalidData, std::format("Malformed {} message", what)};
}

// Checks the type byte and returns a reader positioned after it.
std::optional<ByteReader> open(std::span<const std::uint8_t> data, MessageType type) {
    if (data.empty() || data[0] != static_cast<std::uint8_t>(type)) return std::nullopt;
    ByteReader r(data);
    (void)r.u8();
    return r;
}

} // namespace

const char* describeReason(DisconnectReason reason) {
    switch (reason) {
        case DisconnectReason::None: return "disconnected";
        case DisconnectReason::VersionMismatch: return "the server runs a different version of the engine";
        case DisconnectReason::ContentMismatch: return "your copy of the game differs from the server's";
        case DisconnectReason::ServerFull: return "the server is full";
        case DisconnectReason::BadMessage: return "the connection sent data the other side could not read";
        case DisconnectReason::Kicked: return "removed from the game by the server";
        case DisconnectReason::ShuttingDown: return "the server shut down";
        case DisconnectReason::Timeout: return "the connection timed out";
        case DisconnectReason::Left: return "left the game";
    }
    return "disconnected";
}

std::string describeDisconnect(std::uint32_t code) {
    if (code == kReasonRefused) return "could not connect (no server there, or it is full)";
    if (code == kReasonLost) return "the connection was lost";
    if (code <= static_cast<std::uint32_t>(DisconnectReason::Left)) {
        return describeReason(static_cast<DisconnectReason>(code));
    }
    return std::format("disconnected (code {})", code);
}

std::vector<std::uint8_t> encode(const HelloMessage& m) {
    ByteWriter w = start(MessageType::Hello);
    w.u32(kProtocolMagic);
    w.u16(m.protocol);
    w.string(m.engineVersion);
    w.u64(m.contentHash);
    w.string(m.playerName);
    return std::move(w.data());
}

std::vector<std::uint8_t> encode(const InputMessage& m) {
    ByteWriter w = start(MessageType::Input);
    w.varint(m.ackEpoch);
    w.varint(m.ackTick);
    w.varint(m.lastSequence);
    w.varint(m.frames.size());
    for (const runtime::InputFrame& f : m.frames) {
        w.varint(f.held);
        w.varint(f.pressed);
    }
    return std::move(w.data());
}

std::vector<std::uint8_t> encodeGoodbye() { return std::move(start(MessageType::Goodbye).data()); }

std::vector<std::uint8_t> encode(const WelcomeMessage& m) {
    ByteWriter w = start(MessageType::Welcome);
    w.u16(m.player);
    w.u16(m.tickRate);
    w.u16(m.snapshotInterval);
    w.string(m.serverName);
    return std::move(w.data());
}

std::vector<std::uint8_t> encode(const RejectMessage& m) {
    ByteWriter w = start(MessageType::Reject);
    w.u8(static_cast<std::uint8_t>(m.reason));
    w.string(m.text);
    return std::move(w.data());
}

void writeSnapshotHeader(ByteWriter& w, const SnapshotHeader& h) {
    w.u8(static_cast<std::uint8_t>(MessageType::Snapshot));
    w.varint(h.epoch);
    w.varint(h.tick);
    w.varint(h.baselineTick);
    w.varint(h.lastInputSequence);
}

std::optional<MessageType> messageType(std::span<const std::uint8_t> data) {
    if (data.empty()) return std::nullopt;
    switch (static_cast<MessageType>(data[0])) {
        case MessageType::Hello:
        case MessageType::Input:
        case MessageType::Goodbye:
        case MessageType::Welcome:
        case MessageType::Reject:
        case MessageType::Snapshot: return static_cast<MessageType>(data[0]);
    }
    return std::nullopt;
}

Result<HelloMessage> decodeHello(std::span<const std::uint8_t> data) {
    auto r = open(data, MessageType::Hello);
    if (!r) return malformed("hello");
    HelloMessage m;
    const std::uint32_t magic = r->u32();
    m.protocol = r->u16();
    m.engineVersion = r->string(64);
    m.contentHash = r->u64();
    m.playerName = r->string(256);
    if (!r->ok() || !r->atEnd() || magic != kProtocolMagic) return malformed("hello");
    return m;
}

Result<InputMessage> decodeInput(std::span<const std::uint8_t> data) {
    auto r = open(data, MessageType::Input);
    if (!r) return malformed("input");
    InputMessage m;
    const auto u32 = [&r](std::uint64_t v) {
        if (v > 0xFFFFFFFFULL) r->fail();
        return static_cast<std::uint32_t>(v);
    };
    m.ackEpoch = u32(r->varint());
    m.ackTick = u32(r->varint());
    m.lastSequence = u32(r->varint());
    const std::size_t n = r->count(kMaxInputFramesPerMessage, 2);
    m.frames.resize(n);
    for (runtime::InputFrame& f : m.frames) {
        f.held = u32(r->varint());
        f.pressed = u32(r->varint());
    }
    if (!r->ok() || !r->atEnd() || n > m.lastSequence) return malformed("input");
    return m;
}

Result<WelcomeMessage> decodeWelcome(std::span<const std::uint8_t> data) {
    auto r = open(data, MessageType::Welcome);
    if (!r) return malformed("welcome");
    WelcomeMessage m;
    m.player = r->u16();
    m.tickRate = r->u16();
    m.snapshotInterval = r->u16();
    m.serverName = r->string(256);
    if (!r->ok() || !r->atEnd() || m.player == runtime::kNoPlayer || m.tickRate < 1 || m.tickRate > 240 ||
        m.snapshotInterval < 1 || m.snapshotInterval > 60) {
        return malformed("welcome");
    }
    return m;
}

Result<RejectMessage> decodeReject(std::span<const std::uint8_t> data) {
    auto r = open(data, MessageType::Reject);
    if (!r) return malformed("reject");
    RejectMessage m;
    m.reason = static_cast<DisconnectReason>(r->u8());
    m.text = r->string(1024);
    if (!r->ok() || !r->atEnd()) return malformed("reject");
    return m;
}

Result<SnapshotHeader> readSnapshotHeader(ByteReader& r) {
    if (r.u8() != static_cast<std::uint8_t>(MessageType::Snapshot)) return malformed("snapshot");
    SnapshotHeader h;
    const auto u32 = [&r](std::uint64_t v) {
        if (v > 0xFFFFFFFFULL) r.fail();
        return static_cast<std::uint32_t>(v);
    };
    h.epoch = u32(r.varint());
    h.tick = u32(r.varint());
    h.baselineTick = u32(r.varint());
    h.lastInputSequence = u32(r.varint());
    if (!r.ok() || h.tick == 0 || h.baselineTick >= h.tick) return malformed("snapshot");
    return h;
}

std::string sanitizePlayerName(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (out.size() >= runtime::kMaxPlayerNameLength) break;
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f) out += c;
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

} // namespace px::net
