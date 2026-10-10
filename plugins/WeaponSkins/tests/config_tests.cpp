// Tests for plugins/WeaponSkins/src/config.cpp (the config.hpp contract).
//
//   * ParseConfig/LoadConfig against the saved non-secret config.toml (or the
//     byte-identical fixture when that copy is not readable) and against inline cases:
//     comments vs '#' inside quoted strings, escapes/unicode, literal strings, multiline and
//     empty arrays, dotted keys, CRLF, custom messages and permission keys;
//   * every invalid case (duplicate key, wrong type, malformed syntax, unsupported backend)
//     must fail with a line-numbered error that never echoes raw file content;
//   * ParseDatabaseConfig/LoadDatabaseConfig against a dummy database.jsonc fixture (object
//     format, URI format, "pass"/"password", validation and error redaction — a password must
//     never appear in an error string);
//   * ParseDatabasesCfg/LoadDatabasesCfg and LoadPluginDatabase against a dummy databases.cfg
//     fixture (the standard's KeyValues section, defaults, the sqlite driver, and the source
//     order: databases.cfg wins, the legacy database.jsonc is only the transition fallback);
//   * ReplaceToken for "{url}"/"{seconds}";
//   * Translations: load, language normalization/fallback, {0} formatting with literal-brace
//     safety, plus the real translation files when they are readable.
//
// The fixtures live in plugins/WeaponSkins/tests/fixtures. The absolute defaults below let the
// test run from build/tests without extra arguments; CMake may override them with
// WS_TEST_FIXTURES / WS_TEST_PROD_CONFIG / WS_TEST_PROD_TRANSLATIONS.

#include "config.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(WS_TEST_FIXTURES) && defined(WS_FIXTURES_DIR)
#define WS_TEST_FIXTURES WS_FIXTURES_DIR
#endif
#if !defined(WS_TEST_FIXTURES) && defined(WEAPONSKINS_TEST_DIR)
#define WS_TEST_FIXTURES WEAPONSKINS_TEST_DIR
#endif
#ifndef WS_TEST_FIXTURES
#define WS_TEST_FIXTURES "tests/fixtures"
#endif
#ifndef WS_TEST_PROD_CONFIG
#define WS_TEST_PROD_CONFIG ""
#endif
#ifndef WS_TEST_PROD_TRANSLATIONS
#define WS_TEST_PROD_TRANSLATIONS ""
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
    g_section = name;
    std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                              \
    do {                                                                                         \
        ++g_checks;                                                                              \
        if (!(expr)) {                                                                           \
            ++g_failures;                                                                        \
            std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),           \
                        g_section.data(), __FILE__, __LINE__, #expr);                            \
        }                                                                                        \
    } while (false)

