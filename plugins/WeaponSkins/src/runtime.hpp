#pragma once
// WeaponSkins (C++ port of the SwiftlyS2 skinchanger) — the lifecycle runtime.
//
// runtime.cpp is the glue between the plugin entry (plugin.cpp) and the engine-independent
// modules (config / econ / storage / engine, see their headers). It owns:
//   * the parsed configuration, the phrase store and the resolved file paths,
//   * the plgdb connection and the Pisex storage,
//   * the per-player cache (the runtime view the engine applies from) and the StatTrak delta
//     queue,
//   * the once-per-second client reconcile, the lazy per-player load and the spawn schedule,
//   * the game-event hooks, the console/chat commands and every teardown.
//
// The original C# plugin is the reference; file:line references in the comments point at
// the behaviour that is being ported. Everything in this file runs on the game thread: the
// Plugify update pump, the s2sdk callbacks, pisexcompat timers and the plgdb callback delivery
// (Poll()). No locking is needed anywhere.
//
// This header additionally holds the *engine-free* building blocks (namespace ws::rt) so that
// tests/runtime_tests.cpp can drive the guards, the cooldown, the snapshot view and the sticker
// fix without linking the engine, the storage or s2sdk. They are header-only on purpose: the test
// binary links nothing but its own translation unit.

