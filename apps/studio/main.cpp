// px-studio: the desktop editor. Uses SDL's main callbacks like px-player.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "StudioApp.hpp"

#include "px/core/Log.hpp"

#include <cstdio>

namespace {

SDL_AppResult toSdl(px::studio::StudioApp::Status status) {
    switch (status) {
        case px::studio::StudioApp::Status::Continue: return SDL_APP_CONTINUE;
        case px::studio::StudioApp::Status::Success: return SDL_APP_SUCCESS;
        case px::studio::StudioApp::Status::Failure: return SDL_APP_FAILURE;
    }
    return SDL_APP_FAILURE;
}

} // namespace

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[]) {
    std::string error;
    auto options = px::studio::parseStudioArgs(argc, argv, error);
    if (!options) {
        if (!error.empty()) std::fprintf(stderr, "px-studio: %s\n\n", error.c_str());
        std::fputs(px::studio::studioUsage(), stderr);
        return error.empty() ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    auto app = px::studio::StudioApp::create(std::move(*options));
    if (!app) {
        px::log::error("{}", app.error().message);
        return SDL_APP_FAILURE;
    }
    *appstate = app.value().release();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    return toSdl(static_cast<px::studio::StudioApp*>(appstate)->iterate());
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    return toSdl(static_cast<px::studio::StudioApp*>(appstate)->handleEvent(*event));
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    delete static_cast<px::studio::StudioApp*>(appstate);
}
