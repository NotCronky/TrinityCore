/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotQuesting.h"
#include "BotCompanion.h"

#include "Bag.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "Random.h"
#include "RotationBotMgr.h" // mod-rotation-bot: opening fights
#include "RotationProfiles.h" // mod-rotation-bot: the talent tree with the most points
#include "SharedDefines.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    constexpr uint32 THINK_MS = 1000;
    constexpr uint32 FIGHT_THINK_MS = 250;
    constexpr uint32 RELEASE_MS = 5 * IN_MILLISECONDS;            // Lying dead before releasing
    constexpr uint32 GHOST_GIVE_UP_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr float RECLAIM_DISTANCE = 20.0f;                      // The server allows up to 39 yards
    constexpr uint32 VENDOR_FREE_SLOTS = 4;                         // Fewer free bag slots: off to sell
    constexpr uint32 VENDOR_COOLDOWN_MS = 5 * MINUTE * IN_MILLISECONDS;
    constexpr uint32 REPLAN_MS = 10 * MINUTE * IN_MILLISECONDS; // Picking its zone again
    constexpr uint32 SKIP_RESET_MS = 10 * MINUTE * IN_MILLISECONDS; // Skipped givers and enders get another try
    constexpr uint32 THINK_JITTER_MS = 500;
    constexpr float SPREAD = 6.0f; // Yards around a place bots walk to, so they don't walk in a line
    constexpr uint32 TRAVEL_CHECK_MS = 30 * IN_MILLISECONDS;

    constexpr float SEARCH_RANGE = 60.0f;      // Live creatures it looks for around itself
    constexpr float NEARBY_GIVER_RANGE = 250.0f; // Quest givers worth walking to while it has quests
    constexpr float RANGED_DISTANCE = 25.0f;

    // The nearest creature a bot may fight: of the given entries (or, without entries, anything worth
    // grinding for its level), alive, not fighting someone else and not tapped by someone else.
    class FightableCheck
    {
    public:
        FightableCheck(Player* bot, std::unordered_set<uint32> const* entries, float range)
            : _bot(bot), _entries(entries), _range(range) { }

        bool operator()(Creature* creature)
        {
            if (!creature->IsAlive() || !_bot->IsWithinDistInMap(creature, _range))
                return false;

            if (_entries ? !_entries->count(creature->GetEntry()) :
                (creature->IsCritter() || creature->isElite() || creature->GetLevel() + 5 < _bot->GetLevel() ||
                    creature->GetLevel() > _bot->GetLevel() + 2 || creature->IsCivilian()))
                return false;

            if ((creature->hasLootRecipient() && !creature->isTappedBy(_bot)) ||
                (creature->IsInCombat() && creature->GetVictim() != _bot) || creature->IsInEvadeMode() ||
                !_bot->IsValidAttackTarget(creature))
                return false;

            _range = _bot->GetDistance(creature); // Nearer ones from now on
            return true;
        }

    private:
        Player* _bot;
        std::unordered_set<uint32> const* _entries;
        float _range;
    };

    // The nearest dead creature the bot tapped that still has loot.
    class LootableCheck
    {
    public:
        LootableCheck(Player* bot, std::unordered_set<ObjectGuid> const& looted, float range)
            : _bot(bot), _looted(looted), _range(range) { }

        bool operator()(Creature* creature)
        {
            if (creature->IsAlive() || !creature->isTappedBy(_bot) || creature->loot.isLooted() ||
                _looted.count(creature->GetGUID()) || !_bot->IsWithinDistInMap(creature, _range))
                return false;

            _range = _bot->GetDistance(creature);
            return true;
        }

    private:
        Player* _bot;
        std::unordered_set<ObjectGuid> const& _looted;
        float _range;
    };

    // The nearest object of the entries that is there to be used, and not one the bot just used.
    class UsableObjectCheck
    {
    public:
        UsableObjectCheck(Player* bot, std::unordered_set<uint32> const& entries, std::unordered_set<ObjectGuid> const& used, float range)
            : _bot(bot), _entries(entries), _used(used), _range(range) { }

        bool operator()(GameObject* go)
        {
            if (!go->isSpawned() || !_entries.count(go->GetEntry()) || _used.count(go->GetGUID()) ||
                !_bot->IsWithinDistInMap(go, _range))
                return false;

            _range = _bot->GetDistance(go);
            return true;
        }

    private:
        Player* _bot;
        std::unordered_set<uint32> const& _entries;
        std::unordered_set<ObjectGuid> const& _used;
        float _range;
    };

    // The nearest live creature of an entry, to talk to.
    Creature* FindCreature(Player* bot, uint32 entry, float range)
    {
        Creature* found = nullptr;
        Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck check(*bot, entry, true, range);
        Trinity::CreatureLastSearcher<Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitGridObjects(bot, searcher, range);
        return found;
    }

    BotQuestData::Spawn const* NearestSpawn(Player* bot, std::vector<uint32> const& entries, bool objects = false,
        std::unordered_set<BotQuestData::Spawn const*> const* skipped = nullptr)
    {
        BotQuestData::Spawn const* nearest = nullptr;
        float nearestDist = std::numeric_limits<float>::max();
        for (uint32 entry : entries)
        {
            if (std::vector<BotQuestData::Spawn> const* spawns = objects ? sBotQuestData.GetObjectSpawns(entry) : sBotQuestData.GetSpawns(entry))
            {
                for (BotQuestData::Spawn const& spawn : *spawns)
                {
                    if (spawn.MapId != bot->GetMapId() || (skipped && skipped->count(&spawn)))
                        continue;

                    float dist = bot->GetExactDist2d(spawn.Pos);
                    if (dist < nearestDist)
                    {
                        nearest = &spawn;
                        nearestDist = dist;
                    }
                }
            }
        }
        return nearest;
    }

    void QueuePacket(Player* bot, WorldPacket* packet)
    {
        bot->GetSession()->QueuePacket(packet);
    }

    // A quest the bot can take now and do.
    bool CanStart(Player* bot, Quest const* quest)
    {
        return sBotQuestData.IsSupported(quest) && bot->GetQuestStatus(quest->GetQuestId()) == QUEST_STATUS_NONE &&
            bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) && bot->SatisfyQuestLevel(quest, false);
    }

    std::vector<Quest const*> AvailableQuests(Player* bot, uint32 giverEntry)
    {
        std::vector<Quest const*> quests;
        if (std::vector<uint32> const* ids = sBotQuestData.GetQuestsOf(giverEntry))
            for (uint32 id : *ids)
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(id); quest && CanStart(bot, quest))
                    quests.push_back(quest);
        return quests;
    }
}