#define CHECK_EQ_INT(actual, expected)                                                           \
    do {                                                                                         \
        ++g_checks;                                                                              \
        const auto a_ = static_cast<long long>(actual);                                          \
        const long long e_ = static_cast<long long>(expected);                                   \
        if (a_ != e_) {                                                                          \
            ++g_failures;                                                                        \
            std::printf("FAIL [%.*s] %s:%d: %s == %lld, expected %lld\n",                        \
                        static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__, \
                        #actual, a_, e_);                                                        \
        }                                                                                        \
    } while (false)

#define CHECK_EQ_STR(actual, expected)                                                           \
    do {                                                                                         \
        ++g_checks;                                                                              \
        const std::string a_ = (actual);                                                         \
        const std::string e_ = (expected);                                                       \
        if (a_ != e_) {                                                                          \
            ++g_failures;                                                                        \
            std::printf("FAIL [%.*s] %s:%d: %s\n  actual:   \"%s\"\n  expected: \"%s\"\n",       \
                        static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__, \
                        #actual, a_.c_str(), e_.c_str());                                        \
        }                                                                                        \
    } while (false)

#define CHECK_CONTAINS(haystack, needle)                                                         \
    do {                                                                                         \
        ++g_checks;                                                                              \
        const std::string h_ = std::string(haystack);                                            \
        const std::string n_ = std::string(needle);                                              \
        if (h_.find(n_) == std::string::npos) {                                                  \
            ++g_failures;                                                                        \
            std::printf("FAIL [%.*s] %s:%d: \"%s\" does not contain \"%s\"\n",                   \
                        static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__, \
                        h_.c_str(), n_.c_str());                                                 \
        }                                                                                        \
    } while (false)

#define CHECK_NOT_CONTAINS(haystack, needle)                                                     \
    do {                                                                                         \
        ++g_checks;                                                                              \
        const std::string h_ = std::string(haystack);                                            \
        const std::string n_ = std::string(needle);                                              \
        if (h_.find(n_) != std::string::npos) {                                                  \
            ++g_failures;                                                                        \
            std::printf("FAIL [%.*s] %s:%d: \"%s\" unexpectedly contains \"%s\"\n",               \
                        static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__, \
                        h_.c_str(), n_.c_str());                                                 \
        }                                                                                        \
    } while (false)

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::filesystem::path FirstExisting(
    std::initializer_list<std::filesystem::path> candidates) {
    for (const std::filesystem::path& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

// The saved files, found either through the WS_TEST_PROD_* defines or relative to the
// plugin directory (WS_SOURCE_DIR, provided by the build).
std::filesystem::path ProdConfigPath() {
    return FirstExisting({WS_TEST_PROD_CONFIG,
#ifdef WS_SOURCE_DIR
                          std::filesystem::path(WS_SOURCE_DIR) / "configs" / "config.toml"
#endif
    });
}

std::filesystem::path ProdTranslationsPath() {
    return FirstExisting({WS_TEST_PROD_TRANSLATIONS,
#ifdef WS_SOURCE_DIR
                          std::filesystem::path(WS_SOURCE_DIR) / "resources" / "translations"
#endif
    });
}

// A parse is expected to fail: error must be non-empty, carry the line number and, when asked,
// contain/avoid the given substrings.
void ExpectParseFailure(std::string_view what, std::string_view text,
                        std::string_view expectedSubstring) {
    ws::Config config;
    std::string error;
    const bool ok = ws::ParseConfig(text, config, error);
    ++g_checks;
    if (ok) {
        ++g_failures;
        std::printf("FAIL [%.*s] %.*s: parsed successfully, expected an error\n",
                    static_cast<int>(g_section.size()), g_section.data(),
                    static_cast<int>(what.size()), what.data());
        return;
    }
    ++g_checks;
    if (error.find("line ") == std::string::npos) {
        ++g_failures;
        std::printf("FAIL [%.*s] %.*s: error lacks a line number: \"%s\"\n",
                    static_cast<int>(g_section.size()), g_section.data(),
                    static_cast<int>(what.size()), what.data(), error.c_str());
    }
    CHECK_CONTAINS(error, expectedSubstring);
}

} // namespace

// ---------------------------------------------------------------------------------------------

static void TestDefaults() {
    Section("defaults");

    ws::Config config;
    std::string error;
    CHECK(ws::ParseConfig("", config, error));
    CHECK_EQ_STR(error, "");
    CHECK_EQ_STR(config.storageBackend, "pisex");
    CHECK_EQ_STR(config.inventoryBackend, "hook");
    CHECK(!config.musicKitMvpAnthem);
    CHECK(!config.syncOnJoin);
    CHECK(config.itemLanguages.empty());

    CHECK_EQ_STR(config.pisex.connectionName, "skins");
    CHECK_EQ_STR(config.pisex.tablePrefix, "sc_");
    CHECK_EQ_INT(config.pisex.serverId, 1);
    CHECK(config.pisex.autoCreatePlayer);
    CHECK(config.pisex.useExtTable);
    CHECK(config.pisex.lazyLoadPerPlayer);
    CHECK(config.pisex.pinsEnabled);

    CHECK(config.commands.redirect);
    CHECK_EQ_STR(config.commands.siteUrl, "example.com");
    CHECK_EQ_INT(config.commands.aliases.size(), 9);
    CHECK_EQ_STR(config.commands.syncCommand, "wp");
    CHECK_EQ_INT(config.commands.cooldownSeconds, 20);
    CHECK_CONTAINS(config.commands.redirectMessage, "{url}");
    CHECK_CONTAINS(config.commands.cooldownMessage, "{seconds}");
    CHECK(config.commands.startedMessage.empty()); // the original printed nothing when a reload started
    CHECK(!config.commands.doneMessage.empty());
    CHECK(!config.commands.emptyMessage.empty());
    CHECK(!config.commands.errorMessage.empty());

    CHECK(config.permissions.weapons.empty());
    CHECK(config.permissions.knives.empty());
    CHECK(config.permissions.gloves.empty());
    CHECK(config.permissions.stickers.empty());
    CHECK(config.permissions.keychains.empty());
    CHECK(config.permissions.agents.empty());
    CHECK(config.permissions.music.empty());
}

static void TestProductionConfig() {
    Section("production config");

    std::filesystem::path path = ProdConfigPath();
    if (path.empty()) {
        path = std::filesystem::path(WS_TEST_FIXTURES) / "config.toml";
        std::printf("   (the saved production config is not readable; using the fixture %s)\n",
                    path.c_str());
    }
    CHECK(std::filesystem::exists(path));

    ws::Config config;
    std::string error;
    const std::string text = ReadAll(path);
    CHECK(ws::ParseConfig(text, config, error));
    CHECK_EQ_STR(error, "");

    CHECK(!config.musicKitMvpAnthem); // MusicKitMvpAnthem = false: round_end_sounds owns the anthem
    CHECK_EQ_STR(config.storageBackend, "pisex");
    CHECK_EQ_STR(config.inventoryBackend, "hook");
    CHECK(!config.syncOnJoin);
    CHECK(config.itemLanguages.empty());

    CHECK_EQ_STR(config.pisex.connectionName, "skins");
    CHECK_EQ_STR(config.pisex.tablePrefix, "sc_");
    CHECK_EQ_INT(config.pisex.serverId, 1);
    CHECK(config.pisex.autoCreatePlayer);
    CHECK(config.pisex.useExtTable);
    CHECK(config.pisex.lazyLoadPerPlayer);
    CHECK(config.pisex.pinsEnabled);

    CHECK(config.commands.redirect);
    CHECK_EQ_STR(config.commands.siteUrl, "example.com");
    CHECK_EQ_INT(config.commands.cooldownSeconds, 20);
    CHECK_EQ_STR(config.commands.syncCommand, "wp");

    const std::vector<std::string> expectedAliases = {"ws",     "skins",   "gloves",  "knife",
                                                      "agents", "music",   "stickers", "keychains",
                                                      "pins"};
    CHECK_EQ_INT(config.commands.aliases.size(), expectedAliases.size());
    for (std::size_t i = 0; i < config.commands.aliases.size() && i < expectedAliases.size(); ++i) {
        CHECK_EQ_STR(config.commands.aliases[i], expectedAliases[i]);
    }
    // Production deliberately does not register "menu" (other plugins own !menu).
    for (const std::string& alias : config.commands.aliases) {
        CHECK(alias != "menu");
    }

    // The file has no message overrides: the model defaults - the English texts of the original
    // Pisex skinchanger's phrases - apply (the Russian ones come from translations/ru.jsonc).
    CHECK_CONTAINS(config.commands.redirectMessage, "[SkinChanger]");
    CHECK_CONTAINS(config.commands.redirectMessage, "{command}");
    CHECK_CONTAINS(config.commands.doneMessage, "successfully");
    CHECK_CONTAINS(config.commands.emptyMessage, "{url}");
    CHECK_CONTAINS(config.commands.cooldownMessage, "{seconds}");
    CHECK_CONTAINS(config.commands.errorMessage, "[SkinChanger]");

    CHECK(config.permissions.weapons.empty() && config.permissions.music.empty());
}

static void TestCommentsStringsArrays() {
    Section("comments, strings, arrays");

    const std::string text = R"TOML(
# a whole-line comment before any table
[Main] # comment after the table header
StorageBackend = "pisex"        # inline comment after a value
InventoryUpdateBackend = 'hook' # literal string
MusicKitMvpAnthem = true # bool
SyncFromDatabaseWhenPlayerJoin = false
ItemLanguages = []              # empty array stays empty

[Main.Pisex]
ServerId = 2                    # integer with comment

[Main.Commands]
SiteUrl = "example.com/#anchor"     # '#' inside the string is not a comment
SyncCommand = "wp"
SyncCooldownSeconds = 15
MenuAliases = [
    "ws", # first alias
    "skins",
    "pins",        # trailing comma follows
]
SyncStartedMessage = "a \"b\" c\nСкин \U0001F600"
SyncDoneMessage = 'literal \n stays raw'
)TOML";

    ws::Config config;
    std::string error;
    CHECK(ws::ParseConfig(text, config, error));
    CHECK_EQ_STR(error, "");

    CHECK(config.musicKitMvpAnthem);
    CHECK_EQ_STR(config.storageBackend, "pisex");
    CHECK_EQ_STR(config.inventoryBackend, "hook");
    CHECK(config.itemLanguages.empty());
    CHECK_EQ_INT(config.pisex.serverId, 2);
    CHECK_EQ_STR(config.commands.siteUrl, "example.com/#anchor");
    CHECK_EQ_INT(config.commands.cooldownSeconds, 15);
    CHECK_EQ_INT(config.commands.aliases.size(), 3);
    if (config.commands.aliases.size() == 3) {
        CHECK_EQ_STR(config.commands.aliases[0], "ws");
        CHECK_EQ_STR(config.commands.aliases[1], "skins");
        CHECK_EQ_STR(config.commands.aliases[2], "pins");
    }
    // Escapes: \" \n \uXXXX and \UXXXXXXXX (4-byte UTF-8).
    CHECK_EQ_STR(config.commands.startedMessage, "a \"b\" c\nСкин \U0001F600");
    // A literal string keeps backslashes untouched.
    CHECK_EQ_STR(config.commands.doneMessage, "literal \\n stays raw");

    // Array of strings across lines; unicode in array elements.
    ws::Config langs;
    CHECK(ws::ParseConfig("[Main]\nItemLanguages = [\n  \"ru\",\n  \"en\",\n  \"zh-CN\",\n]", langs,
                          error));
    CHECK_EQ_INT(langs.itemLanguages.size(), 3);
    if (langs.itemLanguages.size() == 3) {
        CHECK_EQ_STR(langs.itemLanguages[0], "ru");
        CHECK_EQ_STR(langs.itemLanguages[2], "zh-CN");
    }

    // CRLF line endings (a Windows-authored config).
    ws::Config crlf;
    CHECK(ws::ParseConfig("[Main]\r\nMusicKitMvpAnthem = true\r\nItemLanguages = [\"ru\"]\r\n",
                          crlf, error));
    CHECK(crlf.musicKitMvpAnthem);
    CHECK_EQ_INT(crlf.itemLanguages.size(), 1);

    // Dotted keys without a table header, and a UTF-8 BOM.
    ws::Config dotted;
    CHECK(ws::ParseConfig("\xEF\xBB\xBFMain.Commands.SyncCooldownSeconds = 7\n"
                          "Main.Pisex.PinsEnabled = false\n",
                          dotted, error));
    CHECK_EQ_INT(dotted.commands.cooldownSeconds, 7);
    CHECK(!dotted.pisex.pinsEnabled);

    // Surrogate-pair escape decodes to the same UTF-8 as \U0001F600.
    ws::Config pair;
    CHECK(ws::ParseConfig("[Main.Commands]\nSyncStartedMessage = \"\\uD83D\\uDE00\"\n", pair,
                          error));
    CHECK_EQ_STR(pair.commands.startedMessage, "\U0001F600");

    // Real TOML semantics (and the C# binder's): a bare top-level key is NOT "Main.<key>", so a
    // file that forgets its [Main] header parses fine and every option keeps its default — it
    // must not be treated as a section key.
    ws::Config bare;
    CHECK(ws::ParseConfig("StorageBackend = \"sqlite\"\nInventoryUpdateBackend = \"poll\"\n"
                          "MusicKitMvpAnthem = true\nItemLanguages = [\"ru\"]\n",
                          bare, error));
    CHECK_EQ_STR(bare.storageBackend, "pisex");
    CHECK_EQ_STR(bare.inventoryBackend, "hook");
    CHECK(!bare.musicKitMvpAnthem);
    CHECK(bare.itemLanguages.empty());

    // A single-quoted (literal) key is valid TOML too.
    ws::Config quotedKey;
    CHECK(ws::ParseConfig("[\"Main\"]\n'StorageBackend' = \"pisex\"\n", quotedKey, error));
    CHECK_EQ_STR(quotedKey.storageBackend, "pisex");
}

static void TestPermissionsAndMessages() {
    Section("permission keys and custom messages");

    const std::string text = R"TOML(
[Main.ItemPermissions]
WeaponSkins = "skins.use"
KnifeSkins = "skins.knife"
GloveSkins = ""
Stickers = "skins.sticker"
Keychains = "skins.keychain"
Agents = "skins.agent"
MusicKits = "skins.music"

[Main.Commands]
RedirectMessage = "Скины и ножи на {url}!"
SyncStartedMessage = "Загружаю..."
SyncDoneMessage = "Готово: {url}"
SyncEmptyMessage = "Пусто на {url}"
SyncCooldownMessage = "Жди {seconds} с."
SyncErrorMessage = "Ошибка"
)TOML";

    ws::Config config;
    std::string error;
    CHECK(ws::ParseConfig(text, config, error));
    CHECK_EQ_STR(error, "");

    CHECK_EQ_STR(config.permissions.weapons, "skins.use");
    CHECK_EQ_STR(config.permissions.knives, "skins.knife");
    CHECK(config.permissions.gloves.empty());
    CHECK_EQ_STR(config.permissions.stickers, "skins.sticker");
    CHECK_EQ_STR(config.permissions.keychains, "skins.keychain");
    CHECK_EQ_STR(config.permissions.agents, "skins.agent");
    CHECK_EQ_STR(config.permissions.music, "skins.music");

    CHECK_EQ_STR(config.commands.redirectMessage, "Скины и ножи на {url}!");
    CHECK_EQ_STR(config.commands.startedMessage, "Загружаю...");
    CHECK_EQ_STR(config.commands.doneMessage, "Готово: {url}");
    CHECK_EQ_STR(config.commands.emptyMessage, "Пусто на {url}");
    CHECK_EQ_STR(config.commands.cooldownMessage, "Жди {seconds} с.");
    CHECK_EQ_STR(config.commands.errorMessage, "Ошибка");
    // Untouched keys keep their defaults.
    CHECK_EQ_STR(config.commands.siteUrl, "example.com");
    CHECK_EQ_INT(config.commands.cooldownSeconds, 20);
}

static void TestInvalidConfigs() {
    Section("invalid configurations");

    ExpectParseFailure("duplicate key", "[Main]\nMusicKitMvpAnthem = true\nMusicKitMvpAnthem = "
                                       "false\n",
                       "duplicate key 'Main.MusicKitMvpAnthem'");
    {
        // The duplicate error names the second line.
        ws::Config config;
        std::string error;
        CHECK(!ws::ParseConfig("[Main]\nMusicKitMvpAnthem = true\nMusicKitMvpAnthem = false\n",
                               config, error));
        CHECK_CONTAINS(error, "line 3");
    }
    ExpectParseFailure("duplicate dotted keys",
                       "Main.Pisex.ServerId = 1\nMain.Pisex.ServerId = 2\n",
                       "duplicate key 'Main.Pisex.ServerId'");

    ExpectParseFailure("bool key gets a string", "[Main]\nMusicKitMvpAnthem = \"yes\"\n",
                       "expects a boolean");
    ExpectParseFailure("bool key gets an integer", "[Main]\nMusicKitMvpAnthem = 1\n",
                       "expects a boolean");
    ExpectParseFailure("int key gets a string", "[Main.Pisex]\nServerId = \"1\"\n",
                       "expects an integer");
    ExpectParseFailure("string key gets an integer", "[Main.Commands]\nSiteUrl = 42\n",
                       "expects a string");
    ExpectParseFailure("array key gets a scalar", "[Main.Commands]\nMenuAliases = \"ws\"\n",
                       "expects an array of strings");
    ExpectParseFailure("array of integers", "[Main.Commands]\nMenuAliases = [1, 2]\n",
                       "expects an array of strings");

    ExpectParseFailure("missing '='", "[Main]\nMusicKitMvpAnthem true\n", "expected '='");
    ExpectParseFailure("unterminated string",
                       "[Main.Commands]\nSyncStartedMessage = \"SECRET_MARKER\n",
                       "unterminated string");
    ExpectParseFailure("unterminated array", "[Main.Commands]\nMenuAliases = [\"ws\"\n",
                       "unterminated array");
    ExpectParseFailure("invalid escape", "[Main.Commands]\nSyncStartedMessage = \"a\\qb\"\n",
                       "invalid escape");
    ExpectParseFailure("dangling backslash", "[Main.Commands]\nSyncStartedMessage = \"a\\\"\n",
                       "unterminated string");
    ExpectParseFailure("unsupported bare value", "[Main]\nMusicKitMvpAnthem = maybe\n",
                       "unsupported value");
    ExpectParseFailure("floating point value", "[Main.Pisex]\nServerId = 1.5\n",
                       "unexpected characters");
    ExpectParseFailure("trailing characters", "[Main.Commands]\nSiteUrl = \"x\" y\n",
                       "unexpected characters");
    ExpectParseFailure("malformed table header", "[Main\nMusicKitMvpAnthem = true\n",
                       "table header");
    ExpectParseFailure("empty table name", "[]\n", "expected a key");

    ExpectParseFailure("unsupported storage backend", "[Main]\nStorageBackend = \"sqlite\"\n",
                       "StorageBackend");
    {
        ws::Config config;
        std::string error;
        CHECK(!ws::ParseConfig("[Main]\nStorageBackend = \"sqlite\"\n", config, error));
        CHECK_CONTAINS(error, "only 'pisex'");
        CHECK_CONTAINS(error, "line 2");
    }
    ExpectParseFailure("unsupported inventory backend",
                       "[Main]\nInventoryUpdateBackend = \"poll\"\n", "InventoryUpdateBackend");
    ExpectParseFailure("backend of the wrong type", "[Main]\nStorageBackend = 1\n",
                       "expects a string");

    ExpectParseFailure("integer out of the 64-bit range",
                       "[Main.Commands]\nSyncCooldownSeconds = 99999999999999999999\n",
                       "out of range");
    ExpectParseFailure("integer out of the 32-bit range",
                       "[Main.Commands]\nSyncCooldownSeconds = 3000000000\n", "32-bit");

    // Raw content must never leak through an error message.
    {
        ws::Config config;
        std::string error;
        CHECK(!ws::ParseConfig("[Main.Commands]\nSyncStartedMessage = \"SECRET_MARKER\",\n",
                               config, error));
        CHECK_NOT_CONTAINS(error, "SECRET_MARKER");
    }

    // A failed parse must not touch the caller's config.
    {
        ws::Config config;
        config.commands.siteUrl = "keepme";
        std::string error;
        CHECK(!ws::ParseConfig("[Main.Commands]\nSiteUrl = 42\n", config, error));
        CHECK_EQ_STR(config.commands.siteUrl, "keepme");
    }
}

static void TestReplaceToken() {
    Section("ReplaceToken");

    CHECK_EQ_STR(ws::ReplaceToken("go to {url} now {url}", "url", "example.com"),
                 "go to example.com now example.com");
    CHECK_EQ_STR(ws::ReplaceToken("go to {url}", "{url}", "example.com"), "go to example.com");
    CHECK_EQ_STR(ws::ReplaceToken("wait {seconds} s", "seconds", "5"), "wait 5 s");
    CHECK_EQ_STR(ws::ReplaceToken("no token here", "url", "x"), "no token here");
    // Empty token is a no-op (and cannot loop).
    CHECK_EQ_STR(ws::ReplaceToken("{url}", "", "x"), "{url}");
    // A value containing the placeholder must not be re-substituted.
    CHECK_EQ_STR(ws::ReplaceToken("{url}", "url", "{url}"), "{url}");
    // "{url2}" is a different token.
    CHECK_EQ_STR(ws::ReplaceToken("100% {url2} {url}", "url", "X"), "100% {url2} X");
    // Replacement works on the Russian default text used by the plugin.
    const std::string message = "[Скины] Настрой скины на сайте {url}, потом !wp.";
    CHECK_EQ_STR(ws::ReplaceToken(message, "url", "example.com"),
                 "[Скины] Настрой скины на сайте example.com, потом !wp.");
}

// The translation files the plugin ships (resources/translations): the !wp chat phrases exist in
// Russian and English, and a client language without its own file falls back to English.
static void TestShippedSyncPhrases() {
    Section("translations (shipped sync phrases)");

    const std::filesystem::path dir =
        std::filesystem::path(WS_TEST_FIXTURES) / ".." / ".." / "resources" / "translations";
    ws::Translations translations;
    std::string error;
    CHECK(translations.Load(dir, error));
    for (const char* key : {"sync.redirect", "sync.done", "sync.empty", "sync.cooldown", "sync.error"}) {
        CHECK_CONTAINS(translations.Get("russian", key), "[SkinChanger]");
        CHECK_CONTAINS(translations.Get("english", key), "[SkinChanger]");
    }
    CHECK_CONTAINS(translations.Get("russian", "sync.done"), "успешно");
    CHECK_CONTAINS(translations.Get("russian", "sync.cooldown"), "{seconds}");
    CHECK_CONTAINS(translations.Get("russian", "sync.redirect"), "{command}");
    CHECK_CONTAINS(translations.Get("german", "sync.done"), "successfully"); // no de file -> en
}

static void TestTranslationsFixtures() {
    Section("translations (fixtures)");

    const std::filesystem::path dir = std::filesystem::path(WS_TEST_FIXTURES) / "translations";
    ws::Translations translations;
    std::string error;
    CHECK(translations.Load(dir, error));
    CHECK_EQ_STR(error, "");

    CHECK_EQ_STR(translations.Get("en", "menu.title"), "Weapon Skins");
    CHECK_EQ_STR(translations.Get("ru", "menu.title"), "Скины");
    CHECK_EQ_STR(translations.Get("ru", "unicode.name"), "Скины «АК-47»");

    // Language names, codes and case normalization.
    CHECK_EQ_STR(translations.Get("english", "menu.title"), "Weapon Skins");
    CHECK_EQ_STR(translations.Get("russian", "menu.title"), "Скины");
    CHECK_EQ_STR(translations.Get("Russian", "menu.title"), "Скины");
    CHECK_EQ_STR(translations.Get("zh-CN", "menu.title"), "武器皮肤");
    CHECK_EQ_STR(translations.Get("zh-cn", "menu.title"), "武器皮肤");
    CHECK_EQ_STR(translations.Get("schinese", "menu.title"), "武器皮肤");
    CHECK_EQ_STR(translations.Get("", "menu.title"), "Weapon Skins"); // empty -> en

    // Fallback: unknown language -> en; missing phrase in ru -> en; unknown key -> the key.
    CHECK_EQ_STR(translations.Get("de", "menu.title"), "Weapon Skins");
    CHECK_EQ_STR(translations.Get("zh-Hans", "menu.title"), "Weapon Skins"); // subtag zh, then en
    CHECK_EQ_STR(translations.Get("ru", "menu.unset"), "Unset");
    CHECK_EQ_STR(translations.Get("en", "menu.missing.key"), "menu.missing.key");

    // Keys are matched case-insensitively (the C# localizer used OrdinalIgnoreCase).
    CHECK_EQ_STR(translations.Get("en", "case.key.mixedcase"), "Mixed case key");

    // {0}-style formatting; a missing argument stays literal; literal braces are safe.
    CHECK_EQ_STR(translations.Get("en", "menu.skinproperties.setsticker", {"2", "AK-47"}),
                 "Sticker 2 (AK-47)");
    CHECK_EQ_STR(translations.Get("en", "menu.skinproperties.setsticker", {"2"}),
                 "Sticker 2 ({1})");
    CHECK_EQ_STR(translations.Get("en", "greet.hello", {"Иван"}), "Hello, Иван!");
    CHECK_EQ_STR(translations.Get("ru", "greet.hello", {"Иван"}), "Привет, Иван!");
    CHECK_EQ_STR(translations.Get("en", "greet.hello"), "Hello, {0}!");
    CHECK_EQ_STR(translations.Get("en", "braces.literal", {"x"}), "100% {not_a_placeholder} safe");

    // A failed load leaves no stale state behind, and a later successful load works.
    ws::Translations cleared;
    CHECK(!cleared.Load(dir.parent_path() / "no-such-translations", error));
    CHECK_CONTAINS(error, "not found");
    CHECK_EQ_STR(cleared.Get("en", "menu.title"), "menu.title"); // nothing loaded
    CHECK(cleared.Load(dir, error));
    CHECK_EQ_STR(cleared.Get("en", "menu.title"), "Weapon Skins");
}

static void TestTranslationsBrokenFiles() {
    Section("translations (broken files)");

    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path() / "ws_config_tests_translations";
    std::error_code ec;
    std::filesystem::remove_all(tempRoot, ec);
    std::filesystem::create_directories(tempRoot, ec);
    CHECK(!ec);

    {
        std::ofstream(tempRoot / "en.jsonc") << R"({
    "greet.hello": "Hello, {0}!",
    "menu.title": "From the good file"
})";
        std::ofstream(tempRoot / "broken.jsonc") << "{ \"a\": ";
    }
    ws::Translations translations;
    std::string error;
    // Best effort: the good file is kept, the caller still gets false and the file name.
    CHECK(!translations.Load(tempRoot, error));
    CHECK_CONTAINS(error, "broken.jsonc");
    CHECK_CONTAINS(error, "failed to parse");
    CHECK_EQ_STR(translations.Get("en", "menu.title"), "From the good file");

    // A value that is not a scalar is an error too.
    std::filesystem::remove(tempRoot / "broken.jsonc", ec);
    {
        std::ofstream(tempRoot / "bad.jsonc") << R"({ "menu.title": { "nested": true } })";
    }
    CHECK(!translations.Load(tempRoot, error));
    CHECK_CONTAINS(error, "not a string");
    CHECK_CONTAINS(error, "bad.jsonc");

    // An empty directory is an error (packaging broke), not silently empty translations.
    const std::filesystem::path emptyDir = tempRoot / "empty";
    std::filesystem::create_directories(emptyDir, ec);
    ws::Translations empty;
    CHECK(!empty.Load(emptyDir, error));
    CHECK_CONTAINS(error, "no translation files");

    std::filesystem::remove_all(tempRoot, ec);
}

