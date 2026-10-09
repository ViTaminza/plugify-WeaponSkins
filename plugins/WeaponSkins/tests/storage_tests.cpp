// Tests for plugins/WeaponSkins/src/storage.cpp - the Pisex (sc_*) storage backend on plgdb.
//
// The parser helpers (storage_detail.hpp) are tested without a database; everything else runs
// against the real MariaDB that test.sh provides through MYSQL_HOST/MYSQL_PORT/MYSQL_USER/
// MYSQL_PASSWORD/MYSQL_DATABASE. The test never connects anywhere else and creates its own
// tables under a unique prefix, which it drops again at the end.
//
// The storage contract under test:
//   * statements are queued from the game thread and their callbacks only run from Poll();
//   * sc_servers: the configured row, otherwise the first row, an empty table is an error;
//   * a player is auto-created on READ when AutoCreatePlayer is on;
//   * one query for the skins, one for sc_skins_ext and one for sc_items (no per-row N+1);
//   * SaveStatTrak writes only stattrack_count as a delta and never inserts a row;
//   * the plugin creates sc_skins_ext and nothing else; an ext failure keeps the base data
//     and is retried later.

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include <plgdb/db.hpp>

#include "storage.hpp"
#include "storage_detail.hpp"

// The fixtures directory is provided by the build; the same names the sibling tests use.
#if !defined(WS_TEST_FIXTURES) && defined(WS_FIXTURES_DIR)
#define WS_TEST_FIXTURES WS_FIXTURES_DIR
#endif
#if !defined(WS_TEST_FIXTURES) && defined(WEAPONSKINS_TEST_DIR)
#define WS_TEST_FIXTURES WEAPONSKINS_TEST_DIR
#endif
#ifndef WS_TEST_FIXTURES
#define WS_TEST_FIXTURES ""
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";
bool g_inPoll = false;

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),             \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_, e_);                                                                   \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

