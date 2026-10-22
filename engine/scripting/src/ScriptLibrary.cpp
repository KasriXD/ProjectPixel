#include "px/scripting/ScriptLibrary.hpp"

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
    m_scripts.push_back(CompiledScript{id, std::move(name), std::move(result)});
    return {};
}

const CompiledScript* ScriptLibrary::find(assets::AssetId id) const {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_scripts[it->second];
}

Result<std::shared_ptr<const ScriptLibrary>> loadScriptLibrary(const IFileSystem& files,
                                                               const assets::AssetRegistry& assets) {
    auto library = std::make_shared<ScriptLibrary>();
    for (const auto& entry : assets.entries()) {
        if (entry.type != assets::AssetType::Script) continue;
        auto source = files.readText(entry.path);
        if (!source) return source.error();
        if (auto status = library->add(entry.id, entry.path, source.value()); !status) return status.error();
    }
    return std::shared_ptr<const ScriptLibrary>(std::move(library));
}

} // namespace px::scripting
