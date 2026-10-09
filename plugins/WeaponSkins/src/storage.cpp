// Pisex (sc_*) storage backend of WeaponSkins, on top of plgdb.
//
// The website writes the player's loadout into the Skinchanger-by-Pisex tables; this file reads
// a player's rows into a Snapshot and writes the StatTrak counter back. Only the production
// configuration is implemented: the lazy per-player load and the additive sc_skins_ext table
// (UseExtTable). Everything the schema has no room for (quality, sticker slots 5/6, full sticker
// parameters, keychain seed) lives in sc_skins_ext, which is the ONLY table this code creates.
//
// Threading: every statement goes to the single plgdb worker (FIFO); results are delivered only
// from Storage::Poll(), which the plugin calls on the game thread. A load is a chain of
// statements, and each step is issued from the previous step's callback - so the game thread is
// never blocked and plgdb's pending count is meaningful between steps.
//
// Lifetime: the chains hold a weak token of Impl. When the Storage is destroyed the database is
// destroyed first (~Impl), and plgdb drops completions that were never polled (libs/plgdb/src/
// database.cpp: StopWorker), so an in-flight chain cannot touch freed state.
//
// Destruction is NOT a flush: dropping the database discards every not-yet-polled result (only
// the first statement of a chain is normally queued, the rest are issued from callbacks), so a
// caller that has to be sure a SaveStatTrak was applied must drain the chain first - call Poll()
// until Pending() reaches zero - and only then reset the Storage. Pending() counts the operations
// whose final callback has not been delivered yet, which is what a drain loop must wait for.
//
// StatTrak: SaveStatTrak only moves the counter (stattrack_count = stattrack_count + delta) so a
// simultaneous website change of paint/seed/stickers is never overwritten. There is deliberately
// no UPDATE-then-INSERT fallback: plgdb connects without CLIENT_FOUND_ROWS, so "affected rows ==
// 0" does not mean "row missing", and a blind INSERT would create duplicate sc_skins rows (the
// schema has no unique key).

#include "storage.hpp"

#include "storage_detail.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ws {

using storage_detail::IsWeaponDefinition;
using storage_detail::ParseKeychainPayload;
using storage_detail::ParseSkinPayload;
using storage_detail::ParseStickerFull;
using storage_detail::ParseStickerIds;
using storage_detail::RowKey;
using storage_detail::ValidIdentifier;

