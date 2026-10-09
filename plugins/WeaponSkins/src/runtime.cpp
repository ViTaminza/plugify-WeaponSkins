// WeaponSkins — the lifecycle runtime (see runtime.hpp for the design notes).
//
// Port map (original -> here):
//   StorageService.Configure / DatabaseSynchronizeService  -> Start()/RequestLoad/OnLoaded
//   HookInventoryUpdateService.OnPlayerSpawn + PisexPinService.OnPlayerSpawn -> OnSpawnEvent
//   HooKInventoryUpdateService.OnRoundMvp (MusicKitMvpAnthem) -> OnRoundMvpEvent
//   StattrakService.OnPlayerDeath                          -> OnDeathEvent
//   PisexSyncCommandService (aliases + !wp)                -> RegisterCommands/OnSyncCommand
//   PlayerService + the auth reconcile                     -> Reconcile/BindPlayer
//
// Everything runs on the game thread. The database is plgdb: the worker only executes the SQL,
// the callbacks are delivered by Storage::Poll() from Update(). No lock is taken anywhere.

#include "runtime.hpp"

#include <pisex/pisex.hpp>
#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <utility>

namespace ws {

namespace {

// One non-empty plg::Log call; the host may run with logging disabled.
void Log(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) { Log(message, plg::Severity::Info); }
void LogWarn(std::string_view message) { Log(message, plg::Severity::Warning); }
void LogError(std::string_view message) { Log(message, plg::Severity::Error); }

std::string JoinPath(const std::filesystem::path& directory, std::string_view file) {
	return (directory / std::filesystem::path(file)).string();
}

} // namespace

// The callbacks handed to s2sdk/pisexcompat are plain function pointers, so they live at namespace
// scope (defined after Impl, which registers them).
void OnClientDisconnectTrampoline(int32_t playerSlot, s2sdk::NetworkDisconnectionReason reason);
void OnMapStartTrampoline();
void OnMapEndTrampoline();
void OnSpawnTrampoline(const char* name, pisex::IGameEvent* event, bool dontBroadcast);
void OnDeathTrampoline(const char* name, pisex::IGameEvent* event, bool dontBroadcast);
void OnRoundStartTrampoline(const char* name, pisex::IGameEvent* event, bool dontBroadcast);
s2sdk::ResultType OnRoundMvpTrampoline(const plg::string& name, void* event, bool dontBroadcast);

struct Runtime::Impl {
	// --- configuration and paths ------------------------------------------------------------
	Config config;
	Translations translations;
	std::filesystem::path configDir;
	std::filesystem::path translationsDir;
	std::filesystem::path gamedataPath;
	std::string pluginLocation;

	// --- storage ------------------------------------------------------------------------------
	std::unique_ptr<plgdb::Database> db;
	std::unique_ptr<Storage> storage;
	bool storageReady = false;
	std::string storageError;

	// --- economy / engine ---------------------------------------------------------------------
	EconData econ;
	bool econReady = false;
	std::string econError;
	std::unique_ptr<Engine> engine;
	bool engineReady = false;   // Prepare() succeeded: the apply paths may run
	bool hookInstalled = false; // the GiveNamedItem detour is live
	std::string engineError;

	// --- players ------------------------------------------------------------------------------
	// A load waits for acknowledged StatTrak writes before acquiring the read/write barrier.
	struct PendingLoad {
		bool pending = false;
		bool sync = false;	// came from !wp: answer with a message and apply when it lands
		bool force = false; // re-load even when the cache already has data
		bool flushed = false; // the one immediate StatTrak drain-flush of this request already ran
		rt::Clock::time_point deadline{};
	};

	std::array<rt::SlotBinding, rt::kSlots> slots{};
	std::unordered_map<uint64_t, rt::PlayerCache> players;
	std::array<uint64_t, rt::kSlots> latestRequest{};
	std::array<PendingLoad, rt::kSlots> loadRequests{};
	uint64_t cacheToken = 0;
	uint64_t requestCounter = 0;
	uint64_t mapGeneration = 1;
	rt::Cooldowns cooldowns;

	// --- mvp ------------------------------------------------------------------------------------
	bool mvpEnabled = false;
	bool mvpPlayed = false;

	// --- lifecycle ------------------------------------------------------------------------------
	bool started = false;
	bool mapReady = false;
	bool disconnectListenerRegistered = false;
	bool mapListenersRegistered = false;
	bool mvpHookRegistered = false;
	int prepareAttempts = 0;
	int econAttempts = 0;
	// Throttle of the Update prepare retry (one StartEngineWithMap attempt per second).
	std::chrono::milliseconds sinceEngineRetry{0};
	static constexpr int kMaxPrepareAttempts = 3;
	static constexpr int kMaxEconAttempts = 3;
	static constexpr int kEngineRetrySeconds = 1;
	// A drain that cannot finish in this window fails the load; no SELECT is issued with debt.
	static constexpr int64_t kLoadDrainTimeoutMs = 2000;
	std::chrono::milliseconds sinceReconcile{0};
	bool syncOnJoinNoted = false;
	bool redirectOffNoted = false;

	// --- stats (ws_status) ------------------------------------------------------------------------
	uint64_t statLoadRequests = 0;
	uint64_t statLoadsOk = 0;
	uint64_t statLoadsFailed = 0;
	uint64_t statApplies = 0;
	uint64_t statKills = 0;
	uint64_t statDeltasQueued = 0;
	uint64_t statDeltasSaved = 0;
	uint64_t statDeltasFailed = 0;
	uint64_t statMvpReplays = 0;
	uint64_t statPickupApplies = 0;
	std::string lastError;

	// -------------------------------------------------------------------------------------------
	// paths / loading
	// -------------------------------------------------------------------------------------------
	void ResolvePaths(std::string_view location) {
		pluginLocation.assign(location);
		configDir = std::filesystem::path(std::string(pisex::ConfigDir()));
		// The package layout is configs/WeaponSkins/{config.toml,database.jsonc,assets/gamedata.jsonc,
		// resources/translations}; a bare translations/ directory is accepted as well.
		const std::filesystem::path resources = configDir / "resources" / "translations";
		if (std::filesystem::exists(resources)) {
			translationsDir = resources;
		} else if (!pisex::TranslationsDir().empty() && std::filesystem::exists(pisex::TranslationsDir())) {
			translationsDir = pisex::TranslationsDir();
		} else {
			translationsDir = configDir / "translations";
		}

		std::vector<std::filesystem::path> candidates;
		if (!pluginLocation.empty()) {
			const std::filesystem::path pluginPath(pluginLocation);
			candidates.push_back(pluginPath / "gamedata.jsonc");
			candidates.push_back(pluginPath / "assets" / "gamedata.jsonc");
			// GetLocation() is the plugin's own file (…/WeaponSkins/bin/<lib>): the package root
			// and its assets/ sit one level up.
			candidates.push_back(pluginPath.parent_path() / "gamedata.jsonc");
			candidates.push_back(pluginPath.parent_path() / "assets" / "gamedata.jsonc");
		}
		candidates.push_back(configDir / "assets" / "gamedata.jsonc");
		candidates.push_back(configDir / "gamedata.jsonc");
		for (const std::filesystem::path& candidate : candidates) {
			if (!candidate.empty() && std::filesystem::exists(candidate)) {
				gamedataPath = candidate;
				break;
			}
		}
	}

	bool LoadConfiguration() {
		std::string error;
		const std::string path = JoinPath(configDir, "config.toml");
		if (!ws::LoadConfig(path, config, error)) {
			// A missing config is not fatal: the defaults of models.hpp are the production values.
			LogWarn("WeaponSkins: " + path + " not loaded (" + error + ") - using the built-in production defaults");
			return false;
		}
		LogInfo("WeaponSkins: config loaded from " + path);
		return true;
	}

	void LoadTranslations() {
		std::string error;
		if (!translations.Load(translationsDir, error)) {
			LogWarn("WeaponSkins: translations not loaded from " + translationsDir.string() +
					(error.empty() ? std::string() : " (" + error + ")"));
		}
	}

