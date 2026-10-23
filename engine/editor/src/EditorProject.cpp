#include "px/editor/EditorProject.hpp"

#include "px/assets/Image.hpp"
#include "px/core/Log.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <system_error>

namespace px::editor {

namespace {

constexpr std::size_t kMaxNameLength = 100;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

Status checkName(std::string_view name, std::string_view what) {
    if (name.empty()) return makeError(ErrorCode::InvalidArgument, "The {} name may not be empty", what);
    if (name.size() > kMaxNameLength) {
        return makeError(ErrorCode::InvalidArgument, "The {} name is longer than {} characters", what, kMaxNameLength);
    }
    return {};
}

Result<std::vector<std::uint8_t>> readExternalFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return makeError(ErrorCode::NotFound, "{} is not a file", path.generic_string());
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return makeError(ErrorCode::IoError, "Cannot read {}: {}", path.generic_string(), ec.message());
    if (size > IFileSystem::kMaxFileSize) {
        return makeError(ErrorCode::LimitExceeded, "{} is larger than {} MiB", path.generic_string(),
                         IFileSystem::kMaxFileSize / (1024 * 1024));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) return makeError(ErrorCode::IoError, "Cannot open {}", path.generic_string());
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        return makeError(ErrorCode::IoError, "Failed while reading {}", path.generic_string());
    }
    return data;
}

// Relative path of `file` inside `root`, if it is inside.
std::optional<std::string> pathInside(const std::filesystem::path& root, const std::filesystem::path& file) {
    std::error_code ec;
    const auto r = std::filesystem::weakly_canonical(root, ec);
    if (ec) return std::nullopt;
    const auto f = std::filesystem::weakly_canonical(file, ec);
    if (ec) return std::nullopt;
    const auto rel = f.lexically_relative(r);
    if (rel.empty() || rel.native().starts_with(std::filesystem::path("..").native())) return std::nullopt;
    auto normalized = normalizeRelativePath(rel.generic_string());
    if (!normalized) return std::nullopt;
    return normalized.value();
}

std::string scriptTemplate(std::string_view displayName, const std::string& identifier) {
    return std::format(R"lua(-- {0}: describe what this behaviour does.
-- Attach it to an entity in Studio (inspector > Add Script). Properties
-- declared below appear in the inspector and can be set per entity.
-- API reference: docs/scripting-api.md

local {1} = {{
    properties = {{
        speed = 60,
    }},
}}

function {1}:Start()
    -- Runs once, before the first Tick.
end

function {1}:Tick(dt)
    -- Runs every simulation tick; dt is the tick length in seconds.
end

return {1}
)lua",
                       displayName, identifier);
}

scene::Scene newSceneContent(std::string_view name, Vec2i resolution) {
    scene::Scene scene;
    scene.setName(std::string(name));
    const scene::Entity camera = scene.createEntity("Main Camera");
    scene.set(camera, scene::Transform{{static_cast<float>(resolution.x) * 0.5f, static_cast<float>(resolution.y) * 0.5f}});
    scene::Camera cam;
    cam.clampToBounds = true;
    scene.set(camera, cam);
    return scene;
}

} // namespace

std::string toFileStem(std::string_view name) {
    std::string out;
    bool pendingSeparator = false;
    for (const char raw : name) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 && c < 128) {
            if (pendingSeparator && !out.empty()) out += '_';
            pendingSeparator = false;
            out += static_cast<char>(std::tolower(c));
        } else {
            pendingSeparator = true;
        }
        if (out.size() >= 48) break;
    }
    return out;
}

std::string toIdentifier(std::string_view name) {
    std::string out;
    bool upperNext = true;
    for (const char raw : name) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 && c < 128) {
            out += upperNext ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
            upperNext = false;
        } else {
            upperNext = true;
        }
        if (out.size() >= 48) break;
    }
    if (out.empty()) return "Behaviour";
    if (std::isdigit(static_cast<unsigned char>(out.front())) != 0) out.insert(out.begin(), 'B');
    return out;
}