namespace {

// A failed sc_skins_ext attempt is repeated after this many operations. Until the table exists
// the plugin works from the base columns; the counter keeps a broken ext DDL (no CREATE rights,
// conflicting object) from turning into one failed statement per load.
constexpr int64_t kExtRetryEveryOps = 4;

enum class ExtState { Unknown, Creating, Ready, Failed };

std::string ExtCreateStatement(const std::string& table, plgdb::Driver driver) {
	if (driver == plgdb::Driver::MySQL) {
		// Byte for byte the DDL of the original C# backend, so an existing production
		// sc_skins_ext is left alone and a new one is created identically.
		return "CREATE TABLE IF NOT EXISTS " + table +
			   " (player_id int(11) NOT NULL, server_id int(11) NOT NULL, team int(11) NOT NULL, "
			   "weapon_index int(11) NOT NULL, quality int(11) NOT NULL DEFAULT 0, "
			   "sticker_4 varchar(96) NOT NULL DEFAULT '0;0;0;0;0;0;0', "
			   "sticker_5 varchar(96) NOT NULL DEFAULT '0;0;0;0;0;0;0', "
			   "stickers_full varchar(768) DEFAULT NULL, keychain_seed int(11) NOT NULL DEFAULT 0, "
			   "updated_at int(11) NOT NULL DEFAULT 0, "
			   "PRIMARY KEY (player_id, server_id, team, weapon_index)) "
			   "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci";
	}
	// SQLite (tests): the same columns without the MySQL storage options.
	return "CREATE TABLE IF NOT EXISTS " + table +
		   " (player_id INTEGER NOT NULL, server_id INTEGER NOT NULL, team INTEGER NOT NULL, "
		   "weapon_index INTEGER NOT NULL, quality INTEGER NOT NULL DEFAULT 0, "
		   "sticker_4 TEXT NOT NULL DEFAULT '0;0;0;0;0;0;0', sticker_5 TEXT NOT NULL DEFAULT '0;0;0;0;0;0;0', "
		   "stickers_full TEXT DEFAULT NULL, keychain_seed INTEGER NOT NULL DEFAULT 0, "
		   "updated_at INTEGER NOT NULL DEFAULT 0, "
		   "PRIMARY KEY (player_id, server_id, team, weapon_index))";
}

// --- result access -------------------------------------------------------------------------
// Every SELECT aliases or names its columns; a missing column degrades to the default instead of
// reading a neighbouring field.

int64_t CellInt(const plgdb::Result& result, size_t row, const std::optional<size_t>& column, int64_t fallback = 0) {
	if (!column.has_value() || result.IsNull(row, *column)) {
		return fallback;
	}
	return result.GetInt(row, *column, fallback);
}

std::string CellString(const plgdb::Result& result, size_t row, const std::optional<size_t>& column) {
	if (!column.has_value() || result.IsNull(row, *column)) {
		return {};
	}
	return result.GetString(row, *column, {});
}

std::string ExtractError(const plgdb::Result& result, std::string_view context) {
	std::string message = "Pisex: ";
	message += context;
	message += " failed: ";
	message += result.Error();
	return message;
}

// sc_player/sc_items/sc_skins store the Pisex team (0 = T, 1 = CT); the engine sends 2/3, which
// ws::TeamIndex converts. A database value must NEVER go through TeamIndex - TeamIndex(0) is
// "no team" for the engine and would throw every row away.
int DbTeamIndex(int64_t team) {
	return team == 0 ? 0 : team == 1 ? 1 : -1;
}

// The sc_player insert. AutoCreatePlayer must not create two rows for the same player when two
// loads run into the missing row at the same time (sc_player has no unique key, so the database
// cannot stop it): Impl::creating serializes the create per steamid, and because the worker is
// FIFO the extra load's re-SELECT is always queued after the first load's INSERT, so it sees the
// row that INSERT created.
std::string CreatePlayerStatement(const std::string& table) {
	return "INSERT INTO " + table + " (name, steamid) VALUES ('Unknown', ?)";
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct Storage::Impl {
	struct Op;
	struct LoadOp;
	struct SaveOp;

	PisexConfig config;
	std::unique_ptr<plgdb::Database> db;
	std::shared_ptr<void> life = std::make_shared<int>(0);

	bool prefixOk = false;
	std::optional<int> serverId; // resolved once, like the C# _serverId cache
	ExtState ext = ExtState::Unknown;
	int64_t extAttempts = 0;
	int64_t opsSinceExtAttempt = 0;
	// Steam ids whose AutoCreatePlayer INSERT is in flight; a second load for the same player
	// re-selects instead of inserting again (the row it waits for is created by the first load).
	std::unordered_set<uint64_t> creating;

	// Callbacks that could not be queued as statements (validation failures). They are delivered
	// by Poll() so that a caller never re-enters its own command handler synchronously.
	std::deque<std::function<void()>> deferred;
	size_t operations = 0; // accepted operations whose final callback has not been delivered yet

	Impl(PisexConfig cfg, std::unique_ptr<plgdb::Database> database)
		: config(std::move(cfg)), db(std::move(database)), prefixOk(ValidIdentifier(config.tablePrefix)) {}

	~Impl() {
		// Destroy the database first: it joins the worker and drops unpolled completions, so no
		// chain can run after this point and the remaining members are safe to destroy.
		// NOTE: this is a safety guarantee, not a flush - a pending SaveStatTrak chain loses the
		// statements that were never issued, so the caller drains with Poll() until Pending()==0
		// before resetting the Storage (see the file header).
		db.reset();
	}

	std::string Table(std::string_view name) const { return config.tablePrefix + std::string(name); }
	std::string ExtTable() const { return Table("skins_ext"); }
	plgdb::Driver Driver() const { return db ? db->GetDriver() : plgdb::Driver::MySQL; }

	void Query(std::string sql, plgdb::Params params, plgdb::Callback callback) {
		if (!db) {
			return;
		}
		// The weak token is the project's usual belt-and-braces check for a result arriving after
		// the owner is gone (the database is destroyed before the token anyway).
		std::weak_ptr<void> token = life;
		db->Query(std::move(sql), std::move(params),
				  [token, callback = std::move(callback)](const plgdb::Result& result) {
					  if (token.expired() || !callback) {
						  return;
					  }
					  callback(result);
				  });
	}

	// True when this operation must create sc_skins_ext before reading it. Marks the attempt in
	// flight so concurrent loads do not queue the same CREATE (statements are FIFO, the first
	// result that arrives settles the state).
	bool BeginExtAttempt() {
		++opsSinceExtAttempt;
		if (!config.useExtTable || ext == ExtState::Ready || ext == ExtState::Creating) {
			return false;
		}
		if (extAttempts > 0 && opsSinceExtAttempt < kExtRetryEveryOps) {
			return false;
		}
		ext = ExtState::Creating;
		++extAttempts;
		opsSinceExtAttempt = 0;
		return true;
	}

	void NoteExtResult(bool ok) {
		if (ext == ExtState::Creating) {
			ext = ok ? ExtState::Ready : ExtState::Failed;
		}
	}
};

// --- shared server/player resolution ---------------------------------------------------------
// Both operations first need server_id and the player's sc_player.id. Auto-creating the player
// row happens on READ as well when AutoCreatePlayer is on (the target behaviour: the plugin must
// not depend on the website having written the row before the first StatTrak kill).

struct Storage::Impl::Op : std::enable_shared_from_this<Storage::Impl::Op> {
	Impl* impl = nullptr;
	uint64_t steamId = 0;
	long playerId = 0;
	int stage = 0;
	bool delivered = false;

	enum Stage {
		StageNone = 0,
		StageServerProbe,
		StageServerFirst,
		StagePlayerFind,
		StagePlayerInsert,
		StagePlayerRefind,
		StageExtCreate,
	};

	virtual ~Op() = default;

	virtual void OnOwnResult(const plgdb::Result& result) = 0;
	virtual void ServerReady() = 0;
	virtual void PlayerReady() = 0;
	virtual void PlayerMissing() = 0;

	void Query(std::string sql, plgdb::Params params, int newStage) {
		stage = newStage;
		std::shared_ptr<Op> self = shared_from_this();
		impl->Query(std::move(sql), std::move(params), [self](const plgdb::Result& result) { self->OnResult(result); });
	}

	void OnResult(const plgdb::Result& result) {
		switch (stage) {
		case StageServerProbe:
			return OnServerProbe(result);
		case StageServerFirst:
			return OnServerFirst(result);
		case StagePlayerFind:
			return OnPlayerFind(result);
		case StagePlayerInsert:
			return OnPlayerInsert(result);
		case StagePlayerRefind:
			return OnPlayerRefind(result);
		case StageExtCreate:
			return OnExtCreate(result);
		default:
			return OnOwnResult(result);
		}
	}

	void Complete() {
		if (delivered) {
			return;
		}
		delivered = true;
		if (impl->operations > 0) {
			--impl->operations;
		}
	}

	// server_id: the configured row when it exists, otherwise the first row (the website's own
	// fallback); an empty sc_servers table is an error, not a silent zero.
	void ResolveServer() {
		if (impl->serverId.has_value()) {
			return ServerReady();
		}
		if (impl->config.serverId > 0) {
			return Query("SELECT id FROM " + impl->Table("servers") + " WHERE id = ? LIMIT 1",
						 plgdb::Params{static_cast<int64_t>(impl->config.serverId)}, StageServerProbe);
		}
		return Query("SELECT id FROM " + impl->Table("servers") + " ORDER BY id ASC LIMIT 1", {}, StageServerFirst);
	}

	void OnServerProbe(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Fail(ExtractError(result, "reading " + impl->Table("servers")));
		}
		if (result.RowCount() > 0) {
			impl->serverId = static_cast<int>(result.GetInt(0, 0, impl->config.serverId));
			return ServerReady();
		}
		// The configured server row is gone: fall back to the first one, exactly like the
		// website does (the C# logged a warning here; we only report an error if this fails).
		return Query("SELECT id FROM " + impl->Table("servers") + " ORDER BY id ASC LIMIT 1", {}, StageServerFirst);
	}

	void OnServerFirst(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Fail(ExtractError(result, "reading " + impl->Table("servers")));
		}
		if (result.RowCount() == 0) {
			return Fail("Pisex: " + impl->Table("servers") + " is empty, cannot resolve server_id");
		}
		impl->serverId = static_cast<int>(result.GetInt(0, 0, 0));
		return ServerReady();
	}