	// The connection comes from the standard's databases.cfg first (docs/PLUGIN_STANDARD.md §4:
	// <GetConfigsDir()>/WeaponSkins/databases.cfg, section Main.Pisex.ConnectionName); the legacy
	// SwiftlyS2 configs/WeaponSkins/database.jsonc is still read while a server has not been
	// migrated to it. Credentials are never logged (only the non-secret target is).
	void OpenStorage() {
		// pisexcompat maps the Metamod spelling onto the plugin config dir (SetPluginName ran in
		// Start) and returns the expected location even while the file does not exist yet.
		const std::string databasesPath = pisex::ResolvePath("addons/configs/databases.cfg");
		const std::string legacyPath = JoinPath(configDir, "database.jsonc");
		bool usedLegacy = false;
		std::string error;
		const std::optional<plgdb::Config> database =
			ws::LoadPluginDatabase(databasesPath, legacyPath, config.pisex.connectionName, usedLegacy, error);
		if (usedLegacy) {
			// The one warning of the transition period: the connection still comes from the
			// pre-standard file and must be moved to databases.cfg.
			LogWarn("WeaponSkins: the database connection was read from " + legacyPath +
					" - move it to " + databasesPath + " (section '" + config.pisex.connectionName + "')");
		}
		if (!database) {
			storageError = "database config not available (" + error + ")";
			LogError("WeaponSkins: " + storageError + " - skins will not load until it is fixed");
			return;
		}

		plgdb::Config databaseConfig = *database;
		if (databaseConfig.driver == plgdb::Driver::SQLite && !databaseConfig.sqlitePath.empty()) {
			// Same rule as admin_system/levels_ranks: a relative sqlite_path lives in data/<name>/.
			databaseConfig.sqlitePath = pisex::ResolveDataPath(databaseConfig.sqlitePath);
		}
		db = plgdb::Database::Create(databaseConfig);
		if (db == nullptr) {
			storageError = "database connection '" + config.pisex.connectionName + "' could not be created";
			LogError("WeaponSkins: " + storageError);
			return;
		}

		storage = std::make_unique<Storage>(config.pisex, std::move(db));
		storageReady = true;
		if (databaseConfig.driver == plgdb::Driver::MySQL) {
			LogInfo("WeaponSkins: storage 'pisex' on " + databaseConfig.host + ":" +
					std::to_string(databaseConfig.port) + "/" + databaseConfig.database + ", serverId=" +
					std::to_string(config.pisex.serverId) + ", prefix=" + config.pisex.tablePrefix);
		} else {
			LogInfo("WeaponSkins: storage 'pisex' on " + databaseConfig.sqlitePath + ", serverId=" +
					std::to_string(config.pisex.serverId) + ", prefix=" + config.pisex.tablePrefix);
		}
	}

	// The economy data is read from the game files (VPKs) — file I/O only, so it may run before a
	// map exists. If it fails at plugin start it is retried once a map is up (a plugin loaded before
	// the game files are mounted would fail here).
	bool TryLoadEcon() {
		if (econReady) {
			return true;
		}
		std::string error;
		// The item names (resource/csgo_*.txt, all 29 languages ~158 MB with ItemLanguages = [])
		// are not loaded: only the reference's in-game menus displayed them, and those are not
		// ported (the menu lives on the website). Parsing them cost ~8 s of the game thread at
		// every server start. Load them again (true) once something here shows item names.
		if (!econ.LoadFromGame(config.itemLanguages, error, /*loadLanguages=*/false)) {
			econError = error;
			return false;
		}
		econReady = true;
		econError.clear();
		return true;
	}

	// --- engine --------------------------------------------------------------------------------
	// Prepare()/InstallHook() are the only schema/gamedata work and they run with a live map and
	// pisexcompat's readiness granted (PITFALLS.md §B.1/§C.8). The engine and its detour are
	// created once per plugin load; only Prepare() is retried (a map that is still settling).
	void StartEngineWithMap() {
		if (engineReady) {
			return;
		}
		// The runtime's own s2sdk OnMapStart listener calls this before pisexcompat has granted
		// readiness (its ready flag only flips in Poll, which runs after the engine's whole
		// map-start dispatch), and no schema offset may be resolved without a live map
		// (PITFALLS §C.8). That is not a failure and must not spend an attempt: the pisexcompat
		// map-start callback and the Update retry below run with readiness granted and drive the
		// real attempt (live bug: every map start used to fail here forever).
		if (!pisex::IsGameReady()) {
			return;
		}
		if (!econReady) {
			// The econ data is VPK file I/O; a plugin loaded before the game files are visible fails
			// here and is retried while a map is up (bounded, with a final clear error).
			if (econAttempts >= kMaxEconAttempts) {
				return;
			}
			++econAttempts;
			if (!TryLoadEcon()) {
				LogError("WeaponSkins: econ load attempt " + std::to_string(econAttempts) + "/" +
						 std::to_string(kMaxEconAttempts) + " failed (" + econError + ")" +
						 (econAttempts >= kMaxEconAttempts ? " - the skinchanger stays inert" : " - retried while a map is up"));
				return;
			}
		}
		if (engine == nullptr) {
			engine = std::make_unique<Engine>(config, econ);
		}
		// The hook is re-installed after every map end (Engine::Shutdown removed it there, and its
		// gamedata/offsets are per map); InstallHook is a no-op while the detour is live.
		if (!hookInstalled) {
			if (gamedataPath.empty()) {
				LogError("WeaponSkins: gamedata.jsonc not found next to the plugin or in " + configDir.string() +
						 " - the GiveNamedItem hook stays off");
			} else {
				std::string hookError;
				if (engine->InstallHook(gamedataPath, [this](int slot) { return LookupSnapshot(slot); }, hookError)) {
					hookInstalled = true;
					LogInfo("WeaponSkins: CCSPlayer_ItemServices::GiveNamedItem hooked");
				} else {
					LogError("WeaponSkins: InstallHook failed: " + hookError +
							 " - weapon skins are only applied on spawn and by !wp");
				}
			}
		}

		std::string error;
		if (!engine->Prepare(error)) {
			++prepareAttempts;
			engineError = error;
			if (prepareAttempts >= kMaxPrepareAttempts) {
				LogError("WeaponSkins: Engine::Prepare failed " + std::to_string(prepareAttempts) +
						 " times (" + error + ") - applying is disabled for this run");
			} else {
				LogWarn("WeaponSkins: Engine::Prepare failed (" + error + ") - retried while a map is up");
			}
			return;
		}
		engineReady = true;
		LogInfo("WeaponSkins: engine ready (hook " + std::string(hookInstalled ? "installed" : "off") +
				", mvpHook " + std::string(mvpEnabled ? "on" : "off (MusicKitMvpAnthem=false)") + ")");
	}

	// -------------------------------------------------------------------------------------------
	// player cache
	// -------------------------------------------------------------------------------------------
	rt::PlayerCache* FindCache(uint64_t steamId) {
		const auto it = players.find(steamId);
		return it == players.end() ? nullptr : &it->second;
	}

	const rt::PlayerCache* FindCache(uint64_t steamId) const {
		const auto it = players.find(steamId);
		return it == players.end() ? nullptr : &it->second;
	}

	void EvictOneUnbound() {
		uint64_t victim = 0;
		rt::Clock::time_point oldest = rt::Clock::time_point::max();
		for (const auto& [steamId, cache] : players) {
			// An entry that still owes StatTrak writes is never evicted (the kills would be lost).
			if (!rt::CacheDroppable(cache.ledger)) {
				continue;
			}
			bool bound = false;
			for (const rt::SlotBinding& binding : slots) {
				if (binding.steamId == steamId) {
					bound = true;
					break;
				}
			}
			if (!bound && cache.lastUse < oldest) {
				oldest = cache.lastUse;
				victim = steamId;
			}
		}
		if (victim != 0) {
			players.erase(victim);
		}
	}

	rt::PlayerCache& TouchCache(uint64_t steamId) {
		auto it = players.find(steamId);
		if (it == players.end()) {
			if (players.size() >= rt::kMaxCachedPlayers) {
				EvictOneUnbound();
			}
			it = players.emplace(steamId, rt::PlayerCache{}).first;
			it->second.stored.steamId = steamId;
			it->second.view.steamId = steamId;
			it->second.token = ++cacheToken;
		}
		it->second.lastUse = rt::Clock::now();
		return it->second;
	}

	bool Allowed(uint64_t steamId, const std::string& permission) const {
		if (permission.empty()) {
			return true;
		}
		// The permissions plugin is an optional dependency: without it every check passes, which is
		// exactly what an empty ItemPermissions configuration means (the production config).
		if (__permissions_HasPermission == nullptr) {
			return true;
		}
		try {
			return permissions::HasPermission(steamId, plg::string(permission)) == permissions::Status::Allow;
		} catch (...) {
			return true; // fail open: a permissions failure must not strip a paying player's skins
		}
	}

	void PrepareCache(rt::PlayerCache& cache, uint64_t steamId) {
		rt::BuildView(cache.stored, config.permissions, [this, steamId](const std::string& permission) {
			return Allowed(steamId, permission);
		}, cache.view, cache.stickerHashes);
	}

	// -------------------------------------------------------------------------------------------
	// loads
	// -------------------------------------------------------------------------------------------
	bool CheckGuard(const rt::SlotGuard& guard) const {
		return rt::GuardMatches(guard, slots, mapGeneration);
	}

