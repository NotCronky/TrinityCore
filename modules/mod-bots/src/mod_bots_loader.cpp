/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

void AddSC_mod_bots();

// Called by the generated script loader; the name is Add<directory name with - replaced by _>Scripts
void Addmod_botsScripts()
{
    AddSC_mod_bots();
}
