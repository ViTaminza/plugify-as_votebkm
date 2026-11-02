// as_votebkm core tests — no engine, no s2sdk, no Plugify runtime. They exercise the code the
// plugin shares with its own config/phrase loader:
//   * the KV1 grammar (comments, BOM, escapes, #base, malformed files);
//   * votebkm.ini: the production file, the module default and the original defaults;
//   * the "commands" spelling rules (!// triggers, bare names, empty tokens);
//   * the ceil(players * percents / 100) arithmetic;
//   * the pisexcompat path order (raw cwd first) for votebkm.ini and admin_system.phrases.txt;
//   * the colour table and printf formatting;
//   * the <configs>/core.cfg ServerLang -> s2sdk -> "en" chain and the phrase fallback.
//
// Run by ./test.sh (build/tests/as_votebkm_core_tests).

#include "../src/votebkm_core.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
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

std::string ReadFixture(const char* name) {
	const std::string path = std::string(AS_VOTEBKM_TEST_DATA_DIR) + "/" + name;
	const std::optional<std::string> text = votebkm::ReadTextFile(path);
	Check(text.has_value(), std::string("fixture is readable: ") + path);
	return text.value_or(std::string());
}

votebkm::KvNode ParseOrDie(std::string_view text, const std::string& what) {
	const std::optional<votebkm::KvNode> root = votebkm::ParseKv1(text);
	Check(root.has_value(), "KV1 parses: " + what);
	return root.value_or(votebkm::KvNode{});
}

std::vector<std::string> Lookups(const std::vector<votebkm::CommandSpec>& specs) {
	std::vector<std::string> out;
	for (const votebkm::CommandSpec& spec : specs) {
		out.push_back(spec.lookup);
	}
	return out;
}

// --- KV1 ---------------------------------------------------------------------------------------

void TestKv1() {
	Section("KV1 grammar used by the production files");
	const votebkm::KvNode root = ParseOrDie(R"(
"VoteBKM"
{
    // a line comment
    "immunity_limit" "10"
    "commands" "!votekick|!voteban"  /* a block comment */
    "nested" { "en" "text with \"quotes\"" }
    #base "included.cfg"
    "unquoted" value
}
)",
											 "nested block, comments, unquoted value");
	CheckEqStr(root.GetString("immunity_limit", ""), "10", "quoted value inside the root block");
	CheckEqStr(root.GetString("commands", ""), "!votekick|!voteban", "commands value");
	CheckEqStr(root.GetString("UNQUOTED", ""), "value", "key lookup is case-insensitive");
	const votebkm::KvNode* nested = root.Find("nested");
	Check(nested != nullptr, "nested block exists");
	CheckEqStr(nested != nullptr ? nested->GetString("en", "") : "", "text with \"quotes\"", "escaped quotes");
	CheckEqInt(root.GetInt("missing", 7), 7, "GetInt default");
	CheckEqInt(root.GetInt("immunity_limit", 0), 10, "GetInt reads the value");
	CheckEqInt(ParseOrDie("\"Root\" { \"x\" \"abc\" }", "non-numeric").GetInt("x", 5), 0,
			   "non-numeric value keeps Valve's atoi semantics");

	const std::string withBom = std::string("\xEF\xBB\xBF") + "\"Root\" { \"a\" \"1\" }";
	Check(ParseOrDie(withBom, "BOM").GetString("a", "") == "1", "a leading UTF-8 BOM is skipped");

	const std::optional<votebkm::KvNode> missingBrace = votebkm::ParseKv1("\"Root\" { \"a\" \"1\"");
	Check(!missingBrace.has_value(), "a missing '}' makes the file invalid");
	const std::optional<votebkm::KvNode> strayBrace = votebkm::ParseKv1("\"a\" \"1\" }");
	Check(!strayBrace.has_value(), "a stray top-level '}' makes the file invalid");
	const std::optional<votebkm::KvNode> unterminated = votebkm::ParseKv1("\"Root\" { \"a\" \"1 }");
	Check(!unterminated.has_value(), "an unterminated string makes the file invalid");
	Check(!votebkm::ReadTextFile("/nonexistent/as_votebkm_test.ini").has_value(), "missing file -> nullopt");
}

// --- settings ----------------------------------------------------------------------------------

