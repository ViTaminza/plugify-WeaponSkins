// Tests for plugins/WeaponSkins/src/engine.cpp — the hot GiveNamedItem path and the apply paths.
//
// The engine runs against a *fake engine*: the generated s2sdk and polyhook globals (plain non-const
// pointers in sdk/include/{s2sdk,polyhook}/*.cpp) are assigned fake implementations, so the real
// engine code runs end to end without a server (the technique
// libs/pisexcompat/test/timers_engine_test.cpp established). The fake provides:
//   * entities as byte buffers with a handle <-> pointer map and a schema offset table pointing
//     into them, so every raw write lands in inspectable memory (sentinel byte 0xAA everywhere);
//   * a recording native CAttributeList::SetOrAddAttributeValueByName, resolved through the fake
//     gamedata exactly like the real one, so attribute names and values are assertable;
//   * a recording polyhook detour table, so the shared-detour teardown rules are assertable;
//   * recording timers plus pisex::Poll(), so the deferred glove/agent passes are drivable.
//
// Covered: the detour's declared ABI, the owner/bot/map/stale-cache guards (the owner from the
// hook's services argument with its owner-chain fallback), the controller pawn handle with the
// refused pointer conversion, every raw field width (sentinel bytes around each write), the
// attribute names and the int-bit encodings (StatTrak, sticker ids/schemas, keychain id/seed), the
// 161-byte nametag bound, the sticker slot table with the schema-1337 rule, the knife ChangeSubclass
// and the gated vector clear, the glove / music / pin / agent paths, the regive ammo restore
// (including the page-guarded reserve-ammo pointer), and the teardown order of the shared detour.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "engine.hpp"
#include "engine_detail.hpp"

#include <pisex/compat.hpp>
#include <pisex/timers.hpp>
#include <plg/plugin.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL [%.*s]: %s\n", static_cast<int>(g_section.size()), g_section.data(), what.c_str());
	}
}

void CheckEq(long long actual, long long expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL [%.*s]: %s (expected %lld, got %lld)\n", static_cast<int>(g_section.size()),
					g_section.data(), what.c_str(), expected, actual);
	}
}

void CheckFloat(float actual, float expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL [%.*s]: %s (expected %g, got %g)\n", static_cast<int>(g_section.size()),
					g_section.data(), what.c_str(), static_cast<double>(expected), static_cast<double>(actual));
	}
}

void CheckStr(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL [%.*s]: %s (expected '%.*s', got '%.*s')\n", static_cast<int>(g_section.size()),
					g_section.data(), what.c_str(), static_cast<int>(expected.size()), expected.data(),
					static_cast<int>(actual.size()), actual.data());
	}
}

// =================================================================================================
// The fake world: memory layout and entities
// =================================================================================================
constexpr std::uint8_t kSentinel = 0xAA; // "no write may ever land here"
constexpr int kBufferSize = 2048;

constexpr int kWeaponOwner = 0x10;			  // CBasePlayerWeapon::m_hOwnerEntity (4)
constexpr int kWeaponClip = 0x30;			  // m_iClip1 (4)
constexpr int kWeaponReserveAmmo = 0x38;	  // m_pReserveAmmo: int32[2] INLINE (8 bytes, no pointer)
constexpr int kWeaponAttributeManager = 0x100; // m_AttributeManager (inline)
constexpr int kTaserFireTime = 0x130;		  // CWeaponTaser::m_fFireTime (4)
constexpr int kTaserLastAttack = 0x134;		  // m_nLastAttackTick (4)
constexpr int kContainerItem = 0x40;		  // CAttributeContainer::m_Item (inline)
constexpr int kItemOffset = kWeaponAttributeManager + kContainerItem; // item view at 0x140

constexpr int kItemDefIndex = 0x08;				   // uint16
constexpr int kItemQuality = 0x0C;				   // int32
constexpr int kItemInitialized = 0x10;			   // bool (no overlap with the int32 above)
constexpr int kItemCustomName = 0x20;			   // char[161]
constexpr int kItemAttributeList = 0x200;		   // inline CAttributeList
constexpr int kItemNetworkedAttributes = 0x280;	   // inline CAttributeList
constexpr int kAttributeVector = 8;				   // CAttributeList::m_Attributes
// The real header layout of the 104-byte CUtlVectorEmbeddedNetworkVar (fixture + disassembly):
// count (int32) at +0, data pointer at +8, capacity (int32) at +16 - relative to the object.
constexpr int kAttributeCountOffset = 0;
constexpr int kAttributeDataOffset = 8;
constexpr int kAttributeCapacityOffset = 16;

constexpr int kPawnController = 0x20;		// CBasePlayerPawn::m_hController (4)
constexpr int kPawnWeaponServices = 0x28;	// m_pWeaponServices (8, pointer)
constexpr int kPawnEconGloves = 0x400;		// CCSPlayerPawn::m_EconGloves (inline item view)
// m_nEconGlovesChanged (uint8) sits directly behind the 680-byte CEconItemView, exactly like the
// real schema places it (sdk.json: m_EconGloves at 6720 size 680, m_nEconGlovesChanged at 7400).
constexpr int kPawnEconGlovesChanged = kPawnEconGloves + 680;

constexpr int kServicesActive = 0x40;		// CPlayer_WeaponServices::m_hActiveWeapon (4)
constexpr int kControllerMusicKit = 0x30;	// CCSPlayerController::m_iMusicKitID (4)
constexpr int kControllerInventory = 0x38;	// m_pInventoryServices (8, pointer)
constexpr int kControllerPlayerPawn = 0x48; // CCSPlayerController::m_hPlayerPawn (CHandle, 4)
constexpr int kServicesPawn = 0x08;			// CPlayerPawnComponent::__m_pChainEntity's leading pawn ptr (8)
constexpr int kInventoryMusicId = 0x40;		// m_unMusicID (2)
constexpr int kInventoryRank = 0x48;		// m_rank (6 x int32)

struct FakeEntity {
	std::string classname;
	std::string model;
	std::array<std::uint8_t, kBufferSize> memory{};
	int handle = 0;
	int gearSlot = 0;
};

std::vector<FakeEntity*> g_entities;
int g_nextEntityIndex = 1;

FakeEntity* NewEntity(const std::string& classname) {
	auto* entity = new FakeEntity();
	entity->classname = classname;
	entity->memory.fill(kSentinel);
	entity->handle = 0x8000 | g_nextEntityIndex++; // index | serial << 15
	g_entities.push_back(entity);
	return entity;
}

void ResetEntities() {
	for (FakeEntity* entity : g_entities) {
		delete entity;
	}
	g_entities.clear();
	g_nextEntityIndex = 1;
}

FakeEntity* FromHandle(int32_t handle) {
	// A valid handle is any int32 except -1 (INVALID_EHANDLE_INDEX) and 0 (never handed out); the
	// serial in the high bits makes live handles negative once the global counter passes 65535.
	for (FakeEntity* entity : g_entities) {
		if (handle != -1 && handle != 0 && entity->handle == handle) {
			return entity;
		}
	}
	return nullptr;
}

FakeEntity* FromPointer(const void* pointer) {
	for (FakeEntity* entity : g_entities) {
		if (pointer != nullptr && entity->memory.data() == pointer) {
			return entity;
		}
	}
	return nullptr;
}

void* PointerOf(FakeEntity* entity) { return entity != nullptr ? entity->memory.data() : nullptr; }

template <typename T>
void Poke(FakeEntity* entity, int offset, T value) {
	std::memcpy(entity->memory.data() + offset, &value, sizeof(T));
}

template <typename T>
T Peek(const FakeEntity* entity, int offset) {
	T value{};
	std::memcpy(&value, entity->memory.data() + offset, sizeof(T));
	return value;
}

std::uint8_t PeekByte(const FakeEntity* entity, int offset) { return entity->memory[offset]; }

// The same reads at a raw address (the inline attribute-list objects the engine writes through).
template <typename T>
T PeekRaw(const void* base, int offset) {
	T value{};
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
	return value;
}

std::uint8_t PeekRawByte(const void* base, int offset) {
	return static_cast<const std::uint8_t*>(base)[offset];
}

template <typename T>
void PokeRaw(void* base, int offset, T value) {
	std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
}

// --- fake player slots ---------------------------------------------------------------------------
struct FakeSlot {
	bool connected = false;
	bool bot = false;
	uint64_t steamId = 0;
	int team = 2; // CSTeam::T
	FakeEntity* controller = nullptr;
	FakeEntity* pawn = nullptr;
	FakeEntity* weaponServices = nullptr;
	FakeEntity* inventoryServices = nullptr;
	FakeEntity* itemServices = nullptr; // the hook's first argument (CCSPlayer_ItemServices)
	std::vector<int32_t> weapons; // all weapons, the knife included
	std::vector<int32_t> reserved; // not a knife
};

std::array<FakeSlot, 64> g_slots;

void ResetSlots() { g_slots = {}; }

void MakePlayer(int slot, uint64_t steamId, bool bot = false, int team = 2) {
	FakeSlot& state = g_slots[static_cast<std::size_t>(slot)];
	state.connected = true;
	state.bot = bot;
	state.steamId = steamId;
	state.team = team;
	state.controller = NewEntity("CCSPlayerController");
	state.pawn = NewEntity("CCSPlayerPawn");
	state.weaponServices = NewEntity("CCSPlayer_WeaponServices");
	state.inventoryServices = NewEntity("CCSPlayerController_InventoryServices");
	state.itemServices = NewEntity("CCSPlayer_ItemServices");
	Poke<int32_t>(state.pawn, kPawnController, state.controller->handle);
	Poke<void*>(state.pawn, kPawnWeaponServices, PointerOf(state.weaponServices));
	Poke<void*>(state.controller, kControllerInventory, PointerOf(state.inventoryServices));
	// CPlayerPawnComponent::__m_pChainEntity: the chainer's leading pointer is the owning pawn.
	Poke<void*>(state.itemServices, kServicesPawn, PointerOf(state.pawn));
	// The controller's current pawn handle: the source PawnHandle reads (the live-build fix).
	Poke<int32_t>(state.controller, kControllerPlayerPawn, state.pawn->handle);
	// The glove item's two attribute vectors start empty (count 0, capacity 0, no data), like on a
	// fresh pawn - the engine announces a list only when its header is plausible.
	for (const int list : {kItemAttributeList, kItemNetworkedAttributes}) {
		const int vector = kPawnEconGloves + list + kAttributeVector;
		Poke<int32_t>(state.pawn, vector + kAttributeCountOffset, 0);
		Poke<void*>(state.pawn, vector + kAttributeDataOffset, nullptr);
		Poke<int32_t>(state.pawn, vector + kAttributeCapacityOffset, 0);
	}
}

FakeEntity* AddWeapon(int slot, uint16_t definition, const std::string& classname, int gearSlot) {
	FakeEntity* weapon = NewEntity(classname);
	weapon->gearSlot = gearSlot;
	FakeSlot& state = g_slots[static_cast<std::size_t>(slot)];
	Poke<int32_t>(weapon, kWeaponOwner, state.pawn != nullptr ? state.pawn->handle : -1);
	Poke<uint16_t>(weapon, kItemOffset + kItemDefIndex, definition);
	weapon->memory[kItemOffset + kItemDefIndex + 2] = kSentinel; // the byte after the uint16
	// m_pReserveAmmo stays the 0xAA sentinel pattern: it is an inline int32[2] the engine must treat
	// as data, never as a pointer (a mis-read pointer would be a wild address in every test).
	state.weapons.push_back(weapon->handle);
	return weapon;
}

// Sets the inline int32[2] of m_pReserveAmmo (with sentinels directly before and after the field).
void SetReserveAmmo(FakeEntity* weapon, std::int32_t first, std::int32_t second) {
	Poke<std::int32_t>(weapon, kWeaponReserveAmmo, first);
	Poke<std::int32_t>(weapon, kWeaponReserveAmmo + 4, second);
	weapon->memory[kWeaponReserveAmmo - 1] = kSentinel;
	weapon->memory[kWeaponReserveAmmo + 8] = kSentinel;
}

std::int32_t ReserveAmmoAt(const FakeEntity* weapon, int index) {
	return Peek<std::int32_t>(weapon, kWeaponReserveAmmo + index * 4);
}

// --- fake schema offsets -------------------------------------------------------------------------
std::string g_hiddenSchemaField; // non-empty: that field resolves to -1 (the missing-field path)

int FakeSchemaOffset(const plg::string& className, const plg::string& fieldName) {
	const std::string cls(className.data(), className.size());
	const std::string field(fieldName.data(), fieldName.size());
	if (!g_hiddenSchemaField.empty() && field == g_hiddenSchemaField) {
		return -1;
	}
	struct Entry {
		const char* cls;
		const char* field;
		int offset;
	};
	static const Entry kTable[] = {
		{"CBasePlayerWeapon", "m_hOwnerEntity", kWeaponOwner},
		{"CBasePlayerPawn", "m_hController", kPawnController},
		{"CBasePlayerWeapon", "m_AttributeManager", kWeaponAttributeManager},
		{"CAttributeContainer", "m_Item", kContainerItem},
		{"CEconItemView", "m_iItemDefinitionIndex", kItemDefIndex},
		{"CEconItemView", "m_iEntityQuality", kItemQuality},
		{"CEconItemView", "m_szCustomName", kItemCustomName},
		{"CEconItemView", "m_bInitialized", kItemInitialized},
		{"CEconItemView", "m_AttributeList", kItemAttributeList},
		{"CEconItemView", "m_NetworkedDynamicAttributes", kItemNetworkedAttributes},
		{"CAttributeList", "m_Attributes", kAttributeVector},
		{"CCSPlayerPawn", "m_EconGloves", kPawnEconGloves},
		{"CCSPlayerPawn", "m_nEconGlovesChanged", kPawnEconGlovesChanged},
		{"CBasePlayerPawn", "m_pWeaponServices", kPawnWeaponServices},
		{"CPlayer_WeaponServices", "m_hActiveWeapon", kServicesActive},
		{"CBasePlayerWeapon", "m_iClip1", kWeaponClip},
		{"CBasePlayerWeapon", "m_pReserveAmmo", kWeaponReserveAmmo},
		{"CWeaponTaser", "m_fFireTime", kTaserFireTime},
		{"CWeaponTaser", "m_nLastAttackTick", kTaserLastAttack},
		{"CCSPlayerController", "m_iMusicKitID", kControllerMusicKit},
		{"CCSPlayerController", "m_pInventoryServices", kControllerInventory},
		{"CCSPlayerController", "m_hPlayerPawn", kControllerPlayerPawn},
		{"CCSPlayerController_InventoryServices", "m_unMusicID", kInventoryMusicId},
		{"CCSPlayerController_InventoryServices", "m_rank", kInventoryRank},
		// This build's schema names the component's pawn pointer through its chainer field; a
		// build with the direct m_pPawn name is covered by g_hiddenSchemaField tests instead.
		{"CPlayerPawnComponent", "__m_pChainEntity", kServicesPawn},
	};
	for (const Entry& entry : kTable) {
		if (cls == entry.cls && field == entry.field) {
			return entry.offset;
		}
	}
	return -1; // m_nMusicID is deliberately absent: this build has m_unMusicID
}