#define CHECK_QUERY_OK(result)                                                                     \
	do {                                                                                           \
		++g_checks;                                                                                \
		const plgdb::Result& r_ = (result);                                                        \
		if (!r_.Ok()) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: query failed: %s\n",                                   \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						r_.Error().c_str());                                                       \
		}                                                                                          \
	} while (false)

bool Near(float actual, float expected, float epsilon = 0.0001F) {
	return std::fabs(actual - expected) <= epsilon;
}

// --- database helpers -----------------------------------------------------------------------

std::unique_ptr<plgdb::Database> OpenDb(const plgdb::Config& config) {
	return plgdb::Database::Create(config);
}

plgdb::Result Exec(plgdb::Database& db, std::string sql, plgdb::Params params = {}) {
	return db.QuerySync(std::move(sql), std::move(params));
}

void MustExec(plgdb::Database& db, const std::string& sql, plgdb::Params params = {}) {
	const plgdb::Result result = Exec(db, sql, std::move(params));
	if (!result.Ok()) {
		std::printf("FAIL [%.*s] setup query failed: %s\n  %s\n", static_cast<int>(g_section.size()),
					g_section.data(), result.Error().c_str(), sql.c_str());
		++g_checks;
		++g_failures;
	}
}

int64_t ScalarInt(plgdb::Database& db, const std::string& sql, plgdb::Params params = {}) {
	const plgdb::Result result = Exec(db, sql, std::move(params));
	if (!result.Ok() || result.RowCount() == 0 || result.ColumnCount() == 0) {
		return -1;
	}
	return result.GetInt(0, 0, -1);
}

std::string ScalarString(plgdb::Database& db, const std::string& sql, plgdb::Params params = {}) {
	const plgdb::Result result = Exec(db, sql, std::move(params));
	if (!result.Ok() || result.RowCount() == 0 || result.ColumnCount() == 0) {
		return "<query failed>";
	}
	return result.GetString(0, 0, {});
}

bool TableExists(plgdb::Database& db, const std::string& name) {
	return ScalarInt(db, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = DATABASE() "
						 "AND table_name = ?",
					 plgdb::Params{name}) > 0;
}

int64_t ColumnCount(plgdb::Database& db, const std::string& name) {
	return ScalarInt(db, "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() "
						 "AND table_name = ?",
					 plgdb::Params{name});
}

// --- fixture ---------------------------------------------------------------------------------

std::string FixturePath() {
	std::vector<std::string> candidates;
	const std::string defined = WS_TEST_FIXTURES;
	if (!defined.empty()) {
		candidates.push_back(defined + "/pisex-schema.sql");
	}
#ifdef WS_SOURCE_DIR
	candidates.emplace_back(std::string(WS_SOURCE_DIR) + "/tests/fixtures/pisex-schema.sql");
#endif
	// Fallbacks for a hand-run binary: from build/tests (test.sh's cwd) and from the tree root.
	candidates.emplace_back("fixtures/pisex-schema.sql");
	candidates.emplace_back("../../plugins/WeaponSkins/tests/fixtures/pisex-schema.sql");
	candidates.emplace_back("../plugins/WeaponSkins/tests/fixtures/pisex-schema.sql");
	candidates.emplace_back("plugins/WeaponSkins/tests/fixtures/pisex-schema.sql");
	for (const std::string& candidate : candidates) {
		std::ifstream probe(candidate);
		if (probe.good()) {
			return candidate;
		}
	}
	return {};
}

std::string ReadFile(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Splits the fixture on ';' at line ends; comment lines (--) are skipped.
std::vector<std::string> SplitStatements(const std::string& text) {
	std::vector<std::string> statements;
	std::string current;
	size_t position = 0;
	while (position <= text.size()) {
		const size_t end = text.find('\n', position);
		std::string line = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
		position = end == std::string::npos ? text.size() + 1 : end + 1;
		const size_t first = line.find_first_not_of(" \t\r");
		if (first != std::string::npos && line.compare(first, 2, "--") == 0) {
			continue;
		}
		current += line;
		current += '\n';
		const size_t last = current.find_last_not_of(" \t\r\n");
		if (last != std::string::npos && current[last] == ';') {
			statements.push_back(current.substr(0, last));
			current.clear();
		}
	}
	const size_t last = current.find_last_not_of(" \t\r\n");
	if (last != std::string::npos) {
		statements.push_back(current.substr(0, last + 1));
	}
	return statements;
}

bool ReplaceAll(std::string& text, std::string_view token, std::string_view value) {
	bool replaced = false;
	for (size_t position = text.find(token); position != std::string::npos; position = text.find(token, position)) {
		text.replace(position, token.size(), value);
		position += value.size();
		replaced = true;
	}
	return replaced;
}

// --- fixture scaffolding ----------------------------------------------------------------------

std::string NewPrefix() {
	static int counter = 0;
	return "wst" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(counter++) + "_";
}

bool LoadMySqlConfig(plgdb::Config& config, std::string& error) {
	const char* host = std::getenv("MYSQL_HOST");
	if (!host || !*host) {
		return false;
	}
	config.driver = plgdb::Driver::MySQL;
	config.host = host;
	config.user = std::getenv("MYSQL_USER") ? std::getenv("MYSQL_USER") : "";
	config.password = std::getenv("MYSQL_PASSWORD") ? std::getenv("MYSQL_PASSWORD") : "";
	config.database = std::getenv("MYSQL_DATABASE") ? std::getenv("MYSQL_DATABASE") : "";
	const char* port = std::getenv("MYSQL_PORT");
	if (port && *port) {
		try {
			const int value = std::stoi(port);
			if (value < 1 || value > 65535) {
				error = "MYSQL_PORT out of range";
				return false;
			}
			config.port = static_cast<uint16_t>(value);
		} catch (const std::exception&) {
			error = "MYSQL_PORT is not a number";
			return false;
		}
	}
	return true;
}

// One test case: its own tables under a unique prefix, a seeding connection and the Storage.
struct Fixture {
	std::string prefix;
	plgdb::Config config;
	std::unique_ptr<plgdb::Database> seed;
	std::unique_ptr<ws::Storage> storage;

	bool Init(const plgdb::Config& base, const ws::PisexConfig& pisexConfig) {
		prefix = NewPrefix();
		config = base;
		seed = OpenDb(config);
		if (!seed) {
			std::printf("FAIL database connection failed\n");
			++g_checks;
			++g_failures;
			return false;
		}
		const std::string path = FixturePath();
		if (path.empty()) {
			std::printf("FAIL pisex-schema.sql not found\n");
			++g_checks;
			++g_failures;
			return false;
		}
		std::string sql = ReadFile(path);
		if (!ReplaceAll(sql, "{{PREFIX}}", prefix)) {
			std::printf("FAIL fixture has no {{PREFIX}} token\n");
			++g_checks;
			++g_failures;
			return false;
		}
		for (const std::string& statement : SplitStatements(sql)) {
			MustExec(*seed, statement);
		}
		ws::PisexConfig used = pisexConfig;
		used.tablePrefix = prefix;
		storage = std::make_unique<ws::Storage>(used, OpenDb(config));
		return true;
	}

	~Fixture() {
		storage.reset();
		if (!seed) {
			return;
		}
		for (const char* table : {"skins", "items", "player", "servers", "skins_ext"}) {
			Exec(*seed, "DROP TABLE IF EXISTS " + prefix + table, {});
		}
		Exec(*seed, "DROP VIEW IF EXISTS " + prefix + "skins_ext", {});
	}

	std::string Table(std::string_view name) const { return prefix + std::string(name); }

	bool TableExists(std::string_view name) const { return ::TableExists(*seed, Table(name)); }

	int64_t SeedServer(int64_t id) {
		MustExec(*seed, "INSERT INTO " + Table("servers") + " (id, name) VALUES (?, 'Test')", plgdb::Params{id});
		return id;
	}

	int64_t SeedPlayer(uint64_t steamId) {
		MustExec(*seed, "INSERT INTO " + Table("player") + " (name, steamid) VALUES ('Tester', ?)",
				 plgdb::Params{std::to_string(steamId)});
		return ScalarInt(*seed, "SELECT id FROM " + Table("player") + " WHERE steamid = ?",
						 plgdb::Params{std::to_string(steamId)});
	}

	void SeedSkin(int64_t playerId, int64_t serverId, int64_t team, int64_t definition, const char* skin,
				  int64_t stattrack, int64_t count, const char* stickers, const char* tag, const char* keychain) {
		plgdb::Params params;
		params.emplace_back(playerId);
		params.emplace_back(serverId);
		params.emplace_back(team);
		params.emplace_back(definition);
		params.emplace_back(std::string(skin));
		params.emplace_back(stattrack);
		params.emplace_back(count);
		params.emplace_back(stickers ? std::string(stickers) : std::string());
		params.emplace_back(tag ? std::string(tag) : std::string());
		if (keychain) {
			params.emplace_back(std::string(keychain));
		} else {
			params.emplace_back(std::monostate{});
		}
		MustExec(*seed,
				 "INSERT INTO " + Table("skins") +
					 " (player_id, server_id, team, weapon_index, skin, stattrack, stattrack_count, stickers, tag, keychain)"
					 " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
				 params);
	}

	void SeedItems(int64_t playerId, int64_t serverId, int64_t team, int64_t agent, int64_t music, int64_t coin,
				   int64_t knife, int64_t glove) {
		MustExec(*seed,
				 "INSERT INTO " + Table("items") +
					 " (player_id, server_id, team, agent, music, coin, knife, glove) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
				 plgdb::Params{playerId, serverId, team, agent, music, coin, knife, glove});
	}
};

// --- polling helpers --------------------------------------------------------------------------

size_t PollOnce(ws::Storage& storage) {
	g_inPoll = true;
	const size_t ran = storage.Poll();
	g_inPoll = false;
	return ran;
}

bool WaitFor(ws::Storage& storage, const std::function<bool()>& done, int timeoutMs = 10000) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;) {
		PollOnce(storage);
		if (done()) {
			return true;
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
}

void Settle(ws::Storage& storage) {
	for (int round = 0; round < 200 && storage.Pending() > 0; ++round) {
		PollOnce(storage);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (int round = 0; round < 3; ++round) {
		PollOnce(storage);
	}
}

struct LoadOutcome {
	bool done = false;
	bool sawInPoll = false;
	ws::Snapshot snapshot;
	std::string error;
};

LoadOutcome Load(ws::Storage& storage, uint64_t steamId, bool expectData = true) {
	LoadOutcome outcome;
	storage.LoadPlayer(steamId, [&](ws::Snapshot snapshot, std::string error) {
		outcome.done = true;
		outcome.sawInPoll = g_inPoll;
		outcome.snapshot = std::move(snapshot);
		outcome.error = std::move(error);
	});
	CHECK(!outcome.done); // callbacks never run inside LoadPlayer
	if (!WaitFor(storage, [&] { return outcome.done; })) {
		std::printf("FAIL [%.*s] load timed out\n", static_cast<int>(g_section.size()), g_section.data());
		++g_checks;
		++g_failures;
		return outcome;
	}
	CHECK(outcome.sawInPoll);
	if (expectData) {
		CHECK(outcome.error.empty());
	}
	return outcome;
}

struct SaveOutcome {
	bool done = false;
	bool sawInPoll = false;
	bool ok = false;
	std::string error;
};

SaveOutcome Save(ws::Storage& storage, uint64_t steamId, int team, uint16_t definition, int delta) {
	SaveOutcome outcome;
	storage.SaveStatTrak(steamId, team, definition, delta,
						 [&](bool ok, std::string error) {
							 outcome.done = true;
							 outcome.sawInPoll = g_inPoll;
							 outcome.ok = ok;
							 outcome.error = std::move(error);
						 });
	CHECK(!outcome.done);
	if (!WaitFor(storage, [&] { return outcome.done; })) {
		std::printf("FAIL [%.*s] save timed out\n", static_cast<int>(g_section.size()), g_section.data());
		++g_checks;
		++g_failures;
		return outcome;
	}
	CHECK(outcome.sawInPoll);
	return outcome;
}

// --- parser tests (no database) ----------------------------------------------------------------

void TestParsers() {
	Section("payload parsers");
	int paint = -1;
	int seed = -1;
	float wear = -1.0F;
	ws::storage_detail::ParseSkinPayload("180;42;0.15", paint, seed, wear);
	CHECK_EQ_INT(paint, 180);
	CHECK_EQ_INT(seed, 42);
	CHECK(Near(wear, 0.15F));
	ws::storage_detail::ParseSkinPayload("", paint, seed, wear);
	CHECK_EQ_INT(paint, 0);
	CHECK_EQ_INT(seed, 0);
	CHECK(Near(wear, 0.0F));
	ws::storage_detail::ParseSkinPayload("junk;9;x", paint, seed, wear);
	CHECK_EQ_INT(paint, 0);
	CHECK_EQ_INT(seed, 9);
	CHECK(Near(wear, 0.0F));
	ws::storage_detail::ParseSkinPayload("180", paint, seed, wear);
	CHECK_EQ_INT(paint, 180);
	CHECK_EQ_INT(seed, 0);

	std::array<ws::Sticker, 6> stickers{};
	ws::storage_detail::ParseStickerIds("1001;1002;0;1003", stickers);
	CHECK_EQ_INT(stickers[0].id, 1001);
	CHECK_EQ_INT(stickers[0].schema, 0); // id-only stickers are visible (schema 0)
	CHECK_EQ_INT(stickers[1].id, 1002);
	CHECK_EQ_INT(stickers[2].id, 0);
	CHECK_EQ_INT(stickers[2].schema, 1337);
	CHECK_EQ_INT(stickers[3].id, 1003);
	CHECK_EQ_INT(stickers[4].id, 0);
	stickers = {}; // parsing fills the slots it finds; the caller always passes a fresh Skin
	ws::storage_detail::ParseStickerIds("", stickers);
	CHECK_EQ_INT(stickers[0].id, 0);
	CHECK_EQ_INT(stickers[0].schema, 1337);

	const std::optional<ws::Sticker> full = ws::storage_detail::ParseStickerFull("7;1337;0.1;0.2;0.3;1.5;45");
	CHECK(full.has_value());
	if (full.has_value()) {
		CHECK_EQ_INT(full->id, 7);
		CHECK_EQ_INT(full->schema, 1337);
		CHECK(Near(full->x, 0.1F));
		CHECK(Near(full->y, 0.2F));
		CHECK(Near(full->wear, 0.3F));
		CHECK(Near(full->scale, 1.5F));
		CHECK(Near(full->rotation, 45.0F));
	}
	CHECK(!ws::storage_detail::ParseStickerFull("0;0;0;0;0;0;0").has_value());
	CHECK(!ws::storage_detail::ParseStickerFull("7;1;2;3").has_value());
	CHECK(!ws::storage_detail::ParseStickerFull("").has_value());

	const std::optional<ws::Keychain> keychain = ws::storage_detail::ParseKeychainPayload("55;1.5;-2;0");
	CHECK(keychain.has_value());
	if (keychain.has_value()) {
		CHECK_EQ_INT(keychain->id, 55);
		CHECK(Near(keychain->x, 1.5F));
		CHECK(Near(keychain->y, -2.0F));
		CHECK(Near(keychain->z, 0.0F));
	}
	CHECK(!ws::storage_detail::ParseKeychainPayload("").has_value());
	CHECK(!ws::storage_detail::ParseKeychainPayload("0;1;2;3").has_value());

	Section("identifiers and definition ranges");
	CHECK(ws::storage_detail::ValidIdentifier("sc_"));
	CHECK(ws::storage_detail::ValidIdentifier("wst123_"));
	CHECK(!ws::storage_detail::ValidIdentifier(""));
	CHECK(!ws::storage_detail::ValidIdentifier("sc-"));
	CHECK(!ws::storage_detail::ValidIdentifier("1sc"));
	CHECK(!ws::storage_detail::ValidIdentifier("sc skins"));
	CHECK(!ws::storage_detail::ValidIdentifier("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	CHECK(ws::storage_detail::IsWeaponDefinition(7));
	CHECK(ws::storage_detail::IsWeaponDefinition(1));
	CHECK(!ws::storage_detail::IsWeaponDefinition(0));
	CHECK(!ws::storage_detail::IsWeaponDefinition(42));
	CHECK(!ws::storage_detail::IsWeaponDefinition(59));
	CHECK(!ws::storage_detail::IsWeaponDefinition(500));
	CHECK(!ws::storage_detail::IsWeaponDefinition(5027));
	CHECK(!ws::storage_detail::IsWeaponDefinition(100));
	CHECK_EQ_INT(ws::TeamIndex(2), 0);
	CHECK_EQ_INT(ws::TeamIndex(3), 1);
	CHECK_EQ_INT(ws::TeamIndex(0), -1);
	CHECK_EQ_INT(ws::TeamIndex(1), -1);
}

// --- base load ---------------------------------------------------------------------------------

constexpr uint64_t kSteamA = 76561198000000001ULL;

ws::PisexConfig BaseConfig() {
	ws::PisexConfig config;
	config.serverId = 1;
	config.autoCreatePlayer = true;
	config.useExtTable = true;
	config.pinsEnabled = true;
	return config;
}

void TestBaseLoad(const plgdb::Config& dbConfig) {
	Section("base load: skins, ext, knife, glove, agent, pin, music");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 1, 1337, "1001;0;1002;0", "MyAK", nullptr);
	fixture.SeedSkin(playerId, 1, 1, 7, "279;1;0", 0, 0, "0;0;0;0", "", "55;1.5;-2;0");
	fixture.SeedSkin(playerId, 1, 0, 59, "123;7;0.5", 1, 10, nullptr, "KnifeTag", nullptr);
	fixture.SeedSkin(playerId, 1, 0, 5027, "10006;3;0.25", 0, 0, nullptr, nullptr, nullptr);
	fixture.SeedSkin(playerId, 1, 7, 9, "999;0;0", 0, 0, nullptr, nullptr, nullptr); // invalid team
	fixture.SeedItems(playerId, 1, 0, 4613, 3, 4691, 59, 5027);
	fixture.SeedItems(playerId, 1, 1, 4613, 0, 0, 0, 0);
	// ext is created by the plugin on the first load; seed it afterwards is impossible, so use
	// a second connection that waits until the table exists (below) - instead the ext row is
	// inserted after the first load, and the second load checks the merge.

	const LoadOutcome first = Load(*fixture.storage, kSteamA);
	const ws::Snapshot& snapshot = first.snapshot;
	CHECK(fixture.TableExists("skins_ext")); // created by the plugin on the first load
	CHECK_EQ_INT(snapshot.teams[0].weapons.size(), 1); // knife/glove rows are not weapons
	CHECK_EQ_INT(snapshot.teams[1].weapons.size(), 1);
	CHECK_EQ_INT(snapshot.teams[1].knife.has_value(), 0);
	CHECK_EQ_INT(snapshot.teams[0].agent, 4613);
	CHECK_EQ_INT(snapshot.teams[0].pin, 4691);
	CHECK_EQ_INT(snapshot.teams[1].agent, 4613);
	CHECK_EQ_INT(snapshot.teams[1].pin, 0);
	CHECK_EQ_INT(snapshot.music, 3);
	CHECK(!snapshot.Empty());

	const auto t0 = snapshot.teams[0].weapons.find(7);
	CHECK(t0 != snapshot.teams[0].weapons.end());
	if (t0 != snapshot.teams[0].weapons.end()) {
		CHECK_EQ_INT(t0->second.paint, 180);
		CHECK_EQ_INT(t0->second.seed, 42);
		CHECK(Near(t0->second.wear, 0.15F));
		CHECK_EQ_INT(t0->second.quality, ws::kStatTrakQuality);
		CHECK_EQ_INT(t0->second.stattrak, 1337);
		CHECK_EQ_STR(t0->second.nametag, "MyAK");
		CHECK_EQ_INT(t0->second.stickers[0].id, 1001);
		CHECK_EQ_INT(t0->second.stickers[0].schema, 0);
		CHECK_EQ_INT(t0->second.stickers[1].id, 0);
		CHECK_EQ_INT(t0->second.stickers[2].id, 1002);
		CHECK_EQ_INT(t0->second.keychain.id, 0);
	}
	const auto t1 = snapshot.teams[1].weapons.find(7);
	CHECK(t1 != snapshot.teams[1].weapons.end());
	if (t1 != snapshot.teams[1].weapons.end()) {
		CHECK_EQ_INT(t1->second.paint, 279);
		CHECK_EQ_INT(t1->second.seed, 1);
		CHECK(Near(t1->second.wear, 0.0F));
		CHECK_EQ_INT(t1->second.quality, 0);
		CHECK_EQ_STR(t1->second.nametag, "");
		CHECK_EQ_INT(t1->second.keychain.id, 55);
		CHECK(Near(t1->second.keychain.x, 1.5F));
		CHECK(Near(t1->second.keychain.y, -2.0F));
	}
	CHECK(snapshot.teams[0].knife.has_value());
	if (snapshot.teams[0].knife.has_value()) {
		CHECK_EQ_INT(snapshot.teams[0].knife->definition, 59);
		CHECK_EQ_INT(snapshot.teams[0].knife->paint, 123);
		CHECK_EQ_INT(snapshot.teams[0].knife->seed, 7);
		CHECK(Near(snapshot.teams[0].knife->wear, 0.5F));
		CHECK_EQ_INT(snapshot.teams[0].knife->quality, ws::kStatTrakQuality);
		CHECK_EQ_INT(snapshot.teams[0].knife->stattrak, 10);
		CHECK_EQ_STR(snapshot.teams[0].knife->nametag, "KnifeTag");
	}
	CHECK(snapshot.teams[0].glove.has_value());
	if (snapshot.teams[0].glove.has_value()) {
		CHECK_EQ_INT(snapshot.teams[0].glove->definition, 5027);
		CHECK_EQ_INT(snapshot.teams[0].glove->paint, 10006);
		CHECK_EQ_INT(snapshot.teams[0].glove->seed, 3);
		CHECK(Near(snapshot.teams[0].glove->wear, 0.25F));
	}

	// The website (and the original plugin) put everything the base schema cannot hold into the
	// additive ext table; the second load must merge it. The team 0 row carries quality 0 to
	// prove a stale ext row cannot switch StatTrak off; the team 1 row carries a quality the
	// base column cannot express.
	MustExec(*fixture.seed,
			 "INSERT INTO " + fixture.Table("skins_ext") +
				 " (player_id, server_id, team, weapon_index, quality, sticker_4, sticker_5, stickers_full, "
				 "keychain_seed, updated_at) VALUES (?, 1, 0, 7, 0, ?, ?, ?, 777, 0)",
			 plgdb::Params{playerId, std::string("2001;0;0.1;0.2;0.3;1.5;45"),
						   std::string("2002;1337;0.4;0.5;0.6;2;90"),
						   std::string("[\"1001;0;0.11;0.12;0.13;1.2;10\",\"0;0;0;0;0;0;0\","
									   "\"1002;0;0.21;0.22;0.23;1.3;20\",\"0;0;0;0;0;0;0\","
									   "\"0;0;0;0;0;0;0\",\"0;0;0;0;0;0;0\"]")});
	MustExec(*fixture.seed,
			 "INSERT INTO " + fixture.Table("skins_ext") +
				 " (player_id, server_id, team, weapon_index, quality, keychain_seed) VALUES (?, 1, 1, 7, 12, 999)",
			 plgdb::Params{playerId});
	const LoadOutcome second = Load(*fixture.storage, kSteamA);
	const auto merged = second.snapshot.teams[0].weapons.find(7);
	CHECK(merged != second.snapshot.teams[0].weapons.end());
	if (merged != second.snapshot.teams[0].weapons.end()) {
		CHECK_EQ_INT(merged->second.quality, ws::kStatTrakQuality); // the base flag wins
		CHECK(Near(merged->second.stickers[0].x, 0.11F));
		CHECK(Near(merged->second.stickers[0].scale, 1.2F));
		CHECK(Near(merged->second.stickers[0].rotation, 10.0F));
		CHECK_EQ_INT(merged->second.stickers[2].id, 1002);
		CHECK(Near(merged->second.stickers[2].rotation, 20.0F));
		CHECK_EQ_INT(merged->second.stickers[3].id, 0);
		CHECK_EQ_INT(merged->second.stickers[4].id, 2001);
		CHECK_EQ_INT(merged->second.stickers[5].id, 2002);
		CHECK_EQ_INT(merged->second.stickers[5].schema, 1337);
		CHECK_EQ_INT(merged->second.stattrak, 1337); // the counter is not touched by ext
	}
	const auto mergedCt = second.snapshot.teams[1].weapons.find(7);
	CHECK(mergedCt != second.snapshot.teams[1].weapons.end());
	if (mergedCt != second.snapshot.teams[1].weapons.end()) {
		CHECK_EQ_INT(mergedCt->second.quality, 12); // ext supplies the quality for a Normal base
		CHECK_EQ_INT(mergedCt->second.keychain.id, 55);
		CHECK_EQ_INT(mergedCt->second.keychain.seed, 999);
	}

	// Website-only changes must win over older extensions (no plugin menu rewrites them).
	MustExec(*fixture.seed, "UPDATE " + fixture.Table("skins") +
			 " SET stattrack=0, stickers='3001;0;0;0' WHERE player_id=? AND server_id=1 AND team=0 AND weapon_index=7",
			 plgdb::Params{playerId});
	MustExec(*fixture.seed, "UPDATE " + fixture.Table("skins_ext") +
			 " SET quality=9 WHERE player_id=? AND server_id=1 AND team=0 AND weapon_index=7",
			 plgdb::Params{playerId});
	const LoadOutcome changed = Load(*fixture.storage, kSteamA);
	CHECK(changed.error.empty());
	const auto& website = changed.snapshot.teams[0].weapons.at(7);
	CHECK_EQ_INT(website.quality, 0);
	CHECK_EQ_INT(website.stickers[0].id, 3001);
	CHECK_EQ_INT(website.stickers[2].id, 0);
	CHECK_EQ_INT(website.stickers[4].id, 2001);
	CHECK_EQ_INT(website.stattrak, 1337);
}

// --- player auto-creation ----------------------------------------------------------------------

void TestNoPlayer(const plgdb::Config& dbConfig) {
	Section("no player row: AutoCreatePlayer off/on");
	{
		Fixture fixture;
		ws::PisexConfig config = BaseConfig();
		config.autoCreatePlayer = false;
		if (!fixture.Init(dbConfig, config)) {
			return;
		}
		fixture.SeedServer(1);
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		CHECK(outcome.error.empty()); // "nothing configured" is not an error
		CHECK(outcome.snapshot.Empty());
		CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("player")), 0);
	}
	{
		Fixture fixture;
		if (!fixture.Init(dbConfig, BaseConfig())) {
			return;
		}
		fixture.SeedServer(1);
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		CHECK(outcome.error.empty());
		CHECK(outcome.snapshot.Empty());
		CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("player") +
												  " WHERE steamid = ?",
							   plgdb::Params{std::to_string(kSteamA)}),
					 1);
		CHECK_EQ_STR(ScalarString(*fixture.seed, "SELECT name FROM " + fixture.Table("player") +
													 " WHERE steamid = ?",
								  plgdb::Params{std::to_string(kSteamA)}),
					 "Unknown");
		// The created row is reused by the next load.
		const LoadOutcome again = Load(*fixture.storage, kSteamA);
		CHECK(again.error.empty());
		CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("player")), 1);
	}
}

