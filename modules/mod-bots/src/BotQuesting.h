/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#ifndef MOD_BOTS_BOT_QUESTING_H
#define MOD_BOTS_BOT_QUESTING_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <string>
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

    // A questing zone: where its quest givers stand and which levels its quests are for.
    struct Zone
    {
        uint32 Id = 0;
        uint32 MapId = 0;
        uint8 MinLevel = 0;
        uint8 MaxLevel = 0;
        uint32 Quests[2] = { 0, 0 }; // Bot-doable quests for Alliance, Horde
        Position Center;
        std::vector<Spawn const*> Givers;
    };

    struct Vendor
    {
        Spawn Where;
        bool Repairs;
        uint32 Faction; // FactionTemplate.dbc
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
    // Game objects' spawns, and the objects a quest item comes from (chests, crates, plants...).
    std::vector<Spawn> const* GetObjectSpawns(uint32 entry) const;
    std::vector<uint32> const* GetObjectSources(uint32 itemId) const;
    // Vendors on a map.
    std::vector<Vendor> const* GetVendors(uint32 mapId) const;

    // The nearest zone on the bot's continent that suits its level and has enough quests for its faction,
    // skipping exhausted ones; nullptr when there is none.
    Zone const* PickZone(Player* bot, std::unordered_set<uint32> const& exhausted) const;
    Zone const* GetZone(uint32 zoneId) const;
    static std::string GetZoneName(uint32 zoneId);

    // Whether bots can do the quest: it needs only creatures killed, objects used, and items from
    // creatures or objects, is handed in to a creature, and isn't an event, escort or timed quest.
    bool IsSupported(Quest const* quest) const;

private:
    BotQuestData() = default;

    std::unordered_map<uint32, std::vector<Spawn>> _spawns;
    std::unordered_map<uint32, std::vector<uint32>> _starters;
    std::unordered_map<uint32, std::vector<uint32>> _enders;
    std::unordered_map<uint32, std::vector<uint32>> _questsOf;
    std::unordered_map<uint32, std::vector<uint32>> _droppers;
    std::unordered_map<uint32, std::vector<Spawn>> _givers;
    std::unordered_map<uint32, std::vector<Spawn>> _objectSpawns;
    std::unordered_map<uint32, std::vector<uint32>> _objectSources;
    std::unordered_map<uint32, std::vector<Vendor>> _vendors;
    std::unordered_map<uint32, Zone> _zones;

    void LoadZones();
};

#define sBotQuestData BotQuestData::Instance()

// One of the server's bots questing on its own: it takes quests near it, kills and loots what they
// need, hands them in, equips better rewards, learns spells and spends talents as it levels, and grinds
// when there is nothing to do.
using Spawn = BotQuestData::Spawn;

class BotQuester
{
public:
    enum class Activity : uint8 { Idle, Dead, Fighting, Resting, Vendor, Looting, HandingIn, TakingQuests, Hunting, Grinding, Travelling, Count };
    static char const* GetActivityName(Activity activity);
    Activity GetActivity() const { return _activity; }
    uint32 GetZoneId() const { return _zoneId; }
    // For .bot info: what it is doing and why.
    std::vector<std::string> Describe(Player* bot) const;

    // Called on the world thread while maps are idle. Returns how long until it wants to think again.
    uint32 Think(Player* bot);

    // SMSG_LOOT_RESPONSE: takes everything and closes the loot.
    void OnLootResponse(Player* bot, WorldPacket& packet);

private:
    bool Fight(Player* bot);
    bool Loot(Player* bot);
    bool VisitVendor(Player* bot);
    void Dead(Player* bot);
    bool HandIn(Player* bot);
    bool TakeQuests(Player* bot);
    bool Hunt(Player* bot);
    bool Grind(Player* bot);
    bool Travel(Player* bot);

    void MoveTo(Player* bot, Position const& pos);
    void Engage(Player* bot, class Unit* target);
    void LevelUp(Player* bot);
    void PlanZone(Player* bot);
    void EquipUpgrades(Player* bot);

    Activity _activity = Activity::Idle;
    uint8 _lastLevel = 0;
    // The zone it is levelling in, zones with nothing left for it, and when to think about it again.
    uint32 _zoneId = 0;
    std::unordered_set<uint32> _exhaustedZones;
    uint32 _replanMs = 0;
    uint32 _deadMs = 0;
    uint32 _lootAttemptMs = 0;
    ObjectGuid _lootGuid;
    Position _moveDest;
    bool _moving = false;
    std::unordered_set<ObjectGuid> _lootedCorpses;
    std::unordered_set<ObjectGuid> _usedObjects;
    uint32 _travelCheckMs = 0;
    bool _travelling = false;
    Position _travelDest;
    // Quest giver and ender spawns it found nobody at (event or phased NPCs) or got nothing from; skipped
    // for a while so it doesn't walk back and forth.
    std::unordered_set<Spawn const*> _skippedSpawns;
    uint32 _skipResetMs = 0;
    uint32 _vendorCooldownMs = 0;  // After a vendor visit, so full bags of unsellable items don't send it back
    uint32 _ghostMs = 0;
    uint32 _reclaimMs = 0;
};

#endif