		// Reads begin with an empty ledger and block subsequent writes until their current response.
		// A !wp during a live read joins it; its message/apply flags survive ordinary reconcile calls.
	void RequestLoad(int slot, uint64_t steamId, bool force, bool sync) {
		if (!storageReady || storage == nullptr || slot < 0 || slot >= rt::kSlots) {
			if (sync) {
				Say(slot, "sync.error", config.commands.errorMessage, {{"url", config.commands.siteUrl}});
				cooldowns.Clear(steamId);
			}
			return;
		}
		rt::SlotBinding& binding = slots[slot];
		if (binding.steamId != steamId) {
			return;
		}
		const rt::Clock::time_point now = rt::Clock::now();
			PendingLoad& request = loadRequests[slot];
				if (rt::JoinPlayerLoad(binding, force, sync, now, request.force, request.sync)) {
				return;
			}
		if (!rt::ShouldRequestLoad(binding, force, now)) {
			return;
		}
			if (binding.loadRequested) {
				// The old read may still finish its multi-statement chain. Invalidate it before
				// releasing writes, then drain those writes before a replacement read starts.
				rt::InvalidatePlayerLoad(binding, latestRequest[slot]);
				request.force = true;
			}

		TouchCache(steamId);
			if (!request.pending) {
			// This is a new request: give the outstanding StatTrak writes a bounded amount of time to
			// be acknowledged before the load is issued.
				request.deadline = now + std::chrono::milliseconds(kLoadDrainTimeoutMs);
				request.flushed = false;
		}
			request.pending = true;
			request.sync = request.sync || sync;
			request.force = request.force || force;
		// Try right now: when the ledger is already empty this issues the load in this tick.
		PumpLoadRequest(slot);
	}

		// Issues only after all prior writes were acknowledged. A drain timeout preserves the cache
		// and its debt, reports the failure, and leaves ordinary load retries paced.
	void PumpLoadRequest(int slot) {
		PendingLoad& request = loadRequests[slot];
		if (!request.pending || storage == nullptr || !storageReady) {
			return;
		}
		rt::SlotBinding& binding = slots[slot];
		if (binding.steamId == 0 || !pisex::IsGameReady()) {
			return;
		}
		const uint64_t steamId = binding.steamId;
		rt::PlayerCache* cache = FindCache(steamId);
			if (cache == nullptr || binding.loadRequested) {
				return;
			}
			if (!cache->ledger.Empty()) {
			// One immediate drain-flush, then only the paced retries (PumpDeltas paces them, a broken
			// database must not be hammered by a waiting load).
			if (!request.flushed) {
				request.flushed = true;
				FlushDeltas(steamId, /*finalFlush=*/true);
			} else {
				FlushDeltas(steamId, /*finalFlush=*/false);
			}
		}
			const rt::Clock::time_point now = rt::Clock::now();
			const rt::LoadDrainState drain = rt::CheckLoadDrain(cache->ledger, now, request.deadline);
			if (drain == rt::LoadDrainState::Waiting) {
				return;
			}
			if (drain == rt::LoadDrainState::TimedOut) {
				const bool fromSync = request.sync;
				request = PendingLoad{};
				rt::InvalidatePlayerLoad(binding, latestRequest[slot]);
				binding.nextRetry = now + std::chrono::seconds(rt::kLoadRetrySeconds);
				++statLoadsFailed;
				lastError = "StatTrak writes were not acknowledged before the load drain deadline";
				LogWarn("WeaponSkins: load of " + std::to_string(steamId) + " postponed: " + lastError);
				if (fromSync) {
					Say(slot, "sync.error", config.commands.errorMessage, {{"url", config.commands.siteUrl}});
					cooldowns.Clear(steamId);
				}
				return;
			}
		if (binding.loaded && !request.force) {
				request = PendingLoad{};
			return;
		}

		const uint64_t requestId = ++requestCounter;
			if (!rt::BeginPlayerLoad(binding, cache->ledger, requestId, latestRequest[slot],
								 now + std::chrono::seconds(3 * rt::kLoadRetrySeconds))) {
				return;
			}
			request.pending = false;
		++statLoadRequests;

		const uint64_t generation = binding.generation;
		const uint64_t mapGen = mapGeneration;
		storage->LoadPlayer(steamId, [this, slot, steamId, generation, mapGen, requestId](Snapshot snapshot,
																						std::string error) {
			OnLoaded(slot, steamId, generation, mapGen, requestId, std::move(snapshot), std::move(error));
		});
	}

	// The plgdb callback: the only place a database answer enters the game state. It may arrive
	// long after the request, so the slot, the steam id, the slot generation, the map generation
	// and the request id are all re-checked first (a late answer must never be applied to the
	// player who took the slot over).
	void OnLoaded(int slot, uint64_t steamId, uint64_t generation, uint64_t mapGen, uint64_t requestId,
				  Snapshot snapshot, std::string error) {
			if (!started || slot < 0 || slot >= rt::kSlots || !rt::ResponseIsCurrent(requestId, latestRequest[slot]) ||
			!CheckGuard(rt::SlotGuard{slot, steamId, generation, mapGen})) {
			return; // superseded, or the player/slot is gone
		}

		const bool fromSync = loadRequests[slot].sync;
		loadRequests[slot].sync = false;
		loadRequests[slot].force = false;
		loadRequests[slot].pending = false;
		rt::SlotBinding& binding = slots[slot];
		rt::PlayerCache& cache = TouchCache(steamId);

		if (!error.empty()) {
				rt::InvalidatePlayerLoad(binding, latestRequest[slot]);
			binding.nextRetry = rt::Clock::now() + std::chrono::seconds(rt::kLoadRetrySeconds);
			++statLoadsFailed;
			lastError = error;
			LogError("WeaponSkins: load of " + std::to_string(steamId) + " (slot " + std::to_string(slot) +
					 ") failed: " + error + " - retrying in " + std::to_string(rt::kLoadRetrySeconds) + "s");
			if (fromSync) {
				Say(slot, "sync.error", config.commands.errorMessage, {{"url", config.commands.siteUrl}});
				cooldowns.Clear(steamId);
			}
				FlushDeltas(steamId, /*finalFlush=*/true);
			return;
		}

		cache.stored = std::move(snapshot);
		cache.stored.steamId = steamId;
			rt::MergePendingCounters(cache.ledger, cache.stored);
		cache.loaded = true;
		PrepareCache(cache, steamId);
		binding.loaded = true;
			rt::InvalidatePlayerLoad(binding, latestRequest[slot]);
		binding.nextRetry = {};
		++statLoadsOk;
			FlushDeltas(steamId, /*finalFlush=*/true);

		if (fromSync) {
			if (cache.stored.Empty()) {
				// Nothing configured: answer with the empty message and let the player retry at once
				// (PisexSyncCommandService.cs:119-124). Snapshot::Empty() also covers agents and
				// pins, which the original's check ignored.
				cooldowns.Clear(steamId);
				Say(slot, "sync.empty", config.commands.emptyMessage, {{"url", config.commands.siteUrl}});
			} else {
				Say(slot, "sync.done", config.commands.doneMessage, {{"url", config.commands.siteUrl}});
			}
		}

		// The data is applied as soon as it exists for a player who is already alive: a !wp
		// (fromSync), a spawn that happened before the row was loaded (applyWhenLoaded), or a first
		// load that landed after both the spawn and the auth event (the once-per-second reconcile
		// may be the only thing that saw the player). Anything else - no map yet, no controller, a
		// bot, a dead/spectating player - is remembered: the spawn path applies everything,
		// including the music kit and the pin, exactly like the reference.
		const bool canApply = pisex::IsGameReady() && pisex::IsClientReady(slot) && !pisex::IsFakeClient(slot) &&
							  pisex::IsAlive(slot);
		if (canApply) {
			cache.applyWhenLoaded = false;
			ApplyPlayer(rt::SlotGuard{slot, steamId, generation, mapGen}, /*refresh=*/true, /*inventory=*/true);
		} else {
			cache.applyWhenLoaded = true;
		}
	}

	// -------------------------------------------------------------------------------------------
	// applying
	// -------------------------------------------------------------------------------------------
	// Applies the current cache to a live player. `inventory` re-applies the owned weapons (the
	// !wp path); `refresh` marks the deferred spawn passes for the engine. No pointer is captured:
	// only the slot, the steam id and the two generations, all re-validated here.
	void ApplyPlayer(const rt::SlotGuard& guard, bool refresh, bool inventory) {
		if (!engineReady || engine == nullptr || !started) {
			return;
		}
		if (!pisex::IsGameReady() || !CheckGuard(guard)) {
			return;
		}
		const int slot = guard.slot;
		if (!pisex::IsClientReady(slot) || pisex::IsFakeClient(slot)) {
			return;
		}
		if (pisex::GetSteamID64(slot) != guard.steamId) {
			return;
		}
		rt::PlayerCache* cache = FindCache(guard.steamId);
		if (cache == nullptr) {
			return;
		}
		if (!cache->loaded) {
			cache->applyWhenLoaded = true; // the load callback applies as soon as the row arrives
			return;
		}

		if (inventory) {
			// The !wp path and "a load landed while the player is awake": the engine applies the
			// whole snapshot and re-gives the held items, so a skin removed on the website actually
			// clears (the deferred regive of Engine::RefreshInventory is what does that).
			engine->RefreshInventory(slot, cache->view);
		} else {
			engine->ApplySpawn(slot, cache->view, refresh);
		}
		++statApplies;
	}

	// The engine's GiveNamedItem post hook asks for the *runtime view* of the owner's cache. Memory
	// only, no allocation, no database, no logging (handoff §I).
	const Snapshot* LookupSnapshot(int slot) const {
		if (slot < 0 || slot >= rt::kSlots) {
			return nullptr;
		}
		const rt::SlotBinding& binding = slots[slot];
		if (binding.steamId == 0) {
			return nullptr;
		}
		const rt::PlayerCache* cache = FindCache(binding.steamId);
		if (cache == nullptr || !cache->loaded || cache->view.steamId != binding.steamId) {
			return nullptr;
		}
		return &cache->view;
	}

