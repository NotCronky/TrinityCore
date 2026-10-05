/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotMgr.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Log.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

namespace
{
    bool BotsEnabled = true;
}

class BotCommandScript : public CommandScript
{
public:
    BotCommandScript() : CommandScript("BotCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable botTable =
        {
            { "add",       HandleAdd,       rbac::RBAC_ROLE_GAMEMASTER, Console::Yes },
            { "remove",    HandleRemove,    rbac::RBAC_ROLE_GAMEMASTER, Console::Yes },
            { "removeall", HandleRemoveAll, rbac::RBAC_ROLE_GAMEMASTER, Console::Yes },
            { "list",      HandleList,      rbac::RBAC_ROLE_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "bot", botTable },
        };

        return commandTable;
    }

    static bool HandleAdd(ChatHandler* handler, PlayerIdentifier character)
    {
        if (!BotsEnabled)
        {
            handler->SendSysMessage("Bots are disabled on this server.");
            return true;
        }

        std::string error = sBotMgr.Add(character.GetGUID());
        handler->SendSysMessage(error.empty() ? Trinity::StringFormat("Logging in {} as a bot.", character.GetName()) : error);
        return true;
    }

    static bool HandleRemove(ChatHandler* handler, PlayerIdentifier character)
    {
        handler->SendSysMessage(sBotMgr.Remove(character.GetGUID()) ?
            Trinity::StringFormat("{} is logging out.", character.GetName()) :
            Trinity::StringFormat("{} isn't a bot.", character.GetName()));
        return true;
    }

    static bool HandleRemoveAll(ChatHandler* handler)
    {
        sBotMgr.RemoveAll();
        handler->SendSysMessage("Every bot is logging out.");
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        std::vector<Bot const*> bots = sBotMgr.GetBots();
        handler->SendSysMessage(Trinity::StringFormat("{} bot{}:", bots.size(), bots.size() == 1 ? "" : "s"));
        for (Bot const* bot : bots)
        {
            char const* state = "in the world";
            if (bot->GetState() != Bot::State::InWorld)
                state = "logging in";
            if (bot->IsRemovalRequested())
                state = "logging out";

            handler->SendSysMessage(Trinity::StringFormat("  {} - {}", bot->GetName(), state));
        }

        return true;
    }
};

class BotPlayerScript : public PlayerScript
{
public:
    BotPlayerScript() : PlayerScript("BotPlayerScript") { }

    // Runs on the world thread (CMSG_PLAYER_LOGIN is thread-unsafe), like every BotMgr change.
    bool CanLogin(WorldSession* session, ObjectGuid guid) override
    {
        Bot* bot = sBotMgr.Find(guid);
        if (!bot || bot->GetSession() == session)
            return true;

        // A player wants this character back: the bot logs out, and the player can log in a moment later.
        TC_LOG_INFO("module", "mod-bots: account {} is logging in {}, which is a bot; the bot logs out.",
            session->GetAccountId(), bot->GetName());
        bot->RequestRemoval();
        return false;
    }
};

class BotWorldScript : public WorldScript
{
public:
    BotWorldScript() : WorldScript("BotWorldScript") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        BotsEnabled = sConfigMgr->GetBoolDefault("Bots.Enable", true);
        if (!BotsEnabled)
            sBotMgr.RemoveAll();
    }

    // After the maps have updated, so bots can log in and out safely.
    void OnUpdate(uint32 diff) override { sBotMgr.Update(diff); }

    // Before the maps unload, so every bot is saved.
    void OnShutdown() override { sBotMgr.LogOutAll(); }
};

void AddSC_mod_bots()
{
    new BotCommandScript();
    new BotPlayerScript();
    new BotWorldScript();
}
