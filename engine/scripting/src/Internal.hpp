#pragma once

// Declarations shared between the scripting module's source files.

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace px::scripting {

class ScriptSystem;

// " (did you mean 'x'?)" when a candidate is a close spelling of `name`, else "".
[[nodiscard]] std::string suggestName(std::string_view name, const std::vector<std::string>& candidates);

// Registers the gameplay API (Api.cpp) into a VM before it is sandboxed.
void registerScriptApi(lua_State* L, ScriptSystem& system);

} // namespace px::scripting
