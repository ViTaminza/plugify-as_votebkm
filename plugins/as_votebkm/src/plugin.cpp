// as_votebkm — player-run votes to ban / kick / mute / gag someone.
//
// Plugify port of the Pisex admin_system module "VoteBKM" ([AS] VoteBKM). The original's
// behaviour is kept, its machinery is not:
//   * type_vote = 1: a player picks the action and the target, every connected human gets a
//     Yes/No menu, and the result is evaluated after result_timer seconds;
//   * type_vote = 0: every player picks the target directly and the punishment is evaluated after
//     every single vote;
//   * both paths share the per-slot votes, the cooldown, the "a vote is running" flag, the
//     immunity/disable flag rules and the punishment priority of the original
//     (ban > kick > mute > gag, ceil(players * percents / 100) votes needed).
//
// This is a standalone native s2sdk plugin: no pisexcompat, no s2guard and no shared wrapper
// library (docs/PLUGIN_STANDARD.md §7). Only the admin_system API is consumed for the punishment,
// the permissions, the immunity values and the VoteBKM_* phrases; the optional hudmenu plugin is
// read for the player's own "!menu" rendering choice (a legitimate optional dependency, not a
// wrapper), with the same "hud if registered, else button" fallback pisexcompat had.
// The safety rules of handoff/weaponskins/PITFALLS.md live in this file:
//
//   * A1 — OnPluginStart never returns Failed: a missing binding or an unreadable config only logs
//     and disables what needs it.
//   * A7/C12 — OnPluginEnd removes every registration in the opposite order: the admin callback,
//     the two player-event hooks, the say/say_team listeners, the console commands (RemoveCommand
//     only for names this plugin created, RemoveCommandListener for a pre-existing name), the map
//     listeners and every menu (DestroyMenu), then it drops the state.
//   * B1/B2 — the map gate g_mapReady. No client query and no menu display happens before the
//     first map or between OnMapEnd and the next OnMapStart. OnMapEnd cancels every client
//     display (CancelClientMenu(Disconnect)) and then destroys the menus - both are menu-manager
//     calls, not client queries - and clears only plugin state. A plugin loaded into a running map
//     latches readiness on its first update.
//   * B4/B5/C9 — there is no s2sdk timer at all. The delayed vote result is an update-scheduler
//     deadline advanced from OnPluginUpdate(dt), so a changelevel, a disconnect or a slot reuse
//     can never fire a stale callback. Only one pass runs per update. The kick path revalidates
//     the target and the author after the synchronous SendAction listeners and only reports
//     success after KickClient actually ran.
//   * B6 — OnClientAuthenticated is not reliable in this stack, so a once-per-second reconcile
//     over the slots keys each slot by its SteamID64. A slot reused by another player is noticed
//     even when the disconnect/connect events are missed.
//   * C2 — every client access goes through IsRealPlayer(): slot range, map gate,
//     IsClientConnected/IsClientInGame, !IsFakeClient, !IsClientSourceTV.
//   * C10/C11 — menus are always destroyed by us (DestroyMenu, outside the handler; the handler
//     only queues work) and commands are registered as Release|ClientCanExecute; the console
//     context of a configured chat spelling is deliberately not answered. CancelClientMenu runs
//     before DestroyMenu on a map end / a map-down unload, so a backend close never touches a
//     client in the B2 window. Menu id 0 is burned once (pisexcompat did the same).
//   * admin_system 1.1.0 readiness — every permission/immunity answer that can start a vote or
//     punish somebody is gated on IsClientAdminDataReady(slot, canonicalSteamID64) for that exact
//     identity. Pending/failed/unknown answers deny fail-soft; a stale answer of a previous slot
//     occupant can never authorize a punishment.
//   * "GetEventPlayerSlot on player events" — player_disconnect and player_connect_full are Pre
//     hooks and their handlers resolve the slot only through s2sdk::GetEventPlayerSlot, never from
//     the raw "userid" value.
//
// The delayed-result identity rule (the reason this port exists):
//   * a vote records the target's SteamID64, the controller handle and the slot it was cast
//     against, plus the voter's SteamID64 and controller handle; a vote counts only while that
//     same identity still occupies its slot;
//   * deferred menu work carries the author's SteamID64/controller handle/generation and is
//     rejected when the slot changed hands;
//   * the result is applied only when the captured target SteamID64 + controller handle still
//     occupy the captured target slot and IsClientAdminDataReady() is authoritative for it; the
//     captured author identity/rights are revalidated with the same rules immediately before the
//     punishment. If the target or the author left, the slot was reused, the map changed, the
//     target gained immunity, the admin data is pending/failed, or the admin core/permissions are
//     unavailable, nothing is punished (fail-open), the vote is reported as cancelled and no
//     kick/ban is ever sent.
//
// The config keeps the original path and format (addons/configs/admin_system/votebkm.ini). The
// plugin-local KV1 reader and the pisexcompat path order live in src/kv1.hpp / src/votebkm_core.*:
// the raw cwd file is tried first, then <configs>/admin_system, its translations dir and the data
// dir. The VoteBKM_* phrases are asked from the admin_system API; a local copy of
// admin_system.phrases.txt (raw cwd first) is only a fallback for a key the core cannot translate,
// resolved once per plugin load with <configs>/core.cfg "ServerLang" -> s2sdk ServerLanguage ->
// "en" (the pisexcompat chain).

#include "votebkm_core.hpp"

#include <admin_api.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <hudmenu/hudmenu.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace vb = votebkm;

constexpr int32_t kMaxSlots = 64;
constexpr int64_t kReconcileIntervalMs = 1000;
constexpr int32_t kMenuPageSize = 5; // pisexcompat/cs2-menus showed five items per page
constexpr int32_t kMenuTimeoutSeconds = 60; // pisexcompat's g_menuTimeout default
constexpr char kLogTag[] = "VoteBKM";
constexpr char kPermission[] = "@admin/votebkm";

// The engine commands Pisex/pisexcompat never registered with AddConsoleCommand: CS2 parses them
// in its ClientCommand path, so creating a ConCommand would shadow the game's own "vote" (the
// config ships !vote). They are dispatched through one global listener instead (PITFALLS C11).
constexpr std::string_view kGameCommands[] = {"jointeam", "joingame", "spectate", "teammenu", "kill",
											  "explode",  "buy",      "drop",     "use",      "callvote",
											  "vote",     "say",      "say_team", "autobuy",  "rebuy"};

// pisexcompat's RegisterS2sdkName(): every configured chat spelling was a client-executable
// ConCommand with the release flag. Combined through the underlying type (ConVarFlag is a plain
// enum class without the plg bitmask trait).
constexpr s2sdk::ConVarFlag kCommandFlags = static_cast<s2sdk::ConVarFlag>(
	static_cast<int64_t>(s2sdk::ConVarFlag::Release) | static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));

const char* const kTitlePhrases[vb::kTypes] = {"VoteBKM_Title_Ban", "VoteBKM_Title_Mute", "VoteBKM_Title_Gag",
											   "VoteBKM_Title_Kick"};
const char* const kActionPhrases[vb::kTypes] = {"VoteBKM_Action_Ban", "VoteBKM_Action_Mute", "VoteBKM_Action_Gag",
												"VoteBKM_Action_Kick"};
const char* const kCreatedPhrases[vb::kTypes] = {"VoteBKM_Created_Ban", "VoteBKM_Created_Mute", "VoteBKM_Created_Gag",
												 "VoteBKM_Created_Kick"};
const char* const kCreatedAdminPhrases[vb::kTypes] = {"VoteBKM_Created_Ban_Admin", "VoteBKM_Created_Mute_Admin",
													  "VoteBKM_Created_Gag_Admin", "VoteBKM_Created_Kick_Admin"};

// The admin_system consumer API (the original's g_pAdminApi). Null before the start and after the
// end, so a callback the engine still holds for a moment cannot reach it (PITFALLS A7).
admin_system::AdminApi* g_pAdminApi = nullptr;

vb::Settings g_settings;
vb::PhraseMap g_localPhrases;
std::string g_phrasesLanguage;
bool g_phrasesLanguageDetected = false;

// --- PITFALLS B1/B2: the map gate ---------------------------------------------------------------
bool g_mapReady = false;
bool g_mapEndedSinceStart = false;
bool g_firstUpdateDone = false;
bool g_mapListeners = false;

// The update-scheduler clock and the once-per-second identity reconcile (B6).
double g_clock = 0.0;
int64_t g_sinceReconcileMs = 0;
std::array<uint64_t, kMaxSlots> g_slotIdentity{};

// --- votes --------------------------------------------------------------------------------------
struct SlotVotes {
	bool set[vb::kTypes]{};
	uint64_t targetSteam[vb::kTypes]{};
	uint64_t voterSteam[vb::kTypes]{};
	int32_t targetSlot[vb::kTypes]{};
};

std::array<SlotVotes, kMaxSlots> g_votes{};

struct ActiveVote {
	bool active = false;
	uint32_t generation = 0; // identifies the Yes/No menus of this vote
	int32_t type = 0;
	int32_t targetSlot = -1;
	uint64_t targetSteam = 0;
	int32_t targetController = 0; // CEntityHandle captured with the SteamID64, when available
	int32_t authorSlot = -1;       // the player who started the vote (the deferred action's author)
	uint64_t authorSteam = 0;
	int32_t authorController = 0;
	double deadline = 0.0;
};

ActiveVote g_active;
int64_t g_lastVoteEpoch = 0; // the original's g_iLastVote: std::time(nullptr) + cooldown

// --- menus --------------------------------------------------------------------------------------
enum class MenuKind { VoteType, Players, VoteYesNo };

struct MenuRecord {
	s2sdk::MenuId id = 0;
	int32_t slot = -1;
	MenuKind kind = MenuKind::VoteType;
	int32_t voteType = 0;
	uint64_t initiatorSteam = 0;
	int32_t initiatorController = 0; // CEntityHandle captured when the menu was created
	uint64_t targetSteam = 0;
	int32_t targetSlot = -1;
	int32_t targetController = 0;
	uint32_t generation = 0;
	std::vector<std::string> itemInfo; // AddMenuItem info strings, index-aligned
};

std::map<s2sdk::MenuId, MenuRecord> g_menus;

struct DestroyRequest {
	s2sdk::MenuId id = 0;
	int32_t slot = -1;
};

std::vector<DestroyRequest> g_destroyQueue;
std::vector<s2sdk::MenuId> g_parkedMenus; // map-down destruction failures, retried when the map is up
bool g_parkedRetried = false;
// s2sdk's menu manager starts ids at 0, but 0 also means "no menu"; the first menu of the process
// could never be tracked (PITFALLS C10). Like pisexcompat's BurnMenuIdZero(), one dummy menu is
// created and kept when it receives id 0, so every real menu gets a usable handle.
bool g_menuZeroBurned = false;
bool g_reservedMenuZero = false;

