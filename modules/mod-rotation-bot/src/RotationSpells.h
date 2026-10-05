/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_ROTATION_BOT_SPELLS_H
#define MOD_ROTATION_BOT_SPELLS_H

#include "Define.h"
#include "Optional.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class Item;
class Player;
class SpellInfo;
class Unit;

// A spell named in a profile, by its spellbook name ("Mortal Strike") or by id. A name covers
// every rank and every spell sharing it; an id covers every rank in that id's chain.
class SpellRef
{
public:
    // Fails when no spell has that name or id.
    static Optional<SpellRef> Parse(std::string_view text);
    static Optional<SpellRef> FromId(uint32 spellId);

    std::string const& Text() const { return _text; }

    // The highest rank the player knows, or 0.
    uint32 KnownRank(Player* player) const;

    // Whether an aura or cast of this spell id counts as this spell.
    bool Matches(uint32 spellId) const;

    // One spell standing for all of them, for properties every rank shares (diminishing returns).
    SpellInfo const* Representative() const;

private:
    std::string _text;
    uint32 _firstRankId = 0;                // Set for an id; matched by chain.
    std::unordered_set<uint32> _ids;         // Set for a name; every spell with it.
    std::vector<uint32> _castable;           // Non-passive spells with the name, highest rank first.
};

// The diminishing returns level the spell would land at on the target, read from the server's own
// tracking: 0 full duration, 1 half, 2 quarter, 3 immune. 0 for spells without diminishing returns,
// and for player-only categories on units that are not players or their pets.
uint8 GetDiminishingLevel(Unit* target, SpellInfo const* spellInfo);

// An item named in a profile's `use:`: trinket1 or trinket2 for the equipped trinket slots,
// pvp_trinket for whichever equipped trinket breaks crowd control (Medallion, Insignia, ...), or an
// item's name or id, found anywhere in the player's bags or equipment.
class ItemRef
{
public:
    // Fails when no item has that name or id.
    static Optional<ItemRef> Parse(std::string_view text);

    std::string const& Text() const { return _text; }

    // The item the player has and may use, and its on-use spell in spellId; nullptr when there is none.
    Item* Find(Player* player, uint32& spellId) const;

private:
    std::string _text;
    uint8 _slot = 0;                // Equipment slot for trinket1/trinket2; 0 otherwise.
    bool _pvpTrinket = false;
    std::vector<uint32> _entries;   // Item entries with the name or id.
};

#endif