int FakeChainOffset(const plg::string& className) {
	const std::string cls(className.data(), className.size());
	if (cls == "CCSPlayerController_InventoryServices") {
		return 0; // "0 is not none": the engine must turn this into the inline marker (-2)
	}
	return 0x1000;
}

// --- recordings ----------------------------------------------------------------------------------
struct AttributeCall {
	void* list = nullptr;
	std::string name;
	float value = 0.0f;
};
std::vector<AttributeCall> g_attributeCalls;

struct StateChange {
	void* entity = nullptr;
	int offset = 0;
	int chain = 0;
};
std::vector<StateChange> g_stateChanges;

struct InputCall {
	int32_t handle = 0;
	std::string input;
	std::string value;
};
std::vector<InputCall> g_inputCalls;

struct ModelCall {
	int slot = 0;
	std::string model;
};
std::vector<ModelCall> g_modelCalls;

struct WeaponCall {
	std::string action; // "remove" | "select" | "give"
	int slot = 0;
	int32_t handle = 0;
	std::string name;
};
std::vector<WeaponCall> g_weaponCalls;

void ResetRecordings() {
	g_attributeCalls.clear();
	g_stateChanges.clear();
	g_inputCalls.clear();
	g_modelCalls.clear();
	g_weaponCalls.clear();
}

const AttributeCall* FindAttribute(void* list, std::string_view name) {
	for (const AttributeCall& call : g_attributeCalls) {
		if (call.list == list && call.name == name) {
			return &call;
		}
	}
	return nullptr;
}

int CountAttribute(std::string_view name) {
	int count = 0;
	for (const AttributeCall& call : g_attributeCalls) {
		count += call.name == name ? 1 : 0;
	}
	return count;
}

// The engine notifies with the entity's base pointer (weapon + 0), i.e. the entity's own buffer -
// not the FakeEntity object that owns it. Comparing the object address would never match.
const StateChange* FindStateChange(const FakeEntity* entity, int offset) {
	const void* pointer = entity != nullptr ? entity->memory.data() : nullptr;
	for (const StateChange& change : g_stateChanges) {
		if (change.entity == pointer && change.offset == offset) {
			return &change;
		}
	}
	return nullptr;
}

bool InputIssued(const FakeEntity* entity, std::string_view input, std::string_view value) {
	for (const InputCall& call : g_inputCalls) {
		if (call.input == input && call.value == value &&
			(entity == nullptr || call.handle == entity->handle)) {
			return true;
		}
	}
	return false;
}

// =================================================================================================
// The fake s2sdk bindings
// =================================================================================================
// The live build's IsEntityPtr rejects the pawn pointers GetClientPawn hands out, which made
// EntPointerToEntHandle answer INVALID_EHANDLE_INDEX for every player (agent=0, gloves invisible).
bool g_rejectPawnPointerHandles = false;

void* FakeEntHandleToEntPointer(int32_t handle) { return PointerOf(FromHandle(handle)); }
int32_t FakeEntPointerToEntHandle(void* pointer) {
	FakeEntity* entity = FromPointer(pointer);
	if (entity == nullptr) {
		return -1;
	}
	if (g_rejectPawnPointerHandles && entity->classname == "CCSPlayerPawn") {
		return -1;
	}
	return entity->handle;
}
int32_t FakeEntPointerToEntIndex(void* pointer) {
	FakeEntity* entity = FromPointer(pointer);
	return entity != nullptr ? (entity->handle & 0x7FFF) : 0;
}
void* FakeEntIndexToEntPointer(int32_t index) {
	for (FakeEntity* entity : g_entities) {
		if ((entity->handle & 0x7FFF) == index) {
			return PointerOf(entity);
		}
	}
	return nullptr;
}
bool FakeIsValidEntPointer(void* pointer) { return FromPointer(pointer) != nullptr; }
bool FakeIsValidEntHandle(int32_t handle) { return FromHandle(handle) != nullptr; }
int32_t FakeEntHandleToEntIndex(int32_t handle) {
	FakeEntity* entity = FromHandle(handle);
	return entity != nullptr ? (entity->handle & 0x7FFF) : -1;
}
int32_t FakeEntIndexToEntHandle(int32_t index) {
	for (FakeEntity* entity : g_entities) {
		if ((entity->handle & 0x7FFF) == index) {
			return entity->handle;
		}
	}
	return -1;
}

bool FakeIsClientConnected(int32_t slot) {
	return slot >= 0 && slot < 64 && g_slots[static_cast<std::size_t>(slot)].connected;
}
bool FakeIsClientInGame(int32_t slot) { return FakeIsClientConnected(slot); }
bool FakeIsFakeClient(int32_t slot) {
	return slot >= 0 && slot < 64 && g_slots[static_cast<std::size_t>(slot)].bot;
}
uint64_t FakeGetClientSteamID64(int32_t slot) {
	return slot >= 0 && slot < 64 ? g_slots[static_cast<std::size_t>(slot)].steamId : 0;
}
s2sdk::CSTeam FakeGetClientTeam(int32_t slot) {
	const int team = slot >= 0 && slot < 64 ? g_slots[static_cast<std::size_t>(slot)].team : 0;
	return static_cast<s2sdk::CSTeam>(team);
}
void* FakePlayerSlotToEntPointer(int32_t slot) {
	return slot >= 0 && slot < 64 ? PointerOf(g_slots[static_cast<std::size_t>(slot)].controller) : nullptr;
}
void* FakeGetClientPawn(int32_t slot) {
	return slot >= 0 && slot < 64 ? PointerOf(g_slots[static_cast<std::size_t>(slot)].pawn) : nullptr;
}
int32_t FakeEntPointerToPlayerSlot(void* pointer) {
	for (int slot = 0; slot < 64; ++slot) {
		if (PointerOf(g_slots[static_cast<std::size_t>(slot)].controller) == pointer) {
			return slot;
		}
	}
	return -1;
}
int32_t FakeGetMaxClients() { return 64; }

plg::vector<int32_t> FakeGetClientWeapons(int32_t slot) {
	plg::vector<int32_t> handles;
	if (slot < 0 || slot >= 64) {
		return handles;
	}
	for (const int32_t handle : g_slots[static_cast<std::size_t>(slot)].weapons) {
		handles.push_back(handle);
	}
	return handles;
}

s2sdk::GearSlot FakeGetWeaponGearSlot(int32_t handle) {
	FakeEntity* entity = FromHandle(handle);
	return entity != nullptr ? static_cast<s2sdk::GearSlot>(entity->gearSlot) : s2sdk::GearSlot::Invalid;
}

void RemoveFromSlot(int slot, int32_t handle) {
	if (slot < 0 || slot >= 64) {
		return;
	}
	auto& weapons = g_slots[static_cast<std::size_t>(slot)].weapons;
	for (auto it = weapons.begin(); it != weapons.end(); ++it) {
		if (*it == handle) {
			weapons.erase(it);
			return;
		}
	}
}

void FakeRemoveWeapon(int32_t slot, int32_t handle) {
	g_weaponCalls.push_back({"remove", slot, handle, ""});
	RemoveFromSlot(slot, handle);
}

void FakeSelectWeapon(int32_t slot, int32_t handle) {
	g_weaponCalls.push_back({"select", slot, handle, ""});
	if (slot >= 0 && slot < 64 && g_slots[static_cast<std::size_t>(slot)].weaponServices != nullptr) {
		Poke<int32_t>(g_slots[static_cast<std::size_t>(slot)].weaponServices, kServicesActive, handle);
	}
}

// The definition index the fake engine gives a classname (only the ones the tests use).
int32_t DefinitionForClassname(const std::string& name) {
	if (name == "weapon_ak47") {
		return 7;
	}
	if (name == "weapon_taser") {
		return 31;
	}
	if (name == "weapon_knife") {
		return 42;
	}
	if (name == "weapon_deagle") {
		return 1;
	}
	return 0;
}

// The engine's own hook must see the new weapon, like the real detour fires inside
// s2sdk::GiveNamedItem: the fake keeps the installed post callback and calls it.
polyhook::CallbackHandler g_installedPostHandler = nullptr;
polyhook::HookHandle g_installedHook = nullptr;
void* g_lastReturnPointer = nullptr;

// The fake trampoline's argument array: index 0 is `this` (CCSPlayer_ItemServices*), the same
// declaration and indexing polyhook uses (one slot per declared argument). All-null unless a test
// (or FakeGiveNamedItem) fills it, which leaves every other test on the owner-chain fallback.
constexpr std::size_t kHookArgumentCount = 6;
std::array<void*, kHookArgumentCount> g_hookArguments{};

int32_t FakeGiveNamedItem(int32_t slot, const plg::string& itemName) {
	const std::string name(itemName.data(), itemName.size());
	FakeEntity* weapon = NewEntity(name);
	weapon->gearSlot = name == "weapon_knife" ? 2 : 1; // GEAR_SLOT_KNIFE = 2, the rest is a filler
	g_hookArguments.fill(nullptr);
	if (slot >= 0 && slot < 64) {
		FakeSlot& state = g_slots[static_cast<std::size_t>(slot)];
		Poke<int32_t>(weapon, kWeaponOwner, state.pawn != nullptr ? state.pawn->handle : -1);
		Poke<uint16_t>(weapon, kItemOffset + kItemDefIndex, static_cast<uint16_t>(DefinitionForClassname(name)));
		if (weapon->gearSlot == 2) {
			state.weapons.insert(state.weapons.begin(), weapon->handle);
		} else {
			state.weapons.push_back(weapon->handle);
		}
		// Argument 0 of the real call: the item services the weapon is given through (the engine
		// resolves the owner from it before touching the weapon's m_hOwnerEntity).
		g_hookArguments[0] = PointerOf(state.itemServices);
	}
	// The fresh weapon's inline reserve ammo starts at the 0xAA sentinel pattern: the engine has to
	// copy the real values into it, so the restored bytes are visible in the test.
	g_weaponCalls.push_back({"give", slot, weapon->handle, name});

	if (g_installedPostHandler != nullptr) {
		g_lastReturnPointer = PointerOf(weapon);
		int32_t returned = weapon->handle;
		g_installedPostHandler(g_installedHook, g_hookArguments.data(), static_cast<int32_t>(kHookArgumentCount),
							   &returned, polyhook::CallbackType::Post);
	}
	return weapon->handle;
}

plg::string FakeGetEntityClassname(int32_t handle) {
	FakeEntity* entity = FromHandle(handle);
	return entity != nullptr ? plg::string(entity->classname.c_str()) : plg::string();
}

plg::string FakeGetEntityModel(int32_t handle) {
	FakeEntity* entity = FromHandle(handle);
	return entity != nullptr ? plg::string(entity->model.c_str()) : plg::string();
}

void FakeSetClientModel(int32_t slot, const plg::string& model) {
	const std::string value(model.data(), model.size());
	g_modelCalls.push_back({slot, value});
	if (slot >= 0 && slot < 64 && g_slots[static_cast<std::size_t>(slot)].pawn != nullptr) {
		g_slots[static_cast<std::size_t>(slot)].pawn->model = value;
	}
}

void FakeAcceptEntityInput(int32_t entityHandle, const plg::string& inputName, int32_t activatorHandle,
						   int32_t callerHandle, const plg::any& value, s2sdk::FieldType type, int32_t outputId) {
	(void)activatorHandle;
	(void)callerHandle;
	(void)type;
	(void)outputId;
	InputCall call;
	call.handle = entityHandle;
	call.input.assign(inputName.data(), inputName.size());
	if (plg::holds_alternative<plg::string>(value)) {
		const plg::string& text = plg::get<plg::string>(value);
		call.value.assign(text.data(), text.size());
	}
	g_inputCalls.push_back(std::move(call));
}

void FakeChangeEntityState2(void* entity, int32_t offset, int32_t chainOffset) {
	g_stateChanges.push_back({entity, offset, chainOffset});
}

// The native attribute writer, resolved through the fake gamedata like the real one.
void FakeSetAttribute(void* list, const char* name, float value) {
	g_attributeCalls.push_back({list, name != nullptr ? std::string(name) : std::string(), value});
}

// The GiveNamedItem target the fake signature points at (only its address matters).
void FakeGiveNamedItemTarget() {}

// --- fake gamedata -------------------------------------------------------------------------------
bool g_gamedataLoadFails = false;
uint32_t g_lastClosedConfig = 0;

s2sdk::ConfigId FakeLoadGameConfigFile(const plg::vector<plg::string>& paths) {
	if (g_gamedataLoadFails || paths.empty()) {
		return 0;
	}
	return 7;
}

void* FakeGetGameConfigSignature(s2sdk::ConfigId id, const plg::string& name) {
	if (id != 7) {
		return nullptr;
	}
	const std::string key(name.data(), name.size());
	if (key == "GiveNamedItem") {
		return reinterpret_cast<void*>(&FakeGiveNamedItemTarget);
	}
	if (key == "CAttributeList::SetOrAddAttributeValueByName") {
		return reinterpret_cast<void*>(&FakeSetAttribute);
	}
	return nullptr;
}

void FakeCloseGameConfigFile(s2sdk::ConfigId id) { g_lastClosedConfig = id; }