EditorProject::EditorProject(std::filesystem::path root, project::ProjectManifest manifest)
    : m_root(std::move(root)), m_files(m_root), m_manifest(std::move(manifest)) {}

Result<std::unique_ptr<EditorProject>> EditorProject::open(const std::filesystem::path& folder) {
    std::error_code ec;
    const auto root = std::filesystem::absolute(folder, ec).lexically_normal();
    if (ec) return makeError(ErrorCode::InvalidArgument, "Invalid folder {}", folder.generic_string());
    DirectoryFileSystem fs(root);
    if (!fs.exists(project::kProjectFileName)) {
        return makeError(ErrorCode::NotFound, "{} does not contain a project ({} is missing)", root.generic_string(),
                         project::kProjectFileName);
    }
    auto manifest = project::loadProjectManifest(fs);
    if (!manifest) return manifest.error();
    return std::unique_ptr<EditorProject>(new EditorProject(root, std::move(manifest.value())));
}

Result<std::unique_ptr<EditorProject>> EditorProject::create(const std::filesystem::path& folder, std::string name,
                                                             const std::optional<std::filesystem::path>& templateFolder) {
    if (auto ok = checkName(name, "project"); !ok) return ok.error();
    std::error_code ec;
    const auto root = std::filesystem::absolute(folder, ec).lexically_normal();
    if (ec) return makeError(ErrorCode::InvalidArgument, "Invalid folder {}", folder.generic_string());
    if (std::filesystem::exists(root, ec)) {
        if (!std::filesystem::is_directory(root, ec)) {
            return makeError(ErrorCode::InvalidArgument, "{} exists and is not a folder", root.generic_string());
        }
        if (!std::filesystem::is_empty(root, ec)) {
            return makeError(ErrorCode::InvalidArgument, "{} is not empty; choose a new or empty folder",
                             root.generic_string());
        }
    }
    std::filesystem::create_directories(root, ec);
    if (ec) return makeError(ErrorCode::IoError, "Cannot create {}: {}", root.generic_string(), ec.message());

    if (templateFolder) {
        std::filesystem::copy(*templateFolder, root,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_symlinks,
                              ec);
        if (ec) {
            return makeError(ErrorCode::IoError, "Cannot copy the template {}: {}", templateFolder->generic_string(),
                             ec.message());
        }
        auto opened = open(root);
        if (!opened) return opened.error();
        auto& project = *opened.value();
        project.m_manifest.id = Guid::generate();
        project.m_manifest.name = std::move(name);
        if (auto saved = project.saveManifest(); !saved) return saved.error();
        return opened;
    }

    project::ProjectManifest manifest;
    manifest.id = Guid::generate();
    manifest.name = std::move(name);
    manifest.input.actions = {{"move_left", {Key::Left, Key::A}},
                              {"move_right", {Key::Right, Key::D}},
                              {"jump", {Key::Space, Key::Z, Key::Up, Key::W}}};
    std::unique_ptr<EditorProject> project(new EditorProject(root, std::move(manifest)));
    auto scene = project->createScene("Main");
    if (!scene) return scene.error();
    return project;
}

// --- Manifest ----------------------------------------------------------------------------------

Status EditorProject::saveManifest() {
    const std::string text = project::serializeProjectManifest(m_manifest);
    if (auto check = project::parseProjectManifest(text, project::kProjectFileName); !check) {
        return makeError(ErrorCode::InvalidData, "The project settings are invalid and were not saved: {}",
                         check.error().message);
    }
    if (auto written = m_files.writeText(project::kProjectFileName, text); !written) return written;
    m_manifestDirty = false;
    return {};
}

Status EditorProject::reloadManifest() {
    auto manifest = project::loadProjectManifest(m_files);
    if (!manifest) return manifest.error();
    m_manifest = std::move(manifest.value());
    m_manifestDirty = false;
    m_scriptsStale = true;
    return {};
}

