// WeaponSkins econ tests — the engine-free half of EconData: items_game parsing (prefab chains,
// the weapon/knife/glove filter, paint kits, agents, sticker kits, keychains, music kits, loot
// lists), the language token tables (UTF-8 + BOM, UTF-16, fallback) and the atomicity/error paths.
//
// LoadFromGame is exercised with fake s2sdk filesystem bindings, the same trick the pisexcompat
// tests use (libs/pisexcompat/test/timers_engine_test.cpp): the __s2sdk_* binding globals are
// plain function pointers, so a test can install fakes and drive the real code path.
//
// Fixtures live next to this file (tests/fixtures/) and are read through __FILE__, so the test
// needs no compile definition from CMake. Run by ./test.sh (build/tests/econ_tests).

#include "econ.hpp"

#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A real plugin gets these from PLUGIFY_PLUGIN; the test binary has no plugify module, so they
// stay null (the same block every engine test in the tree carries).
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what, const char* file, int line) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
	}
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)

void Section(const char* name) { std::printf("== %s\n", name); }

// --- fixtures ---------------------------------------------------------------------------------

std::string ReadFixture(const std::string& name) {
	const std::filesystem::path path =
		std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		std::printf("FAIL: cannot read fixture %s\n", path.string().c_str());
		++g_failures;
		return {};
	}
	return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

const std::string& ItemsGame() {
	static const std::string text = ReadFixture("items_game.txt");
	return text;
}
const std::string& English() {
	static const std::string text = ReadFixture("csgo_english.txt");
	return text;
}
const std::string& Russian() {
	static const std::string text = ReadFixture("csgo_russian.txt");
	return text;
}
// The source-grounded excerpt of the installed game data (fixtures/game_excerpt_source.md): whole
// verbatim records of items_game plus a sibling "lang" block with original English tokens. It was
// extracted for these tests; the whole file is passed to Parse and the final "lang" substring to
// ParseLanguage, as the source note instructs.
const std::string& GameExcerpt() {
	static const std::string text = ReadFixture("game_excerpt.txt");
	return text;
}