enum class PendingKind { VoteTypeChosen, PlayerChosen, VoteYes, VoteNo, Back };

struct PendingAction {
	PendingKind kind = PendingKind::VoteTypeChosen;
	int32_t slot = -1;
	int32_t voteType = 0;
	uint64_t initiatorSteam = 0;
	int32_t initiatorController = 0;
	int32_t targetController = 0;
	uint32_t generation = 0;
	std::string info;
};

std::vector<PendingAction> g_actions;

// --- commands / events --------------------------------------------------------------------------
struct CommandRegistration {
	std::string name;
	bool attempted = false;
	bool owned = false; // true: AddConsoleCommand created it; false: callback attached to a foreign command
};

std::vector<CommandRegistration> g_commands;
std::vector<std::string> g_bareCommands;
std::vector<std::string> g_gameCommandNames;
bool g_gameCommandListener = false;
bool g_sayWanted = false;

struct SayListener {
	const char* name = nullptr;
	bool hooked = false;
};

SayListener g_sayListeners[] = {{"say"}, {"say_team"}};

struct EventRegistration {
	const char* name = nullptr;
	s2sdk::EventCallback callback = nullptr;
	bool hooked = false;
};

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& name, void* event, bool dontBroadcast);
s2sdk::ResultType OnPlayerConnectFullEvent(const plg::string& name, void* event, bool dontBroadcast);

EventRegistration g_events[] = {{"player_disconnect", &OnPlayerDisconnectEvent, false},
								{"player_connect_full", &OnPlayerConnectFullEvent, false}};

bool g_coreLoadedRegistered = false;

// --- forward declarations -----------------------------------------------------------------------
s2sdk::ResultType OnVoteCommand(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& args);
s2sdk::ResultType OnSayChat(int32_t caller, s2sdk::ConCommandContext context,
							const plg::vector<plg::string>& args);
s2sdk::ResultType OnGameCommand(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& args);
void MenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param);
void OnMapStartGame();
void OnMapEndGame();
void ForgetMenusOfKind(MenuKind kind);
bool OwnerStillThere(int32_t slot, uint64_t steamid, int32_t controller, bool requirePermission);

// --- logging ------------------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Error);
}

void LogWarning(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Warning);
}

// --- filesystem / language ----------------------------------------------------------------------

std::string ConfigsRoot() {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	return std::string(plg::GetConfigsDir());
}

std::string DataRoot() {
	if (plg::GetDataDir == nullptr) {
		return {};
	}
	return std::string(plg::GetDataDir());
}

bool ReadFirstExisting(const std::vector<std::string>& candidates, std::string& content, std::string& usedPath) {
	usedPath = candidates.empty() ? std::string(vb::kConfigFile) : candidates.front();
	for (const std::string& candidate : candidates) {
		if (std::optional<std::string> text = vb::ReadTextFile(candidate); text) {
			content = std::move(*text);
			usedPath = candidate;
			return true;
		}
	}
	return false;
}

// <configs>/core.cfg "ServerLang" (the Metamod core setting pisexcompat read first); "" when the
// file is missing, malformed or has no ServerLang.
std::string CoreConfigLanguage() {
	const std::string root = ConfigsRoot();
	if (root.empty()) {
		return {};
	}
	const std::optional<std::string> text = vb::ReadTextFile(vb::JoinPath(root, vb::kCoreConfigFile));
	if (!text) {
		return {};
	}
	const std::optional<vb::KvNode> parsed = vb::ParseKv1(*text);
	if (!parsed) {
		return {};
	}
	return std::string(parsed->GetString("ServerLang", ""));
}

std::string ServerLanguage() {
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		const plg::string language = s2sdk::GetServerLanguage();
		return std::string(language.data(), language.size());
	} catch (...) {
		return {};
	}
}

// core.cfg -> s2sdk -> "en", detected once per plugin load (pisexcompat detected it once per
// process; a core.cfg edit takes effect on the next load, exactly like the old port).
std::string PhrasesLanguage() {
	if (!g_phrasesLanguageDetected) {
		g_phrasesLanguage = vb::PickLanguage(CoreConfigLanguage(), ServerLanguage());
		g_phrasesLanguageDetected = true;
	}
	return g_phrasesLanguage;
}

// The fallback phrase table: admin_system.phrases.txt, raw cwd first (see PhraseCandidates). It is
// only consulted when the admin_system API returns the key itself, i.e. when the core has no
// translated value for it. No error is logged when the file is absent: the core normally owns the
// translations and a missing fallback is not a plugin failure.
void LoadLocalPhrases() {
	g_localPhrases.clear();
	std::string content;
	std::string path;
	if (!ReadFirstExisting(vb::PhraseCandidates(ConfigsRoot()), content, path)) {
		return;
	}
	const std::optional<vb::KvNode> root = vb::ParseKv1(content);
	if (!root) {
		LogError("Failed to parse the fallback phrase file '" + path + "'");
		return;
	}
	g_localPhrases = vb::ResolvePhrases(*root, PhrasesLanguage());
}

const char* Phrase(const char* key) {
	if (key == nullptr || *key == '\0') {
		return "";
	}
	if (g_pAdminApi != nullptr && __admin_system_GetTranslation != nullptr) {
		try {
			const char* translated = g_pAdminApi->GetTranslation(key);
			if (translated != nullptr && *translated != '\0' && std::strcmp(translated, key) != 0) {
				return translated;
			}
		} catch (...) {
			// fall through to the local table / the key
		}
	}
	const auto it = g_localPhrases.find(key);
	if (it != g_localPhrases.end() && !it->second.empty()) {
		return it->second.c_str();
	}
	return key;
}

// --- configuration ------------------------------------------------------------------------------

void LoadConfig() {
	g_settings = vb::Settings();
	std::string content;
	std::string path;
	if (!ReadFirstExisting(vb::ConfigCandidates(ConfigsRoot(), DataRoot()), content, path)) {
		// Like the original: the defaults stay and no command is registered.
		LogError("Failed to load the VoteBKM config '" + path + "'");
		return;
	}
	const std::optional<vb::KvNode> root = vb::ParseKv1(content);
	if (!root) {
		LogError("Failed to parse the VoteBKM config '" + path + "' - the defaults are used and no command is registered");
		return;
	}
	g_settings = vb::ParseSettings(*root);
	if (g_settings.commands.empty()) {
		LogError("No commands configured in " + path);
	}
}

// --- admin_system wrappers (never throw across the plugin boundary) ------------------------------

// admin_api.hpp restores the Pisex shape but only checks that *some* admin_system is loaded
// (GetPluginVersion). A partial/older export set would then call a null function pointer, so every
// entry point this plugin uses is checked here: a missing binding behaves as "no answer" and the
// plugin fails open (no menu, no punishment), never crashes.
bool AdminPunishBindingsReady() {
	return __admin_system_GetPluginVersion != nullptr && __admin_system_IsCoreLoaded != nullptr &&
		   __admin_system_HasPermission != nullptr && __admin_system_GetAdminImmunity != nullptr &&
		   __admin_system_IsPlayerPunished != nullptr && __admin_system_AddPlayerPunishment != nullptr &&
		   __admin_system_SendAction != nullptr;
}

bool AdminCoreReady() {
	if (g_pAdminApi == nullptr || __admin_system_GetPluginVersion == nullptr ||
		__admin_system_IsCoreLoaded == nullptr || __admin_system_HasPermission == nullptr ||
		__admin_system_GetAdminImmunity == nullptr || __admin_system_IsPlayerPunished == nullptr) {
		return false;
	}
	try {
		return g_pAdminApi->IsCoreLoaded();
	} catch (...) {
		return false;
	}
}

// admin_system 1.1.0: true only when the asynchronous admin SELECT answered for exactly this
// SteamID64 in this slot. False means "ask again later" (pending/failed load, a reclaimed slot, an
// older core without the export, or no admin_system at all). Every permission/immunity decision
// that can punish somebody requires this answer for the concrete identity, otherwise the plugin
// denies fail-soft: a stale answer for the previous occupant must never authorize a punishment.
bool AdminDataReady(int32_t slot, uint64_t steamid) {
	if (g_pAdminApi == nullptr || steamid == 0 || __admin_system_IsClientAdminDataReady == nullptr ||
		__admin_system_GetPluginVersion == nullptr) {
		return false;
	}
	try {
		return g_pAdminApi->IsClientAdminDataReady(slot, steamid);
	} catch (...) {
		return false;
	}
}

bool HasPermission(int32_t slot, const char* permission) {
	if (g_pAdminApi == nullptr || __admin_system_HasPermission == nullptr || permission == nullptr || *permission == '\0') {
		return false;
	}
	try {
		return g_pAdminApi->HasPermission(slot, permission);
	} catch (...) {
		return false;
	}
}

int AdminImmunity(int32_t slot) {
	if (g_pAdminApi == nullptr || __admin_system_GetAdminImmunity == nullptr) {
		return 0;
	}
	try {
		return g_pAdminApi->GetAdminImmunity(slot);
	} catch (...) {
		return 0;
	}
}

bool IsPunished(int32_t slot, int32_t type) {
	if (g_pAdminApi == nullptr || __admin_system_IsPlayerPunished == nullptr) {
		return false;
	}
	try {
		return g_pAdminApi->IsPlayerPunished(slot, type);
	} catch (...) {
		return false;
	}
}

int AdminMessageType() {
	if (g_pAdminApi == nullptr || __admin_system_GetMessageType == nullptr) {
		return 0;
	}
	try {
		return g_pAdminApi->GetMessageType();
	} catch (...) {
		return 0;
	}
}

bool ApplyPunishment(int32_t slot, int32_t type) {
	if (g_pAdminApi == nullptr || __admin_system_AddPlayerPunishment == nullptr) {
		return false;
	}
	try {
		g_pAdminApi->AddPlayerPunishment(slot, type, g_settings.timePunish, g_settings.reason.c_str());
		return true;
	} catch (const std::exception& ex) {
		LogError(std::string("AddPlayerPunishment threw: ") + ex.what());
	} catch (...) {
		LogError("AddPlayerPunishment threw an unknown exception");
	}
	return false;
}

bool ApplyKickAction(int32_t slot) {
	if (g_pAdminApi == nullptr || __admin_system_SendAction == nullptr) {
		return false;
	}
	try {
		const std::string target = std::to_string(slot);
		g_pAdminApi->SendAction(-1, "kick", target.c_str());
		return true;
	} catch (const std::exception& ex) {
		LogError(std::string("SendAction(kick) threw: ") + ex.what());
	} catch (...) {
		LogError("SendAction(kick) threw an unknown exception");
	}
	return false;
}

