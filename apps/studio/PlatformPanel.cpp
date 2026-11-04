// The Platform panel: log in, publish the open project, manage your games,
// browse and play published games, and (administrators) moderate
// (docs/studio.md#publishing). Every request runs on a worker thread; the
// panel shows "Working..." meanwhile and never blocks the editor.
#include "MultiplayerTest.hpp"
#include "Panels.hpp"
#include "StudioApp.hpp"
#include "UiHelpers.hpp"

#include "px/core/Log.hpp"
#include "px/online/PlatformClient.hpp"
#include "px/project/Package.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
#include <fstream>

namespace px::studio {

using online::GameInfo;
using online::PlatformClient;
using online::Visibility;

namespace {
const char* kVisibilities[] = {"Public", "Unlisted", "Private"};

std::string ago(std::int64_t when) {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const auto s = std::max<std::int64_t>(0, now - when);
    if (s < 90) return "just now";
    if (s < 5400) return std::format("{} min ago", s / 60);
    if (s < 2 * 86400) return std::format("{} h ago", s / 3600);
    return std::format("{} days ago", s / 86400);
}

std::filesystem::path linkFile(const std::filesystem::path& project) { return project / ".px" / "publish.json"; }
} // namespace

PlatformPanel::PlatformPanel() {
    if (auto saved = online::loadSavedLogin(online::userPlatformDir())) {
        m_url = saved->platformUrl;
        m_user = saved->username;
        m_token = saved->token;
        m_checkSaved = true;
    }
}

PlatformPanel::~PlatformPanel() {
    if (m_job.valid()) m_job.wait();
}

PlatformClient PlatformPanel::client() const {
    PlatformClient c(m_url);
    c.setToken(m_token);
    return c;
}

void PlatformPanel::run(std::string what, Work work) {
    if (busy()) return;
    m_jobText = std::move(what);
    m_error.clear();
    PlatformClient c = client();
    m_job = std::async(std::launch::async, [work = std::move(work), c]() mutable { return work(c); });
}

void PlatformPanel::pollJob(StudioApp& app) {
    if (!m_job.valid() || m_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    Apply apply = m_job.get();
    m_jobText.clear();
    if (apply) apply(*this, app);
}

void PlatformPanel::loggedIn(const online::AuthResult& auth) {
    m_me = auth.user;
    m_token = auth.token;
    m_password.clear();
    if (auto saved = online::saveLogin(online::userPlatformDir(), {m_url, auth.user.username, auth.token}); !saved) {
        log::warn("{}", saved.error().message);
    }
    m_refreshMine = true;
    m_refreshBrowse = true;
}

void PlatformPanel::draw(StudioApp& app) {
    pollJob(app);
    if (!m_open) return;
    if (m_focus) {
        ImGui::SetNextWindowFocus();
        m_focus = false;
    }
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 40.0f, ImGui::GetFontSize() * 32.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Platform###platform", &m_open)) {
        ImGui::End();
        return;
    }
    if (m_checkSaved && !busy()) {
        m_checkSaved = false;
        run("Logging in...", [](PlatformClient& c) -> Apply {
            auto me = c.me();
            return [me](PlatformPanel& p, StudioApp&) {
                if (me) {
                    p.m_me = me.value();
                    p.m_refreshMine = true;
                    p.m_refreshBrowse = true;
                } else {
                    p.m_token.clear();
                }
            };
        });
    }
    if (busy()) {
        ImGui::TextColored({0.9f, 0.8f, 0.4f, 1.0f}, "%s", m_jobText.c_str());
    } else if (!m_error.empty()) {
        ImGui::TextColored(ui::kErrorColor, "%s", m_error.c_str());
    } else if (!m_status.empty()) {
        ImGui::TextColored({0.5f, 0.85f, 0.5f, 1.0f}, "%s", m_status.c_str());
    } else {
        ImGui::TextDisabled("%s", m_me ? std::format("Logged in to {} as {}", m_url, m_me->username).c_str()
                                       : "Not logged in");
    }
    ImGui::Separator();
    if (!m_me) {
        drawLogin();
    } else if (ImGui::BeginTabBar("platform-tabs")) {
        if (ImGui::BeginTabItem("Publish")) {
            drawPublish(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("My Games")) {
            drawMyGames(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Play")) {
            drawBrowse(app);
            ImGui::EndTabItem();
        }
        if (m_me->role == online::Role::Admin && ImGui::BeginTabItem("Moderation")) {
            drawModeration();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Account")) {
            ImGui::Text("%s on %s%s", m_me->username.c_str(), m_url.c_str(),
                        m_me->role == online::Role::Admin ? " (administrator)" : "");
            if (ImGui::Button("Log Out")) {
                run("Logging out...", [](PlatformClient& c) -> Apply {
                    (void)c.logout();
                    return [](PlatformPanel& p, StudioApp&) {
                        p.m_me.reset();
                        p.m_token.clear();
                        online::forgetLogin(online::userPlatformDir());
                    };
                });
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    drawReportPopup();
    ImGui::End();
}

void PlatformPanel::drawLogin() {
    ImGui::TextWrapped("Log in to a platform to publish your game and play other people's. Start one on this "
                       "computer with px-platform (see docs/platform.md).");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputText("Platform", &m_url);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputText("Name", &m_user);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    const bool enter = ImGui::InputText("Password", &m_password,
                                        ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(busy() || m_user.empty() || m_password.empty());
    const auto auth = [this](bool create) {
        const std::string user = m_user;
        const std::string password = m_password;
        run(create ? "Creating the account..." : "Logging in...", [user, password, create](PlatformClient& c) -> Apply {
            auto result = create ? c.registerAccount(user, password) : c.login(user, password);
            return [result](PlatformPanel& p, StudioApp&) {
                if (result) {
                    p.loggedIn(result.value());
                    p.m_status = std::format("Welcome, {}", result->user.username);
                } else {
                    p.m_error = result.error().message;
                }
            };
        });
    };
    if (ImGui::Button("Log In") || (enter && !busy())) auth(false);
    ImGui::SameLine();
    if (ImGui::Button("Create Account")) auth(true);
    ImGui::EndDisabled();
    ui::tooltip("Names are 3 to 20 letters, digits or _; passwords at least 8 characters");
}

void PlatformPanel::loadLink(StudioApp& app) {
    const auto* project = app.project();
    const std::filesystem::path root = project != nullptr ? project->root() : std::filesystem::path();
    if (root == m_linkedProject) return;
    m_linkedProject = root;
    m_linkedGame = 0;
    m_title = project != nullptr ? project->manifest().name : "";
    std::ifstream in(linkFile(root));
    if (!in) return;
    try {
        nlohmann::json j;
        in >> j;
        if (j.value("platform", std::string()) == m_url) m_linkedGame = j.value("game", std::int64_t{0});
    } catch (const std::exception&) {
    }
}

void PlatformPanel::saveLink() {
    std::error_code ec;
    std::filesystem::create_directories(m_linkedProject / ".px", ec);
    std::ofstream out(linkFile(m_linkedProject), std::ios::trunc);
    out << nlohmann::json{{"platform", m_url}, {"game", m_linkedGame}}.dump(2) << "\n";
}

void PlatformPanel::drawPublish(StudioApp& app) {
    loadLink(app);
    refreshMine(); // to show what the project is published as
    if (app.project() == nullptr) {
        ImGui::TextDisabled("Open a project to publish it.");
        return;
    }
    const GameInfo* linked = nullptr;
    for (const GameInfo& g : m_mine) {
        if (g.id == m_linkedGame) linked = &g;
    }
    if (m_linkedGame != 0) {
        ImGui::Text("This project is published as game %lld%s", static_cast<long long>(m_linkedGame),
                    linked != nullptr ? std::format(": '{}', version {}", linked->title, linked->latestVersion).c_str()
                                      : "");
        if (linked != nullptr && linked->hidden) {
            ImGui::TextColored(ui::kErrorColor, "Taken down by a moderator: %s", linked->moderationNote.c_str());
        }
    } else {
        ImGui::TextWrapped("Publishing uploads the saved project. The platform checks it (every script, scene and "
                           "image) before anyone can play it; then it appears under Play for everyone.");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        ImGui::InputText("Title", &m_title);
        ImGui::InputTextMultiline("Description", &m_description, {ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 4.0f});
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        ImGui::Combo("Visibility", &m_visibility, kVisibilities, 3);
        ui::tooltip("Public: listed under Play. Unlisted: playable by id. Private: only you.");
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
    ImGui::InputText("Notes", &m_notes);
    ui::tooltip("What changed in this version (shown to you in the version list)");
    ImGui::BeginDisabled(busy() || (m_linkedGame == 0 && m_title.empty()));
    if (ImGui::Button(m_linkedGame != 0 ? "Publish New Version" : "Create Game and Publish")) {
        if (app.saveAll()) {
            const std::filesystem::path root = app.project()->root();
            const std::int64_t gameId = m_linkedGame;
            const std::string title = m_title;
            const std::string description = m_description;
            const auto visibility = static_cast<Visibility>(m_visibility);
            const std::string notes = m_notes;
            run("Packaging and checking the game...", [=](PlatformClient& c) -> Apply {
                auto package = project::collectPackage(root);
                if (!package) return [e = package.error()](PlatformPanel& p, StudioApp&) { p.m_error = e.message; };
                std::int64_t id = gameId;
                if (id == 0) {
                    auto game = c.createGame(title, description, visibility);
                    if (!game) return [e = game.error()](PlatformPanel& p, StudioApp&) { p.m_error = e.message; };
                    id = game->id;
                }
                auto version = c.publish(id, project::encodePackage(package.value()), notes);
                return [version, id](PlatformPanel& p, StudioApp& a) {
                    p.m_linkedGame = id;
                    p.saveLink();
                    p.m_refreshMine = true;
                    if (!version) {
                        p.m_error = version.error().message;
                        return;
                    }
                    p.m_notes.clear();
                    p.m_status = std::format("Published version {} of game {} ({} KB){}", version->number, id,
                                             (version->bytes + 1023) / 1024,
                                             version->scriptErrors > 0 ? "; it had script errors when tried" : "");
                    a.notify(p.m_status);
                };
            });
        }
    }
    ImGui::EndDisabled();
    if (m_linkedGame != 0) {
        ImGui::SameLine();
        if (ImGui::Button("Unlink")) {
            m_linkedGame = 0;
            saveLink();
        }
        ui::tooltip("Publish this project as a different game next time");
    }
}

void PlatformPanel::launchPlayer(StudioApp& app, std::int64_t gameId) {
    const auto player = MultiplayerTest::findProgram("px-player", PX_PLAYER_PATH);
    if (player.empty()) {
        app.notify("px-player was not found next to Studio", true);
        return;
    }
    const std::string exe = player.string();
    const std::string id = std::to_string(gameId);
    const char* argv[] = {exe.c_str(), "--platform", m_url.c_str(), "--play", id.c_str(), nullptr};
    SDL_Process* process = SDL_CreateProcess(argv, false);
    if (process == nullptr) {
        app.notify(std::format("Could not start px-player: {}", SDL_GetError()), true);
        return;
    }
    SDL_DestroyProcess(process); // the game keeps running on its own
    app.notify(std::format("Starting game {}...", gameId));
}

void PlatformPanel::drawGameRow(StudioApp& app, const GameInfo& g, bool mine) {
    ImGui::PushID(static_cast<int>(g.id));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("%s", g.title.c_str());
    if (!g.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g.description.c_str());
    ImGui::TableNextColumn();
    if (mine) {
        ImGui::Text("v%d, %s%s", g.latestVersion, std::string(online::visibilityName(g.visibility)).c_str(),
                    g.hidden ? ", HIDDEN" : "");
        if (g.hidden && ImGui::IsItemHovered()) ImGui::SetTooltip("Moderator: %s", g.moderationNote.c_str());
    } else {
        ImGui::Text("%s", g.ownerName.c_str());
    }
    ImGui::TableNextColumn();
    ImGui::Text("%lld plays, %d now", static_cast<long long>(g.plays), g.playing);
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(g.latestVersion == 0);
    if (ImGui::SmallButton("Play")) launchPlayer(app, g.id);
    ImGui::EndDisabled();
    if (!mine) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Report")) {
            m_reportGame = g.id;
            m_reportTitle = g.title;
            m_reportReason.clear();
            ImGui::OpenPopup("Report a game");
        }
    } else {
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) {
            m_editGame = g.id;
            m_editTitle = g.title;
            m_editDescription = g.description;
            m_editVisibility = static_cast<int>(g.visibility);
            m_editMaxPlayers = g.maxPlayers;
        }
    }
    ImGui::PopID();
}

void PlatformPanel::refreshMine() {
    if (m_refreshMine && !busy()) {
        m_refreshMine = false;
        run("Loading your games...", [](PlatformClient& c) -> Apply {
            auto games = c.myGames();
            return [games](PlatformPanel& p, StudioApp&) {
                if (games) {
                    p.m_mine = games.value();
                } else {
                    p.m_error = games.error().message;
                }
            };
        });
    }
}

void PlatformPanel::drawMyGames(StudioApp& app) {
    refreshMine();
    if (ImGui::Button("Refresh")) m_refreshMine = true;
    if (m_mine.empty()) ImGui::TextDisabled("You have not published anything yet (Publish tab).");
    if (ImGui::BeginTable("mine", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const GameInfo& g : m_mine) drawGameRow(app, g, true);
        ImGui::EndTable();
    }
    if (m_editGame != 0) {
        ImGui::SeparatorText(std::format("Game {}", m_editGame).c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        ImGui::InputText("Title##edit", &m_editTitle);
        ImGui::InputTextMultiline("Description##edit", &m_editDescription,
                                  {ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 4.0f});
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        ImGui::Combo("Visibility##edit", &m_editVisibility, kVisibilities, 3);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
        ImGui::SliderInt("Players per server", &m_editMaxPlayers, 1, 64);
        if (ImGui::Button("Save Settings") && !busy()) {
            online::GameUpdate update;
            update.title = m_editTitle;
            update.description = m_editDescription;
            update.visibility = static_cast<Visibility>(m_editVisibility);
            update.maxPlayers = m_editMaxPlayers;
            const std::int64_t id = m_editGame;
            run("Saving...", [update, id](PlatformClient& c) -> Apply {
                auto r = c.updateGame(id, update);
                return [r](PlatformPanel& p, StudioApp&) {
                    if (!r) {
                        p.m_error = r.error().message;
                        return;
                    }
                    p.m_status = "Saved";
                    p.m_editGame = 0;
                    p.m_refreshMine = true;
                };
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) m_editGame = 0;
        ImGui::SeparatorText("A player's saved data");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::InputText("Player", &m_dataPlayer);
        ImGui::SameLine();
        if (ImGui::Button("Show") && !busy() && !m_dataPlayer.empty()) {
            const std::int64_t id = m_editGame;
            const std::string who = m_dataPlayer;
            run("Loading saved data...", [id, who](PlatformClient& c) -> Apply {
                auto data = c.playerData(id, who);
                return [data](PlatformPanel& p, StudioApp&) {
                    p.m_dataText = data ? data.value() : data.error().message;
                };
            });
        }
        if (!m_dataText.empty()) ImGui::TextWrapped("%s", m_dataText.c_str());
    }
}

void PlatformPanel::drawBrowse(StudioApp& app) {
    if (m_refreshBrowse && !busy()) {
        m_refreshBrowse = false;
        online::GameQuery q;
        q.search = m_search;
        q.sort = m_sort == 0 ? "popular" : m_sort == 1 ? "playing" : "recent";
        run("Loading games...", [q](PlatformClient& c) -> Apply {
            auto games = c.games(q);
            return [games](PlatformPanel& p, StudioApp&) {
                if (games) {
                    p.m_browse = games.value();
                } else {
                    p.m_error = games.error().message;
                }
            };
        });
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::InputTextWithHint("##search", "Search", &m_search, ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_refreshBrowse = true;
    }
    ImGui::SameLine();
    const char* sorts[] = {"Popular", "Playing now", "Recently updated"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    if (ImGui::Combo("##sort", &m_sort, sorts, 3)) m_refreshBrowse = true;
    ImGui::SameLine();
    if (ImGui::Button("Refresh##browse")) m_refreshBrowse = true;
    if (m_browse.empty()) ImGui::TextDisabled("No games found.");
    if (ImGui::BeginTable("browse", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const GameInfo& g : m_browse) drawGameRow(app, g, false);
        ImGui::EndTable();
    }
}

void PlatformPanel::drawReportPopup() {
    if (m_reportGame != 0 && !ImGui::IsPopupOpen("Report a game")) ImGui::OpenPopup("Report a game");
    if (!ImGui::BeginPopupModal("Report a game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("What is wrong with '%s'?", m_reportTitle.c_str());
    ImGui::InputTextMultiline("##reason", &m_reportReason, {ImGui::GetFontSize() * 20.0f, ImGui::GetFontSize() * 4.0f});
    ImGui::BeginDisabled(m_reportReason.empty() || busy());
    if (ImGui::Button("Send Report")) {
        const std::int64_t id = m_reportGame;
        const std::string reason = m_reportReason;
        run("Sending the report...", [id, reason](PlatformClient& c) -> Apply {
            auto r = c.report(id, reason);
            return [r](PlatformPanel& p, StudioApp&) {
                if (r) {
                    p.m_status = "Thank you; a moderator will look at it";
                } else {
                    p.m_error = r.error().message;
                }
            };
        });
        m_reportGame = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_reportGame = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void PlatformPanel::drawModeration() {
    if (m_refreshReports && !busy()) {
        m_refreshReports = false;
        run("Loading reports...", [](PlatformClient& c) -> Apply {
            auto reports = c.reports(true);
            auto audit = c.audit(50);
            return [reports, audit](PlatformPanel& p, StudioApp&) {
                if (reports) p.m_reports = reports.value();
                if (audit) p.m_audit = audit.value();
                if (!reports) p.m_error = reports.error().message;
            };
        });
    }
    if (ImGui::Button("Refresh##reports")) m_refreshReports = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ImGui::InputTextWithHint("##note", "Note for the audit log", &m_moderationNote);
    const auto act = [this](std::string what, std::function<Status(PlatformClient&)> call) {
        run(std::move(what), [call](PlatformClient& c) -> Apply {
            const Status done = call(c);
            return [done](PlatformPanel& p, StudioApp&) {
                if (!done) p.m_error = done.error().message;
                p.m_refreshReports = true;
            };
        });
    };
    ImGui::SeparatorText(std::format("Open reports ({})", m_reports.size()).c_str());
    if (ImGui::BeginTable("reports", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& r : m_reports) {
            ImGui::PushID(static_cast<int>(r.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("'%s' (game %lld)", r.gameTitle.c_str(), static_cast<long long>(r.gameId));
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s: %s (%s)", r.reporter.c_str(), r.reason.c_str(), ago(r.created).c_str());
            ImGui::TableNextColumn();
            const std::string note = m_moderationNote;
            const std::int64_t id = r.id;
            if (ImGui::SmallButton("Dismiss")) act("Resolving...", [id, note](PlatformClient& c) { return c.resolveReport(id, "dismiss", note); });
            ImGui::SameLine();
            if (ImGui::SmallButton("Hide Game")) act("Hiding...", [id, note](PlatformClient& c) { return c.resolveReport(id, "hide_game", note); });
            ImGui::SameLine();
            if (ImGui::SmallButton("Ban Owner")) act("Banning...", [id, note](PlatformClient& c) { return c.resolveReport(id, "ban_owner", note); });
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Games and accounts");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    ImGui::InputScalar("Game id", ImGuiDataType_S64, &m_moderateGame);
    ImGui::SameLine();
    const std::string note = m_moderationNote;
    const std::int64_t game = m_moderateGame;
    if (ImGui::Button("Hide")) act("Hiding...", [game, note](PlatformClient& c) { return c.setGameHidden(game, true, note); });
    ImGui::SameLine();
    if (ImGui::Button("Unhide")) act("Unhiding...", [game, note](PlatformClient& c) { return c.setGameHidden(game, false, note); });
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    ImGui::InputText("Account", &m_moderateUser);
    ImGui::SameLine();
    const std::string user = m_moderateUser;
    if (ImGui::Button("Ban")) act("Banning...", [user, note](PlatformClient& c) { return c.setUserBanned(user, true, note); });
    ImGui::SameLine();
    if (ImGui::Button("Unban")) act("Unbanning...", [user, note](PlatformClient& c) { return c.setUserBanned(user, false, note); });
    ImGui::SeparatorText("Audit log");
    for (const auto& e : m_audit) {
        ImGui::TextDisabled("%s", ago(e.at).c_str());
        ImGui::SameLine();
        ImGui::TextWrapped("%s: %s %s %s", e.actor.c_str(), e.action.c_str(), e.target.c_str(), e.detail.c_str());
    }
}

} // namespace px::studio