// --- ext disabled, save before load, concurrent first loads --------------------------------------

void TestExtDisabled(const plgdb::Config& dbConfig) {
	Section("UseExtTable=false: base columns only, no ext table");
	Fixture fixture;
	ws::PisexConfig config = BaseConfig();
	config.useExtTable = false;
	if (!fixture.Init(dbConfig, config)) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 1, 11, "1001;0;0;0", "Tag", nullptr);
	const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
	CHECK(!fixture.TableExists("skins_ext")); // UseExtTable=false must not create it
	const auto weapon = outcome.snapshot.teams[0].weapons.find(7);
	CHECK(weapon != outcome.snapshot.teams[0].weapons.end());
	if (weapon != outcome.snapshot.teams[0].weapons.end()) {
		CHECK_EQ_INT(weapon->second.paint, 180);
		CHECK_EQ_INT(weapon->second.quality, ws::kStatTrakQuality); // from stattrack, no ext
		CHECK_EQ_INT(weapon->second.stattrak, 11);
		CHECK_EQ_INT(weapon->second.stickers[0].id, 1001);
		CHECK_EQ_INT(weapon->second.stickers[4].id, 0);
	}
	// The second load goes through the same "ext not used" path with the server id cached.
	const LoadOutcome again = Load(*fixture.storage, kSteamA);
	CHECK(again.snapshot.teams[0].weapons.find(7) != again.snapshot.teams[0].weapons.end());
}

