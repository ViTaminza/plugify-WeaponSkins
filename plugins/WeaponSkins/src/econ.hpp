#pragma once
#include "models.hpp"
#include <string_view>
namespace ws {
struct ItemDefinition {
    uint16_t index = 0;
    std::string name, model;
    int team = 0;
    std::unordered_map<std::string, std::string> localizedNames;
};
struct PaintKit {
    int index = 0;
    std::string name;
    bool legacy = false;
    std::unordered_map<std::string, std::string> localizedNames;
};
struct EconDefinition {
    int index = 0;
    std::string name, model;
    std::unordered_map<std::string, std::string> localizedNames;
};
class EconData {
public:
    bool Parse(std::string_view itemsGame, std::string& error);
    bool ParseLanguage(std::string_view language, std::string_view text, std::string& error);
    // loadLanguages = false skips the resource/csgo_*.txt token tables: localizedNames then stay
    // empty. They only feed the reference's in-game menus, which this port does not have, and all
    // 29 languages are ~158 MB of text to read and parse at plugin start (~8 s on the game thread).
    bool LoadFromGame(const std::vector<std::string>& languages, std::string& error, bool loadLanguages = true);
    const ItemDefinition* Item(uint16_t index) const;
    const EconDefinition* Agent(int index) const;
    bool Legacy(uint16_t definition, int paint) const;
    std::unordered_map<uint16_t, ItemDefinition> items;
    std::unordered_map<int, PaintKit> paints;
    std::unordered_map<int, EconDefinition> agents, stickers, keychains, musicKits;
    std::unordered_map<std::string, std::vector<std::string>> lootLists;
private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> tokens_;
};
} // namespace ws