// --- fake polyhook -------------------------------------------------------------------------------
struct DetourRecord {
	void* target = nullptr;
	polyhook::DataType returnType = polyhook::DataType::Void;
	std::vector<polyhook::DataType> arguments;
	int32_t varIndex = 0;
	std::string name;
};
DetourRecord g_lastDetour;
int g_hookDetourCalls = 0;
int g_addCallbackCalls = 0;
int g_removeCallbackCalls = 0;
int g_unhookCalls = 0;
void* g_unhookedTarget = nullptr;
bool g_areCallbacksRegistered = false;
bool g_addCallbackFails = false;

polyhook::HookHandle FakeHookDetour(void* pFunc, polyhook::DataType returnType,
									const plg::vector<polyhook::DataType>& arguments, int32_t varIndex,
									const plg::string& name) {
	++g_hookDetourCalls;
	g_lastDetour.target = pFunc;
	g_lastDetour.returnType = returnType;
	g_lastDetour.arguments.assign(arguments.begin(), arguments.end());
	g_lastDetour.varIndex = varIndex;
	g_lastDetour.name.assign(name.data(), name.size());
	return pFunc; // any non-null handle
}

bool FakeAddCallback(polyhook::HookHandle hook, polyhook::CallbackType type, polyhook::CallbackHandler handler) {
	++g_addCallbackCalls;
	if (g_addCallbackFails) {
		return false;
	}
	if (type == polyhook::CallbackType::Post) {
		g_installedPostHandler = handler;
		g_installedHook = hook;
	}
	return true;
}

bool FakeRemoveCallback(polyhook::HookHandle hook, polyhook::CallbackType type,
						polyhook::CallbackHandler handler) {
	(void)hook;
	(void)handler;
	++g_removeCallbackCalls;
	if (type == polyhook::CallbackType::Post) {
		g_installedPostHandler = nullptr;
		g_installedHook = nullptr;
	}
	return true;
}

bool FakeAreCallbacksRegistered(polyhook::HookHandle hook) {
	(void)hook;
	return g_areCallbacksRegistered;
}

bool FakeUnhookDetour(void* pFunc) {
	++g_unhookCalls;
	g_unhookedTarget = pFunc;
	return true;
}

plg::string FakeGetError() { return {}; }
void* FakeGetReturnPointer(polyhook::ReturnHandle ret) {
	(void)ret;
	return g_lastReturnPointer;
}

void* FakeGetArgumentPointer(polyhook::ParametersHandle params, uint64_t index) {
	if (params == nullptr || index >= kHookArgumentCount) {
		return nullptr;
	}
	return static_cast<void* const*>(params)[index];
}

// --- fake timers ----------------------------------------------------------------------------------
struct EngineTimer {
	std::uint32_t id = 0;
	double delay = 0.0;
	s2sdk::TimerCallback callback = nullptr;
};
std::vector<EngineTimer> g_engineTimers;
std::uint32_t g_nextTimerId = 0;
int g_killCalls = 0; // every s2sdk KillTimer (= pisexcompat stopping a timer)

s2sdk::TimerId FakeCreateTimer(double delay, s2sdk::TimerCallback callback, s2sdk::TimerFlag flags,
							   const plg::vector<plg::any>& userData) {
	(void)flags;
	(void)userData;
	EngineTimer timer;
	timer.id = g_nextTimerId++;
	timer.delay = delay;
	timer.callback = callback;
	g_engineTimers.push_back(timer);
	return timer.id;
}

void FakeKillsTimer(s2sdk::TimerId id) {
	++g_killCalls;
	for (auto it = g_engineTimers.begin(); it != g_engineTimers.end(); ++it) {
		if (it->id == id) {
			g_engineTimers.erase(it);
			return;
		}
	}
}

void FakeRescheduleTimer(s2sdk::TimerId id, double delay) {
	for (EngineTimer& timer : g_engineTimers) {
		if (timer.id == id) {
			timer.delay = delay;
			return;
		}
	}
}

// Drains the deferred work: fire the pending engine timers in creation order (what
// TimerSystem::RunFrame does to a due timer) and let pisexcompat process the result, which also
// runs the NextFrame callbacks. Bounded: a callback that only reschedules must not hang the test.
// Fires the pending engine timers in creation order (what TimerSystem::RunFrame does to a due
// timer) and lets pisexcompat process the result after each fire, which also runs the NextFrame
// callbacks. Bounded: a callback that keeps rescheduling must not hang the test.
int DrainTimers(int maxFires) {
	int fired = 0;
	while (fired < maxFires && !g_engineTimers.empty()) {
		const EngineTimer timer = g_engineTimers.front();
		if (timer.callback != nullptr) {
			timer.callback(timer.id, plg::vector<plg::any>{});
		}
		pisex::Poll();
		++fired;
	}
	return fired;
}

void RunDeferred() {
	DrainTimers(8);
	pisex::Poll();
}

// =================================================================================================
// Installing and resetting the fakes
// =================================================================================================
void InstallFakeEngine() {
	__s2sdk_EntHandleToEntPointer = &FakeEntHandleToEntPointer;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_EntIndexToEntPointer = &FakeEntIndexToEntPointer;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_EntHandleToEntIndex = &FakeEntHandleToEntIndex;
	__s2sdk_EntIndexToEntHandle = &FakeEntIndexToEntHandle;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_EntPointerToPlayerSlot = &FakeEntPointerToPlayerSlot;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;
	__s2sdk_GetClientWeapons = &FakeGetClientWeapons;
	__s2sdk_GetWeaponGearSlot = &FakeGetWeaponGearSlot;
	__s2sdk_RemoveWeapon = &FakeRemoveWeapon;
	__s2sdk_SelectWeapon = &FakeSelectWeapon;
	__s2sdk_GiveNamedItem = &FakeGiveNamedItem;
	__s2sdk_GetEntityClassname = &FakeGetEntityClassname;
	__s2sdk_GetEntityModel = &FakeGetEntityModel;
	__s2sdk_SetClientModel = &FakeSetClientModel;
	__s2sdk_AcceptEntityInput = &FakeAcceptEntityInput;
	__s2sdk_ChangeEntityState2 = &FakeChangeEntityState2;
	__s2sdk_GetSchemaOffset = &FakeSchemaOffset;
	__s2sdk_GetSchemaChainOffset = &FakeChainOffset;
	__s2sdk_LoadGameConfigFile = &FakeLoadGameConfigFile;
	__s2sdk_GetGameConfigSignature = &FakeGetGameConfigSignature;
	__s2sdk_CloseGameConfigFile = &FakeCloseGameConfigFile;
	__s2sdk_IsClientAlive = nullptr; // the engine paths under test do not call it

	__polyhook_HookDetour = &FakeHookDetour;
	__polyhook_AddCallback = &FakeAddCallback;
	__polyhook_RemoveCallback = &FakeRemoveCallback;
	__polyhook_AreCallbacksRegistered = &FakeAreCallbacksRegistered;
	__polyhook_UnhookDetour = &FakeUnhookDetour;
	__polyhook_GetError = &FakeGetError;
	__polyhook_GetReturnPointer = &FakeGetReturnPointer;
	__polyhook_GetArgumentPointer = &FakeGetArgumentPointer;

	__s2sdk_CreateTimer = &FakeCreateTimer;
	__s2sdk_KillsTimer = &FakeKillsTimer;
	__s2sdk_RescheduleTimer = &FakeRescheduleTimer;
}

// What the engine's hook lookup answers with (the runtime's cache view). The tests set it before
// applying when the deferred re-apply or the hook needs the current cache.
const ws::Snapshot* g_lookupSnapshot = nullptr;

const ws::Snapshot* LookupCurrent(int slot) {
	(void)slot;
	return g_lookupSnapshot;
}

void ResetFakeState() {
	ResetEntities();
	ResetSlots();
	ResetRecordings();
	g_engineTimers.clear();
	g_nextTimerId = 0;
	g_killCalls = 0;
	g_lookupSnapshot = nullptr;
	g_lastDetour = DetourRecord{};
	g_hookDetourCalls = 0;
	g_addCallbackCalls = 0;
	g_removeCallbackCalls = 0;
	g_unhookCalls = 0;
	g_unhookedTarget = nullptr;
	g_areCallbacksRegistered = false;
	g_addCallbackFails = false;
	g_gamedataLoadFails = false;
	g_lastClosedConfig = 0;
	g_hiddenSchemaField.clear();
	g_installedPostHandler = nullptr;
	g_installedHook = nullptr;
	g_lastReturnPointer = nullptr;
	g_hookArguments.fill(nullptr);
	g_rejectPawnPointerHandles = false;
}

// =================================================================================================
// World helpers
// =================================================================================================
ws::Skin MakeSkin(uint16_t definition, int paint, int seed, float wear, int quality = 0) {
	ws::Skin skin;
	skin.definition = definition;
	skin.paint = paint;
	skin.seed = seed;
	skin.wear = wear;
	skin.quality = quality;
	return skin;
}

constexpr uint64_t kSteamId = 76561198000000001ULL;
constexpr uint16_t kAk47 = 7;
constexpr uint16_t kKnife = 507; // karambit
constexpr uint16_t kTaser = 31;

ws::Snapshot MakeSnapshot(uint64_t steamId, int team, const ws::TeamItems& items, int music = 0) {
	ws::Snapshot snapshot;
	snapshot.steamId = steamId;
	snapshot.music = music;
	const int index = ws::TeamIndex(team);
	if (index >= 0) {
		snapshot.teams[static_cast<std::size_t>(index)] = items;
	}
	return snapshot;
}

// Invokes the installed post callback exactly like polyhook's trampoline would, on the fallback
// route: argument 0 is empty, so the hook must resolve the owner from the weapon's m_hOwnerEntity
// (the route the pre-fix code used exclusively). CallHookWithServices covers the realistic route.
void InvokeHook(void* weaponPointer) {
	g_lastReturnPointer = weaponPointer;
	if (g_installedPostHandler == nullptr) {
		Check(false, "the hook should be installed");
		return;
	}
	g_installedPostHandler(g_installedHook, g_hookArguments.data(), static_cast<int32_t>(kHookArgumentCount), nullptr,
						   polyhook::CallbackType::Post);
}

void CallHook(void* weaponPointer) {
	g_hookArguments[0] = nullptr;
	InvokeHook(weaponPointer);
}

// The realistic trampoline call: argument 0 is the CCSPlayer_ItemServices* the weapon is being
// given through (FakeGiveNamedItem fills the same slot).
void CallHookWithServices(void* weaponPointer, FakeEntity* services) {
	g_hookArguments[0] = PointerOf(services);
	InvokeHook(weaponPointer);
}

// =================================================================================================
// Tests
// =================================================================================================

void TestDecodeHelpers() {
	Section("decode helpers");

	// The StatTrak counter is stored as the int's bit pattern, not as the numeric value.
	const float stattrak = ws::detail::StatTrakValue(7);
	CheckEq(static_cast<long long>(std::bit_cast<std::int32_t>(stattrak)), 7, "StatTrak 7 keeps its bits");
	Check(stattrak != 7.0f, "StatTrak 7 is not the float 7.0f");
	CheckEq(static_cast<long long>(std::bit_cast<std::int32_t>(ws::detail::IntBitsAsAttribute(1337))), 1337,
			"a sticker schema keeps its bits");

	CheckStr(ws::detail::KnifeSubclassValue(507), "507", "the karambit ChangeSubclass value");
	CheckStr(ws::detail::KnifeSubclassValue(42), "42", "the default knife ChangeSubclass value");
	Check(ws::detail::KnifeSubclassValue(9999) == nullptr, "an unknown knife index has no literal");

	// The nametag write is bounded and always NUL-terminated.
	std::array<std::uint8_t, 200> buffer{};
	buffer.fill(kSentinel);
	const std::string longName(400, 'x');
	ws::detail::WriteFixedString(buffer.data(), 8, longName, ws::detail::kWidthCustomName);
	std::size_t length = 0;
	while (length < ws::detail::kWidthCustomName && buffer[8 + length] != 0) {
		++length;
	}
	CheckEq(static_cast<long long>(length), ws::detail::kWidthCustomName - 1, "the nametag is truncated to 160 bytes");
	CheckEq(buffer[8 + ws::detail::kWidthCustomName], kSentinel, "the byte after the nametag buffer is untouched");

	// The m_Attributes header gate: count at +0, data at +8, capacity at +16 (the 104-byte
	// CUtlVectorEmbeddedNetworkVar of the fixture/disassembly - NOT the old CUtlVector guess).
	std::array<std::uint8_t, 64> list{};
	std::int32_t capacity = 4;
	std::int32_t count = 2;
	void* data = list.data();
	std::memcpy(list.data() + ws::detail::kAttributeCountOffset, &count, sizeof(count));
	std::memcpy(list.data() + ws::detail::kAttributeDataOffset, &data, sizeof(data));
	std::memcpy(list.data() + ws::detail::kAttributeCapacityOffset, &capacity, sizeof(capacity));
	Check(ws::detail::IsAttributeHeaderPlausible(list.data(), 0), "a plausible header passes the gate");
	CheckEq(ws::detail::kAttributeCountOffset, 0, "the count is at object + 0");
	CheckEq(ws::detail::kAttributeDataOffset, 8, "the data pointer is at object + 8");
	CheckEq(ws::detail::kAttributeCapacityOffset, 16, "the capacity is at object + 16");
	capacity = 100000;
	std::memcpy(list.data() + ws::detail::kAttributeCapacityOffset, &capacity, sizeof(capacity));
	Check(!ws::detail::IsAttributeHeaderPlausible(list.data(), 0), "an implausible capacity fails the gate");
	capacity = 4;
	count = -1;
	std::memcpy(list.data() + ws::detail::kAttributeCapacityOffset, &capacity, sizeof(capacity));
	std::memcpy(list.data() + ws::detail::kAttributeCountOffset, &count, sizeof(count));
	Check(!ws::detail::IsAttributeHeaderPlausible(list.data(), 0), "a negative count fails the gate");
	count = 9; // more live attributes than capacity
	std::memcpy(list.data() + ws::detail::kAttributeCountOffset, &count, sizeof(count));
	Check(!ws::detail::IsAttributeHeaderPlausible(list.data(), 0), "count > capacity fails the gate");
	count = 0;
	std::memcpy(list.data() + ws::detail::kAttributeCountOffset, &count, sizeof(count));
	Check(ws::detail::IsAttributeHeaderPlausible(list.data(), 0), "an empty list is always clearable");

	// The inline reserve ammo: two int32 read/written in place, no pointer is ever loaded.
	FakeEntity* weapon = NewEntity("CBasePlayerWeapon");
	SetReserveAmmo(weapon, 90, 3);
	CheckEq(ws::detail::ReadReserveAmmoValue(weapon->memory.data(), kWeaponReserveAmmo, 0), 90,
			"the first inline reserve value is read in place");
	CheckEq(ws::detail::ReadReserveAmmoValue(weapon->memory.data(), kWeaponReserveAmmo, 1), 3,
			"the second inline reserve value is read in place");
	ws::detail::WriteReserveAmmoValue(weapon->memory.data(), kWeaponReserveAmmo, 1, 7);
	CheckEq(ReserveAmmoAt(weapon, 1), 7, "the inline write lands on the second int32");
	CheckEq(static_cast<int>(weapon->memory[kWeaponReserveAmmo - 1]), static_cast<int>(kSentinel),
			"nothing is written before the field");
	CheckEq(static_cast<int>(weapon->memory[kWeaponReserveAmmo + 8]), static_cast<int>(kSentinel),
			"nothing is written after the field");
	ResetEntities();

	// The page guard behind every schema-derived pointer dereference (service pointers, the
	// attribute-header data check): a mapped pointer passes, the null page and an unmapped address
	// do not.
	Check(!ws::detail::IsPageMapped(nullptr), "the page guard rejects null");
	std::int32_t mapped = 0;
	Check(ws::detail::IsPageMapped(&mapped), "the page guard accepts the test's own stack");
	Check(!ws::detail::IsPageMapped(reinterpret_cast<const void*>(0x1000)), "the page guard rejects the null page");
}