bool KickClientSafe(int32_t slot) {
	if (__s2sdk_KickClient == nullptr) {
		LogError("the s2sdk KickClient binding is unavailable - the engine kick is skipped");
		return false;
	}
	try {
		s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kicked, plg::string());
		return true;
	} catch (const std::exception& ex) {
		LogError(std::string("KickClient threw: ") + ex.what());
	} catch (...) {
		LogError("KickClient threw an unknown exception");
	}
	return false;
}

// --- guarded client access (PITFALLS B2 / C2) ---------------------------------------------------

bool ValidSlot(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

// From here on an s2sdk client call cannot touch a torn-down client list or an out-of-range slot.
bool CanQueryClient(int32_t slot) {
	return g_mapReady && ValidSlot(slot);
}

bool IsConnectedClient(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

bool IsInGameClient(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientInGame == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientInGame(slot);
	} catch (...) {
		return false;
	}
}

bool IsFakeClientSafe(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsFakeClient == nullptr) {
		return true; // fail closed: a slot that cannot be proved human is not a voter/target
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true;
	}
}

bool IsSourceTVSafe(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientSourceTV == nullptr) {
		// PITFALLS C2 requires the SourceTV check before treating a slot as a player; a host that
		// cannot answer is treated as "not provably a player", so fail closed.
		return true;
	}
	try {
		return s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return false;
	}
}

uint64_t ClientSteamID(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

std::string ClientName(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(slot);
		const std::string text(name.data(), name.size());
		return text == "<unknown>" ? std::string() : text; // PITFALLS C2
	} catch (...) {
		return {};
	}
}

bool IsRealPlayer(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return false;
	}
	if (!IsConnectedClient(slot) || !IsInGameClient(slot)) {
		return false;
	}
	if (IsFakeClientSafe(slot) || IsSourceTVSafe(slot)) {
		return false;
	}
	return true;
}

// The controller entity handle (CEntityHandle: index | serial << 15) of a live player. It is an
// extra identity component on top of the SteamID64: a slot reused by another player has a different
// serial even if the account id were the same (and it catches a stale menu/action after a reconnect).
// 0 means "the host cannot answer" and the SteamID64 check is used alone.
int32_t ControllerHandleForSlot(int32_t slot) {
	if (!IsRealPlayer(slot) || __s2sdk_PlayerSlotToEntHandle == nullptr) {
		return 0;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		// PITFALLS C1: only 0 and -1 are invalid; a negative handle may be valid.
		if (handle == 0 || handle == -1) {
			return 0;
		}
		if (__s2sdk_IsValidEntHandle != nullptr && !s2sdk::IsValidEntHandle(handle)) {
			return 0;
		}
		return handle;
	} catch (...) {
		return 0;
	}
}

int32_t FindSlotBySteamID(uint64_t steamid) {
	if (steamid == 0) {
		return -1;
	}
	int32_t found = -1;
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsRealPlayer(slot) || ClientSteamID(slot) != steamid) {
			continue;
		}
		if (found >= 0) {
			return -1; // a duplicated identity is never punished by slot
		}
		found = slot;
	}
	return found;
}

// The original's GetPlayersCount(): every connected human in game. -1 when a player carrying the
// disable flag is online, -2 when that answer cannot be trusted yet because the admin_system SELECT
// for one of them is still pending/failed (a permission answer read before readiness is a stale
// answer of the previous occupant). SourceTV is not a player (PITFALLS C2) and is skipped.
int GetPlayersCount() {
	constexpr int kDisabledByFlag = -1;
	constexpr int kAdminDataPending = -2;
	int count = 0;
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!CanQueryClient(slot) || IsFakeClientSafe(slot) || !IsConnectedClient(slot) || !IsInGameClient(slot) ||
			IsSourceTVSafe(slot)) {
			continue;
		}
		if (!g_settings.disableFlag.empty()) {
			const uint64_t steamid = ClientSteamID(slot);
			if (steamid == 0 || !AdminDataReady(slot, steamid)) {
				return kAdminDataPending;
			}
			if (HasPermission(slot, g_settings.disableFlag.c_str())) {
				return kDisabledByFlag;
			}
		}
		++count;
	}
	return count;
}

// --- chat ---------------------------------------------------------------------------------------

void PrintChatText(int32_t slot, std::string_view text) {
	if (!g_mapReady || !ValidSlot(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(vb::Colorize(text)));
	} catch (...) {
	}
}

void PrintChatAllText(std::string_view text) {
	if (!g_mapReady || __s2sdk_PrintToChatAll == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChatAll(plg::string(vb::Colorize(text)));
	} catch (...) {
	}
}

void PrintCooldown(int32_t slot) {
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	const int remaining = static_cast<int>(g_lastVoteEpoch > now ? g_lastVoteEpoch - now : 0);
	PrintChatText(slot, vb::Format(Phrase("VoteBKM_CoolDown"), remaining));
}

bool OnCooldown(int32_t slot) {
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	if (g_lastVoteEpoch <= now) {
		return false;
	}
	PrintCooldown(slot);
	return true;
}

void PrintAction(int32_t type, int32_t voterSlot, int32_t targetSlot, int count, int needed) {
	const std::string voter = ClientName(voterSlot);
	const std::string target = ClientName(targetSlot);
	PrintChatAllText(vb::Format(Phrase(kActionPhrases[type]), voter.c_str(), target.c_str(), count, needed));
}

// --- vote bookkeeping ---------------------------------------------------------------------------

void ClearVotesForVoterSlot(int32_t slot) {
	if (!ValidSlot(slot)) {
		return;
	}
	SlotVotes& votes = g_votes[static_cast<std::size_t>(slot)];
	for (int32_t type = 0; type < vb::kTypes; ++type) {
		votes.set[type] = false;
		votes.targetSteam[type] = 0;
		votes.voterSteam[type] = 0;
		votes.targetSlot[type] = -1;
	}
}

void ClearVotesTargetingSlot(int32_t slot) {
	if (!ValidSlot(slot)) {
		return;
	}
	for (SlotVotes& votes : g_votes) {
		for (int32_t type = 0; type < vb::kTypes; ++type) {
			if (votes.set[type] && votes.targetSlot[type] == slot) {
				votes.set[type] = false;
			}
		}
	}
}

void ClearVotesTargetingSteam(uint64_t steamid) {
	if (steamid == 0) {
		return;
	}
	for (SlotVotes& votes : g_votes) {
		for (int32_t type = 0; type < vb::kTypes; ++type) {
			if (votes.set[type] && votes.targetSteam[type] == steamid) {
				votes.set[type] = false;
			}
		}
	}
}

void ClearVotesOfType(int32_t type) {
	if (type < 0 || type >= vb::kTypes) {
		return;
	}
	for (SlotVotes& votes : g_votes) {
		votes.set[type] = false;
	}
}

void ClearAllVotes() {
	for (SlotVotes& votes : g_votes) {
		votes = SlotVotes{};
	}
}

bool RecordVote(int32_t voterSlot, int32_t type, int32_t targetSlot, uint64_t targetSteam) {
	if (!ValidSlot(voterSlot) || type < 0 || type >= vb::kTypes || !IsRealPlayer(voterSlot) || targetSteam == 0) {
		return false;
	}
	const uint64_t voterSteam = ClientSteamID(voterSlot);
	if (voterSteam == 0) {
		return false;
	}
	SlotVotes& votes = g_votes[static_cast<std::size_t>(voterSlot)];
	votes.set[type] = true;
	votes.targetSteam[type] = targetSteam;
	votes.voterSteam[type] = voterSteam;
	votes.targetSlot[type] = targetSlot;
	return true;
}

// The number of still-valid votes of one type for the captured player: a vote exists only while
// the same SteamID64 still occupies the voter slot that cast it.
int CountVotes(int32_t type, uint64_t targetSteam) {
	int count = 0;
	if (type < 0 || type >= vb::kTypes || targetSteam == 0) {
		return 0;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		const SlotVotes& votes = g_votes[static_cast<std::size_t>(slot)];
		if (!votes.set[type] || votes.targetSteam[type] != targetSteam) {
			continue;
		}
		if (!IsRealPlayer(slot) || ClientSteamID(slot) != votes.voterSteam[type]) {
			continue;
		}
		++count;
	}
	return count;
}

// The original's target filtering in PlayersMenu(): bots are already excluded, a player who is
// already punished with that type is skipped (except for kick), as is an immune player.
bool IsVoteTargetAllowed(int32_t slot, int32_t type) {
	if (!IsRealPlayer(slot) || type < 0 || type >= vb::kTypes) {
		return false;
	}
	const uint64_t steamid = ClientSteamID(slot);
	if (steamid == 0 || ControllerHandleForSlot(slot) == 0 || !AdminDataReady(slot, steamid)) {
		return false;
	}
	if (type != vb::kKick && IsPunished(slot, type)) {
		return false;
	}
	if (!g_settings.immunityFlag.empty() && HasPermission(slot, g_settings.immunityFlag.c_str())) {
		return false;
	}
	if (AdminImmunity(slot) >= g_settings.immunityLimit) {
		return false;
	}
	return true;
}

// The identity rule of the delayed result: the captured SteamID64 must still occupy the captured
// slot, it must still be a connected human, and the permission/immunity answers the punishment
// needs must be available. Anything else is a fail-open refusal (no kick, no ban).
bool TargetIdentityValid(int32_t targetSlot, uint64_t targetSteam, int32_t targetController) {
	if (!g_mapReady || targetSteam == 0 || !ValidSlot(targetSlot) || !AdminCoreReady() ||
		!AdminPunishBindingsReady()) {
		return false;
	}
	if (!IsRealPlayer(targetSlot) || ClientSteamID(targetSlot) != targetSteam) {
		return false;
	}
	if (targetController == 0 || ControllerHandleForSlot(targetSlot) != targetController) {
		return false; // unknown controller or a different occupant/session
	}
	if (FindSlotBySteamID(targetSteam) != targetSlot) {
		return false;
	}
	if (!AdminDataReady(targetSlot, targetSteam)) {
		return false;
	}
	if (!g_settings.immunityFlag.empty() && HasPermission(targetSlot, g_settings.immunityFlag.c_str())) {
		return false;
	}
	if (AdminImmunity(targetSlot) >= g_settings.immunityLimit) {
		return false;
	}
	return true;
}

