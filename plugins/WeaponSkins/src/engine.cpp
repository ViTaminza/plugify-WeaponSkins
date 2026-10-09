// WeaponSkins engine — the GiveNamedItem detour and every item-application path.
//
// What this file is: the C++ replacement of the reference's
//   * WeaponSkins.Services/Natives/NativeService.cs        (the detour),
//   * WeaponSkins.Services/Inventory/HookInventoryUpdateService.cs (the apply logic),
//   * WeaponSkins.Extensions/PlayerExtensions.cs           (the regive logic).
// The reference is the original SwiftlyS2 plugin (ref/src/WeaponSkins); the finished C# port
// (plugins_dotnet/WeaponSkins) is the same code over a Plugify shim, and its raw behaviour
// (widths, attribute names, ordering) was re-checked against the saved schema dump
// (backup-27035-20261007-224244/addons/swiftlys2/gamedata/cs2/sdk.json).
//
// The hot path is the GiveNamedItem post hook. It runs on every weapon creation (spawn, buy,
// pickup, round start) *inside* the engine's item creation, so it obeys PITFALLS.md §I:
//   * no SQL, no malloc, no string formatting, no schema offset resolution, no DeepClone: every
//     offset is resolved once per map by Prepare(), every attribute name is a compile-time
//     literal, and the only data touched is the read-only Snapshot the runtime hands in;
//   * no Give/Remove/NextFrame: the hook only writes the item's econ fields, calls the native
//     CAttributeList::SetOrAddAttributeValueByName, and issues the two cosmetic entity inputs the
//     reference issues there (SetBodygroup / ChangeSubclass);
//   * the owner is validated in the same callback (the FIRST argument, CCSPlayer_ItemServices* ->
//     its component chainer's pawn -> m_hController -> controller -> slot; the weapon's own
//     m_hOwnerEntity is only the fallback, it is not assigned yet when the post hook runs), then
//     the map/bot/slot guards, then the steamId match against the cache entry - a stale entry for
//     a reused slot can never be applied;
//   * every entity pointer is resolved at entry and dropped before the call returns; nothing is
//     cached across ticks. The one *stored* pointer is a code address (the native attribute
//     writer), not an entity pointer.
//
// Why the two entity inputs are immediate and not deferred: the C# port's AcceptInputAsync is
// synchronous, and both inputs only mutate the weapon the engine has just created (no entity
// creation/destruction, no inventory restructuring) - the same stack the reference ran them on.
// Their strings are short enough for plg::string's SSO buffer ("SetBodygroup" 13 chars, "body,0/1"
// 6, "ChangeSubclass" 14, and the subclass values come from a compile-time table), so the calls do
// not allocate. Everything that *does* restructure the inventory (give/remove/select) lives in the
// refresh paths only, and even there it is deferred to the next frame - never the hook.
//
// Deferred work (the glove bodygroup's second phase at +0.2 s, the spawn re-apply at +0.1 s, the
// agent model set on the next frame - all reference behaviour) goes through pisexcompat's timers,
// pumped by pisex::Poll() on the game thread and postponed while no map is up. Two rules make it
// safe:
//   * Lifetime: no deferred callback captures a raw `this` that it dereferences. Each captures a
//     weak_ptr to a token owned by the Impl; the token lock plus a generation counter is checked
//     first, so a callback that outlives the engine (or one that raced a Shutdown) is a no-op.
//   * Identity: every callback captures a DeferredGuard (slot, steamId, current pawn HANDLE, team,
//     generation) and re-checks all of it when it fires - a respawn (new pawn), a reconnect or slot
//     reuse (different steamId), a team switch, a map change or an engine shutdown cancels the
//     captured work. Pointers are always resolved anew inside the callback.
// Timer bookkeeping keeps only *pending* one-shot timers, each under a stable id; the record is
// dropped from the tracking inside the callback (before it returns -1, i.e. before pisexcompat
// stops and frees the timer), so a recorded CTimer* is never dereferenced after the object is gone.
// Shutdown() therefore only stops timers that are still pending, i.e. still alive in the registry.
//
// Removals on the website. A site that drops a row must not leave the old skin in the game, so the
// engine remembers what it applied per player life (AppliedState): the !wp pass regives the guns it
// skinned before and the held knife (a fresh copy comes back with the game's default attributes), and
// a glove it set is reset by clearing the item (definition 0 + m_bInitialized false), which makes the
// game rebuild the player's default gloves. Nothing this engine never wrote is touched, and the
// regiving itself happens in the deferred pass, never on the command frame.
//
// Call order (owned by the runtime): Engine(config, econ) -> Prepare(error) with a map up ->
// InstallHook(gamedataPath, cacheLookup, error) -> ApplySpawn/RefreshInventory/SetStatTrak/
// ActiveDefinition during play -> Shutdown() (or ~Engine) at plugin end or reload.

#include "engine.hpp"

#include "engine_detail.hpp"

#include <pisex/compat.hpp>
#include <pisex/players.hpp>
#include <pisex/timers.hpp>
#include <plg/plugin.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws {

namespace {

using detail::IntAttributeValue;
using detail::IntBitsAsAttribute;
using detail::IsAttributeHeaderPlausible;
using detail::IsPageMapped;
using detail::kAttrKillEater;
using detail::kAttrKillEaterScoreType;
using detail::kAttrTexturePrefab;
using detail::kAttrTextureSeed;
using detail::kAttrTextureWear;
using detail::kAttributeCountOffset;
using detail::kAttributeCountWidth; // used by the clear helper below
using detail::kKeychainNames;
using detail::kRankPinIndex;
using detail::kReserveAmmoElements;
using detail::kStickerNames;
using detail::kStickerSchemaNone;
using detail::kWidthCustomName;
using detail::kWidthRank;
using detail::Offsets;
using detail::ReadMem;
using detail::StatTrakValue;
using detail::WriteFixedString;
using detail::WriteMem;

// The gamedata keys of assets/gamedata.jsonc. Both signatures were verified unique in the game
// binary; nothing here hardcodes an address - they are resolved by name through the SDK.
constexpr char kSignatureGiveNamedItem[] = "GiveNamedItem";
constexpr char kSignatureSetAttribute[] = "CAttributeList::SetOrAddAttributeValueByName";

// void CAttributeList::SetOrAddAttributeValueByName(CAttributeList* this, const char* name,
//                                                   float value)
// The one native call of this plugin: the same function SwiftlyS2's GameFunctions.pSetOrAddAttribute
// exposed and the C# port called (Core/Native.cs:90). It is what the clients read the appearance
// from, and the only writer these attributes have (PITFALLS.md §C.7: the typed s2sdk setters refuse
// CS2BadList fields, and CAttributeList has no other public writer).
using SetAttributeFn = void (*)(void* list, const char* name, float value);

// The GiveNamedItem ABI: the function takes SIX 64-bit arguments and returns the new entity
// (the reference's delegate, NativeService.cs:23-28: nint(this, name, subtype, econItemView, a5,
// a6)):
//   CBaseEntity* CCSPlayer_ItemServices::GiveNamedItem(this, const char* name, nint subtype,
//                                                      CEconItemView* pScriptItem, nint a5, nint a6)
// rdi = this, rsi = name, rdx = subtype, rcx = pScriptItem, r8 = a5, r9 = a6 (an origin/vector
// pointer), return in rax. EVERY argument slot must be declared - polyhook's JIT forwards only the
// declared arguments to the original, so an undeclared 6th (or a 32-bit declaration of a 64-bit
// slot) hands the engine garbage: live, the detour once declared 5 arguments and every weapon
// spawned at a garbage origin ("m_cellX cell -2097152 is outside of cell bounds"). The post
// callback reads argument 0 (`this`, the CCSPlayer_ItemServices* the item is given through - the
// player the item goes to, already known at post time) and the return value. polyhook keys one
// detour per target address and shares it between plugins, so this declaration must stay compatible
// with the reference's (see InstallHook/Shutdown teardown).
constexpr polyhook::DataType kGiveNamedItemReturn = polyhook::DataType::Pointer;

void AppendGiveNamedItemArguments(plg::vector<polyhook::DataType>& arguments) {
	// Index 0 is `this` for a member function - the polyhook argument convention of this port.
	arguments.push_back(polyhook::DataType::Pointer); // 0: this: CCSPlayer_ItemServices*
	arguments.push_back(polyhook::DataType::Pointer); // 1: const char* name
	arguments.push_back(polyhook::DataType::Int64);	  // 2: nint subtype
	arguments.push_back(polyhook::DataType::Pointer); // 3: CEconItemView* pScriptItem
	arguments.push_back(polyhook::DataType::Int64);	  // 4: nint a5
	arguments.push_back(polyhook::DataType::Pointer); // 5: nint a6 (origin/vector pointer)
}

// Entity inputs of the reference (HookInventoryUpdateService.cs:479, :545, :601-604).
constexpr char kInputSetBodygroup[] = "SetBodygroup";
constexpr char kInputChangeSubclass[] = "ChangeSubclass";
constexpr char kBodygroupStandard[] = "body,0";
constexpr char kBodygroupLegacy[] = "body,1";
constexpr char kGloveBodygroupOff[] = "first_or_third_person,0";
constexpr char kGloveBodygroupOn[] = "first_or_third_person,1";

// Delays of the reference: DelayBySeconds(0.2f) for the glove bodygroup's second phase
// (HookInventoryUpdateService.cs:602) and DelayBySeconds(0.1f) for the spawn re-apply (:123).
constexpr float kGlovePhaseDelaySeconds = 0.2f;
constexpr float kSpawnReapplyDelaySeconds = 0.1f;

constexpr char kKnifeClassname[] = "weapon_knife"; // RegiveKnife of PlayerExtensions.cs:56
constexpr char kTaserClassname[] = "weapon_taser"; // RegiveWeapon's special case (:23)

// A resolved weapon: the entity plus the offsets of its inline CEconItemView. Valid only inside
// the call that built it (the item view is inline memory of the weapon, so the pointer moves with
// the entity - nothing may be cached).
struct WeaponView {
	void* weapon = nullptr;
	void* attributeManager = nullptr;
	void* item = nullptr;
	int itemOffset = 0;		  // item view offset relative to the weapon (for notifications)
	int32_t weaponHandle = -1; // entity handle for the entity inputs, -1 when unknown
};

std::string ToString(const plg::string& text) { return std::string(text.data(), text.size()); }

// True when the pointer is a live entity: s2sdk's own check (IsEntityPtr) when the binding is
// available, the page guard otherwise - never a bare null test on a value the engine handed us.
bool IsEntityPointerUsable(void* pointer) {
	if (pointer == nullptr) {
		return false;
	}
	if (__s2sdk_IsValidEntPointer != nullptr) {
		try {
			return s2sdk::IsValidEntPointer(pointer);
		} catch (...) {
			return false;
		}
	}
	return IsPageMapped(pointer);
}

// True when the value is a valid entity HANDLE (CEntityHandle). The bits are index (low 15) |
// serial << 15 and the game hands serials out from a global counter, so a few minutes into a
// server's life the serial passes 65535 and bit 31 is set: valid handles are then NEGATIVE ints
// (live: a pawn handle of -921534371 = 0xC912805D = index 93, serial 102949, and it is valid).
// Only INVALID_EHANDLE_INDEX = -1 is invalid (CEntityHandle::IsValid,
// ref/s2sdk-src/src/export/entities.cpp), and 0 (index 0, serial 0 = the world) is never handed
// out - the resolution helpers here return 0 or -1 for "none" instead. The sign means nothing:
// never compare a handle against 0.
inline bool IsValidHandle(int32_t handle) { return handle != -1 && handle != 0; }

void* PointerFromHandle(int32_t handle) {
	if (!IsValidHandle(handle) || __s2sdk_EntHandleToEntPointer == nullptr) {
		return nullptr;
	}
	try {
		return s2sdk::EntHandleToEntPointer(handle);
	} catch (...) {
		return nullptr;
	}
}

int32_t HandleFromPointer(void* pointer) {
	if (pointer == nullptr || __s2sdk_EntPointerToEntHandle == nullptr) {
		return -1;
	}
	try {
		return s2sdk::EntPointerToEntHandle(pointer);
	} catch (...) {
		return -1;
	}
}

// The chain offset of a class, or -1 for "notify the entity directly". s2sdk's
// SafeNetworkStateChanged treats >= 0 as "the chainer sits at object + offset", -1 as the entity
// itself and -2 as an inline component's own NetworkStateChanged; a missing binding or an offset
// of 0 must both become -1 (0 would be read as a real offset and dereference the object's vtable).
int ChainOffsetOrNone(const char* className) {
	if (__s2sdk_GetSchemaChainOffset == nullptr) {
		return -1;
	}
	try {
		const int32_t offset = s2sdk::GetSchemaChainOffset(plg::string(className));
		return offset > 0 ? static_cast<int>(offset) : -1;
	} catch (...) {
		return -1;
	}
}

} // namespace