void TestPrepare() {
	Section("Prepare");
	ResetFakeState();
	InstallFakeEngine();

	ws::Config config;
	ws::EconData econ;
	ws::Engine engine(config, econ);
	std::string error;

	// No map yet: Prepare must refuse instead of letting GetSchemaOffset run (PITFALLS §C.8).
	Check(!engine.Prepare(error), "Prepare without a live map fails");
	Check(!error.empty(), "Prepare without a live map explains itself");

	// pisex::Poll marks the game ready (the host pumps it from a live map).
	pisex::Poll();
	Check(engine.Prepare(error), "Prepare succeeds with a live map: " + error);

	// A field the build does not have is reported by name and fails the prepare.
	ResetFakeState();
	InstallFakeEngine();
	ws::Engine broken(config, econ);
	g_hiddenSchemaField = "m_pReserveAmmo";
	Check(!broken.Prepare(error), "a missing schema field fails Prepare");
	Check(error.find("m_pReserveAmmo") != std::string::npos, "the error names the missing field");
	g_hiddenSchemaField.clear();
	Check(broken.Prepare(error), "re-prepare after a map change succeeds again: " + error);
}

void TestInstallAndTeardown() {
	Section("InstallHook and teardown");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);

	const ws::Snapshot* none = nullptr;
	(void)none;
	auto lookup = [](int) -> const ws::Snapshot* { return nullptr; };

	// A missing lookup is a hard error (the hook would have nothing to apply).
	Check(!engine.InstallHook("/tmp/gamedata.jsonc", {}, error), "InstallHook without a lookup fails");
	Check(error.find("lookup") != std::string::npos, "the error names the lookup");

	// A failed gamedata load (0, not -1) is detected and the config is closed.
	g_gamedataLoadFails = true;
	Check(!engine.InstallHook("/tmp/gamedata.jsonc", lookup, error), "InstallHook with a failed gamedata load fails");
	Check(error.find("could not load") != std::string::npos, "the error names the load failure");
	g_gamedataLoadFails = false;

	// Success: the detour declares the reference's full six-argument ABI, all 64-bit - polyhook's
	// JIT forwards only the declared arguments to the original, and the 6th slot (an origin/vector
	// pointer) shipped garbage when the declaration stopped at five (live: every weapon spawned at
	// a garbage origin, "m_cellX cell -2097152 is outside of cell bounds").
	Check(engine.InstallHook("/tmp/gamedata.jsonc", lookup, error), "InstallHook succeeds: " + error);
	CheckEq(g_hookDetourCalls, 1, "HookDetour is called once");
	CheckEq(g_addCallbackCalls, 1, "AddCallback is called once");
	CheckEq(static_cast<long long>(g_lastDetour.arguments.size()), 6, "six declared arguments");
	if (g_lastDetour.arguments.size() == 6) {
		Check(g_lastDetour.arguments[0] == polyhook::DataType::Pointer, "arg 0 (this) is a pointer");
		Check(g_lastDetour.arguments[1] == polyhook::DataType::Pointer, "arg 1 (name) is a pointer");
		Check(g_lastDetour.arguments[2] == polyhook::DataType::Int64, "arg 2 (subtype) is a 64-bit integer");
		Check(g_lastDetour.arguments[3] == polyhook::DataType::Pointer, "arg 3 (script item) is a pointer");
		Check(g_lastDetour.arguments[4] == polyhook::DataType::Int64, "arg 4 (a5) is a 64-bit integer");
		Check(g_lastDetour.arguments[5] == polyhook::DataType::Pointer, "arg 5 (a6) is a pointer");
	}
	Check(g_lastDetour.returnType == polyhook::DataType::Pointer, "the return type is a pointer");
	CheckEq(g_lastDetour.varIndex, -1, "varIndex is -1");
	CheckStr(g_lastDetour.name, "WeaponSkins::GiveNamedItem", "the detour name");
	Check(g_lastDetour.target == reinterpret_cast<void*>(&FakeGiveNamedItemTarget),
		  "the detour target is the signature address");
	Check(g_installedPostHandler != nullptr, "the post callback is installed");

	// Idempotent install.
	Check(engine.InstallHook("/tmp/gamedata.jsonc", lookup, error), "a second install is a no-op");
	CheckEq(g_hookDetourCalls, 1, "no second detour");

	// Teardown: RemoveCallback -> UnhookDetour(target) -> the config closes.
	engine.Shutdown();
	CheckEq(g_removeCallbackCalls, 1, "RemoveCallback is called on teardown");
	CheckEq(g_unhookCalls, 1, "UnhookDetour is called when no callbacks remain");
	Check(g_unhookedTarget == reinterpret_cast<void*>(&FakeGiveNamedItemTarget),
		  "UnhookDetour gets the target address, not the trampoline");
	CheckEq(static_cast<long long>(g_lastClosedConfig), 7, "the gamedata config is closed");

	// Shared detour: another plugin still has a callback -> the detour must stay installed.
	ResetFakeState();
	InstallFakeEngine();
	ws::Engine shared(config, econ);
	Check(shared.Prepare(error), "prepare (shared): " + error);
	Check(shared.InstallHook("/tmp/gamedata.jsonc", lookup, error), "install (shared): " + error);
	g_areCallbacksRegistered = true;
	shared.Shutdown();
	CheckEq(g_removeCallbackCalls, 1, "our callback is removed");
	CheckEq(g_unhookCalls, 0, "the shared detour is left alone while others use it");
	CheckEq(g_addCallbackCalls, 1, "no extra callback registrations");
}

struct GunWorld {
	ws::Config config;
	ws::EconData econ;
	ws::TeamItems items;
	ws::Snapshot snapshot;
	std::unique_ptr<ws::Engine> engine;

	GunWorld() {
		items = ws::TeamItems{};
		snapshot = MakeSnapshot(kSteamId, 2, items);
	}
};

void PrepareGunWorld(GunWorld& world) {
	world.items = ws::TeamItems{};
	ws::Skin ak = MakeSkin(kAk47, 44, 123, 0.25f, ws::kStatTrakQuality);
	ak.stattrak = 7;
	ak.nametag = "my-ak";
	ak.stickers[0] = ws::Sticker{1001, 1337, 0.1f, 0.2f, 0.3f, 1.0f, 15.0f};	 // the default schema
	ak.stickers[3] = ws::Sticker{2002, 99, -0.1f, -0.2f, 0.4f, 1.5f, -30.0f};	 // a patch schema
	ak.keychain = ws::Keychain{3003, 4242, 0.5f, -0.5f, 0.25f};
	world.items.weapons[kAk47] = ak;
	// The karambit knife skin the knife tests apply (no StatTrak, so the clear path is taken).
	world.items.knife = MakeSkin(kKnife, 77, 5, 0.5f);
	world.econ.items[kAk47] = ws::ItemDefinition{kAk47, "weapon_ak47", "", 0, {}};
	world.snapshot = MakeSnapshot(kSteamId, 2, world.items);

	world.engine = std::make_unique<ws::Engine>(world.config, world.econ);
	std::string error;
	Check(world.engine->Prepare(error), "prepare: " + error);
	Check(world.engine->InstallHook("/tmp/gamedata.jsonc",
									[&world](int) -> const ws::Snapshot* { return &world.snapshot; }, error),
		  "install: " + error);
}

void TestHookGuards() {
	Section("hook guards");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	GunWorld world;
	PrepareGunWorld(world);
	MakePlayer(0, kSteamId);
	FakeEntity* weapon = AddWeapon(0, kAk47, "weapon_ak47", 1);

	FakeEntity* pawn = g_slots[0].pawn;

	CallHook(nullptr);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a null weapon pointer applies nothing");

	std::array<std::uint8_t, 64> notAnEntity{};
	CallHook(notAnEntity.data());
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a non-entity pointer applies nothing");

	// A weapon without an owner (a dropped item) is skipped on the fallback route - the route this
	// call takes, because CallHook leaves argument 0 empty. With a services argument the same
	// weapon is applied: TestHookOwnerFromServices covers exactly that (the live fix).
	Poke<int32_t>(weapon, kWeaponOwner, -1);
	CallHook(PointerOf(weapon));
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a weapon without an owner is skipped");
	Poke<int32_t>(weapon, kWeaponOwner, pawn->handle);

	// A bot is skipped even with a full snapshot.
	g_slots[0].bot = true;
	CallHook(PointerOf(weapon));
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a bot never gets a skin");
	g_slots[0].bot = false;

	// A stale cache entry for a slot that was taken over is skipped.
	const uint64_t realSteamId = g_slots[0].steamId;
	g_slots[0].steamId = 12345;
	CallHook(PointerOf(weapon));
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a snapshot with a foreign steamId is ignored");
	g_slots[0].steamId = realSteamId;

	// The map guard: between map end and the next map start nothing may run.
	pisex::OnMapEndNotify();
	CallHook(PointerOf(weapon));
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "no apply during the map shutdown");
	pisex::OnMapStartNotify();
	pisex::Poll();
}

void TestHookOwnerFromServices() {
	Section("hook owner from argument 0 (the services the item is given through)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	GunWorld world;
	PrepareGunWorld(world);
	MakePlayer(0, kSteamId);
	FakeEntity* weapon = AddWeapon(0, kAk47, "weapon_ak47", 1);
	FakeEntity* pawn = g_slots[0].pawn;
	FakeEntity* services = g_slots[0].itemServices;

	// The live failure: GiveNamedItem has not assigned the weapon's m_hOwnerEntity yet when the
	// post hook runs, so the owner chain alone would skip every weapon. Argument 0 (the services)
	// knows the pawn, so the skin must land anyway.
	Poke<int32_t>(weapon, kWeaponOwner, -1);
	CallHookWithServices(PointerOf(weapon), services);
	Check(CountAttribute("set item texture prefab") > 0, "the skin is applied from the services argument");
	CheckEq(static_cast<long long>(world.engine->Stats().hookGun), 1, "the hook counted a gun apply");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoServices), 0, "the services argument was readable");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoPawn), 0, "a pawn was resolved");

	// The owner-chain fallback stays: no readable argument 0 (all-null params, like a polyhook
	// without the argument getter) still applies the skin through the weapon's owner.
	ResetRecordings();
	Poke<int32_t>(weapon, kWeaponOwner, pawn->handle);
	CallHook(PointerOf(weapon));
	Check(CountAttribute("set item texture prefab") > 0, "the owner-chain fallback still applies the skin");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoServices), 1, "the fallback call is counted");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoPawn), 0, "the fallback found a pawn");

	// An argument 0 that resolves to nothing also falls back - and still succeeds when the owner
	// is set.
	ResetRecordings();
	Poke<void*>(services, kServicesPawn, nullptr);
	CallHookWithServices(PointerOf(weapon), services);
	Check(CountAttribute("set item texture prefab") > 0, "a services argument without a pawn falls back");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoPawn), 0, "the fallback covered the empty services");
	Poke<void*>(services, kServicesPawn, PointerOf(pawn));

	// With no pawn anywhere the call is refused and counted as hookNoPawn.
	ResetRecordings();
	Poke<int32_t>(weapon, kWeaponOwner, -1);
	Poke<void*>(services, kServicesPawn, nullptr);
	CallHookWithServices(PointerOf(weapon), services);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "no pawn anywhere applies nothing");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoPawn), 1, "the failed pawn resolution is counted");
	Poke<int32_t>(weapon, kWeaponOwner, pawn->handle);
	Poke<void*>(services, kServicesPawn, PointerOf(pawn));

	// The bot guard still runs after the services route: bots take the same GiveNamedItem path.
	ResetRecordings();
	g_slots[0].bot = true;
	CallHookWithServices(PointerOf(weapon), services);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a bot never gets a skin through the services route");
	CheckEq(static_cast<long long>(world.engine->Stats().hookBot), 1, "the bot is counted as hookBot");
	g_slots[0].bot = false;

	// A slot that is not ready is refused with its own counter.
	ResetRecordings();
	g_slots[0].connected = false;
	CallHookWithServices(PointerOf(weapon), services);
	CheckEq(static_cast<long long>(world.engine->Stats().hookNotReady), 1, "a not-ready client is counted");
	g_slots[0].connected = true;

	// The pawn identity comes from the controller's m_hPlayerPawn, not from the services pointer,
	// so the services route and the fallback key the same life.
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoController), 0, "the controller resolved");
	CheckEq(static_cast<long long>(world.engine->Stats().hookNoSlot), 0, "the slot resolved");
}

