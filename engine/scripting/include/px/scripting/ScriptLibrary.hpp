#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"
#include "px/scripting/ScriptTypes.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::scripting {

struct CompiledScript {
    assets::AssetId id;
    std::string name;     // file path inside the project, used in messages
    std::string bytecode; // Luau bytecode, produced by our compiler only
};

// The compiled scripts of a project. Compiling happens at load time, so
// syntax errors are reported before the game starts. Immutable once built
// and shareable between sessions (restart, multiple server sessions).
//
// Bytecode is only ever produced here from source. The VM never loads
// bytecode from untrusted sources: Luau's loader does not validate
// bytecode, so malicious bytecode could corrupt memory.
class ScriptLibrary {
public:
    // Compiles `source`. Syntax errors return "name:line: message".
    [[nodiscard]] Status add(assets::AssetId id, std::string name, std::string_view source,
                             const ScriptLimits& limits = {});

    [[nodiscard]] const CompiledScript* find(assets::AssetId id) const;
    [[nodiscard]] std::size_t size() const { return m_scripts.size(); }
    [[nodiscard]] const std::vector<CompiledScript>& scripts() const { return m_scripts; }

private:
    std::vector<CompiledScript> m_scripts;
    std::unordered_map<assets::AssetId, std::size_t> m_index;
};

// Reads and compiles every script asset of a project.
[[nodiscard]] Result<std::shared_ptr<const ScriptLibrary>> loadScriptLibrary(const IFileSystem& files,
                                                                           const assets::AssetRegistry& assets);

} // namespace px::scripting