// -------------------------------------------------------------------------------------------------
// The implementation.
// -------------------------------------------------------------------------------------------------
struct Engine::Impl {
	// The active instance for the polyhook trampoline. polyhook callbacks carry no user pointer, so
	// exactly one engine may own the detour (InstallHook refuses a second one); the static member is
	// how the static callback reaches it without exposing Impl.
	inline static Impl* s_active = nullptr;

	struct SlotState {
		uint64_t steamId = 0;
		std::array<std::string, 2> defaultModel{}; // per engine team (2 -> 0, 3 -> 1)
	};

	// The lifetime token of the engine: owned here, handed to every deferred callback as a
	// weak_ptr. A callback that fires after the engine is gone (the runtime may reset it while a
	// 0.1-0.2 s timer is still pending) fails to lock it and does nothing - no deferred callback
	// captures a raw `this` that it dereferences before that check. `generation` is bumped by
	// Shutdown, so a callback that fires after a teardown of a still-living engine is a no-op too.
	struct EngineToken {
		uint64_t generation = 0;
	};

	// The identity of one deferred pass: every field is captured by copy and re-checked when the
	// callback fires. A respawn (new pawn handle), a reconnect or slot reuse (different steamId), a
	// team switch, a map change or an engine shutdown cancels the captured work.
	struct DeferredGuard {
		int slot = -1;
		uint64_t steamId = 0;
		int32_t pawnHandle = -1;
		int team = 0;
		uint64_t generation = 0;
	};

	// A pending one-shot timer. Only timers that have not fired yet are tracked; the record is
	// removed inside the callback, before it returns -1 (pisexcompat then stops the timer and frees
	// its CTimer at the next Poll), so a recorded pointer is never used after the object is gone.
	struct PendingTimer {
		uint64_t id = 0;
		pisex::CTimer* timer = nullptr;
	};

	// Deferred passes are one per spawn/refresh event (0.1-0.2 s apart); the cap only keeps a burst
	// of events from growing the bookkeeping without bound. A dropped pass is a skipped cosmetic
	// re-apply, never a correctness problem: the immediate pass already ran.
	static constexpr std::size_t kMaxPendingTimers = 128;

	// What this engine has written for one player in the current life. It is what makes a *removal*
	// on the site recoverable: !wp regives the weapons this engine skinned before (their fresh copy
	// comes back with default attributes) and resets a glove this engine set, without ever touching
	// a weapon or glove it never wrote (another plugin's or the game's). Keyed by (steamId, pawn
	// handle): a respawn or a slot reuse drops the record, and the engine has already given the
	// player fresh default items at that point.
	struct AppliedState {
		uint64_t steamId = 0;
		int32_t pawnHandle = -1;
		// (engine team 2/3) << 16 | definition, for the gun skins this engine applied.
		std::array<uint32_t, 32> weapons{};
		std::size_t weaponCount = 0;
		std::array<bool, 2> knife{}; // per team index: our knife skin is on the held knife
		std::array<bool, 2> glove{}; // per team index: our glove skin is in m_EconGloves
	};

	Impl(const Config& config, const EconData& econ) : config_(config), econ_(econ) {}

	// --- setup -----------------------------------------------------------------------------------

	// Resolves every schema offset. Requires a live map (PITFALLS.md §C.8: GetSchemaOffset before a
	// map exists segfaults inside the schema system) and is safe to call again after a map change or
	// a game update - the offsets are simply re-resolved. A failure leaves the engine without
	// offsets, so no code path writes anything until it succeeds.
	bool Prepare(std::string& error) {
		error.clear();
		offsets_ = Offsets{};
		econGlovesChanged_ = -1;
		servicesPawn_ = -1;
		controllerPlayerPawn_ = -1;
		if (__s2sdk_GetSchemaOffset == nullptr) {
			error = "s2sdk GetSchemaOffset binding is missing";
			return false;
		}
		if (!pisex::IsGameReady()) {
			error = "no map is running yet - schema offsets can only be resolved with a live map";
			return false;
		}

		const auto offset = [](const char* className, const char* fieldName) -> int {
			try {
				return static_cast<int>(s2sdk::GetSchemaOffset(plg::string(className), plg::string(fieldName)));
			} catch (...) {
				return -1;
			}
		};

		Offsets resolved;
		resolved.weaponOwner = offset("CBasePlayerWeapon", "m_hOwnerEntity");
		resolved.pawnController = offset("CBasePlayerPawn", "m_hController");
		resolved.weaponAttributeManager = offset("CBasePlayerWeapon", "m_AttributeManager");
		resolved.containerItem = offset("CAttributeContainer", "m_Item");
		resolved.itemDefIndex = offset("CEconItemView", "m_iItemDefinitionIndex");
		resolved.itemQuality = offset("CEconItemView", "m_iEntityQuality");
		resolved.itemCustomName = offset("CEconItemView", "m_szCustomName");
		resolved.itemInitialized = offset("CEconItemView", "m_bInitialized");
		resolved.itemAttributeList = offset("CEconItemView", "m_AttributeList");
		resolved.itemNetworkedAttributes = offset("CEconItemView", "m_NetworkedDynamicAttributes");
		resolved.attributeVector = offset("CAttributeList", "m_Attributes");
		resolved.pawnEconGloves = offset("CCSPlayerPawn", "m_EconGloves");
		// The counter the client rebuilds the glove model from (m_nEconGlovesChanged, uint8 - sdk.json
		// size 1). Resolved like every other offset, but optional: it is not required because a build
		// without it only loses the client-side rebuild signal (today's leaf-write behaviour).
		const int econGlovesChanged = offset("CCSPlayerPawn", "m_nEconGlovesChanged");
		resolved.pawnWeaponServices = offset("CBasePlayerPawn", "m_pWeaponServices");
		resolved.weaponServicesActive = offset("CPlayer_WeaponServices", "m_hActiveWeapon");
		resolved.weaponClip = offset("CBasePlayerWeapon", "m_iClip1");
		resolved.weaponReserveAmmo = offset("CBasePlayerWeapon", "m_pReserveAmmo");
		resolved.taserFireTime = offset("CWeaponTaser", "m_fFireTime");
		resolved.taserLastAttackTick = offset("CWeaponTaser", "m_nLastAttackTick");
		resolved.controllerMusicKit = offset("CCSPlayerController", "m_iMusicKitID");
		resolved.controllerInventoryServices = offset("CCSPlayerController", "m_pInventoryServices");
		resolved.inventoryMusicId = offset("CCSPlayerController_InventoryServices", "m_unMusicID");
		if (resolved.inventoryMusicId < 0) {
			// Other game builds call the same field m_nMusicID (the C# port keeps the same fallback).
			resolved.inventoryMusicId = offset("CCSPlayerController_InventoryServices", "m_nMusicID");
		}
		resolved.inventoryRank = offset("CCSPlayerController_InventoryServices", "m_rank");

		// The two optional fields of the owner resolution, resolved like every other offset but not
		// required (the fallbacks below stay usable):
		//   * CPlayerPawnComponent's pawn pointer. This build's schema exposes it as the
		//     component's CNetworkVarChainer (sdk.json: __m_pChainEntity at offset 8, size 40) whose
		//     leading m_pEntity is the pawn that owns the component
		//     (ref/pisex/SchemaEntity/schemasystem.h:47-56) - the exact 8-byte read s2sdk's
		//     CPlayerPawnComponent::GetPawn() performs (ref/s2sdk-src/src/core/sdk/entity/services.h).
		//     The chainer is asked first: build 2000930 (GameTracking-CS2 DumpSource2/schemas/server/
		//     CPlayerPawnComponent.h) has only __m_pChainEntity, and every failed lookup makes s2sdk
		//     print "'m_pPawn' was not found in 'CPlayerPawnComponent'!". A build that names the
		//     pointer directly (m_pPawn) is the fallback.
		//   * CCSPlayerController::m_hPlayerPawn (CHandle<CCSPlayerPawn>, int32, sdk.json offset
		//     2996) - the pawn handle the glove/agent paths read through PawnHandle.
		const int servicesChainer = offset("CPlayerPawnComponent", "__m_pChainEntity");
		const int servicesChain =
			servicesChainer >= 0 ? servicesChainer : offset("CPlayerPawnComponent", "m_pPawn");
		const int controllerPawnHandle = offset("CCSPlayerController", "m_hPlayerPawn");

		resolved.weaponChain = ChainOffsetOrNone("CBasePlayerWeapon");
		resolved.pawnChain = ChainOffsetOrNone("CCSPlayerPawn");
		resolved.controllerChain = ChainOffsetOrNone("CCSPlayerController");
		// The inventory services object is a component, not an entity: without a chainer of its own,
		// its fields are announced through the object's own NetworkStateChanged (vtable slot 1),
		// which s2sdk spells chainOffset = -2.
		const int inventoryChain = ChainOffsetOrNone("CCSPlayerController_InventoryServices");
		resolved.inventoryChain = inventoryChain > 0 ? inventoryChain : -2;

		std::string missing;
		const auto require = [&missing](int value, const char* label) {
			if (value < 0) {
				if (!missing.empty()) {
					missing += ", ";
				}
				missing += label;
			}
		};
		require(resolved.weaponOwner, "CBasePlayerWeapon::m_hOwnerEntity");
		require(resolved.pawnController, "CBasePlayerPawn::m_hController");
		require(resolved.weaponAttributeManager, "CBasePlayerWeapon::m_AttributeManager");
		require(resolved.containerItem, "CAttributeContainer::m_Item");
		require(resolved.itemDefIndex, "CEconItemView::m_iItemDefinitionIndex");
		require(resolved.itemQuality, "CEconItemView::m_iEntityQuality");
		require(resolved.itemCustomName, "CEconItemView::m_szCustomName");
		require(resolved.itemInitialized, "CEconItemView::m_bInitialized");
		require(resolved.itemAttributeList, "CEconItemView::m_AttributeList");
		require(resolved.itemNetworkedAttributes, "CEconItemView::m_NetworkedDynamicAttributes");
		require(resolved.attributeVector, "CAttributeList::m_Attributes");
		require(resolved.pawnEconGloves, "CCSPlayerPawn::m_EconGloves");
		require(resolved.pawnWeaponServices, "CBasePlayerPawn::m_pWeaponServices");
		require(resolved.weaponServicesActive, "CPlayer_WeaponServices::m_hActiveWeapon");
		require(resolved.weaponClip, "CBasePlayerWeapon::m_iClip1");
		require(resolved.weaponReserveAmmo, "CBasePlayerWeapon::m_pReserveAmmo");
		require(resolved.controllerMusicKit, "CCSPlayerController::m_iMusicKitID");
		require(resolved.controllerInventoryServices, "CCSPlayerController::m_pInventoryServices");
		require(resolved.inventoryMusicId, "CCSPlayerController_InventoryServices::m_unMusicID");
		require(resolved.inventoryRank, "CCSPlayerController_InventoryServices::m_rank");
		// m_fFireTime / m_nLastAttackTick stay optional: a build without them only loses the taser's
		// attack-timing restore.
		if (!missing.empty()) {
			error = "schema fields not found in this game build: " + missing;
			return false;
		}

		resolved.ready = true;
		offsets_ = resolved;
		econGlovesChanged_ = econGlovesChanged;
		servicesPawn_ = servicesChain;
		controllerPlayerPawn_ = controllerPawnHandle;
		if ((servicesPawn_ < 0 || controllerPlayerPawn_ < 0) && !optionalOffsetsLogged_) {
			// Logged once per engine, not per Prepare: both fields are optional, and without them
			// the hook's owner resolution / the pawn handle silently degrade to the fallbacks.
			optionalOffsetsLogged_ = true;
			std::string absent;
			if (servicesPawn_ < 0) {
				absent = "CPlayerPawnComponent::__m_pChainEntity";
			}
			if (controllerPlayerPawn_ < 0) {
				if (!absent.empty()) {
					absent += ", ";
				}
				absent += "CCSPlayerController::m_hPlayerPawn";
			}
			if (plg::Log) {
				plg::Log("WeaponSkins: schema fields not found in this game build: " + absent +
							 " - the GiveNamedItem owner resolution / the pawn handles fall back to the "
							 "weapon-owner chain and the pointer conversion",
						 plg::Severity::Warning, plg::source_location::current());
			}
		}
		if (econGlovesChanged_ < 0 && !econGlovesChangedLogged_) {
			// Logged once per engine, not per Prepare: the field is optional, so the engine stays
			// fully usable without it (glove items are still written, only the rebuild signal is lost).
			econGlovesChangedLogged_ = true;
			if (plg::Log) {
				plg::Log("WeaponSkins: CCSPlayerPawn::m_nEconGlovesChanged not found in this game build - "
						 "the client may not rebuild the glove model",
						 plg::Severity::Warning, plg::source_location::current());
			}
		}
		return true;
	}

