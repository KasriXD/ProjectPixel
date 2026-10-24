#pragma once

#include "px/core/Math.hpp"
#include "px/core/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Process;

namespace px::studio {

struct MultiplayerTestOptions {
    std::filesystem::path project; // saved project folder
    int players = 2;
    std::uint16_t port = 7777;
    Vec2i resolution{320, 180};    // the game's virtual resolution, to tile the windows
    int windowScale = 2;
};

// "Test Multiplayer" in Studio: starts px-server and several px-player
// windows that join it on this computer, shows their output in the Output
// panel, and stops them all again (docs/studio.md#testing-multiplayer).
class MultiplayerTest {
public:
    MultiplayerTest() = default;
    ~MultiplayerTest();
    MultiplayerTest(const MultiplayerTest&) = delete;
    MultiplayerTest& operator=(const MultiplayerTest&) = delete;

    [[nodiscard]] Status start(const MultiplayerTestOptions& options);
    void stop();
    // Reads output, starts the player windows once the server had a moment,
    // notices programs that ended. Call every frame.
    void update();

    // Anything still open (server or player windows).
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool serverRunning() const;
    [[nodiscard]] int playersOpen() const;
    // Next to Studio (a package), else where the build put it.
    [[nodiscard]] static std::filesystem::path findProgram(std::string_view name, const char* builtPath);

private:
    struct Child {
        std::string label;
        SDL_Process* process = nullptr;
        std::string pending; // output not yet ending in a newline
        bool server = false;
        bool done = false;
    };
    void launch(std::string label, const std::vector<std::string>& args, bool server);
    void readOutput(Child& child);

    std::vector<Child> m_children;
    std::vector<std::pair<std::string, std::vector<std::string>>> m_waiting; // players to start
    std::uint64_t m_startedAt = 0;
};

} // namespace px::studio
