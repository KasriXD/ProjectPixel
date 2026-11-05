// px-player entry point. Uses SDL's main-callback model rather than a
// blocking loop: the same structure works on desktop, mobile and in the
// browser (Emscripten), where the platform owns the main loop.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "PlayerApp.hpp"
#if defined(PX_HAVE_LAUNCHER)
#include "Launcher.hpp"
#endif

#include "px/core/Log.hpp"

#include <cstdio>
#include <memory>

namespace {

using px::player::PlayerApp;

SDL_AppResult toSdl(PlayerApp::Status status) {
    switch (status) {
        case PlayerApp::Status::Continue: return SDL_APP_CONTINUE;
        case PlayerApp::Status::Success: return SDL_APP_SUCCESS;
        case PlayerApp::Status::Failure: return SDL_APP_FAILURE;
    }
    return SDL_APP_FAILURE;
}

// What runs: a game, or the launcher and the game it started (one window,
// passed between them).
struct Shell {
    std::unique_ptr<PlayerApp> game;
#if defined(PX_HAVE_LAUNCHER)
    std::unique_ptr<px::player::Launcher> launcher;

    // The game ended: back to the launcher, with the window.
    void backToLauncher(PlayerApp::Status status) {
        const std::string message = game->endMessage();
        launcher->resume(game->releaseWindow(), message, status == PlayerApp::Status::Failure);
        game.reset();
    }

    SDL_AppResult iterateLauncher() {
        switch (launcher->iterate()) {
            case px::player::Launcher::Status::Continue: return SDL_APP_CONTINUE;
            case px::player::Launcher::Status::Quit: return SDL_APP_SUCCESS;
            case px::player::Launcher::Status::Failed: return SDL_APP_FAILURE;
            case px::player::Launcher::Status::Play: break;
        }
        auto window = launcher->takeWindow();
        auto app = PlayerApp::create(launcher->takeGame(), &window);
        if (!app) {
            px::log::warn("{}", app.error().message);
            launcher->resume(std::move(window), app.error().message, true);
            return SDL_APP_CONTINUE;
        }
        game = std::move(app.value());
        return SDL_APP_CONTINUE;
    }

    ~Shell() {
        // The launcher's user interface closes in its window.
        if (game && launcher) launcher->resume(game->releaseWindow(), {}, false);
    }
#endif
};

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
#if defined(SDL_PLATFORM_ANDROID)
    // Android has no console: the log goes where SDL's does (adb logcat).
    px::log::setConsoleOutput(false);
    px::log::addSink([](px::log::Level level, std::string_view message) {
        const SDL_LogPriority priority = level == px::log::Level::Error  ? SDL_LOG_PRIORITY_ERROR
                                         : level == px::log::Level::Warn ? SDL_LOG_PRIORITY_WARN
                                                                         : SDL_LOG_PRIORITY_INFO;
        SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority, "%.*s", static_cast<int>(message.size()),
                       message.data());
    });
#endif
    std::string error;
    auto options = px::player::parsePlayerArgs(argc, argv, error);
    if (!options) {
        if (!error.empty()) std::fprintf(stderr, "px-player: %s\n\n", error.c_str());
        std::fputs(px::player::playerUsage(), stderr);
        return error.empty() ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    auto shell = std::make_unique<Shell>();
#if defined(PX_HAVE_LAUNCHER)
    if (px::player::wantsLauncher(*options)) {
        auto launcher = px::player::Launcher::create(*options);
        if (!launcher) {
            px::log::error("{}", launcher.error().message);
            return SDL_APP_FAILURE;
        }
        shell->launcher = std::move(launcher.value());
        *appstate = shell.release();
        return SDL_APP_CONTINUE;
    }
#endif
    auto app = PlayerApp::create(std::move(*options));
    if (!app) {
        px::log::error("{}", app.error().message);
        return SDL_APP_FAILURE;
    }
    shell->game = std::move(app.value());
    *appstate = shell.release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto& shell = *static_cast<Shell*>(appstate);
#if defined(PX_HAVE_LAUNCHER)
    if (shell.launcher) {
        if (!shell.game) return shell.iterateLauncher();
        const PlayerApp::Status status = shell.game->iterate();
        if (status != PlayerApp::Status::Continue) shell.backToLauncher(status);
        return SDL_APP_CONTINUE;
    }
#endif
    return toSdl(shell.game->iterate());
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto& shell = *static_cast<Shell*>(appstate);
#if defined(PX_HAVE_LAUNCHER)
    if (shell.launcher) {
        // Closing the window leaves the app, also from a game.
        if (event->type == SDL_EVENT_QUIT) return SDL_APP_SUCCESS;
        if (!shell.game) {
            return shell.launcher->handleEvent(*event) == px::player::Launcher::Status::Quit ? SDL_APP_SUCCESS
                                                                                             : SDL_APP_CONTINUE;
        }
        const PlayerApp::Status status = shell.game->handleEvent(*event);
        if (status != PlayerApp::Status::Continue) shell.backToLauncher(status);
        return SDL_APP_CONTINUE;
    }
#endif
    return toSdl(shell.game->handleEvent(*event));
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    delete static_cast<Shell*>(appstate);
}