	void ResolvePlayer() {
		if (playerId > 0) {
			return PlayerReady();
		}
		return Query("SELECT id FROM " + impl->Table("player") + " WHERE steamid = ? LIMIT 1",
					 plgdb::Params{SteamIdText()}, StagePlayerFind);
	}

	void OnPlayerFind(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Fail(ExtractError(result, "reading " + impl->Table("player")));
		}
		if (result.RowCount() > 0) {
			playerId = result.GetInt(0, 0, 0);
			return PlayerReady();
		}
		if (!impl->config.autoCreatePlayer) {
			return PlayerMissing();
		}
		if (!impl->creating.insert(steamId).second) {
			// Another load for this player is already creating the row. It queued its INSERT
			// before this statement, so this re-SELECT runs after it and finds the row.
			return Query("SELECT id FROM " + impl->Table("player") + " WHERE steamid = ? LIMIT 1",
						 plgdb::Params{SteamIdText()}, StagePlayerRefind);
		}
		// The website creates the row the same way (name 'Unknown', SteamID64 as text). The
		// result is ignored: whoever wins, the following SELECT finds the row.
		return Query(CreatePlayerStatement(impl->Table("player")), plgdb::Params{SteamIdText()}, StagePlayerInsert);
	}

	void OnPlayerInsert(const plgdb::Result&) {
		impl->creating.erase(steamId); // the row exists (or not) now; later loads re-check normally
		return Query("SELECT id FROM " + impl->Table("player") + " WHERE steamid = ? LIMIT 1",
					 plgdb::Params{SteamIdText()}, StagePlayerRefind);
	}

	void OnPlayerRefind(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Fail(ExtractError(result, "reading " + impl->Table("player")));
		}
		if (result.RowCount() == 0) {
			return Fail("Pisex: failed to create the " + impl->Table("player") + " row");
		}
		playerId = result.GetInt(0, 0, 0);
		return PlayerReady();
	}

	void OnExtCreate(const plgdb::Result& result) {
		impl->NoteExtResult(result.Ok());
		// An ext failure must never take the base data down: continue with the normal chain.
		return ResolveServer();
	}

	std::string SteamIdText() const { return std::to_string(steamId); }
	virtual void Fail(std::string message) = 0;
};