	// Loads the plugin's own gamedata, resolves the two signatures and installs the detour.
	// Failures are soft: the caller logs the error and the plugin keeps running without the hook
	// (PITFALLS.md §A.1 - returning Failed from OnPluginStart would stop every plugin).
	bool InstallHook(const std::filesystem::path& gamedata, CacheLookup lookup, std::string& error) {
		error.clear();
		if (hookInstalled_) {
			return true;
		}
		if (!lookup) {
			error = "the weapon cache lookup is empty - the hook would have nothing to apply";
			return false;
		}
		if (__s2sdk_LoadGameConfigFile == nullptr || __s2sdk_GetGameConfigSignature == nullptr) {
			error = "s2sdk gamedata bindings are missing";
			return false;
		}
		if (__polyhook_HookDetour == nullptr || __polyhook_AddCallback == nullptr) {
			error = "polyhook bindings are missing (is the polyhook plugin installed?)";
			return false;
		}
		if (s_active != nullptr && s_active != this) {
			error = "another WeaponSkins engine already owns the GiveNamedItem detour";
			return false;
		}
		if (gamedata.empty()) {
			error = "no gamedata.jsonc path was found";
			return false;
		}

		plg::vector<plg::string> paths;
		paths.push_back(plg::string(gamedata.string()));
		const s2sdk::ConfigId config = s2sdk::LoadGameConfigFile(paths);
		// A failed load is 0 (ref/s2sdk-src/src/export/gameconfig.cpp:26) - the older C++ samples
		// that test for (ConfigId)-1 never see the failure.
		if (config == 0) {
			error = "s2sdk could not load " + gamedata.string();
			return false;
		}
		gamedata_ = config;

		void* giveNamedItem = nullptr;
		void* setAttribute = nullptr;
		try {
			giveNamedItem = s2sdk::GetGameConfigSignature(config, plg::string(kSignatureGiveNamedItem));
			setAttribute = s2sdk::GetGameConfigSignature(config, plg::string(kSignatureSetAttribute));
		} catch (...) {
		}
		if (giveNamedItem == nullptr) {
			error = "signature GiveNamedItem not found - the hook is off";
			TeardownHook();
			return false;
		}
		if (setAttribute == nullptr) {
			// Without the native writer no skin can be applied at all (paint/seed/wear, StatTrak,
			// stickers and keychains all go through it), so a hook without it would only do half the
			// work on some paths - fail honestly instead.
			error = "signature CAttributeList::SetOrAddAttributeValueByName not found - skins cannot be applied";
			TeardownHook();
			return false;
		}
		setAttribute_ = reinterpret_cast<SetAttributeFn>(setAttribute);

		polyhook::HookHandle hook = nullptr;
		try {
			plg::vector<polyhook::DataType> arguments;
			AppendGiveNamedItemArguments(arguments);
			hook = polyhook::HookDetour(giveNamedItem, kGiveNamedItemReturn, arguments, -1,
										plg::string("WeaponSkins::GiveNamedItem"));
		} catch (...) {
			hook = nullptr;
		}
		if (hook == nullptr) {
			error = "polyhook HookDetour(GiveNamedItem) failed";
			if (__polyhook_GetError != nullptr) {
				try {
					const plg::string message = polyhook::GetError();
					if (!message.empty()) {
						error += ": ";
						error += ToString(message);
					}
				} catch (...) {
				}
			}
			TeardownHook();
			return false;
		}
		if (!polyhook::AddCallback(hook, polyhook::CallbackType::Post, &Impl::PostCallback)) {
			// Nothing of ours is attached to the (shared) detour: drop it only when nobody else is.
			if (__polyhook_AreCallbacksRegistered != nullptr && __polyhook_UnhookDetour != nullptr) {
				try {
					if (!polyhook::AreCallbacksRegistered(hook)) {
						polyhook::UnhookDetour(giveNamedItem);
					}
				} catch (...) {
				}
			}
			error = "polyhook AddCallback(GiveNamedItem, Post) failed";
			TeardownHook();
			return false;
		}

		lookup_ = std::move(lookup);
		hook_ = hook;
		hookTarget_ = giveNamedItem;
		hookInstalled_ = true;
		s_active = this;
		return true;
	}

	// Drops the detour (RemoveCallback -> AreCallbacksRegistered -> UnhookDetour with the TARGET
	// address; the trampoline is not a key and UnhookAll would unhook other plugins) and closes the
	// gamedata config. The prepared offsets and the resolved native writer stay: the engine can
	// still apply items in place without the hook.
	void TeardownHook() {
		if (hookInstalled_) {
			try {
				if (__polyhook_RemoveCallback != nullptr) {
					polyhook::RemoveCallback(hook_, polyhook::CallbackType::Post, &Impl::PostCallback);
				}
				if (__polyhook_AreCallbacksRegistered != nullptr && __polyhook_UnhookDetour != nullptr &&
					hookTarget_ != nullptr && !polyhook::AreCallbacksRegistered(hook_)) {
					polyhook::UnhookDetour(hookTarget_);
				}
			} catch (...) {
			}
		}
		if (s_active == this) {
			s_active = nullptr;
		}
		hook_ = nullptr;
		hookTarget_ = nullptr;
		hookInstalled_ = false;
		lookup_ = nullptr;

		if (gamedata_ != 0 && __s2sdk_CloseGameConfigFile != nullptr) {
			try {
				s2sdk::CloseGameConfigFile(gamedata_);
			} catch (...) {
			}
		}
		gamedata_ = 0;
	}

	// Full teardown: everything TeardownHook does, plus the deferred callbacks and the offsets.
	// The generation bump first invalidates every deferred callback (timer-based and next-frame);
	// then the still-pending timers are stopped. Only pending records are in timers_ (a fired one is
	// removed inside its own callback), and a pending timer is still alive in pisexcompat's registry,
	// so RemoveTimer here never touches a freed CTimer. Callers must run this before pisex::Shutdown()
	// (the runtime does - Runtime::Impl::Stop calls engine->Shutdown() first), otherwise the registry
	// is already gone and the pointers would be stale.
	void Shutdown() {
		++token_->generation;
		for (PendingTimer& record : timers_) {
			if (record.timer != nullptr) {
				record.timer->RemoveTimer();
			}
		}
		timers_.clear();
		TeardownHook();
		setAttribute_ = nullptr;
		econGlovesChanged_ = -1;
		servicesPawn_ = -1;
		controllerPlayerPawn_ = -1;
		offsets_ = Offsets{};
		for (SlotState& state : slots_) {
			state = SlotState{};
		}
		for (AppliedState& state : applied_) {
			state = AppliedState{};
		}
	}

	// Diagnostic counters (engine.hpp). Game thread only, no allocation.
	const ApplyStats& Stats() const { return stats_; }

	// --- the GiveNamedItem post hook ---------------------------------------------------------------

	// polyhook's handler for the shared detour: a plain function pointer with no user data, and no
	// exception may escape into the detoured engine function.
	static polyhook::ResultType PostCallback(polyhook::HookHandle hook, polyhook::ParametersHandle params,
											 int32_t count, polyhook::ReturnHandle ret, polyhook::CallbackType type) {
		(void)hook;
		(void)count;
		if (type != polyhook::CallbackType::Post) {
			return polyhook::ResultType::Ignored;
		}
		Impl* self = s_active;
		if (self == nullptr || __polyhook_GetReturnPointer == nullptr) {
			return polyhook::ResultType::Ignored;
		}
		try {
			void* weapon = polyhook::GetReturnPointer(ret);
			// Argument 0 is the first declared parameter, `this` for a member function
			// (PITFALLS.md §D.4; the reference's NativeService passed the same pair on). Without
			// the getter the hook still runs: the fallback owner chain covers the call.
			void* services =
				__polyhook_GetArgumentPointer != nullptr ? polyhook::GetArgumentPointer(params, 0) : nullptr;
			self->OnWeaponGiven(weapon, services);
		} catch (...) {
			// never propagate into the engine
		}
		return polyhook::ResultType::Ignored;
	}

	// True when the snapshot configures at least one weapon or knife skin for either team - the only
	// two categories the GiveNamedItem hook applies. Everything else (glove, agent, music kit, pin)
	// is written by the spawn/refresh paths, never here, so a snapshot without weapon or knife rows
	// can leave the hook before any client query.
	static bool HasAnyWeaponSkin(const Snapshot& snapshot) {
		return !snapshot.teams[0].weapons.empty() || !snapshot.teams[1].weapons.empty() ||
			   snapshot.teams[0].knife.has_value() || snapshot.teams[1].knife.has_value();
	}

