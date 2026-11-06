#include "Launcher.hpp"

#include "LauncherFiles.hpp"

#include "px/assets/Image.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/net/Udp.hpp"
#include "px/project/Project.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <initializer_list>
#include <utility>

namespace px::player {

namespace {

constexpr double kSocialPollSeconds = 10.0;   // presence, requests, invitations
constexpr double kGamesRefreshSeconds = 60.0; // the catalog, when its page opens again
constexpr double kSearchDelaySeconds = 0.4;   // typing a friend's name
constexpr int kRequestTimeoutSeconds = 15;
constexpr double kTestTimeoutSeconds = 120.0;
constexpr int kBoardRows = 10;
constexpr const char* kSorts[] = {"popular", "recent", "playing"};
constexpr const char* kSortNames[] = {"Popular", "New", "Playing now"};

ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return {r / 255.0f, g / 255.0f, b / 255.0f, a}; }

const ImVec4 kBackground = rgb(22, 20, 32);
const ImVec4 kAccent = rgb(255, 196, 72);
const ImVec4 kMuted = rgb(154, 150, 176);
const ImVec4 kErrorColor = rgb(255, 118, 104);
const ImVec4 kGood = rgb(128, 222, 146);
const ImVec4 kPlaying = rgb(120, 196, 255);

void applyStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.ScrollbarSize = 12.0f;
    style.FramePadding = {12.0f, 7.0f};
    style.ItemSpacing = {10.0f, 8.0f};
    style.WindowPadding = {18.0f, 14.0f};
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.SeparatorTextBorderSize = 2.0f;
    // Lines as geometry: renderers that sample textures without filtering
    // (SDL's software renderer) draw texture-based lines with seams.
    style.AntiAliasedLinesUseTex = false;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = kBackground;
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_PopupBg] = rgb(36, 33, 52, 0.98f);
    c[ImGuiCol_Text] = rgb(236, 232, 246);
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_FrameBg] = rgb(40, 37, 58);
    c[ImGuiCol_FrameBgHovered] = rgb(50, 46, 72);
    c[ImGuiCol_FrameBgActive] = rgb(58, 54, 84);
    c[ImGuiCol_Button] = rgb(52, 48, 76);
    c[ImGuiCol_ButtonHovered] = rgb(72, 66, 104);
    c[ImGuiCol_ButtonActive] = rgb(86, 78, 124);
    c[ImGuiCol_Header] = rgb(58, 52, 86);
    c[ImGuiCol_HeaderHovered] = rgb(70, 64, 102);
    c[ImGuiCol_HeaderActive] = rgb(82, 74, 118);
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_Separator] = rgb(56, 52, 78);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 0.03f);
    c[ImGuiCol_TableBorderLight] = rgb(56, 52, 78);
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.55f);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

std::string trim(std::string s) {
    const auto space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!s.empty() && space(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && space(static_cast<unsigned char>(s[start]))) ++start;
    return s.substr(start);
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}

double nowSeconds() { return static_cast<double>(SDL_GetTicksNS()) * 1.0e-9; }

platform::WindowSettings launcherWindow(bool test) {
    platform::WindowSettings w;
    w.title = std::format("{} Player", version::productName);
    w.virtualResolution = {320, 180};
    w.windowScale = 3;
    w.vsync = !test;
    return w;
}

std::string presenceText(const online::Presence& p) {
    if (p.state == "playing") return p.gameTitle.empty() ? std::string("Playing") : "Playing " + p.gameTitle;
    if (p.state == "online") return "Online";
    return "Offline";
}

int presenceRank(const online::Presence& p) {
    if (p.state == "playing") return 0;
    if (p.state == "online") return 1;
    return 2;
}

ImVec4 presenceColor(const online::Presence& p) {
    if (p.state == "playing") return kPlaying;
    if (p.state == "online") return kGood;
    return kMuted;
}

std::string scoreText(double value, const std::string& format) {
    return project::formatScore(value, format == "time" ? project::ScoreFormat::Time : project::ScoreFormat::Number);
}

std::string countText(std::int64_t n, const char* one, const char* many) {
    return std::format("{} {}", n, n == 1 ? one : many);
}

// "2026-11-05" from Unix seconds (UTC), without the C library's time zones.
std::string dateText(std::int64_t unixSeconds) {
    const std::int64_t days = (unixSeconds >= 0 ? unixSeconds : unixSeconds - 86399) / 86400;
    // Howard Hinnant's civil_from_days.
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    return std::format("{:04}-{:02}-{:02}", y, m, d);
}

// Cuts UTF-8 text to at most `bytes`, at a character boundary.
void limitUtf8(std::string& text, std::size_t bytes) {
    if (text.size() <= bytes) return;
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    text.resize(end);
}

ImTextureRef textureRef(platform::SdlPlatform& window, unsigned int handle) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(window.sdlRenderer2D().sdlTexture(handle))));
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float radioWidth(const char* label) {
    return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
}

// On the same line, moves the cursor so that items `width` wide end at the right edge.
void alignRightWidth(float width) {
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width));
}

// The same for buttons with these labels.
void alignRight(std::initializer_list<const char*> labels) {
    float width = 0.0f;
    for (const char* l : labels) width += buttonWidth(l) + ImGui::GetStyle().ItemSpacing.x;
    alignRightWidth(width - ImGui::GetStyle().ItemSpacing.x);
}

