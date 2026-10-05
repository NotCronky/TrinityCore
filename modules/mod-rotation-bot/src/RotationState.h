/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_ROTATION_BOT_STATE_H
#define MOD_ROTATION_BOT_STATE_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Optional.h"
#include "Position.h"
#include "SharedDefines.h"
#include "UnitDefines.h"

#include <array>
#include <string>
#include <vector>

class Player;
class Unit;

// Mirrors NUM_RUNE_TYPES from Player.h, which this header avoids including.
constexpr uint8 RUNE_TYPE_COUNT = 4;

struct AuraSnapshot
{
    uint32 SpellId = 0;
    uint32 FirstRankId = 0; // Lets a condition name any rank of a spell.
    int32 RemainingMs = -1; // -1 means it does not expire.
    uint8 Stacks = 0;
    uint8 Charges = 0;
    bool Mine = false;
    bool Positive = false;
    uint8 DispelType = 0;        // DispelType: 1 magic, 2 curse, 3 disease, 4 poison.
    bool IsCrowdControl = false; // Stun, fear, polymorph, root, charm, sleep and the like.
};

struct CastSnapshot
{
    uint32 SpellId = 0;
    int32 RemainingMs = 0;
    bool Channeled = false;
    bool Interruptible = false; // The spell can be interrupted by a kick, ignoring immunities.
    bool IsHeal = false;        // Heals directly or over time.
    bool IsCrowdControl = false; // Stuns, fears, polymorphs, roots, charms and the like.
};

struct UnitSnapshot
{
    ObjectGuid Guid;
    std::string Name;
    float HealthPct = 0.0f;
    uint8 Level = 0;
    uint8 Class = 0;
    bool IsPlayer = false;
    bool IsBoss = false;

    // Relative to the player; left at their defaults in the player's own snapshot.
    Position Pos;
    bool Hostile = false;   // The player may attack it.
    float Distance = 0.0f;
    bool InMelee = false;
    bool InLos = false;
    bool Facing = false;    // The player faces it.
    bool Behind = false;    // The player is behind it.
    float ThreatPct = -1.0f; // The player's threat as a percentage of its current victim's; -1 without a threat list.

    // Who is fighting whom.
    ObjectGuid Victim;           // The unit it is attacking.
    bool TargetingMe = false;    // It is attacking the player.
    bool TargetingParty = false; // It is attacking one of the player's group members other than the player.
    uint32 Attackers = 0;        // Enemies within ENEMY_SEARCH_RADIUS attacking it.
    bool MostAttacked = false;   // Group members only: no other group member has more attackers (at least 1).

    // Loss of control. Charm and mind control are handled by the tick gate.
    bool Stunned = false;
    bool Rooted = false;
    bool Feared = false;
    bool Confused = false;
    bool Silenced = false;
    bool Disarmed = false;

    std::vector<AuraSnapshot> Auras; // Passive auras (talents, stances' passives) are left out.
    Optional<CastSnapshot> Cast;

    // Finds the aura by any rank of the spell. mineOnly ignores copies cast by others.
    AuraSnapshot const* FindAura(uint32 spellId, bool mineOnly) const;

    // Longest time left on a crowd control effect on it, in ms; 0 without one.
    int32 CrowdControlRemainingMs() const;

    // Auras of that dispel type a dispel would remove: buffs on a hostile unit (purge), debuffs
    // on anyone else.
    uint32 Dispellable(uint8 dispelType) const;
};

struct EnemySnapshot
{
    ObjectGuid Guid;
    Position Pos;
    float HealthPct = 0.0f;
    bool IsPlayer = false;
    bool BreakableCc = false; // Damage would break a crowd control effect (Polymorph, Sap, ...).
};

struct RotationState
{
    uint32 TakenAtMs = 0;

    // Player
    UnitSnapshot Self;
    Powers PowerType = POWER_MANA;
    uint32 Power = 0; // Rage and runic power in their displayed units, not the stored tenths.
    uint32 MaxPower = 0;
    uint8 ComboPoints = 0;
    std::array<uint8, RUNE_TYPE_COUNT> ReadyRunes = {}; // Ready runes by RuneType.
    ShapeshiftForm Form = FORM_NONE;
    bool Moving = false;
    uint32 GcdRemainingMs = 0;
    bool AutoRepeat = false;        // Auto Shot, Shoot or Throw is running.
    bool MainHandEnchanted = false; // A temporary enchant (poison, Windfury Weapon, ...) is on the weapon.
    bool OffHandEnchanted = false;
    std::array<bool, 4> Totems = {}; // Fire, earth, water, air: one of the player's is up within TOTEM_RANGE.

    // The player's pet or guardian (hunter and warlock pets, a permanent ghoul, ...).
    Optional<UnitSnapshot> Pet;

    // The unit the player has selected.
    Optional<UnitSnapshot> Target;

    // Attackable units within ENEMY_SEARCH_RADIUS of the player that are fighting, or are players.
    std::vector<EnemySnapshot> Enemies;

    // Full snapshots of Enemies, lowest health first.
    std::vector<UnitSnapshot> EnemyUnits;

    // The player and the group members within ENEMY_SEARCH_RADIUS that are alive, lowest health
    // first. Just the player when not in a group.
    std::vector<UnitSnapshot> Party;

    bool InCombat = false; // The player or a group member within ENEMY_SEARCH_RADIUS is fighting.

    float PowerPct() const { return MaxPower ? 100.0f * Power / MaxPower : 0.0f; }
    uint32 EnemiesNear(Position const& center, float radius) const;
};

namespace RotationStateBuilder
{
    constexpr float ENEMY_SEARCH_RADIUS = 40.0f;

    // A totem farther away than this counts as gone, so walking away from one gets a new one down.
    constexpr float TOTEM_RANGE = 30.0f;

    RotationState Build(Player* player);

    // Whether the fight is on for the rotation: the player, or a group member within
    // ENEMY_SEARCH_RADIUS, is in combat. A healer standing back isn't in combat until it acts.
    bool IsGroupInCombat(Player* player);

    // One line for logs.
    std::string Summarize(RotationState const& state);

    // A full dump for chat, one entry per line.
    std::vector<std::string> Describe(Player* player, RotationState const& state);
}

#endif
