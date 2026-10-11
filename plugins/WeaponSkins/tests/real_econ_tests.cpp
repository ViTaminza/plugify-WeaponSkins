// Integration regression fixture extracted from the installed CS2 VPK (see provenance file).
#include "econ.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#ifndef WS_FIXTURES_DIR
#define WS_FIXTURES_DIR "tests/fixtures"
#endif
namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
std::string Read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}
int main() {
    ws::EconData econ;
    std::string error;
    const std::string text = Read(std::string(WS_FIXTURES_DIR) + "/game_excerpt.txt");
    Check(!text.empty(), "installed VPK excerpt exists");
    const auto language = text.find("\"lang\"");
    Check(language != std::string::npos, "excerpt has real language tokens");
    if (language != std::string::npos)
        Check(econ.ParseLanguage("en", std::string_view(text).substr(language), error), "parse extracted English");
    Check(econ.Parse(text, error), "parse extracted items schema");
    if (!error.empty()) std::cerr << error << '\n';
    Check(econ.Item(23) && econ.Item(23)->name == "weapon_mp5sd", "MP5 defindex comes from item name, not prefab class");
    Check(econ.Item(33) && econ.Item(33)->name == "weapon_mp7", "MP7 defindex is 33");
    Check(!econ.Item(20), "obsolete defindex 20 absent");
    Check(econ.Item(7) && econ.Item(7)->localizedNames.contains("english") &&
          econ.Item(7)->localizedNames.at("english") == "AK-47", "item localized English label");
    Check(econ.Item(500) && econ.Item(5030), "knife and gloves prefab inheritance");
    Check(econ.Item(7) && econ.Item(7)->team == 2, "real AK47 T ownership uses engine team 2");
    Check(econ.Item(16) && econ.Item(16)->team == 3, "real M4A1 CT ownership uses engine team 3");
    Check(econ.paints.contains(941) && econ.paints.contains(106), "merge late repeated paint blocks");
    Check(econ.Legacy(7, 180) && econ.Legacy(9, 344) && !econ.Legacy(60, 106), "legacy paint model from real fields");
    Check(econ.Agent(5105) && econ.Agent(5105)->model == "agents/models/tm_leet/tm_leet_variantg.vmdl", "T agent inherited model");
    Check(econ.Agent(5305) && econ.Agent(5305)->model == "agents/models/ctm_fbi/ctm_fbi_variantf.vmdl", "CT agent inherited model");
    Check(econ.Agent(5105) && econ.Agent(5105)->localizedNames.contains("english"), "agent localization resolved");
    Check(econ.stickers.contains(10369), "merge late sticker definitions");
    Check(econ.keychains.contains(1) && econ.keychains.contains(84), "merge all keychain blocks");
    Check(econ.musicKits.contains(3) && econ.musicKits.contains(104), "merge all music definitions");
    const auto loot = econ.lootLists.find("set_cobblestone_ancient");
    Check(loot != econ.lootLists.end() &&
          std::find(loot->second.begin(), loot->second.end(), "cu_medieval_dragon_awp") != loot->second.end(),
          "loot list actual Dragon Lore entry");
#ifdef WS_FULL_ITEMS
    const std::string full = Read(WS_FULL_ITEMS);
    if (!full.empty()) {
        Check(econ.Parse(full, error), "parse complete current 8MB items_game");
        std::cout << "Full game counts: items=" << econ.items.size() << " paints=" << econ.paints.size()
                  << " agents=" << econ.agents.size() << " stickers=" << econ.stickers.size()
                  << " keychains=" << econ.keychains.size() << " music=" << econ.musicKits.size() << '\n';
        Check(econ.items.size() > 50 && econ.paints.size() > 1000 && econ.agents.size() > 30,
              "full current game catalog parsed");
        Check(econ.stickers.size() > 10000 && econ.keychains.size() > 100 && econ.musicKits.size() > 90,
              "modern repeated sections preserved in full schema");
    }
#endif
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