// --- LoadPlayer ------------------------------------------------------------------------------

struct Storage::Impl::LoadOp final : Op {
	LoadCallback callback;
	Snapshot snapshot;
	std::unordered_map<uint32_t, Skin> rows; // every sc_skins row, base columns only

	enum LoadStage {
		StageSkins = 100,
		StageExtRows,
		StageItems,
	};

	void Begin() {
		if (impl->BeginExtAttempt()) {
			return Query(ExtCreateStatement(impl->ExtTable(), impl->Driver()), {}, StageExtCreate);
		}
		// Never skip ResolveServer, even when the ext attempt is skipped (UseExtTable=false, or
		// another load is creating ext right now): server_id is what every later stage uses.
		return ResolveServer();
	}

	void ServerReady() override { return ResolvePlayer(); }

	void PlayerReady() override { return LoadSkins(); }

	void PlayerMissing() override { Done(std::move(snapshot), std::string()); }
	void Fail(std::string message) override { Done(Snapshot{}, std::move(message)); }

	void OnOwnResult(const plgdb::Result& result) override {
		switch (stage) {
		case StageSkins:
			return OnSkins(result);
		case StageExtRows:
			return OnExtRows(result);
		case StageItems:
			return OnItems(result);
		default:
			return Fail("Pisex: internal load stage error");
		}
	}

