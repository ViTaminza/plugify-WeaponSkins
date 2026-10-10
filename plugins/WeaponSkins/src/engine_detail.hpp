#pragma once
// WeaponSkins engine internals shared by src/engine.cpp and tests/engine_tests.cpp.
//
// Everything in here is pure, allocation-free and free of s2sdk calls, so the unit tests can
// exercise the real byte-level code (widths, name strings, the vector-clear guard, the page
// guard) without a server. The rules the engine obeys, and that this header documents:
//
//   * Raw fields are written with the width the reference used (the C# port's Core/GameEntities.cs).
//     A wrong width corrupts the neighbouring field.
//   * Attribute names are the exact strings the original sent to
//     CAttributeList::SetOrAddAttributeValueByName (HookInventoryUpdateService.cs:462-520).
//   * Any pointer that comes from a schema field and is then dereferenced goes through the
//     mincore page guard first (the same pattern plugins/fortnite_emotes uses): a wrong layout
//     guess must never take the server down.
//   * Fields that only *look* like pointers are treated as the data the schema dump says they are:
//     m_pReserveAmmo is an inline int32[2], and the attribute vector's header is the 104-byte
//     CUtlVectorEmbeddedNetworkVar layout (count +0, data +8, capacity +16), not a CUtlVector.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <sys/mman.h>
#include <unistd.h>

