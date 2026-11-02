// as_votebkm plugin tests — no engine: src/plugin.cpp is compiled into this TU (its handlers live
// in an anonymous namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording
// fake. The tests cover the safety contract of the port:
//   * the delayed vote result revalidates the captured SteamID64 and never punishes a new occupant
//     of a reused target slot; a target disconnect, a lost immunity answer, a missing admin core,
//     a map change and an unload all mean "no punishment";
//   * the voter identity is revalidated too (a reused voter slot cannot inherit a Yes vote);
//   * bots never count, never receive vote menus and never become targets;
//   * commands are chat-only, permission-gated and owned (RemoveCommand vs RemoveCommandListener);
//   * menu id 0 is a valid handle, negative slots/indices are ignored, menus are always destroyed;
//   * the map gate (B1/B2): no client call before the first map or between OnMapEnd and the next
//     OnMapStart; a plugin loaded into a running map latches readiness on its first update;
//   * the config keeps the raw-cwd-first pisexcompat order, the phrases fall back to the local
//     admin_system.phrases.txt with core.cfg ServerLang -> s2sdk -> "en", and OnPluginStart never
//     returns Failed even with every optional binding missing.
//
// Run by ./test.sh (build/tests/as_votebkm_tests); no pisexcompat and no engine are involved.

#include "../src/plugin.cpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	Check(actual == expected, what + " (got " + std::to_string(actual) + ", want " + std::to_string(expected) + ")");
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what) {
	Check(actual == expected, what + " (got \"" + std::string(actual) + "\", want \"" + std::string(expected) + "\")");
}

void Section(const char* name) {
	std::printf("== %s\n", name);
}

// ==========================================================================================
// filesystem fixtures
// ==========================================================================================

std::string g_configsDir;
std::string g_dataDir;
std::string g_workDir;
std::string g_originalCwd;

std::string Join(const std::string& a, const std::string& b) {
	return a + "/" + b;
}

std::string ConfigPath() {
	return Join(g_configsDir, "admin_system/votebkm.ini");
}

std::string PhrasesPath() {
	return Join(g_configsDir, "admin_system/translations/admin_system.phrases.txt");
}

std::string CoreConfigPath() {
	return Join(g_configsDir, "core.cfg");
}

std::string RawConfigPath() {
	return Join(g_workDir, "addons/configs/admin_system/votebkm.ini");
}

std::string RawPhrasePath() {
	return Join(g_workDir, "admin_system.phrases.txt");
}

void WriteTextFile(const std::string& path, std::string_view content) {
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << content;
}

bool CopyFile(const std::string& source, const std::string& target) {
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(target).parent_path(), error);
	std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
	return !error;
}

void RemoveFile(const std::string& path) {
	std::error_code error;
	std::filesystem::remove(path, error);
}

std::string ReadText(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string FixturePath(const char* name) {
	return std::string(AS_VOTEBKM_TEST_DATA_DIR) + "/" + name;
}

void SetUpDirs() {
	const std::string base = (std::filesystem::temp_directory_path() /
							  ("as_votebkm_tests_" + std::to_string(static_cast<long long>(::getpid()))))
								 .string();
	g_configsDir = Join(base, "configs");
	g_dataDir = Join(base, "data");
	g_workDir = Join(base, "game");
	std::error_code error;
	std::filesystem::remove_all(base, error);
	std::filesystem::create_directories(Join(g_configsDir, "admin_system/translations"), error);
	std::filesystem::create_directories(Join(g_dataDir, "admin_system"), error);
	std::filesystem::create_directories(Join(g_workDir, "addons/configs/admin_system"), error);
	std::filesystem::create_directories(Join(g_workDir, "addons/translations"), error);

	std::array<char, 4096> cwd{};
	if (::getcwd(cwd.data(), cwd.size()) != nullptr) {
		g_originalCwd = cwd.data();
	}
	Check(::chdir(g_workDir.c_str()) == 0, "test working directory can be entered");
}

void TearDownDirs() {
	if (!g_originalCwd.empty()) {
		(void)::chdir(g_originalCwd.c_str());
	}
	std::error_code error;
	std::filesystem::remove_all(std::filesystem::path(g_configsDir).parent_path(), error);
}

// ==========================================================================================
// logging / config / phrases
// ==========================================================================================

std::vector<std::string> g_logLines;
std::vector<std::string> g_errorLines;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	if (severity == plg::Severity::Error || severity == plg::Severity::Fatal) {
		g_errorLines.emplace_back(message);
	}
}

bool Logged(const std::string& needle) {
	for (const std::string& line : g_logLines) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

plg::string FakeGetConfigsDir() {
	return plg::string(std::string_view(g_configsDir));
}

plg::string FakeGetDataDir() {
	return plg::string(std::string_view(g_dataDir));
}

std::string g_serverLanguage = "en";

plg::string FakeGetServerLanguage() {
	return plg::string(std::string_view(g_serverLanguage));
}

std::map<std::string, std::string> g_translations;

void LoadTranslations(const char* language) {
	g_translations.clear();
	const std::string text = ReadText(PhrasesPath());
	const std::optional<votebkm::KvNode> root = votebkm::ParseKv1(text);
	if (!root) {
		return;
	}
	g_translations = votebkm::ResolvePhrases(*root, language);
}

void RestoreConfigFiles() {
	std::error_code error;
	std::filesystem::create_directories(Join(g_configsDir, "admin_system/translations"), error);
	Check(CopyFile(FixturePath("votebkm_test.ini"), ConfigPath()), "the test votebkm.ini is installed");
	Check(CopyFile(FixturePath("admin_system.phrases.txt"), PhrasesPath()), "the phrase fixture is installed");
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n    \"ServerLang\"    \"russian\"\n}\n");
	RemoveFile(RawConfigPath());
	RemoveFile(RawPhrasePath());
	g_serverLanguage = "en";
	LoadTranslations("en");
}

// ==========================================================================================
// fake clients / chat / kick
// ==========================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool bot = false;
	uint64_t steamid = 0;
	std::string name;
};

std::array<FakeClient, 64> g_fakeClients;
int g_clientCalls = 0;
int g_clientCallsWhileMapDown = 0;

int g_nextMenuId = 1;
std::vector<std::pair<int32_t, int32_t>> g_fakeKicks; // slot, reason
std::function<void(int32_t, const std::string&)> g_sendActionHook;
int g_eventSequence = 0;
int g_kickSequence = 0;
int g_successSequence = 0;

struct FakeChat {
	int32_t slot = -1;
	std::string text;
};

std::vector<FakeChat> g_chat;
std::vector<std::string> g_chatAll;
int g_chatWhileMapDown = 0;

void NoteClientCall() {
	++g_clientCalls;
	if (!g_mapReady) {
		++g_clientCallsWhileMapDown;
	}
}

bool FakeIsClientConnected(int32_t slot) {
	NoteClientCall();
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	NoteClientCall();
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	NoteClientCall();
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	NoteClientCall();
	return slot < 0 || slot >= 64 || g_fakeClients[static_cast<std::size_t>(slot)].bot;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	NoteClientCall();
	return slot >= 0 && slot < 64 ? g_fakeClients[static_cast<std::size_t>(slot)].steamid : 0;
}

plg::string FakeGetClientName(int32_t slot) {
	NoteClientCall();
	if (slot < 0 || slot >= 64) {
		return plg::string();
	}
	const FakeClient& client = g_fakeClients[static_cast<std::size_t>(slot)];
	return client.name.empty() ? plg::string(std::string_view("<unknown>")) : plg::string(std::string_view(client.name));
}

void FakePrintToChat(int32_t slot, const plg::string& message) {
	if (!g_mapReady) {
		++g_chatWhileMapDown;
	}
	g_chat.push_back(FakeChat{slot, std::string(message.data(), message.size())});
}

void FakePrintToChatAll(const plg::string& message) {
	if (!g_mapReady) {
		++g_chatWhileMapDown;
	}
	++g_eventSequence;
	const std::string text(message.data(), message.size());
	if (text.find("punished") != std::string::npos) {
		g_successSequence = g_eventSequence;
	}
	g_chatAll.push_back(text);
}

void FakeKickClient(int32_t slot, s2sdk::NetworkDisconnectionReason reason, const plg::string& /*message*/) {
	g_kickSequence = ++g_eventSequence;
	g_fakeKicks.emplace_back(slot, static_cast<int32_t>(reason));
}

// CEntityHandle: index | serial << 15. The serial is derived from the SteamID64, so the same slot
// reused by another player has a different handle (a stale menu/action is rejected twice: by the
// SteamID64 and by this handle).
std::map<int32_t, int32_t> g_controllerHandleOverride;

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	const auto overrideIt = g_controllerHandleOverride.find(slot);
	if (overrideIt != g_controllerHandleOverride.end()) {
		return overrideIt->second;
	}
	if (slot < 0 || slot >= 64 || !g_fakeClients[static_cast<std::size_t>(slot)].connected) {
		return -1;
	}
	const uint64_t steamid = g_fakeClients[static_cast<std::size_t>(slot)].steamid;
	if (steamid == 0) {
		return -1;
	}
	const uint32_t serial = static_cast<uint32_t>((steamid >> 32) ^ (steamid & 0xFFFFu));
	return static_cast<int32_t>((slot + 1) | ((serial & 0x7FFFu) << 15));
}

bool FakeIsValidEntHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}


