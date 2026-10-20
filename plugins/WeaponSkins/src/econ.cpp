// WeaponSkins (C++ Plugify port) — econ data: scripts/items/items_game.txt and resource/csgo_*.txt.
//
// Implements the EconData interface from econ.hpp. Everything is parsed from the engine's own data
// files; no engine schema call happens here. The only engine touchpoints are the two filesystem
// bindings used by LoadFromGame (s2sdk::ReadFileVPK / FindFileAbsoluteList), and they are null-
// checked, so a process without the s2sdk plugin fails LoadFromGame with an error string instead
// of crashing.
//
// What and why (the C# reference is plugins_dotnet/WeaponSkins/src/Econ/EconService.cs):
//   * items_game is KV1, ~10 MB in the real game. It is parsed with pisex::KeyValues (the
//     pisexcompat parser) into a temporary tree that is destroyed when Parse() returns, so the
//     whole-file tree never outlives the load.
//   * "items" children are keyed by def_index; only weapons, knives and gloves are kept: an entry
//     is one when `baseitem` is present anywhere on its prefab chain, or the chain contains the
//     `melee_unusual` (knives) or `hands_paintable` (gloves) prefab — the same filter the C#
//     reference applies, extended to the chain so a prefab can carry the marker.
//   * prefab chains are walked recursively (`prefab`, and all values of a `prefabs` block, so the
//     plural form works too). The walk is bounded and cycle-safe: a `visited` set breaks
//     prefab cycles, kMaxPrefabSteps caps the work. Own fields win over nearer prefabs, nearer
//     over farther. This yields the classname (`name`), `model_player` (ItemDefinition::model) and
//     `used_by_classes` (ItemDefinition::team: 2 = T only, 3 = CT only, 0 = both/unrestricted;
//     the installed data spells the sides "terrorists"/"counter-terrorists", the short "t"/"ct"
//     spelling is accepted too).
//   * paint kits: `legacy` is the kit's own direct `use_legacy_model` == "1" (the C# reference
//     asks HasSubKeyWithValue("use_legacy_model", "1") on the kit itself; it is not inherited).
//   * agents: items whose prefab chain contains `customplayertradable`. The model is the first
//     value anywhere in the item subtree, then in the prefab chain, that looks like an agent model
//     ("/tm_", "/ctm_" — after normalising '\' to '/'); it is cut to "characters/models/…" and gets
//     a ".vmdl" extension if missing, and `name` is that path without the "characters/models/"
//     prefix and the extension (the C# GetLocalizedNames/ParseAgents shape).
//   * sticker kits / keychain definitions / music definitions are stored by their numeric id;
//     `name` is the record's own "name" field when it has one, else the block key (the installed
//     data has both shapes). Localized names come from the "#token" fields (item_name / loc_name /
//     description_tag) with the leading '#' stripped. Token lookup is case-insensitive, like the
//     C# dictionaries; a token that maps to an empty string is skipped.
//   * loot lists: `loot_lists` entries are copied verbatim; `client_loot_lists` entries are
//     expanded — a child whose name is another loot list is replaced by that list's entries
//     (recursively, cycle-safe) and "[item]belonging" children are reduced to `item`, which is
//     what the C# ParseClientLootLists does (it only expands one level; this port resolves the
//     full chain with the same guard, and never loops).
//
// Languages: localized names are computed while items_game is parsed, from the languages parsed so
// far — parse languages first, then items (LoadFromGame does exactly that; the C# reference calls
// ParseLanguages() before ParseWeapons() as well). `english` is used as the fallback source, and
// LoadFromGame always keeps the english file even when ItemLanguages selects a subset, because the
// fallback needs it. When a language is missing a token, the primary language's value is used
// (first requested language, or english when no list is configured), else the english one — the
// C# fallback order.
//
// Atomicity: Parse / ParseLanguage / LoadFromGame build their result into local maps and commit
// with moves only on success, so a malformed items_game (or a truncated download) leaves the last
// known-good snapshot completely untouched.
//
// Encoding: items_game and csgo_*.txt are read as raw bytes through ReadFileVPK. UTF-8 BOM is
// stripped; UTF-16 (LE or BE, BOM-marked) is decoded to UTF-8 first, because Valve ships some
// language files in UTF-16.

#include "econ.hpp"

#include <pisex/keyvalues.hpp>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ws {
namespace {

constexpr int kMaxPrefabSteps = 64; // bound for prefab-chain walks (cycles are broken by `visited`)
constexpr int kMaxAgentModelDepth = 16;
constexpr int kMaxLootDepth = 64;

using Kv = pisex::KeyValues;
using TokenMap = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

// --- small string helpers ---------------------------------------------------------------------

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
			return false;
		}
	}
	return true;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
	if (text.size() < prefix.size()) {
		return false;
	}
	return EqualNoCase(text.substr(0, prefix.size()), prefix);
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
	if (text.size() < suffix.size()) {
		return false;
	}
	return EqualNoCase(text.substr(text.size() - suffix.size()), suffix);
}