void TestGunApply() {
	Section("gun apply (fields, attributes, stickers, keychain)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	GunWorld world;
	PrepareGunWorld(world);
	MakePlayer(0, kSteamId);
	FakeEntity* weapon = AddWeapon(0, kAk47, "weapon_ak47", 1);

	CallHook(PointerOf(weapon));
	const ws::Skin& skin = world.items.weapons[kAk47];
	const int itemAt = kItemOffset;

	// The econ item fields, with their real widths (the sentinel after the uint16 is untouched).
	CheckEq(Peek<uint16_t>(weapon, itemAt + kItemDefIndex), kAk47, "the definition index (uint16)");
	CheckEq(PeekByte(weapon, itemAt + kItemDefIndex + 2), kSentinel, "the byte after the defindex is untouched");
	CheckEq(Peek<int32_t>(weapon, itemAt + kItemQuality), ws::kStatTrakQuality, "the quality (int32)");
	CheckEq(PeekByte(weapon, itemAt + kItemQuality + 4), kSentinel, "the bytes after the quality are untouched");

	// The paint trio in both lists, networked first.
	void* plain = weapon->memory.data() + itemAt + kItemAttributeList;
	void* networked = weapon->memory.data() + itemAt + kItemNetworkedAttributes;
	const AttributeCall* prefab = FindAttribute(plain, "set item texture prefab");
	Check(prefab != nullptr, "prefab in the plain list");
	if (prefab != nullptr) {
		CheckFloat(prefab->value, static_cast<float>(skin.paint), "the prefab value");
	}
	const AttributeCall* seed = FindAttribute(networked, "set item texture seed");
	Check(seed != nullptr, "seed in the networked list");
	if (seed != nullptr) {
		CheckFloat(seed->value, static_cast<float>(skin.seed), "the seed value");
	}
	const AttributeCall* wear = FindAttribute(plain, "set item texture wear");
	Check(wear != nullptr, "wear in the plain list");
	if (wear != nullptr) {
		CheckFloat(wear->value, skin.wear, "the wear value");
	}
	CheckEq(CountAttribute("set item texture prefab"), 2, "the prefab is written to both lists");

	// StatTrak: the counter's bits in both lists, plus the score type.
	const AttributeCall* stattrak = FindAttribute(plain, "kill eater");
	Check(stattrak != nullptr, "kill eater in the plain list");
	if (stattrak != nullptr) {
		CheckEq(static_cast<long long>(std::bit_cast<std::int32_t>(stattrak->value)), 7, "the counter keeps its bits");
	}
	Check(FindAttribute(networked, "kill eater") != nullptr, "kill eater in the networked list");
	const AttributeCall* scoreType = FindAttribute(plain, "kill eater score type");
	Check(scoreType != nullptr && scoreType->value == 0.0f, "the score type is 0");

	// The nametag: bounded, NUL-terminated, the sentinel after the buffer untouched.
	CheckStr(std::string(reinterpret_cast<const char*>(weapon->memory.data() + itemAt + kItemCustomName)), "my-ak",
			 "the nametag is written");
	CheckEq(PeekByte(weapon, itemAt + kItemCustomName + ws::detail::kWidthCustomName), kSentinel,
			"the byte after m_szCustomName is untouched");
	Check(FindStateChange(weapon, itemAt + kItemCustomName) != nullptr, "the nametag change is announced");

	// Stickers: slot 0 has the default schema (1337) -> id only; slot 3 has schema and offsets.
	CheckEq(CountAttribute("sticker slot 0 id"), 2, "slot 0 id in both lists");
	CheckEq(CountAttribute("sticker slot 0 schema"), 0, "schema 1337 skips the schema attribute");
	CheckEq(CountAttribute("sticker slot 0 offset x"), 0, "schema 1337 skips the offsets");
	CheckEq(CountAttribute("sticker slot 0 wear"), 2, "slot 0 wear in both lists");
	CheckEq(CountAttribute("sticker slot 3 id"), 2, "slot 3 id in both lists");
	CheckEq(CountAttribute("sticker slot 3 schema"), 2, "slot 3 schema in both lists");
	CheckEq(CountAttribute("sticker slot 3 offset y"), 2, "slot 3 offset y in both lists");
	CheckEq(CountAttribute("sticker slot 3 rotation"), 2, "slot 3 rotation in both lists");
	CheckEq(CountAttribute("sticker slot 1 id"), 0, "an unset slot is skipped");
	CheckEq(CountAttribute("sticker slot 2 id"), 0, "unset slot 2 is skipped");
	CheckEq(CountAttribute("sticker slot 5 id"), 0, "the last unset slot is skipped");
	const AttributeCall* stickerId = FindAttribute(plain, "sticker slot 3 id");
	Check(stickerId != nullptr && std::bit_cast<std::int32_t>(stickerId->value) == 2002, "the sticker id keeps its bits");
	const AttributeCall* offsetX = FindAttribute(plain, "sticker slot 3 offset x");
	Check(offsetX != nullptr && offsetX->value == skin.stickers[3].x, "the sticker offset is a plain float");

	// The keychain (slot 0 only), with the int-bit fields.
	CheckEq(CountAttribute("keychain slot 0 id"), 2, "the keychain id in both lists");
	CheckEq(CountAttribute("keychain slot 0 seed"), 2, "the keychain seed in both lists");
	const AttributeCall* keychainSeed = FindAttribute(plain, "keychain slot 0 seed");
	Check(keychainSeed != nullptr && std::bit_cast<std::int32_t>(keychainSeed->value) == 4242,
		  "the keychain seed keeps its bits");

	// The bodygroup input (no legacy paint kit -> body,0) and the announced raw writes.
	Check(InputIssued(weapon, "SetBodygroup", "body,0"), "the standard bodygroup input");
	Check(FindStateChange(weapon, itemAt + kItemDefIndex) != nullptr, "the defindex change is announced");
	Check(FindStateChange(weapon, itemAt + kItemQuality) != nullptr, "the quality change is announced");

	// A legacy paint kit selects body,1.
	world.econ.paints[44] = ws::PaintKit{44, "legacy_kit", true, {}};
	ResetRecordings();
	CallHook(PointerOf(weapon));
	Check(InputIssued(weapon, "SetBodygroup", "body,1"), "a legacy paint kit selects body,1");

	// A weapon whose definition has no econ entry stops after the paint (the reference's early
	// exit): no bodygroup, no StatTrak, no stickers.
	ResetRecordings();
	world.snapshot.teams[0].weapons[63] = MakeSkin(63, 5, 1, 0.1f);
	FakeEntity* unknown = AddWeapon(0, 63, "weapon_cz75a", 1); // no econ entry for 63
	CallHook(PointerOf(unknown));
	CheckEq(CountAttribute("kill eater"), 0, "no StatTrak without a classname");
	CheckEq(CountAttribute("sticker slot 0 id"), 0, "no stickers without a classname");
	Check(!InputIssued(unknown, "SetBodygroup", "body,0"), "no bodygroup without a classname");
	CheckEq(Peek<uint16_t>(unknown, itemAt + kItemDefIndex), 63, "the defindex is still written");
	Check(FindAttribute(unknown->memory.data() + itemAt + kItemAttributeList, "set item texture prefab") != nullptr,
		  "the paint is still written without a classname");
}

void TestKnifeApply() {
	Section("knife apply (ChangeSubclass, gated attribute-count clear)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	GunWorld world;
	PrepareGunWorld(world); // the team items carry the karambit knife (no StatTrak)
	MakePlayer(0, kSteamId);

	FakeEntity* knife = AddWeapon(0, 42, "weapon_knife", 2);
	const int itemAt = kItemOffset;
	void* plain = knife->memory.data() + itemAt + kItemAttributeList;
	void* networked = knife->memory.data() + itemAt + kItemNetworkedAttributes;

	// Plausible headers on both lists, in the real CUtlVectorEmbeddedNetworkVar layout: the clear
	// must reset ONLY the count (4 bytes at +0); the data pointer at +8 and the capacity at +16 are
	// left alone, and so is every sentinel around them.
	std::int32_t capacity = 3;
	std::int32_t count = 2;
	std::vector<std::uint8_t> elements(64);
	void* data = elements.data();
	for (void* list : {plain, networked}) {
		auto* base = static_cast<std::uint8_t*>(list) + kAttributeVector;
		std::memcpy(base + kAttributeCountOffset, &count, sizeof(count));
		std::memcpy(base + kAttributeDataOffset, &data, sizeof(data));
		std::memcpy(base + kAttributeCapacityOffset, &capacity, sizeof(capacity));
		base[kAttributeCountOffset + 4] = kSentinel; // the byte after the count field
		base[kAttributeCapacityOffset + 4] = kSentinel;
	}

	CallHook(PointerOf(knife));
	Check(InputIssued(knife, "ChangeSubclass", "507"), "ChangeSubclass carries the new knife index literal");
	CheckEq(Peek<uint16_t>(knife, itemAt + kItemDefIndex), kKnife, "the knife definition index");
	CheckEq(Peek<int32_t>(knife, itemAt + kItemQuality), 0, "the knife quality");
	Check(!InputIssued(knife, "SetBodygroup", "body,0"), "no bodygroup input on the knife path");
	CheckEq(CountAttribute("kill eater"), 0, "a non-StatTrak knife has no counter");
	for (void* list : {plain, networked}) {
		auto* base = static_cast<std::uint8_t*>(list) + kAttributeVector;
		CheckEq(PeekRaw<int32_t>(base, kAttributeCountOffset), 0, "the attribute count is cleared");
		CheckEq(PeekRaw<int32_t>(base, kAttributeCapacityOffset), 3, "the capacity is untouched");
		Check(PeekRaw<void*>(base, kAttributeDataOffset) == data, "the data pointer is untouched");
		CheckEq(PeekRawByte(base, kAttributeCountOffset + 4), kSentinel, "the byte after the count is untouched");
		CheckEq(PeekRawByte(base, kAttributeCapacityOffset + 4), kSentinel, "the byte after the capacity is untouched");
	}
	Check(FindStateChange(knife, itemAt + kItemAttributeList + kAttributeVector) != nullptr,
		  "the plain-list clear is announced");
	Check(FindStateChange(knife, itemAt + kItemNetworkedAttributes + kAttributeVector) != nullptr,
		  "the networked-list clear is announced");
	const AttributeCall* knifePaint = FindAttribute(plain, "set item texture prefab");
	Check(knifePaint != nullptr && knifePaint->value == 77.0f, "the knife paint is written after the clear");

	// An implausible header (count > capacity: a layout this code must not touch) leaves every byte
	// of the header alone.
	ResetRecordings();
	for (void* list : {plain, networked}) {
		auto* base = static_cast<std::uint8_t*>(list) + kAttributeVector;
		std::int32_t bogusCount = 999999;
		std::memcpy(base + kAttributeCountOffset, &bogusCount, sizeof(bogusCount));
	}
	CallHook(PointerOf(knife));
	for (void* list : {plain, networked}) {
		auto* base = static_cast<std::uint8_t*>(list) + kAttributeVector;
		CheckEq(PeekRaw<int32_t>(base, kAttributeCountOffset), 999999, "an implausible count is not rewritten");
		CheckEq(PeekRaw<int32_t>(base, kAttributeCapacityOffset), 3, "the capacity stays intact too");
	}
	Check(FindStateChange(knife, itemAt + kItemAttributeList + kAttributeVector) == nullptr,
		  "an implausible header is not announced");
}

void TestSpawnSet() {
	Section("spawn set: gloves, music, pin, agent");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	config.pisex.pinsEnabled = true;
	ws::EconData econ;
	econ.agents[3] = ws::EconDefinition{3, "agent3", "models/agents/agent3.vmdl", {}};
	econ.agents[4] = ws::EconDefinition{4, "agent4", "models/agents/agent4.vmdl", {}};
	ws::TeamItems items;
	items.glove = MakeSkin(5027, 100, 9, 0.1f);
	items.agent = 3;
	items.pin = 4567;
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items, /*music=*/44);

	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	// The attribute writer and the deferred re-apply both need the installed hook + the cache view.
	g_lookupSnapshot = &snapshot;
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* pawn = g_slots[0].pawn;
	FakeEntity* controller = g_slots[0].controller;
	FakeEntity* inventory = g_slots[0].inventoryServices;
	pawn->model = "models/player/default.vmdl";

	engine.ApplySpawn(0, snapshot);

	// Gloves: the inline econ view of the pawn.
	void* gloves = pawn->memory.data() + kPawnEconGloves;
	CheckEq(PeekRaw<uint16_t>(gloves, kItemDefIndex), 5027, "the glove definition index");
	CheckEq(PeekRawByte(gloves, kItemDefIndex + 2), kSentinel, "the byte after the glove defindex is untouched");
	CheckEq(PeekRaw<uint8_t>(gloves, kItemInitialized), 1, "m_bInitialized is set for the gloves");
	CheckEq(PeekRawByte(gloves, kItemInitialized + 1), kSentinel, "the byte after m_bInitialized is untouched");
	const AttributeCall* glovePaint = FindAttribute(static_cast<std::uint8_t*>(gloves) + kItemAttributeList,
												   "set item texture prefab");
	Check(glovePaint != nullptr && glovePaint->value == 100.0f, "the glove paint reaches the plain list");
	Check(FindAttribute(static_cast<std::uint8_t*>(gloves) + kItemNetworkedAttributes, "set item texture wear") != nullptr,
		  "the glove wear reaches the networked list");

	// The bodygroup phases: off immediately, on from the deferred callback.
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,0"), "the first glove bodygroup phase");
	Check(!InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"), "the second phase waits for the timer");
	RunDeferred();
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"), "the second glove bodygroup phase runs");

	// Music: the controller's int32 and the inventory component's uint16, both announced.
	CheckEq(Peek<int32_t>(controller, kControllerMusicKit), 44, "the controller music kit id (int32)");
	CheckEq(PeekByte(controller, kControllerMusicKit + 4), kSentinel, "the byte after the music id is untouched");
	CheckEq(Peek<uint16_t>(inventory, kInventoryMusicId), 44, "the inventory music id (uint16)");
	CheckEq(PeekByte(inventory, kInventoryMusicId + 2), kSentinel, "the byte after the uint16 music id is untouched");
	Check(FindStateChange(controller, kControllerMusicKit) != nullptr, "the controller music change is announced");
	Check(FindStateChange(inventory, kInventoryMusicId) != nullptr, "the inventory music change is announced");

	// The pin: m_rank[5] as int32.
	CheckEq(Peek<int32_t>(inventory, kInventoryRank + 5 * 4), 4567, "the pin goes to m_rank[5]");
	CheckEq(PeekByte(inventory, kInventoryRank + 5 * 4 + 4), kSentinel, "the bytes after m_rank[5] are untouched");
	Check(FindStateChange(inventory, kInventoryRank) != nullptr, "the pin change is announced on the m_rank field");
	Check(FindStateChange(inventory, kInventoryRank + 5 * 4) == nullptr,
		  "the element offset is never announced (the engine cannot resolve it)");

	// The agent: the econ model lookup, the refresh trick, then the target model next frame.
	bool sawWantedModel = false;
	bool sawRefreshModel = false;
	for (const ModelCall& call : g_modelCalls) {
		sawWantedModel = sawWantedModel || call.model == "models/agents/agent3.vmdl";
		sawRefreshModel = sawRefreshModel || call.model == "models/agents/agent4.vmdl";
	}
	Check(sawRefreshModel, "the refresh trick uses another agent model");
	Check(sawWantedModel, "the wanted agent model is set");

	// Removing the agent restores the model captured when the player first spawned (the
	// reference's CaptureDefaultModel/TryGetDefaultModel pair).
	ws::TeamItems noAgent = items;
	noAgent.agent = 0;
	const ws::Snapshot reset = MakeSnapshot(kSteamId, 2, noAgent);
	ResetRecordings();
	engine.ApplySpawn(0, reset);
	RunDeferred();
	bool sawDefaultModel = false;
	for (const ModelCall& call : g_modelCalls) {
		sawDefaultModel = sawDefaultModel || call.model == "models/player/default.vmdl";
	}
	Check(sawDefaultModel, "the captured default model is restored when the agent is removed");

	// PinsEnabled = false skips the pin write.
	ws::Config noPins = config;
	noPins.pisex.pinsEnabled = false;
	ws::Engine off(noPins, econ);
	Check(off.Prepare(error), "prepare (pins off): " + error);
	const std::int32_t before = Peek<int32_t>(inventory, kInventoryRank + 5 * 4);
	off.ApplySpawn(0, snapshot);
	CheckEq(Peek<int32_t>(inventory, kInventoryRank + 5 * 4), before, "pinsEnabled=false leaves the pin alone");
	off.Shutdown();

	// Music kit 0 resets both fields (the reference's ResetMusicKit).
	ResetRecordings();
	const ws::Snapshot cleared = MakeSnapshot(kSteamId, 2, ws::TeamItems{}, 0);
	engine.ApplySpawn(0, cleared);
	CheckEq(Peek<int32_t>(controller, kControllerMusicKit), 0, "music kit 0 resets the controller field");
	CheckEq(Peek<uint16_t>(inventory, kInventoryMusicId), 0, "music kit 0 resets the inventory field");
}

void TestGloveRebuildSignal() {
	Section("glove rebuild signal (m_EconGloves container + m_nEconGlovesChanged)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	ws::TeamItems items;
	items.glove = MakeSkin(5027, 100, 9, 0.1f);
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);
	const ws::Snapshot cleared = MakeSnapshot(kSteamId, 2, ws::TeamItems{});

	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	g_lookupSnapshot = &snapshot;
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* pawn = g_slots[0].pawn;

	// The client rebuilds the glove model only when the counter moves, so applying the glove must
	// bump m_nEconGlovesChanged by one, announce the m_EconGloves container and announce the counter
	// itself - and the uint8 write must stay inside its own byte.
	Poke<std::uint8_t>(pawn, kPawnEconGlovesChanged, 7);
	engine.ApplySpawn(0, snapshot);
	CheckEq(PeekByte(pawn, kPawnEconGlovesChanged), 8, "applying the glove increments m_nEconGlovesChanged");
	CheckEq(PeekByte(pawn, kPawnEconGlovesChanged + 1), kSentinel, "only the counter's own byte is written");
	Check(FindStateChange(pawn, kPawnEconGloves + kItemAttributeList + kAttributeVector) != nullptr, "the glove attribute list is announced");
	Check(FindStateChange(pawn, kPawnEconGloves + kItemNetworkedAttributes + kAttributeVector) != nullptr,
		  "the networked glove attributes are announced");
	Check(FindStateChange(pawn, kPawnEconGloves) == nullptr,
		  "the m_EconGloves container itself is never announced (the engine cannot resolve it)");
	Check(FindStateChange(pawn, kPawnEconGlovesChanged) != nullptr, "the counter change is announced");

	// The reset (the site no longer configures gloves) must signal the same way or the game would
	// keep the removed glove's model. The field is a uint8, so 255 wraps to 0.
	ResetRecordings();
	Poke<std::uint8_t>(pawn, kPawnEconGlovesChanged, 255);
	engine.ApplySpawn(0, cleared);
	CheckEq(PeekRaw<uint16_t>(pawn->memory.data() + kPawnEconGloves, kItemDefIndex), 0, "the glove is reset");
	CheckEq(PeekByte(pawn, kPawnEconGlovesChanged), 0, "the reset wraps the counter at 255");
	Check(FindStateChange(pawn, kPawnEconGloves + kItemAttributeList + kAttributeVector) != nullptr, "the reset announces the attribute list");
	Check(FindStateChange(pawn, kPawnEconGloves) == nullptr, "the reset never announces the container");
	Check(FindStateChange(pawn, kPawnEconGlovesChanged) != nullptr, "the reset announces the counter");

	// A build whose schema lacks the counter keeps the previous behaviour: Prepare still succeeds,
	// the glove item is written, and only the (unresolvable) counter is left alone.
	engine.Shutdown();
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();
	g_hiddenSchemaField = "m_nEconGlovesChanged";
	ws::Engine noCounter(config, econ);
	Check(noCounter.Prepare(error), "prepare without the counter field: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* secondPawn = g_slots[0].pawn;
	Poke<std::uint8_t>(secondPawn, kPawnEconGlovesChanged, 3);
	noCounter.ApplySpawn(0, snapshot);
	CheckEq(PeekRaw<uint16_t>(secondPawn->memory.data() + kPawnEconGloves, kItemDefIndex), 5027,
			"the glove item is still applied without the counter field");
	CheckEq(PeekByte(secondPawn, kPawnEconGlovesChanged), 3, "the unresolvable counter is left alone");
	Check(FindStateChange(secondPawn, kPawnEconGloves + kItemAttributeList + kAttributeVector) != nullptr, "the attribute list is still announced");
	g_hiddenSchemaField.clear();
}

void TestPawnHandle() {
	Section("pawn handle: the controller's m_hPlayerPawn when EntPointerToEntHandle refuses the pawn");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	econ.agents[3] = ws::EconDefinition{3, "agent3", "models/agents/agent3.vmdl", {}};
	ws::TeamItems items;
	items.glove = MakeSkin(5027, 100, 9, 0.1f);
	items.agent = 3;
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);

	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	g_lookupSnapshot = &snapshot;
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* pawn = g_slots[0].pawn;
	pawn->model = "models/player/default.vmdl";

	// The live build: EntPointerToEntHandle answers INVALID_EHANDLE_INDEX for the pawn pointer
	// (IsEntityPtr rejects it, ref/s2sdk-src/src/export/entities.cpp:51-57). The old code read the
	// handle exactly that way, so every glove bodygroup phase and the whole agent path returned
	// early while the counters said they ran.
	g_rejectPawnPointerHandles = true;

	engine.ApplySpawn(0, snapshot);
	RunDeferred();

	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,0"),
		  "the immediate glove phase runs through the handle");
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"),
		  "the deferred glove phase runs through the handle");
	Check(engine.Stats().gloveApplied >= 1, "the glove was applied");
	CheckEq(static_cast<long long>(engine.Stats().gloveNoHandle), 0, "the glove path found the pawn handle");

	CheckEq(static_cast<long long>(engine.Stats().agentNoHandle), 0, "the agent path found the pawn handle");
	Check(engine.Stats().agentApplied >= 1, "the agent pass was scheduled");
	Check(engine.Stats().agentWritten >= 1, "the wanted model was written");
	bool wroteAgent = false;
	for (const ModelCall& call : g_modelCalls) {
		wroteAgent = wroteAgent || call.model == "models/agents/agent3.vmdl";
	}
	Check(wroteAgent, "the agent model reached the client path");

	// The ws_status diagnostic shows both readings: the controller's handle works, the pointer
	// conversion still refuses the same pawn.
	const ws::Engine::PawnHandles handles = engine.PawnHandleReport(0);
	CheckEq(handles.viaController, pawn->handle, "the controller's m_hPlayerPawn handle");
	CheckEq(handles.viaPointer, -1, "the pointer conversion refuses the same pawn");

	// The fallback branch: a build without the controller field (hidden) must keep working through
	// the pointer conversion, so the engine degrades instead of losing gloves/agents entirely.
	engine.Shutdown();
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();
	g_hiddenSchemaField = "m_hPlayerPawn";
	ws::Engine fallback(config, econ);
	Check(fallback.Prepare(error), "prepare without m_hPlayerPawn: " + error);
	g_lookupSnapshot = &snapshot;
	Check(fallback.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* secondPawn = g_slots[0].pawn;
	secondPawn->model = "models/player/default.vmdl";
	fallback.ApplySpawn(0, snapshot);
	RunDeferred();
	Check(InputIssued(secondPawn, "SetBodygroup", "first_or_third_person,1"),
		  "the pointer conversion carries the fallback path");
	CheckEq(static_cast<long long>(fallback.Stats().gloveNoHandle), 0, "the fallback found the pawn handle");
	Check(fallback.Stats().agentWritten >= 1, "the agent model is written through the fallback");
	g_hiddenSchemaField.clear();
	g_rejectPawnPointerHandles = false;
}

void TestNegativeHandles() {
	Section("negative valid handles (the serial counter past 65535)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	// Live example: a pawn handle of -921534371 = 0xC912805D = index 93, serial 102949. The serial
	// sits in the high bits of a CEntityHandle, so a few minutes into a server's life every handle
	// is a negative int - and still valid; only -1 (INVALID_EHANDLE_INDEX) is invalid. Every
	// sign-based `> 0` / `<= 0` check dropped these entities entirely.
	constexpr int32_t kNegativePawn = -921534371;
	constexpr int32_t kNegativeWeapon = -921534370;

	ws::Config config;
	ws::EconData econ;
	econ.items[kAk47] = ws::ItemDefinition{kAk47, "weapon_ak47", "", 0, {}};
	econ.agents[3] = ws::EconDefinition{3, "agent3", "models/agents/agent3.vmdl", {}};
	ws::TeamItems items;
	items.weapons[kAk47] = MakeSkin(kAk47, 44, 123, 0.25f);
	items.glove = MakeSkin(5027, 100, 9, 0.1f);
	items.agent = 3;
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);

	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	g_lookupSnapshot = &snapshot;
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* pawn = g_slots[0].pawn;
	pawn->model = "models/player/default.vmdl";
	FakeEntity* weapon = AddWeapon(0, kAk47, "weapon_ak47", 1);

	// Re-tag the pawn and its weapon with the negative handles, updating every m_h* field that
	// already cached them the way the game would (the controller's m_hPlayerPawn, the weapon's
	// m_hOwnerEntity, the slot's weapon list).
	const int32_t oldWeaponHandle = weapon->handle;
	pawn->handle = kNegativePawn;
	Poke<int32_t>(g_slots[0].controller, kControllerPlayerPawn, kNegativePawn);
	weapon->handle = kNegativeWeapon;
	Poke<int32_t>(weapon, kWeaponOwner, kNegativePawn);
	for (int32_t& stored : g_slots[0].weapons) {
		if (stored == oldWeaponHandle) {
			stored = kNegativeWeapon;
		}
	}

	// The spawn set through the negative pawn handle: the glove bodygroup phases (immediate and
	// deferred), the glove itself, and the agent path (GetEntityModel takes the handle).
	engine.ApplySpawn(0, snapshot);
	RunDeferred();
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,0"),
		  "the immediate glove phase runs with a negative pawn handle");
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"),
		  "the deferred glove phase runs with a negative pawn handle");
	CheckEq(static_cast<long long>(engine.Stats().gloveNoHandle), 0, "the negative pawn handle is not counted missing");
	CheckEq(static_cast<long long>(engine.Stats().agentNoHandle), 0, "the agent path accepts the negative pawn handle");
	Check(engine.Stats().agentWritten >= 1, "the agent model is written through the negative handle");
	CheckEq(static_cast<long long>(engine.PawnHandleReport(0).viaController), kNegativePawn,
			"ws_status reports the negative controller handle");
	CheckEq(static_cast<long long>(engine.PawnHandleReport(0).viaPointer), kNegativePawn,
			"the pointer conversion reports the same negative handle");

	// The hook's fallback chain: m_hOwnerEntity (negative) -> pawn -> controller -> slot, then the
	// weapon's own negative handle for the bodygroup input.
	ResetRecordings();
	CallHook(PointerOf(weapon));
	Check(CountAttribute("set item texture prefab") > 0, "the hook applies the skin through a negative owner handle");
	CheckEq(static_cast<long long>(engine.Stats().hookGun), 1, "the hook counted the gun apply");
	Check(InputIssued(weapon, "SetBodygroup", "body,0"), "the bodygroup input uses the negative weapon handle");

	// The owned-weapon pass enumerates the slot's weapon list and matches the owner handle - both
	// negative here.
	ResetRecordings();
	engine.ApplyOwnedWeapons(0, snapshot);
	Check(CountAttribute("set item texture prefab") > 0, "the owned-weapon pass applies through the negative handle");
}