bool AnyChatAll(const std::string& needle) {
	for (const std::string& line : g_chatAll) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

int CountChatAll(const std::string& needle) {
	int count = 0;
	for (const std::string& line : g_chatAll) {
		if (line.find(needle) != std::string::npos) {
			++count;
		}
	}
	return count;
}

bool AnyChatTo(int32_t slot, const std::string& needle) {
	for (const FakeChat& line : g_chat) {
		if (line.slot == slot && line.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

FakeClient Human(uint64_t steamid, const std::string& name) {
	FakeClient client;
	client.connected = true;
	client.inGame = true;
	client.steamid = steamid;
	client.name = name;
	return client;
}

FakeClient Bot(const std::string& name) {
	FakeClient client = Human(0, name);
	client.bot = true;
	return client;
}

void SetClient(int32_t slot, const FakeClient& client) {
	if (slot >= 0 && slot < 64) {
		g_fakeClients[static_cast<std::size_t>(slot)] = client;
	}
}

void DisconnectClient(int32_t slot) {
	if (slot >= 0 && slot < 64) {
		g_fakeClients[static_cast<std::size_t>(slot)] = FakeClient{};
	}
}

// ==========================================================================================
// fake commands / listeners
// ==========================================================================================

struct FakeCommand {
	s2sdk::ConCommandCallback callback = nullptr;
	bool listener = false; // true: AddConsoleCommand found an existing name and attached a listener
};

std::map<std::string, FakeCommand> g_fakeCommands;
std::set<std::string> g_preexistingCommands;
std::vector<std::string> g_addedCommands;
std::vector<std::string> g_removedCommands;
std::vector<std::string> g_addedListeners;
std::vector<std::string> g_removedListeners;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/, s2sdk::ConVarFlag /*flags*/,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode /*mode*/) {
	const std::string key(name.data(), name.size());
	g_addedCommands.push_back(key);
	FakeCommand command;
	command.callback = callback;
	command.listener = g_preexistingCommands.count(key) != 0;
	g_fakeCommands[key] = command;
	return !command.listener;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/) {
	const std::string key(name.data(), name.size());
	g_removedCommands.push_back(key);
	g_fakeCommands.erase(key);
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback, s2sdk::HookMode /*mode*/) {
	const std::string key(name.data(), name.size());
	g_addedListeners.push_back(key);
	FakeCommand command = g_fakeCommands[key];
	command.callback = callback;
	command.listener = true;
	g_fakeCommands[key] = command;
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/, s2sdk::HookMode /*mode*/) {
	const std::string key(name.data(), name.size());
	g_removedListeners.push_back(key);
	const auto it = g_fakeCommands.find(key);
	if (it != g_fakeCommands.end()) {
		it->second.callback = nullptr;
	}
	return true;
}

s2sdk::ResultType InvokeCommand(const std::string& name, int32_t slot, s2sdk::ConCommandContext context,
								const std::vector<std::string>& extra = {}) {
	const auto it = g_fakeCommands.find(name);
	if (it == g_fakeCommands.end() || it->second.callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	plg::vector<plg::string> args;
	args.emplace_back(plg::string(std::string_view(name)));
	for (const std::string& value : extra) {
		args.emplace_back(plg::string(std::string_view(value)));
	}
	return it->second.callback(slot, context, args);
}

s2sdk::ResultType InvokeGlobalCommand(int32_t slot, s2sdk::ConCommandContext context,
									  const std::vector<std::string>& args) {
	const auto it = g_fakeCommands.find("");
	if (it == g_fakeCommands.end() || it->second.callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	plg::vector<plg::string> argv;
	for (const std::string& value : args) {
		argv.emplace_back(plg::string(std::string_view(value)));
	}
	return it->second.callback(slot, context, argv);
}

// ==========================================================================================
// fake menus
// ==========================================================================================

struct FakeMenuItem {
	std::string info;
	std::string display;
};

struct FakeMenu {
	s2sdk::MenuId id = 0;
	std::string title;
	std::string type;
	std::vector<FakeMenuItem> items;
	bool exitButton = false;
	bool backButton = false;
	int32_t pagination = 0;
	int32_t displayTime = 0;
	s2sdk::MenuHandlerCallback handler = nullptr;
	std::vector<int32_t> displayedTo;
};

std::map<s2sdk::MenuId, FakeMenu> g_fakeMenus;
std::array<s2sdk::MenuId, 64> g_clientMenu{};
std::set<std::string> g_registeredMenuTypes;
std::array<std::string, 64> g_playerMenuType{};
std::vector<std::string> g_menuEvents; // "create:id", "cancel:slot:reason", "destroy:id"
int g_menuCallsWhileMapDown = 0;
int g_backendCloseCalls = 0;
int g_liveCallbackCalls = 0;
bool g_cancelClientMenuRefuses = false;
bool g_setMenuTypeFails = false;
std::string g_hudmenuDefaultType = "button";

plg::string FakeHudmenuGetClientMenuType(int32_t slot) {
	if (slot >= 0 && slot < 64 && !g_playerMenuType[static_cast<std::size_t>(slot)].empty()) {
		return plg::string(std::string_view(g_playerMenuType[static_cast<std::size_t>(slot)]));
	}
	return plg::string(std::string_view(g_hudmenuDefaultType));
}

s2sdk::MenuId FakeCreateMenu(const plg::string& title, s2sdk::MenuHandlerCallback handler, const plg::string& type) {
	// CreateMenu allocates a menu object; it does not touch a client, so the map gate does not
	// apply to the id-0 reservation done in OnPluginStart. Display/Add/Destroy do.
	FakeMenu menu;
	menu.id = static_cast<s2sdk::MenuId>(g_nextMenuId++);
	menu.title.assign(title.data(), title.size());
	menu.type.assign(type.data(), type.size());
	menu.handler = handler;
	g_fakeMenus[menu.id] = std::move(menu);
	g_menuEvents.push_back("create:" + std::to_string(menu.id));
	return static_cast<s2sdk::MenuId>(g_nextMenuId - 1);
}

bool FakeDestroyMenu(s2sdk::MenuId id) {
	if (!g_mapReady) {
		++g_menuCallsWhileMapDown;
	}
	g_menuEvents.push_back("destroy:" + std::to_string(id));
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return false;
	}
	const bool noOpClose = it->second.type == "chat" || it->second.type == "console";
	std::vector<int32_t> displayed;
	for (const int32_t slot : it->second.displayedTo) {
		if (slot >= 0 && slot < 64 && g_clientMenu[static_cast<std::size_t>(slot)] == id) {
			displayed.push_back(slot);
			g_clientMenu[static_cast<std::size_t>(slot)] = 0;
		}
	}
	const s2sdk::MenuHandlerCallback handler = it->second.handler;
	g_fakeMenus.erase(it);
	// A still-active display goes through the backend close; the builtin chat/console types are
	// no-ops. A close over a live display invokes the plugin handler (live callback).
	if (!displayed.empty() && !noOpClose) {
		++g_backendCloseCalls;
		if (handler != nullptr) {
			++g_liveCallbackCalls;
			for (const int32_t slot : displayed) {
				handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(s2sdk::MenuCancelReason::Destroyed));
				handler(id, s2sdk::MenuAction::End, slot, 0);
			}
		}
	}
	return true;
}

int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle /*style*/) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return -1;
	}
	it->second.items.push_back(FakeMenuItem{std::string(info.data(), info.size()), std::string(display.data(), display.size())});
	return static_cast<int32_t>(it->second.items.size() - 1);
}

bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t time) {
	if (!g_mapReady) {
		++g_menuCallsWhileMapDown;
	}
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return false;
	}
	it->second.displayedTo.push_back(slot);
	it->second.displayTime = time;
	if (slot >= 0 && slot < 64) {
		g_clientMenu[static_cast<std::size_t>(slot)] = id;
	}
	return true;
}

s2sdk::MenuId FakeGetClientMenu(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return 0;
	}
	return g_clientMenu[static_cast<std::size_t>(slot)];
}

bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason) {
	if (!g_mapReady) {
		++g_menuCallsWhileMapDown;
	}
	g_menuEvents.push_back("cancel:" + std::to_string(slot) + ":" + std::to_string(static_cast<int32_t>(reason)));
	if (slot < 0 || slot >= 64 || g_cancelClientMenuRefuses) {
		return false; // the manager contract violation the A7 fail-safe must survive
	}
	const s2sdk::MenuId id = g_clientMenu[static_cast<std::size_t>(slot)];
	if (id == 0) {
		return false;
	}
	g_clientMenu[static_cast<std::size_t>(slot)] = 0; // the display ended without a backend close
	return true;
}

bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& type) {
	if (g_setMenuTypeFails) {
		return false;
	}
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return false;
	}
	it->second.type.assign(type.data(), type.size());
	return true;
}

plg::string FakeGetMenuType(s2sdk::MenuId id) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return plg::string();
	}
	return plg::string(std::string_view(it->second.type));
}

bool FakeIsValidMenu(s2sdk::MenuId id) {
	return g_fakeMenus.find(id) != g_fakeMenus.end();
}

bool FakeIsMenuTypeRegistered(const plg::string& name) {
	const std::string type(name.data(), name.size());
	if (type == "chat" || type == "console" || type == "button" || type == "centerhtml") {
		return true; // s2sdk's built-in backends; chat/console have no-op closers
	}
	return g_registeredMenuTypes.count(type) != 0;
}

bool FakeSetMenuExitButton(s2sdk::MenuId id, bool enabled) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return false;
	}
	it->second.exitButton = enabled;
	return true;
}

bool FakeSetMenuExitBackButton(s2sdk::MenuId id, bool enabled) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return false;
	}
	it->second.backButton = enabled;
	return true;
}

bool FakeSetMenuPagination(s2sdk::MenuId id, int32_t itemsPerPage) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end()) {
		return false;
	}
	it->second.pagination = itemsPerPage;
	return true;
}

FakeMenu* LatestMenuForSlot(int32_t slot) {
	FakeMenu* found = nullptr;
	for (auto& entry : g_fakeMenus) {
		if (std::find(entry.second.displayedTo.begin(), entry.second.displayedTo.end(), slot) !=
			entry.second.displayedTo.end()) {
			if (found == nullptr || entry.first > found->id) {
				found = &entry.second;
			}
		}
	}
	return found;
}

FakeMenu* LatestVoteMenuForSlot(int32_t slot) {
	FakeMenu* found = nullptr;
	for (auto& entry : g_fakeMenus) {
		if (entry.second.items.size() != 2 || std::find(entry.second.displayedTo.begin(),
														entry.second.displayedTo.end(),
														slot) == entry.second.displayedTo.end()) {
			continue;
		}
		if (found == nullptr || entry.first > found->id) {
			found = &entry.second;
		}
	}
	return found;
}

int InfoIndex(const FakeMenu& menu, const std::string& info) {
	for (std::size_t i = 0; i < menu.items.size(); ++i) {
		if (menu.items[i].info == info) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void SelectMenu(s2sdk::MenuId id, int32_t slot, int32_t index) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end() || it->second.handler == nullptr) {
		Check(false, "select on an existing menu");
		return;
	}
	it->second.handler(id, s2sdk::MenuAction::Select, slot, index);
}

void CancelMenu(s2sdk::MenuId id, int32_t slot, s2sdk::MenuCancelReason reason) {
	const auto it = g_fakeMenus.find(id);
	if (it == g_fakeMenus.end() || it->second.handler == nullptr) {
		return;
	}
	it->second.handler(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
}

// ==========================================================================================
// fake events / map
// ==========================================================================================

std::map<std::string, s2sdk::EventCallback> g_fakeEvents;
std::vector<std::string> g_unhookedEvents;
int32_t g_eventSlot = -1;
int g_eventSlotCalls = 0;
int g_eventToken = 0;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode /*mode*/) {
	g_fakeEvents[std::string(name.data(), name.size())] = callback;
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback /*callback*/, s2sdk::HookMode /*mode*/) {
	const std::string key(name.data(), name.size());
	g_unhookedEvents.push_back(key);
	g_fakeEvents.erase(key);
	return s2sdk::EventHookError::Okay;
}

int32_t FakeGetEventPlayerSlot(void* /*event*/, const plg::string& /*key*/) {
	++g_eventSlotCalls;
	return g_eventSlot;
}

void FirePlayerEvent(const char* name, int32_t slot) {
	const auto it = g_fakeEvents.find(name);
	if (it == g_fakeEvents.end() || it->second == nullptr) {
		Check(false, std::string("event is hooked: ") + name);
		return;
	}
	g_eventSlot = slot;
	it->second(plg::string(std::string_view(name)), &g_eventToken, false);
}

s2sdk::OnMapStartCallback g_fakeMapStart = nullptr;
s2sdk::OnMapEndCallback g_fakeMapEnd = nullptr;
int g_mapStartRegisters = 0;
int g_mapStartUnregisters = 0;
int g_mapEndRegisters = 0;
int g_mapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_fakeMapStart = callback;
	++g_mapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_fakeMapStart == callback) {
		g_fakeMapStart = nullptr;
	}
	++g_mapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_fakeMapEnd = callback;
	++g_mapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_fakeMapEnd == callback) {
		g_fakeMapEnd = nullptr;
	}
	++g_mapEndUnregisters;
}

// ==========================================================================================
// fake admin_system
// ==========================================================================================

bool g_coreLoaded = true;
int32_t g_messageType = 0;
std::set<std::pair<int32_t, std::string>> g_permissions;
std::map<int32_t, int32_t> g_immunity;
std::set<std::pair<int32_t, int32_t>> g_punished;
std::set<std::pair<int32_t, uint64_t>> g_adminNotReady;
std::vector<std::pair<int32_t, int32_t>> g_punishments; // slot, type
std::vector<std::pair<int32_t, int32_t>> g_punishmentTimes;
std::vector<std::string> g_punishmentReasons;
std::vector<std::pair<int32_t, std::string>> g_fakeActions; // slot, action
std::vector<std::string> g_fakeActionParams;
admin_system::CoreLoadedCallback g_coreLoadedCallback = nullptr;
int g_coreLoadedRegisters = 0;
int g_coreLoadedUnregisters = 0;