bool ContainsNoCase(std::string_view text, std::string_view needle) {
	if (needle.empty() || text.size() < needle.size()) {
		return needle.empty();
	}
	for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
		if (EqualNoCase(text.substr(i, needle.size()), needle)) {
			return true;
		}
	}
	return false;
}

std::size_t IndexOfNoCase(std::string_view text, std::string_view needle) {
	if (needle.empty() || text.size() < needle.size()) {
		return std::string_view::npos;
	}
	for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
		if (EqualNoCase(text.substr(i, needle.size()), needle)) {
			return i;
		}
	}
	return std::string_view::npos;
}

std::string Lower(std::string_view text) {
	std::string out(text);
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// --- encoding ---------------------------------------------------------------------------------

void AppendUtf8(std::uint32_t codePoint, std::string& out) {
	if (codePoint <= 0x7F) {
		out.push_back(static_cast<char>(codePoint));
	} else if (codePoint <= 0x7FF) {
		out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
		out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
	} else if (codePoint <= 0xFFFF) {
		out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
		out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
	} else {
		out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
		out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
	}
}

std::string DecodeUtf16(std::string_view bytes, bool bigEndian) {
	std::string out;
	out.reserve(bytes.size());
	for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
		const auto unit = static_cast<std::uint16_t>(
			bigEndian ? (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[i])) << 8) |
						static_cast<unsigned char>(bytes[i + 1])
					: (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
						  static_cast<unsigned char>(bytes[i]));
		std::uint32_t codePoint = unit;
		if (unit >= 0xD800 && unit <= 0xDBFF) {
			// Surrogate pair; an unpaired high surrogate becomes U+FFFD.
			codePoint = 0xFFFD;
			if (i + 3 < bytes.size()) {
				const auto low = static_cast<std::uint16_t>(
					bigEndian ? (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[i + 2])) << 8) |
									static_cast<unsigned char>(bytes[i + 3])
							  : (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[i + 3])) << 8) |
									static_cast<unsigned char>(bytes[i + 2]));
				if (low >= 0xDC00 && low <= 0xDFFF) {
					codePoint = 0x10000 + ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
								(static_cast<std::uint32_t>(low) - 0xDC00);
					i += 2;
				}
			}
		} else if (unit >= 0xDC00 && unit <= 0xDFFF) {
			codePoint = 0xFFFD; // unpaired low surrogate
		}
		AppendUtf8(codePoint, out);
	}
	return out;
}

// Raw bytes from ReadFileVPK -> UTF-8 text. BOMs: EF BB BF (UTF-8), FF FE / FE FF (UTF-16).
void DecodeText(std::string_view raw, std::string& out) {
	if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
		out = DecodeUtf16(raw.substr(2), false);
		return;
	}
	if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE && static_cast<unsigned char>(raw[1]) == 0xFF) {
		out = DecodeUtf16(raw.substr(2), true);
		return;
	}
	std::string_view text = raw;
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
		static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		text.remove_prefix(3);
	}
	out.assign(text);
}

// --- language names ---------------------------------------------------------------------------

// The C# reference's LanguageCodeToTranslationKey (EconService.cs:267-299): config codes -> the
// key Valve uses in resource/csgo_<key>.txt. Ported verbatim; keys are lower-case here.
const std::unordered_map<std::string, std::string>& LanguageCodes() {
	static const std::unordered_map<std::string, std::string> codes = {
		{"ar", "arabic"},	 {"bg", "bulgarian"},	{"zh-cn", "schinese"}, {"zh-tw", "tchinese"},
		{"cs", "czech"},	 {"da", "danish"},		{"nl", "dutch"},	   {"en", "english"},
		{"fi", "finnish"},	 {"fr", "french"},		{"de", "german"},	   {"el", "greek"},
		{"hu", "hungarian"}, {"id", "indonesian"},	{"it", "italian"},	   {"ja", "japanese"},
		{"ko", "koreana"},	 {"no", "norwegian"},	{"pl", "polish"},	   {"pt", "portuguese"},
		{"pt-br", "brazilian"}, {"ro", "romanian"}, {"ru", "russian"},	   {"es", "spanish"},
		{"es-419", "latam"}, {"sv", "swedish"},		{"th", "thai"},		   {"tr", "turkish"},
		{"uk", "ukrainian"}, {"vn", "vietnamese"},
	};
	return codes;
}

// "csgo_russian.txt", a full "GAME:resource/csgo_russian.txt" string, a bare code "ru" or the key
// itself "russian" all normalise to the Valve language key ("russian").
std::string NormalizeLanguageName(std::string_view raw) {
	std::string_view name = Trim(raw);
	const std::size_t slash = name.find_last_of("/\\");
	if (slash != std::string_view::npos) {
		name.remove_prefix(slash + 1);
	}
	if (EndsWithNoCase(name, ".txt")) {
		name.remove_suffix(4);
	}
	if (StartsWithNoCase(name, "csgo_")) {
		name.remove_prefix(5);
	}
	const std::string lower = Lower(name);
	const auto& codes = LanguageCodes();
	const auto it = codes.find(lower);
	return it == codes.end() ? lower : it->second;
}