#include "config.hpp"
#include "econ.hpp"
#include "engine.hpp"
#include "models.hpp"
#include "storage.hpp"

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ws {
namespace rt {

// --- constants --------------------------------------------------------------------------------
inline constexpr int kSlots = kMaxPlayers; // 64, the engine limit
// The original reconciles once per second because s2sdk's OnClientAuthenticated cannot be relied
// on in this stack (the same finding plugins/perm_connector is built on).
inline constexpr int kReconcileSeconds = 1;
// A failed/stale player load is retried with this pacing; a broken database must not be hammered.
inline constexpr int kLoadRetrySeconds = 5;
// StatTrak deltas that could not be persisted are retried with this pacing.
inline constexpr int kDeltaRetrySeconds = 5;
// The per-player cache is bounded; entries are dropped on disconnect, and the least recently used
// unbound entry is evicted when the cap is hit (the original grew without a bound).
inline constexpr std::size_t kMaxCachedPlayers = static_cast<std::size_t>(kMaxPlayers);
// The sticker fix of the original walks at most 1000 wear steps (StickerFixService.cs:41).
inline constexpr int kStickerFixMaxAttempts = 1000;
inline constexpr float kStickerFixWearStep = 0.001f;
inline constexpr std::string_view kFallbackLanguage = "en";

using Clock = std::chrono::steady_clock;

// --- slot binding and generation guards --------------------------------------------------------
//
// The engine hands out numbers that outlive the moment they are captured: a plgdb callback, a
// pisexcompat timer or an s2sdk NextFrame task created for slot N may run after the player left,
// after somebody else took the slot or after a map change. Every deferred callback therefore
// captures a SlotGuard and re-validates it against the live binding before it touches anything
// (the same "re-validate at fire time" rule the timer contract demands).
struct SlotBinding {
	uint64_t steamId = 0;
	// Bumped on every bind, so a callback from a previous occupant of the slot can never match.
	uint64_t generation = 1;
	bool loadRequested = false;
	bool loaded = false;
	Clock::time_point nextRetry{};
};

struct SlotGuard {
	int slot = -1;
	uint64_t steamId = 0;
	uint64_t generation = 0;
	uint64_t mapGeneration = 0;
};

// Binds the slot to a (new) player. Returns true when the binding changed, i.e. a load has to be
// requested for `steamId`. The generation only ever grows.
inline bool BindSlot(SlotBinding& binding, uint64_t steamId) {
	if (steamId == 0) {
		return false;
	}
	if (binding.steamId == steamId) {
		return false;
	}
	const uint64_t nextGeneration = binding.generation + 1;
	binding = SlotBinding{};
	binding.generation = nextGeneration;
	binding.steamId = steamId;
	return true;
}

// Drops the binding. Returns the steamId that was bound (0 when there was none).
inline uint64_t UnbindSlot(SlotBinding& binding) {
	const uint64_t previous = binding.steamId;
	const uint64_t nextGeneration = binding.generation + 1;
	binding = SlotBinding{};
	binding.generation = nextGeneration;
	return previous;
}

// True when the guard still describes the live binding of its slot on the live map.
inline bool GuardMatches(const SlotGuard& guard, const std::array<SlotBinding, kSlots>& slots,
						 uint64_t mapGeneration) {
	return guard.slot >= 0 && guard.slot < kSlots && guard.steamId != 0 &&
		   guard.mapGeneration == mapGeneration && slots[guard.slot].steamId == guard.steamId &&
		   slots[guard.slot].generation == guard.generation;
}

// --- the !wp cooldown ---------------------------------------------------------------------------
//
// PisexSyncCommandService.cs:94-102: a per-player cooldown, 20 s in production. steady_clock is
// used here (the original used DateTimeOffset.UtcNow, which a system clock change could move).
class Cooldowns {
public:
	bool OnCooldown(uint64_t id, Clock::time_point now) const {
		const auto it = until_.find(id);
		return it != until_.end() && it->second > now;
	}

	// Whole seconds left, rounded up, never negative (the original's Math.Ceiling).
	int RemainingSeconds(uint64_t id, Clock::time_point now) const {
		const auto it = until_.find(id);
		if (it == until_.end() || it->second <= now) {
			return 0;
		}
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(it->second - now).count();
		return static_cast<int>((left + 999) / 1000);
	}

	void Arm(uint64_t id, Clock::time_point now, int seconds) {
		// An expired record reads as absent everywhere (OnCooldown/RemainingSeconds compare
		// against `now`), so it is dropped here: without this the map kept one entry per player
		// that had ever run the command until the process ended.
		for (auto it = until_.begin(); it != until_.end();) {
			if (it->second <= now) {
				it = until_.erase(it);
			} else {
				++it;
			}
		}
		until_[id] = now + std::chrono::seconds(seconds > 0 ? seconds : 1);
	}

	// The original clears the cooldown when nothing was configured or the load failed, so the
	// player may retry immediately (PisexSyncCommandService.cs:121, :163).
	void Clear(uint64_t id) { until_.erase(id); }

	std::size_t Size() const { return until_.size(); }

private:
	std::unordered_map<uint64_t, Clock::time_point> until_;
};

// --- text helpers --------------------------------------------------------------------------------
inline std::string ToLower(std::string_view text) {
	std::string out(text);
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

inline bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
	return a.size() == b.size() && ToLower(a) == ToLower(b);
}

// Replaces every occurrence of `token` (the original used string.Replace on "{url}"/"{seconds}").
inline std::string ReplaceAll(std::string text, std::string_view token, std::string_view value) {
	if (token.empty()) {
		return text;
	}
	std::size_t pos = 0;
	while ((pos = text.find(token, pos)) != std::string::npos) {
		text.replace(pos, token.size(), value);
		pos += value.size();
	}
	return text;
}

// The menu aliases the original redirects to the website. "menu" is never hijacked: the server has
// its own menu plugin (hudmenu) and the production alias list does not contain it either — a
// config that lists it would break !menu for every player, so it is filtered out here.
inline bool AliasAllowed(std::string_view name) {
	return !name.empty() && !EqualsIgnoreCase(name, "menu") && !EqualsIgnoreCase(name, "!menu");
}

// --- sticker fix --------------------------------------------------------------------------------
//
// StickerFixService.cs: the client renders stickers from a cached appearance; when the same
// appearance key (definition + paint + wear + seed) is sent with a *different* sticker set, the
// skin is nudged by +0.001 wear until the key is new. Bounded, finite-safe, no allocation.
inline uint64_t HashBytes(uint64_t hash, const void* data, std::size_t size) {
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (std::size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL; // FNV-1a
	}
	return hash;
}

inline uint64_t HashInt(uint64_t hash, uint64_t value) { return HashBytes(hash, &value, sizeof(value)); }

// A float is hashed by its bits with 0 normalised (so +0.0f and -0.0f hash alike).
inline uint32_t FloatBits(float value) {
	if (value == 0.0f) {
		return 0;
	}
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

inline uint64_t HashFloat(uint64_t hash, float value) { return HashInt(hash, FloatBits(value)); }

// CalculateKeyHash of StickerFixService.cs:62-72 — the appearance of the weapon, wear included.
inline uint64_t AppearanceKey(const Skin& skin) {
	uint64_t hash = 1469598103934665603ULL;
	hash = HashInt(hash, skin.definition);
	hash = HashInt(hash, static_cast<uint64_t>(static_cast<uint32_t>(skin.paint)));
	hash = HashInt(hash, static_cast<uint64_t>(static_cast<uint32_t>(skin.seed)));
	hash = HashFloat(hash, skin.wear);
	return hash;
}

// CalculateStickerHash of StickerFixService.cs:74-86 — the six sticker slots only (the keychain is
// not part of the original's hash).
inline uint64_t StickerHash(const Sticker* stickers, std::size_t count) {
	uint64_t hash = 1469598103934665603ULL;
	for (std::size_t i = 0; i < count; ++i) {
		const Sticker& s = stickers[i];
		const bool present = s.id != 0;
		hash = HashInt(hash, present ? 1u : 0u);
		if (!present) {
			continue;
		}
		hash = HashInt(hash, static_cast<uint64_t>(static_cast<uint32_t>(s.id)));
		hash = HashInt(hash, static_cast<uint64_t>(static_cast<uint32_t>(s.schema)));
		hash = HashFloat(hash, s.x);
		hash = HashFloat(hash, s.y);
		hash = HashFloat(hash, s.wear);
		hash = HashFloat(hash, s.scale);
		hash = HashFloat(hash, s.rotation);
	}
	return hash;
}

// FixSticker of StickerFixService.cs:36-60. `hashes` is the per-player appearance->sticker map.
// Returns true when the wear was changed. Runs while the snapshot is *prepared* (never inside the
// GiveNamedItem hook, so the hook stays allocation- and work-free).
inline bool FixSticker(Skin& skin, std::unordered_map<uint64_t, int64_t>& hashes) {
	const int64_t stickerHash = static_cast<int64_t>(StickerHash(skin.stickers.data(), skin.stickers.size()));
	for (int attempt = 0; attempt < kStickerFixMaxAttempts; ++attempt) {
		if (!std::isfinite(skin.wear)) {
			return false; // the original bails out on a non-finite wear
		}
		const uint64_t key = AppearanceKey(skin);
		const auto it = hashes.find(key);
		if (it == hashes.end()) {
			hashes[key] = stickerHash;
			return attempt > 0;
		}
		if (it->second == stickerHash) {
			return attempt > 0;
		}
		const float next = skin.wear + kStickerFixWearStep;
		if (!std::isfinite(next) || next == skin.wear) {
			return attempt > 0; // step absorbed by the float precision: stop instead of spinning
		}
		skin.wear = next;
	}
	return true; // bounded: 1000 steps at most, then the last wear stays
}

// --- the runtime snapshot view -------------------------------------------------------------------
//
// The cache holds both what the database said (`stored`, with the runtime's StatTrak counts kept
// in sync) and what the engine applies (`view`): the same data with the permission gates applied
// and the sticker wear fixed. Preparing the view is the only place permissions are asked, so the
// GiveNamedItem hook never pays for them.
inline bool AllPermissionsEmpty(const PermissionConfig& permissions) {
	return permissions.weapons.empty() && permissions.knives.empty() && permissions.gloves.empty() &&
		   permissions.stickers.empty() && permissions.keychains.empty() && permissions.agents.empty() &&
		   permissions.music.empty();
}

inline void ClearStickers(Skin& skin) { skin.stickers = {}; }

// `allowed(perm)` is expected to answer "may this player use this permission" and must fail open.
template <typename Allowed>
void BuildView(const Snapshot& stored, const PermissionConfig& permissions, Allowed&& allowed,
			   Snapshot& view, std::unordered_map<uint64_t, int64_t>& stickerHashes) {
	view = stored;
	// Fast path: the production config has no ItemPermissions at all, so no permission call is made.
	if (!AllPermissionsEmpty(permissions)) {
		const bool canWeapons = permissions.weapons.empty() || allowed(permissions.weapons);
		const bool canStickers = permissions.stickers.empty() || allowed(permissions.stickers);
		const bool canKeychains = permissions.keychains.empty() || allowed(permissions.keychains);
		for (TeamItems& items : view.teams) {
			if (!canWeapons) {
				items.weapons.clear();
			}
			if (!(permissions.knives.empty() || allowed(permissions.knives))) {
				items.knife.reset();
			}
			if (!(permissions.gloves.empty() || allowed(permissions.gloves))) {
				items.glove.reset();
			}
			if (!(permissions.agents.empty() || allowed(permissions.agents))) {
				items.agent = 0;
			}
			if (!canStickers || !canKeychains) {
				for (auto& [definition, skin] : items.weapons) {
					(void)definition;
					if (!canStickers) {
						ClearStickers(skin);
					}
					if (!canKeychains) {
						skin.keychain = Keychain{};
					}
				}
			}
		}
		if (!(permissions.music.empty() || allowed(permissions.music))) {
			view.music = 0;
		}
	}
	// The original fixes stickers where the skin is applied, and only for weapons
	// (HookInventoryUpdateService.cs:458) — the knife/glove/agent paths have no stickers.
	for (TeamItems& items : view.teams) {
		for (auto& [definition, skin] : items.weapons) {
			(void)definition;
			FixSticker(skin, stickerHashes);
		}
	}
}

// Key of a pending StatTrak delta: (team, definition). Team is the engine value (2 = T, 3 = CT).
inline uint64_t DeltaKey(int team, uint16_t definition) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(team)) << 32) | definition;
}

