// as_votebkm — Plugify port of the Pisex admin_system module "VoteBKM" ([AS] VoteBKM).
//
// Player-run votes to ban / kick / mute / gag someone:
//   * type_vote = 1 (the live config): a player picks the vote type from a menu, picks the
//     target, a menu with Yes/No is shown to every connected human player, and after
//     result_timer seconds the result is evaluated.
//   * type_vote = 0: every player picks the target directly and the punishment is evaluated
//     after every single vote.
// Both paths share g_iTarget[slot][type] (the target each player voted against), the cooldown,
// the "a vote is running" flag, the immunity/disable flag checks and the punishment itself,
// which goes through the admin core (AddPlayerPunishment / SendAction "kick").
//
// Porting notes (behaviour kept identical, the mechanism changed):
//   * engine->DisconnectClient(slot, NETWORK_DISCONNECT_KICKED) is s2sdk::KickClient(slot,
//     NetworkDisconnectionReason::Kicked, ""); the SendAction(-1, "kick", slot) of the original
//     is still sent, so plugins listening to the core's OnAction see the same event.
//   * SourceHook/SchemaEntity were not used by this module; everything else is pisexcompat.
//   * The original registered its commands while parsing votebkm.ini; the port does the same,
//     so a missing config file leaves the plugin without commands, exactly like the original.
//   * s2sdk's OnClientAuthenticated is not delivered reliably on a live server (see
//     plugins/perm_connector), so the same "player authorised" handler also runs from a
//     once-per-second reconcile keyed by the slot's steamid.

#include <admin_api.hpp>

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The object the ported module uses (the Pisex variable, now the consumer wrapper). All calls
// return the neutral value when the admin_system plugin is not loaded.
admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

constexpr char kLogTag[] = "VoteBKM";
constexpr int kMaxSlots = 64;
constexpr int kTypes = 4; // ban, mute, gag, kick — the order g_iTarget uses
constexpr char kConfigPath[] = "addons/configs/admin_system/votebkm.ini";

// g_iTarget[i][j] uses the RT_* indexes, the title/action phrase tables the menu order
// (ban, mute, gag, kick) — kept exactly as in the original.
const char* const kTitlePhrases[kTypes] = {"VoteBKM_Title_Ban", "VoteBKM_Title_Mute", "VoteBKM_Title_Gag",
										   "VoteBKM_Title_Kick"};
const char* const kActionPhrases[kTypes] = {"VoteBKM_Action_Ban", "VoteBKM_Action_Mute", "VoteBKM_Action_Gag",
											"VoteBKM_Action_Kick"};
const char* const kCreatedPhrases[kTypes] = {"VoteBKM_Created_Ban", "VoteBKM_Created_Mute", "VoteBKM_Created_Gag",
											 "VoteBKM_Created_Kick"};
const char* const kCreatedAdminPhrases[kTypes] = {"VoteBKM_Created_Ban_Admin", "VoteBKM_Created_Mute_Admin",
												  "VoteBKM_Created_Gag_Admin", "VoteBKM_Created_Kick_Admin"};

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