// --- Assets -----------------------------------------------------------------------------------

Result<std::string> EditorProject::uniquePath(std::string_view folder, std::string_view stem,
                                              std::string_view extension) const {
    const std::string base = stem.empty() ? std::string("asset") : std::string(stem);
    for (int n = 1; n < 10000; ++n) {
        const std::string candidate =
            std::format("{}/{}{}{}", folder, base, n == 1 ? std::string() : std::format("-{}", n), extension);
        auto normalized = normalizeRelativePath(candidate);
        if (!normalized) return normalized.error();
        const bool registered = std::any_of(m_manifest.assets.entries().begin(), m_manifest.assets.entries().end(),
                                            [&](const assets::AssetEntry& e) { return e.path == normalized.value(); });
        if (!registered && !m_files.exists(normalized.value())) return normalized.value();
    }
    return makeError(ErrorCode::LimitExceeded, "Could not find a free file name for {}", base);
}

Result<assets::AssetId> EditorProject::importFile(const std::filesystem::path& source) {
    const std::string fileName = source.filename().string();
    const std::string lowered = lower(fileName);
    assets::AssetType type;
    std::string folder;
    std::string extension;
    std::string stem;
    if (endsWith(lowered, ".scene.json")) {
        type = assets::AssetType::Scene;
        folder = "scenes";
        extension = ".scene.json";
        stem = fileName.substr(0, fileName.size() - extension.size());
    } else {
        const std::string ext = lower(source.extension().string());
        stem = source.stem().string();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") {
            type = assets::AssetType::Texture;
            folder = "assets";
            extension = ext;
        } else if (ext == ".luau" || ext == ".lua") {
            type = assets::AssetType::Script;
            folder = "scripts";
            extension = ".luau";
        } else {
            return makeError(ErrorCode::Unsupported,
                             "Cannot import '{}': supported files are images (.png .jpg .bmp .tga), Luau scripts "
                             "(.luau) and scenes (.scene.json)",
                             fileName);
        }
    }

    auto bytes = readExternalFile(source);
    if (!bytes) return bytes.error();
    if (type == assets::AssetType::Texture) {
        auto image = assets::decodeImage(bytes.value(), fileName);
        if (!image) return makeError(image.error().code, "Cannot import '{}': {}", fileName, image.error().message);
    } else if (type == assets::AssetType::Scene) {
        const std::string text(bytes->begin(), bytes->end());
        auto scene = project::parseScene(text, fileName, &m_manifest.assets);
        if (!scene) return makeError(scene.error().code, "Cannot import '{}': {}", fileName, scene.error().message);
    }

    // A file that is already inside the project is registered where it is.
    std::string path;
    bool copied = false;
    if (auto inside = pathInside(m_root, source)) {
        for (const auto& e : m_manifest.assets.entries()) {
            if (e.path == *inside) return e.id; // already part of the project
        }
        path = *inside;
    } else {
        auto unique = uniquePath(folder, toFileStem(stem).empty() ? std::string("asset") : toFileStem(stem), extension);
        if (!unique) return unique.error();
        path = unique.value();
        const std::string_view content(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        if (auto written = m_files.writeText(path, content); !written) return written.error();
        copied = true;
    }

    assets::AssetEntry entry;
    entry.id = assets::AssetId{Guid::generate()};
    entry.type = type;
    entry.name = stem;
    entry.path = path;
    if (auto added = m_manifest.assets.add(entry); !added) return added.error();
    if (auto saved = saveManifest(); !saved) {
        (void)m_manifest.assets.remove(entry.id);
        if (copied) {
            std::error_code ec;
            std::filesystem::remove(m_root / std::filesystem::path(path), ec);
        }
        return saved.error();
    }
    m_scriptsStale = true;
    log::info("Imported {} as {} '{}'", fileName, assets::assetTypeName(type), entry.name);
    return entry.id;
}