void TestRefreshAndRegive() {
	Section("RefreshInventory: the regive keeps the ammo");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	econ.items[kAk47] = ws::ItemDefinition{kAk47, "weapon_ak47", "", 0, {}};
	econ.items[kTaser] = ws::ItemDefinition{kTaser, "weapon_taser", "", 0, {}};
	ws::TeamItems items;
	ws::Skin ak = MakeSkin(kAk47, 44, 123, 0.25f, ws::kStatTrakQuality);
	items.weapons[kAk47] = ak;
	items.weapons[kTaser] = MakeSkin(kTaser, 0, 0, 0.0f);
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);

	g_lookupSnapshot = &snapshot;
	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);

	FakeEntity* akWeapon = AddWeapon(0, kAk47, "weapon_ak47", 1);
	FakeEntity* taser = AddWeapon(0, kTaser, "weapon_taser", 1);
	FakeEntity* knifeWeapon = AddWeapon(0, 42, "weapon_knife", 2);

	// The ammo state the regive must preserve: the inline int32[2] reserve (both values) and the clip.
	SetReserveAmmo(akWeapon, 90, 3);
	Poke<std::int32_t>(akWeapon, kWeaponClip, 25);
	Poke<float>(taser, kTaserFireTime, 12.5f);
	Poke<std::int32_t>(taser, kTaserLastAttack, 4242);

	// The site first had a knife skin, so the held knife carries this engine's skin. The snapshot
	// below then drops it: that removal is what the knife regive of the second pass clears.
	ws::TeamItems withKnife = items;
	withKnife.knife = MakeSkin(kKnife, 77, 5, 0.5f);
	const ws::Snapshot knifeSnapshot = MakeSnapshot(kSteamId, 2, withKnife);
	g_lookupSnapshot = &knifeSnapshot;
	engine.RefreshInventory(0, knifeSnapshot);
	RunDeferred();
	CheckEq(Peek<uint16_t>(knifeWeapon, kItemOffset + kItemDefIndex), kKnife,
			"the first pass applies the knife skin before regiving it");
	ResetRecordings();

	g_lookupSnapshot = &snapshot; // the site dropped the knife skin
	engine.RefreshInventory(0, snapshot);
	CheckEq(static_cast<long long>(g_weaponCalls.size()), 0, "nothing is given or removed on the command frame");
	RunDeferred();

	int removedAk = 0;
	int givenAk = 0;
	int givenTaser = 0;
	int givenKnife = 0;
	int selectedKnife = 0;
	for (const WeaponCall& call : g_weaponCalls) {
		removedAk += call.action == "remove" ? 1 : 0; // one per regiven weapon (AK, taser, knife)
		givenAk += call.action == "give" && call.name == "weapon_ak47" ? 1 : 0;
		givenTaser += call.action == "give" && call.name == "weapon_taser" ? 1 : 0;
		givenKnife += call.action == "give" && call.name == "weapon_knife" ? 1 : 0;
		selectedKnife += call.action == "select" ? 1 : 0;
	}
	CheckEq(removedAk, 3, "each regiven weapon is removed first (gun, taser, knife)");
	CheckEq(givenAk, 1, "the AK is given again");
	CheckEq(givenTaser, 1, "the taser is given again");
	CheckEq(givenKnife, 1, "the knife is given again");
	CheckEq(selectedKnife, 1, "the fresh knife is selected");

	// The fresh AK: the clip and BOTH inline reserve values are restored, and the skin was applied by
	// our own hook inside GiveNamedItem.
	FakeEntity* freshAk = nullptr;
	FakeEntity* freshTaser = nullptr;
	for (const WeaponCall& call : g_weaponCalls) {
		if (call.action != "give") {
			continue;
		}
		if (call.name == "weapon_ak47") {
			freshAk = FromHandle(call.handle);
		}
		if (call.name == "weapon_taser") {
			freshTaser = FromHandle(call.handle);
		}
	}
	Check(freshAk != nullptr, "the fresh AK exists");
	if (freshAk != nullptr) {
		CheckEq(Peek<int32_t>(freshAk, kWeaponClip), 25, "the clip survives the regive");
		CheckEq(ReserveAmmoAt(freshAk, 0), 90, "the first inline reserve value survives the regive");
		CheckEq(ReserveAmmoAt(freshAk, 1), 3, "the second inline reserve value survives the regive");
		CheckEq(static_cast<int>(freshAk->memory[kWeaponReserveAmmo - 1]), static_cast<int>(kSentinel),
				"the byte before the reserve array is untouched");
		CheckEq(static_cast<int>(freshAk->memory[kWeaponReserveAmmo + 8]), static_cast<int>(kSentinel),
				"the byte after the reserve array is untouched");
		const AttributeCall* applied = FindAttribute(freshAk->memory.data() + kItemOffset + kItemAttributeList,
													 "set item texture prefab");
		Check(applied != nullptr, "the regive triggers the hook and the skin lands on the fresh weapon");
	}
	Check(freshTaser != nullptr, "the fresh taser exists");
	if (freshTaser != nullptr) {
		CheckFloat(Peek<float>(freshTaser, kTaserFireTime), 12.5f, "the taser fire time survives the regive");
		CheckEq(Peek<int32_t>(freshTaser, kTaserLastAttack), 4242, "the taser last attack tick survives the regive");
	}

	// A reserve value that would be a wild pointer if it were read as one must be copied as DATA:
	// the field is an inline int32[2] (fixture + dump), so a sentinel-pattern value has to land in
	// the fresh weapon byte-for-byte. Under the old pointer interpretation this would have skipped
	// the write (page guard) or crashed.
	ResetRecordings();
	FakeEntity* secondAk = AddWeapon(0, kAk47, "weapon_ak47", 1);
	Poke<std::int32_t>(secondAk, kWeaponClip, 7);
	SetReserveAmmo(secondAk, static_cast<std::int32_t>(0xAAAAAAAAu), 5);
	engine.RefreshInventory(0, snapshot);
	RunDeferred();
	FakeEntity* freshSecond = nullptr;
	for (const WeaponCall& call : g_weaponCalls) {
		if (call.action == "give" && call.name == "weapon_ak47") {
			freshSecond = FromHandle(call.handle);
		}
	}
	Check(freshSecond != nullptr, "the second AK is regiven");
	if (freshSecond != nullptr) {
		CheckEq(Peek<int32_t>(freshSecond, kWeaponClip), 7, "the clip is restored on the second regive");
		CheckEq(ReserveAmmoAt(freshSecond, 0), static_cast<int>(0xAAAAAAAAu),
				"a pointer-looking reserve value is copied as data, not dereferenced");
		CheckEq(ReserveAmmoAt(freshSecond, 1), 5, "the second reserve value is copied too");
	}
}