// "GAME:resource/csgo_english.txt" -> "resource/csgo_english.txt" (the filesystem list elements are
// "<search path>:<relative path>", which is why the C# reference does Split(':').Last()).
std::string PathFromSearchString(std::string value) {
	const std::size_t colon = value.rfind(':');
	if (colon != std::string::npos) {
		value.erase(0, colon + 1);
	}
	return value;
}

// "SFUI_WPNHUD_AK47" (a "#token" with the '#' stripped), lower-cased: token lookup is
// case-insensitive (the C# dictionaries use StringComparer.OrdinalIgnoreCase).
std::string TokenFrom(std::string_view value) {
	std::string_view text = Trim(value);
	if (!text.empty() && text.front() == '#') {
		text.remove_prefix(1);
	}
	return Lower(text);
}

// --- KeyValues helpers ------------------------------------------------------------------------

// Direct child with this name; with needValue false any node counts (presence check, e.g.
// `baseitem`), with true only children that carry a value (Valve's FindKey + GetString chain).
const Kv* FindDirect(const Kv& node, std::string_view field, bool needValue) {
	for (const Kv* child = node.GetFirstSubKey(); child; child = child->GetNextKey()) {
		if (EqualNoCase(child->GetName(), field) && (!needValue || child->HasValue())) {
			return child;
		}
	}
	return nullptr;
}

// All direct children with this name that are blocks. items_game repeats some blocks
// (several "sticker_kits" / "paint_kits" / "items" sections), which FindKey alone would hide.
void CollectBlocks(const Kv& node, std::string_view name, std::vector<const Kv*>& out) {
	for (const Kv* child = node.GetFirstSubKey(); child; child = child->GetNextKey()) {
		if (EqualNoCase(child->GetName(), name) && child->HasChildren()) {
			out.push_back(child);
		}
	}
}

using PrefabMap = std::unordered_map<std::string, const Kv*>;

PrefabMap CollectPrefabs(const Kv& root) {
	PrefabMap prefabs;
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "prefabs", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* prefab = block->GetFirstSubKey(); prefab; prefab = prefab->GetNextKey()) {
			// Valve's FindKey answers the first duplicate; keep the same rule.
			prefabs.try_emplace(Lower(prefab->GetName()), prefab);
		}
	}
	return prefabs;
}

// The prefab names a node declares: every "prefab" value child plus the value children of a
// "prefabs" node (both spellings exist in Valve data).
void PrefabNames(const Kv& node, std::vector<std::string>& out) {
	for (const Kv* child = node.GetFirstSubKey(); child; child = child->GetNextKey()) {
		if (EqualNoCase(child->GetName(), "prefab")) {
			if (child->HasValue()) {
				std::string_view value = Trim(child->GetString(nullptr));
				if (!value.empty()) {
					out.emplace_back(value);
				}
			} else if (child->HasChildren()) {
				for (const Kv* nested = child->GetFirstSubKey(); nested; nested = nested->GetNextKey()) {
					if (nested->HasValue()) {
						std::string_view value = Trim(nested->GetString(nullptr));
						if (!value.empty()) {
							out.emplace_back(value);
						}
					}
				}
			}
		} else if (EqualNoCase(child->GetName(), "prefabs") && child->HasChildren()) {
			for (const Kv* nested = child->GetFirstSubKey(); nested; nested = nested->GetNextKey()) {
				if (nested->HasValue()) {
					std::string_view value = Trim(nested->GetString(nullptr));
					if (!value.empty()) {
						out.emplace_back(value);
					}
				}
			}
		}
	}
}

struct PrefabLink {
	std::string name;
	const Kv* node = nullptr; // nullptr when the prefab is not defined
};

// The resolved prefab chain of a node (breadth-first), bounded and cycle-safe: a prefab that is
// already on the chain (or was already visited) is not walked again.
std::vector<PrefabLink> PrefabChain(const Kv& node, const PrefabMap& prefabs) {
	std::vector<PrefabLink> chain;
	std::unordered_set<std::string> seen;
	std::vector<std::string> pending;
	PrefabNames(node, pending);
	while (!pending.empty() && chain.size() < static_cast<std::size_t>(kMaxPrefabSteps)) {
		std::string name = std::move(pending.back());
		pending.pop_back();
		const std::string lower = Lower(name);
		if (!seen.insert(lower).second) {
			continue;
		}
		PrefabLink link;
		link.name = std::move(name);
		const auto it = prefabs.find(lower);
		if (it != prefabs.end()) {
			link.node = it->second;
			std::vector<std::string> more;
			PrefabNames(*it->second, more);
			for (std::string& next : more) {
				pending.push_back(std::move(next));
			}
		}
		chain.push_back(std::move(link));
	}
	return chain;
}

