/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotMgr.h"

#include "AccountMgr.h"
#include "BotCompanion.h"
#include "BotQuesting.h"
#include "Group.h"
#include "RotationBotMgr.h" // mod-rotation-bot: companions fight with the player's rotation
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "World.h"
#include "Timer.h"
#include "Util.h"
#include "WorldSession.h"

#include <chrono>
#include <thread>

namespace
{
    // How often a companion decides what to do.
    constexpr uint32 COMPANION_UPDATE_MS = 250;
    constexpr uint32 SUPPLIES_UPDATE_MS = 30 * IN_MILLISECONDS;

    // A login that hasn't finished by then is given up.
    constexpr uint32 LOGIN_TIMEOUT_MS = 30 * IN_MILLISECONDS;

    // The packets a bot reacts to. Everything else it is sent is dropped without copying, which
    // matters because a character receives every update about the world around it.
    bool IsHandled(uint16 opcode)
    {
        switch (opcode)
        {
            case SMSG_CHAR_ENUM:
            case SMSG_CLIENT_CONTROL_UPDATE:
            case SMSG_LOOT_RESPONSE:
            case SMSG_NEW_WORLD:
            case MSG_MOVE_TELEPORT_ACK:
                return true;
            default:
                return false;
        }
    }
}

Bot::Bot(ObjectGuid guid, uint32 accountId, std::string name, uint32 ownerAccountId) : _guid(guid),
    _accountId(accountId), _name(std::move(name)), _ownerAccountId(ownerAccountId)
{
}

Bot::~Bot() = default;

bool Bot::Start(std::string accountName)
{
    // Bots never get more than player rights, whatever the account's security level.
    _session = std::make_unique<WorldSession>(_accountId, std::move(accountName), nullptr, SEC_PLAYER,
        EXPANSION_WRATH_OF_THE_LICH_KING, time_t(0), Minutes(0), LOCALE_enUS, 0, false);
    _session->SetBotPacketHandler([this](WorldPacket const& packet) { OnServerPacket(packet); });
    _session->LoadPermissions();

    // Like a client: ask for the character list, then log in once it arrives (see HandlePacket).
    QueueClientPacket(new WorldPacket(CMSG_CHAR_ENUM, 0));
    return true;
}

void Bot::OnServerPacket(WorldPacket const& packet)
{
    if (!IsHandled(packet.GetOpcode()))
        return;

    std::lock_guard<std::mutex> lock(_inboxLock);
    _inbox.emplace_back(packet);
}

void Bot::QueueClientPacket(WorldPacket* packet)
{
    _session->QueuePacket(packet);
}

void Bot::SetActiveMover(ObjectGuid guid)
{
    WorldPacket* packet = new WorldPacket(CMSG_SET_ACTIVE_MOVER, 8);
    *packet << guid;
    QueueClientPacket(packet);
}

void Bot::HandlePacket(WorldPacket& packet)
{
    switch (packet.GetOpcode())
    {
        case SMSG_CHAR_ENUM:
        {
            if (_state != State::RequestingCharacters)
                break;

            WorldPacket* login = new WorldPacket(CMSG_PLAYER_LOGIN, 8);
            *login << _guid;
            QueueClientPacket(login);
            _state = State::LoggingIn;
            break;
        }
        // Given control of a unit (its character, after logging in or teleporting): like a client, say it
        // is the one being moved, or the server ignores the bot's movement packets (teleport replies too).
        case SMSG_CLIENT_CONTROL_UPDATE:
        {
            ObjectGuid guid;
            uint8 allowMove = 0;
            packet.rpos(0);
            packet >> guid.ReadAsPacked();
            packet >> allowMove;
            if (!allowMove)
                break;

            SetActiveMover(guid);
            break;
        }
        case SMSG_LOOT_RESPONSE:
            if (_quester && _session->GetPlayer())
                _quester->OnLootResponse(_session->GetPlayer(), packet);
            break;
        // A far teleport waits for the client to load the new map; a client then says again which unit it moves.
        case SMSG_NEW_WORLD:
            if (Player* player = _session->GetPlayer(); player && player->IsBeingTeleportedFar())
            {
                _session->HandleMoveWorldportAck();
                SetActiveMover(player->GetGUID());
            }
            break;
        // A near teleport waits for the client to confirm it.
        case MSG_MOVE_TELEPORT_ACK:
        {
            Player* player = _session->GetPlayer();
            if (!player)
                break;

            // The packet's guid comes from the character's movement info, which stays empty until the
            // client has sent a movement packet; a client knows it is the one teleported, and so does the bot.
            ObjectGuid packetGuid;
            uint32 ackIndex = 0;
            packet.rpos(0);
            packet >> packetGuid.ReadAsPacked();
            packet >> ackIndex;

            WorldPacket* ack = new WorldPacket(MSG_MOVE_TELEPORT_ACK, 8 + 4 + 4);
            *ack << player->GetGUID().WriteAsPacked();
            *ack << uint32(ackIndex);
            *ack << uint32(GameTime::GetGameTimeMS());
            QueueClientPacket(ack);
            break;
        }
        default:
            break;
    }
}