static void TestTranslationsProductionFiles() {
    Section("translations (production files)");

    const std::filesystem::path dir = ProdTranslationsPath();
    if (dir.empty() || !std::filesystem::is_directory(dir)) {
        std::printf("   SKIP: the saved production translations are not readable\n");
        return;
    }
    ws::Translations translations;
    std::string error;
    CHECK(translations.Load(dir, error));
    CHECK_EQ_STR(error, "");
    CHECK_EQ_STR(translations.Get("en", "menu.title"), "Weapon Skins");
    CHECK_EQ_STR(translations.Get("en", "menu.unset"), "Unset");
    // lt/tr are fully translated in production, so those phrases must NOT fall back to English.
    CHECK_EQ_STR(translations.Get("lt", "menu.title"), "Ginklų skinai");
    CHECK_EQ_STR(translations.Get("lt", "menu.unset"), "Nuimti");
    CHECK_EQ_STR(translations.Get("tr", "menu.title"), "Silah Kaplamaları");
    CHECK_EQ_STR(translations.Get("tr", "menu.unset"), "Kaldır");
    CHECK(translations.Get("zh-CN", "menu.title") != "menu.title");
    CHECK(translations.Get("zh-TW", "menu.title") != "menu.title");
    // There is no ru file in production: the English fallback applies.
    CHECK_EQ_STR(translations.Get("ru", "menu.title"), "Weapon Skins");
    // The sticker phrase format is the same {0}/{1} shape the original used.
    CHECK_EQ_STR(translations.Get("en", "menu.skinproperties.setsticker", {"3", "Titan"}),
                 "Sticker 3 (Titan)");
}

