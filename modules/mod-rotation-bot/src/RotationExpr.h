/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_ROTATION_BOT_EXPR_H
#define MOD_ROTATION_BOT_EXPR_H

#include "RotationState.h"

#include <functional>
#include <string_view>
#include <unordered_set>

// The player's rotation settings as they apply this tick, with auto modes already decided.
struct RotationSettings
{
    bool Pvp = false;
    bool Aoe = false;
    bool Burst = true;
    bool InCombat = false;
    bool Manual = false;                  // A `.rot next` press: interrupts don't wait for a reaction delay.
    std::unordered_set<ObjectGuid> Reacted; // Casters whose current cast's reaction delay has passed.

    bool CanReactTo(ObjectGuid caster) const { return Manual || Reacted.count(caster); }
};

// What a condition can read: the tick's snapshot and settings, plus the player for live lookups
// (cooldowns, known spells) that are not worth snapshotting for every spell.
struct AplContext
{
    Player* Me;
    RotationState const& State;
    RotationSettings const& Settings;
    UnitSnapshot const* Unit = nullptr; // The unit the entry is being tried on (`unit` in conditions).
};

// A compiled condition. Booleans are 0 or 1; anything non-zero is true.
using AplExpr = std::function<double(AplContext const&)>;

namespace RotationExpr
{
    // Compiles a condition such as `target.my_aura("Rend").remains < 1 and player.power >= 30`.
    // On failure returns nothing and sets error to a message that includes the column.
    Optional<AplExpr> Parse(std::string_view text, std::string& error);
}

#endif
