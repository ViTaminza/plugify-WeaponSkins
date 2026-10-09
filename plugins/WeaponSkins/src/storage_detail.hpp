#pragma once
// Payload parsing and identifier helpers shared by storage.cpp and the storage tests.
//
// The Pisex schema (Skinchanger by Pisex, the sc_* tables used by the website's
// module_page_skins) stores multi-field values as ';'-separated strings:
//
//   sc_skins.skin            "paint;seed;wear"
//   sc_skins.stickers        "id0;id1;id2;id3"                       (0 = empty slot)
//   sc_skins.keychain        "id;x;y;z"                              (NULL/empty = none)
//   sc_skins_ext.sticker_4   "id;schema;x;y;wear;scale;rotation"     (same for sticker_5)
//   sc_skins_ext.stickers_full
//                            JSON array of six "id;schema;..." strings (slots 0..5)
//
// The rows are written by the website and by the original C# plugin, so the parsers are
// deliberately tolerant: a malformed field yields the neutral value instead of an error.
// Values follow the C# parser defaults exactly (missing float part -> 0, scale -> 0, ...).

#include "models.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ws::storage_detail {

// A MySQL identifier: the TablePrefix from config.toml is concatenated into the SQL text,
// so it must not be able to change a statement. Same shape the other plugins enforce.
inline bool ValidIdentifier(std::string_view text) {
	if (text.empty() || text.size() > 32) {
		return false;
	}
	if (text.front() >= '0' && text.front() <= '9') {
		return false;
	}
	for (const char c : text) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) {
			return false;
		}
	}
	return true;
}

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string_view Trim(std::string_view text) {
	while (!text.empty() && IsSpace(text.front())) {
		text.remove_prefix(1);
	}
	while (!text.empty() && IsSpace(text.back())) {
		text.remove_suffix(1);
	}
	return text;
}

inline std::vector<std::string_view> Split(std::string_view text, char separator) {
	std::vector<std::string_view> parts;
	size_t start = 0;
	for (size_t i = 0; i <= text.size(); ++i) {
		if (i == text.size() || text[i] == separator) {
			parts.push_back(text.substr(start, i - start));
			start = i + 1;
		}
	}
	return parts;
}

// Whole-field integer; trailing junk makes the field unparsable (nullopt).
inline std::optional<int> ParseFieldInt(std::string_view field) {
	field = Trim(field);
	if (field.empty()) {
		return std::nullopt;
	}
	int value = 0;
	const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
	if (result.ec != std::errc{} || result.ptr != field.data() + field.size()) {
		return std::nullopt;
	}
	return value;
}

inline std::optional<float> ParseFieldFloat(std::string_view field) {
	field = Trim(field);
	if (field.empty()) {
		return std::nullopt;
	}
	float value = 0;
	const auto result = std::from_chars(field.data(), field.data() + field.size(), value);
	if (result.ec != std::errc{} || result.ptr != field.data() + field.size()) {
		return std::nullopt;
	}
	return value;
}

// sc_skins.skin -> paint/seed/wear. Missing or malformed parts read as 0, like ParseSkin in
// PisexStorageService.cs.
inline void ParseSkinPayload(std::string_view text, int& paint, int& seed, float& wear) {
	const std::vector<std::string_view> parts = Split(text, ';');
	paint = parts.size() > 0 ? ParseFieldInt(parts[0]).value_or(0) : 0;
	seed = parts.size() > 1 ? ParseFieldInt(parts[1]).value_or(0) : 0;
	wear = parts.size() > 2 ? ParseFieldFloat(parts[2]).value_or(0.0F) : 0.0F;
}

// sc_skins.stickers -> the first four sticker slots. A sticker that only has an id carries no
// offsets, and the original writes schema 0 for it (schema 1337 is the menu's "hidden" marker,
// written when the offsets must not be touched). Slots without an id stay empty (id 0).
inline void ParseStickerIds(std::string_view text, std::array<Sticker, 6>& stickers) {
	const std::vector<std::string_view> parts = Split(text, ';');
	for (size_t slot = 0; slot < 4 && slot < parts.size(); ++slot) {
		const int id = ParseFieldInt(parts[slot]).value_or(0);
		if (id > 0) {
			Sticker sticker;
			sticker.id = id;
			sticker.schema = 0;
			stickers[slot] = sticker;
		}
	}
}

// "id;schema;x;y;wear;scale;rotation" -> a full sticker or nothing. Fewer than seven parts or
// an id of 0 is not a sticker (ParseStickerFull in PisexStorageService.cs).
inline std::optional<Sticker> ParseStickerFull(std::string_view text) {
	const std::vector<std::string_view> parts = Split(text, ';');
	if (parts.size() < 7) {
		return std::nullopt;
	}
	const int id = ParseFieldInt(parts[0]).value_or(0);
	if (id <= 0) {
		return std::nullopt;
	}
	Sticker sticker;
	sticker.id = id;
	sticker.schema = ParseFieldInt(parts[1]).value_or(0);
	sticker.x = ParseFieldFloat(parts[2]).value_or(0.0F);
	sticker.y = ParseFieldFloat(parts[3]).value_or(0.0F);
	sticker.wear = ParseFieldFloat(parts[4]).value_or(0.0F);
	sticker.scale = ParseFieldFloat(parts[5]).value_or(0.0F);
	sticker.rotation = ParseFieldFloat(parts[6]).value_or(0.0F);
	return sticker;
}

// sc_skins.keychain ("id;x;y;z"); empty/NULL and id 0 mean "no keychain".
inline std::optional<Keychain> ParseKeychainPayload(std::string_view text) {
	const std::string_view trimmed = Trim(text);
	if (trimmed.empty()) {
		return std::nullopt;
	}
	const std::vector<std::string_view> parts = Split(trimmed, ';');
	const int id = ParseFieldInt(parts[0]).value_or(0);
	if (id <= 0) {
		return std::nullopt;
	}
	Keychain keychain;
	keychain.id = id;
	keychain.x = parts.size() > 1 ? ParseFieldFloat(parts[1]).value_or(0.0F) : 0.0F;
	keychain.y = parts.size() > 2 ? ParseFieldFloat(parts[2]).value_or(0.0F) : 0.0F;
	keychain.z = parts.size() > 3 ? ParseFieldFloat(parts[3]).value_or(0.0F) : 0.0F;
	return keychain;
}

// A weapon skin row is anything that is neither a knife nor gloves and sits below the
// definition range the original filtered on (Utilities.IsWeaponDefinitionIndex).
inline bool IsWeaponDefinition(int definition) noexcept {
	return definition >= 1 && definition < 100 && !IsKnife(definition) && !IsGlove(definition);
}

// Key of one sc_skins row inside a player snapshot: team index (0/1) plus definition.
inline uint32_t RowKey(int teamIndex, int definition) noexcept {
	return (static_cast<uint32_t>(teamIndex) << 16) | (static_cast<uint32_t>(definition) & 0xFFFFU);
}

} // namespace ws::storage_detail
