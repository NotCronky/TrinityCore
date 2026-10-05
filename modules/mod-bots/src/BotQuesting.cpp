/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotQuesting.h"
#include "BotCompanion.h"

#include "Bag.h"

#include "CellImpl.h"
#include "Creature.h"
#include "DatabaseEnv.h"
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
#include "RotationProfiles.h" // mod-rotation-bot: the talent tree with the most points
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <limits>

namespace
{
    constexpr uint32 THINK_MS = 1000;
    constexpr uint32 FIGHT_THINK_MS = 250;
    constexpr uint32 RESURRECT_MS = 15 * IN_MILLISECONDS;
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

    // The nearest live creature of an entry, to talk to.
    Creature* FindCreature(Player* bot, uint32 entry, float range)
    {
        Creature* found = nullptr;
        Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck check(*bot, entry, true, range);
        Trinity::CreatureLastSearcher<Trinity::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(bot, found, check);
        Cell::VisitGridObjects(bot, searcher, range);
        return found;
    }

    BotQuestData::Spawn const* NearestSpawn(Player* bot, std::vector<uint32> const& entries)
    {
        BotQuestData::Spawn const* nearest = nullptr;
        float nearestDist = std::numeric_limits<float>::max();
        for (uint32 entry : entries)
        {
            if (std::vector<BotQuestData::Spawn> const* spawns = sBotQuestData.GetSpawns(entry))
            {
                for (BotQuestData::Spawn const& spawn : *spawns)
                {
                    if (spawn.MapId != bot->GetMapId())
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

    for (auto const& [entry, quests] : _questsOf)
        if (std::vector<Spawn> const* spawns = GetSpawns(entry))
            for (Spawn const& spawn : *spawns)
                _givers[spawn.MapId].push_back(spawn);

    TC_LOG_INFO("module", "mod-bots: questing data: {} creature entries with spawns, {} quest givers, {} quest items from loot.",
        _spawns.size(), _questsOf.size(), _droppers.size());
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

bool BotQuestData::IsSupported(Quest const* quest) const
{
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_CAST) ||
        quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_REPEATABLE) ||
        !GetEnders(quest->GetQuestId()))
        return false;

    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i] < 0 || (quest->RequiredNpcOrGo[i] > 0 && !GetSpawns(uint32(quest->RequiredNpcOrGo[i]))))
            return false;

    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i] && quest->RequiredItemId[i] != quest->GetSrcItemId() && !GetDroppers(quest->RequiredItemId[i]))
            return false;

    return true;
}

uint32 BotQuester::Think(Player* bot)
{
    if (!bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsInFlight())
        return THINK_MS;

    // Dead: back on its feet after a while where it died (no corpse runs yet).
    if (!bot->IsAlive())
    {
        _deadMs += THINK_MS;
        if (_deadMs >= RESURRECT_MS)
        {
            _deadMs = 0;
            bot->ResurrectPlayer(0.5f);
            bot->SpawnCorpseBones();
        }
        return THINK_MS;
    }

    if (bot->GetLevel() != _lastLevel)
        LevelUp(bot);

    if (Fight(bot))
        return FIGHT_THINK_MS;

    if (BotCompanion::RestIfNeeded(bot))
        return THINK_MS;

    if (Loot(bot) || HandIn(bot) || TakeQuests(bot) || Hunt(bot) || Grind(bot) || Travel(bot))
        return THINK_MS;

    return THINK_MS;
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
}

void BotQuester::MoveTo(Player* bot, Position const& pos)
{
    MotionMaster* motion = bot->GetMotionMaster();
    if (_moving && motion->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE && _moveDest.GetExactDist2d(pos) < 3.0f)
        return;

    motion->Clear();
    motion->MovePoint(0, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), true);
    _moveDest = pos;
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