bool Bot::Update(uint32 diff)
{
    // The session processes the packets the bot queued that need the world thread (logging in,
    // finishing far teleports) and its database callbacks. Packets for a character in the world are
    // processed by its map's thread, which updates the session too.
    WorldSessionFilter filter(_session.get());
    _session->Update(diff, filter);

    std::vector<WorldPacket> inbox;
    {
        std::lock_guard<std::mutex> lock(_inboxLock);
        inbox.swap(_inbox);
    }

    for (WorldPacket& packet : inbox)
        HandlePacket(packet);

    if (_session->IsForcedExit())
        return false;

    switch (_state)
    {
        case State::RequestingCharacters:
        case State::LoggingIn:
            if (Player* player = _session->GetPlayer(); player && player->IsInWorld())
            {
                // A client says which unit it moves once it is in the world; the server ignores the
                // movement packets of a client that hasn't, such as the reply to a teleport.
                SetActiveMover(player->GetGUID());
                BotCompanion::GiveSupplies(player, _givenBags);
                // The server's own bots fight with mod-rotation-bot too; companions turn it on themselves.
                if (_ownerGuid.IsEmpty())
                    sRotationBotMgr.Enable(player);
                _state = State::InWorld;
                TC_LOG_INFO("module", "mod-bots: {} is in the world.", _name);
                break;
            }

            _loginTimeMs += diff;
            if (_loginTimeMs >= LOGIN_TIMEOUT_MS)
            {
                TC_LOG_ERROR("module", "mod-bots: {} didn't finish logging in within {} seconds; giving up.", _name,
                    LOGIN_TIMEOUT_MS / IN_MILLISECONDS);
                return false;
            }
            break;
        case State::InWorld:
        {
            Player* player = _session->GetPlayer();
            if (!player)
                return false;

            if (!player->IsInWorld())
                break;

            // The server's own bots quest on their own.
            if (_ownerGuid.IsEmpty())
            {
                if (!sBotMgr.IsQuestingEnabled())
                    break;

                if (!_quester)
                    _quester = std::make_unique<BotQuester>();

                _suppliesTimerMs += diff;
                if (_suppliesTimerMs >= SUPPLIES_UPDATE_MS && !player->IsInCombat())
                {
                    _suppliesTimerMs = 0;
                    BotCompanion::GiveSupplies(player, _givenBags);
                }

                _questerTimerMs = _questerTimerMs > diff ? _questerTimerMs - diff : 0;
                if (!_questerTimerMs)
                    _questerTimerMs = _quester->Think(player);
                break;
            }

            // Food and water are topped up now and then, out of combat.
            _suppliesTimerMs += diff;
            if (_suppliesTimerMs >= SUPPLIES_UPDATE_MS && !player->IsInCombat())
            {
                _suppliesTimerMs = 0;
                BotCompanion::GiveSupplies(player, _givenBags);
            }

            _companionTimerMs += diff;
            if (_companionTimerMs < COMPANION_UPDATE_MS)
                break;
            _companionTimerMs = 0;

            Player* owner = ObjectAccessor::FindPlayer(_ownerGuid);
            if (!owner || !owner->IsInWorld())
                break;

            if (!_companionStarted)
            {
                BotCompanion::JoinGroup(player, owner);
                sRotationBotMgr.Enable(player);
                _companionStarted = true;
            }

            BotCompanion::Update(player, owner, _staying, _pullTarget);
            break;
        }
    }

    return !_removalRequested;
}