bool ChainHasPrefab(const Kv& node, const PrefabMap& prefabs, std::string_view wanted) {
	for (const PrefabLink& link : PrefabChain(node, prefabs)) {
		if (EqualNoCase(link.name, wanted)) {
			return true;
		}
	}
	return false;
}

const Kv* ResolveFieldImpl(const Kv& node, const PrefabMap& prefabs, std::string_view field, bool needValue,
						   std::unordered_set<const Kv*>& visited, int budget) {
	if (const Kv* own = FindDirect(node, field, needValue)) {
		return own;
	}
	if (budget <= 0 || !visited.insert(&node).second) {
		return nullptr;
	}
	for (const PrefabLink& link : PrefabChain(node, prefabs)) {
		if (link.node == nullptr) {
			continue;
		}
		if (const Kv* found = ResolveFieldImpl(*link.node, prefabs, field, needValue, visited, budget - 1)) {
			return found;
		}
	}
	return nullptr;
}

// Field lookup through the prefab chain: the node's own value wins, then the nearest prefab.
const Kv* ResolveField(const Kv& node, const PrefabMap& prefabs, std::string_view field, bool needValue = true) {
	std::unordered_set<const Kv*> visited;
	return ResolveFieldImpl(node, prefabs, field, needValue, visited, kMaxPrefabSteps);
}

std::string Field(const Kv& node, const PrefabMap& prefabs, std::string_view name) {
	const Kv* found = ResolveField(node, prefabs, name, true);
	return found ? std::string(Trim(found->GetString(nullptr))) : std::string();
}

// --- per-block parsers ------------------------------------------------------------------------

bool ParseIntKey(std::string_view text, int& out) {
	text = Trim(text);
	if (text.empty()) {
		return false;
	}
	int value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
		return false;
	}
	out = value;
	return true;
}

int TeamFromClasses(const Kv* classes) {
	if (classes == nullptr) {
		return 0;
	}
	bool ct = false;
	bool t = false;
	for (const Kv* child = classes->GetFirstSubKey(); child; child = child->GetNextKey()) {
		if (!child->HasValue()) {
			continue;
		}
		// The installed items_game spells the two sides out ("terrorists" /
		// "counter-terrorists"); older data used the short "t"/"ct" names. Both are accepted.
		if (EqualNoCase(child->GetName(), "ct") || EqualNoCase(child->GetName(), "counter-terrorists")) {
			ct = true;
		} else if (EqualNoCase(child->GetName(), "t") || EqualNoCase(child->GetName(), "terrorists")) {
			t = true;
		}
	}
	if (t && !ct) {
		return 2; // s2sdk CSTeam::T (engine team, not Pisex's 0/1)
	}
	if (ct && !t) {
		return 3; // s2sdk CSTeam::CT
	}
	return 0; // both or neither: not team-restricted
}

// First value anywhere below `node` that looks like an agent model ("/tm_", "/ctm_", or the same
// at the start, after '\' -> '/' normalisation) — the C# FindAgentModelInChildren.
const Kv* FindAgentModelNode(const Kv& node, int depth) {
	for (const Kv* child = node.GetFirstSubKey(); child; child = child->GetNextKey()) {
		if (child->HasValue()) {
			std::string value(Trim(child->GetString(nullptr)));
			for (char& c : value) {
				if (c == '\\') {
					c = '/';
				}
			}
			if (ContainsNoCase(value, "/tm_") || ContainsNoCase(value, "/ctm_") || StartsWithNoCase(value, "tm_") ||
				StartsWithNoCase(value, "ctm_")) {
				return child;
			}
		}
		if (child->HasChildren() && depth < kMaxAgentModelDepth) {
			if (const Kv* found = FindAgentModelNode(*child, depth + 1)) {
				return found;
			}
		}
	}
	return nullptr;
}

std::string NormalizeAgentModel(std::string value) {
	for (char& c : value) {
		if (c == '\\') {
			c = '/';
		}
	}
	const std::size_t marker = IndexOfNoCase(value, "characters/models/");
	if (marker != std::string::npos && marker > 0) {
		value.erase(0, marker);
	}
	if (!EndsWithNoCase(value, ".vmdl")) {
		value += ".vmdl";
	}
	return value;
}

std::string AgentInternalName(std::string_view model) {
	std::string name(model);
	if (StartsWithNoCase(name, "characters/models/")) {
		name.erase(0, std::strlen("characters/models/"));
	}
	if (EndsWithNoCase(name, ".vmdl")) {
		name.erase(name.size() - 5);
	}
	return name;
}

// The C# GetLocalizedNames / GetLocalizedName fallback chain, over the languages parsed so far.
class Localizer {
public:
	Localizer(const TokenMap& tokens, std::string primary) : tokens_(&tokens), primary_(Lower(primary)) {}