float FakeGetPluginVersion() {
	return 1.0f;
}

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	return g_permissions.count({slot, std::string(permission.data(), permission.size())}) != 0;
}

bool FakeHasFlag(int32_t /*slot*/, const plg::string& /*flag*/) {
	return false;
}

bool FakeIsAdmin(int32_t slot) {
	for (const auto& permission : g_permissions) {
		if (permission.first == slot) {
			return true;
		}
	}
	return false;
}

bool FakeIsCoreLoaded() {
	return g_coreLoaded;
}

bool FakeIsClientAdminDataReady(int32_t slot, uint64_t steamid) {
	return g_adminNotReady.count({slot, steamid}) == 0;
}

int32_t FakeGetMessageType() {
	return g_messageType;
}

plg::string FakeGetTranslation(const plg::string& key) {
	const std::string name(key.data(), key.size());
	const auto it = g_translations.find(name);
	if (it != g_translations.end()) {
		return plg::string(std::string_view(it->second));
	}
	return key;
}

int32_t FakeGetAdminImmunity(int32_t slot) {
	const auto it = g_immunity.find(slot);
	return it == g_immunity.end() ? 0 : it->second;
}

bool FakeIsPlayerPunished(int32_t slot, int32_t type) {
	return g_punished.count({slot, type}) != 0;
}

void FakeAddPlayerPunishment(int32_t slot, int32_t type, int32_t time, const plg::string& reason, int32_t /*adminID*/,
							 bool /*notify*/, bool /*db*/) {
	g_punishments.emplace_back(slot, type);
	g_punishmentTimes.emplace_back(time, 0);
	g_punishmentReasons.emplace_back(reason.data(), reason.size());
}

void FakeSendAction(int32_t slot, const plg::string& action, const plg::string& param) {
	const std::string actionText(action.data(), action.size());
	const std::string paramText(param.data(), param.size());
	g_fakeActions.emplace_back(slot, actionText);
	g_fakeActionParams.push_back(paramText);
	if (g_sendActionHook) {
		g_sendActionHook(slot, paramText);
	}
}

void FakeCoreLoadedRegister(admin_system::CoreLoadedCallback callback) {
	g_coreLoadedCallback = callback;
	++g_coreLoadedRegisters;
}

void FakeCoreLoadedUnregister(admin_system::CoreLoadedCallback callback) {
	if (g_coreLoadedCallback == callback) {
		g_coreLoadedCallback = nullptr;
	}
	++g_coreLoadedUnregisters;
}

void FireCoreLoaded() {
	if (g_coreLoadedCallback != nullptr) {
		g_coreLoadedCallback();
	}
}

// ==========================================================================================
// installation / lifecycle helpers
// ==========================================================================================

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::GetDataDir = &FakeGetDataDir;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_KickClient = &FakeKickClient;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;

	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToChatAll = &FakePrintToChatAll;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;

	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__hudmenu_GetClientMenuType = &FakeHudmenuGetClientMenuType;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_HasPermission = &FakeHasPermission;
	__admin_system_HasFlag = &FakeHasFlag;
	__admin_system_IsAdmin = &FakeIsAdmin;
	__admin_system_IsCoreLoaded = &FakeIsCoreLoaded;
	__admin_system_IsClientAdminDataReady = &FakeIsClientAdminDataReady;
	__admin_system_GetMessageType = &FakeGetMessageType;
	__admin_system_GetTranslation = &FakeGetTranslation;
	__admin_system_GetAdminImmunity = &FakeGetAdminImmunity;
	__admin_system_IsPlayerPunished = &FakeIsPlayerPunished;
	__admin_system_AddPlayerPunishment = &FakeAddPlayerPunishment;
	__admin_system_SendAction = &FakeSendAction;
	__admin_system_OnCoreLoaded_Register = &FakeCoreLoadedRegister;
	__admin_system_OnCoreLoaded_Unregister = &FakeCoreLoadedUnregister;
}

void ResetWorld() {
	InstallFakes();
	RestoreConfigFiles();
	g_fakeClients.fill(FakeClient{});
	g_clientCalls = 0;
	g_clientCallsWhileMapDown = 0;
	g_fakeKicks.clear();
	g_sendActionHook = nullptr;
	g_eventSequence = 0;
	g_kickSequence = 0;
	g_successSequence = 0;
	g_chat.clear();
	g_chatAll.clear();
	g_chatWhileMapDown = 0;
	g_menuCallsWhileMapDown = 0;
	g_nextMenuId = 1;
	g_fakeMenus.clear();
	g_clientMenu.fill(0);
	g_registeredMenuTypes.clear();
	g_playerMenuType.fill(std::string());
	g_hudmenuDefaultType = "button";
	g_menuEvents.clear();
	g_controllerHandleOverride.clear();
	g_backendCloseCalls = 0;
	g_liveCallbackCalls = 0;
	g_cancelClientMenuRefuses = false;
	g_setMenuTypeFails = false;
	g_fakeCommands.clear();
	g_preexistingCommands.clear();
	g_addedCommands.clear();
	g_removedCommands.clear();
	g_addedListeners.clear();
	g_removedListeners.clear();
	g_fakeEvents.clear();
	g_unhookedEvents.clear();
	g_eventSlot = -1;
	g_eventSlotCalls = 0;
	g_fakeMapStart = nullptr;
	g_fakeMapEnd = nullptr;
	g_mapStartRegisters = 0;
	g_mapStartUnregisters = 0;
	g_mapEndRegisters = 0;
	g_mapEndUnregisters = 0;
	g_coreLoaded = true;
	g_messageType = 0;
	g_permissions.clear();
	g_immunity.clear();
	g_punished.clear();
	g_adminNotReady.clear();
	g_punishments.clear();
	g_punishmentTimes.clear();
	g_punishmentReasons.clear();
	g_fakeActions.clear();
	g_fakeActionParams.clear();
	g_coreLoadedCallback = nullptr;
	g_coreLoadedRegisters = 0;
	g_coreLoadedUnregisters = 0;
	g_logLines.clear();
	g_errorLines.clear();
	admin_system::AdminApi::Get().ReloadTranslations();
}

bool g_pluginStarted = false;