	void LoadSkins() {
		return Query("SELECT team, weapon_index, skin, stattrack, stattrack_count, stickers, tag, keychain FROM " +
						 impl->Table("skins") + " WHERE player_id = ? AND server_id = ?",
					 plgdb::Params{static_cast<int64_t>(playerId), static_cast<int64_t>(*impl->serverId)}, StageSkins);
	}

	void OnSkins(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Fail(ExtractError(result, "reading " + impl->Table("skins")));
		}
		const std::optional<size_t> c_team = result.ColumnIndex("team");
		const std::optional<size_t> c_def = result.ColumnIndex("weapon_index");
		const std::optional<size_t> c_skin = result.ColumnIndex("skin");
		const std::optional<size_t> c_st = result.ColumnIndex("stattrack");
		const std::optional<size_t> c_stc = result.ColumnIndex("stattrack_count");
		const std::optional<size_t> c_stick = result.ColumnIndex("stickers");
		const std::optional<size_t> c_tag = result.ColumnIndex("tag");
		const std::optional<size_t> c_key = result.ColumnIndex("keychain");
		for (size_t row = 0; row < result.RowCount(); ++row) {
			// The column carries the Pisex team (0/1), not the engine team - see DbTeamIndex.
			const int teamIndex = DbTeamIndex(CellInt(result, row, c_team));
			const int64_t definition = CellInt(result, row, c_def);
			if (teamIndex < 0 || definition <= 0 || definition > 0xFFFF) {
				continue;
			}
			Skin skin;
			skin.definition = static_cast<uint16_t>(definition);
			ParseSkinPayload(CellString(result, row, c_skin), skin.paint, skin.seed, skin.wear);
			skin.quality = CellInt(result, row, c_st) == 1 ? kStatTrakQuality : 0;
			skin.stattrak = static_cast<int>(CellInt(result, row, c_stc));
			skin.nametag = CellString(result, row, c_tag);
			ParseStickerIds(CellString(result, row, c_stick), skin.stickers);
			if (const std::optional<Keychain> keychain = ParseKeychainPayload(CellString(result, row, c_key))) {
				skin.keychain = *keychain;
			}
			rows[RowKey(teamIndex, static_cast<int>(definition))] = std::move(skin);
		}
		if (impl->ext == ExtState::Ready) {
			return Query("SELECT team, weapon_index, quality, sticker_4, sticker_5, stickers_full, keychain_seed FROM " +
							 impl->ExtTable() + " WHERE player_id = ? AND server_id = ?",
						 plgdb::Params{static_cast<int64_t>(playerId), static_cast<int64_t>(*impl->serverId)},
						 StageExtRows);
		}
		return LoadItems();
	}

	void OnExtRows(const plgdb::Result& result) {
		if (!result.Ok()) {
			// The ext table disappeared or cannot be read: keep the base data, and let the next
			// operation retry it (the create/read state machine is shared with the CREATE path).
			impl->ext = ExtState::Failed;
			return LoadItems();
		}
		const std::optional<size_t> c_team = result.ColumnIndex("team");
		const std::optional<size_t> c_def = result.ColumnIndex("weapon_index");
		const std::optional<size_t> c_quality = result.ColumnIndex("quality");
		const std::optional<size_t> c_st4 = result.ColumnIndex("sticker_4");
		const std::optional<size_t> c_st5 = result.ColumnIndex("sticker_5");
		const std::optional<size_t> c_full = result.ColumnIndex("stickers_full");
		const std::optional<size_t> c_seed = result.ColumnIndex("keychain_seed");
		for (size_t row = 0; row < result.RowCount(); ++row) {
			const int teamIndex = DbTeamIndex(CellInt(result, row, c_team)); // Pisex 0/1
			const int64_t definition = CellInt(result, row, c_def);
			if (teamIndex < 0 || definition <= 0 || definition > 0xFFFF) {
				continue;
			}
			// SAME key space as OnSkins: (team, definition); ext rows without a base row are
			// ignored, exactly like the original's per-skin ReadExtAsync.
			const auto found = rows.find(RowKey(teamIndex, static_cast<int>(definition)));
			if (found == rows.end()) {
				continue; // no base row: nothing the site would show either
			}
			Skin& skin = found->second;
			// Quality: the website's stattrack flag is authoritative. This backend no longer
			// rewrites ext rows (SaveStatTrak only adds to the counter), so an ext row can be
			// older than the base table - it may add a quality the base cannot express
			// (Souvenir, Unusual, ...) but must never switch a StatTrak weapon back to Normal.
			if (skin.quality != kStatTrakQuality) {
				const int extraQuality = static_cast<int>(CellInt(result, row, c_quality, skin.quality));
				// The site's base flag also wins when StatTrak was turned OFF. Other
				// qualities (e.g. Souvenir) still come from the additive table.
				if (extraQuality != kStatTrakQuality) {
					skin.quality = extraQuality;
				}
			}
			if (const std::optional<Sticker> sticker = ParseStickerFull(CellString(result, row, c_st4))) {
				skin.stickers[4] = *sticker;
			}
			if (const std::optional<Sticker> sticker = ParseStickerFull(CellString(result, row, c_st5))) {
				skin.stickers[5] = *sticker;
			}
			const std::string full = CellString(result, row, c_full);
			if (!full.empty()) {
				const nlohmann::json parsed = nlohmann::json::parse(full, nullptr, false);
				if (parsed.is_array() && parsed.size() >= 4) {
					for (size_t slot = 0; slot < 4; ++slot) {
						const nlohmann::json& entry = parsed[slot];
						if (!entry.is_string()) {
							continue;
						}
						const std::string text = entry.get<std::string>();
						if (const std::optional<Sticker> sticker = ParseStickerFull(text)) {
							// Website IDs in sc_skins are authoritative. Old extension geometry
							// belongs only to the SAME sticker; never resurrect a removed one.
							if (sticker->id == skin.stickers[slot].id) {
								skin.stickers[slot] = *sticker;
							}
						}
					}
				}
			}
			const int64_t seed = CellInt(result, row, c_seed);
			if (skin.keychain.id != 0 && seed != 0) {
				skin.keychain.seed = static_cast<int>(seed);
			}
		}
		return LoadItems();
	}

	void LoadItems() {
		return Query("SELECT team, agent, music, coin, knife, glove FROM " + impl->Table("items") +
						 " WHERE player_id = ? AND server_id = ? ORDER BY team ASC",
					 plgdb::Params{static_cast<int64_t>(playerId), static_cast<int64_t>(*impl->serverId)}, StageItems);
	}

	void OnItems(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Fail(ExtractError(result, "reading " + impl->Table("items")));
		}
		const std::optional<size_t> c_team = result.ColumnIndex("team");
		const std::optional<size_t> c_agent = result.ColumnIndex("agent");
		const std::optional<size_t> c_music = result.ColumnIndex("music");
		const std::optional<size_t> c_coin = result.ColumnIndex("coin");
		const std::optional<size_t> c_knife = result.ColumnIndex("knife");
		const std::optional<size_t> c_glove = result.ColumnIndex("glove");
		for (size_t row = 0; row < result.RowCount(); ++row) {
			const int teamIndex = DbTeamIndex(CellInt(result, row, c_team)); // Pisex 0/1
			if (teamIndex < 0) {
				continue;
			}
			TeamItems& team = snapshot.teams[static_cast<size_t>(teamIndex)];
			team.agent = static_cast<int>(CellInt(result, row, c_agent));
			if (impl->config.pinsEnabled) {
				team.pin = static_cast<int>(CellInt(result, row, c_coin));
			}
			// Music has no per-team meaning: the original took the first non-zero kit, team 0
			// first (the rows are ordered by team).
			const int music = static_cast<int>(CellInt(result, row, c_music));
			if (snapshot.music == 0 && music != 0) {
				snapshot.music = music;
			}
			const int64_t knife = CellInt(result, row, c_knife);
			if (knife > 0 && knife <= 0xFFFF) {
				if (const auto found = rows.find(RowKey(teamIndex, static_cast<int>(knife))); found != rows.end()) {
					team.knife = found->second;
				}
			}
			const int64_t glove = CellInt(result, row, c_glove);
			if (glove > 0 && glove <= 0xFFFF) {
				if (const auto found = rows.find(RowKey(teamIndex, static_cast<int>(glove))); found != rows.end()) {
					Skin skin = found->second;
					// GloveData in the original carries paint, seed and wear only.
					skin.quality = 0;
					skin.stattrak = 0;
					skin.nametag.clear();
					skin.stickers = {};
					skin.keychain = {};
					team.glove = std::move(skin);
				}
			}
		}
		// Weapon skins: everything that is not a knife or gloves. Knife and glove rows are also
		// present in sc_skins (keyed by their definition index) but must not appear as weapons.
		for (const auto& [key, skin] : rows) {
			(void)key;
			const int teamIndex = static_cast<int>(key >> 16);
			if (!IsWeaponDefinition(skin.definition)) {
				continue;
			}
			snapshot.teams[static_cast<size_t>(teamIndex)].weapons[skin.definition] = skin;
		}
		return Done(std::move(snapshot), std::string());
	}

	void Done(Snapshot result, std::string error) {
		if (delivered) {
			return;
		}
		Complete();
		result.steamId = steamId;
		if (callback) {
			callback(std::move(result), std::move(error));
		}
	}
};

