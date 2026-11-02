// as_votebkm — the engine-free half of the plugin: configuration, command spellings, path
// resolution, the vote arithmetic, the chat colour table and the phrase table.
//
// Everything here is pure C++ (no s2sdk, no admin_system, no Plugify): src/plugin.cpp drives it
// and tests/as_votebkm_core_tests.cpp checks it directly. The plugin is self-contained (it is
// published on its own), so the KeyValues reader it needs lives in src/kv1.hpp, exactly like the
// other ports in this tree.
//
// The behaviour mirrors the original Metamod module "[AS] VoteBKM" and the pisexcompat port that
// replaced it:
//   * votebkm.ini keeps every key and default of the original;
//   * "commands" is a '|' separated list of chat spellings ("!votekick"), one leading '!' or '/'
//     trigger removed for the engine command name;
//   * a punishment needs ceil(players * percents / 100) votes, and the vote types use the RT_*
//     order of the admin core (ban=0, mute=1, gag=2, kick=3);
//   * phrases are the server's admin_system.phrases.txt entries; this plugin only resolves the
//     file as a fallback when the admin_system API cannot translate a key (see plugin.cpp).

#pragma once

#include "kv1.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace votebkm {

// The order of the original's g_iTarget[slot][type] / the admin core's RT_* constants.
inline constexpr int32_t kBan = 0;
inline constexpr int32_t kMute = 1;
inline constexpr int32_t kGag = 2;
inline constexpr int32_t kKick = 3;
inline constexpr int32_t kTypes = 4;

// The plugin reads and writes exactly the file the original and the pisexcompat port used.
inline constexpr char kPluginConfigDir[] = "admin_system";
inline constexpr char kConfigFile[] = "votebkm.ini";
inline constexpr char kRawConfigPath[] = "addons/configs/admin_system/votebkm.ini";
inline constexpr char kPhraseFile[] = "admin_system.phrases.txt";
inline constexpr char kCoreConfigFile[] = "core.cfg";

struct CommandSpec {
	std::string spelling; // as written in votebkm.ini, including the ! or / trigger
	std::string lookup;   // lowercase engine command name (one leading !! or // trigger removed)
	bool bare = false;    // no trigger: pisexcompat also dispatched it through say/say_team
};

struct Settings {
	std::string immunityFlag = "@admin/votebkm_i";
	std::string disableFlag = "@admin/votebkm_d";
	int immunityLimit = 10;
	int percents = 40; // the original's integer percentage, divided by 100 when needed
	int minPlayers = 4;
	int timePunish = 3600;
	int cooldown = 300;
	int typeVote = 0;
	int resultTimer = 30;
	std::string reason = "VoteBKM";
	std::vector<CommandSpec> commands;
};

// --- small helpers -----------------------------------------------------------------------------

bool IsWhitespace(char c);
std::string Trim(std::string_view text);
std::string ToLower(std::string_view text);
char Lower(char c);
bool EqualNoCase(std::string_view a, std::string_view b);

std::string JoinPath(std::string_view dir, std::string_view file);

// --- configuration -----------------------------------------------------------------------------

// The keys of votebkm.ini with the original's defaults. Missing keys keep the default; a missing
// or malformed file is the caller's problem (it keeps Settings{} exactly like the original kept
// its globals).
Settings ParseSettings(const KvNode& root);

// "a|b|c" -> trimmed non-empty tokens; a token is skipped when removing its trigger leaves an
// empty name (the original's RegCommand skipped empty names too).
std::vector<CommandSpec> ParseCommandSpecs(std::string_view commands);

// pisexcompat's Candidates() for "addons/configs/admin_system/votebkm.ini" with plugin name
// "admin_system": the raw cwd spelling first, then the plugin config dir, its translations dir and
// the plugin data dir. The first candidate that exists is loaded, so a server that still keeps the
// Metamod file next to the game binary keeps using exactly that file.
std::vector<std::string> ConfigCandidates(std::string_view configsRoot, std::string_view dataRoot);

// pisexcompat's ResolveTranslationsPath() for "admin_system.phrases.txt": the raw cwd file first,
// then <configs>/admin_system/translations, then <configs>/admin_system. This is only the fallback
// used when the admin_system API has no translated value.
std::vector<std::string> PhraseCandidates(std::string_view configsRoot);

// --- vote arithmetic ---------------------------------------------------------------------------

// ceil(players * percents / 100), the original's float math. Callers handle the < 0 / 0 cases.
int NeededVotes(int players, int percents);

// --- localisation ------------------------------------------------------------------------------

// cs2-menus' colour tags -> the chat control bytes pisexcompat sent. "\\n" becomes U+2029, the
// CS2 chat line break.
std::string Colorize(std::string_view text);

// A printf-style format used for the phrases that carry %s/%i. The phrase itself is trusted data;
// player names are arguments, so a '%' in a name is not interpreted.
std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));

// "russian"/"ru" -> "ru", "ukrainian"/"uk" -> "ua", ... Unknown values are returned unchanged
// (pisexcompat's NormalizeLanguage).
std::string NormalizeLanguage(std::string_view name);

// <configs>/core.cfg "ServerLang" first, then s2sdk's ServerLanguage, then "en" (both normalized).
std::string PickLanguage(std::string_view coreLanguage, std::string_view serverLanguage);

using PhraseMap = std::map<std::string, std::string>;

// "Key" { "ru" "..." "en" "..." } -> value for `language`, then "en", then the first language that
// has text. Plain value nodes (not phrase blocks) are skipped.
PhraseMap ResolvePhrases(const KvNode& root, std::string_view language);

} // namespace votebkm