BotQuestData& BotQuestData::Instance()
{
    static BotQuestData instance;
    return instance;
}

void BotQuestData::Load()
{
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        _spawns[data.id].push_back({ data.id, data.mapId, data.spawnPoint });

    auto loadRelations = [](char const* table, std::unordered_map<uint32, std::vector<uint32>>& byQuest,
        std::unordered_map<uint32, std::vector<uint32>>* byCreature)
    {
        if (QueryResult result = WorldDatabase.PQuery("SELECT `id`, `quest` FROM `{}`", table))
        {
            do
            {
                Field* fields = result->Fetch();
                byQuest[fields[1].GetUInt32()].push_back(fields[0].GetUInt32());
                if (byCreature)
                    (*byCreature)[fields[0].GetUInt32()].push_back(fields[1].GetUInt32());
            } while (result->NextRow());
        }
    };
    loadRelations("creature_queststarter", _starters, &_questsOf);
    loadRelations("creature_questender", _enders, nullptr);

    // Quest items from creature loot (directly, not through reference tables).
    if (QueryResult result = WorldDatabase.Query("SELECT ct.`entry`, clt.`Item` FROM `creature_loot_template` clt "
        "JOIN `creature_template` ct ON ct.`lootid` = clt.`Entry` WHERE clt.`Reference` = 0 AND clt.`QuestRequired` = 1"))
    {
        do
        {
            Field* fields = result->Fetch();
            _droppers[fields[1].GetUInt32()].push_back(fields[0].GetUInt32());
        } while (result->NextRow());
    }

    for (auto const& [spawnId, data] : sObjectMgr->GetAllGameObjectData())
        _objectSpawns[data.id].push_back({ data.id, data.mapId, data.spawnPoint });

    // Quest items in chests and the like (game object type 3, whose loot id is Data1).
    if (QueryResult result = WorldDatabase.Query("SELECT gt.`entry`, glt.`Item` FROM `gameobject_loot_template` glt "
        "JOIN `gameobject_template` gt ON gt.`Data1` = glt.`Entry` WHERE gt.`type` = 3 AND glt.`Reference` = 0 AND glt.`QuestRequired` = 1"))
    {
        do
        {
            Field* fields = result->Fetch();
            _objectSources[fields[1].GetUInt32()].push_back(fields[0].GetUInt32());
        } while (result->NextRow());
    }

    for (auto const& [entry, creature] : sObjectMgr->GetCreatureTemplates())
        if (creature.npcflag & UNIT_NPC_FLAG_VENDOR)
            if (std::vector<Spawn> const* spawns = GetSpawns(entry))
                for (Spawn const& spawn : *spawns)
                    _vendors[spawn.MapId].push_back({ spawn, (creature.npcflag & UNIT_NPC_FLAG_REPAIR) != 0, creature.faction });

    for (auto const& [entry, quests] : _questsOf)
        if (std::vector<Spawn> const* spawns = GetSpawns(entry))
            for (Spawn const& spawn : *spawns)
                _givers[spawn.MapId].push_back(spawn);

    LoadZones();

    TC_LOG_INFO("module", "mod-bots: questing data: {} creature entries with spawns, {} quest givers, {} quest items from "
        "creatures and {} from objects, {} questing zones.", _spawns.size(), _questsOf.size(), _droppers.size(),
        _objectSources.size(), _zones.size());
}