namespace ws::detail {

// --- field widths ------------------------------------------------------------------------------
//
// Verified against a saved SwiftlyS2 schema dump:
//   CEconItemView::m_iItemDefinitionIndex  uint16 (size 2), networked
//   CEconItemView::m_iEntityQuality        int32  (size 4), networked
//   CEconItemView::m_szCustomName          char[161],         networked
//   CEconItemView::m_bInitialized          bool   (size 1),   networked
//   CBaseEntity::m_hOwnerEntity            CEntityHandle (size 4), networked
//   CBasePlayerPawn::m_hController         CEntityHandle (size 4), networked
//   CCSPlayerController::m_iMusicKitID     int32  (size 4),   networked
//   CCSPlayerController_InventoryServices::m_unMusicID uint16 (size 2), networked
//   ...::m_rank                            MedalRank_t[6] (int32 each)
//   CBasePlayerWeapon::m_iClip1            int32, CBasePlayerWeapon::m_pReserveAmmo int32[2]
//                                          (a FIXED ARRAY of two int32, 8 bytes inline - despite the
//                                          m_p spelling it is not a pointer; see below)
//   CWeaponTaser::m_fFireTime float / m_nLastAttackTick int32
inline constexpr int kWidthHandle = 4;
inline constexpr int kWidthDefIndex = 2;
inline constexpr int kWidthQuality = 4;
inline constexpr int kWidthInitialized = 1;
inline constexpr int kWidthCustomName = 161; // m_szCustomName is a fixed char buffer
inline constexpr int kWidthMusicController = 4;
inline constexpr int kWidthMusicInventory = 2;
inline constexpr int kWidthRank = 4;
inline constexpr int kWidthClip = 4;
inline constexpr int kWidthAmmo = 4;
inline constexpr int kWidthFireTime = 4;
inline constexpr int kWidthAttackTick = 4;
inline constexpr int kRankPinIndex = 5;		  // pins live in m_rank[5] (GameEntities.cs SetRank(5, ...))
inline constexpr int kReserveAmmoElements = 2; // int32[2]

// --- CAttributeList::m_Attributes ---------------------------------------------------------------
//
// The field is a 104-byte CUtlVectorEmbeddedNetworkVar<CEconItemAttribute>, NOT a plain CUtlVector
// header (tests/fixtures/game_excerpt_source.md, "Attribute-vector check"). Read-only disassembly
// of the current binary - signature-resolved CAttributeList::SetOrAddAttributeValueByName, file
// offset 0x1ca66e0 / VA 0x1ca76e0 - reads the element count at object + 0 and the data pointer at
// object + 8; its insertion helper compares the capacity at object + 16. The saved schema places
// the whole m_Attributes object at CAttributeList + 8; this code always addresses the object, so
// the relative offsets below are 0 / 8 / 16. The old "count at +16, data at +0" CUtlVector guess
// was wrong: a clear through it would have overwritten the capacity field.
inline constexpr int kAttributeCountOffset = 0;		// int32: number of live attributes
inline constexpr int kAttributeDataOffset = 8;		// pointer to the element storage
inline constexpr int kAttributeCapacityOffset = 16; // int32: allocated capacity
inline constexpr int kAttributeCountWidth = 4;		// only these four bytes are ever cleared
inline constexpr int kAttributeMaxCapacity = 4096;	// sanity bound for the header check

// --- attribute names (the reference's exact strings) --------------------------------------------
inline constexpr char kAttrTexturePrefab[] = "set item texture prefab";
inline constexpr char kAttrTextureSeed[] = "set item texture seed";
inline constexpr char kAttrTextureWear[] = "set item texture wear";
inline constexpr char kAttrKillEater[] = "kill eater";
inline constexpr char kAttrKillEaterScoreType[] = "kill eater score type";

struct StickerNames {
	const char* id;
	const char* schema;
	const char* offsetX;
	const char* offsetY;
	const char* wear;
	const char* scale;
	const char* rotation;
};

// Six slots, pre-formatted at compile time: the GiveNamedItem hook must not build strings.
inline constexpr std::array<StickerNames, 6> kStickerNames{{
	{"sticker slot 0 id", "sticker slot 0 schema", "sticker slot 0 offset x", "sticker slot 0 offset y",
	 "sticker slot 0 wear", "sticker slot 0 scale", "sticker slot 0 rotation"},
	{"sticker slot 1 id", "sticker slot 1 schema", "sticker slot 1 offset x", "sticker slot 1 offset y",
	 "sticker slot 1 wear", "sticker slot 1 scale", "sticker slot 1 rotation"},
	{"sticker slot 2 id", "sticker slot 2 schema", "sticker slot 2 offset x", "sticker slot 2 offset y",
	 "sticker slot 2 wear", "sticker slot 2 scale", "sticker slot 2 rotation"},
	{"sticker slot 3 id", "sticker slot 3 schema", "sticker slot 3 offset x", "sticker slot 3 offset y",
	 "sticker slot 3 wear", "sticker slot 3 scale", "sticker slot 3 rotation"},
	{"sticker slot 4 id", "sticker slot 4 schema", "sticker slot 4 offset x", "sticker slot 4 offset y",
	 "sticker slot 4 wear", "sticker slot 4 scale", "sticker slot 4 rotation"},
	{"sticker slot 5 id", "sticker slot 5 schema", "sticker slot 5 offset x", "sticker slot 5 offset y",
	 "sticker slot 5 wear", "sticker slot 5 scale", "sticker slot 5 rotation"},
}};

struct KeychainNames {
	const char* id;
	const char* offsetX;
	const char* offsetY;
	const char* offsetZ;
	const char* seed;
};

// The original only ever wrote keychain slot 0 (HookInventoryUpdateService.cs:512-520).
inline constexpr KeychainNames kKeychainNames{"keychain slot 0 id", "keychain slot 0 offset x",
											  "keychain slot 0 offset y", "keychain slot 0 offset z",
											  "keychain slot 0 seed"};

// The reference treats "schema == 1337" as "the sticker has no patch/schema" and then skips the
// schema and both offsets (StickerData.Schema defaults to 1337, HookInventoryUpdateService.cs:500).
inline constexpr int kStickerSchemaNone = 1337;

// --- change-subclass values ---------------------------------------------------------------------
//
// ChangeSubclass takes the decimal definition index as a string; every knife's index is a
// compile-time literal so no string is formatted anywhere (least of all in the hook).
inline constexpr std::array<std::pair<uint16_t, const char*>, 38> kKnifeSubclassValues{{
	{42, "42"},	  {59, "59"},	{500, "500"}, {501, "501"}, {502, "502"}, {503, "503"}, {504, "504"},
	{505, "505"}, {506, "506"}, {507, "507"}, {508, "508"}, {509, "509"}, {510, "510"}, {511, "511"},
	{512, "512"}, {513, "513"}, {514, "514"}, {515, "515"}, {516, "516"}, {517, "517"}, {518, "518"},
	{519, "519"}, {520, "520"}, {521, "521"}, {522, "522"}, {523, "523"}, {524, "524"}, {525, "525"},
	{526, "526"}, {527, "527"}, {528, "528"}, {529, "529"}, {530, "530"}, {531, "531"}, {532, "532"},
	{533, "533"}, {534, "534"}, {535, "535"},
}};

// Decimal string of a knife definition index, nullptr when it is not a known knife.
inline const char* KnifeSubclassValue(uint16_t definition) {
	for (const auto& [index, text] : kKnifeSubclassValues) {
		if (index == definition) {
			return text;
		}
	}
	return nullptr;
}

// --- raw memory ---------------------------------------------------------------------------------
//
// Typed accessors: the template pins the width at the call site, which is exactly the property the
// reference's raw writes have to keep (memcpy, so unaligned offsets are fine).
template <typename T>
inline T ReadMem(const void* base, int offset) {
	T value{};
	std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
	return value;
}

template <typename T>
inline void WriteMem(void* base, int offset, T value) {
	std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
}

// Bounded, always NUL-terminated copy into a fixed char buffer (m_szCustomName). Byte-level
// truncation, like the reference's Schema.SetFixedString.
inline void WriteFixedString(void* base, int offset, std::string_view text, int bufferSize) {
	if (base == nullptr || bufferSize <= 0) {
		return;
	}
	auto* destination = static_cast<std::uint8_t*>(base) + offset;
	const std::size_t maxBytes = static_cast<std::size_t>(bufferSize - 1);
	const std::size_t count = text.size() < maxBytes ? text.size() : maxBytes;
	if (count > 0) {
		std::memcpy(destination, text.data(), count);
	}
	destination[count] = 0;
}

// The StatTrak counter is an int32 whose *bit pattern* is stored as the attribute's float value
// (BitConverter.Int32BitsToSingle in the reference, HookInventoryUpdateService.cs:483). Sticker
// ids/schemas and the keychain id/seed are encoded the same way (:499, :502, :515, :519).
inline float IntBitsAsAttribute(int value) { return std::bit_cast<float>(static_cast<std::int32_t>(value)); }

inline float StatTrakValue(int count) { return IntBitsAsAttribute(count); }

// Attribute values for the paint trio are floats in the reference as well: the ints convert
// implicitly there, the port says it out loud.
inline float IntAttributeValue(int value) { return static_cast<float>(value); }

// --- page guard ---------------------------------------------------------------------------------
//
// Used for every schema-derived pointer this code dereferences (m_pWeaponServices,
// m_pInventoryServices) and by IsAttributeHeaderPlausible below. mincore answers "is this page
// mapped", so a wrong layout guess ends in a skipped write, not a segfault (the same helper
// plugins/fortnite_emotes/src/emote.cpp:87 uses).
inline bool IsPageMapped(const void* address) {
	if (address == nullptr) {
		return false;
	}
	static const long pageSize = ::sysconf(_SC_PAGESIZE);
	const std::size_t size = pageSize > 0 ? static_cast<std::size_t>(pageSize) : static_cast<std::size_t>(4096);
	const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(address) & ~(static_cast<std::uintptr_t>(size) - 1);
	unsigned char vector = 0;
	return ::mincore(reinterpret_cast<void*>(page), size, &vector) == 0;
}

// --- the attribute vector clear -----------------------------------------------------------------
//
// The reference's RemoveAll() on the attribute vector is "count = 0" for trivial elements. The
// header is validated before the write: a plausible header has 0 <= count <= capacity <=
// kAttributeMaxCapacity, and a non-empty vector has a mapped data pointer. Anything else means the
// layout is not what this code thinks it is - then nothing is written (a stale sticker list is
// harmless, a corrupted header is not). Exactly four bytes (the count) are ever written; the data
// pointer at +8 and the capacity at +16 are left alone.
inline bool IsAttributeHeaderPlausible(const void* listObject, int vectorOffset) {
	const auto* vector = static_cast<const std::uint8_t*>(listObject) + vectorOffset;
	const std::int32_t capacity = ReadMem<std::int32_t>(vector, kAttributeCapacityOffset);
	const std::int32_t count = ReadMem<std::int32_t>(vector, kAttributeCountOffset);
	if (count < 0 || capacity < 0 || count > capacity || capacity > kAttributeMaxCapacity) {
		return false;
	}
	if (count == 0) {
		return true; // nothing to clear
	}
	const void* data = ReadMem<const void*>(vector, kAttributeDataOffset);
	return IsPageMapped(data);
}

// --- inline reserve ammo ------------------------------------------------------------------------
//
// m_pReserveAmmo is a fixed int32[2] inside CBasePlayerWeapon (dump: kind fixed_array, element_size
// 4, element_count 2, size 8; fixture note "not a pointer despite its m_p spelling"). Both values
// are read and written in place - no pointer is loaded and nothing is dereferenced, so the old
// *(int32_t**)(weapon + off) pattern of plugins/vip_infiniteammo must NOT be copied here.
inline std::int32_t ReadReserveAmmoValue(const void* weapon, int fieldOffset, int index) {
	return ReadMem<std::int32_t>(weapon, fieldOffset + index * kWidthAmmo);
}

inline void WriteReserveAmmoValue(void* weapon, int fieldOffset, int index, std::int32_t value) {
	WriteMem<std::int32_t>(weapon, fieldOffset + index * kWidthAmmo, value);
}

// True when the host byte at `offset + width` is unchanged, i.e. a typed write kept its width.
// Used by the tests (and cheap enough to be harmless if a caller ever wants it).
inline bool WidthKept(const void* base, int offset, int width, std::uint8_t sentinel) {
	return ReadMem<std::uint8_t>(base, offset + width) == sentinel;
}

// --- offset bookkeeping --------------------------------------------------------------------------
//
// One table, resolved once per map by Engine::Prepare. Nothing here is read inside the hook before
// Prepare succeeded: every field defaults to -1 and every user checks it.
struct Offsets {
	bool ready = false;