bool accentButton(const char* label, ImVec2 size = {0, 0}) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(255, 214, 120));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(240, 176, 48));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(34, 26, 16));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void heading(const char* text, float size) {
    ImGui::PushFont(nullptr, size);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void mutedText(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// A centred column at most `width` wide (pages that are not grids).
void beginColumn(const char* id, float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = std::min(width, avail);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((avail - w) * 0.5f));
    ImGui::BeginChild(id, {w, 0.0f}, ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
}

void endColumn() { ImGui::EndChild(); }

// A colour per game for pictures that have not arrived (or do not exist).
ImU32 placeholderColor(std::int64_t id) {
    static const ImU32 colors[] = {IM_COL32(74, 58, 118, 255), IM_COL32(46, 92, 120, 255), IM_COL32(110, 66, 82, 255),
                                   IM_COL32(56, 104, 82, 255), IM_COL32(120, 92, 48, 255), IM_COL32(64, 70, 120, 255)};
    return colors[static_cast<std::size_t>(id) % std::size(colors)];
}

} // namespace

// --- Life cycle ---------------------------------------------------------------------------------------

Result<std::unique_ptr<Launcher>> Launcher::create(const PlayerOptions& base) {
    std::unique_ptr<Launcher> launcher(new Launcher(base));
    if (auto started = launcher->initialize(); !started) return started.error();
    return launcher;
}

Launcher::Launcher(PlayerOptions base) : m_base(std::move(base)) {}

px::Status Launcher::initialize() {
    if (m_base.verbose) log::setMinLevel(log::Level::Debug);
    m_test = m_base.frames > 0;
    auto settings = launcherWindow(m_test);
    settings.position = m_base.windowPosition;
    if (m_base.windowScale > 0) settings.windowScale = m_base.windowScale;
    auto window = platform::SdlPlatform::create(settings);
    if (!window) return window.error();
    m_window = std::move(window.value());
    m_window->useWindowPixels();

    // Sizes follow the screen's scale (high-density pixels are handled by
    // ImGui's framebuffer scale); fingers get a little more room.
    SDL_Window* w = m_window->sdlWindow();
    const float density = SDL_GetWindowPixelDensity(w);
    const float display = SDL_GetWindowDisplayScale(w);
    m_scale = density > 0.0f && display > 0.0f ? display / density : 1.0f;
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    m_scale *= 1.15f;
#endif
    if (const char* forced = std::getenv("PX_UI_SCALE"); forced != nullptr && std::atof(forced) > 0.0) {
        m_scale = static_cast<float>(std::atof(forced));
    }
    m_scale = std::clamp(m_scale, 0.75f, 4.0f);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    applyStyle(m_scale);
    ImFontConfig font;
    font.SizePixels = 17.0f;
    io.Fonts->AddFontDefaultVector(&font);
    m_imgui = true;
    if (!ImGui_ImplSDL3_InitForSDLRenderer(w, m_window->sdlRenderer()) ||
        !ImGui_ImplSDLRenderer3_Init(m_window->sdlRenderer())) {
        return makeError(ErrorCode::Unsupported, "Could not start the launcher's user interface");
    }

    if (const auto file = findLauncherFile("avatars.png")) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(file->content.data());
        auto image = assets::decodeImage({bytes, file->content.size()}, "avatars.png");
        if (image) m_avatars = m_window->sdlRenderer2D().createTexture(*image, assets::TextureFilter::Nearest);
    }
    m_local = gamesOnThisDevice();
    m_dataDir = online::userPlatformDir();
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    // Apps keep their files in their own folder.
    if (std::getenv("PX_PLATFORM_DIR") == nullptr) {
        if (char* pref = SDL_GetPrefPath("ProjectPixel", "platform")) {
            m_dataDir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(pref)));
            SDL_free(pref);
        }
    }
#endif
#if defined(SDL_PLATFORM_IOS)
    // iOS gives Mbed TLS no access to the trusted roots: HTTPS platforms are
    // checked against the certificates the app comes with (PX_CA_BUNDLE).
    if (std::getenv("PX_PLATFORM_CA") == nullptr) {
        const char* base = SDL_GetBasePath();
        const std::string bundle = std::string(base != nullptr ? base : "") + "ca-certificates.pem";
        std::error_code ec;
        if (std::filesystem::is_regular_file(bundle, ec)) SDL_setenv_unsafe("PX_PLATFORM_CA", bundle.c_str(), 1);
    }
#endif
    m_startedAt = nowSeconds();
    log::info("Launcher open ({} on this device)", countText(static_cast<std::int64_t>(m_local.size()), "game", "games"));

    // Where to log in: --platform, else the saved login's, else this computer.
    const auto saved = online::loadSavedLogin(m_dataDir);
    m_url = !m_base.platformUrl.empty() ? m_base.platformUrl
            : saved                     ? saved->platformUrl
                                        : std::format("http://localhost:{}", online::kDefaultPlatformPort);
    m_pendingOpen = m_base.open;
    if (m_pendingOpen.starts_with("local:")) {
        const std::size_t index = static_cast<std::size_t>(std::atoi(m_pendingOpen.c_str() + 6));
        m_pendingOpen.clear();
        if (index >= m_local.size()) return makeError(ErrorCode::InvalidArgument, "--open local:{}: there is no such game", index);
        m_testGame = m_test;
        playLocal(index);
    }
    if (!m_base.user.empty()) {
        m_username = m_base.user;
        m_password = m_base.password;
        m_remember = !m_test; // tests do not replace the saved login
        tryLogin(m_base.registerAccount);
    } else if (saved && online::PlatformClient(saved->platformUrl).baseUrl() == online::PlatformClient(m_url).baseUrl()) {
        restoreLogin(*saved);
    } else if (!m_pendingOpen.empty()) {
        return makeError(ErrorCode::InvalidArgument, "--open {} needs a login (--user NAME --password TEXT)", m_pendingOpen);
    }
    return {};
}

Launcher::~Launcher() {
    m_jobs.clear(); // waits for requests still running
    if (m_window) {
        clearPictures();
        if (m_avatars != 0) m_window->sdlRenderer2D().destroyTexture(m_avatars);
        if (m_imgui) {
            ImGui_ImplSDLRenderer3_Shutdown();
            ImGui_ImplSDL3_Shutdown();
        }
    }
    if (m_imgui) ImGui::DestroyContext();
}

PlayerOptions Launcher::takeGame() {
    PlayerOptions options = std::move(*m_launch);
    m_launch.reset();
    return options;
}

std::unique_ptr<platform::SdlPlatform> Launcher::takeWindow() {
    if (m_window) SDL_StopTextInput(m_window->sdlWindow());
    return std::move(m_window);
}

void Launcher::resume(std::unique_ptr<platform::SdlPlatform> window, const std::string& message, bool failed) {
    m_window = std::move(window);
    m_window->reconfigure(launcherWindow(m_test));
    m_window->useWindowPixels();
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
    if (failed) {
        fail(message.empty() ? std::string("The game stopped because of an error") : message);
    } else if (!message.empty()) {
        notice(message);
    }
    m_testGame = false;
    m_frame = 0;
    m_startedAt = nowSeconds();
    if (m_me) {
        refreshSocial();
        if (m_page == Page::Game) refreshGamePage(); // a new best score shows at once
    }
}

// --- Jobs ------------------------------------------------------------------------------------------------

online::PlatformClient Launcher::client() const {
    online::PlatformClient c(m_me ? m_platformUrl : trim(m_url));
    c.setToken(m_token);
    c.setTimeoutSeconds(kRequestTimeoutSeconds);
    return c;
}

void Launcher::run(std::function<Apply(online::PlatformClient&)> work, bool blocking) {
    Job job;
    job.session = m_session;
    job.blocking = blocking;
    job.result = std::async(std::launch::async, [c = client(), work = std::move(work)]() mutable -> Apply {
        try {
            return work(c);
        } catch (const std::exception& e) {
            std::string message = e.what();
            return [message](Launcher& l) { l.fail(message); };
        }
    });
    m_jobs.push_back(std::move(job));
}

void Launcher::applyFinishedJobs() {
    std::vector<Apply> ready;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
        if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        Apply apply = it->result.get();
        if (apply && it->session == m_session) ready.push_back(std::move(apply));
        it = m_jobs.erase(it);
    }
    for (auto& apply : ready) apply(*this);
}

bool Launcher::busy() const {
    return std::any_of(m_jobs.begin(), m_jobs.end(), [](const Job& j) { return j.blocking; });
}

void Launcher::fail(std::string message) {
    log::warn("Launcher: {}", message);
    m_error = std::move(message);
    m_notice.clear();
}

void Launcher::notice(std::string message) {
    log::info("Launcher: {}", message);
    m_notice = std::move(message);
    m_error.clear();
}

// --- Logging in --------------------------------------------------------------------------------------------