void BotQuestData::LoadZones()
{
    // Per zone (a quest's QuestSortID when positive): the levels of the quests bots can do there, how many
    // each faction can do, and the spawns of their givers on the zone's main map.
    struct Collected
    {
        std::vector<int32> Levels;
        uint32 Quests[2] = { 0, 0 };
        std::unordered_map<uint32, std::vector<Spawn const*>> GiversByMap;
    };
    std::unordered_map<uint32, Collected> collected;

    for (auto const& [questId, starterEntries] : _starters)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest || quest->GetZoneOrSort() <= 0 || !IsSupported(quest))
            continue;

        Collected& zone = collected[uint32(quest->GetZoneOrSort())];
        zone.Levels.push_back(quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : int32(quest->GetMinLevel()));

        uint32 races = quest->GetAllowableRaces();
        if (!races || (races & RACEMASK_ALLIANCE))
            ++zone.Quests[0];
        if (!races || (races & RACEMASK_HORDE))
            ++zone.Quests[1];

        for (uint32 entry : starterEntries)
            if (std::vector<Spawn> const* spawns = GetSpawns(entry))
                for (Spawn const& spawn : *spawns)
                    zone.GiversByMap[spawn.MapId].push_back(&spawn);
    }

    for (auto& [zoneId, data] : collected)
    {
        if (data.Levels.size() < 5 || data.GiversByMap.empty())
            continue;

        // The map with most of the zone's givers (a few quests are given elsewhere).
        auto main = std::max_element(data.GiversByMap.begin(), data.GiversByMap.end(),
            [](auto const& a, auto const& b) { return a.second.size() < b.second.size(); });

        std::sort(data.Levels.begin(), data.Levels.end());
        int32 low = data.Levels[data.Levels.size() / 5];
        int32 high = data.Levels[data.Levels.size() * 4 / 5];

        Zone zone;
        zone.Id = zoneId;
        zone.MapId = main->first;
        zone.MinLevel = uint8(std::max(1, low - 2));
        zone.MaxLevel = uint8(std::max(int32(zone.MinLevel), high + 1));
        zone.Quests[0] = data.Quests[0];
        zone.Quests[1] = data.Quests[1];
        zone.Givers = std::move(main->second);

        float x = 0, y = 0, z = 0;
        for (Spawn const* giver : zone.Givers)
        {
            x += giver->Pos.GetPositionX();
            y += giver->Pos.GetPositionY();
            z += giver->Pos.GetPositionZ();
        }
        float count = float(zone.Givers.size());
        zone.Center.Relocate(x / count, y / count, z / count);

        _zones[zoneId] = std::move(zone);
    }
}

BotQuestData::Zone const* BotQuestData::PickZone(Player* bot, std::unordered_set<uint32> const& exhausted) const
{
    constexpr uint32 MIN_QUESTS = 5;
    constexpr float MAX_ZONE_DISTANCE = 1500.0f;
    uint8 faction = bot->GetTeam() == ALLIANCE ? 0 : 1;

    Zone const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (auto const& [zoneId, zone] : _zones)
    {
        if (zone.MapId != bot->GetMapId() || exhausted.count(zoneId) || zone.Quests[faction] < MIN_QUESTS ||
            bot->GetLevel() < zone.MinLevel || bot->GetLevel() > zone.MaxLevel)
            continue;

        // Within walking distance: further zones are usually across mountains or water it can't path over.
        float dist = bot->GetExactDist2d(zone.Center);
        if (dist < bestDist && dist < MAX_ZONE_DISTANCE)
        {
            best = &zone;
            bestDist = dist;
        }
    }
    return best;
}

BotQuestData::Zone const* BotQuestData::GetZone(uint32 zoneId) const
{
    auto itr = _zones.find(zoneId);
    return itr != _zones.end() ? &itr->second : nullptr;
}

std::string BotQuestData::GetZoneName(uint32 zoneId)
{
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
    return area ? area->AreaName[LOCALE_enUS] : std::to_string(zoneId);
}

std::vector<BotQuestData::Spawn> const* BotQuestData::GetSpawns(uint32 entry) const
{
    auto itr = _spawns.find(entry);
    return itr != _spawns.end() ? &itr->second : nullptr;
}

std::vector<uint32> const* BotQuestData::GetStarters(uint32 questId) const
{
    auto itr = _starters.find(questId);
    return itr != _starters.end() ? &itr->second : nullptr;
}

std::vector<uint32> const* BotQuestData::GetEnders(uint32 questId) const
{
    auto itr = _enders.find(questId);
    return itr != _enders.end() ? &itr->second : nullptr;
}

std::vector<uint32> const* BotQuestData::GetQuestsOf(uint32 creatureEntry) const
{
    auto itr = _questsOf.find(creatureEntry);
    return itr != _questsOf.end() ? &itr->second : nullptr;
}

std::vector<uint32> const* BotQuestData::GetDroppers(uint32 itemId) const
{
    auto itr = _droppers.find(itemId);
    return itr != _droppers.end() ? &itr->second : nullptr;
}

std::vector<BotQuestData::Spawn> const* BotQuestData::GetGivers(uint32 mapId) const
{
    auto itr = _givers.find(mapId);
    return itr != _givers.end() ? &itr->second : nullptr;
}

std::vector<BotQuestData::Spawn> const* BotQuestData::GetObjectSpawns(uint32 entry) const
{
    auto itr = _objectSpawns.find(entry);
    return itr != _objectSpawns.end() ? &itr->second : nullptr;
}

std::vector<uint32> const* BotQuestData::GetObjectSources(uint32 itemId) const
{
    auto itr = _objectSources.find(itemId);
    return itr != _objectSources.end() ? &itr->second : nullptr;
}

std::vector<BotQuestData::Vendor> const* BotQuestData::GetVendors(uint32 mapId) const
{
    auto itr = _vendors.find(mapId);
    return itr != _vendors.end() ? &itr->second : nullptr;
}