// The original's CheckPunishment() priority: ban, kick, mute, gag, each needing `needed` valid
// votes for the *same* captured player. The side effects (admin core call / engine kick) run here
// from OnPluginUpdate, never from an s2sdk timer or menu handler.
bool TryPunishTarget(int32_t targetSlot, uint64_t targetSteam, int32_t targetController, int32_t authorSlot,
					 uint64_t authorSteam, int32_t authorController) {
	if (!TargetIdentityValid(targetSlot, targetSteam, targetController)) {
		return false;
	}
	if (!OwnerStillThere(authorSlot, authorSteam, authorController, /*requirePermission=*/true)) {
		return false; // the author's identity/permission is stale or unavailable: no punishment
	}
	const std::string targetName = ClientName(targetSlot);
	const int players = GetPlayersCount();
	if (players < 0 || players < g_settings.minPlayers) {
		return false;
	}
	const int needed = vb::NeededVotes(players, g_settings.percents);
	if (needed <= 0) {
		return false;
	}
	int32_t type = -1;
	if (CountVotes(vb::kBan, targetSteam) >= needed) {
		type = vb::kBan;
	} else if (CountVotes(vb::kKick, targetSteam) >= needed) {
		type = vb::kKick;
	} else if (CountVotes(vb::kMute, targetSteam) >= needed) {
		type = vb::kMute;
	} else if (CountVotes(vb::kGag, targetSteam) >= needed) {
		type = vb::kGag;
	}
	if (type < 0) {
		return false;
	}
	if (type != vb::kKick && IsPunished(targetSlot, type)) {
		return false; // already punished with that type: do not extend a live punishment
	}
	// One last identity read immediately before the side effect.
	if (!IsRealPlayer(targetSlot) || ClientSteamID(targetSlot) != targetSteam) {
		return false;
	}
	if (targetController == 0 || ControllerHandleForSlot(targetSlot) != targetController) {
		return false;
	}
	if (!AdminDataReady(targetSlot, targetSteam)) {
		return false; // a pending/failed admin SELECT at the last moment: deny fail-soft
	}
	bool punished = false;
	if (type == vb::kKick) {
		// The original did both: SendAction to the admin core and engine->DisconnectClient. The
		// core action runs synchronous listeners which may change the slot/controller, the
		// author's rights, the admin readiness or the core state before returning; the actual
		// engine kick is only sent after a fresh identity + author check, and the success
		// message/cooldown are only applied after that kick really ran.
		if (!ApplyKickAction(targetSlot)) {
			return false;
		}
		if (!TargetIdentityValid(targetSlot, targetSteam, targetController) ||
			!OwnerStillThere(authorSlot, authorSteam, authorController, /*requirePermission=*/true)) {
			LogWarning("SendAction(kick) changed the target/author identity or rights - the engine kick was skipped");
			return false;
		}
		if (!KickClientSafe(targetSlot)) {
			return false; // no success message, no cooldown, no vote cleanup for a kick that did not run
		}
		punished = true;
	} else if (type == vb::kBan) {
		punished = ApplyPunishment(targetSlot, admin_system::RT_BAN);
	} else if (type == vb::kMute) {
		punished = ApplyPunishment(targetSlot, admin_system::RT_MUTE);
	} else {
		punished = ApplyPunishment(targetSlot, admin_system::RT_GAG);
	}
	if (!punished) {
		return false;
	}
	g_active.active = false;
	g_lastVoteEpoch = static_cast<int64_t>(std::time(nullptr)) + std::max(0, g_settings.cooldown);
	// The vote is over: every Yes/No menu of it is stale UI. They are forgotten here (outside the
	// menu handler) and destroyed by the next update pass.
	ForgetMenusOfKind(MenuKind::VoteYesNo);
	ClearVotesTargetingSteam(targetSteam);
	PrintChatAllText(vb::Format(Phrase("VoteBKM_Success"), targetName.c_str()));
	return true;
}

// The immediate path (type_vote = 0 always, type_vote = 1 once the Yes votes reach the threshold).
void TryPunishAfterVotes(int32_t targetSlot, uint64_t targetSteam, int32_t targetController, int32_t authorSlot,
						 uint64_t authorSteam, int32_t authorController) {
	TryPunishTarget(targetSlot, targetSteam, targetController, authorSlot, authorSteam, authorController);
}

// --- menu plumbing ------------------------------------------------------------------------------

bool MenuBindingsReady() {
	// DestroyMenu/GetClientMenu/CancelClientMenu are mandatory before any Create/DisplayMenu: a
	// backend close during a map change must be cancellable and we must be able to tell whether a
	// client still holds a menu (a host without them gets no menus at all).
	return __s2sdk_CreateMenu != nullptr && __s2sdk_DestroyMenu != nullptr && __s2sdk_AddMenuItem != nullptr &&
		   __s2sdk_DisplayMenu != nullptr && __s2sdk_GetClientMenu != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_SetMenuType != nullptr && __s2sdk_GetMenuType != nullptr;
}

// Forgets the menu and queues its destroy for the next OnPluginUpdate: DestroyMenu must not run
// inside the menu handler (PITFALLS C10). The record is dropped immediately, so a handler the
// engine still calls for this id becomes a no-op.
void QueueDestroyMenu(s2sdk::MenuId id) {
	const auto it = g_menus.find(id);
	if (it == g_menus.end()) {
		return;
	}
	const int32_t slot = it->second.slot;
	g_menus.erase(it);
	g_destroyQueue.push_back(DestroyRequest{id, slot});
}

// The slot that currently displays `id`, -1 when nobody does (id 0 is s2sdk's "no menu").
int32_t FindDisplaySlot(s2sdk::MenuId id) {
	if (id == 0 || __s2sdk_GetClientMenu == nullptr) {
		return -1;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (s2sdk::GetClientMenu(slot) == id) {
				return slot;
			}
		} catch (...) {
		}
	}
	return -1;
}

// Replaces the menu type with a builtin whose close() is a no-op ("chat"/"console"), so a
// DestroyMenu that still has to end a display cannot reach the client/entity layer while the map
// is being torn down (PITFALLS B2). SetMenuType must be verified with GetMenuType.
bool MakeMenuCloseNoOp(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) {
		return false;
	}
	for (const char* type : {"chat", "console"}) {
		if (__s2sdk_IsMenuTypeRegistered != nullptr) {
			bool registered = false;
			try {
				registered = s2sdk::IsMenuTypeRegistered(plg::string(type));
			} catch (...) {
				registered = false;
			}
			if (!registered) {
				continue;
			}
		}
		try {
			if (!s2sdk::SetMenuType(id, plg::string(type))) {
				continue;
			}
			const plg::string applied = s2sdk::GetMenuType(id);
			if (std::string(applied.data(), applied.size()) == type) {
				return true;
			}
		} catch (...) {
		}
	}
	return false;
}

// Ends every display of `id` and destroys the menu; the caller must not be inside a menu handler.
// mapReady == false is the changelevel window, where a backend close must not touch a client:
// CancelClientMenu(Disconnect) is tried first, a false answer is re-queried, and a display that is
// somehow still there is switched to the builtin no-op-close type before DestroyMenu. Returns true
// when the id is gone from s2sdk's menu table.
bool DestroyMenuChecked(s2sdk::MenuId id, bool mapReady) {
	if (__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	if (id == 0) {
		// The reserved id: client state 0 is s2sdk's "no menu", so it can never be displayed.
		try {
			s2sdk::DestroyMenu(id);
		} catch (...) {
			return false;
		}
		return true;
	}
	if (!mapReady && __s2sdk_GetClientMenu == nullptr) {
		return false; // cannot prove the display is gone: never destroy blindly in the B2 window
	}
	int32_t slot = FindDisplaySlot(id);
	if (slot >= 0) {
		if (__s2sdk_CancelClientMenu != nullptr) {
			try {
				(void)s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
			} catch (...) {
			}
		}
		// Re-query in every case: a false answer means "already ended" (the display is gone), but
		// a contract violation may also leave it active. Only a display that is still there in the
		// map-down window needs the no-op-close type switch before DestroyMenu.
		slot = FindDisplaySlot(id);
		if (slot >= 0 && !mapReady && !MakeMenuCloseNoOp(id)) {
			return false; // would need a backend close over the torn-down client layer
		}
	}
	try {
		s2sdk::DestroyMenu(id);
	} catch (...) {
		return false;
	}
	if (__s2sdk_IsValidMenu != nullptr) {
		try {
			return !s2sdk::IsValidMenu(id);
		} catch (...) {
		}
	}
	return true;
}

void ProcessDestroyQueue() {
	while (!g_destroyQueue.empty()) {
		const DestroyRequest request = g_destroyQueue.front();
		g_destroyQueue.erase(g_destroyQueue.begin());
		if (g_menus.find(request.id) != g_menus.end()) {
			continue; // the engine reused the id before we got here: never destroy the new menu
		}
		if (!DestroyMenuChecked(request.id, /*mapReady=*/true)) {
			g_parkedMenus.push_back(request.id);
			g_parkedRetried = false; // one retry on a later update, not a log loop
			LogError("could not destroy menu " + std::to_string(request.id) + " with a live map - parked");
		}
	}
}

void ForgetMenusOfKind(MenuKind kind) {
	std::vector<s2sdk::MenuId> ids;
	for (const auto& entry : g_menus) {
		if (entry.second.kind == kind) {
			ids.push_back(entry.first);
		}
	}
	for (const s2sdk::MenuId id : ids) {
		QueueDestroyMenu(id);
	}
}

void ForgetMenusForSlot(int32_t slot) {
	std::vector<s2sdk::MenuId> ids;
	for (const auto& entry : g_menus) {
		if (entry.second.slot == slot) {
			ids.push_back(entry.first);
		}
	}
	for (const s2sdk::MenuId id : ids) {
		QueueDestroyMenu(id);
	}
}

void ForgetAllMenus() {
	while (!g_menus.empty()) {
		QueueDestroyMenu(g_menus.begin()->first);
	}
}

// Collects every menu id this plugin still owns (active sessions, queued destroys and parked
// ones) and clears the bookkeeping. The reserved id 0 is handled by the caller.
std::vector<s2sdk::MenuId> CollectOwnedMenuIds() {
	std::vector<s2sdk::MenuId> ids;
	ids.reserve(g_menus.size() + g_destroyQueue.size() + g_parkedMenus.size());
	for (const auto& entry : g_menus) {
		ids.push_back(entry.first);
	}
	for (const DestroyRequest& request : g_destroyQueue) {
		ids.push_back(request.id);
	}
	for (const s2sdk::MenuId id : g_parkedMenus) {
		ids.push_back(id);
	}
	g_menus.clear();
	g_destroyQueue.clear();
	g_parkedMenus.clear();
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

// OnMapEnd (PITFALLS B2): nothing is destroyed through a backend close over the torn-down client
// layer. A menu that survives even the no-op-close switch is parked and retried when the next map
// is up, never silently dropped.
void TeardownMenusOnMapEnd() {
	for (const s2sdk::MenuId id : CollectOwnedMenuIds()) {
		if (DestroyMenuChecked(id, /*mapReady=*/false)) {
			continue;
		}
		g_parkedMenus.push_back(id);
		LogError("could not close menu " + std::to_string(id) +
				 " in the changelevel window - retried when the next map is up");
	}
}

void RetryParkedMenus() {
	if (g_parkedMenus.empty() || !g_mapReady) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_parkedMenus);
	g_parkedMenus.clear();
	for (const s2sdk::MenuId id : ids) {
		if (DestroyMenuChecked(id, /*mapReady=*/true)) {
			continue;
		}
		g_parkedMenus.push_back(id);
		LogError("could not destroy menu " + std::to_string(id) + " with a live map - keeping it for unload");
	}
}

// OnPluginEnd (PITFALLS A7): release every menu, including the reserved id 0. With the map down the
// no-op-close switch above guarantees no client call; an id that cannot be released even then is
// reported as A7_UNRESOLVED instead of being silently dropped.
void TeardownMenusOnUnload() {
	const bool mapReady = g_mapReady;
	std::vector<s2sdk::MenuId> ids = CollectOwnedMenuIds();
	if (g_reservedMenuZero) {
		ids.push_back(0);
		g_reservedMenuZero = false;
	}
	std::vector<s2sdk::MenuId> unresolved;
	for (const s2sdk::MenuId id : ids) {
		if (DestroyMenuChecked(id, mapReady)) {
			continue;
		}
		unresolved.push_back(id);
	}
	if (!unresolved.empty()) {
		std::string list;
		for (const s2sdk::MenuId id : unresolved) {
			if (!list.empty()) {
				list += ", ";
			}
			list += std::to_string(id);
		}
		LogError("A7_UNRESOLVED: s2sdk refused to release menu id(s) " + list +
				 " - their handler points into this library and cannot be removed by the plugin API. This is an "
				 "s2sdk MenuManager contract violation; do not deploy until it is understood");
	}
}

struct MenuItemText {
	std::string info;
	std::string display;
};

bool IsMenuTypeRegisteredName(std::string_view name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr || name.empty()) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(name));
	} catch (...) {
		return false;
	}
}