void Launcher::tryLogin(bool registerAccount) {
    m_error.clear();
    const std::string url = trim(m_url);
    const std::string user = trim(m_username);
    const std::string password = m_password;
    if (url.empty()) return fail("Enter the platform's address");
    if (user.empty() || password.empty()) return fail("Enter your name and password");
    if (registerAccount && !online::isValidUsername(user)) {
        return fail(std::format("Names have {} to {} letters, digits or _", online::kMinUsernameLength,
                                online::kMaxUsernameLength));
    }
    if (registerAccount && password.size() < online::kMinPasswordLength) {
        return fail(std::format("Passwords have at least {} characters", online::kMinPasswordLength));
    }
    const bool remember = m_remember;
    run([url, user, password, registerAccount, remember](online::PlatformClient&) -> Apply {
        online::PlatformClient c(url);
        c.setTimeoutSeconds(kRequestTimeoutSeconds);
        auto auth = registerAccount ? c.registerAccount(user, password) : c.login(user, password);
        if (!auth) return [m = auth.error().message](Launcher& l) { l.fail(m); };
        return [auth = *auth, base = c.baseUrl(), remember](Launcher& l) { l.loggedIn(auth.user, auth.token, base, remember); };
    });
}

void Launcher::restoreLogin(const online::SavedLogin& saved) {
    m_username = saved.username;
    run([saved](online::PlatformClient&) -> Apply {
        online::PlatformClient c(saved.platformUrl);
        c.setTimeoutSeconds(kRequestTimeoutSeconds);
        c.setToken(saved.token);
        auto me = c.me();
        if (!me) {
            const bool expired = me.error().code == ErrorCode::AccessDenied;
            return [expired, m = me.error().message](Launcher& l) {
                if (expired) {
                    online::forgetLogin(l.m_dataDir);
                    l.fail("Your login has expired: log in again");
                } else {
                    l.fail(std::format("Could not reach the platform: {}", m));
                }
                l.m_pendingOpen.clear();
            };
        }
        return [user = *me, saved, base = c.baseUrl()](Launcher& l) { l.loggedIn(user, saved.token, base, false); };
    });
}

void Launcher::loggedIn(const online::UserInfo& user, const std::string& token, const std::string& url, bool remember) {
    ++m_session;
    m_me = user;
    m_token = token;
    m_platformUrl = url;
    m_url = url;
    m_username = user.username;
    m_password.clear();
    if (remember) {
        if (auto saved = online::saveLogin(m_dataDir, {url, user.username, token}); !saved) {
            log::warn("{}", saved.error().message);
        }
    }
    log::info("Launcher: logged in to {} as {}", url, user.username);
    m_error.clear();
    m_notice.clear();
    m_games.clear();
    m_gamesLoaded = false;
    m_social = {};
    m_profile.reset();
    clearPictures();
    refreshSocial();
    refreshProfile();
    openStartPage();
}

// --open (tests and links): the first page after logging in.
void Launcher::openStartPage() {
    const std::string open = std::exchange(m_pendingOpen, {});
    const auto number = [&open](std::size_t from) { return std::atoll(open.c_str() + from); };
    if (open.starts_with("game:")) {
        openGame(number(5));
    } else if (open.starts_with("play:")) {
        openPage(Page::Games);
        m_testGame = m_test;
        play(number(5), {});
    } else if (open.starts_with("join:")) {
        openPage(Page::Friends);
        m_testGame = m_test;
        joinFriend(open.substr(5), {});
    } else if (open == "friends") {
        openPage(Page::Friends);
    } else if (open == "you") {
        openPage(Page::You);
    } else {
        if (!open.empty()) fail(std::format("--open {}: not a page (game:ID, play:ID, join:NAME, friends, you)", open));
        openPage(Page::Games);
    }
}

void Launcher::logOut() {
    run([](online::PlatformClient& c) -> Apply {
        (void)c.logout();
        return {};
    }, false);
    online::forgetLogin(m_dataDir);
    ++m_session;
    log::info("Launcher: logged out");
    m_me.reset();
    m_token.clear();
    m_platformUrl.clear();
    m_profile.reset();
    m_social = {};
    m_games.clear();
    m_suggestions.clear();
    m_page = Page::Login;
    notice("Logged out");
}

// --- Pages and requests ----------------------------------------------------------------------------------

void Launcher::openPage(Page page) {
    m_page = page;
    m_error.clear();
    m_notice.clear();
    m_scrollTop = true;
    if (!m_me) return;
    if (page == Page::Games && (!m_gamesLoaded || nowSeconds() - m_gamesAt > kGamesRefreshSeconds)) refreshGames();
    if (page == Page::Friends) refreshSocial();
    if (page == Page::You) refreshProfile();
}

void Launcher::openGame(std::int64_t id) {
    m_gameId = id;
    m_game.reset();
    for (const auto& g : m_games) {
        if (g.id == id) m_game = g;
    }
    m_boards.clear();
    m_boardsLoaded = false;
    openPage(Page::Game);
    refreshGamePage();
}

void Launcher::refreshGames() {
    online::GameQuery query;
    query.search = trim(m_search);
    query.sort = kSorts[std::clamp(m_sort, 0, 2)];
    query.limit = 60;
    m_gamesAt = nowSeconds();
    m_gamesLoading = true;
    run([query](online::PlatformClient& c) -> Apply {
        auto games = c.games(query);
        if (!games) {
            return [m = games.error().message](Launcher& l) {
                l.m_gamesLoading = false;
                l.fail(m);
            };
        }
        return [games = std::move(*games)](Launcher& l) {
            l.m_games = games;
            l.m_gamesLoading = false;
            l.m_gamesLoaded = true;
        };
    }, false);
}

void Launcher::refreshSocial() {
    m_socialAt = nowSeconds();
    if (m_socialLoading) return;
    m_socialLoading = true;
    run([](online::PlatformClient& c) -> Apply {
        auto social = c.social();
        if (!social) {
            return [m = social.error().message](Launcher& l) {
                l.m_socialLoading = false;
                log::debug("Launcher: friends: {}", m);
            };
        }
        std::stable_sort(social->friends.begin(), social->friends.end(), [](const auto& a, const auto& b) {
            return presenceRank(a.presence) < presenceRank(b.presence);
        });
        return [social = std::move(*social)](Launcher& l) {
            l.m_social = social;
            l.m_socialLoading = false;
        };
    }, false);
}

void Launcher::refreshGamePage() {
    const std::int64_t id = m_gameId;
    run([id](online::PlatformClient& c) -> Apply {
        auto game = c.game(id);
        if (!game) return [m = game.error().message](Launcher& l) { l.fail(m); };
        auto boards = c.leaderboards(id, kBoardRows);
        if (!boards) {
            return [game = *game, m = boards.error().message](Launcher& l) {
                l.m_game = game;
                l.fail(m);
            };
        }
        return [id, game = *game, boards = std::move(*boards)](Launcher& l) {
            if (l.m_gameId != id) return;
            l.m_game = game;
            l.m_boards = boards;
            l.m_boardsLoaded = true;
        };
    }, false);
}