Result<assets::AssetId> EditorProject::createScript(std::string_view name) {
    if (auto ok = checkName(name, "script"); !ok) return ok.error();
    auto path = uniquePath("scripts", toFileStem(name).empty() ? std::string("script") : toFileStem(name), ".luau");
    if (!path) return path.error();
    if (auto written = m_files.writeText(path.value(), scriptTemplate(name, toIdentifier(name))); !written) {
        return written.error();
    }
    assets::AssetEntry entry{assets::AssetId{Guid::generate()}, assets::AssetType::Script, std::string(name),
                             path.value(), assets::TextureFilter::Nearest};
    if (auto added = m_manifest.assets.add(entry); !added) return added.error();
    if (auto saved = saveManifest(); !saved) {
        (void)m_manifest.assets.remove(entry.id);
        return saved.error();
    }
    m_scriptsStale = true;
    return entry.id;
}

Result<assets::AssetId> EditorProject::createScene(std::string_view name) {
    if (auto ok = checkName(name, "scene"); !ok) return ok.error();
    auto path = uniquePath("scenes", toFileStem(name).empty() ? std::string("scene") : toFileStem(name), ".scene.json");
    if (!path) return path.error();
    const scene::Scene content = newSceneContent(name, m_manifest.settings.display.virtualResolution);
    if (auto written = m_files.writeText(path.value(), project::serializeScene(content)); !written) {
        return written.error();
    }
    assets::AssetEntry entry{assets::AssetId{Guid::generate()}, assets::AssetType::Scene, std::string(name),
                             path.value(), assets::TextureFilter::Nearest};
    if (auto added = m_manifest.assets.add(entry); !added) return added.error();
    const bool first = m_manifest.startScene.isNil() || m_manifest.assets.find(m_manifest.startScene) == nullptr;
    if (first) m_manifest.startScene = entry.id;
    if (auto saved = saveManifest(); !saved) {
        (void)m_manifest.assets.remove(entry.id);
        return saved.error();
    }
    return entry.id;
}

Status EditorProject::renameAsset(assets::AssetId id, std::string name) {
    if (auto ok = checkName(name, "asset"); !ok) return ok;
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Asset {} does not exist", id.toString());
    const std::string previous = entry->name;
    if (auto renamed = m_manifest.assets.rename(id, std::move(name)); !renamed) return renamed;
    if (auto saved = saveManifest(); !saved) {
        (void)m_manifest.assets.rename(id, previous);
        return saved;
    }
    return {};
}

std::vector<AssetUse> EditorProject::findUses(
    assets::AssetId id, const std::vector<std::pair<assets::AssetId, const scene::Scene*>>& openScenes) const {
    std::vector<AssetUse> uses;
    for (const auto& entry : m_manifest.assets.entries()) {
        if (entry.type != assets::AssetType::Scene) continue;
        const scene::Scene* scene = nullptr;
        for (const auto& [openId, openScene] : openScenes) {
            if (openId == entry.id) scene = openScene;
        }
        std::optional<scene::Scene> loaded;
        if (scene == nullptr) {
            auto result = loadScene(entry.id);
            if (!result) continue; // a broken scene is reported when it is opened
            loaded = std::move(result.value());
            scene = &*loaded;
        }
        for (const scene::Entity e : scene->entities()) {
            const auto refs = scene::assetReferences(*scene, e);
            if (std::find(refs.begin(), refs.end(), id) == refs.end()) continue;
            const auto* info = scene->tryGet<scene::EntityInfo>(e);
            uses.push_back({entry.id, entry.name, info != nullptr ? info->name : std::string()});
        }
    }
    return uses;
}