// The player's own "!menu" choice from the optional hudmenu plugin; empty when hudmenu is absent.
std::string PlayerMenuTypeChoice(int32_t slot) {
	if (__hudmenu_GetClientMenuType == nullptr) {
		return {};
	}
	try {
		const plg::string answer = hudmenu::GetClientMenuType(slot);
		return std::string(answer.data(), answer.size());
	} catch (...) {
		return {};
	}
}

// pisexcompat's ResolveS2sdkMenuTypeName() with the VoteBKM module's menu type (HUD_LAYOUT): the
// player's own hudmenu choice wins when it is a known name, "hud"/"wasd" only while the backend is
// registered, and the fallback is "hud" when hudmenu registered it, otherwise s2sdk's built-in
// "button" (the closest on-screen type for a player who is dead). This is why the hudmenu
// dependency is optional and declared as such: without it the plugin still works, with the button
// backend.
std::string ResolveMenuTypeName(int32_t slot) {
	const std::string choice = PlayerMenuTypeChoice(slot);
	if (choice == "chat" || choice == "centerhtml" || choice == "button") {
		return choice;
	}
	const bool hudRegistered = IsMenuTypeRegisteredName("hud");
	if (choice == "hud") {
		return hudRegistered ? "hud" : "button";
	}
	if (choice == "wasd") {
		return IsMenuTypeRegisteredName("wasd") ? "wasd" : "button";
	}
	return hudRegistered ? "hud" : "button";
}

void MenuNoopHandler(s2sdk::MenuId /*id*/, s2sdk::MenuAction /*action*/, int32_t /*slot*/, int32_t /*param*/) {}

// Creates and keeps one dummy menu if the engine hands out id 0 for it (the menu manager's "no
// menu" value), exactly like pisexcompat did once per process. A non-zero id means somebody else
// already consumed 0: the dummy is destroyed immediately.
void EnsureMenuIdZeroBurned() {
	if (g_menuZeroBurned || !MenuBindingsReady()) {
		return;
	}
	g_menuZeroBurned = true;
	s2sdk::MenuId id = 0;
	try {
		id = s2sdk::CreateMenu(plg::string("as_votebkm:reserved-id-0"), &MenuNoopHandler, plg::string("chat"));
	} catch (...) {
		return;
	}
	if (id == 0) {
		g_reservedMenuZero = true;
		return;
	}
	try {
		s2sdk::DestroyMenu(id);
	} catch (...) {
	}
}

bool DisplayTrackedMenu(int32_t slot, MenuKind kind, const std::string& title, const std::vector<MenuItemText>& items,
						bool exitButton, bool backButton, int32_t voteType, uint64_t initiatorSteam,
						int32_t initiatorController, uint64_t targetSteam, int32_t targetSlot, int32_t targetController,
						uint32_t generation) {
	if (!MenuBindingsReady() || !CanQueryClient(slot) || !IsRealPlayer(slot) || items.empty() ||
		initiatorController == 0) {
		return false; // no controller handle = no provable menu owner
	}
	EnsureMenuIdZeroBurned();
	s2sdk::MenuId id = 0;
	const std::string menuType = ResolveMenuTypeName(slot);
	try {
		id = s2sdk::CreateMenu(plg::string(title), &MenuHandler, plg::string(menuType));
	} catch (...) {
		return false;
	}
	if (g_menus.find(id) != g_menus.end()) {
		LogError("s2sdk returned an already tracked menu id");
		return false;
	}
	MenuRecord record;
	record.id = id;
	record.slot = slot;
	record.kind = kind;
	record.voteType = voteType;
	record.initiatorSteam = initiatorSteam;
	record.initiatorController = initiatorController;
	record.targetSteam = targetSteam;
	record.targetSlot = targetSlot;
	record.targetController = targetController;
	record.generation = generation;
	record.itemInfo.reserve(items.size());
	g_menus.emplace(id, record);

	bool ok = true;
	for (const MenuItemText& item : items) {
		int32_t index = -1;
		try {
			index = s2sdk::AddMenuItem(id, plg::string(item.info), plg::string(item.display), s2sdk::MenuItemStyle::Default);
		} catch (...) {
			index = -1;
		}
		if (index < 0) {
			ok = false;
			break;
		}
		g_menus[id].itemInfo.push_back(item.info);
	}
	if (ok && __s2sdk_SetMenuExitButton != nullptr) {
		try {
			s2sdk::SetMenuExitButton(id, exitButton);
		} catch (...) {
		}
	}
	if (ok && backButton && __s2sdk_SetMenuExitBackButton != nullptr) {
		try {
			s2sdk::SetMenuExitBackButton(id, true);
		} catch (...) {
		}
	}
	if (ok && __s2sdk_SetMenuPagination != nullptr) {
		try {
			s2sdk::SetMenuPagination(id, kMenuPageSize);
		} catch (...) {
		}
	}
	if (ok) {
		try {
			ok = s2sdk::DisplayMenu(id, slot, kMenuTimeoutSeconds);
		} catch (...) {
			ok = false;
		}
	}
	if (!ok) {
		QueueDestroyMenu(id);
		return false;
	}
	return true;
}

// The vote type menu: the original's item order is Ban / Kick / Mute / Gag, with the RT_* value as
// the info string.
void ShowVoteTypeMenu(int32_t slot) {
	if (!CanQueryClient(slot) || !IsRealPlayer(slot)) {
		return;
	}
	if (OnCooldown(slot)) {
		return;
	}
	const int players = GetPlayersCount();
	if (players == -1) {
		PrintChatText(slot, Phrase("VoteBKM_Admin"));
		return;
	}
	if (players < 0) {
		return; // some admin identity is not ready: ask again later, fail-soft
	}
	if (players < g_settings.minPlayers) {
		PrintChatText(slot, Phrase("VoteBKM_NotEnoughPlayers"));
		return;
	}
	const uint64_t initiator = ClientSteamID(slot);
	const int32_t initiatorController = ControllerHandleForSlot(slot);
	if (initiator == 0 || initiatorController == 0 || !AdminDataReady(slot, initiator)) {
		return; // the caller's identity/admin data is not authoritative: deny fail-soft
	}
	const std::vector<MenuItemText> items{{"0", Phrase("VoteBKM_Ban")},
										  {"3", Phrase("VoteBKM_Kick")},
										  {"1", Phrase("VoteBKM_Mute")},
										  {"2", Phrase("VoteBKM_Gag")}};
	DisplayTrackedMenu(slot, MenuKind::VoteType, Phrase("VoteBKM_Title"), items, /*exit=*/true, /*back=*/false, 0,
					   initiator, initiatorController, 0, -1, 0, 0);
}

// The target menu: the original's immunity and already-punished filters, one item per human. The
// item info carries the SteamID64, not the slot: the menu stays open while a slot can change hands,
// and the vote must stay on the player who was picked.
void ShowPlayersMenu(int32_t slot, int32_t type) {
	if (!CanQueryClient(slot) || !IsRealPlayer(slot) || type < 0 || type >= vb::kTypes) {
		return;
	}
	if (OnCooldown(slot)) {
		return;
	}
	const uint64_t initiator = ClientSteamID(slot);
	const int32_t initiatorController = ControllerHandleForSlot(slot);
	if (initiator == 0 || initiatorController == 0 || !AdminDataReady(slot, initiator)) {
		return;
	}
	std::vector<MenuItemText> items;
	for (int32_t candidate = 0; candidate < kMaxSlots; ++candidate) {
		if (!IsVoteTargetAllowed(candidate, type)) {
			continue;
		}
		const uint64_t steamid = ClientSteamID(candidate);
		if (steamid == 0) {
			continue;
		}
		items.push_back({std::to_string(steamid), ClientName(candidate)});
	}
	if (items.empty()) {
		PrintChatText(slot, Phrase("VoteBKM_NoPlayers"));
		return;
	}
	DisplayTrackedMenu(slot, MenuKind::Players, Phrase("VoteBKM_SelectPlayer"), items, /*exit=*/true, /*back=*/true,
					   type, initiator, initiatorController, 0, -1, 0, 0);
}

// The Yes/No menu shown to every human for type_vote = 1. The title carries the target name; the
// menu id is per client, exactly like pisexcompat created one menu per display.
void ShowYesNoMenu(int32_t slot, const std::string& title, int32_t targetSlot, uint64_t targetSteam,
				   int32_t targetController, uint32_t generation) {
	const uint64_t initiator = ClientSteamID(slot);
	const int32_t initiatorController = ControllerHandleForSlot(slot);
	if (initiatorController == 0) {
		return;
	}
	const std::vector<MenuItemText> items{{"0", Phrase("VoteBKM_Yes")}, {"1", Phrase("VoteBKM_No")}};
	DisplayTrackedMenu(slot, MenuKind::VoteYesNo, title, items, /*exit=*/true, /*back=*/false, g_active.type, initiator,
					   initiatorController, targetSteam, targetSlot, targetController, generation);
}

