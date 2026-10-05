/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotMgr.h"

#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "World.h"
#include "Timer.h"
#include "WorldSession.h"

#include <chrono>
#include <thread>

namespace
{
    // A login that hasn't finished by then is given up.
    constexpr uint32 LOGIN_TIMEOUT_MS = 30 * IN_MILLISECONDS;

    // The packets a bot reacts to. Everything else it is sent is dropped without copying, which
    // matters because a character receives every update about the world around it.
    bool IsHandled(uint16 opcode)
    {
        switch (opcode)
        {
            case SMSG_CHAR_ENUM:
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
        // A far teleport waits for the client to load the new map.
        case SMSG_NEW_WORLD:
            if (Player* player = _session->GetPlayer(); player && player->IsBeingTeleportedFar())
                _session->HandleMoveWorldportAck();
            break;
        // A near teleport waits for the client to confirm it.
        case MSG_MOVE_TELEPORT_ACK:
        {
            ObjectGuid guid;
            uint32 ackIndex = 0;
            packet.rpos(0);
            packet >> guid.ReadAsPacked();
            packet >> ackIndex;

            WorldPacket* ack = new WorldPacket(MSG_MOVE_TELEPORT_ACK, 8 + 4 + 4);
            *ack << guid.WriteAsPacked();
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
            if (!_session->GetPlayer())
                return false;
            break;
    }

    return !_removalRequested;
}

void Bot::LogOut()
{
    if (!_session)
        return;

    if (_session->GetPlayer())
        _session->LogoutPlayer(true);

    // Deleting the session sends nothing more to the bot, so the handler can go with it.
    _session.reset();
    TC_LOG_INFO("module", "mod-bots: {} logged out.", _name);
}

BotMgr& BotMgr::Instance()
{
    static BotMgr instance;
    return instance;
}

std::string BotMgr::Add(ObjectGuid guid, uint32 ownerAccountId)
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