void StopPlugin() {
	if (g_pluginStarted) {
		Check(g_asVoteBKMPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
		g_pluginStarted = false;
	}
}

void PrepareTest() {
	InstallFakes();
	StopPlugin();
	ResetWorld();
}

void StartPlugin() {
	const plg::PluginResult result = g_asVoteBKMPlugin.OnPluginStart();
	g_pluginStarted = true;
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart is Ok and never Failed");
}

void FireMapStart() {
	if (g_fakeMapStart == nullptr) {
		Check(false, "OnMapStart listener is registered");
		return;
	}
	g_fakeMapStart();
}

void FireMapEnd() {
	if (g_fakeMapEnd == nullptr) {
		Check(false, "OnMapEnd listener is registered");
		return;
	}
	g_fakeMapEnd();
}

void Advance(int64_t milliseconds) {
	g_asVoteBKMPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
}

void StartGame() {
	StartPlugin();
	FireMapStart();
	Advance(0);
}

void DeleteAllMenusForTest();

constexpr uint64_t kAdminSteam = 76561198000000001ULL;
constexpr uint64_t kTargetSteam = 76561198000000002ULL;
constexpr uint64_t kP2Steam = 76561198000000003ULL;
constexpr uint64_t kP3Steam = 76561198000000004ULL;
constexpr uint64_t kP4Steam = 76561198000000005ULL;
constexpr uint64_t kNewSteam = 76561198000000099ULL;

bool NoDanglingFakeCallbacks() {
	for (const auto& entry : g_fakeCommands) {
		if (entry.second.callback != nullptr) {
			return false;
		}
	}
	return g_fakeEvents.empty() && g_fakeMapStart == nullptr && g_fakeMapEnd == nullptr;
}

void Grant(int32_t slot, const char* permission) {
	g_permissions.insert({slot, permission});
}

void Revoke(int32_t slot, const char* permission) {
	g_permissions.erase({slot, permission});
}

void SetUpFourHumans() {
	SetClient(0, Human(kAdminSteam, "Admin"));
	SetClient(1, Human(kTargetSteam, "Target"));
	SetClient(2, Human(kP2Steam, "Voter2"));
	SetClient(3, Human(kP3Steam, "Voter3"));
	Grant(0, "@admin/votebkm");
}

void SetUpFiveHumans() {
	SetUpFourHumans();
	SetClient(4, Human(kP4Steam, "Voter5"));
}

int32_t PlayersCount() {
	return GetPlayersCount();
}

// --- menu/user-flow helpers ---------------------------------------------------------------

bool OpenVoteTypeMenu(int32_t slot) {
	const s2sdk::ResultType result = InvokeCommand("votekick", slot, s2sdk::ConCommandContext::Chat);
	Check(result == s2sdk::ResultType::Handled, "the chat vote command is handled");
	return LatestMenuForSlot(slot) != nullptr;
}

void ChooseVoteType(int32_t slot, const std::string& info) {
	FakeMenu* menu = LatestMenuForSlot(slot);
	Check(menu != nullptr, "the vote type menu is open");
	if (menu == nullptr) {
		return;
	}
	const s2sdk::MenuId id = menu->id;
	const int index = InfoIndex(*menu, info);
	Check(index >= 0, "the vote type menu has the item " + info);
	if (index < 0) {
		return;
	}
	SelectMenu(id, slot, index);
	Advance(0);
}

void ChooseTarget(int32_t slot, uint64_t steamid) {
	FakeMenu* menu = LatestMenuForSlot(slot);
	Check(menu != nullptr, "the player menu is open");
	if (menu == nullptr) {
		return;
	}
	const s2sdk::MenuId id = menu->id;
	int index = InfoIndex(*menu, std::to_string(steamid));
	if (index < 0) {
		// A player menu item for a steamid may have been filtered out.
		Check(false, "the target is in the player menu");
		return;
	}
	SelectMenu(id, slot, index);
	Advance(0);
}

void ChooseBanTarget(int32_t slot, uint64_t steamid) {
	Check(OpenVoteTypeMenu(slot), "the vote type menu opened");
	ChooseVoteType(slot, "0");
	ChooseTarget(slot, steamid);
}

void ChooseKickTarget(int32_t slot, uint64_t steamid) {
	Check(OpenVoteTypeMenu(slot), "the vote type menu opened");
	ChooseVoteType(slot, "3");
	ChooseTarget(slot, steamid);
}

void ChooseMuteTarget(int32_t slot, uint64_t steamid) {
	Check(OpenVoteTypeMenu(slot), "the vote type menu opened");
	ChooseVoteType(slot, "1");
	ChooseTarget(slot, steamid);
}

void ChooseGagTarget(int32_t slot, uint64_t steamid) {
	Check(OpenVoteTypeMenu(slot), "the vote type menu opened");
	ChooseVoteType(slot, "2");
	ChooseTarget(slot, steamid);
}

void CastYes(int32_t slot) {
	FakeMenu* menu = LatestVoteMenuForSlot(slot);
	Check(menu != nullptr, "the Yes/No menu is open for the voter");
	if (menu == nullptr) {
		return;
	}
	const s2sdk::MenuId id = menu->id;
	SelectMenu(id, slot, InfoIndex(*menu, "0"));
	Advance(0);
}

void CastNo(int32_t slot) {
	FakeMenu* menu = LatestVoteMenuForSlot(slot);
	Check(menu != nullptr, "the Yes/No menu is open for the voter");
	if (menu == nullptr) {
		return;
	}
	const s2sdk::MenuId id = menu->id;
	SelectMenu(id, slot, InfoIndex(*menu, "1"));
	Advance(0);
}

// ==========================================================================================
// A. startup, config, command ownership
// ==========================================================================================

void TestStartupAndCommands() {
	Section("A. startup: config, registrations, chat/console command ownership");
	PrepareTest();
	StartPlugin();
	Check(g_mapStartRegisters == 1 && g_mapEndRegisters == 1, "the map listeners are registered exactly once");
	Check(g_fakeEvents.count("player_disconnect") == 1 && g_fakeEvents.count("player_connect_full") == 1,
		  "both player events are hooked");
	Check(g_coreLoadedRegisters == 1, "the admin_system OnCoreLoaded callback is registered");
	CheckEqInt(static_cast<int64_t>(g_addedCommands.size()), 4, "the four non-engine commands are registered");
	for (const char* name : {"votekick", "voteban", "votemute", "votegag"}) {
		Check(g_fakeCommands.count(name) == 1, std::string("the command is registered: ") + name);
	}
	// !vote is CS2's own engine command: it must be a global listener, never a shadowing ConCommand.
	Check(g_fakeCommands.count("vote") == 0, "!vote is not registered as a ConCommand");
	Check(g_fakeCommands.count("") == 1, "the global command listener handles the !vote spelling");
	Check(std::find(g_addedListeners.begin(), g_addedListeners.end(), "") != g_addedListeners.end(),
		  "the global listener was installed");
	CheckEqInt(g_clientCalls, 0, "OnPluginStart made no client call (PITFALLS B1)");
	CheckEqInt(g_clientCallsWhileMapDown, 0, "no client call while no map is up");

	// Before the first map a chat command is swallowed but does nothing and touches no client.
	SetUpFourHumans();
	const s2sdk::ResultType beforeMap = InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(beforeMap == s2sdk::ResultType::Handled, "the chat line is ours even before the first map");
	CheckEqInt(g_clientCalls, 0, "the pre-map command touched no client");
	Check(LatestMenuForSlot(0) == nullptr, "no menu was created before the first map");

	// The console spelling of a chat command is not ours (pisexcompat's console list was empty).
	FireMapStart();
	Advance(0);
	CheckEqInt(static_cast<int>(InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Console)),
			   static_cast<int>(s2sdk::ResultType::Continue), "the console context continues to the engine");
	Check(LatestMenuForSlot(0) == nullptr, "the console context did not open a menu");

	// Server console/out-of-range callers are fail-closed: only a real player slot can open a vote.
	for (const int32_t caller : {-1, -2, 64}) {
		InvokeCommand("votekick", caller, s2sdk::ConCommandContext::Chat);
		Check(LatestMenuForSlot(caller >= 0 ? caller : 0) == nullptr,
			  "no menu for a console/out-of-range caller " + std::to_string(caller));
	}

	// Permission gate.
	Revoke(0, "@admin/votebkm");
	CheckEqInt(static_cast<int>(InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat)),
			   static_cast<int>(s2sdk::ResultType::Handled), "an unauthorised chat line is still swallowed");
	Check(LatestMenuForSlot(0) == nullptr, "an unauthorised caller gets no menu");
	Grant(0, "@admin/votebkm");

	// The !vote engine-command spelling goes through the global listener; its console path is the
	// engine's own vote and continues untouched.
	CheckEqInt(static_cast<int>(InvokeGlobalCommand(0, s2sdk::ConCommandContext::Console, {"vote"})),
			   static_cast<int>(s2sdk::ResultType::Continue), "the console 'vote' continues to the engine");
	Check(LatestMenuForSlot(0) == nullptr, "the console 'vote' opened no menu");
	CheckEqInt(static_cast<int>(InvokeGlobalCommand(0, s2sdk::ConCommandContext::Chat, {"vote"})),
			   static_cast<int>(s2sdk::ResultType::Handled), "the chat '!vote' is handled");
	Check(LatestMenuForSlot(0) != nullptr, "the chat '!vote' opens the vote menu");
	CancelMenu(LatestMenuForSlot(0)->id, 0, s2sdk::MenuCancelReason::Exit);
	Advance(0);

	Check(OpenVoteTypeMenu(0), "an authorised chat command opens the vote type menu");
	FakeMenu* menu = LatestMenuForSlot(0);
	Check(menu != nullptr, "the menu exists");
	if (menu != nullptr) {
		CheckEqInt(static_cast<int64_t>(menu->items.size()), 4, "the vote type menu has four items");
		if (menu->items.size() == 4) {
			CheckEqStr(menu->items[0].info, "0", "Ban is first, info 0");
			CheckEqStr(menu->items[1].info, "3", "Kick is second, info 3");
			CheckEqStr(menu->items[2].info, "1", "Mute is third, info 1");
			CheckEqStr(menu->items[3].info, "2", "Gag is fourth, info 2");
		}
		Check(menu->exitButton && !menu->backButton, "the vote type menu has exit, no back");
		CheckEqInt(menu->pagination, 5, "pisexcompat's five items per page");
		CheckEqInt(menu->displayTime, 60, "pisexcompat's 60 second menu timeout");
		CheckEqStr(menu->type, "button", "without a hudmenu backend the built-in button type is used");
		Check(g_menus[menu->id].initiatorController != 0,
			  "the menu captured a non-zero controller handle for a human");
	}
}

void TestCommandOwnership() {
	Section("A2. command ownership: owned vs pre-existing name, say listeners");
	PrepareTest();
	g_preexistingCommands.insert("voteban");
	StartPlugin();
	FireMapStart();
	Advance(0);
	CheckEqInt(static_cast<int64_t>(g_fakeCommands["voteban"].listener ? 1 : 0), 1,
			   "an existing voteban got only a listener");
	StopPlugin();
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "voteban") == g_removedCommands.end(),
		  "RemoveCommand is never called on somebody else's command");
	Check(std::find(g_removedCommands.begin(), g_removedCommands.end(), "votekick") != g_removedCommands.end(),
		  "the owned commands are removed with RemoveCommand");
	Check(std::find(g_removedListeners.begin(), g_removedListeners.end(), "voteban") != g_removedListeners.end(),
		  "the pre-existing name is detached with RemoveCommandListener");
	CheckEqInt(g_mapStartUnregisters, 1, "OnMapStart is unregistered");
	CheckEqInt(g_mapEndUnregisters, 1, "OnMapEnd is unregistered");
	CheckEqInt(g_coreLoadedUnregisters, 1, "the admin callback is unregistered");
	Check(g_fakeEvents.empty(), "both events are unhooked");
	Check(std::find(g_removedListeners.begin(), g_removedListeners.end(), "") != g_removedListeners.end(),
		  "the global !vote listener is removed on unload");
}

void TestBareSayCommands() {
	Section("A3. a bare configured spelling is dispatched through say/say_team");
	PrepareTest();
	Check(CopyFile(FixturePath("votebkm_bare.ini"), ConfigPath()), "the bare-command config is installed");
	StartPlugin();
	FireMapStart();
	Advance(0);
	SetUpFourHumans();
	Check(std::find(g_addedListeners.begin(), g_addedListeners.end(), "say") != g_addedListeners.end(),
		  "the say listener is installed");
	Check(std::find(g_addedListeners.begin(), g_addedListeners.end(), "say_team") != g_addedListeners.end(),
		  "the say_team listener is installed");
	const s2sdk::ResultType handled = InvokeCommand("say", 0, s2sdk::ConCommandContext::Chat, {"votekick"});
	Check(handled == s2sdk::ResultType::Handled, "a bare 'votekick' say line is consumed");
	Check(LatestMenuForSlot(0) != nullptr, "the bare say command opens the vote menu");
	const s2sdk::ResultType other = InvokeCommand("say", 0, s2sdk::ConCommandContext::Chat, {"hello"});
	Check(other == s2sdk::ResultType::Continue, "an ordinary say line continues");
	StopPlugin();
	Check(std::find(g_removedListeners.begin(), g_removedListeners.end(), "say") != g_removedListeners.end(),
		  "the say listener is removed on unload");
	Check(std::find(g_removedListeners.begin(), g_removedListeners.end(), "say_team") != g_removedListeners.end(),
		  "the say_team listener is removed on unload");
}

// ==========================================================================================
// B. the main type_vote=1 flow, counts and timings
// ==========================================================================================

void TestMainVoteFlow() {
	Section("B. type_vote=1: target menu, Yes/No menu, immediate threshold, cooldown");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	CheckEqInt(PlayersCount(), 4, "four human players are counted");
	CheckEqInt(votebkm::NeededVotes(4, 50), 2, "four players at 50% need two votes");

	ChooseBanTarget(0, kTargetSteam);
	Check(g_active.active, "the vote is active");
	CheckEqInt(static_cast<int64_t>(g_active.targetSteam), static_cast<int64_t>(kTargetSteam), "the target steamid is captured");
	CheckEqInt(g_active.targetSlot, 1, "the target slot is captured");
	CheckEqInt(static_cast<int64_t>(g_active.generation), 1, "the vote generation starts at 1");
	Check(AnyChatAll("created a vote to ban player"), "the created message names the initiator and target");
	for (int32_t slot = 0; slot < 4; ++slot) {
		Check(LatestVoteMenuForSlot(slot) != nullptr, "every human got the Yes/No menu");
	}
	Check(LatestVoteMenuForSlot(4) == nullptr, "an empty slot got no menu");

	CastNo(3); // a "No" closes that player's menu and records nothing
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a No vote does not punish");
	CastYes(2);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "one of two votes does not punish");
	Check(AnyChatAll("(1/2)"), "the action message prints the current count and the threshold");

	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 1, "the second vote reaches the threshold");
	if (!g_punishments.empty()) {
		CheckEqInt(g_punishments[0].first, 1, "the punishment is addressed to the captured target slot");
		CheckEqInt(g_punishments[0].second, admin_system::RT_BAN, "the punishment type is ban");
		CheckEqInt(g_punishmentTimes[0].first, 120, "time_punish is used");
		CheckEqStr(g_punishmentReasons[0], "TestReason", "reason is used");
	}
	Check(AnyChatAll("punished"), "the success phrase is printed");
	Check(!g_active.active, "the vote is no longer active after the punishment");

	// The result deadline must not punish a second time.
	Advance(5000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 1, "the result timer did not punish again");
	Check(!AnyChatAll("Vote failed due to lack of votes"), "no cancelled message after a successful vote");

	// The cooldown started with the punishment.
	DeleteAllMenusForTest();
	CheckEqInt(static_cast<int>(InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat)),
			   static_cast<int>(s2sdk::ResultType::Handled), "the command is still handled");
	Check(LatestMenuForSlot(0) == nullptr, "the cooldown suppresses a new vote menu");
	Check(AnyChatTo(0, "wait 60 seconds") || AnyChatTo(0, "wait 59 seconds") || AnyChatTo(0, "wait 58 seconds"),
		  "the cooldown phrase carries the remaining seconds");
}

void TestResultTimerAccuracy() {
	Section("B2. the delayed result fires exactly after result_timer seconds");
	PrepareTest();
	StartGame();
	SetUpFiveHumans(); // needed = 3, so two Yes votes do not punish early
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	CastYes(3);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "two of three votes do not punish");
	Advance(1999);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "nothing happens before the result deadline");
	Check(g_active.active, "the vote is still active just before the deadline");
	Advance(1);
	Check(!g_active.active, "the deadline closed the vote");
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a cancelled vote punished nobody");
	CheckEqInt(CountChatAll("Vote failed due to lack of votes"), 1, "the cancel phrase is printed once");
}

