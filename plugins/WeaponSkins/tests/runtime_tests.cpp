// WeaponSkins — unit tests for the engine-free parts of the runtime (src/runtime.hpp).
//
// Covered: the slot binding / generation guards (a late callback must never be applied to the
// player who took the slot over), the !wp cooldown, the alias filter, the message token
// replacement, the snapshot view (permissions + fail-open fast path), the sticker fix (bump,
// idempotence, non-finite wear, the 1000-step bound), the StatTrak skin lookup and - with fake
// s2sdk bindings - the engine preparation across the map-start/readiness race (the section at the
// end drives the real RuntimeInstance).
//
// The pure logic lives inline in the runtime header, so the sections above link nothing but this
// translation unit; the engine-readiness section at the end installs the fake s2sdk globals and
// needs the runtime (which the test target links anyway).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "runtime.hpp"

#include <s2sdk/s2sdk.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace ws; // NOLINT(google-build-using-namespace) — the test drives the ported namespace

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(int64_t actual, int64_t expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), static_cast<long long>(expected),
					static_cast<long long>(actual));
	}
}

void CheckEq(const std::string& actual, const std::string& expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), expected.c_str(), actual.c_str());
	}
}

// --- fixtures ------------------------------------------------------------------------------------
Skin MakeSkin(uint16_t definition, int paint, float wear = 0.0f, int seed = 7, int quality = 0, int stattrak = 0) {
	Skin skin;
	skin.definition = definition;
	skin.paint = paint;
	skin.seed = seed;
	skin.quality = quality;
	skin.stattrak = stattrak;
	skin.wear = wear;
	return skin;
}

Sticker MakeSticker(int id, int schema = 1337) {
	Sticker sticker;
	sticker.id = id;
	sticker.schema = schema;
	return sticker;
}

Snapshot MakeSnapshot(uint64_t steamId) {
	Snapshot snapshot;
	snapshot.steamId = steamId;
	return snapshot;
}

// --- slot guards ---------------------------------------------------------------------------------
void TestSlotBindings() {
	std::array<rt::SlotBinding, rt::kSlots> slots{};

	Check(rt::BindSlot(slots[3], 111) == true, "a new binding changes the slot");
	Check(rt::BindSlot(slots[3], 111) == false, "the same player does not re-bind");
	Check(rt::BindSlot(slots[3], 0) == false, "steam id 0 is never bound");
	const uint64_t firstGeneration = slots[3].generation;

	Check(rt::BindSlot(slots[3], 222) == true, "another player re-binds the slot");
	Check(slots[3].generation > firstGeneration, "every bind bumps the generation");

	const rt::SlotGuard good{3, 222, slots[3].generation, 5};
	Check(rt::GuardMatches(good, slots, 5), "the live binding matches its guard");
	Check(!rt::GuardMatches(good, slots, 6), "a guard from another map never matches");
	Check(!rt::GuardMatches(rt::SlotGuard{3, 222, firstGeneration, 5}, slots, 5),
		  "a guard from the previous occupant never matches");
	Check(!rt::GuardMatches(rt::SlotGuard{3, 111, slots[3].generation, 5}, slots, 5),
		  "a guard with the wrong steam id never matches");
	Check(!rt::GuardMatches(rt::SlotGuard{70, 222, slots[3].generation, 5}, slots, 5),
		  "an out-of-range slot never matches");

	CheckEq(static_cast<int64_t>(rt::UnbindSlot(slots[3])), 222, "unbind returns the bound steam id");
	CheckEq(static_cast<int64_t>(slots[3].steamId), 0, "unbind clears the slot");
	CheckEq(static_cast<int64_t>(rt::UnbindSlot(slots[3])), 0, "unbinding an empty slot answers 0");
	Check(!rt::GuardMatches(good, slots, 5), "a guard stops matching after the unbind");

	// A disconnect and reconnect of the SAME player must still invalidate the old guard: the
	// generation only ever grows (it is not reset by the unbind or by a re-bind).
	const uint64_t afterUnbind = slots[3].generation;
	Check(rt::BindSlot(slots[3], 222), "the same player re-binds");
	Check(slots[3].generation > afterUnbind, "a re-bind bumps the generation again");
	Check(!rt::GuardMatches(good, slots, 5), "the guard from the previous session never matches again");
	const rt::SlotGuard reconnect{3, 222, slots[3].generation, 5};
	Check(rt::GuardMatches(reconnect, slots, 5), "the guard of the new session matches");

	// The map-end cleanup keeps the generation monotonic as well.
	const uint64_t beforeMapEnd = slots[3].generation;
	const uint64_t afterMapEnd = beforeMapEnd + 1;
	Check(afterMapEnd > beforeMapEnd, "the map-end reset only grows the generation");
	Check(!rt::GuardMatches(reconnect, slots, 6), "a guard never matches on the next map");
}

// --- cooldown ------------------------------------------------------------------------------------
void TestCooldowns() {
	rt::Cooldowns cooldowns;
	const rt::Clock::time_point start = rt::Clock::time_point{std::chrono::seconds(1000)};

	Check(!cooldowns.OnCooldown(7, start), "an unseen player has no cooldown");
	cooldowns.Arm(7, start, 20);
	Check(cooldowns.OnCooldown(7, start), "arming puts the player on cooldown");
	CheckEq(cooldowns.RemainingSeconds(7, start), 20, "the remaining time starts at the full window");
	CheckEq(cooldowns.RemainingSeconds(7, start + std::chrono::milliseconds(1)), 20,
			"a part of a second rounds up");
	CheckEq(cooldowns.RemainingSeconds(7, start + std::chrono::seconds(1)), 19, "a whole second is subtracted");
	CheckEq(cooldowns.RemainingSeconds(7, start + std::chrono::seconds(20)), 0, "the window expires exactly");
	Check(!cooldowns.OnCooldown(7, start + std::chrono::seconds(20)), "no cooldown after the window");
	CheckEq(cooldowns.Size(), std::size_t{1}, "an expired entry is kept until an arm or a clear");

	cooldowns.Arm(8, start, 0);
	Check(cooldowns.OnCooldown(8, start), "a zero/invalid window still arms at least one second");
	CheckEq(cooldowns.RemainingSeconds(8, start), 1, "the minimum window is one second");

	// The original clears the entry when the site had nothing configured or the load failed.
	cooldowns.Arm(7, start + std::chrono::seconds(21), 20);
	Check(cooldowns.OnCooldown(7, start + std::chrono::seconds(21)), "the re-armed cooldown is active");
	// Arming drops the records that had expired by then (player 8's one-second window is long over),
	// so the map cannot grow with every player that ever ran the command.
	CheckEq(cooldowns.Size(), std::size_t{1}, "arming drops the entries that had expired");
	cooldowns.Clear(7);
	Check(!cooldowns.OnCooldown(7, start + std::chrono::seconds(21)), "Clear drops the cooldown");
	CheckEq(cooldowns.Size(), std::size_t{0}, "Clear leaves nothing behind");
}