bool BotQuestData::IsSupported(Quest const* quest) const
{
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_CAST) ||
        quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_REPEATABLE) ||
        !GetEnders(quest->GetQuestId()))
        return false;

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if ((quest->RequiredNpcOrGo[i] > 0 && !GetSpawns(uint32(quest->RequiredNpcOrGo[i]))) ||
            (quest->RequiredNpcOrGo[i] < 0 && !GetObjectSpawns(uint32(-quest->RequiredNpcOrGo[i]))))
            return false;

    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemId[i] != quest->GetSrcItemId() &&
            !GetDroppers(quest->RequiredItemId[i]) && !GetObjectSources(quest->RequiredItemId[i]))
            return false;

    return true;
}

char const* BotQuester::GetActivityName(Activity activity)
{
    switch (activity)
    {
        case Activity::Dead: return "dead";
        case Activity::Fighting: return "fighting";
        case Activity::Resting: return "eating/drinking";
        case Activity::Vendor: return "at a vendor";
        case Activity::Looting: return "looting";
        case Activity::HandingIn: return "handing in";
        case Activity::TakingQuests: return "taking quests";
        case Activity::Hunting: return "hunting quest targets";
        case Activity::Grinding: return "grinding";
        case Activity::Travelling: return "travelling";
        default: return "idle";
    }
}

std::vector<std::string> BotQuester::Describe(Player* bot) const
{
    std::vector<std::string> lines;
    lines.push_back(Trinity::StringFormat("{}: level {}, {}, map {} at {:.0f} {:.0f} {:.0f}.", bot->GetName(), bot->GetLevel(),
        GetActivityName(_activity), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()));

    BotQuestData::Zone const* zone = sBotQuestData.GetZone(_zoneId);
    lines.push_back(zone ? Trinity::StringFormat("Zone: {} (levels {}-{}), {} zones used up.", BotQuestData::GetZoneName(zone->Id),
        zone->MinLevel, zone->MaxLevel, _exhaustedZones.size()) : std::string("Zone: none planned."));

    if (Unit* victim = bot->GetVictim())
        lines.push_back(Trinity::StringFormat("Target: {} level {}, {:.0f}% health, {:.1f} yards, {}{}{}; motion {}, {} attackers.",
            victim->GetName(), victim->GetLevel(), victim->GetHealthPct(), bot->GetDistance(victim),
            bot->IsWithinMeleeRange(victim) ? "in melee" : "not in melee", victim->ToCreature() && victim->ToCreature()->IsInEvadeMode() ? ", evading" : "",
            victim->GetVictim() == bot ? ", fighting the bot" : (victim->GetVictim() ? ", fighting someone else" : ""),
            uint32(bot->GetMotionMaster()->GetCurrentMovementGeneratorType()), bot->getAttackers().size()));
    else if (!bot->getAttackers().empty())
        lines.push_back(Trinity::StringFormat("No target, {} attackers; first: {} {:.1f} yards away.", bot->getAttackers().size(),
            (*bot->getAttackers().begin())->GetName(), bot->GetDistance(*bot->getAttackers().begin())));

    if (_moving)
        lines.push_back(Trinity::StringFormat("Moving to {:.0f} {:.0f} {:.0f}, {:.0f} yards away; travel target {}.", _moveDest.GetPositionX(),
            _moveDest.GetPositionY(), _moveDest.GetPositionZ(), bot->GetExactDist2d(_moveDest), _travelling ? "set" : "none"));

    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = questId ? sObjectMgr->GetQuestTemplate(questId) : nullptr;
        if (!quest)
            continue;

        std::string progress;
        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i])
                progress += Trinity::StringFormat(" {}/{}", bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]),
                    quest->RequiredNpcOrGoCount[i]);
        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i])
                progress += Trinity::StringFormat(" {}/{} items", bot->GetItemCount(quest->RequiredItemId[i]), quest->RequiredItemCount[i]);

        lines.push_back(Trinity::StringFormat("  [{}] {}{}{}", questId, quest->GetLogTitle(),
            bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE ? " (complete)" : "", progress));
    }

    return lines;
}

uint32 BotQuester::Think(Player* bot)
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInFlight())
        return THINK_MS;

    if (!bot->IsAlive())
    {
        _activity = Activity::Dead;
        Dead(bot);
        return THINK_MS;
    }

    if (_vendorCooldownMs)
        _vendorCooldownMs = _vendorCooldownMs > THINK_MS ? _vendorCooldownMs - THINK_MS : 0;

    _skipResetMs += THINK_MS;
    if (_skipResetMs >= SKIP_RESET_MS)
    {
        _skipResetMs = 0;
        _skippedSpawns.clear();
    }

    if (bot->GetLevel() != _lastLevel)
    {
        LevelUp(bot);
        _replanMs = 0;
    }

    _replanMs = _replanMs > THINK_MS ? _replanMs - THINK_MS : 0;
    if (!_replanMs)
    {
        _replanMs = REPLAN_MS;
        PlanZone(bot);
    }

    if (Fight(bot))
    {
        _activity = Activity::Fighting;
        return FIGHT_THINK_MS;
    }

    if (BotCompanion::RestIfNeeded(bot))
        _activity = Activity::Resting;
    else if (VisitVendor(bot))
        _activity = Activity::Vendor;
    else if (Loot(bot))
        _activity = Activity::Looting;
    else if (HandIn(bot, true))
        _activity = Activity::HandingIn;
    else if (TakeQuests(bot))
        _activity = Activity::TakingQuests;
    else if (Hunt(bot))
        _activity = Activity::Hunting;
    // Quests to hand in further away (talk-to quests sending it to the next town) once nothing is left here.
    else if (HandIn(bot, false))
        _activity = Activity::HandingIn;
    else if (Grind(bot))
        _activity = Activity::Grinding;
    else if (Travel(bot))
        _activity = Activity::Travelling;
    else
        _activity = Activity::Idle;

    // A little randomness, so bots that started together don't keep acting in step.
    return THINK_MS + urand(0, THINK_JITTER_MS);
}

