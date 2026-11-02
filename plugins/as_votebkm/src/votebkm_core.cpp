#include "votebkm_core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>

namespace votebkm {
namespace {

// cs2-menus/utils.h: colors_text[] / colors_hex[], same order, same bytes. The last entry is the
// chat line break: the literal two characters "\n" -> U+2029 (e2 80 a9), which the CS2 chat
// renderer shows as a new line. This is byte for byte pisexcompat's table.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
												  {"{WHITE}", "\x01"},
												  {"{RED}", "\x02"},
												  {"{LIGHTPURPLE}", "\x03"},
												  {"{GREEN}", "\x04"},
												  {"{LIME}", "\x05"},
												  {"{LIGHTGREEN}", "\x06"},
												  {"{DARKRED}", "\x07"},
												  {"{GRAY}", "\x08"},
												  {"{LIGHTOLIVE}", "\x09"},
												  {"{OLIVE}", "\x10"},
												  {"{LIGHTBLUE}", "\x0B"},
												  {"{BLUE}", "\x0C"},
												  {"{PURPLE}", "\x0E"},
												  {"{LIGHTRED}", "\x0F"},
												  {"{GRAYBLUE}", "\x0A"},
												  {"\\n", "\xe2\x80\xa9"}}};

void PushUnique(std::vector<std::string>& values, std::string value) {
	if (value.empty()) {
		return;
	}
	if (std::find(values.begin(), values.end(), value) == values.end()) {
		values.push_back(std::move(value));
	}
}

} // namespace

bool IsWhitespace(char c) {
	return std::isspace(static_cast<unsigned char>(c)) != 0;
}

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string ToLower(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text) {
		out.push_back(Lower(c));
	}
	return out;
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

std::string Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && IsWhitespace(text[begin])) {
		++begin;
	}
	while (end > begin && IsWhitespace(text[end - 1])) {
		--end;
	}
	return std::string(text.substr(begin, end - begin));
}

std::string JoinPath(std::string_view dir, std::string_view file) {
	if (dir.empty()) {
		return std::string(file);
	}
	if (file.empty()) {
		return std::string(dir);
	}
	std::string out(dir);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (file.front() == '/') {
		file.remove_prefix(1);
	}
	out.append(file);
	return out;
}

Settings ParseSettings(const KvNode& root) {
	Settings settings;
	settings.immunityFlag = std::string(root.GetString("immunity_flag", settings.immunityFlag));
	settings.disableFlag = std::string(root.GetString("disable_flag", settings.disableFlag));
	settings.immunityLimit = root.GetInt("immunity_limit", settings.immunityLimit);
	settings.percents = root.GetInt("percents", settings.percents);
	settings.minPlayers = root.GetInt("min_players", settings.minPlayers);
	settings.timePunish = root.GetInt("time_punish", settings.timePunish);
	settings.cooldown = root.GetInt("cooldown", settings.cooldown);
	settings.typeVote = root.GetInt("type_vote", settings.typeVote);
	settings.resultTimer = root.GetInt("result_timer", settings.resultTimer);
	settings.reason = std::string(root.GetString("reason", settings.reason));
	settings.commands = ParseCommandSpecs(root.GetString("commands", ""));
	return settings;
}

