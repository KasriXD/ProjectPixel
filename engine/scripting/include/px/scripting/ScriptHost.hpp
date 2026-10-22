#pragma once

#include "px/core/Log.hpp"
#include "px/scene/Registry.hpp"
#include "px/scripting/ScriptTypes.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace px::scripting {

// Services the game runtime provides to scripts. The scripting module has
// no dependency on the runtime; GameSession implements this interface.
// Every script API call that touches game state goes through here, which is
// what lets the same scripts run in a local session or on a server.
class IScriptHost {
public:
    virtual ~IScriptHost() = default;

    // --- Input actions for this tick ----------------------------------------
    // Index of the action, or -1 if the project does not define it.
    [[nodiscard]] virtual int findAction(std::string_view name) const = 0;
    [[nodiscard]] virtual std::vector<std::string> actionNames() const = 0;
    [[nodiscard]] virtual bool actionHeld(int action) const = 0;
    [[nodiscard]] virtual bool actionPressed(int action) const = 0;

    // --- Authoritative game state (authority realm only) ----------------------
    virtual void addScore(int amount) = 0;
    [[nodiscard]] virtual int score() const = 0;
    virtual void setObjective(std::string text) = 0;
    virtual void completeLevel(std::string message) = 0;

    // --- Simulation time -------------------------------------------------------
    // Seconds of simulated time at the start of the current tick.
    [[nodiscard]] virtual double timeSeconds() const = 0;
    [[nodiscard]] virtual std::uint64_t tickCount() const = 0;

    // --- Entities --------------------------------------------------------------
    // Destruction is deferred until the current phase ends.
    virtual void requestDestroy(scene::Entity entity) = 0;

    // --- Reporting --------------------------------------------------------------
    virtual void scriptLog(log::Level level, std::string_view script, std::string_view message) = 0;
    virtual void scriptError(const ScriptError& error) = 0;
    // A custom event emitted by a script (for tools and logs).
    virtual void scriptEvent(std::string_view /*name*/, std::string_view /*valueText*/) {}
};

} // namespace px::scripting