bool BotQuester::Fight(Player* bot)
{
    Unit* target = bot->GetVictim();
    if (!target || !target->IsAlive())
    {
        target = nullptr;
        for (Unit* attacker : bot->getAttackers())
            if (attacker->IsAlive() && bot->IsValidAttackTarget(attacker))
            {
                target = attacker;
                break;
            }
    }

    if (!target)
        return false;

    Engage(bot, target);
    return true;
}

void BotQuester::Engage(Player* bot, Unit* target)
{
    if (bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    bot->SetSelection(target->GetGUID());
    bool melee = BotCompanion::GetRole(bot) == BotCompanion::Role::Melee ||
        BotCompanion::GetRole(bot) == BotCompanion::Role::Tank;
    bool newTarget = bot->GetVictim() != target;
    if (newTarget)
        bot->Attack(target, melee);

    MotionMaster* motion = bot->GetMotionMaster();
    if (newTarget || motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
    {
        motion->Clear();
        if (melee)
            motion->MoveChase(target);
        else
            motion->MoveChase(target, RANGED_DISTANCE);
    }
    _moving = false;

    // The rotation never starts a fight on its own (out of combat it only buffs), so open it with the best
    // ability, like a player pressing .rot next; melee auto-attack opens it too once in range.
    if (!bot->IsInCombat())
        sRotationBotMgr.Next(bot);
}

void BotQuester::MoveTo(Player* bot, Position const& pos, float spread)
{
    MotionMaster* motion = bot->GetMotionMaster();
    if (_moving && motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE && _moveDest.GetExactDist2d(pos) < 3.0f)
        return;

    _moveDest = pos;
    _moveActual = pos;
    if (spread > 0.0f)
    {
        float angle = frand(0.0f, 2.0f * float(M_PI));
        float distance = frand(0.0f, spread);
        _moveActual.Relocate(pos.GetPositionX() + std::cos(angle) * distance, pos.GetPositionY() + std::sin(angle) * distance,
            pos.GetPositionZ());
    }

    motion->Clear();
    motion->MovePoint(0, _moveActual.GetPositionX(), _moveActual.GetPositionY(), _moveActual.GetPositionZ(), true);
    _moving = true;
}

bool BotQuester::Loot(Player* bot)
{
    // Still waiting for the server's answer to the last loot request.
    if (!_lootGuid.IsEmpty())
    {
        _lootAttemptMs += THINK_MS;
        if (_lootAttemptMs < 3 * IN_MILLISECONDS)
            return true;

        _lootedCorpses.insert(_lootGuid);
        _lootGuid.Clear();
    }

    if (_lootedCorpses.size() > 50)
        _lootedCorpses.clear();

    Creature* corpse = nullptr;
    LootableCheck check(bot, _lootedCorpses, 30.0f);
    Trinity::CreatureLastSearcher<LootableCheck> searcher(bot, corpse, check);
    Cell::VisitGridObjects(bot, searcher, 30.0f);
    if (!corpse)
        return false;

    if (bot->GetDistance(corpse) > INTERACTION_DISTANCE - 1.0f)
    {
        MoveTo(bot, corpse->GetPosition());
        return true;
    }

    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    _lootGuid = corpse->GetGUID();
    _lootAttemptMs = 0;
    WorldPacket* packet = new WorldPacket(CMSG_LOOT, 8);
    *packet << corpse->GetGUID();
    QueuePacket(bot, packet);
    return true;
}

void BotQuester::OnLootResponse(Player* bot, WorldPacket& packet)
{
    ObjectGuid owner;
    uint8 acquireReason = 0;
    packet.rpos(0);
    packet >> owner >> acquireReason;

    if (acquireReason)
    {
        uint32 coins = 0;
        uint8 count = 0;
        packet >> coins >> count;
        for (uint8 i = 0; i < count; ++i)
        {
            uint8 slot = 0;
            packet >> slot;
            packet.read_skip(4 + 4 + 4 + 4 + 4 + 1); // Item, count, display, random seed and properties, UI type

            WorldPacket* take = new WorldPacket(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            *take << uint8(slot);
            QueuePacket(bot, take);
        }

        if (coins)
            QueuePacket(bot, new WorldPacket(CMSG_LOOT_MONEY, 0));
    }

    WorldPacket* release = new WorldPacket(CMSG_LOOT_RELEASE, 8);
    *release << owner;
    QueuePacket(bot, release);

    _lootedCorpses.insert(owner);
    if (owner == _lootGuid)
        _lootGuid.Clear();
}

bool BotQuester::HandIn(Player* bot, bool nearbyOnly)
{
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        std::vector<uint32> const* enders = sBotQuestData.GetEnders(questId);
        if (!quest || !enders)
            continue;

        BotQuestData::Spawn const* spawn = NearestSpawn(bot, *enders, false, &_skippedSpawns);
        if (!spawn || (nearbyOnly && bot->GetExactDist2d(spawn->Pos) > NEARBY_GIVER_RANGE))
            continue;

        Creature* ender = nullptr;
        for (uint32 entry : *enders)
            if ((ender = FindCreature(bot, entry, SEARCH_RANGE)))
                break;

        if (!ender)
        {
            // At the spot and nobody there: try another of its enders later.
            if (bot->GetExactDist2d(spawn->Pos) < INTERACTION_DISTANCE)
                _skippedSpawns.insert(spawn);
            else
                MoveTo(bot, spawn->Pos, bot->GetExactDist2d(spawn->Pos) > SEARCH_RANGE ? SPREAD : 0.0f);
            return true;
        }

        if (bot->GetDistance(ender) > INTERACTION_DISTANCE - 1.0f)
        {
            MoveTo(bot, ender->GetPosition());
            return true;
        }

        // The reward it would use most.
        uint32 reward = 0;
        float bestScore = -std::numeric_limits<float>::max();
        for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
            if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]))
                if (float score = BotCompanion::ScoreItem(bot, *item); score > bestScore)
                {
                    bestScore = score;
                    reward = i;
                }

        if (bot->CanRewardQuest(quest, reward, false))
        {
            bot->RewardQuest(quest, reward, ender);
            EquipUpgrades(bot);
        }
        return true;
    }

    return false;
}

