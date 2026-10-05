/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_ROTATION_BOT_APL_H
#define MOD_ROTATION_BOT_APL_H

#include "RotationExpr.h"
#include "RotationSpells.h"

#include <map>

class Item;
class SpellInfo;

enum class AplTarget : uint8
{
    Target,       // The hostile unit the player has selected.
    Self,
    // Each of these units in range, lowest health first; the first that passes is used.
    EnemyPlayers,
    Enemies,      // Every attackable unit that is fighting, plus enemy players.
    Party,        // The player and their group members.
    Pet           // The player's pet or guardian.
};

struct AplEntry
{
    Optional<SpellRef> Spell;    // `cast:` the highest rank the player knows...
    Optional<ItemRef> UseItem;   // ...or `use:` an item's on-use spell.
    AplTarget Target = AplTarget::Target;
    std::string ConditionText;   // As written in the profile, for the debug trace.
    AplExpr Condition;           // Empty means always.

    std::string const& ActionText() const { return Spell ? Spell->Text() : UseItem->Text(); }
};

// One profile file: the lists for a class (and optionally a spec, detected by a known spell).
struct AplProfile
{
    std::string Name;
    std::string File;
    uint8 Class = 0;
    Optional<SpellRef> Requires; // Only used when the player knows this spell.
    Optional<uint8> Spec;        // Only used when this talent tree (0-2) has the most points.
    int32 Priority = 0;          // Among the profiles that apply, the highest wins.
    bool AutoAttack = false;     // Start melee auto-attack on a hostile target in melee range.
    uint32 AoeEnemies = 3;       // Automatic AoE starts at this many enemies...
    float AoeRadius = 8.0f;      // ...within this many yards of the target (of the player without one).
    std::map<std::string, std::vector<AplEntry>, std::less<>> Lists;
};

struct AplChoice
{
    std::string_view ListName;
    AplEntry const* Entry = nullptr;
    std::size_t Index = 0;
    uint32 SpellId = 0;      // The rank to cast, or the item's on-use spell.
    Item* CastItem = nullptr;
    Unit* Target = nullptr;
};

namespace RotationApl
{
    // Every list name a profile may use.
    std::vector<std::string_view> const& ListNames();

    // The lists tried this tick, in order: defensives, interrupts, then pvp in PvP or pve_aoe (when
    // AoE is on) and pve_st in PvE. A profile without lists for the mode uses its other mode's.
    // Out of combat only precombat is tried, unless the tick is a `.rot next` press, which may
    // open the fight: then precombat comes first and the rest follow.
    std::vector<std::string_view> ListOrder(AplProfile const& profile, RotationSettings const& settings);

    // Everything the server would check before letting the player cast it now, including facing,
    // immunity and immunity from diminishing returns.
    SpellCastResult CanCast(Player* player, SpellInfo const* spellInfo, Unit* target, Item* castItem);

    // Casts the choice as the player. Ground-targeted spells (Blizzard, Death and Decay) go at the
    // chosen unit's feet.
    SpellCastResult Cast(Player* player, AplChoice const& choice);

    // Whether automatic AoE should be on: enough enemies around the target, and none of them in
    // crowd control that damage would break.
    bool WantsAoe(Player* player, AplProfile const& profile, RotationState const& state);

    // The first entry, across the profile's lists in ListOrder(), whose spell is known, whose
    // condition passes and that can be cast now. Interrupts entries wait for the reaction delay of
    // the unit they would hit (of the selected target, for self entries).
    Optional<AplChoice> Evaluate(Player* player, AplProfile const& profile, RotationState const& state,
        RotationSettings const& settings);
}

#endif
