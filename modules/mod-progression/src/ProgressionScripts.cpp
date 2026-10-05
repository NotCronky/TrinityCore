/*
 * mod-progression: each character plays in a patch of its own, from 1.1 to 3.3.5.
 */

#include "ProgressionMgr.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "GameTime.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <unordered_map>

using namespace Trinity::ChatCommands;

namespace
{
    // How long a ".patch next" preview stays valid for its "confirm".
    constexpr time_t CONFIRM_SECONDS = 120;

    struct PendingAdvance
    {
        uint8 PatchId = 0;
        time_t ExpiresAt = 0;
    };

    // Commands run on the world thread, so this needs no lock.
    std::unordered_map<ObjectGuid, PendingAdvance> PendingAdvances;

    void Send(ChatHandler* handler, std::string const& text)
    {
        handler->SendSysMessage(text);
    }

    std::string Describe(ProgressionPatch const& patch)
    {
        return Trinity::StringFormat("{} (level cap {})", patch.Title(), patch.LevelCap);
    }
}

class ProgressionCommandScript : public CommandScript
{
public:
    ProgressionCommandScript() : CommandScript("ProgressionCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable patchTable =
        {
            { "info",      HandleInfo,      rbac::RBAC_ROLE_PLAYER,        Console::No  },
            { "list",      HandleList,      rbac::RBAC_ROLE_PLAYER,        Console::Yes },
            { "next",      HandleNext,      rbac::RBAC_ROLE_PLAYER,        Console::No  },
            { "advance",   HandleAdvance,   rbac::RBAC_ROLE_PLAYER,        Console::No  },
            { "set",       HandleSet,       rbac::RBAC_ROLE_GAMEMASTER,    Console::No  },
            { "servercap", HandleServerCap, rbac::RBAC_ROLE_ADMINISTRATOR, Console::Yes },
            { "reload",    HandleReload,    rbac::RBAC_ROLE_ADMINISTRATOR, Console::Yes },
            { "",          HandleInfoSelf,  rbac::RBAC_ROLE_PLAYER,        Console::No  },
        };

        static ChatCommandTable commandTable =
        {
            { "patch", patchTable },
        };

        return commandTable;
    }

    static bool CheckEnabled(ChatHandler* handler)
    {
        if (sProgressionMgr.IsEnabled())
            return true;

        Send(handler, "Patch progression is disabled on this server.");
        return false;
    }

    static bool HandleInfoSelf(ChatHandler* handler)
    {
        return HandleInfo(handler, {});
    }

    static bool HandleInfo(ChatHandler* handler, Optional<PlayerIdentifier> player)
    {
        if (!CheckEnabled(handler))
            return true;

        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!player || !player->IsConnected())
        {
            Send(handler, "That character isn't online.");
            return true;
        }

        Player* target = player->GetConnectedPlayer();
        ProgressionPatch const* patch = sProgressionMgr.GetCharacterPatch(target);
        if (!patch)
        {
            Send(handler, Trinity::StringFormat("{} has no patch yet.", target->GetName()));
            return true;
        }

        Send(handler, Trinity::StringFormat("{} is in patch {}.", target->GetName(), Describe(*patch)));

        ProgressionPatch const* cap = sProgressionMgr.GetServerCap();
        if (patch->Id > cap->Id)
            Send(handler, Trinity::StringFormat("The server is capped at {}, so {} plays as if in that patch.",
                cap->Version, target->GetName()));

        if (ProgressionPatch const* next = sProgressionMgr.GetNextPatch(*patch))
            Send(handler, Trinity::StringFormat("Next: {}. Type .patch next to see what it brings.", next->Title()));
        else
            Send(handler, "This is the last patch.");

        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        if (!CheckEnabled(handler))
            return true;

        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        ProgressionPatch const* current = player ? sProgressionMgr.GetCharacterPatch(player) : nullptr;
        ProgressionPatch const* cap = sProgressionMgr.GetServerCap();