// ==========================================================================================
// C. the delayed result and a reused target slot
// ==========================================================================================

void TestDelayedResultReusedSlot() {
	Section("C. delayed result: a new occupant of the target slot is never punished");
	PrepareTest();
	StartGame();
	SetUpFiveHumans(); // 5 humans -> needed 3; two Yes votes stay below the threshold
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	CastYes(3);
	Check(g_active.active, "the vote is still running");
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "no early punishment");

	// The target leaves; a different player joins the same slot. One other player leaves too, so
	// the live count drops to four and the two recorded votes now meet the threshold: this is the
	// exact shape the old slot-based code punished on the result timer.
	FirePlayerEvent("player_disconnect", 1);
	SetClient(1, Human(kNewSteam, "Newcomer"));
	FirePlayerEvent("player_connect_full", 1);
	FirePlayerEvent("player_disconnect", 4);
	DisconnectClient(4);
	Advance(1000); // reconcile sees the new identity
	CheckEqInt(PlayersCount(), 4, "the live player count dropped to four");
	CheckEqInt(votebkm::NeededVotes(4, 50), 2, "two votes are now enough");
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "still no early punishment");

	Advance(1000); // reaches the 2 s result deadline
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the new occupant was never punished");
	Check(g_fakeKicks.empty(), "no kick was sent to the new occupant");
	Check(AnyChatAll("Vote failed due to lack of votes"), "the result was reported as cancelled");
	Check(!g_active.active, "the vote is closed");
}

void TestDelayedResultMissedReuse() {
	Section("C1a. delayed result: identity check is the last line of defence when reuse was missed");
	PrepareTest();
	StartGame();
	SetUpFiveHumans(); // needed 3
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	CastYes(3);
	Check(g_active.active, "the vote is running with two of three votes");
	// Neither the connect event nor the once-per-second reconcile runs before the deadline: the
	// slot state still says the old SteamID64 owns slot 1 while a different player is there.
	DisconnectClient(4);
	SetClient(1, Human(kNewSteam, "Newcomer"));
	g_clock = g_active.deadline + 0.001;
	EvaluateResultIfDue();
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the captured SteamID64 no longer matches slot 1");
	Check(g_fakeKicks.empty(), "no kick was sent");
	Check(AnyChatAll("Vote failed due to lack of votes"), "the vote was cancelled");
}

void TestVoterSlotReuse() {
	Section("C2. a reused voter slot cannot inherit the previous occupant's Yes vote");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2); // one of two needed votes
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "one vote does not punish");
	// The voter leaves and a different player takes slot 2. The old Yes must be dropped.
	FirePlayerEvent("player_disconnect", 2);
	SetClient(2, Human(kNewSteam, "Newcomer"));
	FirePlayerEvent("player_connect_full", 2);
	CastYes(0); // the admin's own Yes: with the old vote cleared this is only 1 of 2
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0,
			   "the previous occupant's Yes was dropped, so the threshold is not reached");
	CastYes(3); // a real second voter
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 1, "two real votes reach the threshold");
	if (!g_punishments.empty()) {
		CheckEqInt(g_punishments[0].first, 1, "the punishment still targets the captured player");
	}
}

void TestTargetDisconnect() {
	Section("D. target disconnect: no slot-based punishment, no kick");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2); // 1 of 2
	FirePlayerEvent("player_disconnect", 1);
	DisconnectClient(1);
	CastYes(0); // the threshold is met, but the target is gone
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "nobody was punished after the target left");
	Advance(3000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the result timer also punished nobody");
	Check(g_fakeKicks.empty(), "no engine kick was sent");
	Check(AnyChatAll("Vote failed due to lack of votes"), "the vote was cancelled");
}

// ==========================================================================================
// E. rights/core lost
// ==========================================================================================

void TestRightsLost() {
	Section("E. rights lost: target immunity/core unavailable => fail-open, no punishment");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	Grant(1, "@admin/votebkm_i"); // the target gained the immunity flag during the vote
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "an immune target is not punished");
	Advance(3000); // the result deadline
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the result timer did not override the immunity flag");
	Check(AnyChatAll("Vote failed due to lack of votes"), "the vote is cancelled");

	// Admin immunity limit.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	g_immunity[1] = 10;
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "immunity >= immunity_limit blocks the punishment");

	// Already punished with that type.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	// The target becomes muted while the vote runs: the punishment executed at the result must
	// re-check the live state and must not extend it.
	ChooseMuteTarget(0, kTargetSteam);
	CastYes(2);
	g_punished.insert({1, admin_system::RT_MUTE});
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a live punishment is not extended again");

	// The admin core goes away after the vote started: no punishment, no crash.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	g_coreLoaded = false;
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a missing core fails open");
	Advance(3000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the result timer also fails open");
	Check(g_pluginStarted, "the plugin is still loaded");
	g_coreLoaded = true;
	FireCoreLoaded(); // the admin core came back: the callback must be safe
	CheckEqInt(g_coreLoadedRegisters, 1, "the OnCoreLoaded callback was registered once");
}

void TestInitiatorPermissionLost() {
	Section("E2. the initiator losing @admin/votebkm closes the already-open menu");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "the admin opened the vote type menu");
	ChooseVoteType(0, "0");
	FakeMenu* players = LatestMenuForSlot(0);
	Check(players != nullptr, "the player menu is open");
	Revoke(0, "@admin/votebkm");
	if (players != nullptr) {
		const int index = InfoIndex(*players, std::to_string(kTargetSteam));
		SelectMenu(players->id, 0, index);
		Advance(0);
	}
	Check(!g_active.active, "no vote was created after the permission was lost");
	Check(LatestVoteMenuForSlot(2) == nullptr, "nobody received a Yes/No menu");
}

void TestAdminDataReadiness() {
	Section("E3. IsClientAdminDataReady gates the start and the punishment (stale rights denied)");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	// Another player takes the admin's slot before the core's async SELECT answered for him.
	SetClient(0, Human(kNewSteam, "Newcomer"));
	g_adminNotReady.insert({0, kNewSteam});
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a pending admin identity gets no vote menu");
	CheckEqInt(static_cast<int64_t>(g_clientCallsWhileMapDown), 0, "the gate stayed closed correctly");

	// The target's admin data goes pending between the votes: the threshold must not punish.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	g_adminNotReady.insert({1, kTargetSteam});
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "pending target admin data fails open (no punishment)");
	Advance(3000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the result timer also refuses to punish");
	Check(AnyChatAll("Vote failed due to lack of votes"), "the pending identity result is reported as cancelled");
	Check(g_pluginStarted, "the plugin is still alive after the denied vote");

	// Another player's admin SELECT is pending: the disable-flag answer is not authoritative, so a
	// new vote is not started (fail-soft, no misleading "not enough players").
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	g_adminNotReady.insert({2, kP2Steam});
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a pending admin identity blocks the vote start");

	// An old core without the readiness export is denied as well.
	PrepareTest();
	__admin_system_IsClientAdminDataReady = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing readiness export denies the menu");
	StopPlugin();
}

void TestVoteAuthorLost() {
	Section("E4. the vote author's identity/rights are revalidated before the punishment");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2); // 1 of 2
	// The initiating admin's slot changes hands while the vote is pending.
	SetClient(0, Human(kNewSteam, "Newcomer"));
	CastYes(3); // the target threshold is met, but the author is stale
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a stale author identity prevents the punishment");
	Advance(3000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the result timer also refuses the stale author");
	Check(AnyChatAll("Vote failed due to lack of votes"), "the vote is reported as cancelled");

	// The author disconnecting (unknown identity) is fail-soft too.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	FirePlayerEvent("player_disconnect", 0);
	DisconnectClient(0);
	CastYes(3);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a missing author identity prevents the punishment");
	Advance(3000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the result timer also refuses the missing author");
}

// ==========================================================================================
// F. bots
// ==========================================================================================

void TestBots() {
	Section("F. bots are not voters, targets or menu recipients");
	PrepareTest();
	StartGame();
	SetClient(0, Human(kAdminSteam, "Admin"));
	Grant(0, "@admin/votebkm");
	SetClient(1, Bot("Bot1"));
	SetClient(2, Bot("Bot2"));
	CheckEqInt(PlayersCount(), 1, "bots are not counted");
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "min_players is checked against humans only");
	Check(AnyChatTo(0, "Not enough players"), "the original's NotEnoughPlayers phrase is printed");

	SetClient(3, Human(kTargetSteam, "Target"));
	Grant(3, "@admin/votebkm");
	CheckEqInt(PlayersCount(), 2, "the human count is two");
	Check(OpenVoteTypeMenu(0), "two humans satisfy min_players=2");
	ChooseVoteType(0, "0");
	FakeMenu* players = LatestMenuForSlot(0);
	Check(players != nullptr, "the target menu is open");
	if (players != nullptr) {
		Check(InfoIndex(*players, "0") < 0, "a bot steamid is never a target");
		CheckEqInt(static_cast<int64_t>(players->items.size()), 2, "only the two humans are listed");
	}
	ChooseTarget(0, kTargetSteam);
	Check(LatestVoteMenuForSlot(1) == nullptr, "the bot slot got no vote menu");
	Check(LatestVoteMenuForSlot(2) == nullptr, "the second bot slot got no vote menu");
	Check(LatestVoteMenuForSlot(0) != nullptr && LatestVoteMenuForSlot(3) != nullptr,
		  "both humans got the vote menu");
	// A bot caller cannot open the menu.
	InvokeCommand("votekick", 1, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(1) == nullptr, "a bot caller gets no menu");
}

// ==========================================================================================
// G. map change, unload, reload, map gate
// ==========================================================================================

void TestMapChange() {
	Section("G. map change clears a running vote; no punishment on the new map");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	const int callsBeforeEnd = g_clientCalls;
	FireMapEnd();
	Check(!g_mapReady, "OnMapEnd closed the map gate");
	Check(!g_active.active, "OnMapEnd cleared the active vote");
	CheckEqInt(g_clientCalls, callsBeforeEnd, "OnMapEnd made no client call (PITFALLS B2)");
	CheckEqInt(g_chatWhileMapDown, 0, "OnMapEnd printed nothing");
	const std::size_t menusBeforeCommand = g_fakeMenus.size();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	CheckEqInt(g_clientCalls, callsBeforeEnd, "a command between maps touched no client");
	Check(g_fakeMenus.size() == menusBeforeCommand, "a command between maps opened no menu");

	SetClient(1, Human(kNewSteam, "Newcomer"));
	FireMapStart();
	Advance(0);
	Check(g_mapReady, "OnMapStart reopened the gate");
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the old vote did not survive the changelevel");
	Advance(5000);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the old deadline is gone");
	Check(g_fakeMenus.empty(), "the previous map's menus were destroyed");
}

void TestMapGateMidMapLoad() {
	Section("G2. a plugin loaded into a running map latches on its first update");
	PrepareTest();
	SetUpFourHumans();
	StartPlugin();
	Check(!g_mapReady, "the gate is shut before the first update");
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	CheckEqInt(g_clientCalls, 0, "no client call before readiness");
	Advance(0);
	Check(g_mapReady, "the first update opened the gate for a mid-map load");
	Check(OpenVoteTypeMenu(0), "the vote menu now opens");
}

void TestMapGateMapEndBeforeFirstUpdate() {
	Section("G3. an OnMapEnd before the first update never re-opens the gate");
	PrepareTest();
	SetUpFourHumans();
	StartPlugin();
	FireMapStart();
	FireMapEnd();
	Advance(0);
	Check(!g_mapReady, "the gate stays shut after OnMapEnd");
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	CheckEqInt(g_clientCalls, 0, "no client call while the gate is shut");
}

