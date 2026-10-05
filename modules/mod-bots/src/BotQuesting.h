/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#ifndef MOD_BOTS_BOT_QUESTING_H
#define MOD_BOTS_BOT_QUESTING_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class Quest;
class WorldPacket;

// What the server's bots know about the world to quest in it, read once at startup: where creatures
// spawn, who gives and takes quests, and which creatures drop which quest items.
class BotQuestData
{
public:
    struct Spawn
    {
        uint32 Entry;
        uint32 MapId;
        Position Pos;
    };

    static BotQuestData& Instance();
    void Load();

    std::vector<Spawn> const* GetSpawns(uint32 entry) const;
    // Creatures that start, and that end, a quest.
    std::vector<uint32> const* GetStarters(uint32 questId) const;
    std::vector<uint32> const* GetEnders(uint32 questId) const;
    // The quests a creature starts.
    std::vector<uint32> const* GetQuestsOf(uint32 creatureEntry) const;
    // Creatures dropping a quest item.
    std::vector<uint32> const* GetDroppers(uint32 itemId) const;
    // Quest givers' spawns on a map.
    std::vector<Spawn> const* GetGivers(uint32 mapId) const;

    // Whether bots can do the quest: it needs only creatures killed and items they drop, is handed in
    // to a creature, and isn't an event, escort or timed quest.
    bool IsSupported(Quest const* quest) const;

private:
    BotQuestData() = default;

    std::unordered_map<uint32, std::vector<Spawn>> _spawns;
    std::unordered_map<uint32, std::vector<uint32>> _starters;
    std::unordered_map<uint32, std::vector<uint32>> _enders;
    std::unordered_map<uint32, std::vector<uint32>> _questsOf;
    std::unordered_map<uint32, std::vector<uint32>> _droppers;
    std::unordered_map<uint32, std::vector<Spawn>> _givers;
};

#define sBotQuestData BotQuestData::Instance()

// One of the server's bots questing on its own: it takes quests near it, kills and loots what they
// need, hands them in, equips better rewards, learns spells and spends talents as it levels, and grinds
// when there is nothing to do.
class BotQuester
{
public:
    // Called on the world thread while maps are idle. Returns how long until it wants to think again.
    uint32 Think(Player* bot);

    // SMSG_LOOT_RESPONSE: takes everything and closes the loot.
    void OnLootResponse(Player* bot, WorldPacket& packet);

private:
    bool Fight(Player* bot);
    bool Loot(Player* bot);
    bool HandIn(Player* bot);
    bool TakeQuests(Player* bot);
    bool Hunt(Player* bot);
    bool Grind(Player* bot);
    bool Travel(Player* bot);

    void MoveTo(Player* bot, Position const& pos);
    void Engage(Player* bot, class Unit* target);
    void LevelUp(Player* bot);
    void EquipUpgrades(Player* bot);

    uint8 _lastLevel = 0;
    uint32 _deadMs = 0;
    uint32 _lootAttemptMs = 0;
    ObjectGuid _lootGuid;
    Position _moveDest;
    bool _moving = false;
    std::unordered_set<ObjectGuid> _lootedCorpses;
    uint32 _travelCheckMs = 0;
    bool _travelling = false;
    Position _travelDest;
};

#endif