bool BotQuester::TakeQuests(Player* bot)
{
    std::vector<BotQuestData::Spawn> const* givers = sBotQuestData.GetGivers(bot->GetMapId());
    if (!givers)
        return false;

    // The nearest giver with something for it, within walking distance.
    BotQuestData::Spawn const* best = nullptr;
    float bestDist = NEARBY_GIVER_RANGE;
    for (BotQuestData::Spawn const& spawn : *givers)
    {
        float dist = bot->GetExactDist2d(spawn.Pos);
        if (dist < bestDist && !_skippedSpawns.count(&spawn) && !AvailableQuests(bot, spawn.Entry).empty())
        {
            best = &spawn;
            bestDist = dist;
        }
    }

    if (!best)
        return false;

    Creature* giver = FindCreature(bot, best->Entry, SEARCH_RANGE);
    if (!giver)
    {
        // At the spot and nobody there (an event or phased NPC): another giver.
        if (bot->GetExactDist2d(best->Pos) < INTERACTION_DISTANCE)
            _skippedSpawns.insert(best);
        else
            MoveTo(bot, best->Pos);
        return true;
    }

    if (bot->GetDistance(giver) > INTERACTION_DISTANCE - 1.0f)
    {
        MoveTo(bot, giver->GetPosition());
        return true;
    }

    uint32 taken = 0;
    for (Quest const* quest : AvailableQuests(bot, giver->GetEntry()))
    {
        if (!giver->hasQuest(quest->GetQuestId()))
            continue;

        bot->AddQuestAndCheckCompletion(quest, giver);
        if (bot->GetQuestStatus(quest->GetQuestId()) != QUEST_STATUS_NONE)
            ++taken;
    }

    // It got nothing here after all (conditions the bot data doesn't know): another giver.
    if (!taken)
        _skippedSpawns.insert(best);

    return true;
}

bool BotQuester::Hunt(Player* bot)
{
    // Everything its quests still need: creatures to kill or loot an item from, and objects to use or
    // take an item from.
    std::unordered_set<uint32> creatures;
    std::unordered_set<uint32> objects;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        {
            int32 target = quest->RequiredNpcOrGo[i];
            if (!target || bot->GetReqKillOrCastCurrentCount(questId, target) >= quest->RequiredNpcOrGoCount[i])
                continue;

            if (target > 0)
                creatures.insert(uint32(target));
            else
                objects.insert(uint32(-target));
        }

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        {
            uint32 item = quest->RequiredItemId[i];
            if (!item || bot->GetItemCount(item) >= quest->RequiredItemCount[i])
                continue;

            if (std::vector<uint32> const* droppers = sBotQuestData.GetDroppers(item))
                creatures.insert(droppers->begin(), droppers->end());
            if (std::vector<uint32> const* sources = sBotQuestData.GetObjectSources(item))
                objects.insert(sources->begin(), sources->end());
        }
    }

    if (creatures.empty() && objects.empty())
        return false;

    if (_usedObjects.size() > 50)
        _usedObjects.clear();

    Creature* creature = nullptr;
    if (!creatures.empty())
    {
        FightableCheck check(bot, &creatures, SEARCH_RANGE);
        Trinity::CreatureLastSearcher<FightableCheck> searcher(bot, creature, check);
        Cell::VisitGridObjects(bot, searcher, SEARCH_RANGE);
    }

    GameObject* object = nullptr;
    if (!objects.empty())
    {
        UsableObjectCheck check(bot, objects, _usedObjects, SEARCH_RANGE);
        Trinity::GameObjectLastSearcher<UsableObjectCheck> searcher(bot, object, check);
        Cell::VisitGridObjects(bot, searcher, SEARCH_RANGE);
    }

    // The nearer of the two.
    if (object && (!creature || bot->GetDistance(object) < bot->GetDistance(creature)))
    {
        if (bot->GetDistance(object) > INTERACTION_DISTANCE - 1.0f)
        {
            MoveTo(bot, object->GetPosition());
            return true;
        }

        // Like a client clicking it: the server opens its loot (handled in OnLootResponse) or gives the credit.
        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        WorldPacket* packet = new WorldPacket(CMSG_GAMEOBJ_USE, 8);
        *packet << object->GetGUID();
        QueuePacket(bot, packet);
        _usedObjects.insert(object->GetGUID());
        return true;
    }

    if (creature)
    {
        Engage(bot, creature);
        return true;
    }

    // None near it: off to where they spawn.
    BotQuestData::Spawn const* spawn = NearestSpawn(bot, std::vector<uint32>(creatures.begin(), creatures.end()));
    BotQuestData::Spawn const* objectSpawn = NearestSpawn(bot, std::vector<uint32>(objects.begin(), objects.end()), true);
    if (objectSpawn && (!spawn || bot->GetExactDist2d(objectSpawn->Pos) < bot->GetExactDist2d(spawn->Pos)))
        spawn = objectSpawn;

    if (spawn)
    {
        // At their spawn and none there yet: grind what is around while they come back, or wait, rather
        // than wander off.
        if (bot->GetExactDist2d(spawn->Pos) > 10.0f)
            MoveTo(bot, spawn->Pos, SPREAD);
        else
            Grind(bot);
        return true;
    }

    return false;
}