// True when the active weapon of `definition` maps to the StatTrak skin of that team.
inline Skin* FindMutableStatTrakSkin(Snapshot& view, int team, uint16_t definition) {
	const int index = TeamIndex(team);
	if (index < 0) {
		return nullptr;
	}
	TeamItems& items = view.teams[static_cast<std::size_t>(index)];
	if (IsKnife(definition)) {
		if (!items.knife || items.knife->quality != kStatTrakQuality) {
			return nullptr;
		}
		return &*items.knife;
	}
	const auto it = items.weapons.find(definition);
	if (it == items.weapons.end() || it->second.quality != kStatTrakQuality) {
		return nullptr;
	}
	return &it->second;
}

inline const Skin* FindStatTrakSkin(const Snapshot& view, int team, uint16_t definition) {
	return FindMutableStatTrakSkin(const_cast<Snapshot&>(view), team, definition);
}

// --- the StatTrak delta ledger ---------------------------------------------------------------------
//
// Every kill adds +1 to a per-key (team, definition) counter that has to reach the database as a
// delta (Storage::SaveStatTrak does `count = count + ?`, so a website edit is never overwritten).
// The ledger keeps two numbers per key:
//   owed     — kills that have not received a successful persistence acknowledgement,
//   inflight — the amount of that debt handed to the storage and not acknowledged yet.
// A key may be queued while another batch is still in flight (the new kill must not wait), and a
// batch is only subtracted from `owed` when the storage confirms it. A failed batch remains owed.
// An ambiguous MySQL error may follow a committed UPDATE; retrying that delta is at-least-once,
// not an exactly-once guarantee. Batch IDs prevent duplicate callback accounting only.
// Batches are bounded (int32) so an old debt cannot overflow the storage parameter.
struct DeltaBatch {
	uint64_t id = 0; // identifies the batch in Ack() (a duplicated/late ack is ignored)
	uint64_t key = 0;
	int32_t amount = 0;
};

