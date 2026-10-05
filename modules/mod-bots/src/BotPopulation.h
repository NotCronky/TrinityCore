/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#ifndef MOD_BOTS_BOT_POPULATION_H
#define MOD_BOTS_BOT_POPULATION_H

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "ObjectGuid.h"

#include <array>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class WorldSession;

// The server's own bots: creates bot accounts and characters once, then keeps
// Bots.Population.Count of them online, exactly half Alliance and half Horde. Race and class follow
// bot_census within each faction; names come from bot_names.
//
// Everything runs on the world thread, from WorldScript hooks, while maps are idle.
class BotPopulation
{
public:
    static BotPopulation& Instance();

    void LoadConfig();
    // Reads the census, the names and the bots created before. Called once at startup.
    void Load();
    void Update(uint32 diff);
    // Stops creating and logging in; BotMgr logs the bots out at shutdown.
    void Shutdown() { _shuttingDown = true; }

    bool IsPopulationBot(ObjectGuid guid) const { return _factionOf.count(guid) != 0; }

    // Logs every bot of the population out, deletes their characters, and lets the population be made
    // again from scratch (level 1, new names). The bot accounts are kept. Returns what happens.
    std::string Reset();
    // Status lines for ".bot population".
    std::vector<std::string> Describe() const;

private:
    enum Faction : uint8 { FACTION_ALLIANCE = 0, FACTION_HORDE = 1, FACTION_COUNT = 2 };

    struct CensusEntry
    {
        uint8 Race;
        uint8 Class;
        uint32 Weight;
    };

    struct NameEntry
    {
        std::string Name;
        uint8 Gender; // 0 male, 1 female, 2 either
    };

    struct BotAccount
    {
        uint32 Id = 0;
        std::string Username;
        uint32 Characters = 0; // Created or being created
    };

    BotPopulation() = default;

    uint32 GetTargetPerFaction() const { return _count / 2; }

    void LoadCensus();
    void LoadNames();
    void LoadBots();

    void CreateCharacters();
    bool CreateCharacter(Faction faction);
    BotAccount* GetAccountWithRoom();
    WorldSession* GetCreationSession(BotAccount const& account);
    std::string TakeName(uint8 gender);

    void LogInBots(uint32 diff);
    void LogOutExtraBots();

    // Config
    uint32 _count = 0;
    uint32 _loginsPerSecond = 10;
    uint32 _charactersPerAccount = 10;

    bool _loaded = false;
    bool _resetting = false; // Waiting for the bots to log out before deleting them
    void FinishReset();
    bool _shuttingDown = false;

    std::array<std::vector<CensusEntry>, FACTION_COUNT> _census;
    std::array<uint64, FACTION_COUNT> _censusTotal = { };
    std::vector<NameEntry> _names; // Unused names, in random order

    std::vector<BotAccount> _accounts;
    std::unordered_map<uint32, std::unique_ptr<WorldSession>> _creationSessions; // By account; freed when creation is done

    // Created characters (saved), by faction, and every bot's faction.
    std::array<std::vector<ObjectGuid>, FACTION_COUNT> _bots;
    std::unordered_map<ObjectGuid, Faction> _factionOf;

    // With mod-progression: each bot's race, class and patch (progression_patch id; it only goes up), and
    // the patch the online bots follow: the one most online players are in.
    struct BotInfo
    {
        uint8 Race = 0;
        uint8 Class = 0;
        uint8 Patch = 0;
        uint32 PatchQuest = 0; // As loaded; turned into Patch once mod-progression has its patches
    };
    bool _patchesResolved = false;
    void ResolvePatches();
    std::unordered_map<ObjectGuid, BotInfo> _info;
    uint8 _targetPatch = 0; // 0: patches are ignored (mod-progression off)
    uint32 _targetCheckMs = 0;

    void UpdateTargetPatch();
    // Whether the bot can be online for the target patch: its race and class existed by then, and it
    // isn't in a later patch already.
    bool IsEligible(ObjectGuid guid) const;
    // Online bots behind the target patch move up to it.
    void AdvanceOnlineBots();
    std::array<uint32, FACTION_COUNT> _pendingCreations = { };
    AsyncCallbackProcessor<TransactionCallback> _saveCallbacks;
    bool _outOfNames = false;

    float _loginCredit = 0.0f;
    // Bots this manager logged in; only these are logged out again when there are too many.
    std::unordered_set<ObjectGuid> _loggedIn;

    // World tick times over the last minute, for the load test.
    std::deque<uint32> _tickTimes;
    uint64 _tickTimeTotal = 0;
};

#define sBotPopulation BotPopulation::Instance()

#endif