void TestDeferredGuards() {
	Section("deferred work is cancelled by pawn/team/map/steam changes");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	config.pisex.pinsEnabled = true;
	ws::EconData econ;
	econ.agents[3] = ws::EconDefinition{3, "agent3", "models/agents/agent3.vmdl", {}};
	econ.agents[4] = ws::EconDefinition{4, "agent4", "models/agents/agent4.vmdl", {}};
	ws::TeamItems items;
	items.glove = MakeSkin(5027, 100, 9, 0.1f);
	items.agent = 3;
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);

	// Every block below prepares a fresh world: a live player at slot 0, an engine whose spawn path
	// needs no hook, and a first ApplySpawn that leaves a deferred glove/agent pass pending.

	// --- a new pawn (respawn with the same steamId) cancels the deferred pass ---------------------
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Engine engine(config, econ);
		std::string error;
		Check(engine.Prepare(error), "prepare: " + error);
		MakePlayer(0, kSteamId);
		FakeEntity* firstPawn = g_slots[0].pawn;
		firstPawn->model = "models/player/default.vmdl";

		engine.ApplySpawn(0, snapshot);
		Check(InputIssued(firstPawn, "SetBodygroup", "first_or_third_person,0"), "the first glove phase ran");
		const std::size_t modelCallsBefore = g_modelCalls.size();
		const std::size_t inputCallsBefore = g_inputCalls.size();

		// Respawn: the same player gets a new pawn entity (same steamId, same team). The engine's
		// pawn identity is the controller's m_hPlayerPawn, so a real respawn moves that too.
		FakeEntity* newPawn = NewEntity("CCSPlayerPawn");
		newPawn->model = "models/player/default.vmdl";
		Poke<int32_t>(newPawn, kPawnController, g_slots[0].controller->handle);
		Poke<void*>(newPawn, kPawnWeaponServices, PointerOf(g_slots[0].weaponServices));
		g_slots[0].pawn = newPawn;
		Poke<int32_t>(g_slots[0].controller, kControllerPlayerPawn, newPawn->handle);

		RunDeferred();
		CheckEq(static_cast<long long>(g_modelCalls.size()), static_cast<long long>(modelCallsBefore),
				"a deferred agent pass does not touch the new pawn");
		CheckEq(static_cast<long long>(g_inputCalls.size()), static_cast<long long>(inputCallsBefore),
				"a deferred glove phase does not run for the new pawn");
		Check(!InputIssued(newPawn, "SetBodygroup", "first_or_third_person,1"),
			  "the second glove phase is cancelled by the respawn");
	}

	// --- a team switch cancels it ------------------------------------------------------------------
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Engine engine(config, econ);
		std::string error;
		Check(engine.Prepare(error), "prepare: " + error);
		MakePlayer(0, kSteamId);
		FakeEntity* pawn = g_slots[0].pawn;
		pawn->model = "models/player/default.vmdl";

		engine.ApplySpawn(0, snapshot);
		const std::size_t modelCallsBefore = g_modelCalls.size();
		g_slots[0].team = 3; // CT now
		RunDeferred();
		CheckEq(static_cast<long long>(g_modelCalls.size()), static_cast<long long>(modelCallsBefore),
				"a deferred pass does not run after a team switch");
		Check(!InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"), "no glove phase after the team switch");
	}

	// --- a map change cancels it ---------------------------------------------------------------------
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Engine engine(config, econ);
		std::string error;
		Check(engine.Prepare(error), "prepare: " + error);
		MakePlayer(0, kSteamId);
		FakeEntity* pawn = g_slots[0].pawn;
		pawn->model = "models/player/default.vmdl";

		engine.ApplySpawn(0, snapshot);
		const std::size_t modelCallsBefore = g_modelCalls.size();
		pisex::OnMapEndNotify();
		RunDeferred();
		CheckEq(static_cast<long long>(g_modelCalls.size()), static_cast<long long>(modelCallsBefore),
				"no deferred work runs between map end and the next map start");
		pisex::OnMapStartNotify();
		pisex::Poll();
	}

	// --- a different steamId (reconnect / slot reuse) cancels it -------------------------------------
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Engine engine(config, econ);
		std::string error;
		Check(engine.Prepare(error), "prepare: " + error);
		MakePlayer(0, kSteamId);
		FakeEntity* pawn = g_slots[0].pawn;
		pawn->model = "models/player/default.vmdl";

		engine.ApplySpawn(0, snapshot);
		const std::size_t modelCallsBefore = g_modelCalls.size();
		g_slots[0].steamId = 76561198000000099ULL; // somebody else took the slot
		RunDeferred();
		CheckEq(static_cast<long long>(g_modelCalls.size()), static_cast<long long>(modelCallsBefore),
				"a deferred pass does not run for a different player");
	}

	// --- Shutdown cancels it (and stops the pending timers) ------------------------------------------
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Engine engine(config, econ);
		std::string error;
		Check(engine.Prepare(error), "prepare: " + error);
		MakePlayer(0, kSteamId);
		FakeEntity* pawn = g_slots[0].pawn;
		pawn->model = "models/player/default.vmdl";

		engine.ApplySpawn(0, snapshot);
		const std::size_t modelCallsBefore = g_modelCalls.size();
		engine.Shutdown();
		RunDeferred();
		CheckEq(static_cast<long long>(g_modelCalls.size()), static_cast<long long>(modelCallsBefore),
				"no deferred work runs after Shutdown");
		CheckEq(g_killCalls, 2, "Shutdown stopped the two pending timers (the glove phase and the re-apply)");
	}

	// --- the deferred regive is cancelled by a new pawn too ------------------------------------------
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::EconData econ2;
		econ2.items[kAk47] = ws::ItemDefinition{kAk47, "weapon_ak47", "", 0, {}};
		ws::TeamItems akItems;
		akItems.weapons[kAk47] = MakeSkin(kAk47, 44, 123, 0.25f);
		const ws::Snapshot akSnapshot = MakeSnapshot(kSteamId, 2, akItems);
		ws::Engine engine(config, econ2);
		std::string error;
		Check(engine.Prepare(error), "prepare: " + error);
		MakePlayer(0, kSteamId);
		AddWeapon(0, kAk47, "weapon_ak47", 1);

		engine.RefreshInventory(0, akSnapshot);
		// A respawn between the command and the deferred regive (the controller's pawn handle moves
		// with it, like the engine's own respawn).
		FakeEntity* newPawn = NewEntity("CCSPlayerPawn");
		Poke<int32_t>(newPawn, kPawnController, g_slots[0].controller->handle);
		Poke<void*>(newPawn, kPawnWeaponServices, PointerOf(g_slots[0].weaponServices));
		g_slots[0].pawn = newPawn;
		Poke<int32_t>(g_slots[0].controller, kControllerPlayerPawn, newPawn->handle);

		RunDeferred();
		int weaponCalls = 0;
		for (const WeaponCall& call : g_weaponCalls) {
			weaponCalls += call.action == "give" || call.action == "remove" ? 1 : 0;
		}
		CheckEq(weaponCalls, 0, "the deferred regive does not give/remove anything after a respawn");
	}
}

void TestTimerBookkeeping() {
	Section("timer bookkeeping (one-shots are reaped before the next schedule)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* pawn = g_slots[0].pawn;
	pawn->model = "models/player/default.vmdl";

	ws::TeamItems items;
	items.glove = MakeSkin(5027, 100, 9, 0.1f);
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);
	// The re-apply body reads the cache through the hook lookup, so the hook has to be installed
	// for the third fire (the one the re-apply schedules) to exist at all.
	g_lookupSnapshot = &snapshot;
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);

	// Round 1: a spawn schedules two one-shots (glove phase B and the spawn re-apply); the re-apply
	// itself schedules the next glove phase, so the drain fires three. Every fire must drop its
	// record before pisexcompat stops and frees the CTimer - the engine must never touch a dead
	// timer afterwards, which shows up as exactly one kill per fire.
	engine.ApplySpawn(0, snapshot);
	CheckEq(static_cast<long long>(g_engineTimers.size()), 2, "two pending timers after the spawn");
	int killsBefore = g_killCalls;
	int fired = DrainTimers(8);
	CheckEq(g_killCalls - killsBefore, fired, "each fired one-shot is killed exactly once by pisexcompat");
	CheckEq(static_cast<long long>(g_engineTimers.size()), 0, "no timer is left pending after the drain");
	CheckEq(fired, 3, "the spawn re-apply schedules and runs the next glove phase");

	// Round 2: a second burst after the reaping. The bookkeeping must not reference the freed ones.
	ResetRecordings();
	engine.ApplySpawn(0, snapshot);
	fired = DrainTimers(8);
	CheckEq(fired, 3, "a second burst behaves like the first");
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"), "the second burst applied its glove phase");

	// Shutdown with two pending timers: it stops exactly the live ones (the fired ones are gone and
	// their records were dropped inside their callbacks).
	engine.ApplySpawn(0, snapshot);
	const int pending = static_cast<int>(g_engineTimers.size());
	CheckEq(pending, 2, "two pending timers before the shutdown");
	const int killsBeforeShutdown = g_killCalls;
	engine.Shutdown();
	CheckEq(g_killCalls - killsBeforeShutdown, pending, "Shutdown stops exactly the still-pending timers");
	CheckEq(static_cast<long long>(g_engineTimers.size()), 0, "nothing is left pending after Shutdown");
	RunDeferred();
	CheckEq(g_killCalls - killsBeforeShutdown, pending, "a shutdown engine schedules nothing new");

	// A fresh engine over the same fake world still works (the ids restart, no state leaks).
	ResetRecordings();
	ws::Engine second(config, econ);
	Check(second.Prepare(error), "prepare (second engine): " + error);
	second.ApplySpawn(0, snapshot);
	RunDeferred();
	Check(InputIssued(pawn, "SetBodygroup", "first_or_third_person,1"),
		  "a new engine schedules and runs its own deferred work");
}

