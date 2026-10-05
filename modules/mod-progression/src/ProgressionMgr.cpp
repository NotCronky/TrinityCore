/*
 * mod-progression: each character plays in a patch of its own, from 1.1 to 3.3.5.
 */

#include "ProgressionMgr.h"

#include "Battleground.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>

namespace
{
    // Random battlegrounds and the Dungeon Finder's random dungeons arrived in 3.3.
    constexpr char const* RANDOM_QUEUES_VERSION = "3.3";
}

std::string ProgressionPatch::Title() const
{
    return Name.empty() ? Version : Version + " " + Name;
}

ProgressionMgr& ProgressionMgr::Instance()
{
    static ProgressionMgr instance;
    return instance;
}

void ProgressionMgr::LoadConfig()
{
    _enabled = sConfigMgr->GetBoolDefault("Progression.Enable", true);
    _serverCapVersion = sConfigMgr->GetStringDefault("Progression.ServerCap", "3.3.5");
    _startVersion = sConfigMgr->GetStringDefault("Progression.StartPatch", "1.1");
    _existingCharacterVersion = sConfigMgr->GetStringDefault("Progression.ExistingCharacterPatch", "3.3.5");
    _requireLevelCapToAdvance = sConfigMgr->GetBoolDefault("Progression.RequireLevelCapToAdvance", false);
    _dungeonFinderEveryPatch = sConfigMgr->GetBoolDefault("Progression.DungeonFinderEveryPatch", true);

    if (!_patches.empty())
        ResolveConfig();
}

bool ProgressionMgr::LoadPatches()
{
    std::vector<ProgressionPatch> patches;

    QueryResult result = WorldDatabase.Query("SELECT `id`, `version`, `name`, `expansion`, `level_cap`, `arena_season`, "
        "`quest_id`, `unlocks`, `removes` FROM `progression_patch` ORDER BY `id`");
    if (!result)
    {
        TC_LOG_ERROR("module", "mod-progression: the progression_patch table is missing or empty; the module is disabled.");
        _patches.clear();
        return false;
    }

    do
    {
        Field* fields = result->Fetch();
        ProgressionPatch patch;
        patch.Id = fields[0].GetUInt8();
        patch.Version = fields[1].GetString();
        patch.Name = fields[2].GetString();
        patch.Expansion = fields[3].GetUInt8();
        patch.LevelCap = fields[4].GetUInt8();
        patch.ArenaSeason = fields[5].GetUInt8();
        patch.QuestId = fields[6].GetUInt32();
        patch.Unlocks = fields[7].GetString();
        patch.Removes = fields[8].GetString();

        // RemoveRewardedQuest asserts on quests that don't exist, so check them all up front.
        if (!sObjectMgr->GetQuestTemplate(patch.QuestId))
        {
            TC_LOG_ERROR("module", "mod-progression: patch {} uses quest {}, which doesn't exist in quest_template; "
                "the module is disabled.", patch.Version, patch.QuestId);
            _patches.clear();
            return false;
        }

        patches.push_back(std::move(patch));
    } while (result->NextRow());

    _patches = std::move(patches);
    ResolveConfig();
    LoadMaps();

    TC_LOG_INFO("module", "mod-progression: loaded {} patches, server cap {}.", _patches.size(), GetServerCap()->Version);
    return true;
}

void ProgressionMgr::LoadMaps()
{
    _mapPatches.clear();

    QueryResult result = WorldDatabase.Query("SELECT `map`, `patch` FROM `progression_map`");
    if (!result)
    {
        TC_LOG_ERROR("module", "mod-progression: the progression_map table is missing or empty; maps aren't limited by patch.");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        uint32 mapId = fields[0].GetUInt32();
        uint8 patchId = fields[1].GetUInt8();
        if (!sMapStore.LookupEntry(mapId) || !GetPatch(patchId))
        {
            TC_LOG_ERROR("module", "mod-progression: progression_map has map {} with patch {}; one of them doesn't exist, skipped.",
                mapId, patchId);
            continue;
        }

        _mapPatches[mapId] = patchId;
    } while (result->NextRow());
}

ProgressionPatch const* ProgressionMgr::GetMapPatch(uint32 mapId) const
{
    auto itr = _mapPatches.find(mapId);
    return itr != _mapPatches.end() ? GetPatch(itr->second) : nullptr;
}

ProgressionPatch const* ProgressionMgr::CheckPatch(Player const* player, ProgressionPatch const* required) const
{
    if (!required || player->IsGameMaster())
        return nullptr;

    ProgressionPatch const* patch = GetEffectivePatch(player);
    return patch && required->Id > patch->Id ? required : nullptr;
}

ProgressionPatch const* ProgressionMgr::GetRequiredPatch(Player const* player, uint32 mapId) const
{
    return CheckPatch(player, GetMapPatch(mapId));
}

ProgressionPatch const* ProgressionMgr::GetRequiredPatch(Player const* player, Battleground const* bg) const
{
    ProgressionPatch const* required = nullptr;
    if (bg->IsRandom())
        required = FindPatch(RANDOM_QUEUES_VERSION);
    else if (bg->isArena())
    {
        // The first patch with an arena season.
        for (ProgressionPatch const& patch : _patches)
        {
            if (patch.ArenaSeason)
            {
                required = &patch;
                break;
            }
        }
    }
    else
        required = GetMapPatch(bg->GetMapId());

    return CheckPatch(player, required);
}

