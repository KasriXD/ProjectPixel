// px-player entry point. Uses SDL's main-callback model rather than a
// blocking loop: the same structure works on desktop, mobile and in the
// browser (Emscripten), where the platform owns the main loop.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "PlayerApp.hpp"

#include "px/core/Log.hpp"

#include <cstdio>

namespace {

SDL_AppResult toSdl(px::player::PlayerApp::Status status) {
    switch (status) {
        case px::player::PlayerApp::Status::Continue: return SDL_APP_CONTINUE;
        case px::player::PlayerApp::Status::Success: return SDL_APP_SUCCESS;
        case px::player::PlayerApp::Status::Failure: return SDL_APP_FAILURE;
    }
    return SDL_APP_FAILURE;
}

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    std::string error;
    auto options = px::player::parsePlayerArgs(argc, argv, error);
    if (!options) {
        if (!error.empty()) std::fprintf(stderr, "px-player: %s\n\n", error.c_str());
        std::fputs(px::player::playerUsage(), stderr);
        return error.empty() ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    auto app = px::player::PlayerApp::create(std::move(*options));
    if (!app) {
        px::log::error("{}", app.error().message);
        return SDL_APP_FAILURE;
    }
    *appstate = app.value().release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    return toSdl(static_cast<px::player::PlayerApp*>(appstate)->iterate());
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    return toSdl(static_cast<px::player::PlayerApp*>(appstate)->handleEvent(*event));
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    delete static_cast<px::player::PlayerApp*>(appstate);
}