	int weaponOwner = -1;				   // CBasePlayerWeapon::m_hOwnerEntity (CBaseEntity)
	int pawnController = -1;			   // CBasePlayerPawn::m_hController
	int weaponAttributeManager = -1;	   // CBasePlayerWeapon::m_AttributeManager (CEconEntity)
	int containerItem = -1;				   // CAttributeContainer::m_Item (inline CEconItemView)
	int itemDefIndex = -1;				   // CEconItemView::m_iItemDefinitionIndex (uint16)
	int itemQuality = -1;				   // CEconItemView::m_iEntityQuality (int32)
	int itemCustomName = -1;			   // CEconItemView::m_szCustomName (char[161])
	int itemInitialized = -1;			   // CEconItemView::m_bInitialized (bool)
	int itemAttributeList = -1;			   // CEconItemView::m_AttributeList
	int itemNetworkedAttributes = -1;	   // CEconItemView::m_NetworkedDynamicAttributes
	int attributeVector = -1;			   // CAttributeList::m_Attributes
	int pawnEconGloves = -1;			   // CCSPlayerPawn::m_EconGloves (inline CEconItemView)
	int pawnWeaponServices = -1;		   // CBasePlayerPawn::m_pWeaponServices (pointer)
	int weaponServicesActive = -1;		   // CPlayer_WeaponServices::m_hActiveWeapon
	int weaponClip = -1;				   // CBasePlayerWeapon::m_iClip1
	int weaponReserveAmmo = -1;			   // CBasePlayerWeapon::m_pReserveAmmo
	int taserFireTime = -1;				   // CWeaponTaser::m_fFireTime (optional)
	int taserLastAttackTick = -1;		   // CWeaponTaser::m_nLastAttackTick (optional)
	int controllerMusicKit = -1;		   // CCSPlayerController::m_iMusicKitID
	int controllerInventoryServices = -1;  // CCSPlayerController::m_pInventoryServices
	int inventoryMusicId = -1;			   // CCSPlayerController_InventoryServices::m_unMusicID
	int inventoryRank = -1;				   // CCSPlayerController_InventoryServices::m_rank

	// Network-var chain offsets. >= 0: CNetworkVarChainer offset inside that class; -1: notify the
	// entity directly (s2sdk's SafeNetworkStateChanged); -2: inline component, notify the object's
	// own NetworkStateChanged (vtable slot 1) - the convention for non-entity classes.
	int weaponChain = -1;
	int pawnChain = -1;
	int controllerChain = -1;
	int inventoryChain = -2;
};

} // namespace ws::detail