	std::unordered_map<std::string, std::string> Localized(std::string_view token) const {
		std::unordered_map<std::string, std::string> out;
		if (token.empty() || tokens_->empty()) {
			return out;
		}
		std::string primaryValue;
		std::string englishValue;
		std::vector<std::string> missing;
		for (const auto& [language, strings] : *tokens_) {
			const auto it = strings.find(std::string(token));
			if (it == strings.end()) {
				missing.push_back(language);
				continue;
			}
			out[language] = it->second;
			if (primaryValue.empty() && language == primary_) {
				primaryValue = it->second;
			}
			if (englishValue.empty() && language == "english") {
				englishValue = it->second;
			}
		}
		for (const std::string& language : missing) {
			if (!primaryValue.empty()) {
				out[language] = primaryValue;
			} else if (!englishValue.empty()) {
				// Hard-coded english fallback (EconService.GetLocalizedNames).
				out[primary_] = englishValue;
				out[language] = englishValue;
			}
		}
		return out;
	}

private:
	const TokenMap* tokens_;
	std::string primary_;
};

struct ParsedEcon {
	std::unordered_map<uint16_t, ItemDefinition> items;
	std::unordered_map<int, PaintKit> paints;
	std::unordered_map<int, EconDefinition> agents;
	std::unordered_map<int, EconDefinition> stickers;
	std::unordered_map<int, EconDefinition> keychains;
	std::unordered_map<int, EconDefinition> musicKits;
	std::unordered_map<std::string, std::vector<std::string>> lootLists;
};

void ParseItemsSection(const Kv& root, const PrefabMap& prefabs, const Localizer& localizer, ParsedEcon& out) {
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "items", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* item = block->GetFirstSubKey(); item; item = item->GetNextKey()) {
			if (!item->HasChildren()) {
				continue;
			}
			int index = 0;
			if (!ParseIntKey(item->GetName(), index) || index < 0 || index > 0xFFFF) {
				continue;
			}
			const bool baseItem = ResolveField(*item, prefabs, "baseitem", false) != nullptr;
			const bool knifeOrGlove = ChainHasPrefab(*item, prefabs, "melee_unusual") ||
									  ChainHasPrefab(*item, prefabs, "hands_paintable");
			if (!baseItem && !knifeOrGlove) {
				continue; // agents and non-item entries are handled below / not at all
			}
			const std::string name = Field(*item, prefabs, "name");
			if (name.empty()) {
				continue;
			}
			ItemDefinition definition;
			definition.index = static_cast<uint16_t>(index);
			definition.name = name;
			definition.model = Field(*item, prefabs, "model_player");
			// used_by_classes is a block (no value of its own), so it is resolved by presence.
			definition.team = TeamFromClasses(ResolveField(*item, prefabs, "used_by_classes", false));
			const Kv* itemName = ResolveField(*item, prefabs, "item_name", true);
			if (itemName != nullptr) {
				definition.localizedNames = localizer.Localized(TokenFrom(itemName->GetString(nullptr)));
			}
			out.items[static_cast<uint16_t>(index)] = std::move(definition); // later duplicates win (C# dictionary)
		}
	}
}

void ParseAgentsSection(const Kv& root, const PrefabMap& prefabs, const Localizer& localizer, ParsedEcon& out) {
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "items", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* item = block->GetFirstSubKey(); item; item = item->GetNextKey()) {
			if (!item->HasChildren()) {
				continue;
			}
			int index = 0;
			if (!ParseIntKey(item->GetName(), index)) {
				continue;
			}
			if (!ChainHasPrefab(*item, prefabs, "customplayertradable")) {
				continue;
			}
			// Model: the item's own subtree first, then each prefab of the chain (the C# only
			// looked into the item and its direct prefab; chains make the inherited model work).
			const Kv* modelNode = FindAgentModelNode(*item, 0);
			if (modelNode == nullptr) {
				for (const PrefabLink& link : PrefabChain(*item, prefabs)) {
					if (link.node == nullptr) {
						continue;
					}
					modelNode = FindAgentModelNode(*link.node, 0);
					if (modelNode != nullptr) {
						break;
					}
				}
			}
			if (modelNode == nullptr) {
				continue; // no model -> not a usable agent (C# `continue`)
			}
			EconDefinition definition;
			definition.index = index;
			definition.model = NormalizeAgentModel(std::string(Trim(modelNode->GetString(nullptr))));
			definition.name = AgentInternalName(definition.model);
			std::string token = Field(*item, prefabs, "item_name");
			if (token.empty()) {
				token = Field(*item, prefabs, "loc_name");
			}
			if (!token.empty()) {
				definition.localizedNames = localizer.Localized(TokenFrom(token));
			}
			out.agents[index] = std::move(definition);
		}
	}
}