	// -------------------------------------------------------------------------------------------
	// StatTrak
	// -------------------------------------------------------------------------------------------
	void AddDelta(uint64_t steamId, int team, uint16_t definition, int64_t delta) {
		rt::PlayerCache& cache = TouchCache(steamId);
		cache.ledger.Add(rt::DeltaKey(team, definition), delta);
		++statDeltasQueued;
	}

	// Persists the StatTrak kills that are not acknowledged yet. The write is a *delta*
	// (Storage::SaveStatTrak: `count = count + ?`), so a website change made while the player was in
	// the game is preserved.
	//
	// Accounting (the ledger): a batch is taken from the ledger's "owed" amount and marked in
	// flight; the acknowledgement subtracts it from "owed" only when the storage reports success.
		// A failed batch remains owed for retry. This prevents duplicate queueing while its result is
		// pending; an ambiguous UPDATE/ACK connection loss can still produce an at-least-once write.
		// Loads hold a barrier so no post-issue delta is present in their DB snapshots.
	void FlushDeltas(uint64_t steamId, bool finalFlush) {
		if (!storageReady || storage == nullptr) {
			return;
		}
		rt::PlayerCache* cache = FindCache(steamId);
		if (cache == nullptr) {
			return;
		}
			if (rt::LoadBlocksWrites(steamId, slots)) {
				return;
			}
		if (!finalFlush && rt::Clock::now() < cache->deltaNextRetry) {
			return;
		}
		if (cache->ledger.Empty()) {
			return;
		}
		cache->deltaNextRetry = rt::Clock::now() + std::chrono::seconds(rt::kDeltaRetrySeconds);

		const uint64_t token = cache->token;
			const std::vector<rt::DeltaBatch> batches = rt::QueuePlayerDeltas(cache->ledger, steamId, slots);
		for (const rt::DeltaBatch& batch : batches) {
			const int team = static_cast<int>(batch.key >> 32);
			const uint16_t definition = static_cast<uint16_t>(batch.key & 0xFFFFu);
			storage->SaveStatTrak(steamId, team, definition, batch.amount,
								  [this, steamId, batch, token](bool ok, std::string error) {
				rt::PlayerCache* current = FindCache(steamId);
				if (current == nullptr || current->token != token) {
					// The player disconnected and their entry was already drained; the statement ran
					// anyway (the worker finishes what it was given).
					if (ok) {
						++statDeltasSaved;
					}
					return;
				}
				current->ledger.Ack(batch, ok);
				if (ok) {
					++statDeltasSaved;
				} else {
					++statDeltasFailed;
					lastError = error;
					if (statDeltasFailed == 1 || statDeltasFailed % 30 == 0) {
						LogWarn("WeaponSkins: StatTrak save for " + std::to_string(steamId) + " failed: " + error +
								" - the kill stays owed and is retried");
					}
				}
			});
		}
	}

	// Drops caches that own no unpersisted StatTrak state and no slot. An entry with owed or
	// in-flight kills stays until the storage acknowledged them (the storage outlives the player).
	void PruneCaches() {
		for (auto it = players.begin(); it != players.end();) {
			const uint64_t steamId = it->first;
			bool bound = false;
			for (const rt::SlotBinding& binding : slots) {
				if (binding.steamId == steamId) {
					bound = true;
					break;
				}
			}
			if (!bound && rt::CacheDroppable(it->second.ledger)) {
				it = players.erase(it);
			} else {
				++it;
			}
		}
	}

	// -------------------------------------------------------------------------------------------
	// bind / unbind
	// -------------------------------------------------------------------------------------------
	void BindPlayer(int slot, uint64_t steamId) {
		if (!rt::BindSlot(slots[slot], steamId)) {
			return;
		}
		latestRequest[slot] = 0;
		loadRequests[slot] = PendingLoad{};
		TouchCache(steamId);
		if (config.syncOnJoin && !syncOnJoinNoted) {
			syncOnJoinNoted = true;
			// The original's SyncFromDatabaseWhenPlayerJoin ran a full-table preload; the storage
			// interface of this port loads per player, which is what the production configuration
			// (LazyLoadPerPlayer) does anyway. The flag therefore only logs.
			LogWarn("WeaponSkins: SyncFromDatabaseWhenPlayerJoin is ignored - this port loads per player");
		}
		// The lazy per-player load (production: LazyLoadPerPlayer = true and
		// SyncFromDatabaseWhenPlayerJoin = false; the lazy load is independent of both flags).
		RequestLoad(slot, steamId, false, false);
	}

	void DropPlayer(int slot, std::string_view reason) {
		const uint64_t steamId = rt::UnbindSlot(slots[slot]);
		latestRequest[slot] = 0;
		loadRequests[slot] = PendingLoad{};
		if (steamId == 0) {
			return;
		}
		// Flush the StatTrak debt right away. The cache is NOT erased here when kills are still
		// owed or in flight: PruneCaches() drops it as soon as the storage acknowledged everything
		// (the storage outlives the player; only this callback would be gone).
		FlushDeltas(steamId, /*finalFlush=*/true);
		rt::PlayerCache* cache = FindCache(steamId);
		if (cache != nullptr && rt::CacheDroppable(cache->ledger)) {
			players.erase(steamId);
		}
		(void)reason;
	}

	// -------------------------------------------------------------------------------------------
	// the reconcile
	// -------------------------------------------------------------------------------------------
	void Reconcile() {
		if (!pisex::IsGameReady()) {
			// Before the first map / between two maps: no client call may be made (PITFALLS §B.2).
			return;
		}
		const int max = std::min(pisex::GetMaxPlayers(), rt::kSlots);
		const rt::Clock::time_point now = rt::Clock::now();
		for (int slot = 0; slot < max; ++slot) {
			if (!pisex::IsConnected(slot)) {
				if (slots[slot].steamId != 0) {
					DropPlayer(slot, "client no longer connected (missed disconnect event)");
				}
				continue;
			}
			// Bots first: no SteamID, no net channel, no database row (PITFALLS §B.5/§I).
			if (pisex::IsFakeClient(slot)) {
				continue;
			}
			if (!pisex::IsClientReady(slot)) {
				continue; // still connecting: no controller entity yet
			}
			if (!pisex::IsAuthenticated(slot)) {
				continue; // a non-zero SteamID is not authentication in this stack
			}
			const uint64_t steamId = pisex::GetSteamID64(slot);
			if (steamId == 0) {
				continue;
			}
			if (slots[slot].steamId != steamId) {
				if (slots[slot].steamId != 0) {
					// The slot changed hands without a disconnect event (rare). Start over for the
					// newcomer instead of applying the previous player's data (perm_connector §RequestLoad).
					DropPlayer(slot, "slot taken over by another player");
				}
				BindPlayer(slot, steamId);
			}

			const rt::SlotBinding& binding = slots[slot];
			// A request that timed out (or was never answered) is superseded here: RequestLoad checks
			// the deadline itself, and the new request id invalidates the late answer.
			if (!binding.loaded && rt::ShouldRequestLoad(binding, false, now)) {
				RequestLoad(slot, steamId, false, false);
			}
		}

		// Drops the caches of players that left and whose StatTrak kills were all acknowledged.
		PruneCaches();
	}

	// -------------------------------------------------------------------------------------------
	// messages
	// -------------------------------------------------------------------------------------------
	// The message comes from the translations when the language file defines the key, and from the
	// configuration otherwise. Translations::Get answers the *key itself* for a missing phrase, and
	// the production files define the menu keys only - without the key==missing check every sync
	// message would print "sync.started" instead of the configured text (config.cpp:1270-1303).
	std::string Text(std::string_view key, std::string_view fallback, int slot) const {
		std::string language(rt::kFallbackLanguage);
		if (slot >= 0 && pisex::IsClientReady(slot)) {
			const char* clientLanguage = pisex::g_pPlayers->GetClientLanguage(slot);
			if (clientLanguage != nullptr && *clientLanguage != '\0') {
				language = clientLanguage;
			}
		}
		return rt::ResolveMessage(
			[this](std::string_view lang, std::string_view wanted) {
				return translations.Get(lang, wanted); // Get already walks language -> main subtag -> "en"
			},
			language, key, fallback);
	}

	void Say(int slot, std::string_view key, std::string_view fallback,
			 const std::vector<std::pair<std::string, std::string>>& tokens) const {
		std::string text = Text(key, fallback, slot);
		if (text.empty()) {
			return; // an empty phrase means "say nothing" (sync.started of the original)
		}
		for (const auto& [token, value] : tokens) {
			text = rt::ReplaceAll(std::move(text), "{" + token + "}", value);
		}
		// "%s" so a message with a percent sign is printed verbatim.
		pisex::PrintToChat(slot, "%s", text.c_str());
	}