// The original's `split(s, "|")`.
std::vector<std::string> SplitCommands(const std::string& text) {
	std::vector<std::string> parts;
	std::size_t start = 0;
	while (true) {
		const std::size_t end = text.find('|', start);
		const std::string token = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (!token.empty()) {
			parts.push_back(token);
		}
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return parts;
}

class AsVoteBKMPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// --- configuration (votebkm.ini, same keys and defaults as the original) ------------------
	std::string m_immunityFlag = "@admin/votebkm_i";
	std::string m_disableFlag = "@admin/votebkm_d";
	int m_immunityLimit = 10;
	float m_percents = 0.4f;
	int m_minPlayers = 4;
	int m_time = 3600;
	int m_cooldown = 300;
	int m_typeVote = 0;
	int m_resultTimer = 30;
	std::string m_reason = "VoteBKM";
	std::size_t m_commandCount = 0;

	// --- vote state ---------------------------------------------------------------------------
	static std::array<std::array<int, kTypes>, kMaxSlots> MakeEmptyTargets();
	std::array<std::array<int, kTypes>, kMaxSlots> m_target = MakeEmptyTargets();
	int m_lastVote = 0;
	bool m_activeVote = false;

	// --- the ported Pisex functions -----------------------------------------------------------
	void LoadConfig();
	void VoteMenu(int iSlot);
	void PlayersMenu(int iSlot, int iType);
	void CreateVoteMenu(int iSlot, int iTarget, int iType);
	void CheckPunishment(int iTarget);
	// Number of human players in game; -1 when a player with the disable flag is online
	// (the original g_pUtils count helper).
	int GetPlayersCount();
	// The original OnClientAutorized: forget every vote against the reconnecting player.
	void HandleClientAuthorized(int iSlot, uint64_t iSteamID64);

	// Once-per-second replacement for the (unreliable) s2sdk OnClientAuthenticated event.
	void ReconcileAuthorized();
	static constexpr auto kReconcileInterval = std::chrono::seconds(1);
	std::array<uint64_t, kMaxSlots> m_seenSteamIds{};
	bool m_seenSeeded = false;
	std::chrono::milliseconds m_sinceReconcile{0};
};

AsVoteBKMPlugin g_asVoteBKMPlugin;

// --- the ported module ------------------------------------------------------------------------

std::array<std::array<int, kTypes>, kMaxSlots> AsVoteBKMPlugin::MakeEmptyTargets() {
	std::array<std::array<int, kTypes>, kMaxSlots> targets{};
	for (auto& perType : targets) {
		perType.fill(-1);
	}
	return targets;
}

void AsVoteBKMPlugin::LoadConfig() {
	pisex::KeyValues::AutoDelete kv("VoteBKM");
	if (!kv->LoadFromFile(kConfigPath)) {
		pisex::g_pUtils->ErrorLog("[%s] Failed to load %s", kLogTag, kConfigPath);
		return; // like the original: defaults, and the commands stay unregistered
	}

	m_immunityFlag = kv->GetString("immunity_flag", "@admin/votebkm_i");
	m_disableFlag = kv->GetString("disable_flag", "@admin/votebkm_d");
	m_immunityLimit = kv->GetInt("immunity_limit", 10);
	m_percents = static_cast<float>(kv->GetInt("percents", 40)) / 100.0f;
	m_minPlayers = kv->GetInt("min_players", 4);
	m_time = kv->GetInt("time_punish", 3600);
	m_cooldown = kv->GetInt("cooldown", 300);
	m_typeVote = kv->GetInt("type_vote", 0);
	m_resultTimer = kv->GetInt("result_timer", 30);
	m_reason = kv->GetString("reason", "VoteBKM");

	const std::vector<std::string> commands = SplitCommands(kv->GetString("commands", ""));
	if (commands.empty()) {
		pisex::g_pUtils->ErrorLog("[%s] No commands configured in %s", kLogTag, kConfigPath);
		return;
	}
	pisex::g_pUtils->RegCommand({}, commands, [](int iSlot, const char* szContent) {
		(void)szContent; // the original ignores the argument string as well
		if (g_pAdminApi->HasPermission(iSlot, "@admin/votebkm")) {
			g_asVoteBKMPlugin.VoteMenu(iSlot);
		}
		return true;
	});
	m_commandCount = commands.size();
}