bool BotQuester::HandIn(Player* bot)
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

        BotQuestData::Spawn const* spawn = NearestSpawn(bot, *enders);
        if (!spawn)
            continue;

        Creature* ender = nullptr;
        for (uint32 entry : *enders)
            if ((ender = FindCreature(bot, entry, SEARCH_RANGE)))
                break;

        if (!ender || bot->GetDistance(ender) > INTERACTION_DISTANCE - 1.0f)
        {
            MoveTo(bot, ender ? ender->GetPosition() : spawn->Pos);
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
        if (dist < bestDist && !AvailableQuests(bot, spawn.Entry).empty())
        {
            best = &spawn;
            bestDist = dist;
        }
    }

    if (!best)
        return false;

    Creature* giver = FindCreature(bot, best->Entry, SEARCH_RANGE);
    if (!giver || bot->GetDistance(giver) > INTERACTION_DISTANCE - 1.0f)
    {
        MoveTo(bot, giver ? giver->GetPosition() : best->Pos);
        return true;
    }

    for (Quest const* quest : AvailableQuests(bot, giver->GetEntry()))
        if (giver->hasQuest(quest->GetQuestId()))
            bot->AddQuestAndCheckCompletion(quest, giver);

    return true;
}

bool BotQuester::Hunt(Player* bot)
{
    // Every creature its quests still need: to kill, or to loot an item from.
    std::unordered_set<uint32> wanted;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId || bot->GetQuestStatus(questId) != QUEST_STATUS_INCOMPLETE)
            continue;

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredNpcOrGo[i] > 0 &&
                bot->GetReqKillOrCastCurrentCount(questId, quest->RequiredNpcOrGo[i]) < quest->RequiredNpcOrGoCount[i])
                wanted.insert(uint32(quest->RequiredNpcOrGo[i]));

        for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
            if (quest->RequiredItemId[i] && bot->GetItemCount(quest->RequiredItemId[i]) < quest->RequiredItemCount[i])
                if (std::vector<uint32> const* droppers = sBotQuestData.GetDroppers(quest->RequiredItemId[i]))
                    wanted.insert(droppers->begin(), droppers->end());
    }

    if (wanted.empty())
        return false;

    Creature* target = nullptr;
    FightableCheck check(bot, &wanted, SEARCH_RANGE);
    Trinity::CreatureLastSearcher<FightableCheck> searcher(bot, target, check);
    Cell::VisitGridObjects(bot, searcher, SEARCH_RANGE);
    if (target)
    {
        Engage(bot, target);
        return true;
    }

    // None alive near it: off to where they spawn.
    std::vector<uint32> entries(wanted.begin(), wanted.end());
    if (BotQuestData::Spawn const* spawn = NearestSpawn(bot, entries))
    {
        if (bot->GetExactDist2d(spawn->Pos) > 10.0f)
        {
            MoveTo(bot, spawn->Pos);
            return true;
        }
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
    // Nothing to do here: the nearest giver on the map with a quest for it, however far. Looking through
    // every giver on the map is slow, so the destination is kept for a while.
    if (_travelCheckMs > 0)
    {
        _travelCheckMs = _travelCheckMs > THINK_MS ? _travelCheckMs - THINK_MS : 0;
        if (_travelling)
        {
            MoveTo(bot, _travelDest);
            return true;
        }
        return false;
    }

    _travelCheckMs = TRAVEL_CHECK_MS;
    _travelling = false;
    std::vector<BotQuestData::Spawn> const* givers = sBotQuestData.GetGivers(bot->GetMapId());
    if (!givers)
        return false;

    BotQuestData::Spawn const* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    for (BotQuestData::Spawn const& spawn : *givers)
    {
        float dist = bot->GetExactDist2d(spawn.Pos);
        if (dist < bestDist && !AvailableQuests(bot, spawn.Entry).empty())
        {
            best = &spawn;
            bestDist = dist;
        }
    }

    if (!best)
        return false;

    _travelDest = best->Pos;
    _travelling = true;
    MoveTo(bot, best->Pos);
    return true;
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
