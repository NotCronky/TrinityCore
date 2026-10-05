/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotMgr.h"
#include "BotCompanion.h"
#include "BotPopulation.h"
#include "BotQuesting.h"
#include "Player.h"

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
            { "population", HandlePopulation, rbac::RBAC_ROLE_ADMINISTRATOR, Console::Yes },
            { "info",      HandleInfo,      rbac::RBAC_ROLE_ADMINISTRATOR, Console::Yes },
            { "gear",      HandleGear,      rbac::RBAC_ROLE_PLAYER, Console::Yes },
            { "stay",      HandleStay,      rbac::RBAC_ROLE_PLAYER, Console::Yes },
            { "follow",    HandleFollow,    rbac::RBAC_ROLE_PLAYER, Console::Yes },
            { "pull",      HandlePull,      rbac::RBAC_ROLE_PLAYER, Console::No },
            { "eat",       HandleEat,       rbac::RBAC_ROLE_PLAYER, Console::Yes },
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

        // Added in game, the bot is a companion of the character who added it.
        ObjectGuid owner = handler->GetSession() && handler->GetSession()->GetPlayer() ?
            handler->GetSession()->GetPlayer()->GetGUID() : ObjectGuid::Empty;
        std::string error = sBotMgr.Add(character.GetGUID(), GetOwnerAccountId(handler), owner);
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

    // .bot gear <name|all> [spec]: class spells for the level, talents in the spec, and the best gear for both.
    static bool HandleGear(ChatHandler* handler, std::string who, Optional<std::string> spec)
    {
        std::vector<Bot*> bots = sBotMgr.FindBots(GetOwnerAccountId(handler), who);
        if (bots.empty())
            handler->SendSysMessage(Trinity::StringFormat("No bot of yours is called {}.", who));

        for (Bot* bot : bots)
        {
            Player* player = bot->GetSession() ? bot->GetSession()->GetPlayer() : nullptr;
            if (!player || !player->IsInWorld())
            {
                handler->SendSysMessage(Trinity::StringFormat("{} isn't in the world yet.", bot->GetName()));
                continue;
            }

            Optional<uint8> tree;
            if (spec)
            {
                tree = BotCompanion::ParseSpec(player->GetClass(), *spec);
                if (!tree)
                {
                    handler->SendSysMessage(Trinity::StringFormat("{}: \"{}\" isn't a spec of the class; use its tree name or 1-3.",
                        bot->GetName(), *spec));
                    continue;
                }
            }

            handler->SendSysMessage(BotCompanion::Equip(player, tree));
        }

        return true;
    }

    // .bot pull [name]: your tank bots (or the named bot) attack your target; the others join once it's a fight.
    static bool HandlePull(ChatHandler* handler, Optional<std::string> who)
    {
        Unit* target = handler->getSelectedUnit();
        Player* player = handler->GetSession()->GetPlayer();
        if (!target || target == player || !player->IsValidAttackTarget(target))
        {
            handler->SendSysMessage("Target an enemy first.");
            return true;
        }

        std::vector<std::string> sent;
        for (Bot* bot : sBotMgr.FindBots(GetOwnerAccountId(handler), who.value_or("all")))
        {
            Player* botPlayer = bot->GetSession() ? bot->GetSession()->GetPlayer() : nullptr;
            if (!botPlayer || (!who && BotCompanion::GetRole(botPlayer) != BotCompanion::Role::Tank))
                continue;

            bot->SetPullTarget(target->GetGUID());
            sent.push_back(bot->GetName());
        }

        if (sent.empty())
            handler->SendSysMessage(who ? Trinity::StringFormat("No bot of yours is called {}.", *who) :
                std::string("None of your bots is a tank; use .bot pull <name> to send one."));
        else
            for (std::string const& name : sent)
                handler->SendSysMessage(Trinity::StringFormat("{} is pulling {}.", name, target->GetName()));

        return true;
    }

    // .bot eat [name|all]: bots out of combat sit down to eat and drink until full.
    static bool HandleEat(ChatHandler* handler, Optional<std::string> who)
    {
        uint32 eating = 0;
        for (Bot* bot : sBotMgr.FindBots(GetOwnerAccountId(handler), who.value_or("all")))
            if (Player* player = bot->GetSession() ? bot->GetSession()->GetPlayer() : nullptr; player && player->IsInWorld())
                eating += BotCompanion::EatAndDrink(player);

        handler->SendSysMessage(Trinity::StringFormat("{} bot{} eating and drinking.", eating, eating == 1 ? " is" : "s are"));
        return true;
    }

    static bool HandleStay(ChatHandler* handler, std::string who)
    {
        for (Bot* bot : sBotMgr.FindBots(GetOwnerAccountId(handler), who))
            bot->SetStaying(true);
        handler->SendSysMessage("Staying.");
        return true;
    }

    static bool HandleFollow(ChatHandler* handler, std::string who)
    {
        for (Bot* bot : sBotMgr.FindBots(GetOwnerAccountId(handler), who))
            bot->SetStaying(false);
        handler->SendSysMessage("Following.");
        return true;
    }

    // .bot info <name>: what one of the server's questing bots is doing.
    static bool HandleInfo(ChatHandler* handler, PlayerIdentifier character)
    {
        Bot* bot = sBotMgr.Find(character.GetGUID());
        Player* player = bot && bot->GetSession() ? bot->GetSession()->GetPlayer() : nullptr;
        if (!player || !bot->GetQuester())
        {
            handler->SendSysMessage(Trinity::StringFormat("{} isn't one of the server's questing bots in the world.", character.GetName()));
            return true;
        }

        for (std::string const& line : bot->GetQuester()->Describe(player))
            handler->SendSysMessage(line);
        return true;
    }

    static bool HandlePopulation(ChatHandler* handler, Optional<EXACT_SEQUENCE("reset")> reset, Optional<EXACT_SEQUENCE("confirm")> confirm)
    {
        if (reset)
        {
            if (!confirm)
            {
                handler->SendSysMessage("This deletes every one of the server's bot characters (not players' characters or "
                    "alts) and makes the population again from level 1 with new names. Type .bot population reset confirm.");
                return true;
            }

            handler->SendSysMessage(sBotPopulation.Reset());
            return true;
        }

        for (std::string const& line : sBotPopulation.Describe())
            handler->SendSysMessage(line);

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
        sBotPopulation.LoadConfig();
        sBotMgr.SetQuestingEnabled(sConfigMgr->GetBoolDefault("Bots.Questing.Enable", true));
        if (!BotsEnabled)
            sBotMgr.RemoveAll(0);
    }

    // Names, census and earlier bots; the character cache and quests are loaded by now.
    void OnStartup() override
    {
        sBotQuestData.Load();
        sBotPopulation.Load();
    }

    // After the maps have updated, so bots can log in and out safely.
    void OnUpdate(uint32 diff) override
    {
        if (BotsEnabled)
            sBotPopulation.Update(diff);

        sBotMgr.Update(diff);
    }

    // Before the maps unload, so every bot is saved.
    void OnShutdown() override
    {
        sBotPopulation.Shutdown();
        sBotMgr.LogOutAll();
    }
};

void AddSC_mod_bots()
{
    new BotCommandScript();
    new BotPlayerScript();
    new BotWorldScript();
}