void TestSaveBeforeLoad(const plgdb::Config& dbConfig) {
	Section("SaveStatTrak before any load resolves server_id itself");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 1, 5, nullptr, nullptr, nullptr);
	// No Load() first: server_id and the player id are not cached yet.
	const SaveOutcome first = Save(*fixture.storage, kSteamA, 2, 7, 1);
	CHECK(first.ok);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND server_id = 1 AND team = 0 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 6);
	// The cached server id is reused by the next save.
	const SaveOutcome second = Save(*fixture.storage, kSteamA, 2, 7, 2);
	CHECK(second.ok);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 8);
}

void TestConcurrentFirstLoads(const plgdb::Config& dbConfig) {
	Section("concurrent first loads: empty cache, one sc_player row");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerA = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerA, 1, 0, 7, "180;42;0.15", 0, 0, nullptr, nullptr, nullptr);
	const uint64_t steamB = kSteamA + 1; // no row at all: auto-create on read
	const uint64_t steamC = kSteamA + 2;

	LoadOutcome a1;
	LoadOutcome a2;
	LoadOutcome b1;
	LoadOutcome b2;
	fixture.storage->LoadPlayer(kSteamA, [&](ws::Snapshot snapshot, std::string error) {
		a1.done = true;
		a1.snapshot = std::move(snapshot);
		a1.error = std::move(error);
	});
	fixture.storage->LoadPlayer(kSteamA, [&](ws::Snapshot snapshot, std::string error) {
		a2.done = true;
		a2.snapshot = std::move(snapshot);
		a2.error = std::move(error);
	});
	fixture.storage->LoadPlayer(steamB, [&](ws::Snapshot snapshot, std::string error) {
		b1.done = true;
		b1.snapshot = std::move(snapshot);
		b1.error = std::move(error);
	});
	fixture.storage->LoadPlayer(steamB, [&](ws::Snapshot snapshot, std::string error) {
		b2.done = true;
		b2.snapshot = std::move(snapshot);
		b2.error = std::move(error);
	});
	CHECK(!a1.done);
	CHECK(!b1.done);
	CHECK(WaitFor(*fixture.storage, [&] { return a1.done && a2.done && b1.done && b2.done; }));
	CHECK(a1.error.empty());
	CHECK(a2.error.empty());
	CHECK(b1.error.empty());
	CHECK(b2.error.empty());
	CHECK(a1.snapshot.teams[0].weapons.find(7) != a1.snapshot.teams[0].weapons.end());
	CHECK(a2.snapshot.teams[0].weapons.find(7) != a2.snapshot.teams[0].weapons.end());
	CHECK(b1.snapshot.Empty());
	CHECK(b2.snapshot.Empty());
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("player") +
											  " WHERE steamid = ?",
						   plgdb::Params{std::to_string(steamB)}),
				 1);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("player")), 2);

	// Two loads for the same unknown player, queued before either is polled.
	LoadOutcome c1;
	LoadOutcome c2;
	fixture.storage->LoadPlayer(steamC, [&](ws::Snapshot snapshot, std::string error) {
		c1.done = true;
		c1.snapshot = std::move(snapshot);
		c1.error = std::move(error);
	});
	fixture.storage->LoadPlayer(steamC, [&](ws::Snapshot snapshot, std::string error) {
		c2.done = true;
		c2.snapshot = std::move(snapshot);
		c2.error = std::move(error);
	});
	CHECK(WaitFor(*fixture.storage, [&] { return c1.done && c2.done; }));
	CHECK(c1.error.empty());
	CHECK(c2.error.empty());
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("player") +
											  " WHERE steamid = ?",
						   plgdb::Params{std::to_string(steamC)}),
				 1);
}