void ParsePaintKitsSection(const Kv& root, const Localizer& localizer, ParsedEcon& out) {
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "paint_kits", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* kit = block->GetFirstSubKey(); kit; kit = kit->GetNextKey()) {
			if (!kit->HasChildren()) {
				continue;
			}
			int index = 0;
			if (!ParseIntKey(kit->GetName(), index)) {
				continue; // non-numeric kit keys are not addressable through the schema
			}
			const Kv* name = FindDirect(*kit, "name", true);
			if (name == nullptr) {
				continue;
			}
			PaintKit paint;
			paint.index = index;
			paint.name = std::string(Trim(name->GetString(nullptr)));
			// Direct flag on the kit itself: HasSubKeyWithValue("use_legacy_model", "1").
			if (const Kv* legacy = FindDirect(*kit, "use_legacy_model", true)) {
				paint.legacy = Trim(legacy->GetString(nullptr)) == "1";
			}
			if (const Kv* tag = FindDirect(*kit, "description_tag", true)) {
				paint.localizedNames = localizer.Localized(TokenFrom(tag->GetString(nullptr)));
			}
			out.paints[index] = std::move(paint);
		}
	}
}

void ParseStickerKitsSection(const Kv& root, const Localizer& localizer, ParsedEcon& out) {
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "sticker_kits", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* kit = block->GetFirstSubKey(); kit; kit = kit->GetNextKey()) {
			if (!kit->HasChildren()) {
				continue;
			}
			int index = 0;
			if (!ParseIntKey(kit->GetName(), index)) {
				continue;
			}
			// The C# reference only keeps kits that carry a sticker material (spray kits and other
			// non-sticker entries of the same block have none).
			if (FindDirect(*kit, "sticker_material", true) == nullptr) {
				continue;
			}
			EconDefinition definition;
			definition.index = index;
			const Kv* name = FindDirect(*kit, "name", true);
			definition.name = name != nullptr ? std::string(Trim(name->GetString(nullptr))) : std::string(kit->GetName());
			if (const Kv* tag = FindDirect(*kit, "item_name", true)) {
				definition.localizedNames = localizer.Localized(TokenFrom(tag->GetString(nullptr)));
			}
			out.stickers[index] = std::move(definition);
		}
	}
}

void ParseKeychainDefinitionsSection(const Kv& root, const Localizer& localizer, ParsedEcon& out) {
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "keychain_definitions", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* keychain = block->GetFirstSubKey(); keychain; keychain = keychain->GetNextKey()) {
			if (!keychain->HasChildren()) {
				continue;
			}
			int index = 0;
			if (!ParseIntKey(keychain->GetName(), index)) {
				continue;
			}
			EconDefinition definition;
			definition.index = index;
			// The record's own "name" field when present (the installed data has
			// "kc_cologne2026"); the block key is the fallback.
			const Kv* name = FindDirect(*keychain, "name", true);
			definition.name = name != nullptr ? std::string(Trim(name->GetString(nullptr))) : std::string(keychain->GetName());
			if (const Kv* tag = FindDirect(*keychain, "loc_name", true)) {
				definition.localizedNames = localizer.Localized(TokenFrom(tag->GetString(nullptr)));
			}
			out.keychains[index] = std::move(definition);
		}
	}
}

void ParseMusicDefinitionsSection(const Kv& root, const Localizer& localizer, ParsedEcon& out) {
	std::vector<const Kv*> blocks;
	CollectBlocks(root, "music_definitions", blocks);
	for (const Kv* block : blocks) {
		for (const Kv* music = block->GetFirstSubKey(); music; music = music->GetNextKey()) {
			if (!music->HasChildren()) {
				continue;
			}
			int index = music->GetInt("id", 0);
			if (index <= 0) {
				int keyIndex = 0;
				if (!ParseIntKey(music->GetName(), keyIndex)) {
					continue;
				}
				index = keyIndex;
			}
			if (index <= 0) {
				continue; // not addressable (the schema key is the music id)
			}
			EconDefinition definition;
			definition.index = index;
			// The record's own "name" field when present (the installed data has
			// "valve_cs2_01"); the block key is the fallback.
			const Kv* name = FindDirect(*music, "name", true);
			definition.name = name != nullptr ? std::string(Trim(name->GetString(nullptr))) : std::string(music->GetName());
			const Kv* tag = FindDirect(*music, "loc_name", true);
			if (tag == nullptr) {
				tag = name;
			}
			if (tag != nullptr) {
				definition.localizedNames = localizer.Localized(TokenFrom(tag->GetString(nullptr)));
			}
			out.musicKits[index] = std::move(definition);
		}
	}
}

struct RawLootList {
	std::vector<std::string> entries;
	bool expand = false; // client_loot_lists references other lists; loot_lists does not
};

// Reference expansion: a child naming another list is replaced by that list (recursively).
// `visiting` breaks cycles, the depth cap bounds the work.
void ResolveLootList(const std::unordered_map<std::string, RawLootList>& raw,
					 const std::unordered_map<std::string, std::string>& byLower, const std::string& name,
					 std::unordered_set<std::string>& visiting, std::vector<std::string>& result, int depth) {
	const auto listIt = raw.find(name);
	if (listIt == raw.end()) {
		return;
	}
	for (const std::string& entry : listIt->second.entries) {
		const auto lowerIt = byLower.find(Lower(entry));
		const bool reference = listIt->second.expand && lowerIt != byLower.end() && lowerIt->second != name;
		if (!reference || depth >= kMaxLootDepth) {
			if (listIt->second.expand && lowerIt != byLower.end() && lowerIt->second == name) {
				continue; // direct self-reference: skip instead of looping
			}
			result.push_back(entry);
			continue;
		}
		const std::string& target = lowerIt->second;
		if (!visiting.insert(target).second) {
			continue; // cycle: the list is already being resolved further up
		}
		ResolveLootList(raw, byLower, target, visiting, result, depth + 1);
		visiting.erase(target);
	}
}

