#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;

namespace px::studio {

// Native file and folder dialogs (SDL3). SDL may call back on another
// thread, so results are queued and collected by the main loop with poll().
// If the platform has no dialog support (e.g. Linux without a portal or
// zenity), the request fails and Studio's text fields remain usable.
class FileDialogs {
public:
    enum class Purpose { OpenProjectFolder, NewProjectParent, ImportAssets };

    struct Result {
        Purpose purpose = Purpose::OpenProjectFolder;
        std::vector<std::filesystem::path> paths; // empty: cancelled
        std::string error;                        // set if the dialog could not be shown
    };

    void pickFolder(SDL_Window* window, Purpose purpose, const std::filesystem::path& start = {});
    void pickImportFiles(SDL_Window* window, const std::filesystem::path& start = {});

    // Results that arrived since the last call.
    [[nodiscard]] std::vector<Result> poll();
    [[nodiscard]] bool busy() const;

private:
    struct Request {
        FileDialogs* owner = nullptr;
        Purpose purpose = Purpose::OpenProjectFolder;
    };
    static void callback(void* userdata, const char* const* files, int filter);

    mutable std::mutex m_mutex;
    std::vector<Result> m_results;
    int m_open = 0;
    // One request object per purpose; addresses stay valid for SDL.
    Request m_requests[3];
    std::string m_startLocation;
};

} // namespace px::studio
