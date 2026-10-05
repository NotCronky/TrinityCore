/*
 * mod-progression: each character plays in a patch of its own, from 1.1 to 3.3.5.
 */

#include "ProgressionMgr.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"

#include <algorithm>

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

    TC_LOG_INFO("module", "mod-progression: loaded {} patches, server cap {}.", _patches.size(), GetServerCap()->Version);
    return true;
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
        return;

    ProgressionPatch const* patch = firstLogin ? GetStartPatch(player->GetRace(), player->GetClass()) :
        GetPatch(_existingCharacterId);
    if (!patch)
        return;

    SetCharacterPatch(player, *patch);
    TC_LOG_INFO("module", "mod-progression: {} starts in patch {}.", player->GetName(), patch->Version);
}