	// The whole hot path. Order matters: the cheapest and most decisive guards first, everything
	// that can fail (owner resolution, cache lookup) before the first write.
	void OnWeaponGiven(void* weapon, void* services) {
		++stats_.hookCalls;
		if (!offsets_.ready || !hookInstalled_ || !lookup_) {
			return;
		}
		// Map guard: between OnMapEnd and the next MapStart no entity or client query is allowed.
		if (!pisex::IsGameReady()) {
			return;
		}
		if (!IsEntityPointerUsable(weapon)) {
			++stats_.hookBadWeapon;
			return;
		}

		// Owner, resolved immediately and validated in this same callback. The player is argument 0
		// - the CCSPlayer_ItemServices* the item is given through, whose component chainer's leading
		// pointer is the owning pawn (see Prepare). The weapon's own m_hOwnerEntity is NOT yet
		// assigned when the post hook runs: live, that read made every call fail (the old counter
		// read hookNoOwner=170, hookGun=0), so it is only the fallback.
		//
		// The component is not an entity instance, so s2sdk's IsEntityPtr check cannot validate it
		// (or a pawn pointer - see PawnHandle): the read below mirrors s2sdk's own
		// CPlayerPawnComponent::GetPawn() and only the page guard rejects a wild address. Every
		// step after it (m_hController -> handle -> controller -> slot) is handle-based and
		// validates itself.
		void* pawn = nullptr;
		if (services != nullptr && servicesPawn_ >= 0) {
			void* candidate = ReadMem<void*>(services, servicesPawn_);
			if (IsPageMapped(candidate)) {
				pawn = candidate;
			}
		} else {
			++stats_.hookNoServices; // no readable services argument (or no offset for its pawn)
		}
		if (pawn == nullptr) {
			// The reference's fallback chain, unchanged: weapon -> m_hOwnerEntity -> pawn.
			pawn = PointerFromHandle(ReadMem<int32_t>(weapon, offsets_.weaponOwner));
			// EntHandleToEntPointer already rejected an invalid or stale handle: a second
			// IsValidEntPointer lookup is the double validation the handle-system guide calls
			// "expensive and unnecessary" (plugins_s2sdk_guides_handle-system.txt:604-612, 678).
			if (pawn == nullptr) {
				++stats_.hookNoPawn; // neither the services argument nor the owner chain had a pawn
				return;
			}
		}
		const int32_t controllerHandle = ReadMem<int32_t>(pawn, offsets_.pawnController);
		void* controller = PointerFromHandle(controllerHandle);
		if (controller == nullptr) { // handle validation is sufficient (see the owner chain above)
			++stats_.hookNoController;
			return;
		}
		if (__s2sdk_EntPointerToPlayerSlot == nullptr) {
			++stats_.hookNoSlot;
			return;
		}
		const int slot = s2sdk::EntPointerToPlayerSlot(controller);
		if (slot < 0 || slot >= kMaxPlayers) {
			++stats_.hookNoSlot;
			return;
		}
		if (!pisex::IsClientReady(slot)) {
			++stats_.hookNotReady;
			return;
		}
		if (pisex::IsFakeClient(slot)) {
			++stats_.hookBot;
			return; // PITFALLS.md §I: bots take the same path and must be skipped
		}

		const Snapshot* snapshot = lookup_(slot);
		if (snapshot == nullptr || snapshot->steamId == 0) {
			++stats_.hookNoSnapshot;
			return;
		}
		// The hook only ever writes a weapon's or a knife's skin. A player who has neither configured
		// (the common case) exits here, before the steam-id/team queries, the item-view resolution and
		// the pawn-handle read below - nothing new is written even if the entry were stale.
		if (!HasAnyWeaponSkin(*snapshot)) {
			return;
		}
		if (snapshot->steamId != pisex::GetSteamID64(slot)) {
			++stats_.hookNoSnapshot;
			return; // the slot was taken over since the cache entry was written
		}
		const int teamValue = static_cast<int>(pisex::GetPlayerTeam(slot));
		const int teamIndex = TeamIndex(teamValue);
		if (teamIndex < 0) {
			++stats_.hookNoTeam;
			return;
		}

		WeaponView view;
		if (!MakeWeaponView(weapon, view)) {
			return;
		}
		const TeamItems& items = snapshot->teams[static_cast<std::size_t>(teamIndex)];
		const uint16_t definition = ReadMem<uint16_t>(view.item, offsets_.itemDefIndex);
		// The life key is the pawn HANDLE, read the same way every other path does (PawnHandle),
		// not through the pawn pointer, whose conversion the live build refuses.
		AppliedState& applied = AppliedFor(slot, snapshot->steamId, PawnHandle(slot));
		if (IsKnife(definition)) {
			if (items.knife.has_value()) {
				ApplyKnifeFields(view, *items.knife);
				++stats_.hookKnife;
				applied.knife[static_cast<std::size_t>(teamIndex)] = true;
			}
			return;
		}
		const auto it = items.weapons.find(definition);
		if (it != items.weapons.end()) {
			ApplyGunFields(view, it->second);
			++stats_.hookGun;
			MarkWeaponApplied(applied, teamValue, definition);
		} else {
			++stats_.hookNoMatch;
		}
	}

	// --- application paths (called by the runtime, never from the hook) -----------------------------

	// Captures the identity of the player the deferred pass belongs to. Called while the player is
	// validated, so the pawn handle is the one the pass was created for.
	DeferredGuard MakeGuard(int slot, uint64_t steamId) const {
		DeferredGuard guard;
		guard.slot = slot;
		guard.steamId = steamId;
		guard.pawnHandle = PawnHandle(slot);
		guard.team = static_cast<int>(pisex::GetPlayerTeam(slot));
		guard.generation = token_->generation;
		return guard;
	}

	// True when the deferred work is still about the same player on the same map with the same pawn
	// and team, and the engine is still the same incarnation.
	bool GuardValid(const DeferredGuard& guard) const {
		if (guard.generation != token_->generation) {
			return false; // Shutdown() ran (or the engine is about to be destroyed)
		}
		if (!offsets_.ready || !pisex::IsGameReady()) {
			return false; // teardown, or a map change in flight (no client query is allowed there)
		}
		if (guard.slot < 0 || guard.slot >= kMaxPlayers) {
			return false;
		}
		if (!pisex::IsClientReady(guard.slot) || pisex::IsFakeClient(guard.slot)) {
			return false;
		}
		if (pisex::GetSteamID64(guard.slot) != guard.steamId) {
			return false; // a reconnect or a different player took the slot
		}
		if (PawnHandle(guard.slot) != guard.pawnHandle) {
			return false; // a respawn: the pawn the pass was created for is gone
		}
		if (static_cast<int>(pisex::GetPlayerTeam(guard.slot)) != guard.team) {
			return false; // a team switch: the items of the captured team are no longer worn
		}
		return true;
	}

	// True when the deferred pass may run. The FIRST step must not touch the Impl: a queued
	// NextFrame callback can outlive the engine (pisexcompat's queue is only dropped by
	// pisex::Shutdown, which is not necessarily called between engine resets), so `weak.lock()` -
	// called on the captured copy, no member involved - is what decides whether `this` is still a
	// live object. Only after that does GuardValid() read member state. Call sites spell the two
	// steps out (see below) precisely so the lock comes first.
	static bool TokenAlive(const std::weak_ptr<EngineToken>& weak) { return weak.lock() != nullptr; }

	// --- applied-state bookkeeping (one record per player life) ------------------------------------

	AppliedState& AppliedFor(int slot, uint64_t steamId, int32_t pawnHandle) {
		AppliedState& state = applied_[static_cast<std::size_t>(slot)];
		if (state.steamId != steamId || state.pawnHandle != pawnHandle) {
			// A different player or a new pawn: the engine has already given the player fresh
			// default items, so nothing this engine wrote before is on them any more.
			state = AppliedState{};
			state.steamId = steamId;
			state.pawnHandle = pawnHandle;
		}
		return state;
	}

	AppliedState& AppliedFor(int slot, const DeferredGuard& guard) {
		return AppliedFor(slot, guard.steamId, guard.pawnHandle);
	}

	static uint32_t WeaponKey(int team, uint16_t definition) {
		return (static_cast<uint32_t>(team) << 16) | static_cast<uint32_t>(definition);
	}

	static bool WasWeaponApplied(const AppliedState& state, int team, uint16_t definition) {
		const uint32_t key = WeaponKey(team, definition);
		for (std::size_t i = 0; i < state.weaponCount; ++i) {
			if (state.weapons[i] == key) {
				return true;
			}
		}
		return false;
	}

	static void MarkWeaponApplied(AppliedState& state, int team, uint16_t definition) {
		if (WasWeaponApplied(state, team, definition) || state.weaponCount >= state.weapons.size()) {
			return; // bounded: a full table only means one removal may not be cleaned up by !wp
		}
		state.weapons[state.weaponCount++] = WeaponKey(team, definition);
	}

	static void UntrackWeapon(AppliedState& state, int team, uint16_t definition) {
		const uint32_t key = WeaponKey(team, definition);
		for (std::size_t i = 0; i < state.weaponCount; ++i) {
			if (state.weapons[i] == key) {
				state.weapons[i] = state.weapons[state.weaponCount - 1];
				--state.weaponCount;
				return;
			}
		}
	}

	void ApplySpawn(int slot, const Snapshot& snapshot, bool refresh) {
		if (!SlotUsable(slot) || !SnapshotMatches(slot, snapshot)) {
			return;
		}
		const int teamIndex = TeamIndex(pisex::GetPlayerTeam(slot));
		if (teamIndex < 0) {
			return;
		}
		const std::size_t index = static_cast<std::size_t>(teamIndex);
		const TeamItems& items = snapshot.teams[index];
		const DeferredGuard guard = MakeGuard(slot, snapshot.steamId);

		ApplyKnife(slot, items, /*regive=*/false, guard);
		if (refresh) {
			// A deferred load landed while the player is already alive: the weapons they hold were
			// given before the data existed, so their skins are applied in place (the hook covers
			// everything given later).
			ApplyHeldWeapons(slot, items, /*regive=*/false, guard);
		}
		ApplyGlove(slot, items, guard);
		ApplyAgent(slot, items, index, guard);
		ApplyMusic(slot, snapshot);
		ApplyPin(slot, items);

		// The reference applies glove and agent twice (NextTick and DelayBySeconds(0.1f)) because the
		// engine may reset the player model right after the spawn. The deferred pass re-resolves
		// everything and does nothing when the player, the pawn, the team or the map changed.
		const std::weak_ptr<EngineToken> weak = token_;
		ScheduleOnce(kSpawnReapplyDelaySeconds, [this, weak, guard]() {
			if (!TokenAlive(weak)) {
				return; // the engine is gone: no member may be touched from here
			}
			if (!GuardValid(guard)) {
				return; // a different pawn, team, player or map
			}
			const Snapshot* current = lookup_ ? lookup_(guard.slot) : nullptr;
			if (current == nullptr || current->steamId != guard.steamId) {
				return;
			}
			const int teamIndex = TeamIndex(guard.team);
			if (teamIndex < 0) {
				return;
			}
			const TeamItems& items = current->teams[static_cast<std::size_t>(teamIndex)];
			ApplyGlove(guard.slot, items, guard);
			ApplyAgent(guard.slot, items, static_cast<std::size_t>(teamIndex), guard);
		});
	}

	void RefreshInventory(int slot, const Snapshot& snapshot) {
		if (!SlotUsable(slot) || !SnapshotMatches(slot, snapshot)) {
			return;
		}
		const int teamIndex = TeamIndex(pisex::GetPlayerTeam(slot));
		if (teamIndex < 0) {
			return;
		}
		const std::size_t index = static_cast<std::size_t>(teamIndex);
		const TeamItems& items = snapshot.teams[index];
		const DeferredGuard guard = MakeGuard(slot, snapshot.steamId);

		// Everything that only *writes* happens now; everything that regives weapons is deferred to
		// the next frame (the reference's Scheduler.NextTick around Regive*), so a command handler
		// never gives/removes items on its own stack.
		ApplyHeldWeapons(slot, items, /*regive=*/false, guard);
		ApplyGlove(slot, items, guard);
		ApplyAgent(slot, items, index, guard);
		ApplyMusic(slot, snapshot);
		ApplyPin(slot, items);

		const std::weak_ptr<EngineToken> weak = token_;
		DeferNextFrame([this, weak, guard]() {
			if (!TokenAlive(weak)) {
				return; // the engine is gone: no member may be touched from here
			}
			if (!GuardValid(guard)) {
				return; // a different pawn, team, player or map
			}
			const Snapshot* current = lookup_ ? lookup_(guard.slot) : nullptr;
			if (current == nullptr || current->steamId != guard.steamId) {
				return;
			}
			const int teamIndex = TeamIndex(guard.team);
			if (teamIndex < 0) {
				return;
			}
			const TeamItems& items = current->teams[static_cast<std::size_t>(teamIndex)];
			ApplyHeldWeapons(guard.slot, items, /*regive=*/true, guard);
			ApplyKnife(guard.slot, items, /*regive=*/true, guard);
		});
	}