class DeltaLedger {
public:
	// A single statement never carries more than this (int32-safe, and a stuck key cannot grow the
	// parameter without bound).
	static constexpr int64_t kMaxBatch = 1000000;

	void Add(uint64_t key, int64_t delta) {
		if (delta == 0) {
			return;
		}
		entries_[key].owed += delta;
	}

	// Batches that may be written now: `owed - inflight` per key, at most one batch per key per
	// call. Queueing while a batch is in flight is allowed and only carries the new kills.
	std::vector<DeltaBatch> Queue() {
		std::vector<DeltaBatch> batches;
		for (auto& [key, entry] : entries_) {
			const int64_t available = entry.owed - entry.inflight;
			if (available <= 0) {
				continue;
			}
			const int64_t amount = available > kMaxBatch ? kMaxBatch : available;
			entry.inflight += amount;
			DeltaBatch batch;
			batch.id = ++nextId_;
			batch.key = key;
			batch.amount = static_cast<int32_t>(amount);
			outstanding_.insert(batch.id);
			batches.push_back(batch);
		}
		return batches;
	}

	// Confirms (ok) or rejects a batch. Returns false for an unknown id (a duplicated or late
	// acknowledgement never changes the counters). On success the amount leaves `owed`; on failure
	// only `inflight` is released, so the kills stay owed and are retried.
	bool Ack(const DeltaBatch& batch, bool ok) {
		if (outstanding_.erase(batch.id) == 0) {
			return false;
		}
		const auto it = entries_.find(batch.key);
		if (it == entries_.end()) {
			return false;
		}
		Entry& entry = it->second;
		entry.inflight -= batch.amount;
		if (entry.inflight < 0) {
			entry.inflight = 0;
		}
		if (ok) {
			entry.owed -= batch.amount;
			if (entry.owed < 0) {
				entry.owed = 0;
			}
		}
		if (entry.owed == 0 && entry.inflight == 0) {
			entries_.erase(it);
		}
		return true;
	}