        for (ProgressionPatch const& patch : sProgressionMgr.GetPatches())
        {
            std::string line = Trinity::StringFormat("{} {}", current && current->Id == patch.Id ? ">" : " ",
                Describe(patch));
            if (patch.Id > cap->Id)
                line += " - not available on this server yet";

            Send(handler, line);
        }

        return true;
    }

    static bool HandleNext(ChatHandler* handler, Optional<EXACT_SEQUENCE("confirm")> confirm)
    {
        if (!CheckEnabled(handler))
            return true;

        Player* player = handler->GetSession()->GetPlayer();
        ProgressionPatch const* current = sProgressionMgr.GetCharacterPatch(player);
        ProgressionPatch const* next = current ? sProgressionMgr.GetNextPatch(*current) : nullptr;
        if (!next)
        {
            Send(handler, "You are already in the last patch.");
            return true;
        }

        return Advance(handler, player, *current, *next, confirm.has_value(), ".patch next confirm");
    }

    static bool HandleAdvance(ChatHandler* handler, std::string version, Optional<EXACT_SEQUENCE("confirm")> confirm)
    {
        if (!CheckEnabled(handler))
            return true;

        ProgressionPatch const* target = sProgressionMgr.FindPatch(version);
        if (!target)
        {
            Send(handler, Trinity::StringFormat("There is no patch \"{}\". Type .patch list to see them.", version));
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        ProgressionPatch const* current = sProgressionMgr.GetCharacterPatch(player);
        if (!current)
            return true;

        if (target->Id <= current->Id)
        {
            Send(handler, Trinity::StringFormat("You are in {}. You can only move forward to a later patch.",
                current->Version));
            return true;
        }

        return Advance(handler, player, *current, *target, confirm.has_value(),
            Trinity::StringFormat(".patch advance {} confirm", target->Version));
    }

    // Shows what moving from current to target brings and removes; with confirm, after such a preview, moves.
    static bool Advance(ChatHandler* handler, Player* player, ProgressionPatch const& current,
        ProgressionPatch const& target, bool confirm, std::string const& confirmCommand)
    {
        ProgressionPatch const* cap = sProgressionMgr.GetServerCap();
        if (target.Id > cap->Id)
        {
            Send(handler, Trinity::StringFormat("{} isn't available on this server yet; it is capped at {}.",
                target.Version, cap->Version));
            return true;
        }

        if (sProgressionMgr.RequireLevelCapToAdvance() && player->GetLevel() < current.LevelCap)
        {
            Send(handler, Trinity::StringFormat("Reach level {} before moving on from {}.", current.LevelCap,
                current.Version));
            return true;
        }

        time_t now = GameTime::GetGameTime();
        auto pending = PendingAdvances.find(player->GetGUID());
        bool previewed = pending != PendingAdvances.end() && pending->second.PatchId == target.Id &&
            pending->second.ExpiresAt >= now;

        if (!confirm || !previewed)
        {
            Send(handler, Trinity::StringFormat("Moving from {} to {}:", current.Title(), Describe(target)));
            for (ProgressionPatch const& patch : sProgressionMgr.GetPatches())
            {
                if (patch.Id <= current.Id || patch.Id > target.Id)
                    continue;

                if (!patch.Unlocks.empty())
                    Send(handler, Trinity::StringFormat("  {} adds: {}", patch.Version, patch.Unlocks));
                if (!patch.Removes.empty())
                    Send(handler, Trinity::StringFormat("  {} ENDS: {}", patch.Version, patch.Removes));
            }

            Send(handler, "You can't go back to an earlier patch afterwards.");
            Send(handler, Trinity::StringFormat("Type {} within {} minutes to move on.", confirmCommand,
                CONFIRM_SECONDS / 60));
            PendingAdvances[player->GetGUID()] = { target.Id, now + CONFIRM_SECONDS };
            return true;
        }

        PendingAdvances.erase(player->GetGUID());
        sProgressionMgr.SetCharacterPatch(player, target);
        player->SaveToDB();
        Send(handler, Trinity::StringFormat("You are now in patch {}.", Describe(target)));
        return true;
    }

    static bool HandleSet(ChatHandler* handler, std::string version, Optional<PlayerIdentifier> player)
    {
        if (!CheckEnabled(handler))
            return true;

        ProgressionPatch const* patch = sProgressionMgr.FindPatch(version);
        if (!patch)
        {
            Send(handler, Trinity::StringFormat("There is no patch \"{}\". Type .patch list to see them.", version));
            return true;
        }

        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);
        if (!player || !player->IsConnected())
        {
            Send(handler, "That character isn't online.");
            return true;
        }

        Player* target = player->GetConnectedPlayer();
        sProgressionMgr.SetCharacterPatch(target, *patch);
        target->SaveToDB();
        PendingAdvances.erase(target->GetGUID());

        Send(handler, Trinity::StringFormat("{} is now in patch {}.", target->GetName(), Describe(*patch)));
        if (target != handler->GetSession()->GetPlayer())
            ChatHandler(target->GetSession()).SendSysMessage(Trinity::StringFormat(
                "A game master moved you to patch {}.", Describe(*patch)));

        return true;
    }

    static bool HandleServerCap(ChatHandler* handler, Optional<std::string> version)
    {
        if (!CheckEnabled(handler))
            return true;

        if (version)
        {
            ProgressionPatch const* patch = sProgressionMgr.FindPatch(*version);
            if (!patch)
            {
                Send(handler, Trinity::StringFormat("There is no patch \"{}\". Type .patch list to see them.", *version));
                return true;
            }

            sProgressionMgr.SetServerCap(*patch);
            Send(handler, "Until the next restart or .reload config; set Progression.ServerCap to keep it.");
        }

        Send(handler, Trinity::StringFormat("Server cap: {}.", sProgressionMgr.GetServerCap()->Title()));
        return true;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        if (sProgressionMgr.LoadPatches())
            Send(handler, Trinity::StringFormat("Reloaded {} patches.", sProgressionMgr.GetPatches().size()));
        else
            Send(handler, "Reload failed; see the server log. Patch progression is disabled until it loads.");

        return true;
    }
};

