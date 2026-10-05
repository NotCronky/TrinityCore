/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationBotMgr.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Player.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

class RotationBotCommandScript : public CommandScript
{
public:
    RotationBotCommandScript() : CommandScript("RotationBotCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable rotTable =
        {
            { "on",     HandleOn,     rbac::RBAC_ROLE_PLAYER, Console::No },
            { "off",    HandleOff,    rbac::RBAC_ROLE_PLAYER, Console::No },
            { "status", HandleStatus, rbac::RBAC_ROLE_PLAYER, Console::No },
            { "debug",  HandleDebug,  rbac::RBAC_ROLE_PLAYER, Console::No },
            { "state",  HandleState,  rbac::RBAC_ROLE_PLAYER, Console::No },
            { "next",   HandleNext,   rbac::RBAC_ROLE_PLAYER, Console::No },
            { "reload", HandleReload, rbac::RBAC_ROLE_PLAYER, Console::No },
            { "mode",   HandleMode,   rbac::RBAC_ROLE_PLAYER, Console::No },
            { "aoe",    HandleAoe,    rbac::RBAC_ROLE_PLAYER, Console::No },
            { "burst",  HandleBurst,  rbac::RBAC_ROLE_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "rot", rotTable },
        };

        return commandTable;
    }

    static bool HandleOn(ChatHandler* handler) { return Reply(handler, sRotationBotMgr.Enable(GetPlayer(handler))); }
    static bool HandleOff(ChatHandler* handler) { return Reply(handler, sRotationBotMgr.Disable(GetPlayer(handler))); }

    static bool HandleStatus(ChatHandler* handler)
    {
        return Reply(handler, sRotationBotMgr.Status(GetPlayer(handler)));
    }

    static bool HandleDebug(ChatHandler* handler)
    {
        return Reply(handler, sRotationBotMgr.ToggleDebug(GetPlayer(handler)));
    }

    static bool HandleNext(ChatHandler* handler)
    {
        return Reply(handler, sRotationBotMgr.Next(GetPlayer(handler)));
    }

    static bool HandleState(ChatHandler* handler)
    {
        return Reply(handler, sRotationBotMgr.DescribeState(GetPlayer(handler)));
    }

    static bool HandleReload(ChatHandler* handler)
    {
        return Reply(handler, sRotationBotMgr.Reload(GetPlayer(handler)));
    }

    static bool HandleMode(ChatHandler* handler, Optional<std::string> mode)
    {
        return Reply(handler, sRotationBotMgr.SetMode(GetPlayer(handler), mode));
    }

    static bool HandleAoe(ChatHandler* handler, Optional<std::string> mode)
    {
        return Reply(handler, sRotationBotMgr.SetAoe(GetPlayer(handler), mode));
    }

    static bool HandleBurst(ChatHandler* handler, Optional<bool> enable)
    {
        return Reply(handler, sRotationBotMgr.SetBurst(GetPlayer(handler), enable));
    }

private:
    static Player* GetPlayer(ChatHandler* handler) { return handler->GetSession()->GetPlayer(); }

    static bool Reply(ChatHandler* handler, std::vector<std::string> const& lines)
    {
        for (std::string const& line : lines)
            handler->SendSysMessage(line);

        return true;
    }

    static bool Reply(ChatHandler* handler, std::string const& message)
    {
        if (!message.empty())
            handler->SendSysMessage(message);

        return true;
    }
};

class RotationBotPlayerScript : public PlayerScript
{
public:
    RotationBotPlayerScript() : PlayerScript("RotationBotPlayerScript") {}

    void OnAfterUpdate(Player* player, uint32 diff) override { sRotationBotMgr.OnPlayerUpdate(player, diff); }
    void OnLogout(Player* player) override { sRotationBotMgr.OnPlayerLogout(player); }
};

class RotationBotWorldScript : public WorldScript
{
public:
    RotationBotWorldScript() : WorldScript("RotationBotWorldScript") {}

    void OnConfigLoad(bool /*reload*/) override { sRotationBotMgr.LoadConfig(); }

    // Profiles name spells, so they load once the spell store is ready.
    void OnStartup() override { sRotationBotMgr.LoadProfiles(); }
};

void AddSC_mod_rotation_bot()
{
    new RotationBotCommandScript();
    new RotationBotPlayerScript();
    new RotationBotWorldScript();
}