// --- SaveStatTrak ----------------------------------------------------------------------------

struct Storage::Impl::SaveOp final : Op {
	int pisexTeam = -1;
	uint16_t definition = 0;
	int delta = 0;
	SaveCallback callback;

	enum SaveStage {
		StageUpdate = 200,
		StageVerify,
	};

	void Begin() { return ResolveServer(); }

	void ServerReady() override { return ResolvePlayer(); }

	void PlayerReady() override {
		// Delta only: stattrack_count = stattrack_count + delta never touches the columns the
		// website owns, and two servers (or a website edit) cannot overwrite each other's counter.
		Query("UPDATE " + impl->Table("skins") +
				  " SET stattrack_count = stattrack_count + ? WHERE player_id = ? AND server_id = ? AND team = ? AND "
				  "weapon_index = ?",
			  plgdb::Params{static_cast<int64_t>(delta), static_cast<int64_t>(playerId),
							static_cast<int64_t>(*impl->serverId), static_cast<int64_t>(pisexTeam),
							static_cast<int64_t>(definition)},
			  StageUpdate);
	}

	void PlayerMissing() override {
		Done(false, "Pisex: no " + impl->Table("player") + " row for " + SteamIdText());
	}

	void Fail(std::string message) override { Done(false, std::move(message)); }