void CreateVoteMenu(int32_t initiatorSlot, int32_t targetSlot, uint64_t targetSteam, int32_t targetController,
					int32_t type) {
	if (!CanQueryClient(initiatorSlot) || type < 0 || type >= vb::kTypes) {
		return;
	}
	if (OnCooldown(initiatorSlot)) {
		return;
	}
	if (g_active.active) {
		PrintChatText(initiatorSlot, Phrase("VoteBKM_AlreadyActive"));
		return;
	}
	const uint64_t authorSteam = ClientSteamID(initiatorSlot);
	const int32_t authorController = ControllerHandleForSlot(initiatorSlot);
	if (authorSteam == 0 || authorController == 0 || !AdminDataReady(initiatorSlot, authorSteam)) {
		return;
	}
	if (targetController == 0 || !IsVoteTargetAllowed(targetSlot, type) || ClientSteamID(targetSlot) != targetSteam ||
		ControllerHandleForSlot(targetSlot) != targetController) {
		return;
	}
	const std::string targetName = ClientName(targetSlot);
	ClearVotesOfType(type);
	ForgetMenusOfKind(MenuKind::VoteYesNo);
	++g_active.generation;
	if (g_active.generation == 0) {
		g_active.generation = 1; // 0 is only the initial value
	}
	g_active.active = true;
	g_active.type = type;
	g_active.targetSlot = targetSlot;
	g_active.targetSteam = targetSteam;
	g_active.targetController = targetController;
	g_active.authorSlot = initiatorSlot;
	g_active.authorSteam = authorSteam;
	g_active.authorController = authorController;
	g_active.deadline = g_clock + static_cast<double>(std::max(0, g_settings.resultTimer));
	const std::string title = vb::Format(Phrase(kTitlePhrases[type]), targetName.c_str());
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (IsRealPlayer(slot)) {
			ShowYesNoMenu(slot, title, targetSlot, targetSteam, targetController, g_active.generation);
		}
	}
}

// --- delayed result / menu actions --------------------------------------------------------------

void EvaluateResultIfDue() {
	if (!g_active.active || g_clock < g_active.deadline) {
		return;
	}
	const int32_t type = g_active.type;
	const int32_t targetSlot = g_active.targetSlot;
	const uint64_t targetSteam = g_active.targetSteam;
	const int32_t targetController = g_active.targetController;
	const int32_t authorSlot = g_active.authorSlot;
	const uint64_t authorSteam = g_active.authorSteam;
	const int32_t authorController = g_active.authorController;
	g_active.active = false;
	ForgetMenusOfKind(MenuKind::VoteYesNo);
	const int count = CountVotes(type, targetSteam);
	const int players = GetPlayersCount();
	if (players == -2) {
		PrintChatAllText(Phrase("VoteBKM_Canceled")); // permissions were not authoritative
		return;
	}
	const int needed = vb::NeededVotes(players, g_settings.percents);
	if (count >= needed) {
		if (TryPunishTarget(targetSlot, targetSteam, targetController, authorSlot, authorSteam, authorController)) {
			return;
		}
		if (needed > 0) {
			PrintChatAllText(Phrase("VoteBKM_Canceled"));
		}
		return;
	}
	PrintChatAllText(Phrase("VoteBKM_Canceled"));
}

bool OwnerStillThere(int32_t slot, uint64_t steamid, int32_t controller, bool requirePermission) {
	if (!CanQueryClient(slot) || !IsRealPlayer(slot)) {
		return false;
	}
	if (steamid != 0 && ClientSteamID(slot) != steamid) {
		return false;
	}
	if (controller == 0 || ControllerHandleForSlot(slot) != controller) {
		return false; // unknown controller or a previous occupant of this slot
	}
	if (requirePermission) {
		const uint64_t canonical = steamid != 0 ? steamid : ClientSteamID(slot);
		if (!AdminDataReady(slot, canonical) || !HasPermission(slot, kPermission)) {
			return false;
		}
	}
	return true;
}

void PrintCreated(int32_t initiatorSlot, int32_t targetSlot, int32_t type) {
	const std::string initiator = ClientName(initiatorSlot);
	const std::string target = ClientName(targetSlot);
	if (AdminMessageType() == 1) {
		PrintChatAllText(vb::Format(Phrase(kCreatedPhrases[type]), target.c_str()));
	} else {
		PrintChatAllText(vb::Format(Phrase(kCreatedAdminPhrases[type]), initiator.c_str(), target.c_str()));
	}
}

void ApplyPlayerChosen(const PendingAction& action) {
	if (!OwnerStillThere(action.slot, action.initiatorSteam, action.initiatorController, /*requirePermission=*/true)) {
		return;
	}
	if (OnCooldown(action.slot)) {
		return;
	}
	const int32_t type = action.voteType;
	if (type < 0 || type >= vb::kTypes) {
		return;
	}
	const uint64_t targetSteam = std::strtoull(action.info.c_str(), nullptr, 10);
	if (targetSteam == 0) {
		return;
	}
	const int32_t targetSlot = FindSlotBySteamID(targetSteam);
	if (targetSlot < 0 || !IsVoteTargetAllowed(targetSlot, type)) {
		return; // the player left or became immune while the menu was open
	}
	if (!RecordVote(action.slot, type, targetSlot, targetSteam)) {
		return;
	}
	const int32_t targetController = ControllerHandleForSlot(targetSlot);
	if (targetController == 0) {
		return;
	}
	if (g_settings.typeVote == 0) {
		const int players = GetPlayersCount();
		if (players < 0) {
			return; // permissions are not authoritative yet: no count, no punishment
		}
		const int count = CountVotes(type, targetSteam);
		const int needed = vb::NeededVotes(players, g_settings.percents);
		PrintAction(type, action.slot, targetSlot, count, needed);
		TryPunishAfterVotes(targetSlot, targetSteam, targetController, action.slot, action.initiatorSteam,
							action.initiatorController);
		return;
	}
	PrintCreated(action.slot, targetSlot, type);
	CreateVoteMenu(action.slot, targetSlot, targetSteam, targetController, type);
}

void ApplyVoteYes(const PendingAction& action) {
	// The display's owner is checked before any chat/side effect: a deferred Yes from a previous
	// occupant must be a silent no-op in the reused slot, never an "already ended" message to the
	// new player. Only the original owner may receive the expired-vote notice.
	if (!OwnerStillThere(action.slot, action.initiatorSteam, action.initiatorController, /*requirePermission=*/false)) {
		return;
	}
	if (!g_active.active || action.generation != g_active.generation) {
		PrintChatText(action.slot, Phrase("VoteBKM_AlreadyEnded"));
		return;
	}
	const int32_t type = g_active.type;
	const int32_t targetSlot = g_active.targetSlot;
	const uint64_t targetSteam = g_active.targetSteam;
	const int32_t targetController = g_active.targetController;
	const int32_t authorSlot = g_active.authorSlot;
	const uint64_t authorSteam = g_active.authorSteam;
	const int32_t authorController = g_active.authorController;
	if (!RecordVote(action.slot, type, targetSlot, targetSteam)) {
		return;
	}
	const int players = GetPlayersCount();
	if (players < 0) {
		return; // an admin identity is pending: keep the vote open, but never count/print a guess
	}
	const int count = CountVotes(type, targetSteam);
	const int needed = vb::NeededVotes(players, g_settings.percents);
	PrintAction(type, action.slot, targetSlot, count, needed);
	if (count >= needed) {
		TryPunishAfterVotes(targetSlot, targetSteam, targetController, authorSlot, authorSteam, authorController);
	}
}

void ApplyBack(int32_t slot, uint64_t initiatorSteam, int32_t initiatorController) {
	if (!OwnerStillThere(slot, initiatorSteam, initiatorController, /*requirePermission=*/true)) {
		return;
	}
	ShowVoteTypeMenu(slot);
}

void DispatchPendingAction(const PendingAction& action) {
	switch (action.kind) {
	case PendingKind::VoteTypeChosen:
		if (OwnerStillThere(action.slot, action.initiatorSteam, action.initiatorController, /*requirePermission=*/true)) {
			ShowPlayersMenu(action.slot, action.voteType);
		}
		break;
	case PendingKind::PlayerChosen:
		ApplyPlayerChosen(action);
		break;
	case PendingKind::VoteYes:
		ApplyVoteYes(action);
		break;
	case PendingKind::VoteNo:
		// The original did nothing on "No" apart from closing the menu.
		break;
	case PendingKind::Back:
		ApplyBack(action.slot, action.initiatorSteam, action.initiatorController);
		break;
	}
}

void ProcessPendingActions() {
	std::vector<PendingAction> actions = std::move(g_actions);
	g_actions.clear();
	for (const PendingAction& action : actions) {
		try {
			DispatchPendingAction(action);
		} catch (const std::exception& ex) {
			LogError(std::string("menu action failed: ") + ex.what());
		} catch (...) {
			LogError("menu action failed with an unknown exception");
		}
	}
}

// Called by s2sdk for Start/Select/Cancel/End. It must not call into the engine that holds the menu
// manager lock: it copies what it needs and queues the work for OnPluginUpdate.
void MenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	try {
		const auto it = g_menus.find(id);
		if (it == g_menus.end()) {
			return;
		}
		const MenuRecord record = it->second;
		if (slot != record.slot) {
			return;
		}
		if (action == s2sdk::MenuAction::Select) {
			if (param < 0 || static_cast<std::size_t>(param) >= record.itemInfo.size()) {
				return;
			}
			PendingAction pending;
			pending.slot = slot;
			pending.voteType = record.voteType;
			pending.initiatorSteam = record.initiatorSteam;
			pending.initiatorController = record.initiatorController;
			pending.targetController = record.targetController;
			pending.generation = record.generation;
			pending.info = record.itemInfo[static_cast<std::size_t>(param)];
			switch (record.kind) {
			case MenuKind::VoteType:
				pending.kind = PendingKind::VoteTypeChosen;
				// The vote type menu carries all four actions; the selected item's info is the
				// RT_* value ("0" ban, "3" kick, "1" mute, "2" gag).
				pending.voteType = std::atoi(pending.info.c_str());
				break;
			case MenuKind::Players:
				pending.kind = PendingKind::PlayerChosen;
				break;
			case MenuKind::VoteYesNo:
				pending.kind = pending.info == "0" ? PendingKind::VoteYes : PendingKind::VoteNo;
				break;
			}
			g_actions.push_back(std::move(pending));
			QueueDestroyMenu(id);
			return;
		}
		if (action == s2sdk::MenuAction::Cancel) {
			QueueDestroyMenu(id);
			if (static_cast<s2sdk::MenuCancelReason>(param) == s2sdk::MenuCancelReason::ExitBack &&
				record.kind == MenuKind::Players) {
				PendingAction pending;
				pending.kind = PendingKind::Back;
				pending.slot = slot;
				pending.initiatorSteam = record.initiatorSteam;
				pending.initiatorController = record.initiatorController;
				g_actions.push_back(std::move(pending));
			}
			return;
		}
		if (action == s2sdk::MenuAction::End) {
			QueueDestroyMenu(id);
		}
	} catch (...) {
		// The engine must never see an exception from a menu callback.
	}
}