Status EditorProject::deleteAsset(assets::AssetId id,
                                  const std::vector<std::pair<assets::AssetId, const scene::Scene*>>& openScenes) {
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Asset {} does not exist", id.toString());
    if (id == m_manifest.startScene) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is the start scene; choose another start scene first",
                         entry->name);
    }
    for (const auto& [openId, openScene] : openScenes) {
        if (openId == id) return makeError(ErrorCode::InvalidArgument, "'{}' is open; close it first", entry->name);
    }
    const auto uses = findUses(id, openScenes);
    if (!uses.empty()) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is used by {} entit{} (for example '{}' in '{}')",
                         entry->name, uses.size(), uses.size() == 1 ? "y" : "ies", uses.front().entityName,
                         uses.front().sceneName);
    }
    const std::string path = entry->path;
    const std::string name = entry->name;
    assets::AssetEntry backup = *entry;
    (void)m_manifest.assets.remove(id);
    if (auto saved = saveManifest(); !saved) {
        (void)m_manifest.assets.add(backup);
        return saved;
    }
    std::error_code ec;
    if (auto normalized = normalizeRelativePath(path)) {
        std::filesystem::remove(m_root / std::filesystem::path(normalized.value()), ec);
    }
    if (ec) log::warn("'{}' was removed from the project, but its file could not be deleted: {}", name, ec.message());
    m_scriptBuffers.erase(id);
    m_scriptInfo.erase(id);
    log::info("Deleted asset '{}' ({})", name, path);
    return {};
}

Status EditorProject::setStartScene(assets::AssetId id) {
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr || entry->type != assets::AssetType::Scene) {
        return makeError(ErrorCode::InvalidArgument, "The start scene must be a scene asset");
    }
    const assets::AssetId previous = m_manifest.startScene;
    m_manifest.startScene = id;
    if (auto saved = saveManifest(); !saved) {
        m_manifest.startScene = previous;
        return saved;
    }
    return {};
}

std::filesystem::path EditorProject::absolutePath(assets::AssetId id) const {
    const auto* entry = m_manifest.assets.find(id);
    return entry == nullptr ? std::filesystem::path() : m_root / std::filesystem::path(entry->path);
}

// --- Scenes -----------------------------------------------------------------------------------

Result<scene::Scene> EditorProject::loadScene(assets::AssetId id, std::vector<std::string>* warnings) const {
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr || entry->type != assets::AssetType::Scene) {
        return makeError(ErrorCode::NotFound, "Scene {} is not part of the project", id.toString());
    }
    auto text = m_files.readText(entry->path);
    if (!text) return text.error();
    return project::parseScene(text.value(), m_files.describe(entry->path), &m_manifest.assets, warnings);
}

Status EditorProject::saveScene(assets::AssetId id, const scene::Scene& scene) {
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr || entry->type != assets::AssetType::Scene) {
        return makeError(ErrorCode::NotFound, "Scene {} is not part of the project", id.toString());
    }
    const std::string text = project::serializeScene(scene);
    // Saving must never produce a file that does not load.
    if (auto check = project::parseScene(text, entry->path, &m_manifest.assets); !check) {
        return makeError(ErrorCode::InvalidData, "'{}' was not saved because it would not load: {}", entry->name,
                         check.error().message);
    }
    return m_files.writeText(entry->path, text);
}

// --- Scripts -----------------------------------------------------------------------------------

Result<std::string> EditorProject::readSource(assets::AssetId id) const {
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr || entry->type != assets::AssetType::Script) {
        return makeError(ErrorCode::NotFound, "Script {} is not part of the project", id.toString());
    }
    return m_files.readText(entry->path);
}

Result<std::string> EditorProject::scriptText(assets::AssetId id) const {
    if (const auto it = m_scriptBuffers.find(id); it != m_scriptBuffers.end()) return it->second.text;
    return readSource(id);
}

void EditorProject::setScriptBuffer(assets::AssetId id, std::string text) {
    auto& buffer = m_scriptBuffers[id];
    if (buffer.text == text && !buffer.text.empty()) return;
    buffer.text = std::move(text);
    auto disk = readSource(id);
    buffer.dirty = !disk || disk.value() != buffer.text;
    m_scriptsStale = true;
}