int AsVoteBKMPlugin::GetPlayersCount() {
	int iCount = 0;
	for (int i = 0; i < kMaxSlots; ++i) {
		if (pisex::g_pPlayers->IsFakeClient(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsConnected(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsInGame(i)) {
			continue;
		}
		if (!m_disableFlag.empty() && g_pAdminApi->HasPermission(i, m_disableFlag.c_str())) {
			return -1;
		}
		++iCount;
	}
	return iCount;
}

void AsVoteBKMPlugin::CheckPunishment(int iTarget) {
	const int iCount = GetPlayersCount();
	if (iCount < m_minPlayers) {
		return;
	}
	const int iNeeded = static_cast<int>(std::ceil(static_cast<float>(iCount) * m_percents));
	if (iNeeded <= 0) {
		return;
	}
	int iBans = 0;
	int iKicks = 0;
	int iMutes = 0;
	int iGags = 0;
	for (int i = 0; i < kMaxSlots; ++i) {
		if (pisex::g_pPlayers->IsFakeClient(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsConnected(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsInGame(i)) {
			continue;
		}
		if (m_target[i][0] == iTarget) {
			++iBans;
		}
		if (m_target[i][3] == iTarget) {
			++iKicks;
		}
		if (m_target[i][1] == iTarget) {
			++iMutes;
		}
		if (m_target[i][2] == iTarget) {
			++iGags;
		}
	}
	bool bPunish = false;
	if (iBans >= iNeeded) {
		g_pAdminApi->AddPlayerPunishment(iTarget, admin_system::RT_BAN, m_time, m_reason.c_str());
		bPunish = true;
	} else if (iKicks >= iNeeded) {
		g_pAdminApi->SendAction(-1, "kick", std::to_string(iTarget).c_str());
		bPunish = true;
	} else if (iMutes >= iNeeded) {
		g_pAdminApi->AddPlayerPunishment(iTarget, admin_system::RT_MUTE, m_time, m_reason.c_str());
		bPunish = true;
	} else if (iGags >= iNeeded) {
		g_pAdminApi->AddPlayerPunishment(iTarget, admin_system::RT_GAG, m_time, m_reason.c_str());
		bPunish = true;
	}
	if (bPunish) {
		m_activeVote = false;
		m_lastVote = static_cast<int>(std::time(nullptr)) + m_cooldown;
		for (int i = 0; i < kMaxSlots; ++i) {
			for (int j = 0; j < kTypes; ++j) {
				if (m_target[i][j] == iTarget) {
					m_target[i][j] = -1;
				}
			}
		}
		pisex::g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation("VoteBKM_Success"),
										pisex::g_pPlayers->GetPlayerName(iTarget));
		if (iKicks >= iNeeded && __s2sdk_KickClient && pisex::g_pPlayers->IsConnected(iTarget)) {
			// The original: engine->DisconnectClient(CPlayerSlot(iTarget), NETWORK_DISCONNECT_KICKED).
			// The slot is validated here, not just when the vote started: the target may have left
			// during the vote (result_timer seconds), and KickClient (engine->DisconnectClient)
			// does not check the slot itself.
			s2sdk::KickClient(iTarget, s2sdk::NetworkDisconnectionReason::Kicked, plg::string());
		}
	}
}

void AsVoteBKMPlugin::CreateVoteMenu(int iSlot, int iTarget, int iType) {
	const int iNow = static_cast<int>(std::time(nullptr));
	if (m_lastVote > iNow) {
		pisex::g_pUtils->PrintToChat(iSlot, g_pAdminApi->GetTranslation("VoteBKM_CoolDown"), m_lastVote - iNow);
		return;
	}
	if (m_activeVote) {
		// The phrase is translated data, not a format string: it may contain a '%'.
		pisex::g_pUtils->PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("VoteBKM_AlreadyActive"));
		return;
	}

	for (int i = 0; i < kMaxSlots; ++i) {
		m_target[i][iType] = -1;
	}

	m_activeVote = true;
	pisex::g_pUtils->CreateTimer(static_cast<float>(m_resultTimer), [this, iTarget, iType]() -> float {
		if (!m_activeVote) {
			return -1.0f;
		}
		m_activeVote = false;
		int iCount = 0;
		const int iNeeded = static_cast<int>(std::ceil(static_cast<float>(GetPlayersCount()) * m_percents));
		for (int i = 0; i < kMaxSlots; ++i) {
			if (pisex::g_pPlayers->IsFakeClient(i)) {
				continue;
			}
			if (!pisex::g_pPlayers->IsConnected(i)) {
				continue;
			}
			if (!pisex::g_pPlayers->IsInGame(i)) {
				continue;
			}
			if (m_target[i][iType] == iTarget) {
				++iCount;
			}
		}
		if (iCount >= iNeeded) {
			CheckPunishment(iTarget);
		} else {
			pisex::g_pUtils->PrintToChatAll("%s", g_pAdminApi->GetTranslation("VoteBKM_Canceled"));
		}
		return -1.0f;
	});

	for (int i = 0; i < kMaxSlots; ++i) {
		if (pisex::g_pPlayers->IsFakeClient(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsConnected(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsInGame(i)) {
			continue;
		}

		pisex::Menu hMenu;
		const plg::string title = g_pAdminApi->GetTranslation(kTitlePhrases[iType]);
		const std::string szBuffer =
			pisex::FormatString(title.c_str(), pisex::g_pPlayers->GetPlayerName(iTarget));
		pisex::g_pMenus->SetTitleMenu(hMenu, szBuffer.c_str());
		pisex::g_pMenus->AddItemMenu(hMenu, "0", g_pAdminApi->GetTranslation("VoteBKM_Yes"));
		pisex::g_pMenus->AddItemMenu(hMenu, "1", g_pAdminApi->GetTranslation("VoteBKM_No"));
		pisex::g_pMenus->SetBackMenu(hMenu, false);
		pisex::g_pMenus->SetExitMenu(hMenu, true);
		pisex::g_pMenus->SetCallback(hMenu, [this, iTarget, iType](const char* szBack, const char* szFront, int iItem,
																  int iSlot) {
			(void)szFront;
			if (iItem < 7) {
				if (!m_activeVote) {
					pisex::g_pUtils->PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("VoteBKM_AlreadyEnded"));
					return;
				}
				const int iVote = std::atoi(szBack);
				if (iVote == 0) {
					m_target[iSlot][iType] = iTarget;
					int iCount = 0;
					const int iNeeded =
						static_cast<int>(std::ceil(static_cast<float>(GetPlayersCount()) * m_percents));
					for (int i = 0; i < kMaxSlots; ++i) {
						if (pisex::g_pPlayers->IsFakeClient(i)) {
							continue;
						}
						if (!pisex::g_pPlayers->IsConnected(i)) {
							continue;
						}
						if (!pisex::g_pPlayers->IsInGame(i)) {
							continue;
						}
						if (m_target[i][iType] == iTarget) {
							++iCount;
						}
					}
					pisex::g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation(kActionPhrases[iType]),
													pisex::g_pPlayers->GetPlayerName(iSlot),
													pisex::g_pPlayers->GetPlayerName(iTarget), iCount, iNeeded);
					if (iCount >= iNeeded) {
						CheckPunishment(iTarget);
					}
				}
				pisex::g_pMenus->ClosePlayerMenu(iSlot);
			}
		});
		pisex::g_pMenus->DisplayPlayerMenu(hMenu, i);
	}
}

void AsVoteBKMPlugin::PlayersMenu(int iSlot, int iType) {
	const int iNow = static_cast<int>(std::time(nullptr));
	if (m_lastVote > iNow) {
		pisex::g_pUtils->PrintToChat(iSlot, g_pAdminApi->GetTranslation("VoteBKM_CoolDown"), m_lastVote - iNow);
		return;
	}
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdminApi->GetTranslation("VoteBKM_SelectPlayer"));
	bool bFound = false;
	for (int i = 0; i < kMaxSlots; ++i) {
		if (pisex::g_pPlayers->IsFakeClient(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsConnected(i)) {
			continue;
		}
		if (!pisex::g_pPlayers->IsInGame(i)) {
			continue;
		}
		if (iType != 3 && g_pAdminApi->IsPlayerPunished(i, iType)) {
			continue;
		}
		if (g_pAdminApi->HasPermission(i, m_immunityFlag.c_str())) {
			continue;
		}
		if (g_pAdminApi->GetAdminImmunity(i) >= m_immunityLimit) {
			continue;
		}
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), pisex::g_pPlayers->GetPlayerName(i));
		bFound = true;
	}
	if (!bFound) {
		pisex::g_pUtils->PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("VoteBKM_NoPlayers"));
		return;
	}
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [this, iType](const char* szBack, const char* szFront, int iItem, int iSlot) {
		(void)szFront;
		if (iItem < 7) {
			const int iNow = static_cast<int>(std::time(nullptr));
			if (m_lastVote > iNow) {
				pisex::g_pUtils->PrintToChat(iSlot, g_pAdminApi->GetTranslation("VoteBKM_CoolDown"), m_lastVote - iNow);
				return;
			}
			const int iTarget = std::atoi(szBack);
			m_target[iSlot][iType] = iTarget;
			pisex::g_pMenus->ClosePlayerMenu(iSlot);
			if (!m_typeVote) {
				int iCount = 0;
				const int iNeeded = static_cast<int>(std::ceil(static_cast<float>(GetPlayersCount()) * m_percents));
				for (int i = 0; i < kMaxSlots; ++i) {
					if (pisex::g_pPlayers->IsFakeClient(i)) {
						continue;
					}
					if (!pisex::g_pPlayers->IsConnected(i)) {
						continue;
					}
					if (!pisex::g_pPlayers->IsInGame(i)) {
						continue;
					}
					if (m_target[i][iType] == iTarget) {
						++iCount;
					}
				}
				pisex::g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation(kActionPhrases[iType]),
												pisex::g_pPlayers->GetPlayerName(iSlot),
												pisex::g_pPlayers->GetPlayerName(iTarget), iCount, iNeeded);
				CheckPunishment(iTarget);
			} else {
				if (g_pAdminApi->GetMessageType() == 1) {
					pisex::g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation(kCreatedPhrases[iType]),
													pisex::g_pPlayers->GetPlayerName(iTarget));
				} else {
					pisex::g_pUtils->PrintToChatAll(g_pAdminApi->GetTranslation(kCreatedAdminPhrases[iType]),
													pisex::g_pPlayers->GetPlayerName(iSlot),
													pisex::g_pPlayers->GetPlayerName(iTarget));
				}
				CreateVoteMenu(iSlot, iTarget, iType);
			}
		} else if (iItem == 7) {
			VoteMenu(iSlot);
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void AsVoteBKMPlugin::VoteMenu(int iSlot) {
	const int iNow = static_cast<int>(std::time(nullptr));
	if (m_lastVote > iNow) {
		pisex::g_pUtils->PrintToChat(iSlot, g_pAdminApi->GetTranslation("VoteBKM_CoolDown"), m_lastVote - iNow);
		return;
	}
	const int iCount = GetPlayersCount();
	if (iCount == -1) {
		pisex::g_pUtils->PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("VoteBKM_Admin"));
		return;
	}
	if (iCount < m_minPlayers) {
		pisex::g_pUtils->PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("VoteBKM_NotEnoughPlayers"));
		return;
	}
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, g_pAdminApi->GetTranslation("VoteBKM_Title"));
	pisex::g_pMenus->AddItemMenu(hMenu, "0", g_pAdminApi->GetTranslation("VoteBKM_Ban"));
	pisex::g_pMenus->AddItemMenu(hMenu, "3", g_pAdminApi->GetTranslation("VoteBKM_Kick"));
	pisex::g_pMenus->AddItemMenu(hMenu, "1", g_pAdminApi->GetTranslation("VoteBKM_Mute"));
	pisex::g_pMenus->AddItemMenu(hMenu, "2", g_pAdminApi->GetTranslation("VoteBKM_Gag"));
	pisex::g_pMenus->SetBackMenu(hMenu, false);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [this](const char* szBack, const char* szFront, int iItem, int iSlot) {
		(void)szFront;
		if (iItem < 7) {
			const int iType = std::atoi(szBack);
			PlayersMenu(iSlot, iType);
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot);
}

