/*
 * mod-progression: each character plays in a patch of its own, from 1.1 to 3.3.5.
 */

void AddSC_mod_progression();

// Called by the generated script loader; the name is Add<directory name with - replaced by _>Scripts
void Addmod_progressionScripts()
{
    AddSC_mod_progression();
}