// --- server id ---------------------------------------------------------------------------------

void TestServerFallback(const plgdb::Config& dbConfig) {
	Section("server_id: validate, fallback, empty table");
	{
		Fixture fixture;
		ws::PisexConfig config = BaseConfig();
		config.serverId = 99; // not present in sc_servers
		if (!fixture.Init(dbConfig, config)) {
			return;
		}
		fixture.SeedServer(1);
		fixture.SeedServer(2);
		const int64_t playerId = fixture.SeedPlayer(kSteamA);
		fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 0, 0, nullptr, nullptr, nullptr);
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		const auto weapon = outcome.snapshot.teams[0].weapons.find(7);
		CHECK(weapon != outcome.snapshot.teams[0].weapons.end()); // fell back to server 1
	}
	{
		Fixture fixture;
		ws::PisexConfig config = BaseConfig();
		config.serverId = 2;
		if (!fixture.Init(dbConfig, config)) {
			return;
		}
		fixture.SeedServer(1);
		fixture.SeedServer(2);
		const int64_t playerId = fixture.SeedPlayer(kSteamA);
		fixture.SeedSkin(playerId, 2, 0, 7, "279;1;0", 0, 0, nullptr, nullptr, nullptr);
		fixture.SeedSkin(playerId, 1, 0, 9, "180;42;0.15", 0, 0, nullptr, nullptr, nullptr);
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		CHECK(outcome.snapshot.teams[0].weapons.find(7) != outcome.snapshot.teams[0].weapons.end());
		CHECK(outcome.snapshot.teams[0].weapons.find(9) == outcome.snapshot.teams[0].weapons.end());
	}
	{
		Fixture fixture;
		if (!fixture.Init(dbConfig, BaseConfig())) {
			return;
		}
		// sc_servers is empty.
		fixture.SeedPlayer(kSteamA);
		LoadOutcome outcome;
		fixture.storage->LoadPlayer(kSteamA, [&](ws::Snapshot snapshot, std::string error) {
			outcome.done = true;
			outcome.snapshot = std::move(snapshot);
			outcome.error = std::move(error);
		});
		CHECK(WaitFor(*fixture.storage, [&] { return outcome.done; }));
		CHECK(!outcome.error.empty());
	}
}

