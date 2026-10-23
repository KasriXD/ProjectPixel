#include "FileDialogs.hpp"

#include "Utf8.hpp"

#include <SDL3/SDL.h>

namespace px::studio {

namespace {

const SDL_DialogFileFilter kImportFilters[] = {
    {"Images, scripts and scenes", "png;jpg;jpeg;bmp;tga;luau;json"},
    {"Images", "png;jpg;jpeg;bmp;tga"},
    {"Luau scripts", "luau"},
    {"Scenes", "json"},
};

} // namespace

void FileDialogs::callback(void* userdata, const char* const* files, int) {
    auto* request = static_cast<Request*>(userdata);
    Result result;
    result.purpose = request->purpose;
    if (files == nullptr) {
        const char* error = SDL_GetError();
        result.error = error != nullptr && *error != '\0' ? error : "the dialog could not be shown";
    } else {
        for (const char* const* f = files; *f != nullptr; ++f) result.paths.push_back(fromUtf8(*f));
    }
    const std::lock_guard lock(request->owner->m_mutex);
    request->owner->m_results.push_back(std::move(result));
    --request->owner->m_open;
}

void FileDialogs::pickFolder(SDL_Window* window, Purpose purpose, const std::filesystem::path& start) {
    Request& request = m_requests[static_cast<int>(purpose)];
    request.owner = this;
    request.purpose = purpose;
    {
        const std::lock_guard lock(m_mutex);
        ++m_open;
    }
    m_startLocation = start.empty() ? std::string() : toUtf8(start);
    SDL_ShowOpenFolderDialog(&FileDialogs::callback, &request, window,
                             m_startLocation.empty() ? nullptr : m_startLocation.c_str(), false);
}

void FileDialogs::pickImportFiles(SDL_Window* window, const std::filesystem::path& start) {
    Request& request = m_requests[static_cast<int>(Purpose::ImportAssets)];
    request.owner = this;
    request.purpose = Purpose::ImportAssets;
    {
        const std::lock_guard lock(m_mutex);
        ++m_open;
    }
    m_startLocation = start.empty() ? std::string() : toUtf8(start);
    SDL_ShowOpenFileDialog(&FileDialogs::callback, &request, window, kImportFilters,
                           static_cast<int>(std::size(kImportFilters)),
                           m_startLocation.empty() ? nullptr : m_startLocation.c_str(), true);
}

std::vector<FileDialogs::Result> FileDialogs::poll() {
    const std::lock_guard lock(m_mutex);
    std::vector<Result> out = std::move(m_results);
    m_results.clear();
    return out;
}

bool FileDialogs::busy() const {
    const std::lock_guard lock(m_mutex);
    return m_open > 0;
}

} // namespace px::studio
