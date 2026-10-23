# Editor model used by Studio: projects, scene documents with undo/redo,
# picking, play mode and the console log. No UI and no graphics, so it is
# unit tested headlessly and is not linked into px-player or px-headless.
px_add_module(editor
    SOURCES
        src/EditorProject.cpp
        src/Library.cpp
        src/LogBuffer.cpp
        src/Picking.cpp
        src/PlayMode.cpp
        src/SceneDocument.cpp
    DEPS
        px::core
        px::assets
        px::scene
        px::project
        px::scripting
        px::runtime)
