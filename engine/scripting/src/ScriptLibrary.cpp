#include "px/scripting/ScriptLibrary.hpp"

#include "px/scripting/Graph.hpp"

#include <luacode.h>

#include <cstdlib>
#include <format>

namespace px::scripting {

const char* realmName(Realm realm) noexcept { return realm == Realm::Authority ? "authority" : "client"; }

std::string ScriptError::describe() const {
    std::string text = std::format("Script error in {}", script);
    if (!entity.empty()) text += std::format(" (entity '{}')", entity);
    if (!phase.empty()) text += std::format(" during {}", phase);
    text += ": " + message;
    return text;
}

Status ScriptLibrary::add(assets::AssetId id, std::string name, std::string_view source, const ScriptLimits& limits) {
    if (m_index.contains(id)) {
        return makeError(ErrorCode::InvalidData, "Script {} is already loaded", name);
    }
    if (source.size() > limits.maxSourceBytes) {
        return makeError(ErrorCode::LimitExceeded, "{}: script is {} bytes; the limit is {} bytes", name,
                         source.size(), limits.maxSourceBytes);
    }

    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1; // line info for error messages and tracebacks
    size_t size = 0;
    char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
    if (bytecode == nullptr) {
        return makeError(ErrorCode::Unknown, "{}: the script compiler ran out of memory", name);
    }
    std::string result(bytecode, size);
    std::free(bytecode);

    // A failed compilation is encoded as a 0 byte followed by ":line: message".
    if (result.empty() || result[0] == 0) {
        const std::string message = result.size() > 1 ? result.substr(1) : std::string(": unknown compile error");
        return makeError(ErrorCode::InvalidData, "{}{}", name, message);
    }

    m_index.emplace(id, m_scripts.size());
    m_scripts.push_back(CompiledScript{id, std::move(name), std::move(result), {}});
    return {};
}

Status ScriptLibrary::addGraph(assets::AssetId id, std::string name, std::string_view graphJson,
                               const ScriptLimits& limits) {
    auto graph = parseGraph(graphJson, name);
    if (!graph) return graph.error();
    const CompiledGraph compiled = compileGraph(graph.value(), name);
    if (!compiled.ok()) {
        std::string message = std::format("{}: {}", name, compiled.problems.front().message);
        if (compiled.problems.size() > 1) message += std::format(" (and {} more problems)", compiled.problems.size() - 1);
        return makeError(ErrorCode::InvalidData, "{}", message);
    }
    if (auto added = add(id, name, compiled.source, limits); !added) return added;
    std::vector<std::string> lines;
    lines.reserve(compiled.lineNodes.size());
    for (const std::string& node : compiled.lineNodes) {
        lines.push_back(node.empty() ? std::string() : describeNode(graph.value(), node));
    }
    m_scripts.back().lineNodes = std::move(lines);
    return {};
}

const CompiledScript* ScriptLibrary::findByName(std::string_view name) const {
    for (const CompiledScript& s : m_scripts) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::string CompiledScript::locate(const std::string& message) const {
    if (lineNodes.empty() || !message.starts_with(name + ":")) return message;
    std::size_t p = name.size() + 1;
    int line = 0;
    while (p < message.size() && message[p] >= '0' && message[p] <= '9') line = line * 10 + (message[p++] - '0');
    if (p >= message.size() || message[p] != ':' || line < 1 || static_cast<std::size_t>(line) > lineNodes.size()) {
        return message;
    }
    const std::string& node = lineNodes[static_cast<std::size_t>(line - 1)];
    if (node.empty()) return message;
    return std::format("{}, {}:{}", name, node, message.substr(p + 1));
}

const CompiledScript* ScriptLibrary::find(assets::AssetId id) const {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_scripts[it->second];
}

Result<std::shared_ptr<const ScriptLibrary>> loadScriptLibrary(const IFileSystem& files,
                                                               const assets::AssetRegistry& assets) {
    auto library = std::make_shared<ScriptLibrary>();
    for (const auto& entry : assets.entries()) {
        if (!assets::isBehaviour(entry.type)) continue;
        auto source = files.readText(entry.path);
        if (!source) return source.error();
        const Status status = entry.type == assets::AssetType::Graph
                                  ? library->addGraph(entry.id, entry.path, source.value())
                                  : library->add(entry.id, entry.path, source.value());
        if (!status) return status.error();
    }
    return std::shared_ptr<const ScriptLibrary>(std::move(library));
}

} // namespace px::scripting
