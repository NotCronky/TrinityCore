/*
 * mod-progression: each character plays in a patch of its own, from 1.1 to 3.3.5.
 */

#ifndef MOD_PROGRESSION_MGR_H
#define MOD_PROGRESSION_MGR_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Battleground;
class Player;
struct MapEntry;

struct ProgressionPatch
{
    uint8 Id = 0;            // Order of progression, from 1 (1.1)
    std::string Version;     // "1.6", as typed in commands
    std::string Name;        // "Assault on Blackwing Lair"
    uint8 Expansion = 0;     // 0 Vanilla, 1 TBC, 2 WotLK
    uint8 LevelCap = 0;
    uint8 ArenaSeason = 0;
    uint32 QuestId = 0;      // Hidden quest marking a character in this patch
    std::string Unlocks;     // Shown before advancing
    std::string Removes;     // Content that ends on entering this patch

    // "1.6 Assault on Blackwing Lair", or just the version when it has no name
    std::string Title() const;
};

// Patches are loaded on the world thread (at startup or by .patch reload) while maps are idle, so
// lookups from map threads need no lock.
//
// A character's patch is the one hidden patch quest it has completed. It loads and saves with the
// character like any quest, and only the current patch's quest is kept.
class ProgressionMgr
{
public:
    static ProgressionMgr& Instance();

    void LoadConfig();
    // Returns false when the table is missing or broken; the module then stays disabled.
    bool LoadPatches();

    bool IsEnabled() const { return _enabled && !_patches.empty(); }

    std::vector<ProgressionPatch> const& GetPatches() const { return _patches; }
    ProgressionPatch const* GetPatch(uint8 id) const;
    ProgressionPatch const* FindPatch(std::string_view version) const;
    ProgressionPatch const* GetNextPatch(ProgressionPatch const& patch) const;

    // The patch the character is in, or nullptr when it has none yet (before its first login with the module).
    ProgressionPatch const* GetCharacterPatch(Player const* player) const;
    // The patch the character plays in: its own, limited by the server cap.
    ProgressionPatch const* GetEffectivePatch(Player const* player) const;
    // Moves the character to the patch: replaces its patch quest. Saved with the character.
    void SetCharacterPatch(Player* player, ProgressionPatch const& patch) const;

    // Where a new character of this race and class starts.
    ProgressionPatch const* GetStartPatch(uint8 race, uint8 playerClass) const;

    ProgressionPatch const* GetServerCap() const { return GetPatch(_serverCapId); }
    // Until the config is reloaded or the server restarts.
    void SetServerCap(ProgressionPatch const& patch) { _serverCapId = patch.Id; }

    bool RequireLevelCapToAdvance() const { return _requireLevelCapToAdvance; }

    // The patch a map arrived in, or nullptr when it is open from the first patch.
    ProgressionPatch const* GetMapPatch(uint32 mapId) const;
    // The patch that is required to enter the map, queue for the battleground, or use the Dungeon
    // Finder's dungeon; nullptr when every character may.
    ProgressionPatch const* GetRequiredPatch(Player const* player, uint32 mapId) const;
    ProgressionPatch const* GetRequiredPatch(Player const* player, Battleground const* bg) const;
    ProgressionPatch const* GetRequiredDungeonFinderPatch(Player const* player, uint32 dungeonId, uint32 mapId) const;

    // Tells the player which patch something needs.
    void SendRequiresPatch(Player const* player, std::string const& what, ProgressionPatch const& required) const;

    // Moves a character that is somewhere its patch doesn't allow (after logging in, or after a game
    // master moved it to an earlier patch) to its hearthstone location, or its race's starting point.
    void EnsureAllowedLocation(Player* player) const;

    // Gives a character without a patch its starting one.
    void OnLogin(Player* player, bool firstLogin) const;

private:
    ProgressionMgr() = default;

    // Turns the config's patch versions into patch ids; needs the patches loaded.
    void ResolveConfig();
    void LoadMaps();
    // A later required patch than the character's effective one, or nullptr.
    ProgressionPatch const* CheckPatch(Player const* player, ProgressionPatch const* required) const;

    bool _enabled = true;
    std::string _serverCapVersion;
    std::string _startVersion;
    std::string _existingCharacterVersion;
    bool _requireLevelCapToAdvance = false;
    bool _dungeonFinderEveryPatch = true;

    uint8 _serverCapId = 0;
    uint8 _startId = 0;
    uint8 _existingCharacterId = 0;

    std::vector<ProgressionPatch> _patches; // Ordered by id
    std::unordered_map<uint32, uint8> _mapPatches; // Map id -> patch id
};

#define sProgressionMgr ProgressionMgr::Instance()

#endif