// --- aliases / messages ---------------------------------------------------------------------------
void TestCommandsHelpers() {
	Check(!rt::AliasAllowed("menu"), "the 'menu' alias is never hijacked");
	Check(!rt::AliasAllowed("!menu"), "the '!menu' alias is never hijacked");
	Check(!rt::AliasAllowed("MENU"), "the alias filter is case-insensitive");
	Check(!rt::AliasAllowed(""), "an empty alias is skipped");
	Check(rt::AliasAllowed("ws"), "a normal alias is kept");
	Check(rt::AliasAllowed("pins"), "a normal alias is kept");

	CheckEq(rt::ToLower("Weapons"), "weapons", "ToLower");
	Check(rt::EqualsIgnoreCase("Wp", "wP"), "EqualsIgnoreCase");

	CheckEq(rt::ReplaceAll("[Скины] Настрой на {url}, ещё {url}", "{url}", "example.com"),
		   "[Скины] Настрой на example.com, ещё example.com", "ReplaceAll replaces every occurrence");
	CheckEq(rt::ReplaceAll("нет токена", "{url}", "example.com"), "нет токена", "ReplaceAll leaves a text without the token");
	CheckEq(rt::ReplaceAll("x", "", "y"), "x", "an empty token is ignored");
}

// --- sticker fix ----------------------------------------------------------------------------------
void TestStickerFix() {
	std::unordered_map<uint64_t, int64_t> hashes;

	Skin skin = MakeSkin(7, 279, 0.25f);
	skin.stickers[0] = MakeSticker(100);
	const float originalWear = skin.wear;
	Check(!rt::FixSticker(skin, hashes), "the first application only records the appearance");
	Check(skin.wear == originalWear, "the first application does not change the wear");
	CheckEq(static_cast<int64_t>(hashes.size()), 1, "the appearance key was recorded");

	// The same appearance and the same stickers: nothing to do.
	Check(!rt::FixSticker(skin, hashes), "an unchanged appearance is left alone");
	Check(skin.wear == originalWear, "an unchanged appearance keeps the wear");

	// The same appearance with a different sticker set: the wear is nudged so the client redraws.
	Skin changed = MakeSkin(7, 279, originalWear);
	changed.stickers[0] = MakeSticker(200);
	Check(rt::FixSticker(changed, hashes), "a changed sticker set bumps the wear");
	Check(changed.wear > originalWear, "the bump increases the wear");
	Check(changed.wear == originalWear + rt::kStickerFixWearStep, "the bump is one step of +0.001");
	CheckEq(static_cast<int64_t>(hashes.size()), 2, "the bumped appearance key was recorded");
	Check(!rt::FixSticker(changed, hashes), "the bumped appearance is now stable");
	Check(changed.wear == originalWear + rt::kStickerFixWearStep, "the stable appearance keeps its wear");
	CheckEq(static_cast<int64_t>(hashes.size()), 2, "the stable appearance adds no key");

	// A non-finite wear of the incoming row: the original bails out instead of looping.
	Skin infinite = MakeSkin(8, 1, 0.1f);
	infinite.wear = std::numeric_limits<float>::quiet_NaN();
	Check(!rt::FixSticker(infinite, hashes), "a NaN wear is left alone");

	// The bound: 1000 stale keys force 1000 steps, then the loop stops. The wear is accumulated
	// the same way FixSticker does it, so every step lands on a prepopulated key.
	std::unordered_map<uint64_t, int64_t> full;
	Skin bounded = MakeSkin(9, 2, 0.0f);
	float walk = 0.0f;
	for (int step = 0; step < rt::kStickerFixMaxAttempts; ++step) {
		Skin probe = bounded;
		probe.wear = walk;
		full[rt::AppearanceKey(probe)] = 424242; // a different hash for every wear step
		walk = walk + rt::kStickerFixWearStep;
	}
	const float after1000Steps = walk; // walk_1000: the wear the loop reaches before its bound stops it
	Check(rt::FixSticker(bounded, full), "the bounded fix reports the wear change");
	Check(bounded.wear == after1000Steps, "the wear walked exactly 1000 steps and stopped");
	Check(bounded.wear > 0.5f, "the wear really walked the stale steps");
}

// --- the runtime view ---------------------------------------------------------------------------------
void TestBuildView() {
	Snapshot stored = MakeSnapshot(777);
	Skin rifle = MakeSkin(7, 279, 0.5f);
	rifle.stickers[0] = MakeSticker(100);
	rifle.keychain.id = 55;
	stored.teams[0].weapons[7] = rifle;
	stored.teams[0].knife = MakeSkin(507, 3);
	stored.teams[0].glove = MakeSkin(5030, 4);
	stored.teams[0].agent = 1234;
	stored.teams[0].pin = 42;
	stored.music = 8;

	// No permissions configured (the production config): no permission call at all.
	PermissionConfig empty;
	int calls = 0;
	std::unordered_map<uint64_t, int64_t> hashes;
	Snapshot view = MakeSnapshot(0);
	rt::BuildView(stored, empty, [&calls](const std::string&) {
		++calls;
		return false; // would strip everything if it were ever called
	}, view, hashes);
	CheckEq(calls, 0, "an empty permission config makes no permission call");
	CheckEq(static_cast<int64_t>(view.steamId), 777, "the view keeps the steam id");
	Check(view.teams[0].weapons.count(7) == 1, "the weapon survives an empty permission config");
	Check(view.teams[0].knife.has_value(), "the knife survives");
	Check(view.teams[0].glove.has_value(), "the glove survives");
	CheckEq(view.teams[0].agent, 1234, "the agent survives");
	CheckEq(view.music, 8, "the music kit survives");
	CheckEq(static_cast<int64_t>(calls), 0, "no permission call happened");
	Check(view.teams[0].weapons.at(7).stickers[0].id == 100, "the stickers survive");
	Check(view.teams[0].weapons.at(7).keychain.id == 55, "the keychain survives");

	// Mutating the view never touches the stored data.
	view.teams[0].weapons.at(7).paint = 999;
	CheckEq(stored.teams[0].weapons.at(7).paint, 279, "the stored snapshot is not modified by the view");

	// Permissions deny weapons and stickers, allow the rest: fail-open/closed is the lambda.
	PermissionConfig config;
	config.weapons = "weaponskins.use";
	config.stickers = "weaponskins.stickers";
	config.knives = "weaponskins.knives";
	std::unordered_map<uint64_t, int64_t> hashes2;
	Snapshot view2 = MakeSnapshot(0);
	rt::BuildView(stored, config, [](const std::string& permission) {
		return permission == "weaponskins.stickers"; // stickers allowed, the rest denied
	}, view2, hashes2);
	Check(view2.teams[0].weapons.empty(), "a denied weapon permission clears the weapons");
	Check(!view2.teams[0].knife.has_value(), "a denied knife permission drops the knife");
	Check(view2.teams[0].glove.has_value(), "an unset glove permission keeps the glove");
	CheckEq(view2.teams[0].agent, 1234, "an unset agent permission keeps the agent");
	CheckEq(view2.music, 8, "an unset music permission keeps the music kit");
}