bool Contains(const std::vector<std::string>& values, std::string_view wanted) {
	return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool HasLanguage(const std::unordered_map<std::string, std::string>& names, std::string_view language,
				 std::string_view value) {
	const auto it = names.find(std::string(language));
	return it != names.end() && it->second == value;
}

bool HasLanguageContaining(const std::unordered_map<std::string, std::string>& names, std::string_view language,
						   std::string_view needle) {
	const auto it = names.find(std::string(language));
	return it != names.end() && it->second.find(needle) != std::string::npos;
}

// UTF-8 -> UTF-16LE bytes with a BOM (BMP only: enough for every language file in the tests).
std::string ToUtf16Le(std::string_view utf8) {
	std::string out("\xFF\xFE", 2);
	std::size_t i = 0;
	while (i < utf8.size()) {
		std::uint32_t codePoint = static_cast<unsigned char>(utf8[i]);
		std::size_t length = 1;
		if (codePoint >= 0xF0) {
			length = 4;
		} else if (codePoint >= 0xE0) {
			length = 3;
		} else if (codePoint >= 0xC0) {
			length = 2;
		}
		codePoint = length == 1 ? codePoint : codePoint & (0xFF >> (length + 1));
		for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
			codePoint = (codePoint << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
		}
		i += length;
		if (codePoint <= 0xFFFF) {
			out.push_back(static_cast<char>(codePoint & 0xFF));
			out.push_back(static_cast<char>((codePoint >> 8) & 0xFF));
		} else {
			codePoint -= 0x10000;
			const std::uint16_t high = static_cast<std::uint16_t>(0xD800 + (codePoint >> 10));
			const std::uint16_t low = static_cast<std::uint16_t>(0xDC00 + (codePoint & 0x3FF));
			out.push_back(static_cast<char>(high & 0xFF));
			out.push_back(static_cast<char>((high >> 8) & 0xFF));
			out.push_back(static_cast<char>(low & 0xFF));
			out.push_back(static_cast<char>((low >> 8) & 0xFF));
		}
	}
	return out;
}

// --- fake s2sdk filesystem bindings -----------------------------------------------------------

std::unordered_map<std::string, std::string> g_fakeFiles;
std::vector<std::string> g_fakeList;
std::string g_lastReadPath;
std::string g_lastReadPathId;
std::string g_lastListWildcard;
std::string g_lastListPathId;
int g_readCalls = 0;
int g_listCalls = 0;

plg::string FakeReadFileVPK(const plg::string& name, const plg::string& pathId) {
	++g_readCalls;
	g_lastReadPath.assign(name.data(), name.size());
	g_lastReadPathId.assign(pathId.data(), pathId.size());
	const auto it = g_fakeFiles.find(g_lastReadPath);
	if (it == g_fakeFiles.end()) {
		return {};
	}
	return plg::string(it->second.data(), it->second.size());
}

plg::vector<plg::string> FakeFindFileAbsoluteList(const plg::string& wildcard, const plg::string& pathId) {
	++g_listCalls;
	g_lastListWildcard.assign(wildcard.data(), wildcard.size());
	g_lastListPathId.assign(pathId.data(), pathId.size());
	plg::vector<plg::string> out;
	for (const std::string& entry : g_fakeList) {
		out.push_back(plg::string(entry.data(), entry.size()));
	}
	return out;
}

void InstallFakes() {
	__s2sdk_ReadFileVPK = &FakeReadFileVPK;
	__s2sdk_FindFileAbsoluteList = &FakeFindFileAbsoluteList;
}

void UninstallFakes() {
	__s2sdk_ReadFileVPK = nullptr;
	__s2sdk_FindFileAbsoluteList = nullptr;
}

// --- sections ---------------------------------------------------------------------------------

void TestItems() {
	Section("items (weapons/knives/gloves, prefab merging)");
	ws::EconData econ;
	std::string error;
	CHECK(econ.Parse(ItemsGame(), error));
	CHECK(error.empty());
	CHECK(econ.items.size() == 7); // 7, 23, 40, 60, 507, 5027, 9001

	const ws::ItemDefinition* mp5sd = econ.Item(23);
	CHECK(mp5sd != nullptr);
	if (mp5sd != nullptr) {
		CHECK(mp5sd->index == 23);
		CHECK(mp5sd->name == "weapon_mp5sd");
		CHECK(mp5sd->model == "models/weapons/v_smg_mp5sd.vmdl"); // two prefab levels up
		CHECK(mp5sd->team == 2);								  // used_by_classes inherited from the chain
		CHECK(mp5sd->localizedNames.empty());					  // no language parsed yet
	}

	const ws::ItemDefinition* ak47 = econ.Item(7);
	CHECK(ak47 != nullptr);
	if (ak47 != nullptr) {
		CHECK(ak47->name == "weapon_ak47");
		CHECK(ak47->model == "models/weapons/v_rif_ak47.vmdl");
		CHECK(ak47->team == 2); // T only
	}

	const ws::ItemDefinition* m4 = econ.Item(60);
	CHECK(m4 != nullptr && m4->team == 3); // CT only

	const ws::ItemDefinition* ssg = econ.Item(40);
	CHECK(ssg != nullptr && ssg->team == 0); // no used_by_classes: unrestricted

	const ws::ItemDefinition* knife = econ.Item(507);
	CHECK(knife != nullptr && knife->model == "models/weapons/v_knife_karambit.vmdl");

	const ws::ItemDefinition* gloves = econ.Item(5027);
	CHECK(gloves != nullptr && gloves->model == "models/weapons/v_gloves_bloodhound.vmdl");

	// Prefab cycle (loop_a <-> loop_b): the item is still parsed and its own model wins.
	const ws::ItemDefinition* loop = econ.Item(9001);
	CHECK(loop != nullptr && loop->model == "models/weapons/v_loop.vmdl");

	CHECK(econ.Item(100) == nullptr);  // no baseitem, no weapon prefab
	CHECK(econ.Item(6001) == nullptr); // agent, not a weapon
	CHECK(econ.Item(65000) == nullptr);
}

void TestPaintKits() {
	Section("paint kits (legacy flag, repeated blocks)");
	ws::EconData econ;
	std::string error;
	CHECK(econ.Parse(ItemsGame(), error));
	CHECK(econ.paints.size() == 3); // 1, 400, 777 (two blocks); "abc" skipped

	const auto plain = econ.paints.find(1);
	CHECK(plain != econ.paints.end());
	if (plain != econ.paints.end()) {
		CHECK(plain->second.name == "so_blue");
		CHECK(!plain->second.legacy);
	}
	const auto legacy = econ.paints.find(400);
	CHECK(legacy != econ.paints.end());
	if (legacy != econ.paints.end()) {
		CHECK(legacy->second.name == "aq_mp5sd_legacy");
		CHECK(legacy->second.legacy);
	}
	CHECK(econ.paints.count(777) == 1); // second "paint_kits" block

	CHECK(econ.Legacy(7, 400));
	CHECK(!econ.Legacy(7, 1));
	CHECK(!econ.Legacy(7, 424242)); // unknown paint kit
}

void TestAgents() {
	Section("agents (customplayertradable, model normalisation)");
	ws::EconData econ;
	std::string error;
	CHECK(econ.Parse(ItemsGame(), error));
	CHECK(econ.agents.size() == 2);

	const ws::EconDefinition* sas = econ.Agent(6001);
	CHECK(sas != nullptr);
	if (sas != nullptr) {
		CHECK(sas->model == "characters/models/ctm_sas/ctm_sas.vmdl");
		CHECK(sas->name == "ctm_sas/ctm_sas");
	}
	const ws::EconDefinition* balkan = econ.Agent(6002);
	CHECK(balkan != nullptr);
	if (balkan != nullptr) {
		// backslashes normalised, ".vmdl" appended, model inherited from the prefab chain
		CHECK(balkan->model == "characters/models/tm_balkan/tm_balkan_variantA.vmdl");
		CHECK(balkan->name == "tm_balkan/tm_balkan_variantA");
	}
	CHECK(econ.Agent(7) == nullptr);
}

void TestStickersKeychainsMusic() {
	Section("sticker kits / keychain definitions / music definitions");
	ws::EconData econ;
	std::string error;
	CHECK(econ.Parse(ItemsGame(), error));

	CHECK(econ.stickers.size() == 2); // 9 and 12 (second block); 13 has no sticker_material
	const auto crown = econ.stickers.find(9);
	CHECK(crown != econ.stickers.end());
	if (crown != econ.stickers.end()) {
		CHECK(crown->second.index == 9);
		CHECK(crown->second.name == "sticker_crown");
	}
	CHECK(econ.stickers.count(12) == 1);
	CHECK(econ.stickers.count(13) == 0);

	CHECK(econ.keychains.size() == 1);
	const auto keychain = econ.keychains.find(42);
	CHECK(keychain != econ.keychains.end());
	if (keychain != econ.keychains.end()) {
		CHECK(keychain->second.index == 42);
		CHECK(keychain->second.name == "keychain_diecast"); // preserve the schema's own name
	}

	CHECK(econ.musicKits.size() == 2);
	const auto main = econ.musicKits.find(3);
	CHECK(main != econ.musicKits.end());
	if (main != econ.musicKits.end()) {
		CHECK(main->second.name == "music_csgo_main");
	}
	const auto halo = econ.musicKits.find(70); // "id" wins over the block key "7"
	CHECK(halo != econ.musicKits.end());
	if (halo != econ.musicKits.end()) {
		CHECK(halo->second.index == 70);
		CHECK(halo->second.name == "music_halo");
	}
	CHECK(econ.musicKits.count(7) == 0);
}

void TestLootLists() {
	Section("loot lists (verbatim, reference expansion, cycles)");
	ws::EconData econ;
	std::string error;
	CHECK(econ.Parse(ItemsGame(), error));

	const auto alpha = econ.lootLists.find("weapon_case_alpha");
	CHECK(alpha != econ.lootLists.end());
	if (alpha != econ.lootLists.end()) {
		CHECK(alpha->second.size() == 2);
		CHECK(Contains(alpha->second, "weapon_ak47"));
		CHECK(Contains(alpha->second, "weapon_ssg08"));
	}

	const auto root = econ.lootLists.find("crate_root");
	CHECK(root != econ.lootLists.end());
	if (root != econ.lootLists.end()) {
		// "crate_group" expanded; the "[item]belonging" children reduced to the item names.
		CHECK(root->second.size() == 2);
		CHECK(Contains(root->second, "weapon_ak47"));
		CHECK(Contains(root->second, "weapon_deagle"));
	}

	for (const char* name : {"crate_cycle_a", "crate_cycle_b"}) {
		const auto cycle = econ.lootLists.find(name);
		CHECK(cycle != econ.lootLists.end());
		if (cycle != econ.lootLists.end()) {
			CHECK(cycle->second.size() == 1); // the back reference is dropped
			CHECK(Contains(cycle->second, "weapon_glock"));
		}
	}
}

void TestLanguagesReal() {
	Section("languages (applied to items)");
	ws::EconData econ;
	std::string error;
	// The english fixture carries a BOM in the game; the test adds it explicitly. The language name
	// is given in file form ("csgo_english.txt"), which has to normalise to "english".
	const std::string bom("\xEF\xBB\xBF", 3);
	CHECK(econ.ParseLanguage("csgo_english.txt", bom + English(), error));
	CHECK(econ.ParseLanguage("ru", Russian(), error)); // code form -> "russian"
	CHECK(econ.Parse(ItemsGame(), error));

	const ws::ItemDefinition* ak47 = econ.Item(7);
	CHECK(ak47 != nullptr);
	if (ak47 != nullptr) {
		CHECK(HasLanguage(ak47->localizedNames, "english", "AK-47"));
		CHECK(HasLanguage(ak47->localizedNames, "russian", "АК-47"));
	}
	// SSG 08 exists only in english: the russian entry falls back to it.
	const ws::ItemDefinition* ssg = econ.Item(40);
	CHECK(ssg != nullptr);
	if (ssg != nullptr) {
		CHECK(HasLanguage(ssg->localizedNames, "english", "SSG 08"));
		CHECK(HasLanguage(ssg->localizedNames, "russian", "SSG 08"));
	}
	// M4A1-S has no token at all: no localized names.
	const ws::ItemDefinition* m4 = econ.Item(60);
	CHECK(m4 != nullptr && m4->localizedNames.empty());

	// Paint kit fallback works the same way (only english has the second-block tag).
	const auto second = econ.paints.find(777);
	CHECK(second != econ.paints.end());
	if (second != econ.paints.end()) {
		CHECK(HasLanguage(second->second.localizedNames, "english", "Second Block"));
		CHECK(HasLanguage(second->second.localizedNames, "russian", "Second Block"));
	}
	// Agents, stickers, keychains and music kits carry their tokens too.
	const ws::EconDefinition* sas = econ.Agent(6001);
	CHECK(sas != nullptr && HasLanguage(sas->localizedNames, "russian", "САС"));
	CHECK(econ.stickers.count(9) == 1 &&
		  HasLanguage(econ.stickers.at(9).localizedNames, "english", "Crown (Foil)"));
	CHECK(econ.keychains.count(42) == 1 &&
		  HasLanguage(econ.keychains.at(42).localizedNames, "russian", "Литой"));
	CHECK(econ.musicKits.count(3) == 1 &&
		  HasLanguage(econ.musicKits.at(3).localizedNames, "english", "Main Menu"));

	// A language that was never loaded must not appear.
	CHECK(ak47 != nullptr && ak47->localizedNames.count("german") == 0);
}

void TestLanguageErrors() {
	Section("languages (error paths keep the loaded tables)");
	ws::EconData econ;
	std::string error;
	CHECK(econ.ParseLanguage("english", English(), error));

	CHECK(!econ.ParseLanguage("", "x", error));
	CHECK(!error.empty());
	error.clear();
	CHECK(!econ.ParseLanguage("german", "\"lang\" { \"Tokens\" { \"A\" \"B\"", error)); // truncated
	CHECK(!error.empty());
	error.clear();
	CHECK(!econ.ParseLanguage("german", "\"lang\" { \"Language\" \"German\" }", error)); // no Tokens
	CHECK(!error.empty());
	error.clear();
	CHECK(!econ.ParseLanguage("german", "", error));
	CHECK(!error.empty());

	// The english table survived the failed loads.
	CHECK(econ.Parse(ItemsGame(), error));
	const ws::ItemDefinition* ak47 = econ.Item(7);
	CHECK(ak47 != nullptr && HasLanguage(ak47->localizedNames, "english", "AK-47"));
	CHECK(ak47 != nullptr && ak47->localizedNames.size() == 1);

	// UTF-16LE with BOM (some Valve language files ship like this).
	const std::string utf16 = ToUtf16Le("\"lang\"\n{\n\"Tokens\"\n{\n\"SFUI_WPNHUD_AK47\" \"AK-47 (UTF16)\"\n}\n}\n");
	CHECK(econ.ParseLanguage("uk", utf16, error));
	CHECK(econ.Parse(ItemsGame(), error));
	const ws::ItemDefinition* ak47After = econ.Item(7);
	CHECK(ak47After != nullptr && HasLanguage(ak47After->localizedNames, "ukrainian", "AK-47 (UTF16)"));
	CHECK(ak47After != nullptr && HasLanguage(ak47After->localizedNames, "english", "AK-47"));

	// UTF-16BE with BOM, through the same path.
	std::string utf16be("\xFE\xFF", 2);
	for (std::size_t i = 2; i + 1 < utf16.size(); i += 2) {
		utf16be.push_back(utf16[i + 1]);
		utf16be.push_back(utf16[i]);
	}
	CHECK(econ.ParseLanguage("schinese", utf16be, error));
	CHECK(econ.Parse(ItemsGame(), error));
	const ws::ItemDefinition* ak47Be = econ.Item(7);
	CHECK(ak47Be != nullptr && HasLanguage(ak47Be->localizedNames, "schinese", "AK-47 (UTF16)"));
}

void TestAtomicity() {
	Section("atomicity (a malformed snapshot never corrupts the last good one)");
	ws::EconData econ;
	std::string error;
	CHECK(econ.Parse(ItemsGame(), error));

	const std::string truncated = ItemsGame().substr(0, ItemsGame().size() / 2);
	CHECK(!econ.Parse(truncated, error));
	CHECK(!error.empty());
	CHECK(econ.Item(23) != nullptr);
	CHECK(econ.items.size() == 7);
	CHECK(econ.paints.size() == 3);
	CHECK(econ.agents.size() == 2);

	error.clear();
	CHECK(!econ.Parse("\"items_game\" { \"other\" { \"x\" \"1\" } }", error)); // no items section
	CHECK(!error.empty());
	CHECK(econ.Item(7) != nullptr);

	error.clear();
	CHECK(!econ.Parse("", error));
	CHECK(econ.Item(507) != nullptr);

	// A successful parse replaces the whole snapshot: a document without paint kits empties them.
	const std::string minimal =
		"\"items_game\" { \"items\" { \"5\" { \"name\" \"weapon_test\" \"baseitem\" \"1\" } } }";
	error.clear();
	CHECK(econ.Parse(minimal, error));
	CHECK(econ.items.size() == 1);
	CHECK(econ.Item(5) != nullptr);
	CHECK(econ.paints.empty());
	CHECK(econ.agents.empty());
}

// The source-grounded excerpt: verbatim records of the installed items_game (see
// fixtures/game_excerpt_source.md for provenance, spans and hashes). Expected names/indices are the
// ones the source note lists; nothing here is asserted from outside that fixture.
void TestSourceExcerpt() {
	Section("source-grounded excerpt (installed game data)");
	const std::string& whole = GameExcerpt();
	const std::size_t lang = whole.rfind("\n\"lang\"");
	CHECK(lang != std::string::npos);
	if (lang == std::string::npos) {
		return;
	}
	const std::string itemsText = whole.substr(0, lang);
	const std::string langText = whole.substr(lang + 1); // begins at the "lang" line

	ws::EconData econ;
	std::string error;
	CHECK(econ.ParseLanguage("en", langText, error));
	if (!error.empty()) {
		std::printf("  ParseLanguage error: %s\n", error.c_str());
	}
	CHECK(econ.Parse(itemsText, error));
	if (!error.empty()) {
		std::printf("  Parse error: %s\n", error.c_str());
	}

	// def_index -> classname (source note: 23 = weapon_mp5sd, 33 = weapon_mp7, no 20 exists).
	CHECK(econ.items.size() == 13);
	CHECK(econ.Item(20) == nullptr);
	struct ExpectedItem {
		int index;
		const char* name;
	};
	const ExpectedItem expectedItems[] = {
		{7, "weapon_ak47"},		{9, "weapon_awp"},		 {16, "weapon_m4a1"},
		{23, "weapon_mp5sd"},	{33, "weapon_mp7"},		 {34, "weapon_mp9"},
		{42, "weapon_knife"},	{59, "weapon_knife_t"},	 {60, "weapon_m4a1_silencer"},
		{500, "weapon_bayonet"}, {507, "weapon_knife_karambit"}, {5027, "studded_bloodhound_gloves"},
		{5030, "sporty_gloves"},
	};
	for (const ExpectedItem& expected : expectedItems) {
		const ws::ItemDefinition* item = econ.Item(static_cast<uint16_t>(expected.index));
		CHECK(item != nullptr);
		if (item != nullptr) {
			CHECK(item->name == expected.name);
			CHECK(item->index == static_cast<uint16_t>(expected.index));
		}
	}
	// used_by_classes in the installed data spells the sides out.
	const ws::ItemDefinition* knife = econ.Item(42);
	CHECK(knife != nullptr && knife->team == 3); // counter-terrorists only
	const ws::ItemDefinition* knifeT = econ.Item(59);
	CHECK(knifeT != nullptr && knifeT->team == 2); // terrorists only
	const ws::ItemDefinition* karambit = econ.Item(507);
	CHECK(karambit != nullptr && karambit->team == 0); // both sides
	CHECK(karambit != nullptr &&
		  karambit->model == "weapons/models/knife/knife_karambit/weapon_knife_karambit.vmdl");
	const ws::ItemDefinition* gloves = econ.Item(5027);
	CHECK(gloves != nullptr &&
		  gloves->model == "agents/models/shared/arms/glove_bloodhound/glove_bloodhound.vmdl");

	// Agents: customplayertradable items are not weapon definitions.
	CHECK(econ.agents.size() == 2);
	CHECK(econ.Item(5105) == nullptr);
	const ws::EconDefinition* leet = econ.Agent(5105);
	CHECK(leet != nullptr && leet->model == "agents/models/tm_leet/tm_leet_variantg.vmdl");
	const ws::EconDefinition* fbi = econ.Agent(5305);
	CHECK(fbi != nullptr && fbi->model == "agents/models/ctm_fbi/ctm_fbi_variantf.vmdl");

	// Paint kits: 180/344/941 carry use_legacy_model, 106 does not.
	CHECK(econ.paints.size() == 7);
	CHECK(econ.Legacy(7, 180));
	CHECK(econ.Legacy(7, 344));
	CHECK(econ.Legacy(7, 941));
	CHECK(!econ.Legacy(7, 106));
	const auto defaultKit = econ.paints.find(0);
	CHECK(defaultKit != econ.paints.end() && defaultKit->second.name == "default");
	const auto ddpat = econ.paints.find(5);
	CHECK(ddpat != econ.paints.end() && ddpat->second.name == "hy_ddpat");
	const auto sporty = econ.paints.find(10018);
	CHECK(sporty != econ.paints.end() && sporty->second.name == "sporty_light_blue");

	// Sticker kits / keychains / music kits by their record names.
	CHECK(econ.stickers.size() == 3);
	CHECK(econ.stickers.count(1) == 1 && econ.stickers.at(1).name == "dh_gologo1");
	CHECK(econ.stickers.count(2) == 1 && econ.stickers.at(2).name == "dh_gologo1_holo");
	CHECK(econ.stickers.count(10369) == 1 && econ.stickers.at(10369).name == "cologne2026_team_vita");
	CHECK(econ.keychains.size() == 2);
	CHECK(econ.keychains.count(1) == 1 && econ.keychains.at(1).name == "kc_missinglink_ava");
	CHECK(econ.keychains.count(84) == 1 && econ.keychains.at(84).name == "kc_cologne2026");
	CHECK(econ.musicKits.size() == 3);
	CHECK(econ.musicKits.count(1) == 1 && econ.musicKits.at(1).name == "valve_cs2_01");
	CHECK(econ.musicKits.count(3) == 1 && econ.musicKits.at(3).name == "danielsadowski_01");
	CHECK(econ.musicKits.count(104) == 1 && econ.musicKits.at(104).name == "starjunk95_01");

	// Client loot lists: "[paint]classname" entries reduce to the paint name.
	const auto nuke = econ.lootLists.find("set_nuke_2_rare");
	CHECK(nuke != econ.lootLists.end());
	if (nuke != econ.lootLists.end()) {
		CHECK(Contains(nuke->second, "hy_blueprint_aqua"));
		CHECK(Contains(nuke->second, "am_circuitboard_aqua"));
		CHECK(Contains(nuke->second, "hy_ducts_blue"));
		CHECK(Contains(nuke->second, "hy_nuclear_skulls_redblue"));
	}
	const auto crate = econ.lootLists.find("crate_operation_ii_ancient");
	CHECK(crate != econ.lootLists.end() && Contains(crate->second, "cu_fireserpent_ak47_bravo") &&
		  Contains(crate->second, "am_scales_bravo"));
	const auto cobble = econ.lootLists.find("set_cobblestone_ancient");
	CHECK(cobble != econ.lootLists.end() && Contains(cobble->second, "cu_medieval_dragon_awp"));

	// Original English tokens, from the same excerpt.
	const ws::ItemDefinition* mp5sd = econ.Item(23);
	CHECK(mp5sd != nullptr && HasLanguage(mp5sd->localizedNames, "english", "MP5-SD"));
	const ws::ItemDefinition* bayonet = econ.Item(500);
	CHECK(bayonet != nullptr && HasLanguage(bayonet->localizedNames, "english", "Bayonet"));
	CHECK(karambit != nullptr && HasLanguage(karambit->localizedNames, "english", "Karambit"));
	const auto fireSerpent = econ.paints.find(180);
	CHECK(fireSerpent != econ.paints.end() &&
		  HasLanguage(fireSerpent->second.localizedNames, "english", "Fire Serpent"));
	CHECK(econ.stickers.count(10369) == 1 &&
		  HasLanguage(econ.stickers.at(10369).localizedNames, "english", "Vitality | Cologne 2026"));
	CHECK(econ.keychains.count(1) == 1 &&
		  HasLanguage(econ.keychains.at(1).localizedNames, "english", "Lil' Ava"));
	CHECK(econ.musicKits.count(1) == 1 &&
		  HasLanguage(econ.musicKits.at(1).localizedNames, "english", "Valve, Counter-Strike 2"));
	const ws::EconDefinition* leetAgent = econ.Agent(5105);
	CHECK(leetAgent != nullptr && HasLanguageContaining(leetAgent->localizedNames, "english", "Elite Crew"));
	const ws::EconDefinition* fbiAgent = econ.Agent(5305);
	CHECK(fbiAgent != nullptr && HasLanguageContaining(fbiAgent->localizedNames, "english", "FBI SWAT"));

	// The whole file (items_game followed by the sibling "lang" block) parses too: Parse descends
	// into the "items_game" wrapper when the buffer holds more than one top-level block.
	ws::EconData wholeFile;
	error.clear();
	CHECK(wholeFile.Parse(whole, error));
	CHECK(wholeFile.items.size() == 13);
}

void TestLoadFromGame() {
	Section("LoadFromGame (fake filesystem bindings)");
	InstallFakes();
	g_fakeFiles.clear();
	g_fakeList.clear();
	g_fakeFiles["scripts/items/items_game.txt"] = ItemsGame();
	g_fakeFiles["resource/csgo_english.txt"] = English();
	g_fakeFiles["resource/csgo_russian.txt"] = Russian();
	g_fakeFiles["resource/csgo_german.txt"] =
		"\"lang\" { \"Tokens\" { \"SFUI_WPNHUD_AK47\" \"AK-47 (DE)\" \"PaintKit_so_blue\" \"Blau\" } }";
	g_fakeList = {"GAME:resource/csgo_english.txt", "GAME:resource/csgo_russian.txt",
				  "resource/csgo_german.txt"};

	ws::EconData econ;
	std::string error;
	// ItemLanguages = []: every language Valve ships is loaded.
	CHECK(econ.LoadFromGame({}, error));
	if (!error.empty()) {
		std::printf("  LoadFromGame error: %s\n", error.c_str());
	}
	CHECK(econ.items.size() == 7);
	CHECK(g_lastListWildcard == "resource/csgo_*");
	CHECK(g_lastListPathId == "GAME");
	CHECK(g_lastReadPathId == "GAME");

	const ws::ItemDefinition* ak47 = econ.Item(7);
	CHECK(ak47 != nullptr);
	if (ak47 != nullptr) {
		CHECK(HasLanguage(ak47->localizedNames, "english", "AK-47"));
		CHECK(HasLanguage(ak47->localizedNames, "russian", "АК-47"));
		CHECK(HasLanguage(ak47->localizedNames, "german", "AK-47 (DE)"));
	}

	// A configured subset keeps exactly the requested language plus english (the fallback source).
	error.clear();
	CHECK(econ.LoadFromGame({"ru"}, error));
	const ws::ItemDefinition* ru = econ.Item(7);
	CHECK(ru != nullptr);
	if (ru != nullptr) {
		CHECK(HasLanguage(ru->localizedNames, "russian", "АК-47"));
		CHECK(ru->localizedNames.count("english") == 1);
		CHECK(ru->localizedNames.count("german") == 0);
	}
	const ws::ItemDefinition* ssg = econ.Item(40);
	CHECK(ssg != nullptr && HasLanguage(ssg->localizedNames, "russian", "SSG 08")); // english fallback

	error.clear();
	CHECK(econ.LoadFromGame({"en"}, error));
	const ws::ItemDefinition* en = econ.Item(7);
	CHECK(en != nullptr && en->localizedNames.count("russian") == 0);

	// loadLanguages = false (what the plugin does: no in-game menus show item names): the items are
	// parsed, no language file is listed or read, and the names stay empty.
	error.clear();
	g_lastListWildcard.clear();
	CHECK(econ.LoadFromGame({}, error, false));
	CHECK(econ.items.size() == 7);
	CHECK(g_lastListWildcard.empty());
	const ws::ItemDefinition* bare = econ.Item(7);
	CHECK(bare != nullptr && bare->localizedNames.empty());

	// items_game missing: failure, and the previous snapshot survives.
	error.clear();
	g_fakeFiles.erase("scripts/items/items_game.txt");
	CHECK(!econ.LoadFromGame({}, error));
	CHECK(!error.empty());
	CHECK(econ.Item(23) != nullptr);
	CHECK(econ.Item(23)->name == "weapon_mp5sd");

	// No s2sdk bindings at all: an error, never a crash.
	g_fakeFiles["scripts/items/items_game.txt"] = ItemsGame();
	UninstallFakes();
	error.clear();
	CHECK(!econ.LoadFromGame({}, error));
	CHECK(!error.empty());
	CHECK(econ.Item(23) != nullptr);
}

} // namespace

int main() {
	std::printf("WeaponSkins econ tests\n");
	TestItems();
	TestPaintKits();
	TestAgents();
	TestStickersKeychainsMusic();
	TestLootLists();
	TestLanguagesReal();
	TestLanguageErrors();
	TestAtomicity();
	TestSourceExcerpt();
	TestLoadFromGame();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