	// The pickup path that only touches what the player already owns: same-stack validation of the
	// slot, the live player behind it (controller, pawn, steamId) and the current team, then the
	// snapshot's items are applied to the owned weapons AND the owned knife (a picked-up knife must
	// get the chosen skin immediately - without regiving it). No Give/Remove/Select, no DB, nothing
	// structural - the runtime calls this from an item_pickup NextFrame with its own slot guard.
	void ApplyOwnedWeapons(int slot, const Snapshot& snapshot) {
		if (!SlotUsable(slot) || !SnapshotMatches(slot, snapshot)) {
			return;
		}
		if (FreshPawn(slot) == nullptr) {
			return; // no pawn: nothing is owned right now
		}
		const int teamIndex = TeamIndex(pisex::GetPlayerTeam(slot));
		if (teamIndex < 0) {
			return;
		}
		const DeferredGuard guard = MakeGuard(slot, snapshot.steamId);
		const TeamItems& items = snapshot.teams[static_cast<std::size_t>(teamIndex)];
		ApplyHeldWeapons(slot, items, /*regive=*/false, guard);
		ApplyKnife(slot, items, /*regive=*/false, guard);
	}

	int ActiveDefinition(int slot) const {
		if (!offsets_.ready || !SlotUsable(slot)) {
			return 0;
		}
		void* weapon = ActiveWeapon(slot);
		WeaponView view;
		if (weapon == nullptr || !MakeWeaponView(weapon, view)) {
			return 0;
		}
		return static_cast<int>(ReadMem<uint16_t>(view.item, offsets_.itemDefIndex));
	}

	// Writes the (already persisted by the runtime) StatTrak counter onto the matching weapon. The
	// reference wrote it to the plain list and then re-applied the whole skin
	// (StattrakService.cs:56-59 -> SetWeaponSkins); writing both lists here is the same end state in
	// one step and keeps the counter visible immediately.
	void SetStatTrak(int slot, const Skin& skin) {
		if (!offsets_.ready || !SlotUsable(slot)) {
			return;
		}
		void* weapon = ActiveWeapon(slot);
		WeaponView view;
		if (weapon == nullptr || !MakeWeaponView(weapon, view) ||
			ReadMem<uint16_t>(view.item, offsets_.itemDefIndex) != skin.definition) {
			weapon = FindHeldWeapon(slot, skin.definition);
			if (weapon == nullptr || !MakeWeaponView(weapon, view)) {
				return;
			}
		}
		SetKillEater(view, skin.stattrak);
	}

	// The public handle-based entry. The caller owns the ownership validation (engine.hpp): the
	// handle must belong to a real player in this same callback.
	bool ApplyWeaponByHandle(int32_t handle, const Skin& skin, bool knife) {
		if (!offsets_.ready || !pisex::IsGameReady()) {
			return false;
		}
		void* weapon = PointerFromHandle(handle);
		WeaponView view;
		if (weapon == nullptr || !MakeWeaponView(weapon, view)) {
			return false;
		}
		if (knife || IsKnife(ReadMem<uint16_t>(view.item, offsets_.itemDefIndex))) {
			ApplyKnifeFields(view, skin);
		} else {
			ApplyGunFields(view, skin);
		}
		return true;
	}

	// --- guards ---------------------------------------------------------------------------------

	bool SlotUsable(int slot) const {
		return offsets_.ready && slot >= 0 && slot < kMaxPlayers && pisex::IsGameReady() &&
			   !pisex::IsFakeClient(slot) && pisex::IsClientReady(slot);
	}

	bool SnapshotMatches(int slot, const Snapshot& snapshot) const {
		return snapshot.steamId != 0 && snapshot.steamId == pisex::GetSteamID64(slot);
	}

	// --- pointer resolution (per call, never stored) ----------------------------------------------

	bool MakeWeaponView(void* weapon, WeaponView& view) const {
		if (weapon == nullptr || offsets_.weaponAttributeManager < 0 || offsets_.containerItem < 0) {
			return false;
		}
		view.weapon = weapon;
		view.attributeManager = static_cast<std::uint8_t*>(weapon) + offsets_.weaponAttributeManager;
		view.item = static_cast<std::uint8_t*>(view.attributeManager) + offsets_.containerItem;
		view.itemOffset = offsets_.weaponAttributeManager + offsets_.containerItem;
		view.weaponHandle = HandleFromPointer(weapon);
		return true;
	}

	void* FreshPawn(int slot) const { return __s2sdk_GetClientPawn != nullptr ? s2sdk::GetClientPawn(slot) : nullptr; }

	// The guard every s2sdk::GetClientWeapons call needs: the pawn exists AND the engine has already
	// attached its m_pWeaponServices. s2sdk's GetClientWeapons resolves the services through
	// helpers::GetService2, which prints a WARNING for every call while the pawn is gone ("invalid
	// player pawn") or exists without its services yet ("invalid player service" - the live window
	// during spawn/respawn/team changes, which produced bursts of "Cannot execute 'plg::vector<int>
	// GetClientWeapons(int)'" lines on the test server; plugins/RestrictedWeapons skips the same
	// window before its call). Both states own no weapon, so skipping the call returns exactly what
	// the engine would have answered (an empty list) without the console noise.
	bool HasWeaponServices(int slot) const {
		if (offsets_.pawnWeaponServices < 0) {
			return false;
		}
		void* pawn = FreshPawn(slot);
		if (pawn == nullptr) {
			return false;
		}
		const void* services = ReadMem<const void*>(pawn, offsets_.pawnWeaponServices);
		return services != nullptr && IsPageMapped(services);
	}

	void* FreshController(int slot) const {
		if (__s2sdk_PlayerSlotToEntPointer == nullptr) {
			return nullptr;
		}
		try {
			return s2sdk::PlayerSlotToEntPointer(slot);
		} catch (...) {
			return nullptr;
		}
	}

	// The pawn HANDLE of the player in a slot, read from the controller's m_hPlayerPawn (a CHandle,
	// int32 - the same value shape as every other entity handle here). The pointer conversion
	// (EntPointerToEntHandle) is only the fallback: on the live build IsEntityPtr rejects the pawn
	// pointers GetClientPawn hands out (ref/s2sdk-src/src/export/entities.cpp:51-57), so that route
	// returned -1 for every player - gloves were written but never visible, the agent never
	// changed, and every deferred identity check compared -1 with -1.
	int32_t PawnHandleFromController(void* controller) const {
		if (controller == nullptr || controllerPlayerPawn_ < 0) {
			return -1;
		}
		return ReadMem<int32_t>(controller, controllerPlayerPawn_);
	}

	int32_t PawnHandle(int slot) const {
		const int32_t handle = PawnHandleFromController(FreshController(slot));
		if (IsValidHandle(handle)) {
			return handle;
		}
		return HandleFromPointer(FreshPawn(slot));
	}

	// Diagnostic view for the runtime's ws_status: both readings, raw, so a build where they
	// disagree (or a slot whose handles are gone) is visible in one per-slot line. Client queries
	// only while a map is up (PITFALLS.md §B.2) and only with the offsets prepared.
	PawnHandles PawnHandleReport(int slot) const {
		PawnHandles handles;
		if (!offsets_.ready || !pisex::IsGameReady()) {
			return handles;
		}
		handles.viaController = PawnHandleFromController(FreshController(slot));
		handles.viaPointer = HandleFromPointer(FreshPawn(slot));
		return handles;
	}

	// The active weapon of a live player (m_pWeaponServices -> m_hActiveWeapon).
	void* ActiveWeapon(int slot) const {
		void* pawn = FreshPawn(slot);
		if (pawn == nullptr || offsets_.pawnWeaponServices < 0 || offsets_.weaponServicesActive < 0) {
			return nullptr;
		}
		const void* services = ReadMem<const void*>(pawn, offsets_.pawnWeaponServices);
		if (services == nullptr || !IsPageMapped(services)) {
			return nullptr;
		}
		return PointerFromHandle(ReadMem<int32_t>(services, offsets_.weaponServicesActive));
	}

	void* FindHeldWeapon(int slot, uint16_t definition) const {
		if (__s2sdk_GetClientWeapons == nullptr || !HasWeaponServices(slot)) {
			return nullptr;
		}
		try {
			for (const int32_t handle : s2sdk::GetClientWeapons(slot)) {
				void* weapon = PointerFromHandle(handle);
				WeaponView view;
				if (weapon == nullptr || !MakeWeaponView(weapon, view)) {
					continue;
				}
				if (ReadMem<uint16_t>(view.item, offsets_.itemDefIndex) == definition) {
					return weapon;
				}
			}
		} catch (...) {
		}
		return nullptr;
	}

	void* ReadInventoryServices(void* controller) const {
		if (offsets_.controllerInventoryServices < 0) {
			return nullptr;
		}
		const void* services = ReadMem<const void*>(controller, offsets_.controllerInventoryServices);
		return services != nullptr && IsPageMapped(services) ? const_cast<void*>(services) : nullptr;
	}

	// --- the gun path (the reference's ApplyWeaponAttributes) ---------------------------------------

	void ApplyGunFields(const WeaponView& view, const Skin& skin) {
		// Econ item fields: raw writes at the resolved offsets plus a network state change. The
		// typed s2sdk setters refuse exactly these names with FollowCS2ServerGuidelines on
		// (PITFALLS.md §C.7), so the raw path is deliberate - do not "clean it up".
		WriteItemField<uint16_t>(view, offsets_.itemDefIndex, skin.definition);
		WriteItemField<int32_t>(view, offsets_.itemQuality, static_cast<int32_t>(skin.quality));
		SetPaintAttributes(view.item, skin);

		// The reference stops here when the definition index has no classname
		// (HookInventoryUpdateService.cs:469-474): no bodygroup, no StatTrak, no nametag, no
		// stickers, no keychain. Same early exit, same consequence.
		const ItemDefinition* definition = econ_.Item(skin.definition);
		if (definition == nullptr || definition->name.empty()) {
			NotifyItemViewChanged(view); // the paint writes above still have to reach the client
			return;
		}
		if (IsValidHandle(view.weaponHandle)) {
			InputString(view.weaponHandle, kInputSetBodygroup,
						econ_.Legacy(skin.definition, skin.paint) ? kBodygroupLegacy : kBodygroupStandard);
		}
		if (skin.quality == kStatTrakQuality) {
			SetKillEater(view, skin.stattrak);
		}
		WriteNametag(view, skin);
		SetStickers(view, skin);
		SetKeychain(view, skin);
		NotifyItemViewChanged(view);
	}

	// The reference's ApplyKnifeAttributes: quality, nametag, ChangeSubclass when the item is a
	// different knife, definition index, StatTrak or the vector clear, then the paint trio.
	void ApplyKnifeFields(const WeaponView& view, const Skin& skin) {
		WriteItemField<int32_t>(view, offsets_.itemQuality, static_cast<int32_t>(skin.quality));
		WriteNametag(view, skin);

		const uint16_t current = ReadMem<uint16_t>(view.item, offsets_.itemDefIndex);
		if (current != skin.definition) {
			// ChangeSubclass takes the decimal definition index; the values are compile-time literals
			// (KnifeSubclassValue), so the hook formats nothing.
			const char* value = detail::KnifeSubclassValue(skin.definition);
			if (value != nullptr && IsValidHandle(view.weaponHandle)) {
				InputString(view.weaponHandle, kInputChangeSubclass, value);
			}
		}
		WriteItemField<uint16_t>(view, offsets_.itemDefIndex, skin.definition);

		if (skin.quality == kStatTrakQuality) {
			SetKillEater(view, skin.stattrak);
		} else {
			ClearAttributeLists(view);
		}
		SetPaintAttributes(view.item, skin);
		NotifyItemViewChanged(view);
	}

	// An econ item field: the raw write at its width plus the network state change the reference
	// produced for that field.
	template <typename T>
	void WriteItemField(const WeaponView& view, int offset, T value) {
		if (offset < 0) {
			return;
		}
		WriteMem<T>(view.item, offset, value);
		NotifyField(view.weapon, view.itemOffset + offset, offsets_.weaponChain);
	}