	bool Empty() const { return entries_.empty(); }
	size_t Keys() const { return entries_.size(); }

	int64_t Owed(uint64_t key) const {
		const auto it = entries_.find(key);
		return it == entries_.end() ? 0 : it->second.owed;
	}

	int64_t TotalOwed() const {
		int64_t total = 0;
		for (const auto& [key, entry] : entries_) {
			(void)key;
			total += entry.owed;
		}
		return total;
	}

	int64_t TotalInflight() const {
		int64_t total = 0;
		for (const auto& [key, entry] : entries_) {
			(void)key;
			total += entry.inflight;
		}
		return total;
	}

private:
	struct Entry {
		int64_t owed = 0;
		int64_t inflight = 0;
	};
	std::unordered_map<uint64_t, Entry> entries_;
	std::unordered_set<uint64_t> outstanding_;
	uint64_t nextId_ = 0;
};

// --- load request helpers ---------------------------------------------------------------------------
// Ordinary failures respect nextRetry; a live read is superseded only after its deadline. A !wp
// during a live read joins that read rather than releasing its write barrier.
inline bool ShouldRequestLoad(const SlotBinding& binding, bool force, Clock::time_point now) {
	if (binding.loadRequested) {
		return now >= binding.nextRetry;
	}
	return force || now >= binding.nextRetry;
}

inline bool JoinPlayerLoad(const SlotBinding& binding, bool force, bool sync, Clock::time_point now,
						   bool& pendingForce, bool& pendingSync) {
	if (!binding.loadRequested || now >= binding.nextRetry) {
		return false;
	}
	pendingForce = pendingForce || force;
	pendingSync = pendingSync || sync;
	return true;
}

// A response only belongs to the live request of its slot: any newer request (or a cleared slot)
// invalidates the older one.
inline bool ResponseIsCurrent(uint64_t responseId, uint64_t latestId) {
	return responseId != 0 && responseId == latestId;
}

enum class LoadDrainState { Waiting, Ready, TimedOut };

// An expired drain may fail the request, but must never issue a read with debt still outstanding.
inline LoadDrainState CheckLoadDrain(const DeltaLedger& ledger, Clock::time_point now,
									Clock::time_point deadline) {
	if (ledger.Empty()) {
		return LoadDrainState::Ready;
	}
	return now < deadline ? LoadDrainState::Waiting : LoadDrainState::TimedOut;
}

inline bool LoadBlocksWrites(uint64_t steamId, const std::array<SlotBinding, kSlots>& slots) {
	for (const SlotBinding& binding : slots) {
		if (binding.steamId == steamId && binding.loadRequested) {
			return true;
		}
	}
	return false;
}

// Used by the real flush path: even a final flush cannot enqueue post-issue kills while a read
// for this player is active. Disconnect / map end / shutdown clear bindings to release it.
inline std::vector<DeltaBatch> QueuePlayerDeltas(DeltaLedger& ledger, uint64_t steamId,
												const std::array<SlotBinding, kSlots>& slots) {
	return LoadBlocksWrites(steamId, slots) ? std::vector<DeltaBatch>{} : ledger.Queue();
}

// The read starts only after all previous batches are acknowledged. While it is active, no new
// batch can enter Storage, so every owed kill at callback time is absent from its DB snapshot.
inline bool BeginPlayerLoad(SlotBinding& binding, const DeltaLedger& ledger, uint64_t requestId,
							uint64_t& latestRequest, Clock::time_point readDeadline) {
	if (binding.steamId == 0 || binding.loadRequested || !ledger.Empty() || requestId == 0) {
		return false;
	}
	latestRequest = requestId;
	binding.loadRequested = true;
	binding.nextRetry = readDeadline;
	return true;
}

inline void InvalidatePlayerLoad(SlotBinding& binding, uint64_t& latestRequest) {
	latestRequest = 0;
	binding.loadRequested = false;
}