// --- stickers and the 1337 marker ---------------------------------------------------------------

void TestStickerMarker(const plgdb::Config& dbConfig) {
	Section("sticker payloads: id-only, 1337 hidden marker, empty ext slot");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 0, 0, "1001;1002;0;1003", nullptr, nullptr);

	Load(*fixture.storage, kSteamA); // creates sc_skins_ext
	MustExec(*fixture.seed,
			 "INSERT INTO " + fixture.Table("skins_ext") +
				 " (player_id, server_id, team, weapon_index, quality, sticker_4, sticker_5) VALUES (?, 1, 0, 7, 0, ?, ?)",
			 plgdb::Params{playerId, std::string("0;0;0;0;0;0;0"), std::string("555;1337;0;0;0;0;0")});
	const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
	const auto weapon = outcome.snapshot.teams[0].weapons.find(7);
	CHECK(weapon != outcome.snapshot.teams[0].weapons.end());
	if (weapon != outcome.snapshot.teams[0].weapons.end()) {
		CHECK_EQ_INT(weapon->second.stickers[0].id, 1001);
		CHECK_EQ_INT(weapon->second.stickers[0].schema, 0);
		CHECK_EQ_INT(weapon->second.stickers[1].id, 1002);
		CHECK_EQ_INT(weapon->second.stickers[2].id, 0);
		CHECK_EQ_INT(weapon->second.stickers[3].schema, 0); // the id survives even with schema 0
		CHECK_EQ_INT(weapon->second.stickers[4].id, 0);     // "0;..." is an empty slot
		CHECK_EQ_INT(weapon->second.stickers[5].id, 555);   // the hidden marker keeps 1337
		CHECK_EQ_INT(weapon->second.stickers[5].schema, 1337);
	}
}

// --- agent-only and pin-only --------------------------------------------------------------------

void TestAgentOnly(const plgdb::Config& dbConfig) {
	Section("agent only: not empty, nothing else set");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedItems(playerId, 1, 0, 1234, 0, 0, 0, 0);
	const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
	CHECK_EQ_INT(outcome.snapshot.teams[0].agent, 1234);
	CHECK_EQ_INT(outcome.snapshot.teams[1].agent, 0);
	CHECK(!outcome.snapshot.Empty());
}

