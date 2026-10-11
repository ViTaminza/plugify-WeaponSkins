#pragma once
#include "models.hpp"
#include <filesystem>
#include <string_view>
#include <plgdb/db.hpp>
namespace ws {
bool ParseConfig(std::string_view text, Config& config, std::string& error);
bool LoadConfig(const std::filesystem::path& path, Config& config, std::string& error);
std::optional<plgdb::Config> ParseDatabaseConfig(std::string_view jsonc, std::string_view connection, std::string& error);
std::optional<plgdb::Config> LoadDatabaseConfig(const std::filesystem::path& path, std::string_view connection, std::string& error);
// The standard's connection file (docs/PLUGIN_STANDARD.md §4): the `connection` section ("skins")
// of databases.cfg in KeyValues format, with the vip/admin_system keys (driver, host, port,
// database, user, pass, sqlite_path). LoadDatabasesCfg reads the file, ParseDatabasesCfg takes
// the text; both are credential-free in their error strings.
std::optional<plgdb::Config> ParseDatabasesCfg(std::string_view text, std::string_view connection, std::string& error);
std::optional<plgdb::Config> LoadDatabasesCfg(const std::filesystem::path& path, std::string_view connection, std::string& error);
// The transition-period loader: databases.cfg first, the legacy SwiftlyS2 database.jsonc second.
// usedLegacy is set when the connection came from the legacy file (the caller logs the single
// "move it to databases.cfg" warning then); error explains a failure of both sources.
std::optional<plgdb::Config> LoadPluginDatabase(std::string_view databasesCfgPath, std::string_view legacyJsoncPath,
                                                std::string_view connection, bool& usedLegacy, std::string& error);
std::string ReplaceToken(std::string text, std::string_view token, std::string_view value);
class Translations {
public:
    bool Load(const std::filesystem::path& directory, std::string& error);
    std::string Get(std::string_view language, std::string_view key, const std::vector<std::string>& args = {}) const;
private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> languages_;
};
} // namespace ws