static void TestDatabaseFixture() {
    Section("database config (fixture)");

    const std::filesystem::path path = std::filesystem::path(WS_TEST_FIXTURES) / "database.jsonc";
    std::string error;

    const auto skins = ws::LoadDatabaseConfig(path, "skins", error);
    CHECK(skins.has_value());
    CHECK_EQ_STR(error, "");
    if (skins) {
        CHECK(skins->driver == plgdb::Driver::MySQL);
        CHECK_EQ_STR(skins->host, "db.example.invalid");
        CHECK_EQ_INT(skins->port, 3307);
        CHECK_EQ_STR(skins->database, "mirrors_dummy");
        CHECK_EQ_STR(skins->user, "ws_dummy");
        CHECK_EQ_STR(skins->password, "ws_dummy_pw_1");
        CHECK_EQ_STR(skins->charset, "utf8mb4");
        CHECK_EQ_INT(skins->connectTimeoutSec, 25);
    }

    // Connection names are matched case-insensitively (the C# service used OrdinalIgnoreCase).
    const auto upper = ws::LoadDatabaseConfig(path, "SKINS", error);
    CHECK(upper.has_value());
    if (upper && skins) {
        CHECK_EQ_STR(upper->host, skins->host);
    }

    // URI form with percent-encoded userinfo; mariadb driver; "password" key; defaults.
    const auto uri = ws::LoadDatabaseConfig(path, "skins_uri", error);
    CHECK(uri.has_value());
    if (uri) {
        CHECK_EQ_STR(uri->host, "10.0.0.9");
        CHECK_EQ_INT(uri->port, 3308);
        CHECK_EQ_STR(uri->database, "skins_db");
        CHECK_EQ_STR(uri->user, "ws_uri");
        CHECK_EQ_STR(uri->password, "du@mmy"); // "du%40mm%79"
    }

    const auto legacy = ws::LoadDatabaseConfig(path, "skins_legacy_password", error);
    CHECK(legacy.has_value());
    if (legacy) {
        CHECK_EQ_STR(legacy->password, "ws_dummy_pw_2");
        CHECK_EQ_INT(legacy->port, 3306);
        CHECK_EQ_INT(legacy->connectTimeoutSec, 30); // default timeout
        CHECK_EQ_STR(legacy->charset, "utf8mb4");
    }

    const auto emptyPass = ws::LoadDatabaseConfig(path, "skins_empty_pass", error);
    CHECK(emptyPass.has_value());
    if (emptyPass) {
        CHECK(emptyPass->password.empty());
    }

    const auto defaultUri = ws::LoadDatabaseConfig(path, "default", error);
    CHECK(defaultUri.has_value());
    if (defaultUri) {
        CHECK_EQ_STR(defaultUri->host, "db.example.invalid");
        CHECK_EQ_STR(defaultUri->user, "ws_dummy");
        CHECK_EQ_STR(defaultUri->password, "ws_dummy_pw_default");
        CHECK_EQ_STR(defaultUri->database, "example_db");
    }
}

