#pragma once

#include <span>
#include <string>
#include <string_view>

// Machine-readable description of the gameplay scripting API.
//
// The same table that registers the Luau functions produces this list, so
// it cannot drift from the implementation. It drives:
//  * the generated reference docs/scripting-api.md (a test keeps it current),
//  * realm checks (which functions exist in client VMs),
//  * the future visual-scripting node palette (docs/visual-scripting.md).
namespace px::scripting {

enum class ApiKind {
    Function, // Group.name(args)
    Method,   // entity:name(args)
    Property, // entity.name
    Event,    // lifecycle handler a behaviour may define: function Behaviour:Name(args)
    Global,   // global function, e.g. Vec2(x, y)
};

enum class ApiAvailability {
    Everywhere,    // authority and client VMs
    AuthorityOnly, // only where game state is decided (local session, server)
};

struct ApiMember {
    std::string_view group;     // "Game", "World", "Input", "Events", "Entity", "Lifecycle", "Globals"
    std::string_view name;
    ApiKind kind;
    ApiAvailability availability;
    std::string_view signature; // Luau-style signature with types
    std::string_view description;
    bool writable = false;      // properties: can be assigned (authority only)
    bool pure = false;          // no side effects: a visual-scripting "data" node rather than an "exec" node
};

[[nodiscard]] std::span<const ApiMember> apiReference();
[[nodiscard]] std::string generateApiReferenceMarkdown();
[[nodiscard]] const char* apiKindName(ApiKind kind) noexcept;

} // namespace px::scripting