void EditorProject::discardScriptBuffer(assets::AssetId id) {
    m_scriptBuffers.erase(id);
    m_scriptsStale = true;
}

bool EditorProject::scriptBufferDirty(assets::AssetId id) const {
    const auto it = m_scriptBuffers.find(id);
    return it != m_scriptBuffers.end() && it->second.dirty;
}

bool EditorProject::anyScriptBufferDirty() const {
    return std::any_of(m_scriptBuffers.begin(), m_scriptBuffers.end(), [](const auto& b) { return b.second.dirty; });
}

Status EditorProject::saveScript(assets::AssetId id) {
    const auto it = m_scriptBuffers.find(id);
    if (it == m_scriptBuffers.end() || !it->second.dirty) return {};
    const auto* entry = m_manifest.assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Script {} is not part of the project", id.toString());
    if (auto written = m_files.writeText(entry->path, it->second.text); !written) return written;
    m_scriptBuffers.erase(it);
    m_scriptsStale = true;
    return {};
}

Status EditorProject::saveAllScripts() {
    std::vector<assets::AssetId> dirty;
    for (const auto& [id, buffer] : m_scriptBuffers) {
        if (buffer.dirty) dirty.push_back(id);
    }
    for (const auto& id : dirty) {
        if (auto saved = saveScript(id); !saved) return saved;
    }
    return {};
}

bool EditorProject::checkScriptsChangedOnDisk() {
    bool changed = false;
    for (const auto& entry : m_manifest.assets.entries()) {
        if (entry.type != assets::AssetType::Script) continue;
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(m_root / std::filesystem::path(entry.path), ec);
        if (ec) continue;
        const auto it = m_scriptTimes.find(entry.id);
        if (it == m_scriptTimes.end() || it->second != time) {
            if (it != m_scriptTimes.end()) changed = true;
            m_scriptTimes[entry.id] = time;
        }
    }
    if (changed) m_scriptsStale = true;
    return changed;
}

void EditorProject::refreshScripts() {
    m_scriptInfo.clear();
    auto library = std::make_shared<scripting::ScriptLibrary>();
    for (const auto& entry : m_manifest.assets.entries()) {
        if (entry.type != assets::AssetType::Script) continue;
        ScriptInfo info;
        auto source = scriptText(entry.id);
        if (!source) {
            info.error = source.error().message;
        } else if (auto added = library->add(entry.id, entry.path, source.value()); !added) {
            info.error = added.error().message;
        } else {
            info.compiled = true;
            // Each script is described in its own sandbox, so one broken
            // script does not hide the others.
            auto single = std::make_shared<scripting::ScriptLibrary>();
            (void)single->add(entry.id, entry.path, source.value());
            auto described = scripting::ScriptSystem::describe(single, entry.id);
            if (described) {
                info.behaviour = std::move(described.value());
            } else {
                info.error = described.error().message;
            }
        }
        m_scriptInfo.emplace(entry.id, std::move(info));
    }
    m_scriptsStale = false;
}

const ScriptInfo* EditorProject::scriptInfo(assets::AssetId id) {
    if (m_scriptsStale) refreshScripts();
    const auto it = m_scriptInfo.find(id);
    return it == m_scriptInfo.end() ? nullptr : &it->second;
}

Result<std::shared_ptr<const scripting::ScriptLibrary>> EditorProject::buildScriptLibrary() const {
    auto library = std::make_shared<scripting::ScriptLibrary>();
    for (const auto& entry : m_manifest.assets.entries()) {
        if (entry.type != assets::AssetType::Script) continue;
        auto source = scriptText(entry.id);
        if (!source) return source.error();
        if (auto added = library->add(entry.id, entry.path, source.value()); !added) return added.error();
    }
    return std::shared_ptr<const scripting::ScriptLibrary>(std::move(library));
}

} // namespace px::editor