static void TestDatabaseErrors() {
    Section("database config (errors, redaction)");

    const std::filesystem::path path = std::filesystem::path(WS_TEST_FIXTURES) / "database.jsonc";
    std::string error;

    // Unsupported drivers are rejected; the error must not leak the password next to them.
    const auto postgres = ws::LoadDatabaseConfig(path, "postgres_bad", error);
    CHECK(!postgres.has_value());
    CHECK_CONTAINS(error, "unsupported driver");
    CHECK_CONTAINS(error, "postgres_bad");
    CHECK_NOT_CONTAINS(error, "ws_dummy_pw_3");

    const auto sqlite = ws::LoadDatabaseConfig(path, "sqlite_bad", error);
    CHECK(!sqlite.has_value());
    CHECK_CONTAINS(error, "unsupported driver");

    const auto missing = ws::LoadDatabaseConfig(path, "no_such_connection", error);
    CHECK(!missing.has_value());
    CHECK_CONTAINS(error, "no_such_connection");
    CHECK_NOT_CONTAINS(error, "ws_dummy");

    // Malformed JSONC: only the byte offset may be reported — the wrong backend example
    // (SQLite/Postgres) survives in the SwiftlyS2 template, so the parser must stay redacted
    // even when the input holds a password around the error position.
    {
        const std::string broken = R"({
    "connections": {
        "skins": { "driver": "mysql", "host": "h", "user": "u", "pass": "SECRET_PW_X", }
    }
})";
        const auto result = ws::ParseDatabaseConfig(broken, "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "parse error");
        CHECK_CONTAINS(error, "byte");
        CHECK_NOT_CONTAINS(error, "SECRET_PW_X");
    }

    // Structure validation.
    {
        const auto result = ws::ParseDatabaseConfig("{}", "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "connections");
    }
    {
        const auto result = ws::ParseDatabaseConfig(R"({"connections": {"skins": 42}})", "skins",
                                                    error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "object or a connection URI");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "database": "x", "user": "u"}}})",
            "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "'host' is missing");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "host": "h", "user": "u"}}})",
            "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "'database' is missing");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "host": "h", "database": "d"}}})",
            "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "'user' is missing");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "host": "h", "database": "d", "user": "u", "port": 70000}}})",
            "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "port");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "host": "h", "database": "d", "user": "u", "port": 0}}})",
            "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "1..65535");
    }

    // Ports as digit strings and timeout clamping ([5, 30], the C# behaviour).
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "host": "h", "database": "d",
                "user": "u", "pass": "p", "port": "3310", "timeout": 1}}})",
            "skins", error);
        CHECK(result.has_value());
        if (result) {
            CHECK_EQ_INT(result->port, 3310);
            CHECK_EQ_INT(result->connectTimeoutSec, 5);
        }
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": {"driver": "mysql", "host": "h", "database": "d",
                "user": "u", "timeout": 999}}})",
            "skins", error);
        CHECK(result.has_value());
        if (result) {
            CHECK_EQ_INT(result->connectTimeoutSec, 30);
        }
    }

    // URI validation, redacted too.
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": "mysql://u:SECRET_PW_Y@h"}})", "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "database name");
        CHECK_NOT_CONTAINS(error, "SECRET_PW_Y");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": "mysql://u:SECRET_PW_Y@/db"}})", "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "no host");
        CHECK_NOT_CONTAINS(error, "SECRET_PW_Y");
    }
    {
        const auto result = ws::ParseDatabaseConfig(
            R"({"connections": {"skins": "mysql://u:SECRET_PW_Y@h:abc/db"}})", "skins", error);
        CHECK(!result.has_value());
        CHECK_CONTAINS(error, "port");
        CHECK_NOT_CONTAINS(error, "SECRET_PW_Y");
    }

    // A missing file reports the path, nothing else.
    const auto absent = ws::LoadDatabaseConfig(
        std::filesystem::path(WS_TEST_FIXTURES) / "no-such-file.jsonc", "skins", error);
    CHECK(!absent.has_value());
    CHECK_CONTAINS(error, "cannot open");
}