bool BotQuester::Grind(Player* bot)
{
    Creature* target = nullptr;
    FightableCheck check(bot, nullptr, SEARCH_RANGE);
    Trinity::CreatureLastSearcher<FightableCheck> searcher(bot, target, check);
    Cell::VisitGridObjects(bot, searcher, SEARCH_RANGE);
    if (!target)
        return false;

    Engage(bot, target);
    return true;
}

bool BotQuester::Travel(Player* bot)
{
    // Nothing to do here: to a quest giver of its zone with quests for it, or the zone's middle when none
    // is near enough to tell. Looking through givers is slow, so the destination is kept for a while.
    if (_travelCheckMs > 0)
    {
        _travelCheckMs = _travelCheckMs > THINK_MS ? _travelCheckMs - THINK_MS : 0;
        if (_travelling)
        {
            MoveTo(bot, _travelDest, SPREAD);
            return true;
        }
        return false;
    }

    _travelCheckMs = TRAVEL_CHECK_MS;
    _travelling = false;

    BotQuestData::Zone const* zone = sBotQuestData.GetZone(_zoneId);
    if (!zone)
        return false;

    Spawn const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (Spawn const* spawn : zone->Givers)
    {
        float dist = bot->GetExactDist2d(spawn->Pos);
        if (dist < bestDist && !_skippedSpawns.count(spawn) && !AvailableQuests(bot, spawn->Entry).empty())
        {
            best = spawn;
            bestDist = dist;
        }
    }

    if (!best)
    {
        // Nothing new for it in this zone, and nothing left to finish: another one.
        bool unfinished = false;
        for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE && !unfinished; ++slot)
            unfinished = bot->GetQuestSlotQuestId(slot) != 0;

        if (!unfinished)
        {
            _exhaustedZones.insert(_zoneId);
            _replanMs = 0;
        }
        return false;
    }

    _travelDest = best->Pos;
    _travelling = true;
    MoveTo(bot, best->Pos, SPREAD);
    return true;
}

void BotQuester::PlanZone(Player* bot)
{
    BotQuestData::Zone const* zone = sBotQuestData.PickZone(bot, _exhaustedZones);
    if (!zone && !_exhaustedZones.empty())
    {
        // Every zone for its level was used up: they may have new quests for it by now.
        _exhaustedZones.clear();
        zone = sBotQuestData.PickZone(bot, _exhaustedZones);
    }

    uint32 zoneId = zone ? zone->Id : 0;
    if (zoneId != _zoneId)
    {
        _zoneId = zoneId;
        _travelCheckMs = 0; // Head there now
    }
}

