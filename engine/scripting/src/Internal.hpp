#pragma once

// Declarations shared between the scripting module's source files.

#include "px/scripting/ScriptTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace px::scripting {

class ScriptSystem;

// Registers the gameplay API (Api.cpp) into a VM before it is sandboxed.
void registerScriptApi(lua_State* L, ScriptSystem& system);

} // namespace px::scripting
