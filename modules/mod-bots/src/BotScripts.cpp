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

    // The account using the command, or 0 for the server console.
    uint32 GetOwnerAccountId(ChatHandler* handler)
    {
        return handler->GetSession() ? handler->GetSession()->GetAccountId() : 0;
    }
}

class BotCommandScript : public CommandScript
{
public:
    BotCommandScript() : CommandScript("BotCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable botTable =
        {
            // In game, players manage bots of their own account's characters; the console manages every bot.
            { "add",       HandleAdd,       rbac::RBAC_ROLE_PLAYER, Console::Yes },
            { "remove",    HandleRemove,    rbac::RBAC_ROLE_PLAYER, Console::Yes },
            { "removeall", HandleRemoveAll, rbac::RBAC_ROLE_PLAYER, Console::Yes },
            { "list",      HandleList,      rbac::RBAC_ROLE_PLAYER, Console::Yes },
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

        std::string error = sBotMgr.Add(character.GetGUID(), GetOwnerAccountId(handler));
        handler->SendSysMessage(error.empty() ? Trinity::StringFormat("Logging in {} as a bot.", character.GetName()) : error);
        return true;
    }

    static bool HandleRemove(ChatHandler* handler, PlayerIdentifier character)
    {
        handler->SendSysMessage(sBotMgr.Remove(character.GetGUID(), GetOwnerAccountId(handler)) ?
            Trinity::StringFormat("{} is logging out.", character.GetName()) :
            Trinity::StringFormat("{} isn't one of your bots.", character.GetName()));
        return true;
    }

    static bool HandleRemoveAll(ChatHandler* handler)
    {
        sBotMgr.RemoveAll(GetOwnerAccountId(handler));
        handler->SendSysMessage(handler->GetSession() ? "Your bots are logging out." : "Every bot is logging out.");
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        std::vector<Bot const*> bots = sBotMgr.GetBots(GetOwnerAccountId(handler));
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

    // Runs on the world thread, like every BotMgr change. A player's bots log out with them; a bot
    // logging out (its session is a bot session) must not take its owner's other bots with it.
    void OnLogout(Player* player) override
    {
        WorldSession* session = player->GetSession();
        if (!session->IsBot())
            sBotMgr.RemoveAll(session->GetAccountId());
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
            sBotMgr.RemoveAll(0);
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
