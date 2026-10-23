#include "px/editor/PlayMode.hpp"

#include "px/editor/EditorProject.hpp"

namespace px::editor {

Result<std::unique_ptr<PlayMode>> PlayMode::start(const EditorProject& project, const scene::Scene& scene,
                                                  const runtime::SessionOptions& options) {
    auto scripts = project.buildScriptLibrary();
    if (!scripts) return scripts.error();
    std::unique_ptr<PlayMode> play(new PlayMode());
    play->m_scripts = std::move(scripts.value());
    auto session = runtime::GameSession::create(project.manifest(), scene, play->m_scripts, options);
    if (!session) return session.error();
    play->m_session = std::move(session.value());
    return play;
}

bool PlayMode::advance(const runtime::InputFrame& input) {
    if (m_paused && !m_stepRequested) return false;
    m_stepRequested = false;
    m_session->tick(input);
    return true;
}

} // namespace px::editor
