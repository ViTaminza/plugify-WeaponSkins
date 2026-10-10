#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ws {
constexpr int kStatTrakQuality = 9;
constexpr int kMaxPlayers = 64;
inline int TeamIndex(int team) noexcept { return team == 2 ? 0 : team == 3 ? 1 : -1; }
inline bool IsKnife(int def) noexcept { return def == 42 || def == 59 || (def >= 500 && def < 600); }
inline bool IsGlove(int def) noexcept { return def >= 5000 && def <= 65535; }
struct Sticker {
    int id = 0;
    int schema = 1337;
    float x = 0, y = 0, wear = 0, scale = 1, rotation = 0;
    bool operator==(const Sticker&) const = default;
};
struct Keychain {
    int id = 0, seed = 0;
    float x = 0, y = 0, z = 0;
    bool operator==(const Keychain&) const = default;
};
struct Skin {
    uint16_t definition = 0;
    int paint = 0, seed = 0, quality = 0, stattrak = 0;
    float wear = 0;
    std::string nametag;
    std::array<Sticker, 6> stickers{};
    Keychain keychain{};
    bool operator==(const Skin&) const = default;
};
struct TeamItems {
    std::unordered_map<uint16_t, Skin> weapons;
    std::optional<Skin> knife, glove;
    int agent = 0, pin = 0;
};
struct Snapshot {
    uint64_t steamId = 0;
    std::array<TeamItems, 2> teams{};
    int music = 0;
    bool Empty() const {
        if (music) return false;
        for (const auto& t : teams)
            if (!t.weapons.empty() || t.knife || t.glove || t.agent || t.pin) return false;
        return true;
    }
};
struct PisexConfig {
    std::string connectionName = "skins", tablePrefix = "sc_";
    int serverId = 1;
    bool autoCreatePlayer = true, useExtTable = true, lazyLoadPerPlayer = true, pinsEnabled = true;
};
struct CommandConfig {
    bool redirect = true;
    std::string siteUrl = "example.com", syncCommand = "wp";
    std::vector<std::string> aliases = {"ws", "skins", "gloves", "knife", "agents", "music", "stickers", "keychains", "pins"};
    int cooldownSeconds = 20;
    // The English texts of the original Pisex skinchanger's phrases (Ws_Knife_Command(_2),
    // Success_Reload, Cooldown_Reload). A client whose language has a translation file entry
    // (resources/translations/ru.jsonc for "russian") gets that instead; s2sdk reads every
    // client's cl_language when it connects. "\\n" is a chat line break (pisexcompat Colorize),
    // and an empty text prints nothing (the original printed nothing when a reload started).
    std::string redirectMessage = " {GREEN}[SkinChanger]{DEFAULT} To set skins, go to the website: {GREEN}{url}{DEFAULT}\\n {GREEN}[SkinChanger]{DEFAULT} To update data without rejoining, use {GREEN}{command}{DEFAULT}";
    std::string startedMessage;
    std::string doneMessage = " {GREEN}[SkinChanger]{DEFAULT} You {GREEN}successfully{DEFAULT} updated the data";
    std::string emptyMessage = " {GREEN}[SkinChanger]{DEFAULT} Nothing is set up for you on {GREEN}{url}{DEFAULT} yet";
    std::string cooldownMessage = " {GREEN}[SkinChanger]{DEFAULT} You cannot refresh so frequently, try again in {RED}{seconds} s.{DEFAULT}";
    std::string errorMessage = " {GREEN}[SkinChanger]{DEFAULT} Could not update the data, try again later";
};
struct PermissionConfig {
    std::string weapons, knives, gloves, stickers, keychains, agents, music;
};
struct Config {
    std::string storageBackend = "pisex", inventoryBackend = "hook";
    bool musicKitMvpAnthem = false, syncOnJoin = false;
    PisexConfig pisex{};
    CommandConfig commands{};
    PermissionConfig permissions{};
    std::vector<std::string> itemLanguages;
};
} // namespace ws