void Launcher::refreshProfile() {
    if (!m_me) return;
    const std::string name = m_me->username;
    run([name](online::PlatformClient& c) -> Apply {
        auto profile = c.profile(name);
        if (!profile) return [m = profile.error().message](Launcher& l) { l.fail(m); };
        auto blocked = c.blocked();
        return [profile = *profile, blocked = blocked ? *blocked : std::vector<online::PersonInfo>{}](Launcher& l) {
            l.m_profile = profile;
            l.m_bio = profile.bio;
            l.m_avatar = profile.avatar;
            l.m_joinPolicy = profile.joinPolicy == online::JoinPolicy::Nobody ? 1 : 0;
            l.m_blocked = blocked;
        };
    }, false);
}

void Launcher::saveProfile() {
    online::ProfileUpdate update;
    update.bio = m_bio;
    update.avatar = m_avatar;
    update.joinPolicy = m_joinPolicy == 1 ? online::JoinPolicy::Nobody : online::JoinPolicy::Friends;
    run([update](online::PlatformClient& c) -> Apply {
        auto profile = c.updateProfile(update);
        if (!profile) return [m = profile.error().message](Launcher& l) { l.fail(m); };
        return [profile = *profile](Launcher& l) {
            l.m_profile = profile;
            l.notice("Saved");
        };
    });
}

void Launcher::searchPeople() {
    const std::string prefix = trim(m_addFriend);
    m_searched = m_addFriend;
    if (prefix.size() < 2) {
        m_suggestions.clear();
        return;
    }
    run([prefix](online::PlatformClient& c) -> Apply {
        auto people = c.searchUsers(prefix);
        if (!people) return {};
        return [prefix, people = std::move(*people)](Launcher& l) {
            if (trim(l.m_addFriend) == prefix) l.m_suggestions = people;
        };
    }, false);
}

void Launcher::requestFriend(const std::string& name) {
    run([name](online::PlatformClient& c) -> Apply {
        auto result = c.requestFriend(name);
        if (!result) return [m = result.error().message](Launcher& l) { l.fail(m); };
        return [name, friends = *result == "friends"](Launcher& l) {
            l.m_addFriend.clear();
            l.m_suggestions.clear();
            l.notice(friends ? std::format("You and {} are friends now", name)
                             : std::format("Friend request sent to {}", name));
            l.refreshSocial();
        };
    });
}

// --- Playing -------------------------------------------------------------------------------------------------

PlayerOptions Launcher::gameOptions(const std::string& title) {
    PlayerOptions o;
    o.fromLauncher = true;
    o.gameTitle = title;
    o.verbose = m_base.verbose;
    o.debugOverlay = m_base.debugOverlay;
    o.touch = m_base.touch;
    o.inputScriptPath = m_base.inputScriptPath;
    if (m_testGame) o.frames = m_base.frames;
    if (m_me) {
        o.playerName = m_me->username;
        o.platformUrl = m_platformUrl; // with the token: inviting friends from the game's menu
        o.platformToken = m_token;
    }
    return o;
}

void Launcher::play(std::int64_t gameId, const std::string& title) {
    run([gameId, options = gameOptions(title), dir = m_dataDir](online::PlatformClient& c) mutable -> Apply {
        auto ticket = c.join(gameId);
        if (!ticket) return [m = ticket.error().message](Launcher& l) { l.fail(m); };
        if (options.gameTitle.empty()) {
            if (auto game = c.game(gameId)) options.gameTitle = game->title;
        }
        online::PackageCache cache(dir / "packages");
        auto bytes = cache.get(c, ticket->packageHash);
        if (!bytes) return [m = bytes.error().message](Launcher& l) { l.fail(m); };
        const auto file = cache.pathFor(ticket->packageHash);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return [m = std::format("Could not keep the game in {}", toUtf8(file))](Launcher& l) { l.fail(m); };
        }
        options.packagePath = toUtf8(file);
        options.connectAddress = net::udpSupported() ? std::format("{}:{}", ticket->host, ticket->port) : ticket->webSocket;
        options.token = ticket->token;
        options.serverKey = ticket->serverKey;
        return [options](Launcher& l) {
            log::info("Launcher: playing {} on {}", options.gameTitle, options.connectAddress);
            l.m_launch = options;
        };
    });
}

void Launcher::joinFriend(const std::string& name, const std::string& title) {
    run([name, options = gameOptions(title), dir = m_dataDir](online::PlatformClient& c) mutable -> Apply {
        auto ticket = c.joinFriend(name);
        if (!ticket) return [m = ticket.error().message](Launcher& l) { l.fail(m); };
        online::PackageCache cache(dir / "packages");
        auto bytes = cache.get(c, ticket->packageHash);
        if (!bytes) return [m = bytes.error().message](Launcher& l) { l.fail(m); };
        const auto file = cache.pathFor(ticket->packageHash);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return [m = std::format("Could not keep the game in {}", toUtf8(file))](Launcher& l) { l.fail(m); };
        }
        if (options.gameTitle.empty()) options.gameTitle = std::format("{}'s game", name);
        options.packagePath = toUtf8(file);
        options.connectAddress = net::udpSupported() ? std::format("{}:{}", ticket->host, ticket->port) : ticket->webSocket;
        options.token = ticket->token;
        options.serverKey = ticket->serverKey;
        return [name, options](Launcher& l) {
            log::info("Launcher: joining {} in {}", name, options.gameTitle);
            l.m_launch = options;
        };
    });
}

void Launcher::playLocal(std::size_t index) {
    if (index >= m_local.size()) return;
    PlayerOptions o = gameOptions({});
    o.platformUrl.clear();
    o.platformToken.clear();
    o.playerName.clear();
    o.projectDir = m_local[index].projectDir;
    o.packagePath = m_local[index].packagePath;
    log::info("Launcher: playing {} on this device", m_local[index].title);
    m_launch = o;
}

// --- Pictures --------------------------------------------------------------------------------------------------

void Launcher::requestPicture(std::int64_t gameId, const std::string& hash) {
    if (hash.empty()) return;
    Picture& p = m_pictures[gameId];
    if (p.hash == hash || p.loading) return;
    p.loading = true;
    run([gameId, hash](online::PlatformClient& c) -> Apply {
        std::optional<assets::Image> image;
        if (auto png = c.thumbnail(gameId); png && !png->empty()) {
            if (auto decoded = assets::decodeImage(*png, "thumbnail", {640, 360})) image = std::move(*decoded);
        }
        return [gameId, hash, image](Launcher& l) {
            Picture& picture = l.m_pictures[gameId];
            picture.loading = false;
            picture.hash = hash;
            if (picture.texture != 0) l.m_window->sdlRenderer2D().destroyTexture(picture.texture);
            picture.texture = image ? l.m_window->sdlRenderer2D().createTexture(*image, assets::TextureFilter::Linear) : 0;
        };
    }, false);
}

void Launcher::clearPictures() {
    if (m_window) {
        for (auto& [id, p] : m_pictures) {
            if (p.texture != 0) m_window->sdlRenderer2D().destroyTexture(p.texture);
        }
    }
    m_pictures.clear();
}

// --- Frames and events -----------------------------------------------------------------------------------------