void TestSettings() {
	Section("votebkm.ini: production/module values and the original defaults");
	const votebkm::KvNode production = ParseOrDie(ReadFixture("votebkm_prod.ini"), "the production votebkm.ini");
	const votebkm::Settings prod = votebkm::ParseSettings(production);
	CheckEqStr(prod.immunityFlag, "@admin/votebkm_i", "production immunity_flag");
	CheckEqStr(prod.disableFlag, "@admin/spec", "production disable_flag");
	CheckEqInt(prod.immunityLimit, 10, "production immunity_limit");
	CheckEqInt(prod.percents, 70, "production percents");
	CheckEqInt(prod.minPlayers, 4, "production min_players");
	CheckEqInt(prod.timePunish, 3600, "production time_punish");
	CheckEqInt(prod.cooldown, 60, "production cooldown");
	CheckEqInt(prod.typeVote, 1, "production type_vote");
	CheckEqInt(prod.resultTimer, 30, "production result_timer");
	CheckEqStr(prod.reason, "Голосование игроков", "production reason (UTF-8 untouched)");
	CheckEqInt(static_cast<int64_t>(prod.commands.size()), 6, "production command count");
	if (!prod.commands.empty()) {
		CheckEqStr(prod.commands.front().lookup, "votekick", "production first command lookup");
		CheckEqStr(prod.commands.back().lookup, "vo", "production last command lookup (!vo)");
	}

	const votebkm::Settings module = votebkm::ParseSettings(ParseOrDie(ReadFixture("votebkm_module.ini"), "module config"));
	CheckEqInt(module.percents, 60, "module percents");
	CheckEqStr(module.disableFlag, "@admin/votebkm_d", "module disable_flag");
	CheckEqInt(static_cast<int64_t>(module.commands.size()), 5, "module command count");
	CheckEqStr(module.reason, "Голосованием VoteBKM", "module reason");

	const votebkm::Settings defaults = votebkm::ParseSettings(ParseOrDie("\"VoteBKM\" { }", "empty settings"));
	CheckEqStr(defaults.immunityFlag, "@admin/votebkm_i", "default immunity_flag");
	CheckEqStr(defaults.disableFlag, "@admin/votebkm_d", "default disable_flag");
	CheckEqInt(defaults.immunityLimit, 10, "default immunity_limit");
	CheckEqInt(defaults.percents, 40, "default percents");
	CheckEqInt(defaults.minPlayers, 4, "default min_players");
	CheckEqInt(defaults.timePunish, 3600, "default time_punish");
	CheckEqInt(defaults.cooldown, 300, "default cooldown");
	CheckEqInt(defaults.typeVote, 0, "default type_vote");
	CheckEqInt(defaults.resultTimer, 30, "default result_timer");
	CheckEqStr(defaults.reason, "VoteBKM", "default reason");
	Check(defaults.commands.empty(), "no commands in an empty config");

	// A present-but-empty value is kept, not replaced by the default (the original's disable_flag
	// "leave it empty to disable the feature" contract).
	const votebkm::Settings emptyFlag =
		votebkm::ParseSettings(ParseOrDie("\"VoteBKM\" { \"disable_flag\" \"\" }", "empty disable_flag"));
	Check(emptyFlag.disableFlag.empty(), "an empty disable_flag stays empty");
}

void TestCommands() {
	Section("the 'commands' spelling rules of pisexcompat");
	const std::vector<votebkm::CommandSpec> specs =
		votebkm::ParseCommandSpecs(" !votekick | /vote | votegag | | ! | !vo ");
	const std::vector<std::string> lookups = Lookups(specs);
	CheckEqInt(static_cast<int64_t>(lookups.size()), 4, "empty and trigger-only tokens are skipped");
	if (lookups.size() == 4) {
		CheckEqStr(lookups[0], "votekick", "! spelling");
		CheckEqStr(lookups[1], "vote", "/ spelling");
		CheckEqStr(lookups[2], "votegag", "bare spelling");
		CheckEqStr(lookups[3], "vo", "!vo spelling");
		Check(!specs[0].bare && !specs[1].bare, "triggered spellings are not bare");
		Check(specs[2].bare, "a spelling without a trigger is bare");
		CheckEqStr(specs[0].spelling, "!votekick", "the spelling keeps the trigger for the record");
	}
	Check(votebkm::ParseCommandSpecs("").empty(), "empty commands -> no specs");
	Check(votebkm::ParseCommandSpecs("||").empty(), "only separators -> no specs");
}

void TestVoteMath() {
	Section("ceil players * percents / 100");
	CheckEqInt(votebkm::NeededVotes(4, 40), 2, "4 players at 40%");
	CheckEqInt(votebkm::NeededVotes(4, 70), 3, "4 players at 70% (ceil 2.8)");
	CheckEqInt(votebkm::NeededVotes(3, 50), 2, "3 players at 50% (ceil 1.5)");
	CheckEqInt(votebkm::NeededVotes(5, 50), 3, "5 players at 50% (ceil 2.5)");
	CheckEqInt(votebkm::NeededVotes(1, 50), 1, "1 player at 50%");
	CheckEqInt(votebkm::NeededVotes(0, 50), 0, "0 players");
	CheckEqInt(votebkm::NeededVotes(4, 0), 0, "0 percent");
	CheckEqInt(votebkm::NeededVotes(4, 100), 4, "100 percent");
}

// --- paths -------------------------------------------------------------------------------------

