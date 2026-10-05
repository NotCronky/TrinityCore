/*
 * Example module: greets players on login and counts their logins.
 * Shows the pieces every module uses: a loader function, a config file
 * (conf/mod_hello.conf.dist) and a database update (sql/characters/).
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"

namespace
{
    bool HelloEnabled = true;
    std::string HelloMessage;
}

class mod_hello_world : public WorldScript
{
public:
    mod_hello_world() : WorldScript("mod_hello_world") { }

    // Called on startup and on .reload config
    void OnConfigLoad(bool /*reload*/) override
    {
        HelloEnabled = sConfigMgr->GetBoolDefault("Hello.Enable", true);
        HelloMessage = sConfigMgr->GetStringDefault("Hello.Message", "Hello from mod-hello!");
    }
};

class mod_hello_player : public PlayerScript
{
public:
    mod_hello_player() : PlayerScript("mod_hello_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!HelloEnabled)
            return;

        // Query asynchronously so the login doesn't wait on the database;
        // the callback runs on the session once the result is ready.
        WorldSession* session = player->GetSession();
        ObjectGuid::LowType guid = player->GetGUID().GetCounter();
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(Trinity::StringFormat(
            "SELECT `logins` FROM `mod_hello_logins` WHERE `guid` = {}", guid).c_str()))
            .WithCallback([session, guid](QueryResult result)
        {
            uint32 logins = (result ? (*result)[0].GetUInt32() : 0) + 1;
            CharacterDatabase.Execute(Trinity::StringFormat(
                "REPLACE INTO `mod_hello_logins` (`guid`, `logins`) VALUES ({}, {})", guid, logins).c_str());

            if (session->GetPlayer())
                ChatHandler(session).PSendSysMessage("%s (login #%u)", HelloMessage.c_str(), logins);
        });
    }
};

// Called by the generated script loader; the name is Add<directory name with - replaced by _>Scripts
void Addmod_helloScripts()
{
    new mod_hello_world();
    new mod_hello_player();
}