Launcher::Status Launcher::handleEvent(const SDL_Event& event) {
    if (!m_window) return Status::Continue;
    ImGui_ImplSDL3_ProcessEvent(&event);
    if (m_window->processEvent(event)) {
        m_quit = true;
        return Status::Quit;
    }
    if (event.type == SDL_EVENT_DID_ENTER_FOREGROUND && m_me) refreshSocial();
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        const bool back = event.key.scancode == SDL_SCANCODE_AC_BACK;
        if (back || (event.key.scancode == SDL_SCANCODE_ESCAPE && !ImGui::GetIO().WantTextInput)) {
            if (!m_reportName.empty()) {
                m_reportName.clear();
            } else if (m_me && m_page != Page::Games) {
                openPage(Page::Games);
            } else if (back) {
                m_quit = true; // Android's back button on the first page leaves the app
                return Status::Quit;
            }
        }
    }
    return Status::Continue;
}

Launcher::Status Launcher::iterate() {
    const double now = nowSeconds();
    m_frameStart = now;
    applyFinishedJobs();
    if (m_quit) return Status::Quit;
    if (m_launch) return Status::Play;
    if (m_me && !m_test && now - m_socialAt > kSocialPollSeconds) refreshSocial();
    if (m_page == Page::Friends && m_addFriend != m_searched && now - m_searchAt > kSearchDelaySeconds) searchPeople();

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawFrame();
    ImGui::Render();

    SDL_Renderer* r = m_window->sdlRenderer();
    const ImGuiIO& io = ImGui::GetIO();
    SDL_SetRenderTarget(r, nullptr);
    SDL_SetRenderScale(r, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(r, kBackground.x, kBackground.y, kBackground.z, 1.0f);
    SDL_RenderClear(r);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r);

    if (m_test) {
        // A frame counts once nothing is loading: the result does not depend
        // on how fast the platform answers.
        if (m_jobs.empty() && !m_launch) ++m_frame;
        if (m_frame >= m_base.frames) {
            Status status = Status::Quit;
            if (!m_base.screenshotPath.empty()) {
                if (auto saved = m_window->saveScreenshot(m_base.screenshotPath); !saved) {
                    log::error("{}", saved.error().message);
                    status = Status::Failed;
                } else {
                    log::info("Saved screenshot {}", toUtf8(m_base.screenshotPath));
                }
            }
            SDL_RenderPresent(r);
            if (!m_error.empty()) {
                log::error("Launcher: {}", m_error);
                status = Status::Failed;
            }
            log::info("Launcher finished {} frames on the {} page", m_frame,
                      m_page == Page::Login     ? "login"
                      : m_page == Page::Games   ? "games"
                      : m_page == Page::Game    ? "game"
                      : m_page == Page::Friends ? "friends"
                                                : "you");
            return status;
        }
        if (now - m_startedAt > kTestTimeoutSeconds) {
            log::error("Launcher: still waiting for the platform after {} seconds", kTestTimeoutSeconds);
            return Status::Failed;
        }
    }
    SDL_RenderPresent(r);
    if (!m_test) {
        // No faster than 60 frames a second (and slowly when hidden).
        const bool hidden = (SDL_GetWindowFlags(m_window->sdlWindow()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        const double frame = hidden ? 0.1 : 1.0 / 60.0;
        const double spent = nowSeconds() - now;
        if (spent < frame) SDL_DelayPrecise(static_cast<Uint64>((frame - spent) * 1.0e9));
    }
    return Status::Continue;
}

bool Launcher::tapped() const { return !m_dragged; }

void Launcher::touchScroll() {
    // Fingers scroll by dragging (the mouse has its wheel).
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m_dragged = false;
    if (io.MouseSource != ImGuiMouseSource_TouchScreen && !m_base.touch) return;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, px(8.0f))) {
        m_dragged = true;
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
    }
}

void Launcher::drawFrame() {
    // The whole window, inside the safe area of phones with notches.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 pos = viewport->WorkPos;
    ImVec2 size = viewport->WorkSize;
    SDL_Rect safe{};
    if (SDL_GetWindowSafeArea(m_window->sdlWindow(), &safe) && safe.w > 0 && safe.h > 0) {
        pos = {pos.x + static_cast<float>(safe.x), pos.y + static_cast<float>(safe.y)};
        size = {std::min(size.x, static_cast<float>(safe.w)), std::min(size.y, static_cast<float>(safe.h))};
    }
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##launcher", nullptr, flags);
    if (m_me) drawTopBar();
    const bool messages = busy() || !m_error.empty() || !m_notice.empty();
    const float bottom = messages ? ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y : 0.0f;
    ImGui::BeginChild("##page", {0.0f, -bottom});
    if (std::exchange(m_scrollTop, false)) ImGui::SetScrollY(0.0f);
    touchScroll();
    ImGui::BeginDisabled(busy());
    switch (m_me ? m_page : Page::Login) {
        case Page::Login: drawLogin(); break;
        case Page::Games: drawGames(); break;
        case Page::Game: drawGamePage(); break;
        case Page::Friends: drawFriends(); break;
        case Page::You: drawYou(); break;
    }
    ImGui::EndDisabled();
    drawReportPopup();
    ImGui::EndChild();
    if (messages) drawMessages();
    ImGui::End();
}

void Launcher::drawTopBar() {
    const auto tab = [this](const std::string& label, Page page, bool active) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, rgb(88, 76, 130));
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        }
        ImGui::BeginDisabled(busy());
        if (ImGui::Button(label.c_str())) openPage(page);
        ImGui::EndDisabled();
        if (active) ImGui::PopStyleColor(2);
        ImGui::SameLine();
    };
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("PIXEL");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, px(18.0f));
    tab("Games", Page::Games, m_page == Page::Games || m_page == Page::Game);
    const std::size_t alerts = m_social.incoming.size() + m_social.invitations.size();
    tab(alerts > 0 ? std::format("Friends ({})###friends", alerts) : std::string("Friends###friends"), Page::Friends,
        m_page == Page::Friends);
    tab("You", Page::You, m_page == Page::You);

    const std::string& name = m_me->username;
    const float size = ImGui::GetFrameHeight();
    const float width = size + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize(name.c_str()).x;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - width));
    avatar(m_profile ? m_profile->avatar : 0, size);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name.c_str());
    ImGui::Separator();
}

void Launcher::drawMessages() {
    ImGui::Separator();
    if (busy()) {
        static const char* dots[] = {"", ".", "..", "..."};
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Working%s", dots[static_cast<int>(nowSeconds() * 3.0) % 4]);
        return;
    }
    const bool error = !m_error.empty();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, error ? kErrorColor : kGood);
    ImGui::TextUnformatted(error ? m_error.c_str() : m_notice.c_str());
    ImGui::PopStyleColor();
    alignRight({"OK"});
    if (ImGui::Button("OK")) {
        m_error.clear();
        m_notice.clear();
    }
}

void Launcher::avatar(int index, float size) {
    if (m_avatars == 0 || !m_window) {
        ImGui::Dummy({size, size});
        return;
    }
    const int i = std::clamp(index, 0, online::kAvatarCount - 1);
    const float u = static_cast<float>(i % 4) / 4.0f;
    const float v = static_cast<float>(i / 4) / 4.0f;
    ImGui::Image(textureRef(*m_window, m_avatars), {size, size}, {u, v}, {u + 0.25f, v + 0.25f});
}