void DeleteAllMenusForTest() {
	ForgetAllMenus();
	Advance(0);
}

bool ApiPointerIsNull() {
	return g_pAdminApi == nullptr;
}

void TestUnloadAndReload() {
	Section("G4. unload teardown and a clean reload");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	Check(!g_fakeMenus.empty(), "menus exist before the unload");
	StopPlugin();
	CheckEqInt(static_cast<int64_t>(g_fakeMenus.size()), 0, "OnPluginEnd destroyed every menu");
	Check(g_fakeMapStart == nullptr && g_fakeMapEnd == nullptr, "the map listeners are gone");
	Check(g_fakeEvents.empty(), "the events are gone");
	Check(g_addedCommands.size() == g_removedCommands.size(), "every created command was removed");
	Check(ApiPointerIsNull(), "the admin API pointer is dropped");

	StartPlugin();
	FireMapStart();
	Advance(0);
	CheckEqInt(static_cast<int64_t>(g_addedCommands.size()), 8, "a reload registers the four commands once more");
	CheckEqInt(g_mapStartRegisters, 2, "a reload registers the map listener once more");
	Check(OpenVoteTypeMenu(0), "the reloaded plugin still works");
}

// ==========================================================================================
// H. type_vote=0, vote types, priority
// ==========================================================================================

void InstallTypeZeroConfig() {
	Check(CopyFile(FixturePath("votebkm_type0.ini"), ConfigPath()), "the type_vote=0 fixture is installed");
}

void TestTypeZero() {
	Section("H. type_vote=0: every player votes directly, the threshold punishes");
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	CheckEqInt(g_settings.typeVote, 0, "the type_vote=0 fixture is active");
	CheckEqInt(static_cast<int64_t>(PlayersCount()), 4, "four players");
	ChooseMuteTarget(0, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "one mute vote does not punish");
	Check(AnyChatAll("voted for mute player"), "the action message is printed");
	ChooseMuteTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 1, "the second mute vote punishes");
	if (!g_punishments.empty()) {
		CheckEqInt(g_punishments[0].second, admin_system::RT_MUTE, "mute type");
	}
}

void TestKickAndPriority() {
	Section("H2. kick path and the original ban > kick > mute > gag priority");
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "one kick vote does not punish");
	Grant(3, "@admin/votebkm");
	ChooseKickTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "no AddPlayerPunishment on the kick path");
	CheckEqInt(static_cast<int64_t>(g_fakeActions.size()), 1, "SendAction(kick) is used");
	if (!g_fakeActions.empty()) {
		CheckEqStr(g_fakeActions[0].second, "kick", "the action is kick");
		CheckEqStr(g_fakeActionParams[0], "1", "the action param is the captured target slot");
	}
	CheckEqInt(static_cast<int64_t>(g_fakeKicks.size()), 1, "the engine kick is sent from the update scheduler");
	if (!g_fakeKicks.empty()) {
		CheckEqInt(g_fakeKicks[0].first, 1, "the kick went to the captured target slot");
		CheckEqInt(g_fakeKicks[0].second, static_cast<int32_t>(s2sdk::NetworkDisconnectionReason::Kicked),
				   "the original 'Kicked' reason is used");
	}

	// Ban wins over kick when both reached the threshold.
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	Grant(3, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	ChooseBanTarget(2, kTargetSteam);
	ChooseBanTarget(3, kTargetSteam); // ban count 2 -> ban must win, even though kick also has 1
	CheckEqInt(static_cast<int64_t>(g_fakeActions.size()), 0, "ban wins over the kick path");
	Check(g_punishments.size() == 1 && g_punishments[0].second == admin_system::RT_BAN, "ban is applied first");
}

void TestGagAndNotEnoughPlayers() {
	Section("H3. gag path, min_players and the disable flag");
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseGagTarget(0, kTargetSteam);
	ChooseGagTarget(2, kTargetSteam);
	Check(g_punishments.size() == 1 && g_punishments[0].second == admin_system::RT_GAG, "gag is applied");

	// min_players: one human is below the configured four.
	PrepareTest();
	StartGame();
	SetClient(0, Human(kAdminSteam, "Admin"));
	Grant(0, "@admin/votebkm");
	InvokeCommand("votekick", 1, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(1) == nullptr, "a bot caller is not a player");
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "one player is below min_players=4");
	Check(AnyChatTo(0, "Not enough players"), "NotEnoughPlayers is printed");

	// disable_flag: a player carrying it makes the menu unavailable.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm_d");
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "the disable flag blocks the menu");
	Check(AnyChatTo(0, "Access denied"), "the VoteBKM_Admin phrase is printed");
}

void TestKickRevalidationAfterSendAction() {
	Section("H4. SendAction(kick) listeners may change state: revalidate target + author before KickClient");
	// Normal path: the engine kick runs and only then is the success reported.
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	ChooseKickTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_fakeKicks.size()), 1, "the normal path sends exactly one engine kick");
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "the kick path uses SendAction, not AddPlayerPunishment");
	Check(g_kickSequence > 0 && g_successSequence > g_kickSequence,
		  "the success message is printed only after the real KickClient");

	// A listener changes the controller serial behind the same SteamID64 while SendAction returns.
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	g_sendActionHook = [](int32_t /*adminSlot*/, const std::string& param) {
		int32_t target = -1;
		try {
			target = std::stoi(param);
		} catch (...) {
		}
		g_controllerHandleOverride[target] = -777777; // same SteamID64, different controller serial
	};
	ChooseKickTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_fakeKicks.size()), 0, "a changed controller serial skips KickClient");
	Check(!AnyChatAll("punished"), "no success message is claimed after the skipped kick");
	CheckEqInt(g_lastVoteEpoch, 0, "no cooldown is applied after the skipped kick");

	// A listener revokes the author's permission while SendAction returns.
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	g_sendActionHook = [](int32_t /*adminSlot*/, const std::string& /*param*/) {
		g_permissions.erase({0, "@admin/votebkm"});
		g_permissions.erase({2, "@admin/votebkm"}); // the acting author of the threshold vote
	};
	ChooseKickTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_fakeKicks.size()), 0, "lost author rights skip KickClient");
	Check(!AnyChatAll("punished"), "no success message after the lost-rights skip");
	CheckEqInt(g_lastVoteEpoch, 0, "no cooldown after the lost-rights skip");

	// A listener makes the target's admin data pending while SendAction returns.
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	g_sendActionHook = [](int32_t /*adminSlot*/, const std::string& param) {
		int32_t target = -1;
		try {
			target = std::stoi(param);
		} catch (...) {
		}
		g_adminNotReady.insert({target, kTargetSteam});
	};
	ChooseKickTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_fakeKicks.size()), 0, "pending target admin data skips KickClient");
	Check(!AnyChatAll("punished"), "no success message after the pending-readiness skip");

	// A missing engine kick binding is not reported as a successful punishment.
	PrepareTest();
	InstallTypeZeroConfig();
	StartGame();
	SetUpFourHumans();
	Grant(2, "@admin/votebkm");
	ChooseKickTarget(0, kTargetSteam);
	__s2sdk_KickClient = nullptr;
	ChooseKickTarget(2, kTargetSteam);
	CheckEqInt(static_cast<int64_t>(g_fakeKicks.size()), 0, "without KickClient no engine kick is recorded");
	Check(!AnyChatAll("punished"), "no success message without a real kick");
	CheckEqInt(g_lastVoteEpoch, 0, "no cooldown without a real kick");
}

// ==========================================================================================
// I. menu edges
// ==========================================================================================

void TestMenuEdges() {
	Section("I. menu id 0 is burned, negative slots/indices, back navigation, active vote");
	PrepareTest();
	g_nextMenuId = 0; // the first CreateMenu gets the ambiguous id 0
	StartGame();
	SetUpFourHumans();
	Check(g_fakeMenus.count(0) == 1, "the dummy menu reserved id 0 (PITFALLS C10)");
	Check(OpenVoteTypeMenu(0), "the first real menu is shown");
	FakeMenu* menu = LatestMenuForSlot(0);
	Check(menu != nullptr && menu->id != 0, "a real menu never uses the reserved id 0");
	if (menu != nullptr) {
		ChooseVoteType(0, "0");
		FakeMenu* players = LatestMenuForSlot(0);
		Check(players != nullptr, "the player menu opened after the id-0 menu");
		if (players != nullptr) {
			// Negative item index is ignored.
			SelectMenu(players->id, 0, -1);
			Advance(0);
			Check(!g_active.active, "a negative item index starts no vote");
			// Back returns to the vote type menu.
			const s2sdk::MenuId playersId = players->id;
			CancelMenu(playersId, 0, s2sdk::MenuCancelReason::ExitBack);
			Advance(0);
			FakeMenu* backMenu = LatestMenuForSlot(0);
			Check(backMenu != nullptr && backMenu->items.size() == 4, "Back reopens the vote type menu");
			if (backMenu != nullptr) {
				CancelMenu(backMenu->id, 0, s2sdk::MenuCancelReason::Exit);
				Advance(0);
				Check(LatestMenuForSlot(0) == nullptr, "Exit closes the menu without reopening");
			}
		}
	}
	CheckEqInt(g_menuCallsWhileMapDown, 0, "no menu call happened while the map gate was shut");

	// A second vote while one is running reports AlreadyActive.
	ChooseBanTarget(0, kTargetSteam);
	Check(OpenVoteTypeMenu(0), "the second menu opens");
	ChooseVoteType(0, "0");
	ChooseTarget(0, kTargetSteam);
	Check(AnyChatTo(0, "Vote is already active"), "the AlreadyActive phrase is printed");
	Check(g_active.active, "the first vote is still the active one");

	// The id-0 reservation survives a changelevel; a later real menu never becomes id 0 again.
	FireMapEnd();
	FireMapStart();
	Advance(0);
	Check(g_fakeMenus.count(0) == 1, "the reserved id-0 menu survives a map change");
	Check(OpenVoteTypeMenu(0), "the menu opens after the changelevel");
	FakeMenu* afterMap = LatestMenuForSlot(0);
	Check(afterMap != nullptr && afterMap->id != 0, "a real menu after a changelevel still avoids id 0");
}

void TestNoTargets() {
	Section("I2. no available target -> VoteBKM_NoPlayers");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	for (int32_t slot = 0; slot < 4; ++slot) {
		Grant(slot, "@admin/votebkm_i");
	}
	Check(OpenVoteTypeMenu(0), "the admin can still open the vote type menu");
	ChooseVoteType(0, "0");
	Check(LatestMenuForSlot(0) == nullptr, "no player menu was built");
	Check(AnyChatTo(0, "No available players"), "the NoPlayers phrase is printed");
}

void TestSignedEventSlot() {
	Section("I3. GetEventPlayerSlot uses a signed slot: a negative value is ignored");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	const int before = g_eventSlotCalls;
	FirePlayerEvent("player_disconnect", -1);
	CheckEqInt(g_eventSlotCalls, before + 1, "the event handler resolved the slot through GetEventPlayerSlot");
	Check(g_active.active, "a negative slot did not cancel the vote");
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 1, "the vote still completes for the real target");
}

// ==========================================================================================
// I4. per-player !menu type (optional hudmenu), map-down cancel order, stale menu reuse
// ==========================================================================================

void CloseTopMenuForTest(int32_t slot) {
	FakeMenu* menu = LatestMenuForSlot(slot);
	if (menu == nullptr) {
		return;
	}
	const s2sdk::MenuId id = menu->id;
	CancelMenu(id, slot, s2sdk::MenuCancelReason::Exit);
	Advance(0);
}