	// -------------------------------------------------------------------------------------------
	// events
	// -------------------------------------------------------------------------------------------
	// Binds the slot to its authenticated player when it is not bound yet (the spawn may arrive
	// before the once-per-second reconcile has seen the newcomer). Returns the bound steam id in
	// `steamId`; all checks are the ones the reconcile makes.
	bool EnsureBound(int slot, uint64_t& steamId) {
		if (slot < 0 || slot >= rt::kSlots || !pisex::IsGameReady() || !pisex::IsClientReady(slot) ||
			pisex::IsFakeClient(slot) || !pisex::IsAuthenticated(slot)) {
			return false;
		}
		const uint64_t current = pisex::GetSteamID64(slot);
		if (current == 0) {
			return false;
		}
		if (slots[slot].steamId != current) {
			if (slots[slot].steamId != 0) {
				DropPlayer(slot, "slot taken over by another player (spawn)");
			}
			BindPlayer(slot, current);
		}
		steamId = current;
		return true;
	}

	void OnSpawnEvent(const pisex::IGameEvent& event) {
		if (!started) {
			return;
		}
		// Events keep arriving during a changelevel (data only) — those have no live slot.
		if (!pisex::IsGameReady()) {
			return;
		}
		const int slot = event.GetPlayerSlot("userid");
		uint64_t steamId = 0;
		if (!EnsureBound(slot, steamId)) {
			return; // a bot, a connecting client or no map: nothing to apply
		}
		const rt::SlotGuard guard{slot, steamId, slots[slot].generation, mapGeneration};

		// The original schedules the same apply twice — immediately (NextTick) and after 0.1 s — and
		// the pin pass 0.5 s later (HookInventoryUpdateService.cs:111-130, PisexPinService.cs:108-124).
		// The 0.1 s glove/agent repeat lives inside Engine::ApplySpawn (it re-resolves the player
		// itself), so only the immediate pass and the deferred pin pass are scheduled here; both
		// re-validate the guard inside ApplyPlayer (no pointer is captured).
		pisex::NextFrame([this, guard]() { ApplyPlayer(guard, /*refresh=*/false, /*inventory=*/false); });
		ScheduleApply(guard, 0.5f, /*refresh=*/false);
	}

	void ScheduleApply(const rt::SlotGuard& guard, float seconds, bool refresh) {
		pisex::CreateTimer(seconds, [this, guard, refresh]() {
			ApplyPlayer(guard, refresh, /*inventory=*/false);
			return -1.0f; // one-shot: pisexcompat destroys the timer
		});
	}

	void OnDeathEvent(const pisex::IGameEvent& event) {
		if (!started || !engineReady || engine == nullptr || !pisex::IsGameReady()) {
			return;
		}
		const int victim = event.GetPlayerSlot("userid");
		const int attacker = event.GetPlayerSlot("attacker");
		// A real victim and a real, different attacker (the original checked the victim's controller
		// and skipped bots; its attacker check was inverted and let bots through — not copied).
		if (victim < 0 || attacker < 0 || victim >= rt::kSlots || attacker >= rt::kSlots || victim == attacker) {
			return;
		}
		if (!pisex::IsClientReady(victim) || !pisex::IsClientReady(attacker) || pisex::IsFakeClient(attacker)) {
			return;
		}
		uint64_t attackerId = 0;
		if (!EnsureBound(attacker, attackerId)) {
			return;
		}

		const int definition = engine->ActiveDefinition(attacker);
		if (definition <= 0) {
			return;
		}
		const int team = pisex::g_pPlayers->GetTeam(attacker);
		if (TeamIndex(team) < 0) {
			return;
		}
		rt::PlayerCache* cache = FindCache(attackerId);
		if (cache == nullptr || !cache->loaded) {
			return;
		}
		Skin* skin = rt::FindMutableStatTrakSkin(cache->view, team, static_cast<uint16_t>(definition));
		if (skin == nullptr) {
			return; // not a StatTrak skin of the active team/definition
		}
		skin->stattrak += 1;
		if (Skin* stored = rt::FindMutableStatTrakSkin(cache->stored, team, static_cast<uint16_t>(definition))) {
			stored->stattrak = skin->stattrak; // the stored copy is what a later !wp would publish
		}
		engine->SetStatTrak(attacker, *skin);
		AddDelta(attackerId, team, static_cast<uint16_t>(definition), 1);
		FlushDeltas(attackerId, /*finalFlush=*/false); // queued right away, async
		++statKills;
	}

	void OnRoundStart() {
		// The original clears its "already replayed" flag on round_start
		// (HookInventoryUpdateService.cs:81-86).
		mvpPlayed = false;
	}

	// A ground weapon picked up by a player never goes through CCSPlayer_ItemServices::GiveNamedItem
	// (the engine moves the existing entity into the inventory), so the hook cannot skin it. The
	// item_pickup event is the only signal for that case; the owned weapons are re-applied in place
	// one frame later (never removed/regiven - the player just picked the weapon up).
	void OnPickupEvent(const pisex::IGameEvent& event) {
		if (!started || !engineReady || engine == nullptr || !pisex::IsGameReady()) {
			return;
		}
		const int slot = event.GetPlayerSlot("userid");
		uint64_t steamId = 0;
		if (!EnsureBound(slot, steamId)) {
			return;
		}
		const rt::SlotGuard guard{slot, steamId, slots[slot].generation, mapGeneration};
		// The event fires while the pickup is being processed; defer one frame so the weapon is in
		// the inventory list by the time the engine walks it. The guard re-validates everything.
		pisex::NextFrame([this, guard]() {
			if (!engineReady || engine == nullptr || !pisex::IsGameReady() || !CheckGuard(guard)) {
				return;
			}
			if (!pisex::IsClientReady(guard.slot) || pisex::IsFakeClient(guard.slot) ||
				pisex::GetSteamID64(guard.slot) != guard.steamId) {
				return;
			}
			rt::PlayerCache* cache = FindCache(guard.steamId);
			if (cache == nullptr || !cache->loaded || cache->view.Empty()) {
				return;
			}
			engine->ApplyOwnedWeapons(guard.slot, cache->view);
			++statPickupApplies;
		});
	}