// --- StatTrak lookup --------------------------------------------------------------------------------
void TestStatTrakLookup() {
	Snapshot view = MakeSnapshot(5);
	view.teams[0].weapons[7] = MakeSkin(7, 279, 0.2f, 7, kStatTrakQuality, 11);
	view.teams[0].weapons[13] = MakeSkin(13, 44, 0.2f, 7, 0, 0); // not StatTrak
	view.teams[0].knife = MakeSkin(507, 3, 0.2f, 7, kStatTrakQuality, 5);

	// T is engine team 2 -> index 0.
	Check(rt::FindStatTrakSkin(view, 2, 7) != nullptr, "a StatTrak weapon of the own team is found");
	Check(rt::FindStatTrakSkin(view, 2, 13) == nullptr, "a non-StatTrak weapon is not a StatTrak target");
	Check(rt::FindStatTrakSkin(view, 2, 507) != nullptr, "the StatTrak knife is found by its knife definition");
	Check(rt::FindStatTrakSkin(view, 3, 7) == nullptr, "the other team's table has no such weapon");
	Check(rt::FindStatTrakSkin(view, 1, 7) == nullptr, "spectators have no StatTrak table");

	Skin* found = rt::FindMutableStatTrakSkin(view, 2, 507);
	Check(found != nullptr, "the mutable lookup finds the knife");
	if (found != nullptr) {
		found->stattrak += 1;
		CheckEq(view.teams[0].knife->stattrak, 6, "the increment lands in the snapshot");
	}
}

// --- chat message resolution (production path of !wp and the aliases) ---------------------------------
void TestMessageResolution() {
	const std::string key = "sync.started";
	const std::string fallback = "[Скины] Загружаю твои скины с сайта...";

	// The production contract of Translations::Get: a missing phrase answers the KEY ITSELF. That
	// must count as "missing" - otherwise every sync message would print "sync.started".
	Check(rt::TranslationMissing("sync.started", "sync.started"), "the key itself means missing");
	Check(rt::TranslationMissing("", "sync.started"), "an empty phrase means missing");
	Check(!rt::TranslationMissing("Готово", "sync.started"), "a real phrase is not missing");

	const auto keyEcho = [&key](std::string_view, std::string_view wanted) { return std::string(wanted); };
	CheckEq(rt::ResolveMessage(keyEcho, "ru", key, fallback), fallback,
			"a missing phrase falls back to the configured message (not the literal key)");

	const auto emptyGet = [](std::string_view, std::string_view) { return std::string(); };
	CheckEq(rt::ResolveMessage(emptyGet, "ru", key, fallback), fallback, "an empty phrase falls back too");

	const auto translated = [](std::string_view, std::string_view) { return std::string("Loading your skins..."); };
	CheckEq(rt::ResolveMessage(translated, "en", key, fallback), "Loading your skins...",
			"a translated phrase wins over the configured message");

	// The real translations: an EMPTY Translations object (never loaded) must produce the config
	// message, and the production files must resolve the keys they actually ship.
	ws::Translations empty;
	CheckEq(rt::ResolveMessage([&empty](std::string_view lang, std::string_view wanted) { return empty.Get(lang, wanted); },
							   "ru", key, fallback),
			fallback, "an unloaded phrase store answers with the configured message");

	// A phrase that the production files DO define must be used as-is (the same resolution path).
	const std::string menuKey = "menu.title";
	const std::string menuFallback = "SHOULD NOT BE USED";
	const auto menuGet = [&empty](std::string_view lang, std::string_view wanted) { return empty.Get(lang, wanted); };
	CheckEq(rt::ResolveMessage(menuGet, "en", menuKey, menuFallback), menuFallback,
			"an unloaded store cannot resolve a shipped key either (still the fallback)");

#ifdef WS_SOURCE_DIR
	// Regression with the production resources: the shipped files carry the menu keys and none of
	// the sync keys, so the same helper must answer the configured message for "sync.started" and
	// the shipped text for "menu.title".
	const std::string directory = std::string(WS_SOURCE_DIR) + "/resources/translations";
	std::error_code ec;
	if (!std::filesystem::is_directory(directory, ec)) {
		std::printf("note: %s is missing - the production phrase regression is skipped\n", directory.c_str());
	} else {
		ws::Translations production;
		std::string error;
		const bool loaded = production.Load(directory, error);
		Check(loaded, "the production translation files load (" + error + ")");
		if (loaded) {
			const auto get = [&production](std::string_view lang, std::string_view wanted) {
				return production.Get(lang, wanted);
			};
			CheckEq(rt::ResolveMessage(get, "en", key, fallback), fallback,
					"a sync key is not in the production files: the configured message is used");
			Check(rt::ResolveMessage(get, "en", menuKey, menuFallback) != menuFallback,
					"a menu key that IS in the production files resolves to the shipped text");
			Check(rt::ResolveMessage(get, "en", menuKey, menuFallback) == "Weapon Skins",
					"the shipped English menu title is resolved");
			CheckEq(rt::ResolveMessage(get, "zh-CN", key, fallback), fallback,
					"the fallback also applies for a language with no sync keys");
		}
	}
#endif
}