void Bot::LogOut()
{
    if (!_session)
        return;

    if (Player* player = _session->GetPlayer())
    {
        // A companion (a player's alt) leaves its owner's group and gives back the bags, food and water it
        // was lent; the server's own bots keep theirs and never join a group.
        if (!_ownerGuid.IsEmpty())
        {
            BotCompanion::RemoveSupplies(player, _givenBags);
            if (Group* group = player->GetGroup())
                group->RemoveMember(player->GetGUID());
        }

        _session->LogoutPlayer(true);
    }

    // Deleting the session sends nothing more to the bot, so the handler can go with it.
    _session.reset();
    TC_LOG_INFO("module", "mod-bots: {} logged out.", _name);
}

BotMgr& BotMgr::Instance()
{
    static BotMgr instance;
    return instance;
}

std::string BotMgr::Add(ObjectGuid guid, uint32 ownerAccountId, ObjectGuid ownerGuid)
{
    CharacterCacheEntry const* character = sCharacterCache->GetCharacterCacheByGuid(guid);
    if (!character)
        return "There is no such character.";

    if (ownerAccountId && character->AccountId != ownerAccountId)
        return "You can only log in characters from your own account as bots.";

    if (_bots.count(guid))
        return character->Name + " is already a bot.";

    if (ObjectAccessor::FindConnectedPlayer(guid))
        return character->Name + " is online.";

    std::string accountName;
    if (!AccountMgr::GetName(character->AccountId, accountName))
        return "The character's account doesn't exist.";

    auto bot = std::make_unique<Bot>(guid, character->AccountId, character->Name, ownerAccountId);
    if (!bot->Start(std::move(accountName)))
        return "Couldn't start a session for " + character->Name + ".";

    bot->SetOwner(ownerGuid);
    TC_LOG_INFO("module", "mod-bots: logging in {}.", character->Name);
    _bots.emplace(guid, std::move(bot));
    return {};
}

bool BotMgr::Remove(ObjectGuid guid, uint32 ownerAccountId)
{
    Bot* bot = Find(guid);
    if (!bot || (ownerAccountId && bot->GetOwnerAccountId() != ownerAccountId))
        return false;

    bot->RequestRemoval();
    return true;
}

void BotMgr::RemoveAll(uint32 ownerAccountId)
{
    for (auto& [guid, bot] : _bots)
        if (!ownerAccountId || bot->GetOwnerAccountId() == ownerAccountId)
            bot->RequestRemoval();
}

std::vector<Bot*> BotMgr::FindBots(uint32 ownerAccountId, std::string const& nameOrAll) const
{
    std::vector<Bot*> bots;
    for (auto const& [guid, bot] : _bots)
        if ((!ownerAccountId || bot->GetOwnerAccountId() == ownerAccountId) &&
            (nameOrAll == "all" || StringEqualI(bot->GetName(), nameOrAll)))
            bots.push_back(bot.get());

    return bots;
}

Bot* BotMgr::Find(ObjectGuid guid) const
{
    auto itr = _bots.find(guid);
    return itr != _bots.end() ? itr->second.get() : nullptr;
}

std::vector<Bot const*> BotMgr::GetBots(uint32 ownerAccountId) const
{
    std::vector<Bot const*> bots;
    for (auto const& [guid, bot] : _bots)
        if (!ownerAccountId || bot->GetOwnerAccountId() == ownerAccountId)
            bots.push_back(bot.get());

    return bots;
}

void BotMgr::Update(uint32 diff)
{
    for (auto itr = _bots.begin(); itr != _bots.end();)
    {
        if (itr->second->Update(diff))
        {
            ++itr;
            continue;
        }

        itr->second->LogOut();
        itr = _bots.erase(itr);
    }
}

void BotMgr::LogOutAll()
{
    if (_bots.empty())
        return;

    std::size_t count = _bots.size();
    for (auto& [guid, bot] : _bots)
        bot->LogOut();

    _bots.clear();

    // The saves are queued, and closing the database at shutdown drops whatever is still queued:
    // with hundreds of bots that would lose the last ones' progress. Wait until they are written.
    uint32 start = getMSTime();
    while (CharacterDatabase.QueueSize() > 0 && getMSTimeDiff(start, getMSTime()) < 120 * IN_MILLISECONDS)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TC_LOG_INFO("module", "mod-bots: logged out {} bots; their saves took {} ms to write{}.", count,
        getMSTimeDiff(start, getMSTime()), CharacterDatabase.QueueSize() > 0 ? " (gave up waiting)" : "");
}