void TestPinOnly(const plgdb::Config& dbConfig) {
	Section("pin only: PinsEnabled on/off");
	{
		Fixture fixture;
		if (!fixture.Init(dbConfig, BaseConfig())) {
			return;
		}
		fixture.SeedServer(1);
		const int64_t playerId = fixture.SeedPlayer(kSteamA);
		fixture.SeedItems(playerId, 1, 1, 0, 0, 4691, 0, 0);
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		CHECK_EQ_INT(outcome.snapshot.teams[1].pin, 4691);
		CHECK(!outcome.snapshot.Empty());
	}
	{
		Fixture fixture;
		ws::PisexConfig config = BaseConfig();
		config.pinsEnabled = false;
		if (!fixture.Init(dbConfig, config)) {
			return;
		}
		fixture.SeedServer(1);
		const int64_t playerId = fixture.SeedPlayer(kSteamA);
		fixture.SeedItems(playerId, 1, 1, 0, 0, 4691, 0, 0);
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		CHECK_EQ_INT(outcome.snapshot.teams[1].pin, 0);
		CHECK(outcome.snapshot.Empty());
	}
}

// --- StatTrak -----------------------------------------------------------------------------------

void TestStatTrakSave(const plgdb::Config& dbConfig) {
	Section("SaveStatTrak: delta only, no inserts, team mapping");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 1, 5, "1;2;3;4", "TAG", "9;1;2;3");
	fixture.SeedSkin(playerId, 1, 1, 7, "180;42;0.15", 0, 0, nullptr, nullptr, nullptr);
	Load(*fixture.storage, kSteamA); // resolves server/player and creates the ext table

	const SaveOutcome added = Save(*fixture.storage, kSteamA, 2, 7, 2); // engine team 2 -> pisex 0
	CHECK(added.ok);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND server_id = 1 AND team = 0 AND weapon_index = 7",
							   plgdb::Params{playerId}),
				 7);
	// The columns the website owns are untouched.
	CHECK_EQ_STR(ScalarString(*fixture.seed, "SELECT skin FROM " + fixture.Table("skins") +
												 " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
							  plgdb::Params{playerId}),
				 "180;42;0.15");
	CHECK_EQ_STR(ScalarString(*fixture.seed, "SELECT tag FROM " + fixture.Table("skins") +
												 " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
							  plgdb::Params{playerId}),
				 "TAG");
	CHECK_EQ_STR(ScalarString(*fixture.seed, "SELECT stickers FROM " + fixture.Table("skins") +
												 " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
							  plgdb::Params{playerId}),
				 "1;2;3;4");
	CHECK_EQ_STR(ScalarString(*fixture.seed, "SELECT keychain FROM " + fixture.Table("skins") +
												 " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
							  plgdb::Params{playerId}),
				 "9;1;2;3");

	// A delta of 0 changes nothing but is not an error, and no row is duplicated.
	const SaveOutcome noop = Save(*fixture.storage, kSteamA, 2, 7, 0);
	CHECK(noop.ok);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 7);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 1);

	const SaveOutcome negative = Save(*fixture.storage, kSteamA, 2, 7, -3);
	CHECK(negative.ok);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 4);

	const SaveOutcome ct = Save(*fixture.storage, kSteamA, 3, 7, 1); // engine team 3 -> pisex 1
	CHECK(ct.ok);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 1 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 1);
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT stattrack_count FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 0 AND weapon_index = 7",
						   plgdb::Params{playerId}),
				 4);

	// A missing row is reported, not answered with a blind INSERT (the schema has no unique key).
	const SaveOutcome missing = Save(*fixture.storage, kSteamA, 2, 42, 1);
	CHECK(!missing.ok);
	CHECK(!missing.error.empty());
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("skins") +
											  " WHERE player_id = ? AND team = 0 AND weapon_index = 42",
						   plgdb::Params{playerId}),
				 0);

	// Invalid teams never reach the database.
	for (const int team : {0, 1, 5}) {
		const SaveOutcome invalid = Save(*fixture.storage, kSteamA, team, 7, 1);
		CHECK(!invalid.ok);
		CHECK(!invalid.error.empty());
	}
	CHECK_EQ_INT(ScalarInt(*fixture.seed, "SELECT COUNT(*) FROM " + fixture.Table("skins")), 2);
}

// --- async / polling and shutdown ---------------------------------------------------------------

void TestAsyncPolling(const plgdb::Config& dbConfig) {
	Section("async: callbacks only from Poll, two loads in flight");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerA = fixture.SeedPlayer(kSteamA);
	const uint64_t steamB = kSteamA + 1;
	const int64_t playerB = fixture.SeedPlayer(steamB);
	fixture.SeedSkin(playerA, 1, 0, 7, "180;42;0.15", 0, 0, nullptr, nullptr, nullptr);
	fixture.SeedSkin(playerB, 1, 0, 9, "279;1;0", 0, 0, nullptr, nullptr, nullptr);

	LoadOutcome a;
	LoadOutcome b;
	fixture.storage->LoadPlayer(kSteamA, [&](ws::Snapshot snapshot, std::string error) {
		a.done = true;
		a.sawInPoll = g_inPoll;
		a.snapshot = std::move(snapshot);
		a.error = std::move(error);
	});
	fixture.storage->LoadPlayer(steamB, [&](ws::Snapshot snapshot, std::string error) {
		b.done = true;
		b.sawInPoll = g_inPoll;
		b.snapshot = std::move(snapshot);
		b.error = std::move(error);
	});
	CHECK(!a.done);
	CHECK(!b.done);
	CHECK(fixture.storage->Pending() > 0);

	const bool finished = WaitFor(*fixture.storage, [&] { return a.done && b.done; });
	CHECK(finished);
	CHECK(a.sawInPoll);
	CHECK(b.sawInPoll);
	CHECK(a.snapshot.teams[0].weapons.find(7) != a.snapshot.teams[0].weapons.end());
	CHECK(b.snapshot.teams[0].weapons.find(9) != b.snapshot.teams[0].weapons.end());
	CHECK_EQ_INT(a.snapshot.teams[0].weapons.size(), 1);
	CHECK_EQ_INT(b.snapshot.teams[0].weapons.size(), 1);
	Settle(*fixture.storage);
	CHECK_EQ_INT(fixture.storage->Pending(), 0);
}

void TestShutdownSafety(const plgdb::Config& dbConfig) {
	Section("shutdown: destroying the Storage drops in-flight chains");
	int callbacks = 0;
	{
		Fixture fixture;
		if (!fixture.Init(dbConfig, BaseConfig())) {
			return;
		}
		fixture.SeedServer(1);
		fixture.SeedPlayer(kSteamA);
		for (int i = 0; i < 3; ++i) {
			fixture.storage->LoadPlayer(kSteamA, [&callbacks](ws::Snapshot, std::string) { ++callbacks; });
		}
		CHECK(fixture.storage->Pending() > 0);
		// No Poll(): the Storage (and its database) is destroyed with the chains in flight.
	}
	CHECK_EQ_INT(callbacks, 0);

	// The database is still usable afterwards (a second Storage sees the same rows).
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 0, 0, nullptr, nullptr, nullptr);
	const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
	CHECK(outcome.snapshot.teams[0].weapons.find(7) != outcome.snapshot.teams[0].weapons.end());
}