void AsVoteBKMPlugin::HandleClientAuthorized(int iSlot, uint64_t iSteamID64) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	// The s2sdk event may report a 32-bit account id or 0 (see plugins/perm_connector); the slot's
	// own SteamID64 is what identifies the connection for the dedupe below.
	const uint64_t canonical = pisex::g_pPlayers->GetSteamID64(iSlot);
	if (canonical != 0) {
		iSteamID64 = canonical;
	} else if (iSteamID64 < (1ull << 32)) {
		return; // not a SteamID64 yet - the reconcile pass picks the player up
	}
	if (iSteamID64 == 0 || m_seenSteamIds[static_cast<std::size_t>(iSlot)] == iSteamID64) {
		return; // already handled by the event or a previous reconcile pass
	}
	m_seenSteamIds[static_cast<std::size_t>(iSlot)] = iSteamID64;

	// The original OnClientAutorized: every vote against the (re)connecting player is dropped.
	for (int i = 0; i < kMaxSlots; ++i) {
		for (int j = 0; j < kTypes; ++j) {
			if (m_target[i][j] == iSlot) {
				m_target[i][j] = -1;
			}
		}
	}
}

void AsVoteBKMPlugin::ReconcileAuthorized() {
	// On the first pass the players already on the server are only recorded: the original only
	// reacted to authorization events, it never re-processed players that were already in game.
	for (int iSlot = 0; iSlot < kMaxSlots; ++iSlot) {
		if (!pisex::g_pPlayers->IsConnected(iSlot)) {
			m_seenSteamIds[static_cast<std::size_t>(iSlot)] = 0;
			continue;
		}
		if (pisex::g_pPlayers->IsFakeClient(iSlot) || !pisex::g_pPlayers->IsAuthenticated(iSlot)) {
			continue;
		}
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(iSlot);
		if (steamid == 0 || steamid == m_seenSteamIds[static_cast<std::size_t>(iSlot)]) {
			continue;
		}
		if (!m_seenSeeded) {
			m_seenSteamIds[static_cast<std::size_t>(iSlot)] = steamid;
			continue;
		}
		HandleClientAuthorized(iSlot, steamid);
	}
	m_seenSeeded = true;
}