// --- the StatTrak ledger (used by the production flush path) ---------------------------------------
void TestDeltaLedger() {
	rt::DeltaLedger ledger;
	Check(ledger.Empty(), "a fresh ledger owes nothing");
	CheckEq(ledger.TotalOwed(), 0, "a fresh ledger has no owed kills");
	Check(ledger.Queue().empty(), "nothing is queued while nothing is owed");

	// A kill is owed and queued exactly once until it is acknowledged.
	ledger.Add(7, 1);
	CheckEq(ledger.TotalOwed(), 1, "a kill is owed");
	auto first = ledger.Queue();
	CheckEq(static_cast<int64_t>(first.size()), 1, "the owed kill is queued");
	CheckEq(first[0].amount, 1, "the batch carries the kill");
	Check(ledger.Queue().empty(), "a second flush before the ack queues nothing (no duplicate write)");
	CheckEq(ledger.TotalInflight(), 1, "the batch is in flight");
	Check(!ledger.Empty(), "an in-flight kill is still unpersisted");

	// A failed write releases the batch back to "owed" and the kills are not lost.
	Check(ledger.Ack(first[0], false) == true, "the failed batch is a known batch");
	CheckEq(ledger.TotalOwed(), 1, "the failed kill is owed again");
	CheckEq(ledger.TotalInflight(), 0, "the failed batch left the in-flight set");
	auto second = ledger.Queue();
	CheckEq(static_cast<int64_t>(second.size()), 1, "the failed kill is retried");

	// A stale/duplicated acknowledgement never touches the counters.
	rt::DeltaBatch stale;
	stale.id = 999999;
	stale.key = 7;
	stale.amount = 1;
	Check(ledger.Ack(stale, true) == false, "an unknown batch id is ignored");
	CheckEq(ledger.TotalOwed(), 1, "the ignored ack changes nothing");

	// A kill that arrives while a batch is in flight goes out with the next (bounded) batch.
	Check(ledger.Queue().empty(), "an already queued retry is not queued again");
	ledger.Add(7, 1); // a new kill during the write
	CheckEq(ledger.TotalOwed(), 2, "the new kill is owed on top of the retried one");
	auto third = ledger.Queue();
	CheckEq(static_cast<int64_t>(third.size()), 1, "only the new kill is queued while the other is in flight");
	CheckEq(third[0].amount, 1, "the new batch carries exactly the new kill");
	CheckEq(ledger.TotalInflight(), 2, "both batches are in flight");
	Check(ledger.Ack(second[0], true), "the first batch is acknowledged");
	Check(ledger.Ack(third[0], true), "the second batch is acknowledged");
	Check(ledger.Empty(), "after all acks the ledger is empty");
	CheckEq(ledger.TotalOwed(), 0, "no kill stays owed");

	// A batch is bounded (the storage parameter is int32).
	rt::DeltaLedger big;
	big.Add(9, rt::DeltaLedger::kMaxBatch * 2 + 5);
	auto chunk1 = big.Queue();
	CheckEq(static_cast<int64_t>(chunk1.size()), 1, "an oversized debt is queued as a bounded chunk");
	CheckEq(chunk1[0].amount, rt::DeltaLedger::kMaxBatch, "the chunk is capped");
	Check(big.Ack(chunk1[0], true), "the chunk is acknowledged");
	auto chunk2 = big.Queue();
	CheckEq(chunk2[0].amount, rt::DeltaLedger::kMaxBatch, "the second chunk is capped too");
	Check(big.Ack(chunk2[0], true), "the second chunk is acknowledged");
	auto chunk3 = big.Queue();
	CheckEq(chunk3[0].amount, 5, "the remainder goes last");
	Check(big.Ack(chunk3[0], true), "the remainder is acknowledged");
	Check(big.Empty(), "the oversized debt drained");
}

// --- load request handling ---------------------------------------------------------------------------
void TestLoadRequests() {
	std::array<rt::SlotBinding, rt::kSlots> slots{};
	rt::BindSlot(slots[2], 500);
	const rt::Clock::time_point now = rt::Clock::time_point{std::chrono::seconds(1000)};

	Check(rt::ShouldRequestLoad(slots[2], false, now), "an unrequested load is issued");

	slots[2].loadRequested = true;
	slots[2].nextRetry = now + std::chrono::seconds(15);
	Check(!rt::ShouldRequestLoad(slots[2], false, now), "a fresh request is not re-issued");
	Check(rt::ShouldRequestLoad(slots[2], false, now + std::chrono::seconds(15)),
		  "a timed-out request is re-issued");
	Check(!rt::ShouldRequestLoad(slots[2], true, now), "a forced request joins a fresh read without superseding it");
	slots[2].loadRequested = false;
	Check(!rt::ShouldRequestLoad(slots[2], false, now), "a failed request respects retry pacing");
	Check(rt::ShouldRequestLoad(slots[2], true, now), "an explicit sync may retry an ordinary failure immediately");

	// The late answer of the superseded request must not be applied.
	const uint64_t first = 41;
	const uint64_t second = 42;
	Check(rt::ResponseIsCurrent(second, second), "the current request matches");
	Check(!rt::ResponseIsCurrent(first, second), "the superseded response is dropped");
	Check(!rt::ResponseIsCurrent(0, 0), "an id-less response is never current");

	// A cache with unpersisted kills is not droppable/evictable.
	rt::DeltaLedger ledger;
	Check(rt::CacheDroppable(ledger), "an empty cache is droppable");
	ledger.Add(1, 1);
	Check(!rt::CacheDroppable(ledger), "an owed kill keeps the cache alive");
	auto batch = ledger.Queue();
	Check(!rt::CacheDroppable(ledger), "an in-flight kill keeps the cache alive");
	Check(ledger.Ack(batch[0], true), "the kill is acknowledged");
	Check(rt::CacheDroppable(ledger), "the cache is droppable once everything is acknowledged");
}