std::vector<CommandSpec> ParseCommandSpecs(std::string_view commands) {
	std::vector<CommandSpec> specs;
	std::size_t start = 0;
	while (true) {
		const std::size_t end = commands.find('|', start);
		const std::string token =
			Trim(commands.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
		if (!token.empty()) {
			CommandSpec spec;
			spec.spelling = token;
			std::size_t trigger = 0;
			if (token.front() == '!' || token.front() == '/') {
				trigger = 1;
				spec.bare = false;
			} else {
				spec.bare = true;
			}
			if (trigger < token.size()) {
				spec.lookup = ToLower(std::string_view(token).substr(trigger));
			}
			if (!spec.lookup.empty()) {
				specs.push_back(std::move(spec));
			}
		}
		if (end == std::string_view::npos) {
			break;
		}
		start = end + 1;
	}
	return specs;
}

std::vector<std::string> ConfigCandidates(std::string_view configsRoot, std::string_view dataRoot) {
	std::vector<std::string> candidates;
	// pisexcompat's Candidates(): the path as given (relative to the server working directory)
	// always comes first.
	candidates.emplace_back(kRawConfigPath);
	const std::string configDir =
		configsRoot.empty() ? std::string(kPluginConfigDir) : JoinPath(configsRoot, kPluginConfigDir);
	const std::string translationsDir = JoinPath(configDir, "translations");
	const std::string dataDir = dataRoot.empty() ? std::string(kPluginConfigDir) : JoinPath(dataRoot, kPluginConfigDir);
	PushUnique(candidates, JoinPath(configDir, kConfigFile));
	PushUnique(candidates, JoinPath(translationsDir, kConfigFile));
	PushUnique(candidates, JoinPath(dataDir, kConfigFile));
	return candidates;
}

std::vector<std::string> PhraseCandidates(std::string_view configsRoot) {
	std::vector<std::string> candidates;
	// pisexcompat's ResolveTranslationsPath("admin_system.phrases.txt") tried the raw cwd file
	// first, then <configs>/admin_system/translations/<file>, then <configs>/admin_system/<file>.
	candidates.emplace_back(kPhraseFile);
	const std::string configDir =
		configsRoot.empty() ? std::string(kPluginConfigDir) : JoinPath(configsRoot, kPluginConfigDir);
	const std::string translationsDir = JoinPath(configDir, "translations");
	PushUnique(candidates, JoinPath(translationsDir, kPhraseFile));
	PushUnique(candidates, JoinPath(configDir, kPhraseFile));
	return candidates;
}

int NeededVotes(int players, int percents) {
	return static_cast<int>(std::ceil(static_cast<float>(players) * (static_cast<float>(percents) / 100.0f)));
}

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

std::string Format(const char* format, ...) {
	if (format == nullptr) {
		return {};
	}
	std::array<char, 4096> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	buffer.back() = '\0';
	return std::string(buffer.data());
}

std::string NormalizeLanguage(std::string_view name) {
	const std::string lower = ToLower(name);
	// Source language names -> the codes used by the Pisex phrase files (pisexcompat's
	// NormalizeLanguage); an unknown value is kept as written.
	if (lower == "russian" || lower == "ru") {
		return "ru";
	}
	if (lower == "english" || lower == "en") {
		return "en";
	}
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") {
		return "ua";
	}
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") {
		return "pt-BR";
	}
	if (lower == "spanish" || lower == "es") {
		return "es";
	}
	if (lower == "german" || lower == "de") {
		return "de";
	}
	if (lower == "french" || lower == "fr") {
		return "fr";
	}
	if (lower == "polish" || lower == "pl") {
		return "pl";
	}
	if (lower == "turkish" || lower == "tr") {
		return "tr";
	}
	if (lower == "schinese" || lower == "zh-cn") {
		return "zh-CN";
	}
	return std::string(name);
}

std::string PickLanguage(std::string_view coreLanguage, std::string_view serverLanguage) {
	if (!coreLanguage.empty()) {
		return NormalizeLanguage(coreLanguage);
	}
	if (!serverLanguage.empty()) {
		return NormalizeLanguage(serverLanguage);
	}
	return "en";
}

PhraseMap ResolvePhrases(const KvNode& root, std::string_view language) {
	constexpr std::string_view kFallback = "en";
	PhraseMap phrases;
	for (const KvNode& key : root.children) {
		if (key.children.empty()) {
			continue; // a plain value ("language" "ru"), not a phrase
		}
		std::string text;
		const KvNode* localized = key.Find(language);
		if (localized != nullptr && localized->hasValue) {
			text = localized->value;
		}
		if (text.empty() && !EqualNoCase(language, kFallback)) {
			const KvNode* english = key.Find(kFallback);
			if (english != nullptr && english->hasValue) {
				text = english->value;
			}
		}
		if (text.empty()) {
			// Last resort: any language the file has, so a partially translated file still shows
			// something instead of an empty chat line (pisexcompat's LoadPhrasesFile).
			for (const KvNode& candidate : key.children) {
				if (candidate.hasValue && !candidate.value.empty()) {
					text = candidate.value;
					break;
				}
			}
		}
		if (!text.empty()) {
			phrases[key.name] = std::move(text);
		}
	}
	return phrases;
}

} // namespace votebkm