static void TestDatabasesCfg() {
    Section("databases.cfg (standard section)");

    std::string error;

    // The fixture is the standard template shape with dummy values (section 'skins', the
    // connection name of Main.Pisex.ConnectionName).
    const std::filesystem::path path = std::filesystem::path(WS_TEST_FIXTURES) / "databases.cfg";
    const auto skins = ws::LoadDatabasesCfg(path, "skins", error);
    CHECK(skins.has_value());
    CHECK_EQ_STR(error, "");
    if (skins) {
        CHECK(skins->driver == plgdb::Driver::MySQL);
        CHECK_EQ_STR(skins->host, "cfg.example.invalid");
        CHECK_EQ_INT(skins->port, 3308);
        CHECK_EQ_STR(skins->user, "ws_cfg_user");
        CHECK_EQ_STR(skins->password, "ws_cfg_pw_1");
        CHECK_EQ_STR(skins->database, "cfg_dummy_db");
    }

    // The section name is the connection name: "other" reads its own block (sqlite driver).
    const auto other = ws::LoadDatabasesCfg(path, "other", error);
    CHECK(other.has_value());
    if (other) {
        CHECK(other->driver == plgdb::Driver::SQLite);
        CHECK_EQ_STR(other->sqlitePath, "other.sqlite3");
    }

    // KeyValues FindKey is case-insensitive, like Valve's (and the jsonc reader).
    const auto upper = ws::LoadDatabasesCfg(path, "SKINS", error);
    CHECK(upper.has_value());
    if (upper && skins) {
        CHECK_EQ_STR(upper->host, skins->host);
    }

    // Defaults and the sqlite fallback name, from inline text: the shipped template minus the
    // optional keys.
    {
        const auto minimal = ws::ParseDatabasesCfg(R"(
"Databases"
{
	"skins"
	{
		"database"	"mirrors"
	}
})", "skins", error);
        CHECK(minimal.has_value());
        if (minimal) {
            CHECK(minimal->driver == plgdb::Driver::MySQL);
            CHECK_EQ_STR(minimal->host, "127.0.0.1");
            CHECK_EQ_INT(minimal->port, 3306);
            CHECK(minimal->user.empty());
            CHECK(minimal->password.empty());
            CHECK_EQ_STR(minimal->database, "mirrors");
        }
    }
    {
        const auto sqlite = ws::ParseDatabasesCfg(R"(
"Databases" { "skins" { "driver" "sqlite" "database" "s" } })", "skins", error);
        CHECK(sqlite.has_value());
        if (sqlite) {
            CHECK(sqlite->driver == plgdb::Driver::SQLite);
            CHECK_EQ_STR(sqlite->sqlitePath, "WeaponSkins.sqlite3");
        }
    }

    // The port is clamped into the valid range (admin_system/levels_ranks do the same).
    {
        const auto low = ws::ParseDatabasesCfg(
            R"("Databases" { "skins" { "port" "0" } })", "skins", error);
        CHECK(low.has_value());
        if (low) {
            CHECK_EQ_INT(low->port, 1);
        }
        const auto high = ws::ParseDatabasesCfg(
            R"("Databases" { "skins" { "port" "70000" } })", "skins", error);
        CHECK(high.has_value());
        if (high) {
            CHECK_EQ_INT(high->port, 65535);
        }
    }

    // A file without the section and a malformed file both fail with a credential-free error.
    const auto noSection = ws::LoadDatabasesCfg(
        std::filesystem::path(WS_TEST_FIXTURES) / "databases_no_skins.cfg", "skins", error);
    CHECK(!noSection.has_value());
    CHECK_CONTAINS(error, "no 'skins' section");
    {
        const auto malformed = ws::ParseDatabasesCfg(R"("Databases" { "skins" { )", "skins", error);
        CHECK(!malformed.has_value());
        CHECK_CONTAINS(error, "invalid KeyValues");
    }
    const auto absent = ws::LoadDatabasesCfg(
        std::filesystem::path(WS_TEST_FIXTURES) / "no-such-databases.cfg", "skins", error);
    CHECK(!absent.has_value());
    CHECK_CONTAINS(error, "cannot open");
}