// --- ext availability and retry -----------------------------------------------------------------

void TestExtFailure(const plgdb::Config& dbConfig) {
	Section("ext: broken table keeps base data, retried later");
	Fixture fixture;
	if (!fixture.Init(dbConfig, BaseConfig())) {
		return;
	}
	fixture.SeedServer(1);
	const int64_t playerId = fixture.SeedPlayer(kSteamA);
	fixture.SeedSkin(playerId, 1, 0, 7, "180;42;0.15", 1, 3, nullptr, nullptr, nullptr);
	// A conflicting object with the ext name: CREATE TABLE IF NOT EXISTS is a no-op, and the
	// ext SELECT fails on the missing columns.
	MustExec(*fixture.seed, "CREATE TABLE " + fixture.Table("skins_ext") + " (player_id INT NOT NULL)");
	CHECK(fixture.TableExists("skins_ext"));

	const LoadOutcome degraded = Load(*fixture.storage, kSteamA);
	CHECK(degraded.error.empty()); // ext problems never fail the load
	const auto weapon = degraded.snapshot.teams[0].weapons.find(7);
	CHECK(weapon != degraded.snapshot.teams[0].weapons.end());
	if (weapon != degraded.snapshot.teams[0].weapons.end()) {
		CHECK_EQ_INT(weapon->second.quality, ws::kStatTrakQuality); // from the base columns
		CHECK_EQ_INT(weapon->second.stattrak, 3);
	}

	// Remove the conflicting object; one of the next loads must recreate the real ext table.
	MustExec(*fixture.seed, "DROP TABLE " + fixture.Table("skins_ext"));
	bool created = false;
	for (int attempt = 0; attempt < 8 && !created; ++attempt) {
		const LoadOutcome outcome = Load(*fixture.storage, kSteamA);
		CHECK(outcome.error.empty());
		created = fixture.TableExists("skins_ext");
	}
	CHECK(created);
	CHECK_EQ_INT(ColumnCount(*fixture.seed, fixture.Table("skins_ext")), 10);

	MustExec(*fixture.seed,
			 "INSERT INTO " + fixture.Table("skins_ext") +
				 " (player_id, server_id, team, weapon_index, quality) VALUES (?, 1, 0, 7, 12)",
			 plgdb::Params{playerId});
	const LoadOutcome recovered = Load(*fixture.storage, kSteamA);
	CHECK(recovered.error.empty());
	const auto after = recovered.snapshot.teams[0].weapons.find(7);
	CHECK(after != recovered.snapshot.teams[0].weapons.end());
	if (after != recovered.snapshot.teams[0].weapons.end()) {
		CHECK_EQ_INT(after->second.quality, ws::kStatTrakQuality); // base StatTrak wins over ext
	}
}

// --- schema safety ------------------------------------------------------------------------------

void TestOnlyAdditiveExtIsCreated(const plgdb::Config& dbConfig) {
	Section("schema safety: only the additive ext table is created");
	const std::string prefix = NewPrefix();
	std::unique_ptr<plgdb::Database> db = OpenDb(dbConfig);
	if (!db) {
		CHECK(false);
		return;
	}
	ws::PisexConfig pisex;
	pisex.tablePrefix = prefix;
	ws::Storage storage(pisex, OpenDb(dbConfig));
	LoadOutcome outcome;
	storage.LoadPlayer(kSteamA, [&](ws::Snapshot snapshot, std::string error) {
		outcome.done = true;
		outcome.snapshot = std::move(snapshot);
		outcome.error = std::move(error);
	});
	CHECK(WaitFor(storage, [&] { return outcome.done; }));
	CHECK(!outcome.error.empty()); // the missing base table is reported
	CHECK(TableExists(*db, prefix + "skins_ext")); // additive, so it is created
	CHECK(!TableExists(*db, prefix + "player"));
	CHECK(!TableExists(*db, prefix + "skins"));
	CHECK(!TableExists(*db, prefix + "servers"));
	CHECK(!TableExists(*db, prefix + "items"));
	Exec(*db, "DROP TABLE IF EXISTS " + prefix + "skins_ext", {});
}

void TestIdentifierValidation(const plgdb::Config& dbConfig) {
	Section("TablePrefix validation");
	for (const std::string& prefix : {std::string("sc-1"), std::string("1sc"), std::string("sc 1"),
									  std::string("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")}) {
		ws::PisexConfig config = BaseConfig();
		config.tablePrefix = prefix;
		ws::Storage storage(config, OpenDb(dbConfig));
		LoadOutcome load;
		storage.LoadPlayer(kSteamA, [&](ws::Snapshot snapshot, std::string error) {
			load.done = true;
			load.snapshot = std::move(snapshot);
			load.error = std::move(error);
		});
		CHECK(!load.done); // still delivered from Poll
		CHECK(WaitFor(storage, [&] { return load.done; }));
		CHECK(!load.error.empty());
		SaveOutcome save;
		storage.SaveStatTrak(kSteamA, 2, 7, 1, [&](bool ok, std::string error) {
			save.done = true;
			save.ok = ok;
			save.error = std::move(error);
		});
		CHECK(WaitFor(storage, [&] { return save.done; }));
		CHECK(!save.ok);
		CHECK(!save.error.empty());
	}
}

void RunDatabaseSuite(const plgdb::Config& config, const char* label) {
	std::printf("== %s\n", label);
	TestBaseLoad(config);
	TestNoPlayer(config);
	TestExtDisabled(config);
	TestSaveBeforeLoad(config);
	TestConcurrentFirstLoads(config);
	TestServerFallback(config);
	TestStickerMarker(config);
	TestAgentOnly(config);
	TestPinOnly(config);
	TestStatTrakSave(config);
	TestAsyncPolling(config);
	TestShutdownSafety(config);
	TestExtFailure(config);
	TestOnlyAdditiveExtIsCreated(config);
	TestIdentifierValidation(config);
}

} // namespace

int main() {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("WeaponSkins storage tests\n");

	TestParsers();

	plgdb::Config mysql;
	std::string error;
	if (LoadMySqlConfig(mysql, error)) {
		RunDatabaseSuite(mysql, "mysql");
	} else if (!error.empty()) {
		std::printf("FATAL: %s\n", error.c_str());
		++g_failures;
	} else {
		std::printf("== mysql: skipped (MYSQL_HOST not set)\n");
	}

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
