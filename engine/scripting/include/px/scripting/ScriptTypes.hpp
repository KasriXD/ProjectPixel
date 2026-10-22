#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace px::scripting {

// Where a script VM runs. Determines which API functions exist in it.
//
// Authority: the simulation that decides game state: a single-player
//            session today, the dedicated server in multiplayer.
// Client:    a player's machine in multiplayer (planned, Milestone 4):
//            presentation and prediction only. Functions that change
//            authoritative state are not registered at all in client VMs.
enum class Realm : std::uint8_t { Authority, Client };

[[nodiscard]] const char* realmName(Realm realm) noexcept;

// Resource limits for one script VM. Exceeding a limit raises a script
// error in the offending callback; the game keeps running.
struct ScriptLimits {
    // Total memory of the VM (all scripts of a session together).
    std::size_t memoryBytes = 64u * 1024u * 1024u;
    // Safepoints (loop iterations and function calls) one callback may run.
    // Deterministic: the same script hits the limit at the same point everywhere.
    std::uint64_t stepsPerCallback = 5'000'000;
    // Wall-clock watchdog per callback, for code that is slow without
    // looping (e.g. huge string operations). Not deterministic; a backstop.
    double secondsPerCallback = 0.5;
    // Custom events delivered per tick (stops endless event ping-pong).
    int maxEventDeliveriesPerTick = 10'000;
    // print()/warn() lines accepted per tick, and characters per line.
    int maxLogLinesPerTick = 100;
    std::size_t maxLogLineLength = 1000;
    // Size limit for a single script source file.
    std::size_t maxSourceBytes = 1024u * 1024u;
};

// A script failure, reported to the host and shown to creators.
struct ScriptError {
    std::string script;    // script asset name, e.g. "scripts/coin.luau"
    std::string entity;    // entity name, or empty for module-level errors
    std::string phase;     // "load", "Start", "Tick", "TriggerEnter", event name...
    std::string message;   // "scripts/coin.luau:12: attempt to index nil with 'x'"
    std::string traceback; // Luau stack trace, may be empty

    [[nodiscard]] std::string describe() const;
};

} // namespace px::scripting