void Launcher::picture(std::int64_t gameId, const std::string& hash, const std::string& label, float width) {
    requestPicture(gameId, hash);
    const ImVec2 size{width, std::floor(width * 9.0f / 16.0f)};
    const auto it = m_pictures.find(gameId);
    if (it != m_pictures.end() && it->second.texture != 0 && it->second.hash == hash) {
        ImGui::Image(textureRef(*m_window, it->second.texture), size);
        return;
    }
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, {a.x + size.x, a.y + size.y}, placeholderColor(gameId), ImGui::GetStyle().FrameRounding);
    const ImVec2 text = ImGui::CalcTextSize(label.c_str());
    dl->AddText({a.x + (size.x - text.x) * 0.5f, a.y + (size.y - text.y) * 0.5f}, IM_COL32(255, 255, 255, 150), label.c_str());
    ImGui::Dummy(size);
}

// --- Login -----------------------------------------------------------------------------------------------------

void Launcher::drawLogin() {
    ImGui::Dummy({0.0f, px(18.0f)});
    beginColumn("##login", px(440.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    heading("Pixel Player", 30.0f);
    ImGui::PopStyleColor();
    mutedText("Log in to a platform to play its games, see your friends and their best scores.");
    ImGui::Dummy({0.0f, px(6.0f)});

    ImGui::TextUnformatted("Platform");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##url", "https://example.com", &m_url);
    ImGui::TextUnformatted("Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool submit = ImGui::InputText("##name", &m_username, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextUnformatted("Password");
    ImGui::SetNextItemWidth(-FLT_MIN);
    submit |= ImGui::InputText("##password", &m_password, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Checkbox("Stay logged in on this device", &m_remember);
    ImGui::Dummy({0.0f, px(4.0f)});
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (accentButton("Log in", {half, 0.0f}) || submit) tryLogin(false);
    ImGui::SameLine();
    if (ImGui::Button("Create an account", {half, 0.0f})) tryLogin(true);
    ImGui::Dummy({0.0f, px(14.0f)});
    drawLocalGames();
    endColumn();
}

void Launcher::drawLocalGames() {
    if (m_local.empty()) return;
    ImGui::SeparatorText("On this device");
    mutedText("These games play without a platform.");
    for (std::size_t i = 0; i < m_local.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto& g = m_local[i];
        if (ImGui::Button(std::format("{}   ({})", g.title, g.gameType).c_str(), {-FLT_MIN, 0.0f}) && tapped()) playLocal(i);
        ImGui::PopID();
    }
}

// --- Games -----------------------------------------------------------------------------------------------------

void Launcher::drawInvitations() {
    for (const auto& inv : m_social.invitations) {
        ImGui::PushID(static_cast<int>(inv.id));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(52, 44, 30));
        ImGui::BeginChild("##invitation", {0.0f, 0.0f}, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        avatar(inv.fromAvatar, ImGui::GetFrameHeight());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s invites you to play %s", inv.from.c_str(), inv.gameTitle.c_str());
        alignRight({"Join", "Not now"});
        if (accentButton("Join")) joinFriend(inv.from, inv.gameTitle);
        ImGui::SameLine();
        if (ImGui::Button("Not now")) {
            const std::int64_t id = inv.id;
            std::erase_if(m_social.invitations, [id](const auto& i) { return i.id == id; });
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
            run([id](online::PlatformClient& c) -> Apply {
                (void)c.dismissInvitation(id);
                return {};
            }, false);
            return;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void Launcher::drawGames() {
    drawInvitations();
    const float sortWidth = px(150.0f);
    ImGui::SetNextItemWidth(std::max(px(120.0f), ImGui::GetContentRegionAvail().x - sortWidth - buttonWidth("Refresh") -
                                                     ImGui::GetStyle().ItemSpacing.x * 2.0f));
    const bool searched = ImGui::InputTextWithHint("##search", "Search games", &m_search, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sortWidth);
    const bool sorted = ImGui::Combo("##sort", &m_sort, kSortNames, 3);
    ImGui::SameLine();
    if (ImGui::Button("Refresh") || searched || sorted) refreshGames();
    ImGui::Dummy({0.0f, px(4.0f)});

    if (m_games.empty()) {
        mutedText(m_gamesLoading || !m_gamesLoaded ? "Loading games..."
                  : m_search.empty()               ? "Nobody has published a game here yet."
                                                   : "No games match your search.");
    }
    // A grid of cards, as many columns as fit.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (px(230.0f) + spacing)));
    const float width = std::floor((avail - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    for (std::size_t i = 0; i < m_games.size(); ++i) {
        if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine();
        if (gameCard(m_games[i], width) && tapped()) openGame(m_games[i].id);
    }
    ImGui::Dummy({0.0f, px(10.0f)});
    drawLocalGames();
}

bool Launcher::gameCard(const online::GameInfo& game, float width) {
    ImGui::PushID(static_cast<int>(game.id));
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad = px(8.0f);
    const float pictureHeight = std::floor((width - pad * 2.0f) * 9.0f / 16.0f);
    const float line = ImGui::GetTextLineHeight();
    const float height = pad + pictureHeight + pad + line + style.ItemSpacing.y * 0.5f + line + pad;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##card", {width, height});
    const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, {a.x + width, a.y + height}, hovered ? IM_COL32(58, 52, 86, 255) : IM_COL32(36, 33, 52, 255),
                      style.ChildRounding);

    // The picture (or a coloured block with the game type).
    requestPicture(game.id, game.thumbnail);
    const ImVec2 p0{a.x + pad, a.y + pad};
    const ImVec2 p1{a.x + width - pad, a.y + pad + pictureHeight};
    const auto it = m_pictures.find(game.id);
    if (it != m_pictures.end() && it->second.texture != 0 && it->second.hash == game.thumbnail) {
        dl->AddImageRounded(textureRef(*m_window, it->second.texture), p0, p1, {0, 0}, {1, 1}, IM_COL32_WHITE,
                            style.FrameRounding);
    } else {
        dl->AddRectFilled(p0, p1, placeholderColor(game.id), style.FrameRounding);
        const std::string type = game.gameType.empty() ? std::string("game") : game.gameType;
        const ImVec2 t = ImGui::CalcTextSize(type.c_str());
        dl->AddText({(p0.x + p1.x - t.x) * 0.5f, (p0.y + p1.y - t.y) * 0.5f}, IM_COL32(255, 255, 255, 140), type.c_str());
    }
    if (game.playing > 0) {
        const std::string badge = std::format("{} playing", game.playing);
        const ImVec2 t = ImGui::CalcTextSize(badge.c_str());
        const ImVec2 b0{p1.x - t.x - pad * 1.5f, p0.y + pad * 0.5f};
        dl->AddRectFilled(b0, {p1.x - pad * 0.5f, b0.y + t.y + pad * 0.5f}, IM_COL32(20, 18, 30, 210), style.FrameRounding);
        dl->AddText({b0.x + pad * 0.5f, b0.y + pad * 0.25f}, ImGui::GetColorU32(kPlaying), badge.c_str());
    }

    // Title and owner, cut to the card.
    const ImVec4 clip{p0.x, p1.y, p1.x, a.y + height};
    float y = p1.y + pad;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {p0.x, y}, ImGui::GetColorU32(ImGuiCol_Text), game.title.c_str(),
                nullptr, 0.0f, &clip);
    y += line + style.ItemSpacing.y * 0.5f;
    const std::string by = std::format("by {} · {}", game.ownerName, countText(game.plays, "play", "plays"));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {p0.x, y}, ImGui::GetColorU32(kMuted), by.c_str(), nullptr, 0.0f,
                &clip);
    ImGui::PopID();
    return pressed;
}

void Launcher::drawGamePage() {
    if (ImGui::Button("< Games")) {
        openPage(Page::Games);
        return;
    }
    if (!m_game) {
        mutedText("Loading...");
        return;
    }
    const online::GameInfo& g = *m_game;
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool wide = avail > px(640.0f);
    const float pictureWidth = wide ? std::min(avail * 0.5f, px(480.0f)) : avail;
    picture(g.id, g.thumbnail, g.gameType.empty() ? "game" : g.gameType, pictureWidth);
    if (wide) ImGui::SameLine(0.0f, px(18.0f));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(0.0f);
    heading(g.title.c_str(), 26.0f);
    mutedText(std::format("by {}", g.ownerName));
    if (!g.description.empty()) ImGui::TextWrapped("%s", g.description.c_str());
    mutedText(std::format("{} now · {} · version {}", countText(g.playing, "player", "players"),
                          countText(g.plays, "play", "plays"), g.latestVersion));
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0.0f, px(4.0f)});
    if (g.latestVersion == 0) {
        mutedText("Nothing has been published yet.");
    } else if (accentButton("Play", {px(160.0f), ImGui::GetFrameHeight() * 1.3f})) {
        play(g.id, g.title);
    }
    // Friends in this game.
    for (const auto& f : m_social.friends) {
        if (f.presence.state != "playing" || f.presence.gameId != g.id) continue;
        ImGui::PushID(f.username.c_str());
        avatar(f.avatar, ImGui::GetFrameHeight());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, kPlaying);
        ImGui::Text("%s is playing", f.username.c_str());
        ImGui::PopStyleColor();
        if (f.presence.joinable) {
            ImGui::SameLine();
            if (ImGui::Button("Join")) joinFriend(f.username, g.title);
        }
        ImGui::PopID();
    }
    ImGui::EndGroup();

    ImGui::Dummy({0.0f, px(8.0f)});
    if (!m_boardsLoaded) return;
    if (m_boards.empty()) {
        mutedText("This game has no leaderboards.");
        return;
    }
    heading("Leaderboards", 21.0f);
    alignRightWidth(radioWidth("Everyone") + ImGui::GetStyle().ItemSpacing.x + radioWidth("Friends"));
    if (ImGui::RadioButton("Everyone", !m_friendsBoards)) m_friendsBoards = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Friends", m_friendsBoards)) m_friendsBoards = true;
    // Two boards side by side when there is room.
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = ImGui::GetContentRegionAvail().x > px(700.0f) && m_boards.size() > 1 ? 2 : 1;
    const float width = (ImGui::GetContentRegionAvail().x - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    for (std::size_t i = 0; i < m_boards.size(); ++i) {
        if (i % static_cast<std::size_t>(columns) != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginChild("##board", {width, 0.0f}, ImGuiChildFlags_AutoResizeY);
        drawLeaderboard(m_boards[i]);
        ImGui::EndChild();
        ImGui::PopID();
    }
}

void Launcher::drawLeaderboard(const online::LeaderboardView& board) {
    ImGui::SeparatorText(board.title.c_str());
    const auto& rows = m_friendsBoards ? board.friends : board.top;
    if (rows.empty()) {
        mutedText(m_friendsBoards ? "None of your friends has a score yet." : "No scores yet: be the first!");
        return;
    }
    const std::string me = m_me ? m_me->username : std::string();
    const float size = ImGui::GetTextLineHeight() * 1.3f;
    if (!ImGui::BeginTable("##rows", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000").x);
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00:00.00").x * 1.3f);
    const auto row = [&](const online::LeaderboardEntry& e) {
        const bool mine = e.username == me;
        ImGui::TableNextRow();
        if (mine) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%d", e.rank);
        ImGui::TableNextColumn();
        avatar(e.avatar, size);
        ImGui::SameLine();
        ImGui::TextUnformatted(e.username.c_str());
        ImGui::TableNextColumn();
        const std::string score = scoreText(e.value, board.format);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(score.c_str()).x);
        ImGui::TextUnformatted(score.c_str());
        if (mine) ImGui::PopStyleColor();
    };
    bool shownMe = false;
    for (const auto& e : rows) {
        row(e);
        shownMe = shownMe || e.username == me;
    }
    if (!m_friendsBoards && !shownMe && board.you) row(*board.you);
    ImGui::EndTable();
    if (board.order == "lowest") mutedText("Lowest is best.");
}

// --- Friends ---------------------------------------------------------------------------------------------------

void Launcher::drawPersonMenu(const std::string& name, bool isFriend) {
    if (ImGui::Button("...")) ImGui::OpenPopup("##person");
    if (!ImGui::BeginPopup("##person")) return;
    if (isFriend && ImGui::MenuItem("Remove from friends")) {
        run([name](online::PlatformClient& c) -> Apply {
            if (auto removed = c.removeFriend(name); !removed) return [m = removed.error().message](Launcher& l) { l.fail(m); };
            return [name](Launcher& l) {
                l.notice(std::format("{} is no longer your friend", name));
                l.refreshSocial();
            };
        });
    }
    if (ImGui::MenuItem("Block")) {
        run([name](online::PlatformClient& c) -> Apply {
            if (auto blocked = c.block(name); !blocked) return [m = blocked.error().message](Launcher& l) { l.fail(m); };
            return [name](Launcher& l) {
                l.notice(std::format("You blocked {}: you will not meet in games. Unblock them on your page.", name));
                l.refreshSocial();
            };
        });
    }
    if (ImGui::MenuItem("Report...")) {
        m_reportName = name;
        m_reportReason.clear();
    }
    ImGui::EndPopup();
}

void Launcher::drawReportPopup() {
    if (m_reportName.empty()) return;
    if (!ImGui::IsPopupOpen("Report")) ImGui::OpenPopup("Report");
    ImGui::SetNextWindowSize({std::min(px(460.0f), ImGui::GetIO().DisplaySize.x - px(20.0f)), 0.0f});
    if (!ImGui::BeginPopupModal("Report", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar)) return;
    ImGui::TextWrapped("Report %s to the moderators", m_reportName.c_str());
    mutedText("What did they do? A moderator reads every report.");
    ImGui::InputTextMultiline("##reason", &m_reportReason, {-FLT_MIN, px(90.0f)});
    limitUtf8(m_reportReason, online::kMaxReportLength);
    ImGui::BeginDisabled(trim(m_reportReason).empty());
    if (accentButton("Send")) {
        const std::string name = m_reportName;
        const std::string reason = trim(m_reportReason);
        run([name, reason](online::PlatformClient& c) -> Apply {
            if (auto sent = c.reportUser(name, reason); !sent) return [m = sent.error().message](Launcher& l) { l.fail(m); };
            return [](Launcher& l) { l.notice("Thank you: a moderator will look at it"); };
        });
        m_reportName.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_reportName.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Launcher::drawFriends() {
    beginColumn("##friends", px(620.0f));
    const float size = ImGui::GetFrameHeight();
    drawInvitations();

    heading("Add a friend", 21.0f);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth("Send a request") - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputTextWithHint("##add", "Their name", &m_addFriend, ImGuiInputTextFlags_EnterReturnsTrue) &&
        !trim(m_addFriend).empty()) {
        requestFriend(trim(m_addFriend));
    }
    if (ImGui::IsItemEdited()) m_searchAt = nowSeconds();
    ImGui::SameLine();
    ImGui::BeginDisabled(trim(m_addFriend).empty());
    if (ImGui::Button("Send a request")) requestFriend(trim(m_addFriend));
    ImGui::EndDisabled();
    for (const auto& p : m_suggestions) {
        if (p.username == m_me->username || p.username == trim(m_addFriend)) continue;
        ImGui::PushID(p.username.c_str());
        avatar(p.avatar, size * 0.8f);
        ImGui::SameLine();
        if (ImGui::Selectable(p.username.c_str())) {
            m_addFriend = p.username;
            m_searched = m_addFriend;
            m_suggestions.clear();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (!m_social.incoming.empty()) {
        ImGui::Dummy({0.0f, px(6.0f)});
        heading("Want to be your friend", 21.0f);
        for (const auto& r : m_social.incoming) {
            ImGui::PushID(r.username.c_str());
            avatar(r.avatar, size);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.username.c_str());
            alignRight({"Accept", "Decline", "..."});
            const std::string name = r.username;
            if (accentButton("Accept")) {
                run([name](online::PlatformClient& c) -> Apply {
                    if (auto ok = c.acceptFriend(name); !ok) return [m = ok.error().message](Launcher& l) { l.fail(m); };
                    return [name](Launcher& l) {
                        l.notice(std::format("You and {} are friends now", name));
                        l.refreshSocial();
                    };
                });
            }
            ImGui::SameLine();
            if (ImGui::Button("Decline")) {
                run([name](online::PlatformClient& c) -> Apply {
                    (void)c.removeFriendRequest(name);
                    return [](Launcher& l) { l.refreshSocial(); };
                });
            }
            ImGui::SameLine();
            drawPersonMenu(name, false);
            ImGui::PopID();
        }
    }

    ImGui::Dummy({0.0f, px(6.0f)});
    heading(m_social.friends.empty() ? "Friends" : std::format("Friends ({})", m_social.friends.size()).c_str(), 21.0f);
    if (m_social.friends.empty()) mutedText("Add friends by their name to see when they play and join them.");
    for (const auto& f : m_social.friends) {
        ImGui::PushID(f.username.c_str());
        avatar(f.avatar, size * 1.4f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(f.username.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, presenceColor(f.presence));
        ImGui::TextUnformatted(presenceText(f.presence).c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        const std::string name = f.username;
        if (f.presence.joinable) {
            alignRight({"Join", "..."});
            if (accentButton("Join")) joinFriend(name, f.presence.gameTitle);
        } else if (f.presence.state == "playing" && f.presence.gameId != 0) {
            alignRight({"See the game", "..."});
            if (ImGui::Button("See the game")) openGame(f.presence.gameId);
        } else {
            alignRight({"..."});
        }
        ImGui::SameLine();
        drawPersonMenu(name, true);
        ImGui::PopID();
    }

    if (!m_social.outgoing.empty()) {
        ImGui::Dummy({0.0f, px(6.0f)});
        heading("Waiting for an answer", 21.0f);
        for (const auto& r : m_social.outgoing) {
            ImGui::PushID(r.username.c_str());
            avatar(r.avatar, size);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.username.c_str());
            alignRight({"Take back"});
            const std::string name = r.username;
            if (ImGui::Button("Take back")) {
                run([name](online::PlatformClient& c) -> Apply {
                    (void)c.removeFriendRequest(name);
                    return [](Launcher& l) { l.refreshSocial(); };
                });
            }
            ImGui::PopID();
        }
    }
    endColumn();
}

// --- You -------------------------------------------------------------------------------------------------------

void Launcher::drawYou() {
    beginColumn("##you", px(560.0f));
    if (!m_profile) {
        mutedText("Loading...");
        endColumn();
        return;
    }
    const online::Profile& p = *m_profile;
    avatar(m_avatar, px(72.0f));
    ImGui::SameLine();
    ImGui::BeginGroup();
    heading(p.user.username.c_str(), 26.0f);
    mutedText(std::format("Joined {} · {}", dateText(p.user.created), countText(p.friends, "friend", "friends")));
    ImGui::EndGroup();

    ImGui::SeparatorText("Portrait");
    const float cell = px(40.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float framed = cell + px(4.0f) * 2.0f;
    const int perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (framed + spacing)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {px(4.0f), px(4.0f)});
    for (int i = 0; i < online::kAvatarCount; ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        ImGui::PushID(i);
        const bool selected = i == m_avatar;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        const float u = static_cast<float>(i % 4) / 4.0f;
        const float v = static_cast<float>(i / 4) / 4.0f;
        if (m_avatars != 0 &&
            ImGui::ImageButton("##a", textureRef(*m_window, m_avatars), {cell, cell}, {u, v}, {u + 0.25f, v + 0.25f}) &&
            tapped()) {
            m_avatar = i;
        }
        if (selected) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    ImGui::SeparatorText("About you");
    ImGui::InputTextMultiline("##bio", &m_bio, {-FLT_MIN, px(80.0f)});
    limitUtf8(m_bio, online::kMaxBioLength);
    mutedText(std::format("{} / {}", m_bio.size(), online::kMaxBioLength));

    ImGui::SeparatorText("Who can join you in games");
    ImGui::RadioButton("Friends", &m_joinPolicy, 0);
    ImGui::RadioButton("Only friends I invite", &m_joinPolicy, 1);
    const bool changed = m_bio != p.bio || m_avatar != p.avatar ||
                         m_joinPolicy != (p.joinPolicy == online::JoinPolicy::Nobody ? 1 : 0);
    ImGui::BeginDisabled(!changed);
    if (accentButton("Save")) saveProfile();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Blocked");
    if (m_blocked.empty()) mutedText("Nobody. Blocked players cannot add you, invite you or play in the same game as you.");
    for (const auto& b : m_blocked) {
        ImGui::PushID(b.username.c_str());
        avatar(b.avatar, ImGui::GetFrameHeight());
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(b.username.c_str());
        alignRight({"Unblock"});
        if (ImGui::Button("Unblock")) {
            const std::string name = b.username;
            run([name](online::PlatformClient& c) -> Apply {
                if (auto ok = c.unblock(name); !ok) return [m = ok.error().message](Launcher& l) { l.fail(m); };
                return [name](Launcher& l) {
                    l.notice(std::format("{} is no longer blocked", name));
                    l.refreshProfile();
                };
            });
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Account");
    mutedText(std::format("Logged in to {}", m_platformUrl));
    if (ImGui::Button("Log out")) logOut();
    ImGui::Dummy({0.0f, px(10.0f)});
    drawLocalGames();
    endColumn();
}

} // namespace px::player
