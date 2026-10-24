#include "MultiplayerTest.hpp"

#include "px/core/Log.hpp"

#include <SDL3/SDL.h>

#include <format>

namespace px::studio {

namespace {

constexpr std::uint64_t kPlayerDelayMs = 400; // give the server a moment to open its port

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A line of a child's log ("[  1.234] INFO  text") goes to Studio's log at its level.
void forward(const std::string& label, std::string_view line) {
    if (line.empty()) return;
    log::Level level = log::Level::Info;
    std::string_view text = line;
    if (text.starts_with('[')) {
        if (const auto close = text.find(']'); close != std::string_view::npos) text.remove_prefix(close + 1);
    }
    while (text.starts_with(' ')) text.remove_prefix(1);
    const auto take = [&](std::string_view word, log::Level l) {
        if (!text.starts_with(word)) return;
        level = l;
        text.remove_prefix(word.size());
        while (text.starts_with(' ')) text.remove_prefix(1);
    };
    take("ERROR", log::Level::Error);
    take("WARN", log::Level::Warn);
    take("INFO", log::Level::Info);
    take("DEBUG", log::Level::Debug);
    if (text.starts_with("error: XDG_RUNTIME_DIR")) return; // noise from SDL on some Linux setups
    log::write(level, std::format("[{}] {}", label, text));
}

} // namespace

MultiplayerTest::~MultiplayerTest() { stop(); }

std::filesystem::path MultiplayerTest::findProgram(std::string_view name, const char* builtPath) {
#ifdef _WIN32
    const std::string file = std::string(name) + ".exe";
#else
    const std::string file(name);
#endif
    std::error_code ec;
    if (const char* base = SDL_GetBasePath()) {
        const auto beside = std::filesystem::path(base) / file;
        if (std::filesystem::is_regular_file(beside, ec)) return beside;
    }
    const std::filesystem::path built(builtPath);
    if (std::filesystem::is_regular_file(built, ec)) return built;
    return {};
}

Status MultiplayerTest::start(const MultiplayerTestOptions& options) {
    stop();
    const auto server = findProgram("px-server", PX_SERVER_PATH);
    const auto player = findProgram("px-player", PX_PLAYER_PATH);
    if (server.empty() || player.empty()) {
        return makeError(ErrorCode::NotFound,
                         "px-server and px-player must be built for multiplayer tests (build the px_server and "
                         "px_player targets, or keep them next to px-studio)");
    }
    const std::string project = utf8(options.project);
    launch("server", {utf8(server), project, "--port", std::to_string(options.port), "--restart-delay", "5"}, true);
    if (m_children.empty()) return makeError(ErrorCode::Unsupported, "Could not start px-server: {}", SDL_GetError());

    // Player windows side by side along the bottom of the main screen (so
    // Studio's menu and toolbar stay visible), further rows above.
    SDL_Rect area{0, 0, 1280, 720};
    (void)SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &area);
    const int w = options.resolution.x * options.windowScale;
    const int h = options.resolution.y * options.windowScale;
    const int gap = 24;
    const int titleBar = 40;
    const int perRow = std::max(1, (area.w - gap) / (w + gap));
    for (int i = 0; i < options.players; ++i) {
        const int x = area.x + gap + (i % perRow) * (w + gap);
        const int y = std::max(area.y + titleBar, area.y + area.h - gap - h - (i / perRow) * (h + titleBar + gap));
        const std::string name = std::format("P{}", i + 1); // short: name tags of neighbours stay readable
        m_waiting.push_back({name,
                             {utf8(player), project, "--connect", std::format("127.0.0.1:{}", options.port), "--name",
                              name, "--scale", std::to_string(options.windowScale), "--window-pos",
                              std::format("{},{}", x, y)}});
    }
    m_startedAt = SDL_GetTicks();
    log::info("Multiplayer test: starting a server on port {} and {} player window{}", options.port, options.players,
              options.players == 1 ? "" : "s");
    return {};
}

void MultiplayerTest::launch(std::string label, const std::vector<std::string>& args, bool server) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
#ifdef _WIN32
    // No console window for the server (its output goes to the Output panel).
    if (server) SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif
    SDL_Process* process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (process == nullptr) {
        log::error("Multiplayer test: could not start {}: {}", label, SDL_GetError());
        return;
    }
    Child child;
    child.label = std::move(label);
    child.process = process;
    child.server = server;
    m_children.push_back(std::move(child));
}

void MultiplayerTest::readOutput(Child& child) {
    SDL_IOStream* out = SDL_GetProcessOutput(child.process); // non-blocking
    if (out == nullptr) return;
    char buffer[4096];
    for (int rounds = 0; rounds < 16; ++rounds) {
        const std::size_t n = SDL_ReadIO(out, buffer, sizeof buffer); // non-blocking pipe
        if (n == 0) break;
        child.pending.append(buffer, n);
    }
    std::size_t start = 0;
    for (std::size_t nl = child.pending.find('\n'); nl != std::string::npos; nl = child.pending.find('\n', start)) {
        std::string_view line(child.pending.data() + start, nl - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        forward(child.label, line);
        start = nl + 1;
    }
    child.pending.erase(0, start);
}

void MultiplayerTest::update() {
    if (!m_waiting.empty() && SDL_GetTicks() - m_startedAt >= kPlayerDelayMs) {
        if (serverRunning()) {
            for (auto& [label, args] : m_waiting) launch(label, args, false);
        } else {
            log::error("Multiplayer test: the server stopped before players could join (see its message above)");
        }
        m_waiting.clear();
    }
    for (Child& c : m_children) {
        if (c.done) continue;
        readOutput(c);
        int code = 0;
        if (SDL_WaitProcess(c.process, false, &code)) {
            readOutput(c);
            if (!c.pending.empty()) forward(c.label, c.pending);
            c.pending.clear();
            c.done = true;
            if (code != 0) {
                log::warn("[{}] stopped with exit code {}", c.label, code);
            } else {
                log::info("[{}] closed", c.label);
            }
            SDL_DestroyProcess(c.process);
            c.process = nullptr;
        }
    }
    std::erase_if(m_children, [](const Child& c) { return c.done; });
}

void MultiplayerTest::stop() {
    m_waiting.clear();
    if (m_children.empty()) return;
    // Players first, then the server; each may exit cleanly before being forced.
    for (const bool server : {false, true}) {
        for (Child& c : m_children) {
            if (c.done || c.server != server) continue;
            SDL_KillProcess(c.process, false);
        }
        const std::uint64_t until = SDL_GetTicks() + 1500;
        for (Child& c : m_children) {
            if (c.done || c.server != server) continue;
            int code = 0;
            while (!SDL_WaitProcess(c.process, false, &code)) {
                readOutput(c);
                if (SDL_GetTicks() > until) {
                    SDL_KillProcess(c.process, true);
                    (void)SDL_WaitProcess(c.process, true, &code);
                    break;
                }
                SDL_Delay(10);
            }
            readOutput(c);
            SDL_DestroyProcess(c.process);
            c.process = nullptr;
            c.done = true;
        }
    }
    m_children.clear();
    log::info("Multiplayer test stopped");
}

bool MultiplayerTest::active() const { return !m_children.empty() || !m_waiting.empty(); }

bool MultiplayerTest::serverRunning() const {
    for (const Child& c : m_children) {
        if (c.server && !c.done) return true;
    }
    return false;
}

int MultiplayerTest::playersOpen() const {
    int n = 0;
    for (const Child& c : m_children) n += (!c.server && !c.done) ? 1 : 0;
    return n + static_cast<int>(m_waiting.size());
}

} // namespace px::studio