// --- registration -------------------------------------------------------------------------------

void AttachCommand(const std::string& name) {
	if (name.empty()) {
		return;
	}
	// A registration needs its remover pair *before* it is created: an add without the matching
	// remove would leave a callback in the unloaded .so (PITFALLS A7). RemoveCommand handles a
	// command this plugin creates; RemoveCommandListener handles a pre-existing name and the
	// say/global listeners, so both are required.
	if (__s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr ||
		__s2sdk_RemoveCommandListener == nullptr) {
		LogError("the AddConsoleCommand/RemoveCommand/RemoveCommandListener pair is incomplete - '" + name +
				 "' is not registered");
		return;
	}
	CommandRegistration registration;
	registration.name = name;
	try {
		// true: this plugin created the command; false: the name existed and the callback was
		// attached as a listener (PITFALLS C11 teardown distinguishes the two).
		registration.owned =
			s2sdk::AddConsoleCommand(plg::string(name), plg::string(), kCommandFlags, &OnVoteCommand, s2sdk::HookMode::Pre);
		registration.attempted = true;
	} catch (...) {
		LogError("AddConsoleCommand failed for '" + name + "'");
		return;
	}
	if (!registration.owned) {
		LogInfo("'" + name + "' already exists; the command listener is attached");
	}
	g_commands.push_back(std::move(registration));
}

bool IsGameCommandName(std::string_view name) {
	for (const std::string_view candidate : kGameCommands) {
		if (candidate == name) {
			return true;
		}
	}
	return false;
}

void RegisterGameCommandListener() {
	if (g_gameCommandListener) {
		return;
	}
	if (__s2sdk_AddCommandListener == nullptr || __s2sdk_RemoveCommandListener == nullptr) {
		LogError("the AddCommandListener/RemoveCommandListener pair is incomplete - the !vote engine-command spelling is not registered");
		return;
	}
	try {
		g_gameCommandListener = s2sdk::AddCommandListener(plg::string(), &OnGameCommand, s2sdk::HookMode::Pre);
	} catch (...) {
		g_gameCommandListener = false;
	}
	if (!g_gameCommandListener) {
		LogError("could not install the global command listener for the !vote engine-command spelling");
	}
}

void RemoveGameCommandListener() {
	if (!g_gameCommandListener) {
		return;
	}
	g_gameCommandListener = false;
	if (__s2sdk_RemoveCommandListener == nullptr) {
		return;
	}
	try {
		s2sdk::RemoveCommandListener(plg::string(), &OnGameCommand, s2sdk::HookMode::Pre);
	} catch (...) {
	}
}

void RegisterSayListeners() {
	if (g_sayWanted) {
		return;
	}
	if (__s2sdk_AddCommandListener == nullptr || __s2sdk_RemoveCommandListener == nullptr) {
		LogError("the AddCommandListener/RemoveCommandListener pair is incomplete - bare chat commands are not registered");
		return;
	}
	g_sayWanted = true;
	for (SayListener& listener : g_sayListeners) {
		try {
			listener.hooked = s2sdk::AddCommandListener(plg::string(listener.name), &OnSayChat, s2sdk::HookMode::Pre);
		} catch (...) {
			listener.hooked = false;
		}
		if (!listener.hooked) {
			LogError(std::string("could not hook '") + listener.name + "' for bare chat commands");
		}
	}
}