class ProgressionPlayerScript : public PlayerScript
{
public:
    ProgressionPlayerScript() : PlayerScript("ProgressionPlayerScript") { }

    void OnLogin(Player* player, bool firstLogin) override
    {
        if (sProgressionMgr.IsEnabled())
            sProgressionMgr.OnLogin(player, firstLogin);
    }

    void OnLogout(Player* player) override
    {
        PendingAdvances.erase(player->GetGUID());
    }

    // No experience past the patch's level cap.
    void OnGiveXP(Player* player, uint32& amount, Unit* /*victim*/) override
    {
        if (!sProgressionMgr.IsEnabled())
            return;

        if (ProgressionPatch const* patch = sProgressionMgr.GetEffectivePatch(player))
            if (player->GetLevel() >= patch->LevelCap)
                amount = 0;
    }

    void OnLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!sProgressionMgr.IsEnabled())
            return;

        ProgressionPatch const* patch = sProgressionMgr.GetEffectivePatch(player);
        if (!patch || player->GetLevel() != patch->LevelCap || !sProgressionMgr.GetNextPatch(*patch))
            return;

        ChatHandler(player->GetSession()).SendSysMessage(Trinity::StringFormat(
            "You have reached level {}, the level cap of patch {}. Type .patch next when you are ready to move on.",
            patch->LevelCap, patch->Version));
    }
};

class ProgressionWorldScript : public WorldScript
{
public:
    ProgressionWorldScript() : WorldScript("ProgressionWorldScript") { }

    void OnConfigLoad(bool /*reload*/) override { sProgressionMgr.LoadConfig(); }

    // Quests are loaded by now, so the hidden patch quests can be checked.
    void OnStartup() override { sProgressionMgr.LoadPatches(); }
};

void AddSC_mod_progression()
{
    new ProgressionCommandScript();
    new ProgressionPlayerScript();
    new ProgressionWorldScript();
}
