/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#ifndef MOD_BOTS_BOT_COMPANION_H
#define MOD_BOTS_BOT_COMPANION_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Optional.h"

#include <string>
#include <vector>

class Player;

// A player's own alts as bots: they join the player's group, follow, fight what the player fights
// (mod-rotation-bot picks their spells) and come back when they die or fall behind.
namespace BotCompanion
{
    enum class Role : uint8 { Tank, Melee, Ranged, Healer };

    // From the class and the talent tree with the most points.
    Role GetRole(Player* bot);

    // Called on the world thread while maps are idle, every few hundred milliseconds. pullTarget is a unit
    // the owner sent the bot to attack (.bot pull); it is cleared once that unit can't be attacked.
    void Update(Player* bot, Player* owner, bool staying, ObjectGuid& pullTarget);

    // Puts the best bag in each empty bag slot (adding the slots to givenBags) and tops food, and water for
    // mana users, up to a stack for the bot's level.
    void GiveSupplies(Player* bot, std::vector<uint8>& givenBags);
    // Takes back what GiveSupplies gave an alt: the bags (their contents are mailed to the character
    // first), and all bot food and water.
    void RemoveSupplies(Player* bot, std::vector<uint8> const& givenBags);

    // Eats, and drinks if it uses mana, whatever its health and mana (.bot eat). False in combat or without
    // food or water.
    bool EatAndDrink(Player* bot);

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
