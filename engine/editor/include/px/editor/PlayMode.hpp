#pragma once

#include "px/core/Result.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/runtime/Input.hpp"
#include "px/scene/Scene.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <memory>

namespace px::editor {

class EditorProject;

// A game started from the editor ("Play"). It runs a copy of the scene as
// it is in the editor, unsaved changes and unsaved script edits included,
// exactly as px-player would run it. Stopping discards the copy, so play
// never changes the edited scene.
class PlayMode {
public:
    [[nodiscard]] static Result<std::unique_ptr<PlayMode>> start(const EditorProject& project,
                                                                 const scene::Scene& scene,
                                                                 const runtime::SessionOptions& options = {});

    [[nodiscard]] runtime::GameSession& session() { return *m_session; }
    [[nodiscard]] const runtime::GameSession& session() const { return *m_session; }

    // Advances one fixed tick, unless paused (then only after requestStep()).
    // Returns whether a tick ran.
    bool advance(const runtime::InputFrame& input);
    void setPaused(bool paused) { m_paused = paused; }
    [[nodiscard]] bool paused() const { return m_paused; }
    void requestStep() { m_stepRequested = true; }

private:
    PlayMode() = default;
    std::shared_ptr<const scripting::ScriptLibrary> m_scripts;
    std::unique_ptr<runtime::GameSession> m_session;
    bool m_paused = false;
    bool m_stepRequested = false;
};

} // namespace px::editor