void RemoveSayListeners() {
	g_sayWanted = false;
	for (SayListener& listener : g_sayListeners) {
		if (!listener.hooked) {
			continue;
		}
		listener.hooked = false;
		if (__s2sdk_RemoveCommandListener == nullptr) {
			continue;
		}
		try {
			s2sdk::RemoveCommandListener(plg::string(listener.name), &OnSayChat, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
}

void RegisterCommands() {
	g_commands.clear();
	g_bareCommands.clear();
	g_gameCommandNames.clear();
	if (g_settings.commands.empty()) {
		return;
	}
	for (const vb::CommandSpec& spec : g_settings.commands) {
		if (spec.lookup.empty()) {
			continue;
		}
		if (IsGameCommandName(spec.lookup)) {
			// CS2 parses this name itself; a ConCommand would shadow the game's own command.
			if (std::find(g_gameCommandNames.begin(), g_gameCommandNames.end(), spec.lookup) ==
				g_gameCommandNames.end()) {
				g_gameCommandNames.push_back(spec.lookup);
			}
			continue;
		}
		const bool alreadyRegistered =
			std::any_of(g_commands.begin(), g_commands.end(), [&spec](const CommandRegistration& registration) {
				return registration.name == spec.lookup;
			});
		if (!alreadyRegistered) {
			AttachCommand(spec.lookup);
		}
		if (spec.bare &&
			std::find(g_bareCommands.begin(), g_bareCommands.end(), spec.lookup) == g_bareCommands.end()) {
			g_bareCommands.push_back(spec.lookup);
		}
	}
	if (!g_gameCommandNames.empty()) {
		RegisterGameCommandListener();
	}
	if (!g_bareCommands.empty()) {
		RegisterSayListeners();
	}
}

void DetachCommand(CommandRegistration& registration) {
	if (!registration.attempted) {
		return;
	}
	registration.attempted = false;
	if (registration.owned) {
		// This plugin created the command: RemoveCommand detaches the listener and the command.
		if (__s2sdk_RemoveCommand != nullptr) {
			try {
				s2sdk::RemoveCommand(plg::string(registration.name), &OnVoteCommand);
			} catch (...) {
			}
		}
	} else if (__s2sdk_RemoveCommandListener != nullptr) {
		// The name already existed: only the listener is ours. RemoveCommand would unregister
		// somebody else's command (PITFALLS C11).
		try {
			s2sdk::RemoveCommandListener(plg::string(registration.name), &OnVoteCommand, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
	registration.owned = false;
}

void RemoveCommands() {
	for (CommandRegistration& registration : g_commands) {
		DetachCommand(registration);
	}
	RemoveGameCommandListener();
	g_commands.clear();
	g_bareCommands.clear();
	g_gameCommandNames.clear();
}

void RegisterEvents() {
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr) {
		LogError("the HookEvent/UnhookEvent pair is incomplete - disconnect/reuse events are not hooked");
		return;
	}
	for (EventRegistration& event : g_events) {
		if (event.callback == nullptr) {
			continue;
		}
		try {
			event.hooked =
				s2sdk::HookEvent(plg::string(event.name), event.callback, s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay;
		} catch (...) {
			event.hooked = false;
		}
		if (!event.hooked) {
			LogError(std::string("hooking '") + event.name + "' failed - the reconcile fallback still guards slots");
		}
	}
}

void RemoveEvents() {
	for (EventRegistration& event : g_events) {
		if (!event.hooked) {
			continue;
		}
		event.hooked = false;
		if (__s2sdk_UnhookEvent == nullptr) {
			continue;
		}
		try {
			s2sdk::UnhookEvent(plg::string(event.name), event.callback, s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
}

void RegisterMapListeners() {
	if (g_mapListeners) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr ||
		__s2sdk_OnMapStart_Unregister == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr) {
		LogError("the map listener Register/Unregister pair is incomplete - the vote menu stays disabled");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartGame);
	} catch (...) {
		LogError("could not register the s2sdk OnMapStart listener - client calls stay disabled");
		return;
	}
	try {
		s2sdk::OnMapEnd_Register(&OnMapEndGame);
	} catch (...) {
		LogError("could not register the s2sdk OnMapEnd listener - client calls stay disabled");
		try {
			if (__s2sdk_OnMapStart_Unregister != nullptr) {
				s2sdk::OnMapStart_Unregister(&OnMapStartGame);
			}
		} catch (...) {
		}
		return;
	}
	g_mapListeners = true;
}

void UnregisterMapListeners() {
	if (!g_mapListeners) {
		return;
	}
	g_mapListeners = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartGame);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndGame);
		} catch (...) {
		}
	}
}

// --- map lifecycle / reconcile ------------------------------------------------------------------

void ResetVoteState() {
	ClearAllVotes();
	g_active = ActiveVote{};
	g_actions.clear();
}

void OnMapStartGame() {
	try {
		g_mapReady = true;
		g_mapEndedSinceStart = false;
		g_clock = 0.0;
		g_sinceReconcileMs = 0;
		g_parkedRetried = false;
		ResetVoteState();
		g_slotIdentity.fill(0);
		// Anything that outlived the map ends is destroyed by the first update, never from this
		// callback. g_lastVoteEpoch deliberately survives, like the original's g_iLastVote
		// survived StartupServer.
		ForgetAllMenus();
		LogInfo("map started: the vote state is reset");
	} catch (const std::exception& ex) {
		LogError(std::string("OnMapStart failed: ") + ex.what());
	} catch (...) {
		LogError("OnMapStart failed with an unknown exception");
	}
}

void OnMapEndGame() {
	try {
		// PITFALLS B1/B2: close the gate before anything else; no client query happens here.
		g_mapReady = false;
		g_mapEndedSinceStart = true;
		g_clock = 0.0;
		g_sinceReconcileMs = 0;
		// The map is going away: cancel the clients' displays first (Disconnect), then destroy the
		// menus; both run before the state is cleared and never from a menu handler. The backend
		// close path then has no live client to touch under the B2 window.
		TeardownMenusOnMapEnd();
		ResetVoteState();
		g_slotIdentity.fill(0);
	} catch (const std::exception& ex) {
		LogError(std::string("OnMapEnd failed: ") + ex.what());
	} catch (...) {
		LogError("OnMapEnd failed with an unknown exception");
	}
}

void ReconcileSlots() {
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!CanQueryClient(slot) || !IsConnectedClient(slot)) {
			if (g_slotIdentity[static_cast<std::size_t>(slot)] != 0) {
				ClearVotesForVoterSlot(slot);
				g_slotIdentity[static_cast<std::size_t>(slot)] = 0;
			}
			continue;
		}
		if (IsFakeClientSafe(slot) || !IsInGameClient(slot) || IsSourceTVSafe(slot)) {
			if (g_slotIdentity[static_cast<std::size_t>(slot)] != 0) {
				ClearVotesForVoterSlot(slot);
				g_slotIdentity[static_cast<std::size_t>(slot)] = 0;
			}
			continue;
		}
		const uint64_t steamid = ClientSteamID(slot);
		if (steamid == 0) {
			continue;
		}
		const uint64_t previous = g_slotIdentity[static_cast<std::size_t>(slot)];
		if (previous == steamid) {
			continue;
		}
		if (previous != 0) {
			// The slot changed hands while the disconnect/connect events were missed: the old
			// occupant's votes, menus and every vote that targeted his slot are no longer valid.
			ClearVotesForVoterSlot(slot);
			ClearVotesTargetingSlot(slot);
			ForgetMenusForSlot(slot);
		}
		g_slotIdentity[static_cast<std::size_t>(slot)] = steamid;
	}
}

void HandleSlotLeft(int32_t slot) {
	if (!ValidSlot(slot)) {
		return;
	}
	ClearVotesForVoterSlot(slot);
	ForgetMenusForSlot(slot); // the departing client's display is gone; drop the record
	g_slotIdentity[static_cast<std::size_t>(slot)] = 0;
}

void HandleSlotJoined(int32_t slot) {
	if (!ValidSlot(slot)) {
		return;
	}
	// The original's OnClientAutorized cleared every vote against the reconnecting player.
	ClearVotesForVoterSlot(slot);
	ClearVotesTargetingSlot(slot);
	ForgetMenusForSlot(slot); // a menu displayed to the previous occupant is stale now
	g_slotIdentity[static_cast<std::size_t>(slot)] = 0;
}

// --- state reset --------------------------------------------------------------------------------

void ResetAllState() {
	g_mapReady = false;
	g_mapEndedSinceStart = true;
	g_firstUpdateDone = false;
	g_mapListeners = false;
	g_clock = 0.0;
	g_sinceReconcileMs = 0;
	g_settings = vb::Settings();
	g_localPhrases.clear();
	g_phrasesLanguage.clear();
	g_phrasesLanguageDetected = false;
	ClearAllVotes();
	g_active = ActiveVote{};
	g_lastVoteEpoch = 0;
	g_actions.clear();
	g_menus.clear();
	g_destroyQueue.clear();
	g_parkedMenus.clear();
	g_parkedRetried = false;
	g_menuZeroBurned = false;
	g_reservedMenuZero = false;
	g_slotIdentity.fill(0);
	g_commands.clear();
	g_bareCommands.clear();
	g_gameCommandNames.clear();
	g_gameCommandListener = false;
	g_sayWanted = false;
	for (SayListener& listener : g_sayListeners) {
		listener.hooked = false;
	}
	for (EventRegistration& event : g_events) {
		event.hooked = false;
	}
	g_coreLoadedRegistered = false;
}

void OnCoreLoadedCallback() {
	try {
		if (g_pAdminApi != nullptr) {
			g_pAdminApi->ReloadTranslations();
		}
	} catch (...) {
	}
}

// --- event / command trampolines (plain function pointers, never throw) -------------------------

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// The raw "userid" field is not a slot in CS2; GetEventPlayerSlot resolves it (PITFALLS C13).
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
		if (ValidSlot(slot)) {
			HandleSlotLeft(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_disconnect failed: ") + ex.what());
	} catch (...) {
		LogError("player_disconnect failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerConnectFullEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string("userid"));
		if (ValidSlot(slot)) {
			HandleSlotJoined(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_connect_full failed: ") + ex.what());
	} catch (...) {
		LogError("player_connect_full failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// A configured spelling from votebkm.ini. pisexcompat registered these as chat entries: the console
// spelling of the same name continued to the engine and did not open the vote. The chat line is
// always swallowed for a configured name, whether the caller has the permission or not.
s2sdk::ResultType OnVoteCommand(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& /*args*/) {
	if (context != s2sdk::ConCommandContext::Chat) {
		return s2sdk::ResultType::Continue;
	}
	try {
		// Only a real player slot can start a vote. The server console (-1) and every other
		// negative/out-of-range value are fail-closed: this module has no server-console bypass,
		// and a console caller never inherits a player's permission.
		if (CanQueryClient(caller) && IsRealPlayer(caller)) {
			const uint64_t steamid = ClientSteamID(caller);
			if (steamid != 0 && AdminDataReady(caller, steamid) && HasPermission(caller, kPermission)) {
				ShowVoteTypeMenu(caller);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("vote command failed: ") + ex.what());
	} catch (...) {
		LogError("vote command failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

// The trigger-less spellings pisexcompat dispatched through say/say_team (only used when the
// configured file contains a bare command; the shipped config uses !). s2sdk itself dispatches
// messages that start with a trigger, so those are skipped here (no double dispatch).
s2sdk::ResultType OnSayChat(int32_t caller, s2sdk::ConCommandContext /*context*/,
							const plg::vector<plg::string>& args) {
	try {
		if (!g_mapReady || !ValidSlot(caller) || !IsRealPlayer(caller) || args.size() < 2) {
			return s2sdk::ResultType::Continue;
		}
		std::string message;
		for (std::size_t i = 1; i < args.size(); ++i) {
			if (i != 1) {
				message.push_back(' ');
			}
			message.append(args[i].data(), args[i].size());
		}
		const std::size_t end = message.find_first_of(" \t\r\n");
		const std::string word = message.substr(0, end);
		if (word.empty() || word.front() == '!' || word.front() == '/') {
			return s2sdk::ResultType::Continue;
		}
		const std::string lookup = vb::ToLower(word);
		if (std::find(g_bareCommands.begin(), g_bareCommands.end(), lookup) == g_bareCommands.end()) {
			return s2sdk::ResultType::Continue;
		}
		if (HasPermission(caller, kPermission)) {
			ShowVoteTypeMenu(caller);
		}
		return s2sdk::ResultType::Handled;
	} catch (...) {
		return s2sdk::ResultType::Continue;
	}
}

// One global listener for the engine commands pisexcompat never registered as ConCommands. Only
// a chat invocation of a configured name is ours; the console/ClientCommand path continues to the
// engine, so the server's own "vote"/"jointeam" behaviour is untouched (PITFALLS C11).
s2sdk::ResultType OnGameCommand(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& args) {
	if (context != s2sdk::ConCommandContext::Chat || args.empty()) {
		return s2sdk::ResultType::Continue;
	}
	const std::string name = vb::ToLower(std::string_view(args[0].data(), args[0].size()));
	if (std::find(g_gameCommandNames.begin(), g_gameCommandNames.end(), name) == g_gameCommandNames.end()) {
		return s2sdk::ResultType::Continue;
	}
	try {
		if (CanQueryClient(caller) && IsRealPlayer(caller)) {
			const uint64_t steamid = ClientSteamID(caller);
			if (steamid != 0 && AdminDataReady(caller, steamid) && HasPermission(caller, kPermission)) {
				ShowVoteTypeMenu(caller);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("game-command vote failed: ") + ex.what());
	} catch (...) {
		LogError("game-command vote failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled; // the configured chat line is swallowed like any vote command
}

// --- the plugin ---------------------------------------------------------------------------------

class AsVoteBKMPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;
};

AsVoteBKMPlugin g_asVoteBKMPlugin;

plg::PluginResult AsVoteBKMPlugin::OnPluginStart() {
	try {
		ResetAllState();
		g_mapEndedSinceStart = false; // a plugin loaded into a running map latches on its first update
		g_pAdminApi = &admin_system::AdminApi::Get();
		LoadConfig();
		LoadLocalPhrases();
		EnsureMenuIdZeroBurned();

		RegisterMapListeners();
		RegisterCommands();
		RegisterEvents();

		if (g_pAdminApi != nullptr) {
			if (__admin_system_OnCoreLoaded_Register != nullptr && __admin_system_OnCoreLoaded_Unregister != nullptr) {
				try {
					g_pAdminApi->OnCoreLoaded(&OnCoreLoadedCallback);
					g_coreLoadedRegistered = true;
				} catch (...) {
					g_coreLoadedRegistered = false;
					LogError("could not register the admin_system OnCoreLoaded callback");
				}
			} else {
				LogError("the admin_system OnCoreLoaded binding is unavailable - the phrase cache is not refreshed");
			}
			if (AdminCoreReady()) {
				g_pAdminApi->ReloadTranslations();
			}
		}

		if (!MenuBindingsReady()) {
			LogError("the s2sdk menu bindings are unavailable - vote menus stay closed");
		}
		if (__s2sdk_GetEventPlayerSlot == nullptr) {
			LogError("the s2sdk GetEventPlayerSlot binding is unavailable - only the reconcile guards slots");
		}
		LogInfo("ready (" + std::to_string(g_commands.size()) + " vote command" +
				(g_commands.size() == 1 ? "" : "s") + ", type_vote=" + std::to_string(g_settings.typeVote) +
				", fallback phrases=" + std::to_string(g_localPhrases.size()) + ")");
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed (PITFALLS A1)
}

plg::PluginResult AsVoteBKMPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		if (!g_firstUpdateDone) {
			g_firstUpdateDone = true;
			if (g_mapListeners && !g_mapEndedSinceStart) {
				g_mapReady = true;
			}
		}
		if (!g_mapReady) {
			return {};
		}
		const int64_t elapsed = dt.count() > 0 ? static_cast<int64_t>(dt.count()) : 0;
		g_clock += static_cast<double>(elapsed) / 1000.0;
		g_sinceReconcileMs += std::min<int64_t>(elapsed, 1000);
		ProcessDestroyQueue();
		if (!g_parkedRetried && !g_parkedMenus.empty()) {
			g_parkedRetried = true;
			RetryParkedMenus();
		}
		if (g_sinceReconcileMs >= kReconcileIntervalMs) {
			g_sinceReconcileMs = 0;
			ReconcileSlots();
		}
		ProcessPendingActions();
		EvaluateResultIfDue();
		ProcessDestroyQueue();
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult AsVoteBKMPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7/C12: every registration is removed again, in the opposite order.
		if (g_coreLoadedRegistered && g_pAdminApi != nullptr && __admin_system_OnCoreLoaded_Unregister != nullptr) {
			g_coreLoadedRegistered = false;
			try {
				g_pAdminApi->OnCoreLoadedUnregister();
			} catch (...) {
			}
		}
		RemoveEvents();
		RemoveSayListeners();
		RemoveCommands();
		UnregisterMapListeners();
		TeardownMenusOnUnload();
		ResetAllState();
		g_pAdminApi = nullptr;
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asVoteBKMPlugin)
