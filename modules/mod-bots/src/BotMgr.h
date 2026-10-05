/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#ifndef MOD_BOTS_BOT_MGR_H
#define MOD_BOTS_BOT_MGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "WorldPacket.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class WorldSession;

// One character logged in as a bot. Its session has no socket: packets the server sends it arrive in
// OnServerPacket, and it acts by queueing client packets on its session, which the server processes
// like a real client's, on the same threads.
class Bot
{
public:
    enum class State : uint8
    {
        RequestingCharacters, // CMSG_CHAR_ENUM sent; logging in needs the account's character list first
        LoggingIn,            // CMSG_PLAYER_LOGIN sent, waiting for the character to be in the world
        InWorld,
    };

    // ownerAccountId is the account that added the bot, or 0 for the server console.
    Bot(ObjectGuid guid, uint32 accountId, std::string name, uint32 ownerAccountId);
    ~Bot();

    ObjectGuid GetGuid() const { return _guid; }
    std::string const& GetName() const { return _name; }
    uint32 GetOwnerAccountId() const { return _ownerAccountId; }
    // The character that added the bot as a companion; empty for the server's own bots.
    void SetOwner(ObjectGuid owner) { _ownerGuid = owner; }
    ObjectGuid GetOwner() const { return _ownerGuid; }
    void SetStaying(bool staying) { _staying = staying; }
    // Sends a companion to attack the unit (.bot pull).
    void SetPullTarget(ObjectGuid target) { _pullTarget = target; }
    State GetState() const { return _state; }
    WorldSession* GetSession() const { return _session.get(); }

    // Starts logging in. Returns false when the session can't be created.
    bool Start(std::string accountName);

    // Called with every packet the server sends the bot, from whichever thread sends it.
    void OnServerPacket(WorldPacket const& packet);

    // World thread, while maps are idle. Returns false once the bot has to be removed.
    bool Update(uint32 diff);

    // Saves and removes the character from the world. World thread, while maps are idle.
    void LogOut();

    bool IsRemovalRequested() const { return _removalRequested; }
    void RequestRemoval() { _removalRequested = true; }

private:
    void HandlePacket(WorldPacket& packet);
    void QueueClientPacket(WorldPacket* packet);
    // CMSG_SET_ACTIVE_MOVER: the unit the bot moves, needed before the server accepts its movement packets.
    void SetActiveMover(ObjectGuid guid);

    ObjectGuid _guid;
    uint32 _accountId;
    std::string _name;
    uint32 _ownerAccountId;
    ObjectGuid _ownerGuid;
    bool _staying = false;
    ObjectGuid _pullTarget;
    bool _companionStarted = false;
    uint32 _companionTimerMs = 0;
    uint32 _suppliesTimerMs = 0;
    std::vector<uint8> _givenBags; // Bag slots filled with a lent bag, taken back from alts at logout
    State _state = State::RequestingCharacters;
    std::unique_ptr<WorldSession> _session;
    uint32 _loginTimeMs = 0;
    bool _removalRequested = false;

    // Packets the bot reacts to, copied from OnServerPacket and handled in Update.
    std::mutex _inboxLock;
    std::vector<WorldPacket> _inbox;
};

class BotMgr
{
public:
    static BotMgr& Instance();

    // ownerAccountId: the account asking, which may only use its own characters; 0 is the server
    // console, which may use any character and any bot.

    // Logs a character in as a bot. Returns an error message, or an empty string on success.
    std::string Add(ObjectGuid guid, uint32 ownerAccountId, ObjectGuid ownerGuid = ObjectGuid::Empty);
    // Asks a bot to log out; it is removed on the next world update. False when the account may not.
    bool Remove(ObjectGuid guid, uint32 ownerAccountId);
    // Logs out the bots the account added, or every bot for the console.
    void RemoveAll(uint32 ownerAccountId);

    Bot* Find(ObjectGuid guid) const;
    // The bots the account added, or every bot for the console.
    std::vector<Bot const*> GetBots(uint32 ownerAccountId) const;
    // The account's bots (every bot for the console) whose name matches, or all of them for "all".
    std::vector<Bot*> FindBots(uint32 ownerAccountId, std::string const& nameOrAll) const;

    // World thread, while maps are idle.
    void Update(uint32 diff);
    // At shutdown, while maps are still loaded: logs every bot out so they are saved.
    void LogOutAll();

private:
    BotMgr() = default;

    // Only touched by the world thread, outside map updates.
    std::unordered_map<ObjectGuid, std::unique_ptr<Bot>> _bots;
};

#define sBotMgr BotMgr::Instance()

#endif
