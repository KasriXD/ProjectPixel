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
    // Visual scripts: per line of the generated Luau, the node it came from
    // ("node 'Add Score' (n4)", or empty), so errors can name the node.
    std::vector<std::string> lineNodes;

    // `message` ("name:12: ...") with the line replaced by the node, if known.
    [[nodiscard]] std::string locate(const std::string& message) const;
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
    // Compiles a visual script (graph JSON) to Luau and adds it. Problems in
    // the graph are errors naming the node.
    [[nodiscard]] Status addGraph(assets::AssetId id, std::string name, std::string_view graphJson,
                                  const ScriptLimits& limits = {});
    [[nodiscard]] const CompiledScript* findByName(std::string_view name) const;

    [[nodiscard]] const CompiledScript* find(assets::AssetId id) const;
    [[nodiscard]] std::size_t size() const { return m_scripts.size(); }
    [[nodiscard]] const std::vector<CompiledScript>& scripts() const { return m_scripts; }

private:
    std::vector<CompiledScript> m_scripts;
    std::unordered_map<assets::AssetId, std::size_t> m_index;
};

// Reads and compiles every script and visual script (graph) of a project.
[[nodiscard]] Result<std::shared_ptr<const ScriptLibrary>> loadScriptLibrary(const IFileSystem& files,
                                                                           const assets::AssetRegistry& assets);

} // namespace px::scripting
