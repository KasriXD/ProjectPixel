#include "px/scripting/ScriptVm.hpp"

#include <lua.h>
#include <lualib.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace px::scripting {
namespace {

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Standard libraries available to scripts. Deliberately absent: os (clock,
// time, date: non-deterministic), debug (introspection), and the
// experimental integer library.
struct Library {
    const char* name;
    lua_CFunction open;
};
constexpr Library kLibraries[] = {
    {"", luaopen_base},          {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_BITLIBNAME, luaopen_bit32}, {LUA_BUFFERLIBNAME, luaopen_buffer}, {LUA_VECLIBNAME, luaopen_vector},
};

// Base-library globals removed from the sandbox, with the reason.
constexpr const char* kRemovedGlobals[] = {
    "getfenv",        // reads environments of other functions
    "setfenv",        // replaces environments; also disables VM optimisations
    "loadstring",     // runtime compilation of arbitrary strings (not compiled in, removed defensively)
    "collectgarbage", // GC control belongs to the engine
    "gcinfo",         // exposes engine memory details
    "newproxy",       // userdata with custom metatables is not needed by creator code
    "print",          // replaced by the engine's logging print (registered by the API)
};

struct CallContext {
    int functionRef;
    const ScriptVm::ArgPusher* pushArgs;
    int nresults;
};

struct LoadContext {
    const CompiledScript* script;
};

// Runs inside lua_pcall: creates an isolated environment for the module,
// loads its bytecode and runs the top-level code.
int moduleLoader(lua_State* L) {
    const auto* ctx = static_cast<const LoadContext*>(lua_tolightuserdata(L, 1));
    lua_State* thread = lua_newthread(L);
    luaL_sandboxthread(thread);
    const std::string chunkName = "@" + ctx->script->name;
    if (luau_load(thread, chunkName.c_str(), ctx->script->bytecode.data(), ctx->script->bytecode.size(), 0) != 0) {
        lua_xmove(thread, L, 1);
        lua_error(L);
    }
    lua_xmove(thread, L, 1); // the chunk function
    lua_call(L, 0, 1);
    return 1;
}

} // namespace

ScriptVm::ScriptVm(Realm realm, const ScriptLimits& limits, void* owner)
    : m_realm(realm), m_limits(limits), m_owner(owner) {}

ScriptVm::~ScriptVm() {
    if (m_L != nullptr) lua_close(m_L);
}

ScriptVm& ScriptVm::from(lua_State* L) { return *static_cast<ScriptVm*>(lua_callbacks(L)->userdata); }

void* ScriptVm::allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* vm = static_cast<ScriptVm*>(ud);
    if (nsize == 0) {
        if (ptr != nullptr) {
            vm->m_memoryUsed -= osize;
            std::free(ptr);
        }
        return nullptr;
    }
    const std::size_t oldSize = ptr != nullptr ? osize : 0;
    if (nsize > oldSize && vm->m_memoryUsed + (nsize - oldSize) > vm->m_limits.memoryBytes) {
        vm->m_memoryFailed = true;
        return nullptr; // Luau raises "not enough memory" in the running script
    }
    void* block = std::realloc(ptr, nsize);
    if (block == nullptr) {
        vm->m_memoryFailed = true;
        return nullptr;
    }
    vm->m_memoryUsed = vm->m_memoryUsed - oldSize + nsize;
    if (vm->m_memoryUsed > vm->m_peakMemory) vm->m_peakMemory = vm->m_memoryUsed;
    return block;
}

namespace {

// Raises an error located at the running script line. The interrupt runs
// inside the script's own frame (level 0), unlike a C function called by the
// script, so luaL_error's default location (level 1) would point elsewhere.
[[noreturn]] void raiseAtCurrentLine(lua_State* L, const std::string& message) {
    lua_Debug ar{};
    std::string located = message;
    if (lua_getinfo(L, 0, "sl", &ar) != 0 && ar.currentline > 0) {
        located = std::string(ar.short_src) + ":" + std::to_string(ar.currentline) + ": " + message;
    }
    lua_pushlstring(L, located.data(), located.size());
    lua_error(L);
}

} // namespace

void ScriptVm::interrupt(lua_State* L, int gc) {
    if (gc >= 0) return; // called from the garbage collector: must not raise errors
    ScriptVm& vm = from(L);
    if (!vm.m_inCall) return;
    if (++vm.m_steps > vm.m_limits.stepsPerCallback) {
        vm.m_limitHit = true;
        raiseAtCurrentLine(L, "script exceeded its CPU budget of " + std::to_string(vm.m_limits.stepsPerCallback) +
                                  " steps in one callback (endless loop?)");
    }
    if ((vm.m_steps & 1023u) == 0 && nowSeconds() > vm.m_deadline) {
        vm.m_limitHit = true;
        raiseAtCurrentLine(L, "script ran longer than " +
                                  std::to_string(static_cast<int>(vm.m_limits.secondsPerCallback * 1000.0)) +
                                  " ms in one callback");
    }
}

int ScriptVm::errorHandler(lua_State* L) {
    ScriptVm& vm = from(L);
    const char* trace = lua_debugtrace(L);
    vm.m_traceback = trace != nullptr ? trace : "";
    return 1; // keep the original error value
}

int ScriptVm::trampoline(lua_State* L) {
    const auto* ctx = static_cast<const CallContext*>(lua_tolightuserdata(L, 1));
    lua_getref(L, ctx->functionRef);
    const int nargs = ctx->pushArgs != nullptr ? (*ctx->pushArgs)(L) : 0;
    lua_call(L, nargs, ctx->nresults);
    return ctx->nresults;
}