void TestRemovalCleanup() {
	Section("website removals (!wp): stale skins, gloves and knifes are cleared");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	econ.items[kAk47] = ws::ItemDefinition{kAk47, "weapon_ak47", "", 0, {}};
	ws::TeamItems loaded;
	loaded.weapons[kAk47] = MakeSkin(kAk47, 44, 123, 0.25f);
	loaded.knife = MakeSkin(kKnife, 77, 5, 0.5f);
	loaded.glove = MakeSkin(5027, 100, 9, 0.1f);
	const ws::Snapshot loadedSnapshot = MakeSnapshot(kSteamId, 2, loaded);
	const ws::Snapshot emptySnapshot = MakeSnapshot(kSteamId, 2, ws::TeamItems{});

	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	g_lookupSnapshot = &loadedSnapshot;
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* pawn = g_slots[0].pawn;
	FakeEntity* ak = AddWeapon(0, kAk47, "weapon_ak47", 1);
	AddWeapon(0, 42, "weapon_knife", 2);
	void* gloves = pawn->memory.data() + kPawnEconGloves;

	// Load: the glove is applied and the AK/knife get their skins (the knife by the deferred pass).
	engine.RefreshInventory(0, loadedSnapshot);
	RunDeferred();
	CheckEq(PeekRaw<uint16_t>(gloves, kItemDefIndex), 5027, "the loaded glove is applied");
	CheckEq(PeekRaw<uint8_t>(gloves, kItemInitialized), 1, "the loaded glove is initialized");

	// The site drops everything. The glove is reset (definition 0, m_bInitialized false) so the game
	// rebuilds the player's default gloves; the AK and the held knife are regiven once, so the fresh
	// copies come back with the game's default attributes instead of the removed skin.
	ResetRecordings();
	g_lookupSnapshot = &emptySnapshot;
	engine.RefreshInventory(0, emptySnapshot);
	RunDeferred();

	CheckEq(PeekRaw<uint16_t>(gloves, kItemDefIndex), 0, "the removed glove definition is cleared");
	CheckEq(PeekRaw<uint8_t>(gloves, kItemInitialized), 0, "m_bInitialized is cleared for the default rebuild");
	Check(FindStateChange(pawn, kPawnEconGloves + kItemDefIndex) != nullptr, "the glove reset is announced");

	int removed = 0;
	int givenAk = 0;
	int givenKnife = 0;
	int selectedKnife = 0;
	for (const WeaponCall& call : g_weaponCalls) {
		removed += call.action == "remove" ? 1 : 0; // the gun and the knife, once each
		givenAk += call.action == "give" && call.name == "weapon_ak47" ? 1 : 0;
		givenKnife += call.action == "give" && call.name == "weapon_knife" ? 1 : 0;
		selectedKnife += call.action == "select" ? 1 : 0;
	}
	CheckEq(removed, 2, "the gun and the knife with the removed skins are regiven once each");
	CheckEq(givenAk, 1, "the fresh gun is given once");
	CheckEq(givenKnife, 1, "the knife with the removed skin is regiven once");
	CheckEq(selectedKnife, 1, "the fresh knife is selected once");
	(void)ak;

	// A second !wp has nothing left to clear: no weapon may be regiven again and no glove field is
	// written again.
	ResetRecordings();
	engine.RefreshInventory(0, emptySnapshot);
	RunDeferred();
	int weaponCalls = 0;
	for (const WeaponCall& call : g_weaponCalls) {
		weaponCalls += call.action == "give" || call.action == "remove" ? 1 : 0;
	}
	CheckEq(weaponCalls, 0, "a second !wp with the same empty site data regives nothing");
	Check(FindStateChange(pawn, kPawnEconGloves + kItemDefIndex) == nullptr, "the glove reset does not repeat");
	CheckEq(static_cast<long long>(g_weaponCalls.size()), 0, "no item is touched at all the second time");

	// A glove this engine never wrote is left exactly as it is (another plugin's / the game's).
	{
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Config other = config;
		ws::Engine fresh(other, econ);
		std::string freshError;
		Check(fresh.Prepare(freshError), "prepare (foreign glove): " + freshError);
		MakePlayer(0, kSteamId);
		FakeEntity* freshPawn = g_slots[0].pawn;
		void* foreignGloves = freshPawn->memory.data() + kPawnEconGloves;
		PokeRaw<uint16_t>(foreignGloves, kItemDefIndex, 5030); // somebody else's gloves
		PokeRaw<uint8_t>(foreignGloves, kItemInitialized, 1);
		fresh.RefreshInventory(0, emptySnapshot);
		RunDeferred();
		CheckEq(PeekRaw<uint16_t>(foreignGloves, kItemDefIndex), 5030, "gloves this engine never applied are untouched");
		CheckEq(PeekRaw<uint8_t>(foreignGloves, kItemInitialized), 1, "their m_bInitialized is untouched too");
		Check(FindStateChange(freshPawn, kPawnEconGloves + kItemDefIndex) == nullptr,
			  "nothing is announced for a glove this engine never applied");
	}

	// A knife the player does not hold is never created, even when the site configures one.
	{
		// The GiveNamedItem detour is process-wide and InstallHook refuses a second owner
		// (Impl::s_active): release this test's engine first, then the nested one may install.
		engine.Shutdown();
		ResetFakeState();
		InstallFakeEngine();
		pisex::Poll();
		ws::Engine fresh(config, econ);
		std::string freshError;
		const bool noKnifePrepared = fresh.Prepare(freshError);
		Check(noKnifePrepared, "prepare (no knife): " + freshError);
		g_lookupSnapshot = &loadedSnapshot;
		const bool noKnifeInstalled = fresh.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, freshError);
		Check(noKnifeInstalled, "install (no knife): " + freshError);
		MakePlayer(0, kSteamId);
		AddWeapon(0, kAk47, "weapon_ak47", 1); // guns only, no knife
		ResetRecordings();
		fresh.RefreshInventory(0, loadedSnapshot);
		RunDeferred();
		int gotKnife = 0;
		for (const WeaponCall& call : g_weaponCalls) {
			gotKnife += call.action == "give" && call.name == "weapon_knife" ? 1 : 0;
		}
		CheckEq(gotKnife, 0, "a player without a knife is not given one");
	}
}

void TestActiveDefinitionAndStatTrak() {
	Section("ActiveDefinition and SetStatTrak");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	// The native attribute writer comes from the installed gamedata signature.
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* ak = AddWeapon(0, kAk47, "weapon_ak47", 1);
	Poke<int32_t>(g_slots[0].weaponServices, kServicesActive, ak->handle);

	CheckEq(engine.ActiveDefinition(0), kAk47, "ActiveDefinition reads the active weapon's item view");

	ws::Skin skin = MakeSkin(kAk47, 44, 1, 0.1f, ws::kStatTrakQuality);
	skin.stattrak = 21;
	engine.SetStatTrak(0, skin);
	const AttributeCall* stattrak = FindAttribute(ak->memory.data() + kItemOffset + kItemAttributeList, "kill eater");
	Check(stattrak != nullptr, "SetStatTrak writes the counter");
	if (stattrak != nullptr) {
		CheckEq(static_cast<long long>(std::bit_cast<std::int32_t>(stattrak->value)), 21, "the counter bits are right");
	}

	// Bots never get anything, and the public handle API validates the handle.
	g_slots[0].bot = true;
	ResetRecordings();
	engine.SetStatTrak(0, skin);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "SetStatTrak skips bots");
	g_slots[0].bot = false;

	Check(!engine.ActiveDefinition(-1), "ActiveDefinition refuses an out-of-range slot");

	ResetRecordings();
	Check(engine.ApplyWeapon(ak->handle, skin, /*knife=*/false), "ApplyWeapon(handle) succeeds");
	CheckEq(CountAttribute("set item texture prefab"), 2, "the handle path writes both lists");
	Check(!engine.ApplyWeapon(-1, skin, false), "an invalid handle is refused");

	// The engine's destructor tears the hook down too (the runtime may only reset its pointer).
	// The detour is process-wide and InstallHook refuses a second owner (Impl::s_active): release
	// the one this test installed first, or the scoped engine's install is refused.
	engine.Shutdown();
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();
	{
		ws::Engine scoped(config, econ);
		const bool scopedPrepared = scoped.Prepare(error);
		Check(scopedPrepared, "prepare (scope): " + error);
		const bool scopedInstalled =
			scoped.InstallHook("/tmp/gamedata.jsonc", [](int) -> const ws::Snapshot* { return nullptr; }, error);
		Check(scopedInstalled, "install (scope): " + error);
	}
	CheckEq(g_removeCallbackCalls, 1, "the destructor removes the callback");
	CheckEq(g_unhookCalls, 1, "the destructor unhooks the target");
}

void TestApplyOwnedWeapons() {
	Section("ApplyOwnedWeapons (pickup re-apply, no give/remove)");
	ResetFakeState();
	InstallFakeEngine();
	pisex::Poll();

	ws::Config config;
	ws::EconData econ;
	econ.items[kAk47] = ws::ItemDefinition{kAk47, "weapon_ak47", "", 0, {}};
	ws::TeamItems items;
	ws::Skin ak = MakeSkin(kAk47, 44, 123, 0.25f);
	items.weapons[kAk47] = ak;
	items.knife = MakeSkin(kKnife, 77, 5, 0.5f);
	const ws::Snapshot snapshot = MakeSnapshot(kSteamId, 2, items);
	const ws::Snapshot otherTeam = MakeSnapshot(kSteamId, 3, ws::TeamItems{});

	ws::Engine engine(config, econ);
	std::string error;
	Check(engine.Prepare(error), "prepare: " + error);
	// The native attribute writer comes from the installed gamedata signature.
	Check(engine.InstallHook("/tmp/gamedata.jsonc", &LookupCurrent, error), "install: " + error);
	MakePlayer(0, kSteamId);
	FakeEntity* akWeapon = AddWeapon(0, kAk47, "weapon_ak47", 1);
	FakeEntity* knife = AddWeapon(0, 42, "weapon_knife", 2);

	engine.ApplyOwnedWeapons(0, snapshot);
	Check(FindAttribute(akWeapon->memory.data() + kItemOffset + kItemAttributeList, "set item texture prefab") != nullptr,
		  "the owned AK gets its skin");
	CheckEq(Peek<uint16_t>(knife, kItemOffset + kItemDefIndex), kKnife, "the owned knife gets its skin");
	CheckEq(InputIssued(knife, "ChangeSubclass", "507") ? 1 : 0, 1, "the knife swap uses ChangeSubclass");
	int weaponCalls = 0;
	for (const WeaponCall& call : g_weaponCalls) {
		weaponCalls += call.action == "give" || call.action == "remove" ? 1 : 0;
	}
	CheckEq(weaponCalls, 0, "nothing is given, removed or reselected");

	// A weapon that is not owned by this player (dropped, or picked up by somebody else) is skipped.
	Poke<int32_t>(knife, kWeaponOwner, -1);
	Poke<int32_t>(akWeapon, kWeaponOwner, -1);
	ResetRecordings();
	engine.ApplyOwnedWeapons(0, snapshot);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a dropped weapon is not written through");
	Poke<int32_t>(knife, kWeaponOwner, g_slots[0].pawn->handle);
	Poke<int32_t>(akWeapon, kWeaponOwner, g_slots[0].pawn->handle);

	// A snapshot for another player is refused (same-stack identity check).
	ResetRecordings();
	ws::Snapshot foreign = snapshot;
	foreign.steamId = 12345;
	engine.ApplyOwnedWeapons(0, foreign);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a snapshot of another player is not applied");

	// The team in the snapshot is not the player's: the empty CT table means nothing to apply.
	ResetRecordings();
	engine.ApplyOwnedWeapons(0, otherTeam);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "the current team's table is used");

	// A bot, an empty slot and a missing pawn are all refused.
	ResetRecordings();
	g_slots[0].bot = true;
	engine.ApplyOwnedWeapons(0, snapshot);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "bots are skipped");
	g_slots[0].bot = false;

	ResetRecordings();
	engine.ApplyOwnedWeapons(7, snapshot);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "an empty slot is skipped");

	ResetRecordings();
	FakeEntity* pawnBefore = g_slots[0].pawn;
	g_slots[0].pawn = nullptr;
	engine.ApplyOwnedWeapons(0, snapshot);
	CheckEq(static_cast<long long>(g_attributeCalls.size()), 0, "a player without a pawn is skipped");
	g_slots[0].pawn = pawnBefore;
}

} // namespace

int main() {
	std::printf("weaponskins engine_tests\n");

	TestDecodeHelpers();
	TestPrepare();
	TestInstallAndTeardown();
	TestHookGuards();
	TestHookOwnerFromServices();
	TestGunApply();
	TestKnifeApply();
	TestSpawnSet();
	TestGloveRebuildSignal();
	TestPawnHandle();
	TestNegativeHandles();
	TestRefreshAndRegive();
	TestDeferredGuards();
	TestTimerBookkeeping();
	TestRemovalCleanup();
	TestActiveDefinitionAndStatTrak();
	TestApplyOwnedWeapons();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
