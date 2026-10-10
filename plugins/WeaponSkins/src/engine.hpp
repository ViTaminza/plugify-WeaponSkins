#pragma once
#include "econ.hpp"
#include <filesystem>
#include <functional>
#include <memory>
namespace ws {
using CacheLookup = std::function<const Snapshot*(int slot)>;
class Engine {
public:
    Engine(const Config& config, const EconData& econ);
    ~Engine();
    bool Prepare(std::string& error); // only with a ready map; cache offsets outside the hot hook
    bool InstallHook(const std::filesystem::path& gamedata, CacheLookup lookup, std::string& error);
    void Shutdown();
    void ApplySpawn(int slot, const Snapshot& snapshot, bool refresh = false);
    void RefreshInventory(int slot, const Snapshot& snapshot);
    // Reapply owned items after a world pickup without removing or giving items.
    void ApplyOwnedWeapons(int slot, const Snapshot& snapshot);
    int ActiveDefinition(int slot) const;
    void SetStatTrak(int slot, const Skin& skin);
    // Immediate operation; caller must validate the owner in this same callback.
    bool ApplyWeapon(int handle, const Skin& skin, bool knife = false);
    // Diagnostic counters, dumped by the runtime's ws_status. Plain integers only: the
    // GiveNamedItem hook bumps some of them, so nothing here may allocate or log.
    //   hookCalls      - post callbacks on a created weapon that reached the apply code;
    //   hookBadWeapon  - the hook's return pointer was not a usable entity;
    //   hookNoServices - argument 0 (the CCSPlayer_ItemServices* the item is given through) was
    //                    not readable, so the owner had to come from the weapon-owner chain;
    //   hookNoPawn     - neither the services argument nor the owner chain produced a pawn;
    //   hookNoController / hookNoSlot / hookNotReady - the controller step, the slot lookup and
    //                    the client-ready guard refused the resolved player;
    //   hookBot        - a bot took the same path and is skipped (expected on a bot-filled server);
    //   hookNoSnapshot - no loaded per-player cache (or a stale steam id);
    //   hookNoTeam     - the owner's team is not T/CT;
    //   hookKnife/hookGun - a skin was written from the hook;
    //   hookNoMatch    - the given definition has no row in that team's snapshot;
    //   gunInPlace     - a gun skin written to an owned weapon (pickup/refresh/!wp);
    //   gloveNoData / gloveNoHandle - no glove configured / no pawn handle to rebuild it with;
    //   agentNoHandle / agentNoData / agentNoCatalog / agentNoModel / agentNoBinding - the agent
    //                    path's stage counters (handle, data, catalog, model, binding);
    //   agentWritten / agentDeferDropped - next-frame model writes done / cancelled by the guard.
    struct ApplyStats {
        uint64_t hookCalls = 0;
        uint64_t hookBadWeapon = 0;
        uint64_t hookNoServices = 0;
        uint64_t hookNoPawn = 0;
        uint64_t hookNoController = 0;
        uint64_t hookNoSlot = 0;
        uint64_t hookNotReady = 0;
        uint64_t hookBot = 0;
        uint64_t hookNoSnapshot = 0;
        uint64_t hookNoTeam = 0;
        uint64_t hookKnife = 0;
        uint64_t hookGun = 0;
        uint64_t hookNoMatch = 0;
        uint64_t gunInPlace = 0;
        uint64_t knifeApplied = 0;
        uint64_t gloveApplied = 0;
        uint64_t gloveNoData = 0;
        uint64_t gloveNoHandle = 0;
        uint64_t agentApplied = 0;
        uint64_t agentWritten = 0;
        uint64_t agentDeferDropped = 0;
        uint64_t agentNoBinding = 0;
        uint64_t agentNoModel = 0;
        uint64_t agentNoHandle = 0;
        uint64_t agentNoData = 0;
        uint64_t agentNoCatalog = 0;
        uint64_t musicApplied = 0;
        uint64_t pinApplied = 0;
    };
    const ApplyStats& Stats() const;

    // The two pawn-handle readings ws_status prints per slot (diagnostics only; every engine path
    // goes through Impl::PawnHandle): the controller's m_hPlayerPawn handle and the pointer-derived
    // fallback - the reading the live build refused for every player.
    struct PawnHandles {
        int32_t viaController = -1;
        int32_t viaPointer = -1;
    };
    PawnHandles PawnHandleReport(int slot) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ws
