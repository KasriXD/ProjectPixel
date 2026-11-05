#pragma once

#include "px/online/PlatformTypes.hpp"

#include <nlohmann/json.hpp>

// JSON for the platform's wire types: the single definition both the
// platform service and its clients use.
namespace px::online {

void to_json(nlohmann::json& j, const UserInfo& v);
void from_json(const nlohmann::json& j, UserInfo& v);
void to_json(nlohmann::json& j, const VersionInfo& v);
void from_json(const nlohmann::json& j, VersionInfo& v);
void to_json(nlohmann::json& j, const GameInfo& v);
void from_json(const nlohmann::json& j, GameInfo& v);
void to_json(nlohmann::json& j, const JoinTicket& v);
void from_json(const nlohmann::json& j, JoinTicket& v);
void to_json(nlohmann::json& j, const ReportInfo& v);
void from_json(const nlohmann::json& j, ReportInfo& v);
void to_json(nlohmann::json& j, const AuditEntry& v);
void from_json(const nlohmann::json& j, AuditEntry& v);
void to_json(nlohmann::json& j, const PlatformInfo& v);
void from_json(const nlohmann::json& j, PlatformInfo& v);
void to_json(nlohmann::json& j, const Presence& v);
void from_json(const nlohmann::json& j, Presence& v);
void to_json(nlohmann::json& j, const Profile& v);
void from_json(const nlohmann::json& j, Profile& v);
void to_json(nlohmann::json& j, const PersonInfo& v);
void from_json(const nlohmann::json& j, PersonInfo& v);
void to_json(nlohmann::json& j, const FriendInfo& v);
void from_json(const nlohmann::json& j, FriendInfo& v);
void to_json(nlohmann::json& j, const FriendRequestInfo& v);
void from_json(const nlohmann::json& j, FriendRequestInfo& v);
void to_json(nlohmann::json& j, const Invitation& v);
void from_json(const nlohmann::json& j, Invitation& v);
void to_json(nlohmann::json& j, const SocialSummary& v);
void from_json(const nlohmann::json& j, SocialSummary& v);
void to_json(nlohmann::json& j, const UserReportInfo& v);
void from_json(const nlohmann::json& j, UserReportInfo& v);
void to_json(nlohmann::json& j, const LeaderboardEntry& v);
void from_json(const nlohmann::json& j, LeaderboardEntry& v);
void to_json(nlohmann::json& j, const LeaderboardView& v);
void from_json(const nlohmann::json& j, LeaderboardView& v);

} // namespace px::online
