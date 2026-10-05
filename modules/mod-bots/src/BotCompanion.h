/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#ifndef MOD_BOTS_BOT_COMPANION_H
#define MOD_BOTS_BOT_COMPANION_H

#include "Define.h"
#include "Optional.h"

#include <string>

class Player;

// A player's own alts as bots: they join the player's group, follow, fight what the player fights
// (mod-rotation-bot picks their spells) and come back when they die or fall behind.
namespace BotCompanion
{
    enum class Role : uint8 { Melee, Ranged, Healer };

    // From the class and the talent tree with the most points.
    Role GetRole(Player* bot);

    // Called on the world thread while maps are idle, every few hundred milliseconds.
    void Update(Player* bot, Player* owner, bool staying);

    // Joins the owner's group, making one when there is none.
    void JoinGroup(Player* bot, Player* owner);

    // A talent tree by its name ("arms", "holy", "beast_mastery"...) or number (1-3) for the class.
    Optional<uint8> ParseSpec(uint8 playerClass, std::string const& text);
    std::string GetSpecName(uint8 playerClass, uint8 tree);

    // Learns the class spells for the bot's level, resets its talents and spends them in the tree (by
    // default the one it has most points in, else the first), and equips the best gear it can use for its
    // level and role. Returns what it did.
    std::string Equip(Player* bot, Optional<uint8> tree);
}

#endif
