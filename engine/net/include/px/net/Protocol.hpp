#pragma once

#include "px/core/Result.hpp"
#include "px/net/Bytes.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/runtime/Input.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The game protocol: the messages a client and a dedicated server exchange
// (docs/multiplayer.md#protocol). Clients send only who they are and their
// input; the server sends everything else. Every message starts with a type
// byte; decoders check every field and reject anything malformed.
namespace px::net {

inline constexpr std::uint32_t kProtocolMagic = 0x31584E50; // "PNX1" in memory order
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint16_t kDefaultPort = 7777;

// Clients only ever send small messages; anything larger is refused.
inline constexpr std::size_t kMaxClientMessageBytes = 2048;
inline constexpr std::size_t kMaxServerMessageBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxInputFramesPerMessage = 32;

enum class MessageType : std::uint8_t {
    // client -> server
    Hello = 1,   // reliable: who I am, which game I have
    Input = 2,   // unreliable: my recent input frames, the newest snapshot I have
    Goodbye = 3, // reliable: I am leaving
    // server -> client
    Welcome = 16,  // reliable: you are player N
    Reject = 17,   // reliable, then disconnect: why you cannot play
    Snapshot = 18, // unreliable: the world, as a change from a state you have
};

// Why the server ended (or refused) a connection. Also the transport's
// disconnect reason code.
enum class DisconnectReason : std::uint8_t {
    None = 0,
    VersionMismatch = 1, // different engine or protocol version
    ContentMismatch = 2, // different copy of the game
    ServerFull = 3,
    BadMessage = 4,      // malformed or unexpected message
    Kicked = 5,
    ShuttingDown = 6,
    Timeout = 7,         // no Hello in time
    Left = 8,            // the client said goodbye
};
[[nodiscard]] const char* describeReason(DisconnectReason reason);
// Text for a transport disconnect code (game reasons or kReasonRefused/kReasonLost).
[[nodiscard]] std::string describeDisconnect(std::uint32_t code);

struct HelloMessage {
    std::uint16_t protocol = kProtocolVersion;
    std::string engineVersion;
    std::uint64_t contentHash = 0; // see contentHash() in Replication.hpp
    std::string playerName;
};

// The client's newest input frames: frames[i] has sequence number
// lastSequence - frames.size() + 1 + i. Sent every tick; repeating the
// frames the server has not acknowledged makes single losses harmless.
struct InputMessage {
    std::uint32_t ackEpoch = 0;
    std::uint32_t ackTick = 0; // newest snapshot received (0: none yet)
    std::uint32_t lastSequence = 0;
    std::vector<runtime::InputFrame> frames;
};

struct WelcomeMessage {
    runtime::PlayerId player = runtime::kNoPlayer;
    std::uint16_t tickRate = 60;
    std::uint16_t snapshotInterval = 2; // ticks between snapshots
    std::string serverName;
};

struct RejectMessage {
    DisconnectReason reason = DisconnectReason::None;
    std::string text;
};

// The snapshot body (the world change) is encoded by Replication.hpp.
struct SnapshotHeader {
    std::uint32_t epoch = 0;        // increases when the server restarts the level
    std::uint32_t tick = 0;         // server tick the state is from (>= 1)
    std::uint32_t baselineTick = 0; // the state this is a change from (0: the initial scene)
    std::uint32_t lastInputSequence = 0; // newest input of yours the server has used
};

[[nodiscard]] std::vector<std::uint8_t> encode(const HelloMessage& m);
[[nodiscard]] std::vector<std::uint8_t> encode(const InputMessage& m);
[[nodiscard]] std::vector<std::uint8_t> encodeGoodbye();
[[nodiscard]] std::vector<std::uint8_t> encode(const WelcomeMessage& m);
[[nodiscard]] std::vector<std::uint8_t> encode(const RejectMessage& m);
// Writes the type byte and header; the caller appends the body.
void writeSnapshotHeader(ByteWriter& w, const SnapshotHeader& h);

// The type of a message, or nullopt for an empty or unknown one.
[[nodiscard]] std::optional<MessageType> messageType(std::span<const std::uint8_t> data);

[[nodiscard]] Result<HelloMessage> decodeHello(std::span<const std::uint8_t> data);
[[nodiscard]] Result<InputMessage> decodeInput(std::span<const std::uint8_t> data);
[[nodiscard]] Result<WelcomeMessage> decodeWelcome(std::span<const std::uint8_t> data);
[[nodiscard]] Result<RejectMessage> decodeReject(std::span<const std::uint8_t> data);
// Reads the type byte and header; `reader` is left at the body.
[[nodiscard]] Result<SnapshotHeader> readSnapshotHeader(ByteReader& reader);

// Player names: printable ASCII (the in-game font), trimmed, at most
// runtime::kMaxPlayerNameLength characters. Empty if nothing is left.
[[nodiscard]] std::string sanitizePlayerName(std::string_view name);

} // namespace px::net