void TestPaths() {
	Section("the raw cwd first path order of pisexcompat");
	const std::vector<std::string> config =
		votebkm::ConfigCandidates("/cfg", "/data");
	const std::vector<std::string> expected{"addons/configs/admin_system/votebkm.ini",
											"/cfg/admin_system/votebkm.ini",
											"/cfg/admin_system/translations/votebkm.ini",
											"/data/admin_system/votebkm.ini"};
	Check(config == expected, "config candidates: raw cwd, config dir, translations, data dir");

	const std::vector<std::string> noRoots = votebkm::ConfigCandidates("", "");
	const std::vector<std::string> noRootsExpected{"addons/configs/admin_system/votebkm.ini",
												   "admin_system/votebkm.ini",
												   "admin_system/translations/votebkm.ini"};
	Check(noRoots == noRootsExpected, "config candidates without a host config dir keep pisex's relative spelling");

	const std::vector<std::string> phrases = votebkm::PhraseCandidates("/cfg");
	const std::vector<std::string> phrasesExpected{"admin_system.phrases.txt",
												   "/cfg/admin_system/translations/admin_system.phrases.txt",
												   "/cfg/admin_system/admin_system.phrases.txt"};
	Check(phrases == phrasesExpected, "phrase candidates: raw cwd, translations, config dir");
	Check(votebkm::JoinPath("", "x") == "x", "JoinPath with no dir");
	Check(votebkm::JoinPath("/a/", "/x") == "/a/x", "JoinPath normalises the separator");
}

// --- localisation ------------------------------------------------------------------------------

void TestLanguage() {
	Section("<configs>/core.cfg ServerLang -> s2sdk -> en");
	CheckEqStr(votebkm::PickLanguage("russian", "en"), "ru", "core.cfg name wins");
	CheckEqStr(votebkm::PickLanguage("RU", "ua"), "ru", "core.cfg code is normalised");
	CheckEqStr(votebkm::PickLanguage("", "ukrainian"), "ua", "s2sdk language is the second choice");
	CheckEqStr(votebkm::PickLanguage("", "en"), "en", "s2sdk English");
	CheckEqStr(votebkm::PickLanguage("", ""), "en", "the 'en' fallback");
	CheckEqStr(votebkm::NormalizeLanguage("russian"), "ru", "normalize russian");
	CheckEqStr(votebkm::NormalizeLanguage("english"), "en", "normalize english");
	CheckEqStr(votebkm::NormalizeLanguage("uk"), "ua", "normalize uk");
	CheckEqStr(votebkm::NormalizeLanguage("pt-br"), "pt-BR", "normalize pt-br");
	CheckEqStr(votebkm::NormalizeLanguage("schinese"), "zh-CN", "normalize schinese");
	CheckEqStr(votebkm::NormalizeLanguage("klingon"), "klingon", "an unknown language is kept");
}

void TestPhrases() {
	Section("admin_system.phrases.txt resolution");
	const votebkm::KvNode root = ParseOrDie(ReadFixture("admin_system.phrases.txt"), "the phrase fixture");
	const votebkm::PhraseMap ru = votebkm::ResolvePhrases(root, "ru");
	const votebkm::PhraseMap en = votebkm::ResolvePhrases(root, "en");
	Check(ru.size() >= 30, "the fixture resolves every phrase");
	CheckEqStr(ru.at("VoteBKM_Title"), "Выберите действие", "Russian phrase");
	CheckEqStr(en.at("VoteBKM_Title"), "Select action", "English phrase");
	Check(ru.count("language") == 0, "plain value nodes are skipped");
	CheckEqStr(ru.at("VoteBKM_Success"), "Голосование завершено. Игрок {LIGHTRED}%s{DEFAULT} наказан",
			   "colour tags and %s are kept for Colorize/Format");

	const votebkm::KvNode fallback = ParseOrDie(
		"\"Phrases\" { \"K1\" { \"en\" \"English\" } \"K2\" { \"de\" \"Deutsch\" } \"K3\" { \"ru\" \"\" } }",
		"language fallback file");
	const votebkm::PhraseMap ruFallback = votebkm::ResolvePhrases(fallback, "ru");
	CheckEqStr(ruFallback.at("K1"), "English", "a missing language falls back to English");
	CheckEqStr(ruFallback.at("K2"), "Deutsch", "a missing English falls back to the first language with text");
	Check(ruFallback.count("K3") == 0, "a phrase with no text at all is not stored");
}

void TestFormatting() {
	Section("the colour table and printf formatting");
	const std::string colored = votebkm::Colorize("{LIGHTRED}x{DEFAULT}\\n");
	const std::string expected = std::string("\x0F") + "x" + "\x01" + "\xe2\x80\xa9";
	CheckEqStr(colored, expected, "LIGHTRED/DEFAULT and the chat line break");
	CheckEqStr(votebkm::Colorize("plain"), "plain", "text without tags is unchanged");
	CheckEqStr(votebkm::Format("%s/%i/%s", "a", 7, "b"), "a/7/b", "printf formatting");
	CheckEqStr(votebkm::Format("%s", "100%"), "100%", "a percent in an argument is not interpreted");
	CheckEqStr(votebkm::Trim("  x \t"), "x", "Trim");
	CheckEqStr(votebkm::ToLower("VoTe"), "vote", "ToLower");
	Check(votebkm::EqualNoCase("Ru", "rU"), "EqualNoCase");
}

} // namespace

int main() {
	std::printf("as_votebkm_core_tests\n");
	TestKv1();
	TestSettings();
	TestCommands();
	TestVoteMath();
	TestPaths();
	TestLanguage();
	TestPhrases();
	TestFormatting();
	std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
