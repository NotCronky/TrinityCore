/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_ROTATION_BOT_PROFILES_H
#define MOD_ROTATION_BOT_PROFILES_H

#include "RotationApl.h"

// Profiles are YAML files (*.yml, *.yaml) in one folder. Loading happens on the world thread (at
// startup or from `.rot reload`), never while maps update, so lookups from map threads need no lock.
namespace RotationProfiles
{
    struct LoadResult
    {
        std::string Directory;
        std::size_t Loaded = 0;          // Files read without errors.
        std::size_t Kept = 0;            // Files with errors whose last good version stays in use.
        std::vector<std::string> Errors; // One line per problem, prefixed with the file name.
    };

    // ProfileDir from the config; empty means the profiles folder in the module's source tree.
    LoadResult Load(std::string const& configuredDirectory);

    // The profile that applies to the player, or nullptr.
    AplProfile const* Find(Player* player);

    // The talent tree (0-2) with the most points in the active spec, or nothing without talents.
    Optional<uint8> GetMainTree(Player* player);

    // A tree's name as profiles write it ("holy", "beast_mastery"), or empty.
    std::string GetSpecName(uint8 playerClass, uint8 tree);
}

#endif