	// round_mvp, only when MusicKitMvpAnthem is enabled (never on production). The original hides
	// the engine's event and fires its own carrying the player's kit; the recursion guard is set
	// *before* FireEvent because the event we fire goes through this same hook.
	s2sdk::ResultType OnRoundMvpEvent(void* event) {
		if (!mvpEnabled || event == nullptr || !pisex::IsGameReady()) {
			return s2sdk::ResultType::Continue;
		}
		if (mvpPlayed) {
			return s2sdk::ResultType::Continue; // also our own re-fired event
		}
		const int slot = s2sdk::GetEventPlayerSlot(event, "userid");
		if (slot < 0 || slot >= rt::kSlots) {
			return s2sdk::ResultType::Continue;
		}
		const uint64_t steamId = slots[slot].steamId;
		if (steamId == 0 || !pisex::IsClientReady(slot)) {
			return s2sdk::ResultType::Continue;
		}
		const rt::PlayerCache* cache = FindCache(steamId);
		if (cache == nullptr || !cache->loaded || cache->view.music <= 0) {
			return s2sdk::ResultType::Continue;
		}

		mvpPlayed = true;
		// DontBroadcast = true on the original event: s2sdk reads the event's own dont_broadcast
		// flag back in its FireEvent pre-hook and suppresses the broadcast.
		s2sdk::SetEventBroadcast(event, true);
		void* copy = s2sdk::CreateEvent("round_mvp", true);
		if (copy == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		s2sdk::SetEventPlayerSlot(copy, "userid", slot);
		s2sdk::SetEventInt(copy, "musickitid", cache->view.music);
		s2sdk::SetEventInt(copy, "nomusic", 0);
		s2sdk::FireEvent(copy, false);
		++statMvpReplays;
		return s2sdk::ResultType::Continue;
	}

	// -------------------------------------------------------------------------------------------
	// map lifecycle
	// -------------------------------------------------------------------------------------------
	void OnMapStart() {
		mapReady = true;
		if (!started) {
			return;
		}
		StartEngineWithMap();
	}

	void OnMapEnd() {
		// State cleanup only — no client, entity or schema query may run in this window
		// (PITFALLS §B.2). The per-player caches survive a map change (players stay connected);
		// every binding and every in-flight request must not.
		++mapGeneration;
		for (rt::SlotBinding& binding : slots) {
			rt::SlotBinding fresh;
			fresh.generation = binding.generation + 1;
			binding = fresh;
		}
		latestRequest.fill(0);
		loadRequests.fill(PendingLoad{});
		for (auto& [steamId, cache] : players) {
			cache.applyWhenLoaded = false;
			cache.deltaNextRetry = {};
		}
		// The engine's queued work (its deferred regive/glove passes and its timers) belongs to the
		// map that is going away: Engine::Shutdown drops them and the detour, and it never touches a
		// player (it only removes timers, clears the schema caches and unhooks). The next map start
		// re-installs the hook and re-prepares the offsets; until then nothing may apply.
		if (engine != nullptr) {
			engine->Shutdown();
		}
		engineReady = false;
		hookInstalled = false;
		prepareAttempts = 0;
		sinceEngineRetry = std::chrono::milliseconds(0);
		mvpPlayed = false;
		mapReady = false;
		LogInfo("WeaponSkins: map end - bindings cleared, caches kept (" + std::to_string(players.size()) +
				" players cached)");
	}

	// -------------------------------------------------------------------------------------------
	// commands
	// -------------------------------------------------------------------------------------------
	void OnSyncCommand(int slot) {
		if (slot < 0) {
			pisex::PrintToConsole(slot, "%s", "WeaponSkins: !wp is a player command");
			return;
		}
		if (!pisex::IsClientReady(slot) || pisex::IsFakeClient(slot)) {
			return;
		}
		const uint64_t steamId = pisex::GetSteamID64(slot);
		if (steamId == 0) {
			return;
		}
		const rt::Clock::time_point now = rt::Clock::now();
		if (cooldowns.OnCooldown(steamId, now)) {
			Say(slot, "sync.cooldown", config.commands.cooldownMessage,
				{{"seconds", std::to_string(cooldowns.RemainingSeconds(steamId, now))},
				 {"url", config.commands.siteUrl}});
			return;
		}
		cooldowns.Arm(steamId, now, config.commands.cooldownSeconds);
		Say(slot, "sync.started", config.commands.startedMessage, {{"url", config.commands.siteUrl}});

		if (!storageReady || storage == nullptr) {
			Say(slot, "sync.error", config.commands.errorMessage, {{"url", config.commands.siteUrl}});
			cooldowns.Clear(steamId);
			return;
		}
		if (slots[slot].steamId != steamId) {
			// The reconcile has not bound the slot yet: bind it now so the load can be applied.
			BindPlayer(slot, steamId);
		}
		RequestLoad(slot, steamId, /*force=*/true, /*sync=*/true);
	}

	void OnAliasCommand(int slot) {
		if (slot < 0) {
			pisex::PrintToConsole(slot, "%s", "WeaponSkins: the skin menu lives on the website");
			return;
		}
		if (!pisex::IsClientReady(slot) || pisex::IsFakeClient(slot)) {
			return;
		}
		if (!config.commands.redirect) {
			if (!redirectOffNoted) {
				redirectOffNoted = true;
				LogWarn("WeaponSkins: RedirectMenusToSite=false (in-game menus are not ported) - the aliases do nothing");
			}
			return;
		}
		Say(slot, "sync.redirect", config.commands.redirectMessage,
			{{"url", config.commands.siteUrl}, {"command", "!" + config.commands.syncCommand}});
	}

	void OnStatusCommand(int slot) {
		const std::string text = StatusText();
		pisex::PrintToConsole(slot, "%s", text.c_str());
	}

	void RegisterCommands() {
		const std::string sync = rt::ToLower(config.commands.syncCommand.empty() ? "wp" : config.commands.syncCommand);
		std::vector<std::string> aliasConsole;
		std::vector<std::string> aliasChat;
		for (const std::string& alias : config.commands.aliases) {
			// "menu" is never hijacked (the hudmenu plugin owns !menu; the production alias list
			// does not contain it either).
			if (!rt::AliasAllowed(alias)) {
				continue;
			}
			const std::string name = rt::ToLower(alias);
			if (name == sync) {
				continue;
			}
			aliasConsole.push_back(name);
			aliasChat.push_back("!" + name);
		}

		// Both command forms: the engine dispatches console input to the console name and chat
		// input ("!x"/"/x") to the chat name; pisexcompat hands the registered spelling to the
		// callback (commands.hpp).
		if (!aliasConsole.empty()) {
			pisex::g_pUtils->RegCommand(aliasConsole, aliasChat, [this](int slot, const char*) {
				OnAliasCommand(slot);
				return true;
			});
		}
		pisex::g_pUtils->RegCommand({sync}, {"!" + sync}, [this](int slot, const char*) {
			OnSyncCommand(slot);
			return true;
		});
		pisex::g_pUtils->RegCommand({"ws_status"}, {}, [this](int slot, const char*) {
			OnStatusCommand(slot);
			return true;
		});
		LogInfo("WeaponSkins: commands ready ('!" + sync + "'" +
				(aliasConsole.empty() ? std::string() : " and " + std::to_string(aliasConsole.size()) + " aliases") +
				", ws_status)");
	}

	void RegisterEvents() {
		pisex::g_pUtils->HookEvent("player_spawn", [this](const char*, pisex::IGameEvent* event, bool) {
			if (event != nullptr) {
				OnSpawnEvent(*event);
			}
		});
		pisex::g_pUtils->HookEvent("round_start", [this](const char*, pisex::IGameEvent*, bool) { OnRoundStart(); });
		pisex::g_pUtils->HookEvent("player_death", [this](const char*, pisex::IGameEvent* event, bool) {
			if (event != nullptr) {
				OnDeathEvent(*event);
			}
		});
		// Both spellings of the pickup event: CS:GO fired "item_pickup", CS2 ships "item_pickup_slerp"
		// (the CS2 pickup animation). s2sdk rejects a name the game build does not know with a single
		// ErrorLog at registration, and a double fire is harmless - ApplyOwnedWeapons only rewrites
		// the same attributes. (live check: which of the two actually fires)
		const auto pickup = [this](const char*, pisex::IGameEvent* event, bool) {
			if (event != nullptr) {
				OnPickupEvent(*event);
			}
		};
		pisex::g_pUtils->HookEvent("item_pickup", pickup);
		pisex::g_pUtils->HookEvent("item_pickup_slerp", pickup);
	}

	void RegisterListeners() {
		if (__s2sdk_OnClientDisconnect_Post_Register != nullptr) {
			// The disconnect flush/drop uses the *binding* (not a client query), so the Post variant
			// is fine and matches plugins/perm_connector.
			s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectTrampoline);
			disconnectListenerRegistered = true;
		} else {
			LogWarn("WeaponSkins: OnClientDisconnect_Post binding missing - the reconcile covers disconnects");
		}
		if (__s2sdk_OnMapStart_Register != nullptr && __s2sdk_OnMapEnd_Register != nullptr) {
			s2sdk::OnMapStart_Register(&OnMapStartTrampoline);
			s2sdk::OnMapEnd_Register(&OnMapEndTrampoline);
			mapListenersRegistered = true;
		}

		// The auth event is not reliable in this stack, but it is a fast path; the reconcile is the
		// path that always works (PITFALLS §B.6).
		pisex::HookOnClientAuthorized([this](int slot, uint64_t steamId) {
			if (!started || steamId == 0 || !pisex::IsGameReady() || !pisex::IsClientReady(slot)) {
				return;
			}
			if (pisex::IsFakeClient(slot)) {
				return;
			}
			if (slots[slot].steamId != steamId) {
				if (slots[slot].steamId != 0) {
					DropPlayer(slot, "slot taken over by another player (auth)");
				}
				BindPlayer(slot, steamId);
			}
		});
	}

	void UnregisterListeners() {
		if (disconnectListenerRegistered) {
			disconnectListenerRegistered = false;
			if (__s2sdk_OnClientDisconnect_Post_Unregister != nullptr) {
				s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectTrampoline);
			}
		}
		if (mapListenersRegistered) {
			mapListenersRegistered = false;
			if (__s2sdk_OnMapStart_Unregister != nullptr) {
				s2sdk::OnMapStart_Unregister(&OnMapStartTrampoline);
			}
			if (__s2sdk_OnMapEnd_Unregister != nullptr) {
				s2sdk::OnMapEnd_Unregister(&OnMapEndTrampoline);
			}
		}
	}