	void SetPaintAttributes(void* item, const Skin& skin) {
		void* networked = static_cast<std::uint8_t*>(item) + offsets_.itemNetworkedAttributes;
		void* plain = static_cast<std::uint8_t*>(item) + offsets_.itemAttributeList;
		// Networked copy first, then the plain one - the reference's order
		// (HookInventoryUpdateService.cs:462-467). Both are needed: the client reads the plain list.
		SetAttributeValue(networked, kAttrTexturePrefab, IntAttributeValue(skin.paint));
		SetAttributeValue(networked, kAttrTextureSeed, IntAttributeValue(skin.seed));
		SetAttributeValue(networked, kAttrTextureWear, skin.wear);
		SetAttributeValue(plain, kAttrTexturePrefab, IntAttributeValue(skin.paint));
		SetAttributeValue(plain, kAttrTextureSeed, IntAttributeValue(skin.seed));
		SetAttributeValue(plain, kAttrTextureWear, skin.wear);
	}

	void SetKillEater(const WeaponView& view, int count) {
		const float value = StatTrakValue(count);
		void* plain = static_cast<std::uint8_t*>(view.item) + offsets_.itemAttributeList;
		void* networked = static_cast<std::uint8_t*>(view.item) + offsets_.itemNetworkedAttributes;
		SetAttributeValue(plain, kAttrKillEater, value);
		SetAttributeValue(plain, kAttrKillEaterScoreType, 0.0f);
		SetAttributeValue(networked, kAttrKillEater, value);
		SetAttributeValue(networked, kAttrKillEaterScoreType, 0.0f);
	}

	void SetStickers(const WeaponView& view, const Skin& skin) {
		for (std::size_t i = 0; i < skin.stickers.size(); ++i) {
			const Sticker& sticker = skin.stickers[i];
			if (sticker.id == 0) {
				continue; // unset slot (the reference's GetSticker(i) == null)
			}
			const detail::StickerNames& names = kStickerNames[i];
			SetStickerAttribute(view, names.id, IntBitsAsAttribute(sticker.id));
			if (sticker.schema != kStickerSchemaNone) {
				SetStickerAttribute(view, names.schema, IntBitsAsAttribute(sticker.schema));
				SetStickerAttribute(view, names.offsetX, sticker.x);
				SetStickerAttribute(view, names.offsetY, sticker.y);
			}
			SetStickerAttribute(view, names.wear, sticker.wear);
			SetStickerAttribute(view, names.scale, sticker.scale);
			SetStickerAttribute(view, names.rotation, sticker.rotation);
		}
	}

	void SetKeychain(const WeaponView& view, const Skin& skin) {
		const Keychain& keychain = skin.keychain;
		if (keychain.id == 0) {
			return;
		}
		SetStickerAttribute(view, kKeychainNames.id, IntBitsAsAttribute(keychain.id));
		SetStickerAttribute(view, kKeychainNames.offsetX, keychain.x);
		SetStickerAttribute(view, kKeychainNames.offsetY, keychain.y);
		SetStickerAttribute(view, kKeychainNames.offsetZ, keychain.z);
		SetStickerAttribute(view, kKeychainNames.seed, IntBitsAsAttribute(keychain.seed));
	}

	// Stickers and keychains go into both lists (the reference's SetStickerAttribute,
	// HookInventoryUpdateService.cs:526-530): the networked copy alone leaves them invisible.
	void SetStickerAttribute(const WeaponView& view, const char* name, float value) {
		SetAttributeValue(static_cast<std::uint8_t*>(view.item) + offsets_.itemNetworkedAttributes, name, value);
		SetAttributeValue(static_cast<std::uint8_t*>(view.item) + offsets_.itemAttributeList, name, value);
	}

	void SetAttributeValue(void* list, const char* name, float value) {
		if (setAttribute_ == nullptr || list == nullptr) {
			return;
		}
		setAttribute_(list, name, value);
	}

	// The reference's non-StatTrak knife branch: Attributes.RemoveAll() on both lists. The clear is
	// a reset of the m_Attributes *count* (4 bytes at object + 0) - the data pointer at +8 and the
	// capacity at +16 are left alone. The header is validated first (IsAttributeHeaderPlausible,
	// see engine_detail.hpp): an unexpected layout leaves the lists alone instead of corrupting
	// them. The old CUtlVector guess (count at +16) would have overwritten the capacity field.
	void ClearAttributeLists(const WeaponView& view) {
		if (offsets_.attributeVector < 0) {
			return;
		}
		const auto clear = [&](void* list, int listOffset) {
			if (!IsAttributeHeaderPlausible(list, offsets_.attributeVector)) {
				return;
			}
			WriteMem<int32_t>(static_cast<std::uint8_t*>(list) + offsets_.attributeVector, kAttributeCountOffset, 0);
			NotifyField(view.weapon, view.itemOffset + listOffset + offsets_.attributeVector, offsets_.weaponChain);
		};
		// Both lists are notified separately, like the reference's two RemoveAll() calls.
		clear(static_cast<std::uint8_t*>(view.item) + offsets_.itemNetworkedAttributes, offsets_.itemNetworkedAttributes);
		clear(static_cast<std::uint8_t*>(view.item) + offsets_.itemAttributeList, offsets_.itemAttributeList);
	}

	void WriteNametag(const WeaponView& view, const Skin& skin) {
		if (skin.nametag.empty() || offsets_.itemCustomName < 0) {
			return;
		}
		WriteFixedString(view.item, offsets_.itemCustomName, skin.nametag, kWidthCustomName);
		NotifyField(view.weapon, view.itemOffset + offsets_.itemCustomName, offsets_.weaponChain);
	}

	// --- gloves, agent, music, pins (the reference's spawn set) -------------------------------------

	void ApplyGlove(int slot, const TeamItems& items, const DeferredGuard& guard) {
		const int teamIndex = TeamIndex(guard.team);
		if (teamIndex < 0) {
			return;
		}
		void* pawn = FreshPawn(slot);
		if (pawn == nullptr || offsets_.pawnEconGloves < 0) {
			return;
		}
		AppliedState& applied = AppliedFor(slot, guard);
		if (!items.glove.has_value()) {
			++stats_.gloveNoData;
			// The site no longer configures gloves for this team, but this engine put one on this
			// pawn: undo it. Clearing the item (definition 0 + m_bInitialized false) is what makes
			// the *game* rebuild the player's default gloves - the reference's ResetGloveSkin only
			// re-applied its data service (which had just been emptied), so a removal left the old
			// glove skin on the player forever. A glove this engine never wrote is never touched.
			if (applied.glove[static_cast<std::size_t>(teamIndex)]) {
				void* gloves = static_cast<std::uint8_t*>(pawn) + offsets_.pawnEconGloves;
				WriteMem<uint16_t>(gloves, offsets_.itemDefIndex, 0);
				NotifyField(pawn, offsets_.pawnEconGloves + offsets_.itemDefIndex, offsets_.pawnChain);
				if (offsets_.itemInitialized >= 0) {
					WriteMem<std::uint8_t>(gloves, offsets_.itemInitialized, 0);
					NotifyField(pawn, offsets_.pawnEconGloves + offsets_.itemInitialized, offsets_.pawnChain);
				}
				// The game only rebuilds the default gloves when the counter moves - the same
				// announcement as the apply path (weapons_gloves.cpp:76-88).
				NotifyAttributeLists(pawn, offsets_.pawnEconGloves, offsets_.pawnChain);
				BumpEconGlovesChanged(pawn);
				applied.glove[static_cast<std::size_t>(teamIndex)] = false;
			}
			return;
		}
		const Skin& glove = *items.glove;
		void* gloves = static_cast<std::uint8_t*>(pawn) + offsets_.pawnEconGloves;
		WriteMem<uint16_t>(gloves, offsets_.itemDefIndex, glove.definition);
		NotifyField(pawn, offsets_.pawnEconGloves + offsets_.itemDefIndex, offsets_.pawnChain);
		SetPaintAttributes(gloves, glove);
		if (offsets_.itemInitialized >= 0) {
			WriteMem<std::uint8_t>(gloves, offsets_.itemInitialized, 1);
			NotifyField(pawn, offsets_.pawnEconGloves + offsets_.itemInitialized, offsets_.pawnChain);
		}
		// The item is written: announce its attribute lists (the paint writes), then move the counter
		// the client rebuilds the glove model from (the live-tested NotifyEmoteGlovesChanged recipe of
		// ref/src/cs2-fortnite_emotes/weapons_gloves.cpp:76-88).
		NotifyAttributeLists(pawn, offsets_.pawnEconGloves, offsets_.pawnChain);
		BumpEconGlovesChanged(pawn);
		applied.glove[static_cast<std::size_t>(teamIndex)] = true;
		++stats_.gloveApplied;

		// The reference's two bodygroup phases (HookInventoryUpdateService.cs:601-605): off now, on
		// again 0.2 s later so the client rebuilds the glove model. The deferred phase carries the
		// same identity guard as every other deferred pass (it must not fire for a new pawn).
		// The handle is the controller's m_hPlayerPawn through PawnHandle: with the pointer
		// conversion (the live build refuses pawn pointers) the bodygroup phases never ran.
		const int32_t pawnHandle = PawnHandle(slot);
		if (!IsValidHandle(pawnHandle)) {
			++stats_.gloveNoHandle;
			return;
		}
		InputString(pawnHandle, kInputSetBodygroup, kGloveBodygroupOff);
		const std::weak_ptr<EngineToken> weak = token_;
		ScheduleOnce(kGlovePhaseDelaySeconds, [this, weak, guard]() {
			if (!TokenAlive(weak)) {
				return; // the engine is gone: no member may be touched from here
			}
			if (!GuardValid(guard)) {
				return; // a different pawn, team, player or map
			}
			const int32_t handle = PawnHandle(guard.slot);
			if (IsValidHandle(handle)) {
				InputString(handle, kInputSetBodygroup, kGloveBodygroupOn);
			} else {
				++stats_.gloveNoHandle; // the pawn handle is gone between the two phases
			}
		});
	}

	// m_nEconGlovesChanged (uint8) is the change signal the client rebuilds the glove model from, so
	// every m_EconGloves write - the apply and the reset - bumps it and announces it, exactly like
	// NotifyEmoteGlovesChanged (ref/src/cs2-fortnite_emotes/weapons_gloves.cpp:76-88). The increment
	// wraps at 255 like the reference's (uint8)(changed + 1). A build whose schema does not expose
	// the field keeps the previous behaviour (econGlovesChanged_ stays -1).
	void BumpEconGlovesChanged(void* pawn) {
		if (econGlovesChanged_ < 0) {
			return;
		}
		const std::uint8_t changed = ReadMem<std::uint8_t>(pawn, econGlovesChanged_);
		WriteMem<std::uint8_t>(pawn, econGlovesChanged_, static_cast<std::uint8_t>(changed + 1));
		NotifyField(pawn, econGlovesChanged_, offsets_.pawnChain);
	}

