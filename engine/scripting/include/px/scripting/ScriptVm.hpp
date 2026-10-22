#pragma once

#include "px/core/Result.hpp"
#include "px/scripting/ScriptLibrary.hpp"
#include "px/scripting/ScriptTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct lua_State;

namespace px::scripting {

// A sandboxed Luau virtual machine with memory and CPU limits.
//
// Sandbox (see docs/scripting.md#sandbox):
//  * Only these standard libraries exist: base (reduced), string, table,
//    math, utf8, bit32, buffer, vector, coroutine. There is no io, os,
//    debug, require, package, loadstring, getfenv/setfenv or collectgarbage.
//  * All built-in and engine tables are read-only; each script module gets
//    its own global environment, so scripts cannot interfere with each other.
//  * math.random is seeded deterministically from the session seed.
//  * Memory is capped by a custom allocator; CPU per callback is capped by
//    the VM interrupt (deterministic step count plus a wall-clock watchdog).
class ScriptVm {
public:
    // Registers engine globals into the VM before it is sandboxed.
    using ApiRegistrar = std::function<void(lua_State*)>;

    [[nodiscard]] static Result<std::unique_ptr<ScriptVm>> create(Realm realm, const ScriptLimits& limits,
                                                                  std::uint64_t seed, const ApiRegistrar& registerApi,
                                                                  void* owner);
    ~ScriptVm();
    ScriptVm(const ScriptVm&) = delete;
    ScriptVm& operator=(const ScriptVm&) = delete;

    struct CallResult {
        bool ok = true;
        bool limitExceeded = false; // CPU or memory limit was hit
        std::string message;
        std::string traceback;
    };

    // Pushes arguments for a protected call. Runs inside the protected
    // region, so it may allocate. Returns the number of values pushed.
    using ArgPusher = std::function<int(lua_State*)>;

    // Calls the function stored in registry reference `functionRef` with
    // the pushed arguments, under the CPU and memory limits. Results are
    // discarded. Never throws; errors are returned.
    CallResult call(int functionRef, const ArgPusher& pushArgs);

    // Runs host code that manipulates the VM (creates tables, references...)
    // in protected mode, so allocation failures become errors instead of
    // escaping. `body` returns nothing useful; its stack effects are discarded.
    CallResult runProtected(const ArgPusher& body);

    // Loads a compiled script into its own sandboxed environment and runs
    // its top-level code. On success returns a registry reference to the
    // value the script returned (left for the caller to validate).
    Result<int> loadModule(const CompiledScript& script);

    [[nodiscard]] lua_State* state() const { return m_L; }
    [[nodiscard]] Realm realm() const { return m_realm; }
    [[nodiscard]] const ScriptLimits& limits() const { return m_limits; }
    [[nodiscard]] std::size_t memoryUsed() const { return m_memoryUsed; }
    [[nodiscard]] std::size_t peakMemoryUsed() const { return m_peakMemory; }
    [[nodiscard]] bool inCall() const { return m_inCall; }

    // The object that owns this VM (the ScriptSystem), for API callbacks.
    [[nodiscard]] void* owner() const { return m_owner; }
    [[nodiscard]] static ScriptVm& from(lua_State* L);

    void releaseRef(int ref);

private:
    ScriptVm(Realm realm, const ScriptLimits& limits, void* owner);
    static void* allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize);
    static void interrupt(lua_State* L, int gc);
    static int errorHandler(lua_State* L);
    static int trampoline(lua_State* L);
    // Leaves `nresults` values on the stack on success.
    CallResult protectedCall(int functionRef, const ArgPusher* pushArgs, int nresults);

    lua_State* m_L = nullptr;
    Realm m_realm;
    ScriptLimits m_limits;
    void* m_owner;
    std::size_t m_memoryUsed = 0;
    std::size_t m_peakMemory = 0;
    int m_errorHandlerRef = 0;
    int m_trampolineRef = 0;
    int m_loaderRef = 0;
    int m_noopRef = 0;

    // State of the current protected call.
    bool m_inCall = false;
    std::uint64_t m_steps = 0;
    double m_deadline = 0.0;
    bool m_limitHit = false;
    bool m_memoryFailed = false;
    std::string m_traceback;
};

} // namespace px::scripting