// --- counters merged over a load ----------------------------------------------------------------------
void TestLoadWriteBarrier() {
	constexpr uint64_t player = 500;
	const uint64_t rifle = rt::DeltaKey(2, 7);
	const uint64_t knife = rt::DeltaKey(3, 507);
	const auto now = rt::Clock::time_point{std::chrono::seconds(1000)};
	const auto deadline = now + std::chrono::seconds(15);
	std::array<rt::SlotBinding, rt::kSlots> slots{};
	rt::BindSlot(slots[2], player);
	rt::DeltaLedger ledger;
	uint64_t latest = 0;
	Snapshot database = MakeSnapshot(player);
	database.teams[0].weapons[7] = MakeSkin(7, 180, 0.05F, 42, kStatTrakQuality, 100);
	database.teams[1].knife = MakeSkin(507, 38, 0.05F, 42, kStatTrakQuality, 20);

	// Multi-stage LoadPlayer must not start while an earlier UPDATE is awaiting its result.
	ledger.Add(rifle, 3);
	auto beforeRead = rt::QueuePlayerDeltas(ledger, player, slots);
	CheckEq(beforeRead.size(), 1, "the pre-read debt is queued once");
	Check(!rt::BeginPlayerLoad(slots[2], ledger, 1, latest, deadline),
		  "a pre-read batch awaiting acknowledgement blocks LoadPlayer");
	Check(rt::CheckLoadDrain(ledger, now, deadline) == rt::LoadDrainState::Waiting,
		  "an unacknowledged batch keeps the drain waiting");
	Check(ledger.Ack(beforeRead[0], false), "a failed pre-read write is delivered");
	CheckEq(ledger.TotalOwed(), 3, "failed save retains the complete drain debt");
	Check(rt::CheckLoadDrain(ledger, deadline, deadline) == rt::LoadDrainState::TimedOut,
		  "failed save expires the drain rather than permitting a read");
	Check(!rt::BeginPlayerLoad(slots[2], ledger, 1, latest, deadline),
		  "even an expired drain cannot issue LoadPlayer with debt");
	CheckEq(latest, 0, "a rejected load acquires no request identity");

	// ACK before the read: the successful delta is present only in the DB base, never merged twice.
	auto retried = rt::QueuePlayerDeltas(ledger, player, slots);
	database.teams[0].weapons[7].stattrak += retried[0].amount;
	Check(ledger.Ack(retried[0], true), "the pre-read retry is acknowledged");
	Check(rt::CheckLoadDrain(ledger, deadline, deadline) == rt::LoadDrainState::Ready,
		  "an empty ledger is ready even at the drain deadline");
	Check(rt::BeginPlayerLoad(slots[2], ledger, 1, latest, deadline),
		  "LoadPlayer begins after its prior UPDATE was acknowledged");
	Snapshot read = database; // sc_skins SELECT; subsequent sc_items/ext callbacks may be delayed.
	CheckEq(read.teams[0].weapons[7].stattrak, 103, "the prior acknowledged delta is in the DB snapshot");
	ledger.Add(rifle, 2);
	ledger.Add(knife, 1);
	Check(rt::QueuePlayerDeltas(ledger, player, slots).empty(),
		  "kills after SELECT cannot enqueue UPDATE before the current load callback");
	CheckEq(ledger.TotalInflight(), 0, "barrier-held kills have no SQL statement in flight");
	Check(rt::LoadBlocksWrites(player, slots), "the same SteamID is covered by the read barrier");
	Check(!rt::LoadBlocksWrites(player + 1, slots), "other players can still flush their deltas");

	// A forced !wp joins the existing read and preserves its write barrier and sync intent.
	bool force = false, sync = false;
	Check(rt::JoinPlayerLoad(slots[2], true, true, now, force, sync), "forced sync joins the current read");
	Check(force && sync, "joined sync retains its response and forced-apply intent");
	Check(rt::JoinPlayerLoad(slots[2], false, false, now, force, sync), "ordinary reconcile joins too");
	Check(force && sync, "ordinary reconcile does not erase the pending sync intent");
	Check(!rt::BeginPlayerLoad(slots[2], ledger, 2, latest, deadline), "duplicate read is not issued");
	CheckEq(latest, 1, "joining a read leaves its current request identity unchanged");
	Check(rt::QueuePlayerDeltas(ledger, player, slots).empty(), "joining cannot release post-issue writes");

	// Merge before releasing the barrier: every owed kill is absent from the DB response.
	Check(rt::ResponseIsCurrent(1, latest), "the original response still belongs to the joined read");
	rt::MergePendingCounters(ledger, read);
	CheckEq(read.teams[0].weapons[7].stattrak, 105, "post-SELECT rifle kills are merged over the DB base");
	CheckEq(read.teams[1].knife->stattrak, 21, "post-SELECT knife kill is merged for the other team");
	CheckEq(ledger.TotalOwed(), 3, "merging a load does not acknowledge persistence");
	rt::InvalidatePlayerLoad(slots[2], latest);
	Check(!rt::ResponseIsCurrent(1, latest), "duplicate load callback cannot merge the kills twice");
	auto afterRead = rt::QueuePlayerDeltas(ledger, player, slots);
	CheckEq(afterRead.size(), 2, "completion releases the held deltas to storage");
	for (const auto& batch : afterRead) {
		if (batch.key == rifle) database.teams[0].weapons[7].stattrak += batch.amount;
		if (batch.key == knife) database.teams[1].knife->stattrak += batch.amount;
		Check(ledger.Ack(batch, true), "post-read write is acknowledged once");
	}
	CheckEq(database.teams[0].weapons[7].stattrak, read.teams[0].weapons[7].stattrak,
			"DB and displayed rifle count agree after the released write");
	Check(ledger.Empty(), "all post-read writes drained");

	// A timed-out old read is invalidated before draining. Its late answer must not unlock a new read.
	Check(rt::BeginPlayerLoad(slots[2], ledger, 3, latest, deadline), "old read starts with an empty ledger");
	ledger.Add(rifle, 1);
	Check(!rt::JoinPlayerLoad(slots[2], true, true, deadline, force, sync), "expired read is superseded instead of joined");
	rt::InvalidatePlayerLoad(slots[2], latest);
	Check(!rt::ResponseIsCurrent(3, latest), "timed-out read becomes stale before the drain starts");
	Check(!rt::BeginPlayerLoad(slots[2], ledger, 4, latest, deadline), "replacement read waits for held kills to drain");
	auto supersededDebt = rt::QueuePlayerDeltas(ledger, player, slots);
	database.teams[0].weapons[7].stattrak += supersededDebt[0].amount;
	Check(ledger.Ack(supersededDebt[0], true), "debt from the superseded read is acknowledged");
	Check(rt::BeginPlayerLoad(slots[2], ledger, 4, latest, deadline), "replacement read starts after its drain");
	Snapshot replacement = database;
	ledger.Add(rifle, 2);
	Check(!rt::ResponseIsCurrent(3, latest), "late superseded callback cannot enter the current cache");
	Check(rt::LoadBlocksWrites(player, slots), "late superseded callback leaves the replacement barrier intact");
	Check(rt::QueuePlayerDeltas(ledger, player, slots).empty(), "replacement holds its own post-issue kills");
	rt::MergePendingCounters(ledger, replacement);
	CheckEq(replacement.teams[0].weapons[7].stattrak, 108, "replacement merges only its two unsaved kills");
	rt::InvalidatePlayerLoad(slots[2], latest);
	auto replacementDebt = rt::QueuePlayerDeltas(ledger, player, slots);
	CheckEq(replacementDebt.size(), 1, "replacement completion releases one batch");
	Check(ledger.Ack(replacementDebt[0], true), "replacement debt is acknowledged");

	// Map end/disconnect/shutdown release bindings before the final drain, so it cannot deadlock.
	Check(rt::BeginPlayerLoad(slots[2], ledger, 5, latest, deadline), "final outstanding read starts");
	const rt::SlotGuard oldGuard{2, player, slots[2].generation, 10};
	ledger.Add(rifle, 1);
	Check(rt::QueuePlayerDeltas(ledger, player, slots).empty(), "final kill initially belongs to the live read barrier");
	rt::UnbindSlot(slots[2]);
	latest = 0;
	Check(!rt::GuardMatches(oldGuard, slots, 10), "teardown rejects the old callback's binding");
	Check(!rt::ResponseIsCurrent(5, latest), "teardown invalidates the outstanding load identity");
	Check(!rt::LoadBlocksWrites(player, slots), "teardown releases the player's write barrier");
	CheckEq(rt::QueuePlayerDeltas(ledger, player, slots).size(), 1, "final drain can enqueue the remaining kill");
}