	void ApplyAgent(int slot, const TeamItems& items, std::size_t teamIndex, const DeferredGuard& guard) {
		void* pawn = FreshPawn(slot);
		if (pawn == nullptr) {
			return;
		}
		if (__s2sdk_GetEntityModel == nullptr || __s2sdk_SetClientModel == nullptr) {
			++stats_.agentNoBinding; // the model can be neither read nor written
			return;
		}
		// The handle every model call needs, from the controller's m_hPlayerPawn (PawnHandle): the
		// pointer conversion refused the pawn on the live build, so this exit is where the whole
		// agent path died (agentApplied=27, agentWritten=0).
		const int32_t handle = PawnHandle(slot);
		if (!IsValidHandle(handle)) {
			++stats_.agentNoHandle;
			return;
		}
		std::string current;
		try {
			current = ToString(s2sdk::GetEntityModel(handle));
		} catch (...) {
			++stats_.agentNoModel;
			return;
		}
		if (current.empty()) {
			++stats_.agentNoModel;
			return;
		}

		SlotState& state = slots_[static_cast<std::size_t>(slot)];
		if (state.steamId != guard.steamId) {
			state = SlotState{};
			state.steamId = guard.steamId;
		}
		std::string& defaultModel = state.defaultModel[teamIndex];
		if (defaultModel.empty()) {
			// CaptureDefaultModel of the reference: the first model seen for this player and team is
			// the default a removed agent configuration resets to (TryAdd, first wins).
			defaultModel = current;
		}

		const EconDefinition* agent = items.agent != 0 ? econ_.Agent(items.agent) : nullptr;
		const std::string* target = nullptr;
		if (agent != nullptr && !agent->model.empty()) {
			target = &agent->model;
		} else if (defaultModel != current) {
			target = &defaultModel;
		}
		if (target == nullptr) {
			if (items.agent != 0) {
				++stats_.agentNoCatalog; // configured on the site, absent from items_game
			} else {
				++stats_.agentNoData; // nothing configured for this team
			}
			return;
		}

		// The reference's refresh trick: switch to another agent model and back first, so the client
		// reloads the skeleton, then set the wanted model on the next tick.
		if (const EconDefinition* refresh = RefreshModel(current, *target); refresh != nullptr) {
			SetModel(slot, refresh->model);
			SetModel(slot, current);
		}
		const std::string wanted = *target; // copied: the deferred callback outlives this scope
		const std::weak_ptr<EngineToken> weak = token_;
		DeferNextFrame([this, weak, guard, wanted]() {
			if (!TokenAlive(weak)) {
				return; // the engine is gone: no member may be touched from here
			}
			// This is the only write of the wanted model, so a cancelled pass leaves the default model
			// on silently - counted, to tell that apart from a write the game later reverts.
			if (!GuardValid(guard)) {
				++stats_.agentDeferDropped; // a different pawn, team, player or map
				return;
			}
			SetModel(guard.slot, wanted);
			++stats_.agentWritten;
		});
		++stats_.agentApplied; // scheduled; agentWritten counts the writes that actually happened
	}

	const EconDefinition* RefreshModel(const std::string& current, const std::string& target) const {
		for (const auto& entry : econ_.agents) {
			const EconDefinition& agent = entry.second;
			if (agent.model.empty() || agent.model == current || agent.model == target) {
				continue;
			}
			return &agent;
		}
		return nullptr;
	}

	void SetModel(int slot, const std::string& model) {
		if (__s2sdk_SetClientModel == nullptr || model.empty()) {
			return;
		}
		try {
			s2sdk::SetClientModel(slot, plg::string(model));
		} catch (...) {
		}
	}

	// Music kit: the controller's int32 and the inventory component's uint16, both notified - the
	// reference wrote both (HookInventoryUpdateService.cs:609-621). Production keeps the MVP replay
	// off; that gate belongs to the runtime, this is only the item state.
	void ApplyMusic(int slot, const Snapshot& snapshot) {
		void* controller = FreshController(slot);
		if (controller == nullptr) {
			return;
		}
		if (offsets_.controllerMusicKit >= 0) {
			WriteMem<int32_t>(controller, offsets_.controllerMusicKit, static_cast<int32_t>(snapshot.music));
			NotifyField(controller, offsets_.controllerMusicKit, offsets_.controllerChain);
		}
		void* services = ReadInventoryServices(controller);
		if (services != nullptr && offsets_.inventoryMusicId >= 0) {
			// The component field is a uint16; the reference's (ushort) cast is kept but clamped to
			// the field instead of wrapping an absurd value around.
			const int clamped = snapshot.music < 0 ? 0 : (snapshot.music > 0xFFFF ? 0xFFFF : snapshot.music);
			WriteMem<uint16_t>(services, offsets_.inventoryMusicId, static_cast<uint16_t>(clamped));
			NotifyField(services, offsets_.inventoryMusicId, offsets_.inventoryChain);
		}
		if (snapshot.music > 0) {
			++stats_.musicApplied;
		}
	}

	// Pins: m_rank[5] on the inventory services, only when the production flag is on
	// (Main.Pisex.PinsEnabled).
	void ApplyPin(int slot, const TeamItems& items) {
		if (!config_.pisex.pinsEnabled || offsets_.inventoryRank < 0) {
			return;
		}
		void* controller = FreshController(slot);
		void* services = controller != nullptr ? ReadInventoryServices(controller) : nullptr;
		if (services == nullptr) {
			return;
		}
		const int offset = offsets_.inventoryRank + kRankPinIndex * kWidthRank;
		WriteMem<int32_t>(services, offset, static_cast<int32_t>(items.pin));
		// m_rank is a networked fixed array: the engine resolves a change by the field's own offset,
		// not by an element's ("Couldn't resolve offset 88 ... path (2 = '15')" live for m_rank[5]).
		NotifyField(services, offsets_.inventoryRank, offsets_.inventoryChain);
		++stats_.pinApplied;
	}

	// --- weapon/knife application and regive ---------------------------------------------------------

	// Applies the snapshot's weapon skins to the weapons the player is holding. The knife is left to
	// ApplyKnife (which also owns the regive), so a knife is never regiven twice.
	//
	// `regive` is the !wp path: besides the weapons that have a skin in the snapshot, every weapon
	// this engine skinned before but that the site no longer configures is regiven too - the fresh
	// copy comes back with the game's default attributes, so a removal on the website cannot leave a
	// stale paint/sticker/StatTrak behind. Nothing else is ever regiven (a gun this engine never
	// touched keeps its state; the C4 and grenades can never be caught by this).
	void ApplyHeldWeapons(int slot, const TeamItems& items, bool regive, const DeferredGuard& guard) {
		if (__s2sdk_GetClientWeapons == nullptr || !HasWeaponServices(slot)) {
			return; // no pawn / no weapon services: the player holds nothing to apply to
		}
		plg::vector<int32_t> handles;
		try {
			handles = s2sdk::GetClientWeapons(slot);
		} catch (...) {
			return;
		}
		// Every handle is resolved and validated in this same stack, and the weapon must still
		// belong to this player's pawn: a handle that was dropped, picked up by somebody else or
		// left over from a previous round must never be written through. PawnHandle, not the pawn
		// pointer: m_hOwnerEntity holds a handle, so both sides must come from the same kind of
		// reading (and the pointer conversion refuses pawns on the live build anyway).
		const int32_t ownerHandle = PawnHandle(slot);
		AppliedState& applied = AppliedFor(slot, guard);
		for (const int32_t handle : handles) {
			if (!IsValidHandle(handle)) {
				continue;
			}
			void* weapon = PointerFromHandle(handle);
			WeaponView view;
			if (weapon == nullptr || !MakeWeaponView(weapon, view)) {
				continue;
			}
			if (IsValidHandle(ownerHandle) && ReadMem<int32_t>(view.weapon, offsets_.weaponOwner) != ownerHandle) {
				continue; // not this player's weapon any more
			}
			const uint16_t definition = ReadMem<uint16_t>(view.item, offsets_.itemDefIndex);
			if (IsKnife(definition)) {
				continue;
			}
			const auto it = items.weapons.find(definition);
			if (it != items.weapons.end()) {
				ApplyGunFields(view, it->second);
				++stats_.gunInPlace;
				MarkWeaponApplied(applied, guard.team, definition);
				if (regive) {
					RegiveWeapon(slot, handle, definition);
				}
				continue;
			}
			if (regive && WasWeaponApplied(applied, guard.team, definition)) {
				// The site dropped this skin: hand the weapon back to the game (default look).
				UntrackWeapon(applied, guard.team, definition);
				RegiveWeapon(slot, handle, definition);
			}
		}
	}

	// The knife. Two differences from the gun path, both from the removal case:
	//   * a knife the site no longer configures is still regiven on !wp so it comes back default;
	//   * a player who holds no knife is never given one here (the engine hands out the default
	//     knife at spawn; this path must not create items).
	void ApplyKnife(int slot, const TeamItems& items, bool regive, const DeferredGuard& guard) {
		void* weapon = FindKnife(slot);
		if (weapon != nullptr) {
			// Same-stack ownership check (the knife could be a stale handle from a drop).
			const int32_t ownerHandle = PawnHandle(slot);
			if (IsValidHandle(ownerHandle) && ReadMem<int32_t>(weapon, offsets_.weaponOwner) != ownerHandle) {
				weapon = nullptr;
			}
		}
		const int teamIndex = TeamIndex(guard.team);
		AppliedState& applied = AppliedFor(slot, guard);
		const bool hadOurSkin = teamIndex >= 0 && applied.knife[static_cast<std::size_t>(teamIndex)];
		if (items.knife.has_value()) {
			if (weapon != nullptr) {
				WeaponView view;
				if (MakeWeaponView(weapon, view)) {
					ApplyKnifeFields(view, *items.knife);
					++stats_.knifeApplied;
					if (teamIndex >= 0) {
						applied.knife[static_cast<std::size_t>(teamIndex)] = true;
					}
				}
			}
		}
		// RegiveKnife of PlayerExtensions.cs: remove by gear slot, give weapon_knife, select it.
		// Regiven when the knife changed on the site *or* when this engine had put a skin on it and
		// the site no longer configures one (the removal reset - the fresh knife keeps the game's
		// default look, and the hook re-applies a skin only if the snapshot still has one).
		if (regive && weapon != nullptr && (items.knife.has_value() || hadOurSkin)) {
			if (teamIndex >= 0) {
				applied.knife[static_cast<std::size_t>(teamIndex)] = items.knife.has_value();
			}
			RegiveKnife(slot);
		}
	}

	void* FindKnife(int slot) const {
		if (__s2sdk_GetClientWeapons == nullptr || __s2sdk_GetWeaponGearSlot == nullptr ||
			!HasWeaponServices(slot)) {
			return nullptr;
		}
		try {
			for (const int32_t handle : s2sdk::GetClientWeapons(slot)) {
				if (IsValidHandle(handle) && s2sdk::GetWeaponGearSlot(handle) == s2sdk::GearSlot::Knife) {
					return PointerFromHandle(handle);
				}
			}
		} catch (...) {
		}
		return nullptr;
	}

	void RegiveKnife(int slot) {
		if (__s2sdk_GetClientWeapons == nullptr || __s2sdk_GiveNamedItem == nullptr ||
			!HasWeaponServices(slot)) {
			return; // the held knife can only be enumerated through the weapon services
		}
		try {
			int32_t oldHandle = 0;
			if (__s2sdk_GetWeaponGearSlot != nullptr) {
				for (const int32_t handle : s2sdk::GetClientWeapons(slot)) {
					if (IsValidHandle(handle) && s2sdk::GetWeaponGearSlot(handle) == s2sdk::GearSlot::Knife) {
						oldHandle = handle;
						break;
					}
				}
			}
			if (!IsValidHandle(oldHandle)) {
				return; // no knife held: this path replaces an item, it never creates one
			}
			if (__s2sdk_RemoveWeapon != nullptr) {
				s2sdk::RemoveWeapon(slot, oldHandle);
			}
			const int32_t newHandle = s2sdk::GiveNamedItem(slot, plg::string(kKnifeClassname));
			if (IsValidHandle(newHandle) && __s2sdk_SelectWeapon != nullptr) {
				s2sdk::SelectWeapon(slot, newHandle);
			}
			// The fresh knife's skin is applied by our own GiveNamedItem post hook.
		} catch (...) {
		}
	}