void ParseLootListsSection(const Kv& root, ParsedEcon& out) {
	std::unordered_map<std::string, RawLootList> raw;
	std::vector<std::string> order; // insertion order, so the resolved map is deterministic

	const auto collect = [&](std::string_view blockName, bool expand) {
		std::vector<const Kv*> blocks;
		CollectBlocks(root, blockName, blocks);
		for (const Kv* block : blocks) {
			for (const Kv* list = block->GetFirstSubKey(); list; list = list->GetNextKey()) {
				if (!list->HasChildren()) {
					continue;
				}
				const std::string listName = list->GetName();
				RawLootList& entry = raw[listName];
				if (entry.entries.empty() && !entry.expand) {
					order.push_back(listName);
				}
				entry.expand = entry.expand || expand;
				for (const Kv* child = list->GetFirstSubKey(); child; child = child->GetNextKey()) {
					if (!child->HasValue()) {
						continue;
					}
					std::string name = child->GetName();
					if (!name.empty() && name.front() == '[') {
						const std::size_t close = name.find(']');
						if (close == std::string::npos || close <= 1 || name.find(']', close + 1) != std::string::npos) {
							continue; // malformed "[a]b]c": the C# requires exactly one ']'
						}
						name = name.substr(1, close - 1);
					}
					entry.entries.push_back(std::move(name));
				}
			}
		}
	};
	collect("loot_lists", false);
	collect("client_loot_lists", true);

	std::unordered_map<std::string, std::string> byLower;
	for (const auto& [name, list] : raw) {
		(void)list;
		byLower[Lower(name)] = name;
	}

	for (const std::string& name : order) {
		std::unordered_set<std::string> visiting;
		visiting.insert(name);
		std::vector<std::string> resolved;
		ResolveLootList(raw, byLower, name, visiting, resolved, 0);
		out.lootLists[name] = std::move(resolved);
	}
}

// Parses the text of items_game (already decoded to UTF-8) into `out`. `out` is only filled on
// success — the callers commit it, which is what makes Parse() atomic.
bool ParseItemsGameText(const std::string& text, const TokenMap& tokens, const std::string& primaryLanguage,
						ParsedEcon& out, std::string& error) {
	if (text.empty()) {
		error = "items_game: empty input";
		return false;
	}
	pisex::KeyValues kv("items_game");
	if (!kv.LoadFromBuffer(text)) {
		error = "items_game: malformed KeyValues (truncated or unparsable)";
		return false;
	}
	const Kv* root = &kv;
	if (const Kv* inner = kv.FindKey("items_game"); inner != nullptr && inner->HasChildren()) {
		root = inner; // the buffer had a wrapper the parser could not merge
	}
	std::vector<const Kv*> itemBlocks;
	CollectBlocks(*root, "items", itemBlocks);
	if (itemBlocks.empty()) {
		error = "items_game: no 'items' section found";
		return false;
	}
	const PrefabMap prefabs = CollectPrefabs(*root);
	const Localizer localizer(tokens, primaryLanguage);
	ParseItemsSection(*root, prefabs, localizer, out);
	ParseAgentsSection(*root, prefabs, localizer, out);
	ParsePaintKitsSection(*root, localizer, out);
	ParseStickerKitsSection(*root, localizer, out);
	ParseKeychainDefinitionsSection(*root, localizer, out);
	ParseMusicDefinitionsSection(*root, localizer, out);
	ParseLootListsSection(*root, out);
	return true;
}

// Parses one csgo_*.txt file into the language map. Only committed on success.
bool ParseLanguageText(TokenMap& tokens, const std::string& language, std::string_view text, std::string& error) {
	std::string decoded;
	DecodeText(text, decoded);
	if (decoded.empty()) {
		error = "language '" + language + "': empty input";
		return false;
	}
	pisex::KeyValues kv("lang");
	if (!kv.LoadFromBuffer(decoded)) {
		error = "language '" + language + "': malformed KeyValues";
		return false;
	}
	const Kv* tokensNode = nullptr;
	if (EqualNoCase(kv.GetName(), "tokens")) {
		tokensNode = &kv;
	} else {
		tokensNode = kv.FindKey("Tokens");
	}
	if (tokensNode == nullptr || !tokensNode->HasChildren()) {
		error = "language '" + language + "': no Tokens section";
		return false;
	}
	std::unordered_map<std::string, std::string> parsed;
	for (const Kv* child = tokensNode->GetFirstSubKey(); child; child = child->GetNextKey()) {
		if (!child->HasValue()) {
			continue;
		}
		std::string_view value = Trim(child->GetString(nullptr));
		if (value.empty()) {
			continue;
		}
		parsed[Lower(Trim(child->GetName()))] = std::string(value);
	}
	tokens[language] = std::move(parsed);
	return true;
}