	// -------------------------------------------------------------------------------------------
	// lifecycle
	// -------------------------------------------------------------------------------------------
	void Start(std::string_view location) {
		if (started) {
			return;
		}
		// The plugin name drives the config/translations/data directories of pisexcompat.
		pisex::Init("WeaponSkins");
		ResolvePaths(location);
		LoadConfiguration();
		LoadTranslations();
		mvpEnabled = config.musicKitMvpAnthem;
		OpenStorage();
		if (!TryLoadEcon()) {
			// A plugin loaded before the game files exist retries on the first map (StartEngineWithMap).
			LogWarn("WeaponSkins: econ data not loaded yet (" + econError + ") - retried once a map is up");
		}
		RegisterCommands();
		RegisterEvents();
		RegisterListeners();

		// Everything that touches clients, entities or the schema waits for a live map
		// (PITFALLS §B.1). This runs from pisexcompat's Poll(), where its readiness flag has
		// already been granted - unlike the runtime's own s2sdk OnMapStart listener, whose order
		// against that flag flip is what made every map start fail with "no map is running yet".
		// The callback fires again on every map change; pisex::Shutdown (Stop) clears it.
		pisex::RegisterMapStartCallback([this](const char*) { StartEngineWithMap(); });

		if (mvpEnabled && __s2sdk_HookEvent != nullptr) {
			// Only with MusicKitMvpAnthem = true (never on production): the raw s2sdk hook is needed
			// because pisexcompat's event hooks cannot suppress the engine's own broadcast.
			if (s2sdk::HookEvent(plg::string("round_mvp"), &OnRoundMvpTrampoline, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				mvpHookRegistered = true;
			} else {
				LogWarn("WeaponSkins: could not hook round_mvp - the MVP anthem replay is off");
			}
		}

		started = true;
		LogInfo("WeaponSkins: started (storage " + std::string(storageReady ? "ready" : "unavailable") +
				", econ " + std::string(econReady ? "ready" : "pending") + ")");
	}

	void Update(std::chrono::milliseconds dt) {
		// pisexcompat first: the deferred startup callbacks, the timer reschedules and the menu
		// requests that came out of engine callbacks (it is a safe no-op without a map).
		pisex::Poll();
		// The database callbacks run on this thread, even while no map is up (the storage itself
		// answers them without touching a client).
		if (storage != nullptr) {
			storage->Poll();
		}
		if (!started) {
			return;
		}
		// Engine preparation retry. The map-start path normally prepares the engine, but the s2sdk
		// OnMapStart listener runs before pisexcompat grants readiness (Poll is where the flag
		// flips), so this is what covers that window (and a missed callback after a plugin loaded
		// mid-map). It only runs while the map is really up (the schema offset resolution requires
		// it), throttled to one attempt per second and bounded per map by kMaxPrepareAttempts
		// (reset at map end); the success logs "engine ready" once, StartEngineWithMap never
		// re-preps after that.
		if (!engineReady && pisex::IsGameReady() && prepareAttempts < kMaxPrepareAttempts) {
			sinceEngineRetry += dt;
			if (sinceEngineRetry >= std::chrono::seconds(kEngineRetrySeconds)) {
				sinceEngineRetry = std::chrono::milliseconds(0);
				StartEngineWithMap();
			}
		}
		// Every tick: the loads that were waiting for their StatTrak drain and the delta retries.
		// Cheap by construction (a loop over the slot/player tables that exits early when nothing is
		// outstanding); the cache pruning runs with the reconcile, once per second.
		try {
			PumpLoadRequests();
			PumpDeltas();
		} catch (const std::exception& ex) {
			LogError(std::string("WeaponSkins: pump failed: ") + ex.what());
		} catch (...) {
			LogError("WeaponSkins: pump failed with an unknown exception");
		}

		sinceReconcile += dt;
		if (sinceReconcile < std::chrono::seconds(rt::kReconcileSeconds)) {
			return;
		}
		sinceReconcile = std::chrono::milliseconds(0);
		try {
			Reconcile();
		} catch (const std::exception& ex) {
			LogError(std::string("WeaponSkins: reconcile failed: ") + ex.what());
		} catch (...) {
			LogError("WeaponSkins: reconcile failed with an unknown exception");
		}
	}

		// Timed-out reads are invalidated even for a player with an existing cache (!wp). Their
		// replacement waits for a fresh drain; late callbacks cannot release the current barrier.
	void PumpLoadRequests() {
		for (int slot = 0; slot < rt::kSlots; ++slot) {
				if (slots[slot].loadRequested && rt::Clock::now() >= slots[slot].nextRetry) {
					RequestLoad(slot, slots[slot].steamId, /*force=*/false, /*sync=*/false);
				}
			if (loadRequests[slot].pending) {
				PumpLoadRequest(slot);
			}
		}
	}

	// Retries the StatTrak writes that are still owed. The pacing keeps a broken database from
	// being hammered; a failed batch stays owed and is retried here (and on disconnect).
	void PumpDeltas() {
		if (!storageReady || storage == nullptr) {
			return;
		}
		for (auto& [steamId, cache] : players) {
			if (!cache.ledger.Empty()) {
				FlushDeltas(steamId, /*finalFlush=*/false);
			}
		}
	}

	// Waits (bounded) until the storage answered every outstanding write. Called only from Stop(),
	// where the callbacks must still run before the storage is destroyed. Sleeping 1 ms on the game
	// thread is acceptable there: OnPluginEnd is a `plg reload`/unload, not a game frame (and the
	// callback objects live in this translation unit, so `this` stays valid for the whole drain).
	void DrainStorage() {
		constexpr auto kDrainTimeout = std::chrono::seconds(3);
		const auto deadline = rt::Clock::now() + kDrainTimeout;
		for (;;) {
			storage->Poll();
			int64_t outstanding = 0;
			for (const auto& [steamId, cache] : players) {
				(void)steamId;
				outstanding += cache.ledger.TotalOwed();
			}
			if (outstanding == 0 || rt::Clock::now() >= deadline) {
				return;
			}
			// A failed batch is owed again: give it another try with the same bounded budget.
			for (auto& [steamId, cache] : players) {
				if (!cache.ledger.Empty()) {
					FlushDeltas(steamId, /*finalFlush=*/true);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	void Stop() {
		if (!started) {
			return;
		}
		started = false;
			// Release every read barrier before draining writes; any old load response is stale.
			++mapGeneration;
			for (rt::SlotBinding& binding : slots) {
				rt::UnbindSlot(binding);
			}
			latestRequest.fill(0);
			loadRequests.fill(PendingLoad{});
		try {
			// 1. Everything registered directly with s2sdk, whose callbacks point into this library.
			if (mvpHookRegistered) {
				mvpHookRegistered = false;
				if (__s2sdk_UnhookEvent != nullptr) {
					s2sdk::UnhookEvent(plg::string("round_mvp"), &OnRoundMvpTrampoline, s2sdk::HookMode::Pre);
				}
			}
			UnregisterListeners();
			// 2. The engine: it removes the GiveNamedItem detour (only when no other plugin's
			// callback is registered for it) and drops the schema caches.
			if (engine != nullptr) {
				engine->Shutdown();
				engine.reset();
			}
			engineReady = false;
			hookInstalled = false;
			// 3. Commands, game event hooks, the auth/pisex listeners, the timers and the
			// next-frame queue created through pisexcompat.
			pisex::Shutdown();
			// 4. The remaining StatTrak kills: queue them and drain the storage before it is
			// destroyed - the statements still have to reach the worker, and the callbacks have to
			// run here while `this` and the caches are still alive.
			if (storage != nullptr) {
				for (auto& [steamId, cache] : players) {
					(void)cache;
					FlushDeltas(steamId, /*finalFlush=*/true);
				}
				DrainStorage();
			}
			int64_t unresolved = 0;
			for (const auto& [steamId, cache] : players) {
				(void)steamId;
				unresolved += cache.ledger.TotalOwed();
			}
			if (unresolved > 0) {
				LogError("WeaponSkins: " + std::to_string(unresolved) +
						 " StatTrak kills could not be persisted before the plugin stopped (the database did not "
						 "acknowledge them) - they are lost with the plugin instance");
			}
			players.clear();
			storage.reset();
			db.reset();
			storageReady = false;
		} catch (const std::exception& ex) {
			LogError(std::string("WeaponSkins: shutdown failed: ") + ex.what());
		} catch (...) {
			LogError("WeaponSkins: shutdown failed with an unknown exception");
		}
	}

	std::string StatusText() const {
		std::string text = "WeaponSkins: storage=";
		text += storageReady ? "ready" : "unavailable";
		text += " (connection '" + config.pisex.connectionName + "', prefix " + config.pisex.tablePrefix + ", serverId " +
				std::to_string(config.pisex.serverId) + ")";
		text += ", econ=" + std::string(econReady ? "ready" : "pending");
		text += ", engine=" + std::string(engineReady ? "ready" : "not-ready");
		text += ", giveNamedItemHook=" + std::string(hookInstalled ? "installed" : "off");
		text += ", signature=" + std::string(gamedataPath.empty() ? "not-found" : gamedataPath.filename().string());
		text += ", mvpAnthem=" + std::string(mvpEnabled ? "on" : "off (MusicKitMvpAnthem=false)");
		text += ", mapGeneration=" + std::to_string(mapGeneration);
		text += ", mapReady=" + std::string(mapReady ? "yes" : "no");

		int bound = 0;
		int loaded = 0;
		// Per-slot category counts: "loaded but every category empty" (bad data / team mismatch)
		// looks the same in the counters above as "loaded and applied but invisible".
		std::string categories;
		for (int slot = 0; slot < rt::kSlots; ++slot) {
			if (slots[slot].steamId == 0) {
				continue;
			}
			++bound;
			if (slots[slot].loaded) {
				++loaded;
			}
			categories += "\nslot " + std::to_string(slot) + " " + (slots[slot].loaded ? "loaded" : "pending");
			// The two pawn-handle readings of the engine: the controller's m_hPlayerPawn (what the
			// glove/agent/guard paths use) and the pointer-derived one (the reading the live build
			// refused). When only the first is valid, a -1 second value is the expected picture.
			if (engine != nullptr) {
				const Engine::PawnHandles handles = engine->PawnHandleReport(slot);
				categories += " pawnHandle(ctrl)=" + std::to_string(handles.viaController) +
							  " pawnHandle(ptr)=" + std::to_string(handles.viaPointer);
			}
			const rt::PlayerCache* cache = FindCache(slots[slot].steamId);
			if (cache == nullptr) {
				continue;
			}
			for (std::size_t index = 0; index < cache->view.teams.size(); ++index) {
				const TeamItems& items = cache->view.teams[index];
				categories += std::string(index == 0 ? " T[weapons=" : " CT[weapons=") +
							  std::to_string(items.weapons.size());
				categories += std::string(" knife=") + (items.knife ? "Y" : "-");
				categories += std::string(" glove=") + (items.glove ? "Y" : "-");
				if (items.agent == 0) {
					categories += " agent=-";
				} else {
					const EconDefinition* agent = econ.Agent(items.agent);
					categories += " agent=" + std::to_string(items.agent) +
								  (agent != nullptr && !agent->model.empty() ? "" : "(not-in-items_game)");
				}
				categories += std::string(" pin=") + std::to_string(items.pin) + "]";
			}
			categories += std::string(" music=") + std::to_string(cache->view.music);
		}
		int64_t owedDeltas = 0;
		int64_t inflightDeltas = 0;
		int pendingLoads = 0;
		for (const auto& [steamId, cache] : players) {
			(void)steamId;
			owedDeltas += cache.ledger.TotalOwed();
			inflightDeltas += cache.ledger.TotalInflight();
		}
		for (const PendingLoad& request : loadRequests) {
			if (request.pending) {
				++pendingLoads;
			}
		}
		text += "\nslots: bound=" + std::to_string(bound) + " loaded=" + std::to_string(loaded) +
				" cachedPlayers=" + std::to_string(players.size()) + " owedDeltas=" + std::to_string(owedDeltas) +
				" inflightDeltas=" + std::to_string(inflightDeltas) + " pendingLoads=" + std::to_string(pendingLoads) +
				" cooldowns=" + std::to_string(cooldowns.Size());
		text += categories;
		text += "\nstats: loadRequests=" + std::to_string(statLoadRequests) + " loadsOk=" + std::to_string(statLoadsOk) +
				" loadsFailed=" + std::to_string(statLoadsFailed) + " applies=" + std::to_string(statApplies) +
				" stattrakKills=" + std::to_string(statKills) + " deltaQueued=" + std::to_string(statDeltasQueued) +
				" deltaSaved=" + std::to_string(statDeltasSaved) + " deltaFailed=" + std::to_string(statDeltasFailed) +
				" mvpReplays=" + std::to_string(statMvpReplays) + " pickupApplies=" + std::to_string(statPickupApplies);
	// What the engine actually wrote per category (the hook counters are the only way to tell a
	// dead GiveNamedItem path from a silent no-op inside it).
	if (engine != nullptr) {
		const Engine::ApplyStats& s = engine->Stats();
		text += "\napplyStats: hookCalls=" + std::to_string(s.hookCalls) +
				" hookBadWeapon=" + std::to_string(s.hookBadWeapon) + " hookNoServices=" + std::to_string(s.hookNoServices) +
				" hookNoPawn=" + std::to_string(s.hookNoPawn) + " hookNoController=" + std::to_string(s.hookNoController) +
				" hookNoSlot=" + std::to_string(s.hookNoSlot) + " hookNotReady=" + std::to_string(s.hookNotReady) +
				" hookBot=" + std::to_string(s.hookBot) +
				" hookNoSnapshot=" + std::to_string(s.hookNoSnapshot) + " hookNoTeam=" + std::to_string(s.hookNoTeam) +
				" hookKnife=" + std::to_string(s.hookKnife) + " hookGun=" + std::to_string(s.hookGun) +
				" hookNoMatch=" + std::to_string(s.hookNoMatch) + " gunInPlace=" + std::to_string(s.gunInPlace) +
				" knife=" + std::to_string(s.knifeApplied) + " glove=" + std::to_string(s.gloveApplied) +
				" (gloveNoData=" + std::to_string(s.gloveNoData) + " gloveNoHandle=" + std::to_string(s.gloveNoHandle) +
				") agent=" + std::to_string(s.agentApplied) +
				" (agentWritten=" + std::to_string(s.agentWritten) +
				" agentDeferDropped=" + std::to_string(s.agentDeferDropped) +
				" agentNoHandle=" + std::to_string(s.agentNoHandle) +
				" agentNoBinding=" + std::to_string(s.agentNoBinding) + " agentNoModel=" + std::to_string(s.agentNoModel) +
				" agentNoData=" + std::to_string(s.agentNoData) + " agentNoCatalog=" + std::to_string(s.agentNoCatalog) +
				") music=" + std::to_string(s.musicApplied) + " pin=" + std::to_string(s.pinApplied);
	}
		if (!lastError.empty()) {
			text += "\nlastError: " + lastError;
		}
		return text;
	}

	// -------------------------------------------------------------------------------------------
	// callback entry points (called by the trampolines below)
	// -------------------------------------------------------------------------------------------
	void OnClientDisconnect(int slot) {
		if (slot < 0 || slot >= rt::kSlots) {
			return;
		}
		DropPlayer(slot, "disconnect");
	}
};

// --- the callback trampolines ---------------------------------------------------------------------
//
// Each one is called from the engine or from s2sdk/pisex code, so nothing may unwind out of it (the
// s2sdk listener manager has no try/catch; an escaping exception would reach the engine). The
// Runtime::Handle* wrappers below catch everything.

void OnClientDisconnectTrampoline(int32_t playerSlot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	RuntimeInstance().HandleClientDisconnect(playerSlot);
}

void OnMapStartTrampoline() { RuntimeInstance().HandleMapStart(); }

void OnMapEndTrampoline() { RuntimeInstance().HandleMapEnd(); }

void OnSpawnTrampoline(const char* name, pisex::IGameEvent* event, bool dontBroadcast) {
	(void)name;
	(void)dontBroadcast;
	RuntimeInstance().HandleSpawn(event == nullptr ? nullptr : event->Native());
}

void OnDeathTrampoline(const char* name, pisex::IGameEvent* event, bool dontBroadcast) {
	(void)name;
	(void)dontBroadcast;
	RuntimeInstance().HandleDeath(event == nullptr ? nullptr : event->Native());
}

void OnRoundStartTrampoline(const char* name, pisex::IGameEvent* event, bool dontBroadcast) {
	(void)name;
	(void)event;
	(void)dontBroadcast;
	RuntimeInstance().HandleRoundStart();
}

s2sdk::ResultType OnRoundMvpTrampoline(const plg::string& name, void* event, bool dontBroadcast) {
	(void)name;
	(void)dontBroadcast;
	return static_cast<s2sdk::ResultType>(RuntimeInstance().HandleRoundMvp(event));
}

// --- the Runtime entry points ----------------------------------------------------------------------
Runtime::Runtime() : m_impl(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;

Runtime& RuntimeInstance() {
	// One instance per plugin load; Stop() releases everything it holds.
	static Runtime instance;
	return instance;
}

void Runtime::Start(std::string_view location) {
	try {
		m_impl->Start(location);
	} catch (const std::exception& ex) {
		LogError(std::string("WeaponSkins: exception in start: ") + ex.what());
	} catch (...) {
		LogError("WeaponSkins: unknown exception in start");
	}
}

void Runtime::Update(std::chrono::milliseconds dt) {
	try {
		m_impl->Update(dt);
	} catch (const std::exception& ex) {
		LogError(std::string("WeaponSkins: exception in update: ") + ex.what());
	} catch (...) {
		LogError("WeaponSkins: unknown exception in update");
	}
}

void Runtime::Stop() {
	try {
		m_impl->Stop();
	} catch (const std::exception& ex) {
		LogError(std::string("WeaponSkins: exception in stop: ") + ex.what());
	} catch (...) {
		LogError("WeaponSkins: unknown exception in stop");
	}
}

std::string Runtime::StatusText() const {
	try {
		return m_impl->StatusText();
	} catch (...) {
		return "WeaponSkins: status unavailable";
	}
}

void Runtime::HandleClientDisconnect(int slot) {
	try {
		m_impl->OnClientDisconnect(slot);
	} catch (...) {
	}
}

void Runtime::HandleMapStart() {
	try {
		m_impl->OnMapStart();
	} catch (...) {
	}
}

void Runtime::HandleMapEnd() {
	try {
		m_impl->OnMapEnd();
	} catch (...) {
	}
}

void Runtime::HandleSpawn(void* nativeEvent) {
	if (nativeEvent == nullptr) {
		return;
	}
	try {
		pisex::IGameEvent event(nativeEvent);
		m_impl->OnSpawnEvent(event);
	} catch (...) {
	}
}

void Runtime::HandleDeath(void* nativeEvent) {
	if (nativeEvent == nullptr) {
		return;
	}
	try {
		pisex::IGameEvent event(nativeEvent);
		m_impl->OnDeathEvent(event);
	} catch (...) {
	}
}

void Runtime::HandleRoundStart() {
	try {
		m_impl->OnRoundStart();
	} catch (...) {
	}
}

int Runtime::HandleRoundMvp(void* nativeEvent) {
	try {
		return static_cast<int>(m_impl->OnRoundMvpEvent(nativeEvent));
	} catch (...) {
		return 0; // ResultType::Continue
	}
}

} // namespace ws