	// RegiveWeapon/RegiveTaser of PlayerExtensions.cs: keep the ammo, and for the taser the attack
	// timing. The handle and the definition index are re-read at entry: the caller's handle may be a
	// frame old.
	void RegiveWeapon(int slot, int32_t handle, uint16_t definition) {
		if (__s2sdk_GiveNamedItem == nullptr || __s2sdk_RemoveWeapon == nullptr) {
			return;
		}
		void* weapon = PointerFromHandle(handle);
		WeaponView view;
		if (weapon == nullptr || !MakeWeaponView(weapon, view)) {
			return;
		}
		if (ReadMem<uint16_t>(view.item, offsets_.itemDefIndex) != definition) {
			return; // a different weapon lives in that handle now
		}
		const ItemDefinition* item = econ_.Item(definition);
		std::string classname = item != nullptr ? item->name : std::string();
		if (classname.empty() && __s2sdk_GetEntityClassname != nullptr) {
			try {
				classname = ToString(s2sdk::GetEntityClassname(handle));
			} catch (...) {
				classname.clear();
			}
		}
		if (classname.empty()) {
			return;
		}

		const bool taser = classname == kTaserClassname;
		const int32_t clip = offsets_.weaponClip >= 0 ? ReadMem<int32_t>(view.weapon, offsets_.weaponClip) : 0;
		// Both inline reserve values are preserved (the reference copied slot 0 only; the field is a
		// two-element array and the fresh weapon starts from zeros).
		std::array<int32_t, kReserveAmmoElements> reserve{};
		bool hasReserve = true;
		for (int i = 0; i < kReserveAmmoElements; ++i) {
			if (!ReadReserveAmmo(view, i, reserve[static_cast<std::size_t>(i)])) {
				hasReserve = false;
				break;
			}
		}
		float fireTime = 0.0f;
		int32_t lastAttackTick = 0;
		if (taser) {
			if (offsets_.taserFireTime >= 0) {
				fireTime = ReadMem<float>(view.weapon, offsets_.taserFireTime);
			}
			if (offsets_.taserLastAttackTick >= 0) {
				lastAttackTick = ReadMem<int32_t>(view.weapon, offsets_.taserLastAttackTick);
			}
		}

		try {
			s2sdk::RemoveWeapon(slot, handle);
			const int32_t newHandle = s2sdk::GiveNamedItem(slot, plg::string(classname));
			if (!IsValidHandle(newHandle)) {
				return;
			}
			void* fresh = PointerFromHandle(newHandle);
			WeaponView freshView;
			if (fresh == nullptr || !MakeWeaponView(fresh, freshView)) {
				return;
			}
			if (offsets_.weaponClip >= 0) {
				WriteMem<int32_t>(freshView.weapon, offsets_.weaponClip, clip);
			}
			if (hasReserve) {
				for (int i = 0; i < kReserveAmmoElements; ++i) {
					WriteReserveAmmo(freshView, i, reserve[static_cast<std::size_t>(i)]);
				}
			}
			if (taser) {
				if (offsets_.taserFireTime >= 0) {
					WriteMem<float>(freshView.weapon, offsets_.taserFireTime, fireTime);
				}
				if (offsets_.taserLastAttackTick >= 0) {
					WriteMem<int32_t>(freshView.weapon, offsets_.taserLastAttackTick, lastAttackTick);
				}
			}
		} catch (...) {
		}
	}

	// m_pReserveAmmo is a fixed int32[2] inside the weapon (dump: fixed_array, 2 x 4 bytes; fixture
	// note "not a pointer despite its m_p spelling"). Both values are read and written in place; no
	// pointer is loaded and nothing is dereferenced, so the *(int32_t**)(weapon + off) pattern of
	// plugins/vip_infiniteammo must NOT be copied here.
	bool ReadReserveAmmo(const WeaponView& view, int index, int32_t& out) const {
		if (offsets_.weaponReserveAmmo < 0 || index < 0 || index >= detail::kReserveAmmoElements) {
			return false;
		}
		out = detail::ReadReserveAmmoValue(view.weapon, offsets_.weaponReserveAmmo, index);
		return true;
	}

	bool WriteReserveAmmo(const WeaponView& view, int index, int32_t value) {
		if (offsets_.weaponReserveAmmo < 0 || index < 0 || index >= detail::kReserveAmmoElements) {
			return false;
		}
		detail::WriteReserveAmmoValue(view.weapon, offsets_.weaponReserveAmmo, index, value);
		return true;
	}

	// --- notifications, inputs, deferred work ---------------------------------------------------------

	void NotifyField(void* entity, int offset, int chain) {
		if (__s2sdk_ChangeEntityState2 == nullptr || entity == nullptr || offset < 0) {
			return;
		}
		try {
			s2sdk::ChangeEntityState2(entity, offset, chain);
		} catch (...) {
		}
	}

	// The client renders the skin from the item view inside m_AttributeManager, and the native
	// attribute writer (SetOrAddAttributeValueByName) changes its attribute lists without marking any
	// network state - the reference's writes went through SwiftlyS2's setters, which did. Announce the
	// lists after the writes: without it an in-place apply (pickup, !wp, a load landing on a live
	// player) never reaches the client. A freshly given weapon carries the writes in its first
	// snapshot either way.
	void NotifyItemViewChanged(const WeaponView& view) {
		NotifyAttributeLists(view.weapon, view.itemOffset, offsets_.weaponChain);
	}

	// The two networked attribute vectors of an inline CEconItemView at itemOffset inside entity:
	// m_AttributeList.m_Attributes and m_NetworkedDynamicAttributes.m_Attributes. Only networked
	// leaf fields may be announced - announcing a container (m_AttributeManager, m_EconGloves) makes
	// the engine print "requested resolve all N changes, actually resolved only N-1" followed by the
	// entity's whole send table on every apply (seen live on 27035).
	// A list whose vector header is not plausible is skipped, like ClearAttributeLists does.
	void NotifyAttributeLists(void* entity, int itemOffset, int chain) {
		if (entity == nullptr || offsets_.attributeVector < 0) {
			return;
		}
		auto* item = static_cast<std::uint8_t*>(entity) + itemOffset;
		for (const int listOffset : {offsets_.itemAttributeList, offsets_.itemNetworkedAttributes}) {
			if (listOffset < 0 || !IsAttributeHeaderPlausible(item + listOffset, offsets_.attributeVector)) {
				continue;
			}
			NotifyField(entity, itemOffset + listOffset + offsets_.attributeVector, chain);
		}
	}

	// The two cosmetic entity inputs the reference issues from its apply paths. Activator and caller
	// are the entity itself, exactly like the reference's AcceptInput(name, value) call.
	void InputString(int32_t entityHandle, const char* inputName, const char* value) {
		if (__s2sdk_AcceptEntityInput == nullptr || !IsValidHandle(entityHandle)) {
			return;
		}
		try {
			s2sdk::AcceptEntityInput(entityHandle, plg::string(inputName), entityHandle, entityHandle,
									 plg::any(plg::string(value)), s2sdk::FieldType::String, 0);
		} catch (...) {
		}
	}

	void DeferNextFrame(std::function<void()> fn) { pisex::NextFrame(std::move(fn)); }

	// Drops the record of a timer that has fired. Called from inside its own callback, before the
	// callback returns -1: pisexcompat stops the timer then and frees its CTimer at the next Poll,
	// so from that point on the pointer must never be touched again (not even IsAlive()).
	void ReleaseTimer(uint64_t id) {
		for (auto it = timers_.begin(); it != timers_.end(); ++it) {
			if (it->id == id) {
				timers_.erase(it);
				return;
			}
		}
	}

	// Schedules one delayed pass. The timer is tracked by a stable id; the callback releases its own
	// record before the function body runs, so a fired timer is never in the bookkeeping again and
	// the bookkeeping only ever holds timers that are still pending (and therefore alive).
	void ScheduleOnce(float delaySeconds, std::function<void()> fn) {
		if (__s2sdk_CreateTimer == nullptr) {
			return;
		}
		if (timers_.size() >= kMaxPendingTimers) {
			return; // bounded: a burst may skip a cosmetic re-apply, the immediate pass already ran
		}
		const uint64_t id = nextTimerId_++;
		const std::weak_ptr<EngineToken> weak = token_;
		pisex::CTimer* timer = pisex::CreateTimer(delaySeconds, [this, weak, id, fn = std::move(fn)]() -> float {
			if (weak.lock() == nullptr) {
				return -1.0f; // the engine is gone: stop, touch nothing (this is only reachable if a
							  // pending timer somehow survived Shutdown's RemoveTimer loop)
			}
			ReleaseTimer(id);
			fn();
			return -1.0f; // stop after one run: the reference's single delayed callback
		});
		if (timer == nullptr) {
			return;
		}
		timers_.push_back(PendingTimer{id, timer});
	}

	// --- state ---------------------------------------------------------------------------------------

	const Config& config_;
	const EconData& econ_;
	Offsets offsets_{};
	// CCSPlayerPawn::m_nEconGlovesChanged (uint8), resolved in Prepare alongside the Offsets table;
	// kept out of that table because it is optional (see Prepare) and only the glove path reads it.
	int econGlovesChanged_ = -1;
	bool econGlovesChangedLogged_ = false; // the missing-field warning is emitted once per engine
	// The two optional fields of the owner resolution (see Prepare): the pawn pointer leading the
	// CPlayerPawnComponent chainer of the services argument, and the controller's pawn handle.
	// Kept here, not in the Offsets table, because both have a fallback and only the hook / the
	// pawn-handle reads use them.
	int servicesPawn_ = -1;
	int controllerPlayerPawn_ = -1;
	bool optionalOffsetsLogged_ = false; // the missing-field warning is emitted once per engine
	SetAttributeFn setAttribute_ = nullptr;
	s2sdk::ConfigId gamedata_ = 0;
	CacheLookup lookup_;
	polyhook::HookHandle hook_ = nullptr;
	void* hookTarget_ = nullptr;
	bool hookInstalled_ = false;
	// The lifetime of every deferred callback: token_ is owned here, captured as a weak_ptr.
	std::shared_ptr<EngineToken> token_ = std::make_shared<EngineToken>();
	uint64_t nextTimerId_ = 1; // stable timer ids (0 is a valid s2sdk id, so start at 1)
	std::vector<PendingTimer> timers_;
	std::array<SlotState, kMaxPlayers> slots_{};
	// What this engine has written for each player (see AppliedState): the input of the removals
	// cleanup on !wp and of the glove reset.
	std::array<AppliedState, kMaxPlayers> applied_{};
	// Diagnostic counters (engine.hpp), dumped by the runtime's ws_status.
	ApplyStats stats_{};
};

// -------------------------------------------------------------------------------------------------
// The public surface: thin forwarding, nothing else lives here.
// -------------------------------------------------------------------------------------------------
Engine::Engine(const Config& config, const EconData& econ) : impl_(std::make_unique<Impl>(config, econ)) {}

Engine::~Engine() {
	if (impl_) {
		impl_->Shutdown();
	}
}

bool Engine::Prepare(std::string& error) { return impl_->Prepare(error); }

bool Engine::InstallHook(const std::filesystem::path& gamedata, CacheLookup lookup, std::string& error) {
	return impl_->InstallHook(gamedata, std::move(lookup), error);
}

void Engine::Shutdown() { impl_->Shutdown(); }

void Engine::ApplySpawn(int slot, const Snapshot& snapshot, bool refresh) {
	impl_->ApplySpawn(slot, snapshot, refresh);
}

void Engine::RefreshInventory(int slot, const Snapshot& snapshot) { impl_->RefreshInventory(slot, snapshot); }

void Engine::ApplyOwnedWeapons(int slot, const Snapshot& snapshot) {
	impl_->ApplyOwnedWeapons(slot, snapshot);
}

int Engine::ActiveDefinition(int slot) const { return impl_->ActiveDefinition(slot); }

void Engine::SetStatTrak(int slot, const Skin& skin) { impl_->SetStatTrak(slot, skin); }

bool Engine::ApplyWeapon(int handle, const Skin& skin, bool knife) {
	return impl_->ApplyWeaponByHandle(handle, skin, knife);
}

const Engine::ApplyStats& Engine::Stats() const { return impl_->Stats(); }

Engine::PawnHandles Engine::PawnHandleReport(int slot) const { return impl_->PawnHandleReport(slot); }

} // namespace ws