Result<std::unique_ptr<ScriptVm>> ScriptVm::create(Realm realm, const ScriptLimits& limits, std::uint64_t seed,
                                                    const ApiRegistrar& registerApi, void* owner) {
    std::unique_ptr<ScriptVm> vm(new ScriptVm(realm, limits, owner));
    lua_State* L = lua_newstate(&ScriptVm::allocate, vm.get());
    if (L == nullptr) return makeError(ErrorCode::LimitExceeded, "Could not create the script VM (out of memory)");
    vm->m_L = L;
    lua_Callbacks* callbacks = lua_callbacks(L);
    callbacks->userdata = vm.get();
    callbacks->interrupt = &ScriptVm::interrupt;

    for (const Library& lib : kLibraries) {
        lua_pushcfunction(L, lib.open, nullptr);
        lua_pushstring(L, lib.name);
        lua_call(L, 1, 0);
    }
    for (const char* name : kRemovedGlobals) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    // Deterministic random numbers: identical on every machine for a given seed.
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, static_cast<int>(seed & 0x7fffffffu));
    lua_call(L, 1, 0);
    lua_pop(L, 1);

    if (registerApi) registerApi(L);

    lua_pushcfunction(L, &ScriptVm::errorHandler, "errorHandler");
    vm->m_errorHandlerRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushcfunction(L, &ScriptVm::trampoline, "call");
    vm->m_trampolineRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushcfunction(L, &moduleLoader, "load");
    vm->m_loaderRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushcfunction(L, [](lua_State*) -> int { return 0; }, "noop");
    vm->m_noopRef = lua_ref(L, -1);
    lua_pop(L, 1);

    // Make every global table read-only and enable safe-environment optimisations.
    luaL_sandbox(L);
    // Freeze the shared vector metatable too, so no script can change vector behaviour.
    lua_pushvector(L, 0.0f, 0.0f, 0.0f);
    if (lua_getmetatable(L, -1) != 0) {
        lua_setreadonly(L, -1, true);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return vm;
}

ScriptVm::CallResult ScriptVm::call(int functionRef, const ArgPusher& pushArgs) {
    const int top = lua_gettop(m_L);
    CallResult result = protectedCall(functionRef, pushArgs ? &pushArgs : nullptr, 0);
    lua_settop(m_L, top);
    return result;
}

ScriptVm::CallResult ScriptVm::runProtected(const ArgPusher& body) {
    // The trampoline runs `body` while "pushing arguments", then calls a no-op.
    const ArgPusher wrapper = [&body](lua_State* L) {
        const int top = lua_gettop(L);
        body(L);
        lua_settop(L, top);
        return 0;
    };
    return call(m_noopRef, wrapper);
}

ScriptVm::CallResult ScriptVm::protectedCall(int functionRef, const ArgPusher* pushArgs, int nresults) {
    CallResult result;
    if (m_inCall) {
        result.ok = false;
        result.message = "internal error: nested script call";
        return result;
    }
    CallContext ctx{functionRef, pushArgs, nresults};
    m_inCall = true;
    m_steps = 0;
    m_deadline = nowSeconds() + m_limits.secondsPerCallback;
    m_limitHit = false;
    m_memoryFailed = false;
    m_traceback.clear();

    const int top = lua_gettop(m_L);
    lua_getref(m_L, m_errorHandlerRef);
    lua_getref(m_L, m_trampolineRef);
    lua_pushlightuserdata(m_L, &ctx);
    const int status = lua_pcall(m_L, 1, nresults, top + 1);
    m_inCall = false;

    if (status == LUA_OK) {
        lua_remove(m_L, top + 1); // the error handler; results stay on the stack
        return result;
    }
    result.ok = false;
    result.limitExceeded = m_limitHit || status == LUA_ERRMEM || m_memoryFailed;
    if (status == LUA_ERRMEM) {
        result.message = "script ran out of memory (the limit for all scripts is " +
                         std::to_string(m_limits.memoryBytes / (1024 * 1024)) + " MiB)";
    } else {
        const char* message = lua_tostring(m_L, -1);
        result.message = message != nullptr ? message : "error object is not a string";
    }
    result.traceback = m_traceback;
    lua_settop(m_L, top);
    if (result.limitExceeded) {
        // Reclaim garbage left by the failed script before anything else runs.
        lua_gc(m_L, LUA_GCCOLLECT, 0);
    }
    return result;
}

Result<int> ScriptVm::loadModule(const CompiledScript& script) {
    // The module's top-level code runs through the same protected, limited
    // path as any callback.
    LoadContext load{&script};
    const ArgPusher push = [&load](lua_State* L) {
        lua_pushlightuserdata(L, &load);
        return 1;
    };
    const int top = lua_gettop(m_L);
    CallResult result = protectedCall(m_loaderRef, &push, 1);
    if (!result.ok) {
        return makeError(result.limitExceeded ? ErrorCode::LimitExceeded : ErrorCode::InvalidData, "{}",
                         result.message);
    }
    const int moduleRef = lua_ref(m_L, -1);
    lua_settop(m_L, top);
    return moduleRef;
}

void ScriptVm::releaseRef(int ref) {
    if (ref > 0 && m_L != nullptr) lua_unref(m_L, ref);
}

} // namespace px::scripting