std::string ReadGameFile(const std::string& path) {
	if (__s2sdk_ReadFileVPK == nullptr) {
		return {};
	}
	const plg::string contents = s2sdk::ReadFileVPK(plg::string(path.c_str(), path.size()), plg::string("GAME", 4));
	return std::string(contents.data(), contents.size());
}

} // namespace

// --- EconData ---------------------------------------------------------------------------------

bool EconData::Parse(std::string_view itemsGame, std::string& error) {
	error.clear();
	ParsedEcon parsed;
	std::string text;
	DecodeText(itemsGame, text);
	if (!ParseItemsGameText(text, tokens_, "english", parsed, error)) {
		return false; // the previous snapshot stays untouched
	}
	items = std::move(parsed.items);
	paints = std::move(parsed.paints);
	agents = std::move(parsed.agents);
	stickers = std::move(parsed.stickers);
	keychains = std::move(parsed.keychains);
	musicKits = std::move(parsed.musicKits);
	lootLists = std::move(parsed.lootLists);
	return true;
}

bool EconData::ParseLanguage(std::string_view language, std::string_view text, std::string& error) {
	error.clear();
	const std::string key = NormalizeLanguageName(language);
	if (key.empty()) {
		error = "language: empty name";
		return false;
	}
	return ParseLanguageText(tokens_, key, text, error);
}

bool EconData::LoadFromGame(const std::vector<std::string>& languages, std::string& error, bool loadLanguages) {
	error.clear();
	if (__s2sdk_ReadFileVPK == nullptr || __s2sdk_FindFileAbsoluteList == nullptr) {
		error = "LoadFromGame: s2sdk filesystem bindings are not available";
		return false;
	}
	const std::string itemsText = ReadGameFile("scripts/items/items_game.txt");
	if (itemsText.empty()) {
		error = "LoadFromGame: scripts/items/items_game.txt is unreadable or empty";
		return false;
	}

	std::vector<std::string> requested;
	for (const std::string& language : languages) {
		const std::string key = NormalizeLanguageName(language);
		if (!key.empty() && std::find(requested.begin(), requested.end(), key) == requested.end()) {
			requested.push_back(key);
		}
	}
	const std::string primary = requested.empty() ? "english" : requested.front();

	TokenMap tokens;
	const plg::vector<plg::string> languageFiles =
		loadLanguages ? s2sdk::FindFileAbsoluteList(plg::string("resource/csgo_*", 15), plg::string("GAME", 4))
					  : plg::vector<plg::string>{};
	for (const plg::string& entry : languageFiles) {
		const std::string path = PathFromSearchString(std::string(entry.data(), entry.size()));
		const std::string key = NormalizeLanguageName(path);
		if (key.empty()) {
			continue;
		}
		// ItemLanguages = [] loads every language Valve ships; a non-empty list loads exactly the
		// requested languages plus english, which every fallback path needs.
		if (!requested.empty() && key != "english" &&
			std::find(requested.begin(), requested.end(), key) == requested.end()) {
			continue;
		}
		const std::string text = ReadGameFile(path);
		if (text.empty()) {
			continue; // a language file that cannot be read is skipped, not fatal
		}
		std::string ignored;
		ParseLanguageText(tokens, key, text, ignored);
	}

	ParsedEcon parsed;
	if (!ParseItemsGameText(itemsText, tokens, primary, parsed, error)) {
		return false; // nothing is replaced on failure
	}
	tokens_ = std::move(tokens);
	items = std::move(parsed.items);
	paints = std::move(parsed.paints);
	agents = std::move(parsed.agents);
	stickers = std::move(parsed.stickers);
	keychains = std::move(parsed.keychains);
	musicKits = std::move(parsed.musicKits);
	lootLists = std::move(parsed.lootLists);
	return true;
}

const ItemDefinition* EconData::Item(uint16_t index) const {
	const auto it = items.find(index);
	return it == items.end() ? nullptr : &it->second;
}

const EconDefinition* EconData::Agent(int index) const {
	const auto it = agents.find(index);
	return it == agents.end() ? nullptr : &it->second;
}

// Whether the paint kit uses a legacy (pre-rework bodygroup) model. The C# reference asks the same
// question through WeaponToPaintkits[classname] -> UseLegacyModel; the weapon classname is only
// needed there to locate the kit inside the per-weapon list, and this port keys kits by their
// index, so the weapon definition is not required for the answer. It stays in the signature
// because the engine's bodygroup call sites carry it. An unknown paint kit answers false, so the
// caller keeps the default bodygroup instead of guessing.
bool EconData::Legacy(uint16_t definition, int paint) const {
	(void)definition;
	const auto it = paints.find(paint);
	return it != paints.end() && it->second.legacy;
}

} // namespace ws