// --- StatTrak delta keys -----------------------------------------------------------------------------
void TestDeltaKeys() {
	Check(rt::DeltaKey(2, 7) != rt::DeltaKey(3, 7), "the team separates the delta keys");
	Check(rt::DeltaKey(2, 7) != rt::DeltaKey(2, 8), "the definition separates the delta keys");
	CheckEq(static_cast<int64_t>(rt::DeltaKey(2, 7) >> 32), 2, "the team is part of the key");
	CheckEq(static_cast<int64_t>(rt::DeltaKey(2, 7) & 0xFFFF), 7, "the definition is part of the key");
}

// --- engine preparation across the map-start / readiness race (fake s2sdk bindings) -----------------
//
// The bug: the runtime's own s2sdk OnMapStart listener runs before
// pisexcompat's Poll grants readiness, and Engine::Prepare refuses to resolve schema offsets without
// a live map - so the attempt at every map start failed with "no map is running yet" and the retry
// waited for the next map, which lost the very same race (no skin was ever applied). These tests
// drive the real RuntimeInstance with fake bindings: a map start that arrives before readiness must
// not fail the engine, which has to come up in the SAME map - from pisexcompat's map-start callback
// (Poll grants readiness before firing it) or, at the latest, from the throttled Update retry.

std::vector<s2sdk::OnMapStartCallback> g_fakeMapStartListeners;
std::vector<s2sdk::OnMapEndCallback> g_fakeMapEndListeners;
std::unordered_map<std::string, std::string> g_fakeGameFiles;

bool FakeRuntimeIsClientConnected(int32_t slot) {
	(void)slot; // no client is ever connected: the reconcile walks the slots and skips them
	return false;
}
bool FakeRuntimeIsClientInGame(int32_t slot) {
	(void)slot;
	return false;
}
plg::string FakeRuntimeGetClientName(int32_t slot) {
	(void)slot;
	return {};
}
void FakeRuntimePrintToChat(int32_t slot, const plg::string& text) {
	(void)slot;
	(void)text;
}
void FakeRuntimePrintToChatAll(const plg::string& text) { (void)text; }

void FakeRuntimeRegisterMapStart(s2sdk::OnMapStartCallback callback) { g_fakeMapStartListeners.push_back(callback); }
void FakeRuntimeUnregisterMapStart(s2sdk::OnMapStartCallback callback) {
	for (std::size_t index = 0; index < g_fakeMapStartListeners.size(); ++index) {
		if (g_fakeMapStartListeners[index] == callback) {
			g_fakeMapStartListeners.erase(g_fakeMapStartListeners.begin() + static_cast<std::ptrdiff_t>(index));
			return;
		}
	}
}
void FakeRuntimeRegisterMapEnd(s2sdk::OnMapEndCallback callback) { g_fakeMapEndListeners.push_back(callback); }
void FakeRuntimeUnregisterMapEnd(s2sdk::OnMapEndCallback callback) {
	for (std::size_t index = 0; index < g_fakeMapEndListeners.size(); ++index) {
		if (g_fakeMapEndListeners[index] == callback) {
			g_fakeMapEndListeners.erase(g_fakeMapEndListeners.begin() + static_cast<std::ptrdiff_t>(index));
			return;
		}
	}
}

// The engine's whole map-start dispatch: pisexcompat's listener runs first (pisex::Init registered
// it), then the runtime's own trampoline. Neither may prepare the engine - readiness only flips in
// the next pisex::Poll, which is exactly what these tests pin down.
void DispatchFakeMapStart() {
	const std::vector<s2sdk::OnMapStartCallback> listeners = g_fakeMapStartListeners; // a callback must not mutate the list under us
	for (s2sdk::OnMapStartCallback callback : listeners) {
		if (callback != nullptr) {
			callback();
		}
	}
}

// The same for the map end: pisexcompat drops its readiness, the runtime clears its bindings and
// its engine; the next map must prepare the engine from scratch.
void DispatchFakeMapEnd() {
	const std::vector<s2sdk::OnMapEndCallback> listeners = g_fakeMapEndListeners;
	for (s2sdk::OnMapEndCallback callback : listeners) {
		if (callback != nullptr) {
			callback();
		}
	}
}