ProgressionPatch const* ProgressionMgr::GetRequiredDungeonFinderPatch(Player const* player, uint32 dungeonId,
    uint32 mapId) const
{
    ProgressionPatch const* required = GetMapPatch(mapId);

    // Random dungeons arrived with the Dungeon Finder in 3.3; with DungeonFinderEveryPatch off, so does
    // queueing for a particular dungeon, like live.
    LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(dungeonId);
    if (!_dungeonFinderEveryPatch || (dungeon && dungeon->TypeID == lfg::LFG_TYPE_RANDOM))
        if (ProgressionPatch const* finder = FindPatch(RANDOM_QUEUES_VERSION); finder && (!required || finder->Id > required->Id))
            required = finder;

    return CheckPatch(player, required);
}

void ProgressionMgr::SendRequiresPatch(Player const* player, std::string const& what, ProgressionPatch const& required) const
{
    ProgressionPatch const* patch = GetEffectivePatch(player);
    ChatHandler(player->GetSession()).SendSysMessage(Trinity::StringFormat(
        "{} needs patch {}; you are in patch {}. Type .patch to see your progress.", what, required.Title(),
        patch ? patch->Version : "?"));
}

void ProgressionMgr::EnsureAllowedLocation(Player* player) const
{
    MapEntry const* map = sMapStore.LookupEntry(player->GetMapId());
    if (!map || map->IsBattlegroundOrArena() || !GetRequiredPatch(player, player->GetMapId()))
        return;

    if (!GetRequiredPatch(player, player->m_homebindMapId))
    {
        player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY, player->m_homebindZ,
            player->GetOrientation());
        return;
    }

    if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(player->GetRace(), player->GetClass()))
        player->TeleportTo(info->mapId, info->positionX, info->positionY, info->positionZ, info->orientation);
}

void ProgressionMgr::ResolveConfig()
{
    auto resolve = [this](std::string const& version, char const* option, ProgressionPatch const& fallback)
    {
        if (ProgressionPatch const* patch = FindPatch(version))
            return patch->Id;

        TC_LOG_ERROR("module", "mod-progression: {} = \"{}\" is not a patch; using {}.", option, version, fallback.Version);
        return fallback.Id;
    };

    _serverCapId = resolve(_serverCapVersion, "Progression.ServerCap", _patches.back());
    _startId = resolve(_startVersion, "Progression.StartPatch", _patches.front());
    _existingCharacterId = resolve(_existingCharacterVersion, "Progression.ExistingCharacterPatch", _patches.back());
}

ProgressionPatch const* ProgressionMgr::GetPatch(uint8 id) const
{
    for (ProgressionPatch const& patch : _patches)
        if (patch.Id == id)
            return &patch;

    return nullptr;
}

ProgressionPatch const* ProgressionMgr::FindPatch(std::string_view version) const
{
    for (ProgressionPatch const& patch : _patches)
        if (patch.Version == version)
            return &patch;

    return nullptr;
}

ProgressionPatch const* ProgressionMgr::GetNextPatch(ProgressionPatch const& patch) const
{
    for (ProgressionPatch const& other : _patches)
        if (other.Id > patch.Id)
            return &other;

    return nullptr;
}

ProgressionPatch const* ProgressionMgr::GetCharacterPatch(Player const* player) const
{
    auto const& rewarded = player->getRewardedQuests();
    for (auto itr = _patches.rbegin(); itr != _patches.rend(); ++itr)
        if (rewarded.count(itr->QuestId))
            return &*itr;

    return nullptr;
}

ProgressionPatch const* ProgressionMgr::GetEffectivePatch(Player const* player) const
{
    ProgressionPatch const* patch = GetCharacterPatch(player);
    if (!patch)
        return nullptr;

    return patch->Id > _serverCapId ? GetServerCap() : patch;
}

void ProgressionMgr::SetCharacterPatch(Player* player, ProgressionPatch const& patch) const
{
    for (ProgressionPatch const& other : _patches)
        if (other.Id != patch.Id && player->getRewardedQuests().count(other.QuestId))
            player->RemoveRewardedQuest(other.QuestId, false);

    if (!player->getRewardedQuests().count(patch.QuestId))
        player->SetRewardedQuest(patch.QuestId);
}

ProgressionPatch const* ProgressionMgr::GetStartPatch(uint8 race, uint8 playerClass) const
{
    // Races and classes can't start before the expansion that added them.
    uint8 expansion = 0;
    if (race == RACE_BLOODELF || race == RACE_DRAENEI)
        expansion = 1;
    if (playerClass == CLASS_DEATH_KNIGHT)
        expansion = 2;

    ProgressionPatch const* start = GetPatch(_startId);
    if (start && start->Expansion >= expansion)
        return start;

    for (ProgressionPatch const& patch : _patches)
        if (patch.Expansion >= expansion)
            return &patch;

    return start;
}

void ProgressionMgr::OnLogin(Player* player, bool firstLogin) const
{
    if (GetCharacterPatch(player))
    {
        EnsureAllowedLocation(player);
        return;
    }

    ProgressionPatch const* patch = firstLogin ? GetStartPatch(player->GetRace(), player->GetClass()) :
        GetPatch(_existingCharacterId);
    if (!patch)
        return;

    SetCharacterPatch(player, *patch);
    TC_LOG_INFO("module", "mod-progression: {} starts in patch {}.", player->GetName(), patch->Version);
    EnsureAllowedLocation(player);
}