void TestMenuTypeChoice() {
	Section("I4. the player's own !menu choice through the optional hudmenu API");
	PrepareTest();
	StartGame();
	SetUpFourHumans();

	Check(OpenVoteTypeMenu(0), "the menu opens");
	FakeMenu* menu = LatestMenuForSlot(0);
	Check(menu != nullptr && menu->type == "button", "without hudmenu types the button backend is used");
	CloseTopMenuForTest(0);

	g_registeredMenuTypes.insert("hud");
	g_registeredMenuTypes.insert("wasd");
	g_playerMenuType[0] = "hud";
	Check(OpenVoteTypeMenu(0), "the menu opens with hudmenu's choice");
	menu = LatestMenuForSlot(0);
	Check(menu != nullptr && menu->type == "hud", "the player's hud choice wins");
	CloseTopMenuForTest(0);

	g_playerMenuType[0] = "chat";
	Check(OpenVoteTypeMenu(0), "the menu opens with the chat choice");
	menu = LatestMenuForSlot(0);
	Check(menu != nullptr && menu->type == "chat", "the player's chat choice wins");
	CloseTopMenuForTest(0);

	g_playerMenuType[0] = "hud";
	g_registeredMenuTypes.erase("hud");
	Check(OpenVoteTypeMenu(0), "the menu opens while hud is unregistered");
	menu = LatestMenuForSlot(0);
	Check(menu != nullptr && menu->type == "button", "a stale hud choice falls back to button");
	CloseTopMenuForTest(0);

	// Without the hudmenu plugin the binding is null and the Pisex default mapping still applies.
	g_registeredMenuTypes.insert("hud");
	g_playerMenuType[0].clear();
	__hudmenu_GetClientMenuType = nullptr;
	Check(OpenVoteTypeMenu(0), "the menu opens without hudmenu installed");
	menu = LatestMenuForSlot(0);
	Check(menu != nullptr && menu->type == "hud", "the default HUD_LAYOUT mapping uses the registered hud backend");
	CloseTopMenuForTest(0);
}

void TestMapDownCancelOrder() {
	Section("I5. map end cancels the client display before destroying the menu");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "a menu is displayed before the map end");
	const s2sdk::MenuId id = LatestMenuForSlot(0)->id;
	g_menuEvents.clear();
	FireMapEnd();
	Check(g_fakeMenus.empty(), "every menu is destroyed on OnMapEnd");
	CheckEqInt(static_cast<int64_t>(g_clientMenu[0]), 0, "the client menu pointer is cleared");
	const auto cancelIt = std::find_if(g_menuEvents.begin(), g_menuEvents.end(), [](const std::string& event) {
		return event.rfind("cancel:0:", 0) == 0;
	});
	const auto destroyIt = std::find(g_menuEvents.begin(), g_menuEvents.end(), "destroy:" + std::to_string(id));
	Check(cancelIt != g_menuEvents.end(), "CancelClientMenu(0, Disconnect) ran first");
	Check(destroyIt != g_menuEvents.end(), "DestroyMenu ran afterwards");
	Check(cancelIt < destroyIt, "the cancel precedes the destroy");
	if (cancelIt != g_menuEvents.end()) {
		CheckEqStr(*cancelIt, "cancel:0:2", "the cancel reason is Disconnect");
	}
}

void TestUnloadWithMapDownCancel() {
	Section("I6. an unload while the map gate is shut cancels first, then destroys");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "a menu exists");
	const s2sdk::MenuId id = LatestMenuForSlot(0)->id;
	g_mapReady = false; // simulate the B2 window without running OnMapEnd cleanup
	g_menuEvents.clear();
	StopPlugin();
	Check(g_fakeMenus.empty(), "the menu is destroyed on unload");
	const auto cancelIt = std::find_if(g_menuEvents.begin(), g_menuEvents.end(), [](const std::string& event) {
		return event.rfind("cancel:0:", 0) == 0;
	});
	const auto destroyIt = std::find(g_menuEvents.begin(), g_menuEvents.end(), "destroy:" + std::to_string(id));
	Check(cancelIt != g_menuEvents.end() && destroyIt != g_menuEvents.end() && cancelIt < destroyIt,
		  "unload-with-map-down cancels the display before DestroyMenu");
}

void TestStaleMenuAfterSlotReuse() {
	Section("I7. a deferred menu action is tied to SteamID64 + controller handle");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "the admin opens the vote type menu");
	ChooseVoteType(0, "0");
	FakeMenu* players = LatestMenuForSlot(0);
	Check(players != nullptr, "the player menu is open");
	if (players != nullptr) {
		const s2sdk::MenuId id = players->id;
		const int index = InfoIndex(*players, std::to_string(kTargetSteam));
		SetClient(0, Human(kNewSteam, "NewAdmin")); // the initiator slot now holds another player
		SelectMenu(id, 0, index);
		Advance(0);
	}
	Check(!g_active.active, "a stale menu action from the previous occupant starts no vote");
	Check(LatestVoteMenuForSlot(2) == nullptr, "nobody received a Yes/No menu");

	// A negative CEntityHandle is valid (PITFALLS C1): it is not rejected as "<= 0".
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	g_controllerHandleOverride[0] = -12345;
	Check(OpenVoteTypeMenu(0), "the menu opens with a negative controller handle");
	FakeMenu* signedMenu = LatestMenuForSlot(0);
	Check(signedMenu != nullptr && g_menus[signedMenu->id].initiatorController == -12345,
		  "a negative (signed) controller handle is captured, not rejected");
	ChooseVoteType(0, "0");
	ChooseTarget(0, kTargetSteam);
	Check(g_active.active, "a deferred action with a valid negative controller handle is applied");

	// A host that cannot provide the controller handle is not trusted with menus or punishments.
	PrepareTest();
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	StartGame();
	SetUpFourHumans();
	Check(!OpenVoteTypeMenu(0), "without PlayerSlotToEntHandle no menu is created");
	Check(!g_active.active, "no vote starts without the controller identity");
	StopPlugin();
}

void TestMapEndCancelRefused() {
	Section("I8. cancel refused on map end: no-op close switch avoids any backend close");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "a menu is displayed");
	const s2sdk::MenuId id = LatestMenuForSlot(0)->id;
	g_cancelClientMenuRefuses = true; // the manager contract violation the fail-safe must survive
	g_menuEvents.clear();
	FireMapEnd();
	Check(g_fakeMenus.empty(), "the menu was released despite the refused cancel");
	CheckEqInt(g_backendCloseCalls, 0, "no backend close ran over the map-down client layer");
	CheckEqInt(g_liveCallbackCalls, 0, "no live plugin callback ran over the map-down client layer");
	Check(std::find(g_menuEvents.begin(), g_menuEvents.end(), "destroy:" + std::to_string(id)) != g_menuEvents.end(),
		  "DestroyMenu was still called after the type switch");
	Check(!Logged("could not close menu"), "the no-op-close path did not need to park the menu");
	StopPlugin(); // unload while the map is down: nothing left to leak
}

void TestMapEndCancelRefusedUnresolved() {
	Section("I9. cancel refused and switch impossible: explicit A7_UNRESOLVED, no hidden close");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "a menu is displayed");
	g_cancelClientMenuRefuses = true;
	g_setMenuTypeFails = true;
	FireMapEnd();
	Check(g_fakeMenus.size() == 1, "the menu is parked instead of being destroyed over a dead client layer");
	CheckEqInt(g_backendCloseCalls, 0, "no backend close ran on map end");
	CheckEqInt(g_liveCallbackCalls, 0, "no live callback ran on map end");
	Check(Logged("could not close menu"), "the parked menu is logged");
	StopPlugin();
	Check(Logged("A7_UNRESOLVED"), "an unreleasable menu is reported, not silently dropped");
	CheckEqInt(g_backendCloseCalls, 0, "the unload path also avoided the backend close");
	CheckEqInt(g_liveCallbackCalls, 0, "the unload path ran no live plugin callback");
}

void TestMapEndParkedRetry() {
	Section("I10. a menu parked on map end is retried with a live map (no permanent leak)");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	Check(OpenVoteTypeMenu(0), "a menu is displayed");
	g_cancelClientMenuRefuses = true;
	g_setMenuTypeFails = true;
	FireMapEnd();
	Check(g_fakeMenus.size() == 1, "the menu is parked");
	FireMapStart();
	Advance(0);
	Check(g_fakeMenus.empty(), "the retry with a live map destroyed the menu");
	Check(g_backendCloseCalls > 0, "the live-map retry was allowed to run the backend close");
	StopPlugin();
	Check(!Logged("A7_UNRESOLVED"), "no unresolved menu after a successful retry");
}

void TestStaleYesActionChat() {
	Section("I11. a stale deferred Yes is silent for a reused slot, the original owner still gets the notice");
	// Slot reused before the queued action is processed: no chat, no punishment for the new player.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	FakeMenu* menu = LatestVoteMenuForSlot(2);
	Check(menu != nullptr, "the voter's Yes/No menu is open");
	if (menu != nullptr) {
		SelectMenu(menu->id, 2, InfoIndex(*menu, "0")); // queue the action, do not process it yet
	}
	g_active.active = false; // the vote ended (e.g. its result timer), the action is still queued
	SetClient(2, Human(kNewSteam, "Newcomer")); // a different live player/controller owns the slot
	g_chat.clear();
	g_chatAll.clear();
	Advance(0);
	Check(g_chat.empty(), "the new occupant receives no AlreadyEnded message");
	Check(g_chatAll.empty(), "the stale action prints nothing to chat");
	Check(g_punishments.empty(), "the stale action punishes nobody");

	// The same action while the original voter is still in the slot keeps the old notice.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	FakeMenu* same = LatestVoteMenuForSlot(2);
	Check(same != nullptr, "the voter's Yes/No menu is open");
	if (same != nullptr) {
		SelectMenu(same->id, 2, InfoIndex(*same, "0"));
	}
	g_active.active = false;
	g_chat.clear();
	g_chatAll.clear();
	Advance(0);
	Check(AnyChatTo(2, "The vote has already ended"),
		  "the original live owner still receives the AlreadyEnded notice");
}

// ==========================================================================================
// J. config and phrases
// ==========================================================================================

void TestConfigPrecedence() {
	Section("J. votebkm.ini: raw cwd first, missing/broken config keeps defaults and commands off");
	PrepareTest();
	WriteTextFile(RawConfigPath(), "\"VoteBKM\"\n{\n    \"percents\" \"90\"\n    \"commands\" \"!rawvote\"\n}\n");
	StartGame();
	SetUpFourHumans();
	CheckEqInt(g_settings.percents, 90, "the raw cwd votebkm.ini won over <configs>/admin_system");
	Check(g_fakeCommands.count("rawvote") == 1, "the raw cwd commands were registered");
	Check(g_fakeCommands.count("votekick") == 0, "the configs-dir commands were not registered");
	CheckEqInt(static_cast<int64_t>(g_addedCommands.size()), 1, "only the raw command exists");
	StopPlugin();

	// Missing file: defaults, no command (the original left the plugin without commands).
	PrepareTest();
	RemoveFile(ConfigPath());
	RemoveFile(RawConfigPath());
	StartGame();
	CheckEqInt(g_settings.percents, 40, "the default percents are used");
	Check(g_fakeCommands.empty(), "a missing config registers no command");
	Check(Logged("Failed to load the VoteBKM config"), "the missing config is logged");
	StopPlugin();

	// Malformed file: defaults, no command, but a working plugin.
	PrepareTest();
	Check(CopyFile(FixturePath("votebkm_bad.ini"), ConfigPath()), "the malformed config is installed");
	StartGame();
	CheckEqInt(g_settings.percents, 40, "the defaults survive a malformed config");
	Check(g_fakeCommands.empty(), "a malformed config registers no command");
	Check(Logged("Failed to parse the VoteBKM config"), "the parse failure is logged");
	Check(g_pluginStarted, "the plugin is still alive");
}