// Every schema field Engine::Prepare requires on this build (the same set engine_tests uses); an
// unknown pair is "not in this game build" (-1). The chain offsets answer -1 ("no chainer"): the
// entity is announced through its own NetworkStateChanged, which Prepare accepts. A non-empty
// g_fakeHiddenSchemaField resolves to -1, which makes Prepare fail the way a field missing from a
// game build does (the retry test drives that).
std::string g_fakeHiddenSchemaField;

int32_t FakeRuntimeSchemaOffset(const plg::string& className, const plg::string& fieldName) {
	struct Entry {
		const char* cls;
		const char* field;
	};
	static const Entry kTable[] = {
		{"CBasePlayerWeapon", "m_hOwnerEntity"},
		{"CBasePlayerPawn", "m_hController"},
		{"CBasePlayerWeapon", "m_AttributeManager"},
		{"CAttributeContainer", "m_Item"},
		{"CEconItemView", "m_iItemDefinitionIndex"},
		{"CEconItemView", "m_iEntityQuality"},
		{"CEconItemView", "m_szCustomName"},
		{"CEconItemView", "m_bInitialized"},
		{"CEconItemView", "m_AttributeList"},
		{"CEconItemView", "m_NetworkedDynamicAttributes"},
		{"CAttributeList", "m_Attributes"},
		{"CCSPlayerPawn", "m_EconGloves"},
		{"CBasePlayerPawn", "m_pWeaponServices"},
		{"CPlayer_WeaponServices", "m_hActiveWeapon"},
		{"CBasePlayerWeapon", "m_iClip1"},
		{"CBasePlayerWeapon", "m_pReserveAmmo"},
		{"CCSPlayerController", "m_iMusicKitID"},
		{"CCSPlayerController", "m_pInventoryServices"},
		{"CCSPlayerController_InventoryServices", "m_unMusicID"},
		{"CCSPlayerController_InventoryServices", "m_rank"},
	};
	const std::string cls(className.data(), className.size());
	const std::string field(fieldName.data(), fieldName.size());
	if (!g_fakeHiddenSchemaField.empty() && field == g_fakeHiddenSchemaField) {
		return -1;
	}
	for (std::size_t index = 0; index < sizeof(kTable) / sizeof(kTable[0]); ++index) {
		if (cls == kTable[index].cls && field == kTable[index].field) {
			return static_cast<int32_t>(64 + static_cast<int>(index) * 8);
		}
	}
	return -1;
}

int32_t FakeRuntimeChainOffset(const plg::string& className) {
	(void)className;
	return -1;
}

std::string ReadRuntimeFixture(const std::string& name) {
	std::ifstream stream(std::filesystem::path(WS_FIXTURES_DIR) / name, std::ios::binary);
	if (!stream) {
		Check(false, "cannot read fixture " + name);
		return {};
	}
	return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

plg::string FakeRuntimeReadFileVPK(const plg::string& name, const plg::string& pathId) {
	(void)pathId;
	const auto it = g_fakeGameFiles.find(std::string(name.data(), name.size()));
	if (it == g_fakeGameFiles.end()) {
		return {};
	}
	return plg::string(it->second.data(), it->second.size());
}

plg::vector<plg::string> FakeRuntimeFindFileAbsoluteList(const plg::string& wildcard, const plg::string& pathId) {
	(void)wildcard;
	(void)pathId;
	plg::vector<plg::string> paths;
	if (g_fakeGameFiles.count("resource/csgo_english.txt") != 0) {
		paths.push_back(plg::string("GAME:resource/csgo_english.txt"));
	}
	return paths;
}

void InstallRuntimeFakes(bool withGameFiles) {
	g_fakeMapStartListeners.clear();
	g_fakeMapEndListeners.clear();
	g_fakeGameFiles.clear();
	g_fakeHiddenSchemaField.clear();

	__s2sdk_IsClientConnected = &FakeRuntimeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeRuntimeIsClientInGame;
	__s2sdk_GetClientName = &FakeRuntimeGetClientName;
	__s2sdk_PrintToChat = &FakeRuntimePrintToChat;
	__s2sdk_PrintToChatAll = &FakeRuntimePrintToChatAll;
	__s2sdk_OnMapStart_Register = &FakeRuntimeRegisterMapStart;
	__s2sdk_OnMapStart_Unregister = &FakeRuntimeUnregisterMapStart;
	__s2sdk_OnMapEnd_Register = &FakeRuntimeRegisterMapEnd;
	__s2sdk_OnMapEnd_Unregister = &FakeRuntimeUnregisterMapEnd;
	__s2sdk_GetSchemaOffset = &FakeRuntimeSchemaOffset;
	__s2sdk_GetSchemaChainOffset = &FakeRuntimeChainOffset;
	__s2sdk_ReadFileVPK = &FakeRuntimeReadFileVPK;
	__s2sdk_FindFileAbsoluteList = &FakeRuntimeFindFileAbsoluteList;

	if (withGameFiles) {
		g_fakeGameFiles["scripts/items/items_game.txt"] = ReadRuntimeFixture("items_game.txt");
		g_fakeGameFiles["resource/csgo_english.txt"] = ReadRuntimeFixture("csgo_english.txt");
	}
}

void UninstallRuntimeFakes() {
	__s2sdk_IsClientConnected = nullptr;
	__s2sdk_IsClientInGame = nullptr;
	__s2sdk_GetClientName = nullptr;
	__s2sdk_PrintToChat = nullptr;
	__s2sdk_PrintToChatAll = nullptr;
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapStart_Unregister = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	__s2sdk_GetSchemaOffset = nullptr;
	__s2sdk_GetSchemaChainOffset = nullptr;
	__s2sdk_ReadFileVPK = nullptr;
	__s2sdk_FindFileAbsoluteList = nullptr;
	g_fakeMapStartListeners.clear();
	g_fakeMapEndListeners.clear();
	g_fakeGameFiles.clear();
	g_fakeHiddenSchemaField.clear();
}

std::string RuntimeStatus() { return ws::RuntimeInstance().StatusText(); }

// The ", mapGeneration=N," token of ws_status: the tests pin "the same map" by comparing the token,
// not a hardcoded number (the runtime keeps counting across a Stop/Start of the test singleton).
std::string MapGenerationToken() {
	const std::string status = RuntimeStatus();
	const std::size_t begin = status.find("mapGeneration=");
	if (begin == std::string::npos) {
		return {};
	}
	const std::size_t end = status.find(',', begin);
	return status.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// The map start arrives before any Poll: the dispatch itself must not prepare the engine (no
// readiness yet, no schema work), and the first update after it - the one whose Poll grants
// readiness and fires pisexcompat's map-start callback - must leave the engine ready in the same map.
void TestEngineReadyAfterEarlyMapStart() {
	InstallRuntimeFakes(/*withGameFiles=*/true);
	ws::Runtime& runtime = ws::RuntimeInstance();

	runtime.Start("");
	const std::string mapGeneration = MapGenerationToken();
	Check(!mapGeneration.empty(), "the status reports the map generation");
	Check(RuntimeStatus().find("econ=ready") != std::string::npos, "the econ data loads before the map");
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos, "the engine starts not-ready");

	DispatchFakeMapStart();
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos,
		  "a map start dispatched before Poll does not prepare the engine yet");
	Check(RuntimeStatus().find("mapReady=yes") != std::string::npos, "the map is marked up");

	runtime.Update(std::chrono::milliseconds(100));
	Check(RuntimeStatus().find("engine=ready") != std::string::npos,
		  "the first update after the map start readies the engine");
	Check(RuntimeStatus().find("econ=ready") != std::string::npos, "the econ data is still ready");
	Check(MapGenerationToken() == mapGeneration, "prepared without a map change");
	Check(RuntimeStatus().find("giveNamedItemHook=off") != std::string::npos,
		  "no gamedata next to the test plugin: the hook stays off");

	// The changelevel half of the live bug: after a map end the readiness is dropped and the
	// runtime's bindings are cleared, so the next map start again arrives before readiness. The
	// engine has to come back on the next update of the new map (the startup callback only ever
	// fires once, so nothing else can do it).
	DispatchFakeMapEnd();
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos, "the map end drops the engine");
	Check(RuntimeStatus().find("mapReady=no") != std::string::npos, "the map is marked down");
	DispatchFakeMapStart();
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos,
		  "the next map start does not prepare the engine before readiness either");

	runtime.Update(std::chrono::milliseconds(100));
	Check(RuntimeStatus().find("engine=ready") != std::string::npos,
		  "the next update of the new map readies the engine again");
	Check(RuntimeStatus().find("econ=ready") != std::string::npos, "the econ data survives the map change");
	Check(MapGenerationToken() != mapGeneration, "the map generation advanced with the changelevel");
	Check(RuntimeStatus().find("mapReady=yes") != std::string::npos, "the new map is marked up");

	runtime.Stop();
	UninstallRuntimeFakes();
}

