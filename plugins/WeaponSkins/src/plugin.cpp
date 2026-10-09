// WeaponSkins — the Plugify entry point.
//
// The plugin is a skinchanger for CS2: players configure skins, knives, gloves, agents, music
// kits, stickers, keychains and pins on the website, which writes the Pisex `sc_*` tables; this
// plugin reads them (lazily, per player) and applies them in game — weapon skins through a
// GiveNamedItem detour, everything else on spawn and through `!wp`.
//
// This file only holds the Pluggify lifecycle wiring; the behaviour lives in runtime.cpp
// (lifecycle, reconcile, loads, commands, events) and in the engine-independent modules next to
// it (config.cpp, econ.cpp, storage.cpp, engine.cpp).

#include <plugin_export.h> // PLUGIN_API (visibility), used by PLUGIFY_PLUGIN() below

#include <plg/plugin.hpp>

#include "runtime.hpp"

namespace weaponskins {

// The single plugin object; PLUGIFY_PLUGIN() hands its address to the host.
class WeaponSkinsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		// Never Failed: one Failed from OnPluginStart stops every Plugify plugin on the server
		// (handoff/weaponskins/PITFALLS.md §A.1). The runtime fails soft — a missing config,
		// database or gamedata only disables the corresponding feature.
		ws::RuntimeInstance().Start(GetLocation());
		return {};
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override {
		ws::RuntimeInstance().Update(dt);
		return {};
	}

	plg::PluginResult OnPluginEnd() override {
		// `plg reload` must not leave any callback pointing into this library (PITFALLS §A.7):
		// the runtime removes the detour, the s2sdk listeners, the commands, the event hooks and
		// the timers before it goes away.
		ws::RuntimeInstance().Stop();
		return {};
	}
};

WeaponSkinsPlugin g_weaponSkinsPlugin;

} // namespace weaponskins

PLUGIFY_PLUGIN(PLUGIN_API, &weaponskins::g_weaponSkinsPlugin)