void TestPhraseFallbackAndServerLang() {
	Section("J2. phrases: raw-cwd fallback first, core.cfg ServerLang -> s2sdk -> en");
	PrepareTest();
	WriteTextFile(RawPhrasePath(),
				  "\"Phrases\"\n{\n    \"VoteBKM_AlreadyActive\"\n    {\n        \"ru\" \"СЫРОЙ\"\n"
				  "        \"ua\" \"СИРИЙ\"\n        \"en\" \"RAW\"\n    }\n}\n");
	StartGame();
	g_translations.erase("VoteBKM_AlreadyActive"); // force the local fallback
	admin_system::AdminApi::Get().ReloadTranslations();
	CheckEqStr(Phrase("VoteBKM_AlreadyActive"), "СЫРОЙ", "the raw cwd phrase file wins and ru is selected");
	StopPlugin();

	// No raw file: the configs-dir phrase file is used; without core.cfg the s2sdk
	// ServerLanguage is the second link of the chain.
	PrepareTest();
	RemoveFile(CoreConfigPath());
	g_serverLanguage = "ukrainian";
	StartGame();
	admin_system::AdminApi::Get().ReloadTranslations();
	g_translations.erase("VoteBKM_AlreadyActive");
	CheckEqStr(Phrase("VoteBKM_AlreadyActive"), "Голосування вже запущено",
			   "without core.cfg the s2sdk ServerLanguage is used");
	StopPlugin();

	// No core.cfg and no s2sdk language -> "en".
	PrepareTest();
	RemoveFile(CoreConfigPath());
	g_serverLanguage.clear();
	StartGame();
	admin_system::AdminApi::Get().ReloadTranslations();
	g_translations.erase("VoteBKM_AlreadyActive");
	CheckEqStr(Phrase("VoteBKM_AlreadyActive"), "Vote is already active", "the final fallback is en");
	StopPlugin();

	// The core translation wins over the local fallback when it exists.
	PrepareTest();
	StartGame();
	g_translations["VoteBKM_AlreadyActive"] = "CORE VALUE";
	admin_system::AdminApi::Get().ReloadTranslations();
	CheckEqStr(Phrase("VoteBKM_AlreadyActive"), "CORE VALUE", "the admin_system API is authoritative");
	StopPlugin();
}

void TestColorizedChat() {
	Section("J3. the cs2-menus colour tags reach the chat as control bytes");
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	CastYes(0);
	Check(AnyChatAll("\x0F"), "LIGHTRED became the chat control byte");
	Check(AnyChatAll("\x01"), "DEFAULT became the chat control byte");
	bool sawRawTag = false;
	for (const std::string& line : g_chatAll) {
		if (line.find("{LIGHTRED}") != std::string::npos || line.find("{DEFAULT}") != std::string::npos) {
			sawRawTag = true;
		}
	}
	Check(!sawRawTag, "no colour tag leaks into the chat text");
}

// ==========================================================================================
// K. fail-soft start
// ==========================================================================================

void TestCleanupBindingPairs() {
	Section("K1. every add binding requires its remove pair before any registration");
	// HookEvent without UnhookEvent: no event is hooked.
	PrepareTest();
	__s2sdk_UnhookEvent = nullptr;
	StartGame();
	Check(g_fakeEvents.empty(), "no event hook without UnhookEvent");
	Check(Logged("HookEvent/UnhookEvent pair"), "the missing event remover is reported");
	StopPlugin();
	Check(NoDanglingFakeCallbacks(), "no dangling event callback");

	// Map start/end registrations need both matching unregisters.
	for (const bool startUnregister : {true, false}) {
		PrepareTest();
		if (startUnregister) {
			__s2sdk_OnMapStart_Unregister = nullptr;
		} else {
			__s2sdk_OnMapEnd_Unregister = nullptr;
		}
		StartPlugin();
		CheckEqInt(g_mapStartRegisters, 0, "OnMapStart is not registered without its unregister");
		CheckEqInt(g_mapEndRegisters, 0, "OnMapEnd is not registered without its unregister");
		Advance(0);
		Check(!g_mapReady, "the map gate stays closed");
		SetUpFourHumans();
		InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
		CheckEqInt(g_clientCalls, 0, "no client call while the closed gate protects the missing pair");
		Check(LatestMenuForSlot(0) == nullptr, "no menu is created");
		StopPlugin();
		Check(NoDanglingFakeCallbacks(), "no dangling map listener");
	}

	// A command needs AddConsoleCommand + RemoveCommand + RemoveCommandListener before it exists.
	PrepareTest();
	__s2sdk_RemoveCommand = nullptr;
	StartGame();
	Check(g_addedCommands.empty(), "no owned ConCommand without RemoveCommand");
	Check(g_fakeCommands.count("") == 1, "the global !vote listener still has its full listener pair");
	StopPlugin();
	Check(NoDanglingFakeCallbacks(), "no dangling command callback");

	PrepareTest();
	__s2sdk_RemoveCommandListener = nullptr;
	StartGame();
	Check(g_addedCommands.empty(), "no owned ConCommand without RemoveCommandListener");
	Check(g_fakeCommands.count("") == 0, "no global listener without RemoveCommandListener");
	Check(g_addedListeners.empty(), "no listener is installed");
	StopPlugin();
	Check(NoDanglingFakeCallbacks(), "no dangling listener/command callback");

	PrepareTest();
	__s2sdk_AddCommandListener = nullptr;
	StartGame();
	CheckEqInt(static_cast<int64_t>(g_addedCommands.size()), 4, "owned commands still register without the listener add binding");
	Check(g_fakeCommands.count("") == 0, "the !vote global listener is not installed without AddCommandListener");
	StopPlugin();
	Check(NoDanglingFakeCallbacks(), "owned commands are removed normally");

	// Bare say listeners follow the same pair rule.
	PrepareTest();
	Check(CopyFile(FixturePath("votebkm_bare.ini"), ConfigPath()), "the bare-command config is installed");
	__s2sdk_RemoveCommandListener = nullptr;
	StartGame();
	Check(g_addedCommands.empty(), "no ConCommand without the listener remover");
	Check(g_addedListeners.empty(), "no say/say_team listener without the listener remover");
	Check(g_fakeCommands.count("say") == 0 && g_fakeCommands.count("say_team") == 0, "no bare chat hook");
	StopPlugin();
	Check(NoDanglingFakeCallbacks(), "no dangling bare-chat callback");

	// The admin OnCoreLoaded callback needs both halves as well.
	PrepareTest();
	__admin_system_OnCoreLoaded_Unregister = nullptr;
	StartGame();
	CheckEqInt(g_coreLoadedRegisters, 0, "OnCoreLoaded is not registered without its unregister");
	StopPlugin();
	Check(NoDanglingFakeCallbacks(), "no dangling admin callback");
}

void TestFailSoftStart() {
	Section("K. OnPluginStart stays Ok with optional bindings missing");
	PrepareTest();
	__s2sdk_AddConsoleCommand = nullptr;
	__s2sdk_CreateMenu = nullptr;
	__s2sdk_DestroyMenu = nullptr;
	__s2sdk_AddMenuItem = nullptr;
	__s2sdk_DisplayMenu = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_GetEventPlayerSlot = nullptr;
	__admin_system_GetPluginVersion = nullptr;
	StartPlugin();
	Check(g_pluginStarted, "the plugin started");
	Check(Logged("AddConsoleCommand/RemoveCommand/RemoveCommandListener pair"), "the missing command pair is logged");
	Check(Logged("menu bindings are unavailable"), "the missing menu bindings are logged");
	Check(Logged("map listener Register/Unregister pair"), "the missing map listener pair is logged");
	StopPlugin();
	Check(!g_pluginStarted, "the plugin unloads cleanly with those bindings missing");

	// A partial admin_system export set must not crash: the command gate answers "no permission".
	PrepareTest();
	__admin_system_HasPermission = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing HasPermission fails open (no menu)");
	StopPlugin();

	// A host that cannot answer the SourceTV question is not treated as having players.
	PrepareTest();
	__s2sdk_IsClientSourceTV = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing IsClientSourceTV fails closed (no players)");
	StopPlugin();

	// A host without the menu cleanup bindings gets no menus at all.
	PrepareTest();
	__s2sdk_GetClientMenu = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing GetClientMenu disables menus");
	StopPlugin();

	PrepareTest();
	__s2sdk_CancelClientMenu = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing CancelClientMenu disables menus");
	StopPlugin();

	PrepareTest();
	__s2sdk_SetMenuType = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing SetMenuType disables menus (the no-op-close fail-safe needs it)");
	StopPlugin();

	PrepareTest();
	__s2sdk_GetMenuType = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing GetMenuType disables menus");
	StopPlugin();

	// A missing controller-entity binding removes the menu path completely (fail closed).
	PrepareTest();
	__s2sdk_PlayerSlotToEntHandle = nullptr;
	StartGame();
	SetUpFourHumans();
	InvokeCommand("votekick", 0, s2sdk::ConCommandContext::Chat);
	Check(LatestMenuForSlot(0) == nullptr, "a missing PlayerSlotToEntHandle disables menus");
	StopPlugin();

	// A missing punishment binding must not turn into a crash at the threshold.
	PrepareTest();
	StartGame();
	SetUpFourHumans();
	ChooseBanTarget(0, kTargetSteam);
	CastYes(2);
	__admin_system_AddPlayerPunishment = nullptr;
	CastYes(0);
	CheckEqInt(static_cast<int64_t>(g_punishments.size()), 0, "a missing AddPlayerPunishment means no punishment");
	Check(g_pluginStarted, "the plugin survived the missing punishment binding");
	StopPlugin();
}

// ==========================================================================================
// main
// ==========================================================================================

} // namespace

int main() {
	std::printf("as_votebkm_tests\n");
	SetUpDirs();
	TestStartupAndCommands();
	TestCommandOwnership();
	TestBareSayCommands();
	TestMainVoteFlow();
	TestResultTimerAccuracy();
	TestDelayedResultReusedSlot();
	TestDelayedResultMissedReuse();
	TestVoterSlotReuse();
	TestTargetDisconnect();
	TestRightsLost();
	TestInitiatorPermissionLost();
	TestAdminDataReadiness();
	TestVoteAuthorLost();
	TestBots();
	TestMapChange();
	TestMapGateMidMapLoad();
	TestMapGateMapEndBeforeFirstUpdate();
	TestUnloadAndReload();
	TestTypeZero();
	TestKickAndPriority();
	TestGagAndNotEnoughPlayers();
	TestKickRevalidationAfterSendAction();
	TestMenuEdges();
	TestNoTargets();
	TestSignedEventSlot();
	TestMenuTypeChoice();
	TestMapDownCancelOrder();
	TestUnloadWithMapDownCancel();
	TestMapEndCancelRefused();
	TestMapEndCancelRefusedUnresolved();
	TestMapEndParkedRetry();
	TestStaleMenuAfterSlotReuse();
	TestStaleYesActionChat();
	TestConfigPrecedence();
	TestPhraseFallbackAndServerLang();
	TestColorizedChat();
	TestCleanupBindingPairs();
	TestFailSoftStart();
	StopPlugin();
	TearDownDirs();
	std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