// --- lifecycle ---------------------------------------------------------------------------------

plg::PluginResult AsVoteBKMPlugin::OnPluginStart() {
	try {
		// "admin_system" makes pisexcompat resolve "addons/configs/admin_system/..." through the
		// shared <configs>/admin_system directory every admin plugin uses (see the port's layout).
		pisex::Init("admin_system");

		if (!g_pAdminApi->IsCoreLoaded()) {
			LogInfo(std::string("as_votebkm: waiting for the admin_system core to load"));
		}

		pisex::g_pPlayers->HookOnClientAuthorized([](int iSlot, uint64_t iSteamID64) {
			g_asVoteBKMPlugin.HandleClientAuthorized(iSlot, iSteamID64);
		});
		LoadConfig();

		m_sinceReconcile = kReconcileInterval; // run the first reconcile on the first update
		LogInfo("as_votebkm: ready (" + std::to_string(m_commandCount) + " vote command" +
				(m_commandCount == 1 ? "" : "s") + ", type_vote=" + std::to_string(m_typeVote) + ")");
		return {};
	} catch (const std::exception& ex) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginStart failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginStart failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsVoteBKMPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		pisex::Poll(); // timers + deferred startup callbacks + queued menu changes
		m_sinceReconcile += dt;
		if (m_sinceReconcile >= kReconcileInterval) {
			m_sinceReconcile = std::chrono::milliseconds(0);
			ReconcileAuthorized();
		}
	} catch (const std::exception& ex) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginUpdate failed: %s", kLogTag, ex.what());
	} catch (...) {
		pisex::g_pUtils->ErrorLog("[%s] OnPluginUpdate failed with an unknown exception", kLogTag);
	}
	return {};
}

plg::PluginResult AsVoteBKMPlugin::OnPluginEnd() {
	try {
		pisex::Shutdown(); // commands, menus, timers, player hooks
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_asVoteBKMPlugin)