	void OnOwnResult(const plgdb::Result& result) override {
		switch (stage) {
		case StageUpdate:
			return OnUpdate(result);
		case StageVerify:
			return OnVerify(result);
		default:
			return Done(false, "Pisex: internal save stage error");
		}
	}

	void OnUpdate(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Done(false, ExtractError(result, "updating " + impl->Table("skins")));
		}
		if (result.AffectedRows() > 0) {
			return Done(true, std::string());
		}
		// Affected rows of 0 means "nothing changed" (delta 0) or "no row" - plgdb connects
		// without CLIENT_FOUND_ROWS, so this must be verified, never answered with an INSERT.
		Query("SELECT 1 FROM " + impl->Table("skins") +
				  " WHERE player_id = ? AND server_id = ? AND team = ? AND weapon_index = ? LIMIT 1",
			  plgdb::Params{static_cast<int64_t>(playerId), static_cast<int64_t>(*impl->serverId),
							static_cast<int64_t>(pisexTeam), static_cast<int64_t>(definition)},
			  StageVerify);
	}

	void OnVerify(const plgdb::Result& result) {
		if (!result.Ok()) {
			return Done(false, ExtractError(result, "verifying " + impl->Table("skins")));
		}
		if (result.RowCount() == 0) {
			return Done(false, "Pisex: no " + impl->Table("skins") + " row for weapon " + std::to_string(definition));
		}
		return Done(true, std::string());
	}

	void Done(bool ok, std::string message) {
		if (delivered) {
			return;
		}
		Complete();
		if (callback) {
			callback(ok, std::move(message));
		}
	}
};