inline void MergePendingCounters(const DeltaLedger& ledger, Snapshot& snapshot) {
	const auto add = [&](Skin& skin, int team) {
		const int64_t owed = ledger.Owed(DeltaKey(team, skin.definition));
		if (owed <= 0) {
			return;
		}
		const int64_t room = static_cast<int64_t>(std::numeric_limits<int>::max()) - skin.stattrak;
		skin.stattrak = owed >= room ? std::numeric_limits<int>::max()
								: static_cast<int>(static_cast<int64_t>(skin.stattrak) + owed);
	};
	for (std::size_t index = 0; index < snapshot.teams.size(); ++index) {
		const int team = index == 0 ? 2 : 3;
		for (auto& [definition, skin] : snapshot.teams[index].weapons) {
			(void)definition;
			add(skin, team);
		}
		if (snapshot.teams[index].knife) {
			add(*snapshot.teams[index].knife, team);
		}
	}
}

// A cache may be dropped/evicted only when it holds no unpersisted StatTrak state; an entry with
// unsaved or in-flight kills stays until the storage has acknowledged them (the storage outlives
// the player, so the kills are still written — only the callback would be dropped).
inline bool CacheDroppable(const DeltaLedger& ledger) { return ledger.Empty(); }

// --- chat message resolution ------------------------------------------------------------------------
//
// Translations::Get answers the *key itself* when the phrases file has no entry for it (its
// contract, config.cpp). The shipped production translation files carry the menu keys only, so
// every sync message key ("sync.started", ...) comes back as the key - which must be treated as
// "missing" and answered with the message from config.toml, never printed literally.
inline bool TranslationMissing(std::string_view text, std::string_view key) {
	return text.empty() || text == key;
}

// `get(language, key)` is Translations::Get (or a test double). The translation wins; the caller's
// config string is used when the phrase is missing.
template <typename Get>
std::string ResolveMessage(Get&& get, std::string_view language, std::string_view key,
						   std::string_view fallback) {
	std::string text = get(language, key);
	if (TranslationMissing(text, key)) {
		text = std::string(fallback);
	}
	return text;
}

// --- the per-player cache -------------------------------------------------------------------------
struct PlayerCache {
	Snapshot stored; // what the database said, kept in sync with the runtime StatTrak counts
	Snapshot view;	 // what the engine applies
	std::unordered_map<uint64_t, int64_t> stickerHashes;
	DeltaLedger ledger; // StatTrak kills that still have to reach the database
	Clock::time_point lastUse{};
	Clock::time_point deltaNextRetry{};
	// Identity of this cache instance: a StatTrak save callback only touches the entry it was
	// created for, even if the player rejoined and the entry was replaced meanwhile.
	uint64_t token = 0;
	bool loaded = false;
	// A spawn or a !wp happened before the row was loaded: apply as soon as the load lands (the
	// player is re-checked for aliveness at that moment).
	bool applyWhenLoaded = false;
};

} // namespace rt

// --- the runtime -----------------------------------------------------------------------------------
//
// plugin.cpp owns one instance. Start() never throws and never returns an error that would stop
// the plugin: a broken config, database or gamedata only disables the corresponding feature
// (one Failed OnPluginStart takes down every Plugify plugin).
class Runtime {
public:
	Runtime();
	~Runtime();
	Runtime(const Runtime&) = delete;
	Runtime& operator=(const Runtime&) = delete;

	// `location` is the plugin's own location (plg::Plugin::GetLocation()); it is used as the first
	// candidate for the gamedata file.
	void Start(std::string_view location);
	void Update(std::chrono::milliseconds dt);
	void Stop();

	// The ws_status body: counters and lazyload/hook/signature state, never any secret.
	std::string StatusText() const;

	// Callback entry points for the trampolines in runtime.cpp (s2sdk listeners, pisexcompat
	// events). They never throw. The event parameters stay void* so that this header does not pull
	// in the SDK headers; HandleRoundMvp returns the s2sdk ResultType value (0 = Continue).
	void HandleClientDisconnect(int slot);
	void HandleMapStart();
	void HandleMapEnd();
	void HandleSpawn(void* nativeEvent);
	void HandleDeath(void* nativeEvent);
	void HandleRoundStart();
	int HandleRoundMvp(void* nativeEvent);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

// The single runtime instance of this plugin (used by the s2sdk/pisex callback trampolines in
// runtime.cpp).
Runtime& RuntimeInstance();

} // namespace ws