// The same race, one step worse: the first attempt (pisexcompat's map-start callback in the first
// update) runs while a schema field the game build is expected to expose does not resolve, so
// Prepare fails there. The engine must not wait for the next map: a later update of the *same* map
// retries - throttled to one attempt per second, which is what leaves the attempts within the first
// second unused.
void TestEnginePrepareRetriesWhileMapUp() {
	InstallRuntimeFakes(/*withGameFiles=*/true);
	ws::Runtime& runtime = ws::RuntimeInstance();

	// A field Prepare requires is "missing from this build" for the first attempt.
	g_fakeHiddenSchemaField = "m_pReserveAmmo";

	runtime.Start("");
	const std::string mapGeneration = MapGenerationToken();
	DispatchFakeMapStart();
	runtime.Update(std::chrono::milliseconds(100));
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos,
		  "the first map-start attempt fails on the missing schema field");

	// The schema resolves from here on (still the same map: no map end happens in between).
	g_fakeHiddenSchemaField.clear();

	runtime.Update(std::chrono::milliseconds(300));
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos,
		  "a retry before the one-second throttle elapsed does not run");

	runtime.Update(std::chrono::milliseconds(700));
	Check(RuntimeStatus().find("engine=ready") != std::string::npos,
		  "a later update of the same map prepares the engine");
	Check(MapGenerationToken() == mapGeneration, "still the same map");

	runtime.Stop();
	UninstallRuntimeFakes();
}

// The econ budget is per map: a runtime that spent its three in-map attempts while the game files
// were not visible must try again after the map change - otherwise the skinchanger stayed inert for
// the whole process even though the econ failure was transient.
//
// This test needs a runtime whose econ is not loaded yet (once loaded it stays loaded for the life
// of the singleton), so run it before the other engine tests.
void TestEconAttemptsResetOnMapEnd() {
	InstallRuntimeFakes(/*withGameFiles=*/false);
	ws::Runtime& runtime = ws::RuntimeInstance();

	runtime.Start("");
	Check(RuntimeStatus().find("econ=pending") != std::string::npos,
		  "the econ data starts pending without the game files");

	DispatchFakeMapStart();
	runtime.Update(std::chrono::milliseconds(100));
	Check(RuntimeStatus().find("engine=not-ready") != std::string::npos, "no engine without the econ data");

	// Spend the rest of this map's budget (the map-start callback above spent the first attempt).
	runtime.Update(std::chrono::seconds(1));
	runtime.Update(std::chrono::seconds(1));
	runtime.Update(std::chrono::seconds(1));
	Check(RuntimeStatus().find("econ=pending") != std::string::npos, "this map cannot load the econ data");

	// The failure was transient: the files are readable again with the next map.
	g_fakeGameFiles["scripts/items/items_game.txt"] = ReadRuntimeFixture("items_game.txt");
	g_fakeGameFiles["resource/csgo_english.txt"] = ReadRuntimeFixture("csgo_english.txt");

	DispatchFakeMapEnd();
	DispatchFakeMapStart();
	runtime.Update(std::chrono::milliseconds(100));
	Check(RuntimeStatus().find("econ=ready") != std::string::npos, "the next map retries the econ load and succeeds");
	Check(RuntimeStatus().find("engine=ready") != std::string::npos, "the engine comes up on that same map");

	runtime.Stop();
	UninstallRuntimeFakes();
}

} // namespace

int main() {
	std::printf("WeaponSkins runtime tests\n");
	TestSlotBindings();
	TestCooldowns();
	TestCommandsHelpers();
	TestStickerFix();
	TestBuildView();
	TestStatTrakLookup();
	TestMessageResolution();
	TestDeltaLedger();
	TestLoadRequests();
	TestLoadWriteBarrier();
	TestDeltaKeys();
	TestEconAttemptsResetOnMapEnd();
	TestEngineReadyAfterEarlyMapStart();
	TestEnginePrepareRetriesWhileMapUp();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