// --- Storage ---------------------------------------------------------------------------------

Storage::Storage(PisexConfig config, std::unique_ptr<plgdb::Database> database)
	: impl_(std::make_unique<Impl>(std::move(config), std::move(database))) {}

Storage::~Storage() = default;

void Storage::LoadPlayer(uint64_t steamId, LoadCallback callback) {
	if (!impl_) {
		return;
	}
	auto op = std::make_shared<Impl::LoadOp>();
	op->impl = impl_.get();
	op->steamId = steamId;
	op->callback = std::move(callback);
	++impl_->operations;

	if (!impl_->prefixOk) {
		impl_->deferred.push_back(
			[op] { op->Done(Snapshot{}, "Pisex: invalid TablePrefix '" + op->impl->config.tablePrefix + "'"); });
		return;
	}
	if (!impl_->db) {
		impl_->deferred.push_back([op] { op->Done(Snapshot{}, std::string("Pisex: no database connection")); });
		return;
	}
	op->Begin();
}

void Storage::SaveStatTrak(uint64_t steamId, int team, uint16_t definition, int delta, SaveCallback callback) {
	if (!impl_) {
		return;
	}
	const int pisexTeam = TeamIndex(team);
	auto op = std::make_shared<Impl::SaveOp>();
	op->impl = impl_.get();
	op->steamId = steamId;
	op->pisexTeam = pisexTeam;
	op->definition = definition;
	op->delta = delta;
	op->callback = std::move(callback);
	++impl_->operations;

	if (!impl_->prefixOk) {
		impl_->deferred.push_back(
			[op] { op->Done(false, "Pisex: invalid TablePrefix '" + op->impl->config.tablePrefix + "'"); });
		return;
	}
	if (!impl_->db) {
		impl_->deferred.push_back([op] { op->Done(false, std::string("Pisex: no database connection")); });
		return;
	}
	if (pisexTeam < 0) {
		impl_->deferred.push_back([op] { op->Done(false, std::string("Pisex: invalid team for SaveStatTrak")); });
		return;
	}
	op->Begin();
}

size_t Storage::Poll() {
	if (!impl_) {
		return 0;
	}
	size_t ran = impl_->db ? impl_->db->Poll() : 0;

	// Validation failures: delivered from here so that no caller is re-entered inside
	// LoadPlayer/SaveStatTrak. An exception must not escape into the plugin's frame loop.
	std::deque<std::function<void()>> batch;
	batch.swap(impl_->deferred);
	for (auto& completion : batch) {
		++ran;
		try {
			completion();
		} catch (...) {
			// deliberately swallowed, like plgdb does for statement callbacks
		}
	}
	return ran;
}

size_t Storage::Pending() const {
	if (!impl_) {
		return 0;
	}
	// Operations whose final callback has not been delivered (their statements are on the plgdb
	// worker between chain steps, where db->Pending() alone would read 0).
	return impl_->operations;
}

} // namespace ws