static void TestPluginDatabase() {
    Section("plugin database source (databases.cfg wins, legacy fallback)");

    const std::string cfgPath =
        (std::filesystem::path(WS_TEST_FIXTURES) / "databases.cfg").string();
    const std::string legacyPath =
        (std::filesystem::path(WS_TEST_FIXTURES) / "database.jsonc").string();
    const std::string missingCfg =
        (std::filesystem::path(WS_TEST_FIXTURES) / "no-such-databases.cfg").string();
    const std::string missingJsonc =
        (std::filesystem::path(WS_TEST_FIXTURES) / "no-such-file.jsonc").string();
    const std::string noSkinsCfg =
        (std::filesystem::path(WS_TEST_FIXTURES) / "databases_no_skins.cfg").string();

    std::string error;
    bool usedLegacy = true;

    // databases.cfg present: its values win over the legacy file, and no warning is due.
    const auto standard = ws::LoadPluginDatabase(cfgPath, legacyPath, "skins", usedLegacy, error);
    CHECK(standard.has_value());
    CHECK(!usedLegacy);
    CHECK_EQ_STR(error, "");
    if (standard) {
        CHECK_EQ_STR(standard->host, "cfg.example.invalid");
        CHECK_EQ_INT(standard->port, 3308);
        CHECK_EQ_STR(standard->user, "ws_cfg_user");
        CHECK_EQ_STR(standard->password, "ws_cfg_pw_1");
    }

    // databases.cfg missing: the legacy SwiftlyS2 database.jsonc still works; usedLegacy is what
    // makes the runtime log the single "move it to databases.cfg" warning.
    const auto fromLegacy = ws::LoadPluginDatabase(missingCfg, legacyPath, "skins", usedLegacy, error);
    CHECK(fromLegacy.has_value());
    CHECK(usedLegacy);
    CHECK_EQ_STR(error, "");
    if (fromLegacy) {
        CHECK_EQ_STR(fromLegacy->host, "db.example.invalid");
        CHECK_EQ_INT(fromLegacy->port, 3307);
        CHECK_EQ_STR(fromLegacy->user, "ws_dummy");
        CHECK_EQ_STR(fromLegacy->password, "ws_dummy_pw_1");
        CHECK_EQ_STR(fromLegacy->database, "mirrors_dummy");
    }

    // databases.cfg exists but has no 'skins' section: same fallback, same warning.
    const auto fromLegacy2 = ws::LoadPluginDatabase(noSkinsCfg, legacyPath, "skins", usedLegacy, error);
    CHECK(fromLegacy2.has_value());
    CHECK(usedLegacy);
    if (fromLegacy2) {
        CHECK_EQ_STR(fromLegacy2->host, "db.example.invalid");
    }

    // Neither source usable: today's failure (the plugin runs without storage) and the error
    // names both files, never a credential.
    const auto none = ws::LoadPluginDatabase(missingCfg, missingJsonc, "skins", usedLegacy, error);
    CHECK(!none.has_value());
    CHECK(!usedLegacy);
    CHECK_CONTAINS(error, "databases.cfg");
    CHECK_CONTAINS(error, "database.jsonc");
    CHECK_NOT_CONTAINS(error, "ws_dummy");
    CHECK_NOT_CONTAINS(error, "ws_cfg_pw");

    // The section follows Main.Pisex.ConnectionName; it is a parameter, not a constant.
    const auto other = ws::LoadPluginDatabase(cfgPath, legacyPath, "other", usedLegacy, error);
    CHECK(other.has_value());
    CHECK(!usedLegacy);
    if (other) {
        CHECK(other->driver == plgdb::Driver::SQLite);
        CHECK_EQ_STR(other->sqlitePath, "other.sqlite3");
    }
}

int main() {
    std::printf("weaponskins config_tests\n");

    TestDefaults();
    TestProductionConfig();
    TestCommentsStringsArrays();
    TestPermissionsAndMessages();
    TestInvalidConfigs();
    TestReplaceToken();
    TestTranslationsFixtures();
    TestShippedSyncPhrases();
    TestTranslationsBrokenFiles();
    TestTranslationsProductionFiles();
    TestDatabaseFixture();
    TestDatabaseErrors();
    TestDatabasesCfg();
    TestPluginDatabase();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