bool BotQuester::VisitVendor(Player* bot)
{
    if (_vendorCooldownMs > 0)
        return false;

    // Time to go: bags nearly full, or something it wears nearly broken.
    bool needsRepair = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END && !needsRepair; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (uint32 max = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                needsRepair = item->GetUInt32Value(ITEM_FIELD_DURABILITY) * 4 < max;

    if (!needsRepair && bot->GetFreeInventorySpace() >= VENDOR_FREE_SLOTS)
        return false;

    std::vector<BotQuestData::Vendor> const* vendors = sBotQuestData.GetVendors(bot->GetMapId());
    FactionTemplateEntry const* botFaction = bot->GetFactionTemplateEntry();
    if (!vendors || !botFaction)
        return false;

    // The nearest vendor that would serve it (and repair, if it needs that).
    BotQuestData::Vendor const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (BotQuestData::Vendor const& vendor : *vendors)
    {
        if (needsRepair && !vendor.Repairs)
            continue;

        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(vendor.Faction);
        if (!faction || faction->IsHostileTo(*botFaction))
            continue;

        float dist = bot->GetExactDist2d(vendor.Where.Pos);
        if (dist < bestDist)
        {
            best = &vendor;
            bestDist = dist;
        }
    }

    if (!best)
    {
        _vendorCooldownMs = VENDOR_COOLDOWN_MS;
        return false;
    }

    Creature* npc = FindCreature(bot, best->Where.Entry, SEARCH_RANGE);
    if (!npc || bot->GetDistance(npc) > INTERACTION_DISTANCE - 1.0f)
    {
        MoveTo(bot, npc ? npc->GetPosition() : best->Where.Pos);
        return true;
    }

    // Sells junk, and gear and goods it won't use; keeps quest items, its food, water and bags, and
    // anything better than what it wears.
    auto sell = [bot, npc](Item* item)
    {
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto->SellPrice || proto->Class == ITEM_CLASS_QUEST || proto->StartQuest || proto->InventoryType == INVTYPE_BAG ||
            proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_REAGENT || proto->Class == ITEM_CLASS_KEY)
            return;

        if (proto->Quality > ITEM_QUALITY_POOR && proto->InventoryType)
        {
            uint16 dest;
            if (bot->CanEquipItem(NULL_SLOT, dest, item, true) == EQUIP_ERR_OK)
            {
                Item* current = bot->GetItemByPos(dest);
                float score = BotCompanion::ScoreItem(bot, *proto);
                if (score > 0.0f && (!current || score > BotCompanion::ScoreItem(bot, *current->GetTemplate())))
                    return;
            }
        }

        WorldPacket* packet = new WorldPacket(CMSG_SELL_ITEM, 8 + 8 + 4);
        *packet << npc->GetGUID() << item->GetGUID() << uint32(item->GetCount());
        QueuePacket(bot, packet);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            sell(item);

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
            if (Bag* container = bagItem->ToBag())
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    if (Item* item = container->GetItemByPos(uint8(slot)))
                        sell(item);

    if (best->Repairs)
    {
        WorldPacket* packet = new WorldPacket(CMSG_REPAIR_ITEM, 8 + 8 + 1);
        *packet << npc->GetGUID() << ObjectGuid::Empty << uint8(0); // Everything, from its own money
        QueuePacket(bot, packet);
    }

    _vendorCooldownMs = VENDOR_COOLDOWN_MS;
    return true;
}

void BotQuester::Dead(Player* bot)
{
    // Like a player: release, run back from the graveyard as a ghost, and take the body back.
    if (!bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
    {
        _deadMs += THINK_MS;
        _ghostMs = 0;
        if (_deadMs >= RELEASE_MS)
        {
            _deadMs = 0;
            WorldPacket* packet = new WorldPacket(CMSG_REPOP_REQUEST, 1);
            *packet << uint8(0);
            QueuePacket(bot, packet);
        }
        return;
    }

    _ghostMs += THINK_MS;
    WorldLocation const& corpse = bot->GetCorpseLocation();

    // Corpse on another map (it died in an instance), or the run is taking far too long: back to life here.
    if (corpse.GetMapId() != bot->GetMapId() || _ghostMs >= GHOST_GIVE_UP_MS)
    {
        _ghostMs = 0;
        bot->ResurrectPlayer(0.5f);
        bot->SpawnCorpseBones();
        return;
    }

    if (bot->GetExactDist2d(corpse) > RECLAIM_DISTANCE)
    {
        MoveTo(bot, corpse);
        return;
    }

    // At the body: the server takes it back once the corpse delay has passed.
    _reclaimMs += THINK_MS;
    if (_reclaimMs >= 3 * IN_MILLISECONDS)
    {
        _reclaimMs = 0;
        WorldPacket* packet = new WorldPacket(CMSG_RECLAIM_CORPSE, 8);
        *packet << bot->GetGUID();
        QueuePacket(bot, packet);
    }
}

void BotQuester::LevelUp(Player* bot)
{
    _lastLevel = bot->GetLevel();
    BotCompanion::LearnSpellsForLevel(bot);

    // Its tree: the one it has most points in, or one picked by its guid the first time.
    uint8 tree = RotationProfiles::GetMainTree(bot).value_or(uint8(bot->GetGUID().GetCounter() % 3));
    BotCompanion::SpendFreeTalents(bot, tree);
}

void BotQuester::EquipUpgrades(Player* bot)
{
    // Wears an item from its backpack or bags when it scores higher than what is in that slot, like a
    // player right-clicking it.
    auto consider = [bot](uint8 bag, uint8 slot)
    {
        Item* item = bot->GetItemByPos(bag, slot);
        if (!item || !item->GetTemplate()->InventoryType || item->GetTemplate()->InventoryType == INVTYPE_BAG)
            return;

        uint16 dest;
        if (bot->CanEquipItem(NULL_SLOT, dest, item, true) != EQUIP_ERR_OK)
            return;

        float score = BotCompanion::ScoreItem(bot, *item->GetTemplate());
        Item* current = bot->GetItemByPos(dest);
        if (score <= 0.0f || (current && score <= BotCompanion::ScoreItem(bot, *current->GetTemplate())))
            return;

        WorldPacket* packet = new WorldPacket(CMSG_AUTOEQUIP_ITEM, 2);
        *packet << uint8(bag) << uint8(slot);
        QueuePacket(bot, packet);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(INVENTORY_SLOT_BAG_0, slot);

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
            if (Bag* container = bagItem->ToBag())
                for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                    consider(bag, uint8(slot));
}
