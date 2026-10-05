/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationProfiles.h"

#include "DBCStores.h"
#include "Player.h"
#include "StringFormat.h"

#include <fkYAML/node.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
    // Keyed by file name, so a file that fails to load can keep its last good version.
    std::map<std::string, AplProfile> Profiles;

    std::unordered_map<std::string_view, uint8> const CLASS_NAMES =
    {
        { "warrior",      CLASS_WARRIOR },
        { "paladin",      CLASS_PALADIN },
        { "hunter",       CLASS_HUNTER },
        { "rogue",        CLASS_ROGUE },
        { "priest",       CLASS_PRIEST },
        { "death_knight", CLASS_DEATH_KNIGHT },
        { "shaman",       CLASS_SHAMAN },
        { "mage",         CLASS_MAGE },
        { "warlock",      CLASS_WARLOCK },
        { "druid",        CLASS_DRUID }
    };

    // Talent tree names per class, in the client's tab order (the order GetTalentTreePoints uses).
    std::unordered_map<uint8, std::array<std::string_view, 3>> const SPEC_NAMES =
    {
        { CLASS_WARRIOR,      { "arms", "fury", "protection" } },
        { CLASS_PALADIN,      { "holy", "protection", "retribution" } },
        { CLASS_HUNTER,       { "beast_mastery", "marksmanship", "survival" } },
        { CLASS_ROGUE,        { "assassination", "combat", "subtlety" } },
        { CLASS_PRIEST,       { "discipline", "holy", "shadow" } },
        { CLASS_DEATH_KNIGHT, { "blood", "frost", "unholy" } },
        { CLASS_SHAMAN,       { "elemental", "enhancement", "restoration" } },
        { CLASS_MAGE,         { "arcane", "fire", "frost" } },
        { CLASS_WARLOCK,      { "affliction", "demonology", "destruction" } },
        { CLASS_DRUID,        { "balance", "feral", "restoration" } }
    };

    // Thrown for a problem in a profile; path says where, such as "lists.pvp[3].if".
    struct ProfileError
    {
        std::string Path;
        std::string Message;
    };

    [[noreturn]] void Fail(std::string path, std::string message)
    {
        throw ProfileError{ std::move(path), std::move(message) };
    }

    // A scalar as text, whatever YAML type it was read as ("12294" and "true" are not strings).
    std::string ScalarText(fkyaml::node& node, std::string const& path)
    {
        if (node.is_string())
            return node.get_value<std::string>();

        if (node.is_integer())
            return std::to_string(node.get_value<int64_t>());

        if (node.is_boolean())
            return node.get_value<bool>() ? "true" : "false";

        if (node.is_float_number())
            return Trinity::StringFormat("{}", node.get_value<double>());

        Fail(path, "expected a single value");
    }

    SpellRef ParseSpell(fkyaml::node& node, std::string const& path)
    {
        std::string text = ScalarText(node, path);
        Optional<SpellRef> spell = SpellRef::Parse(text);
        if (!spell)
            Fail(path, Trinity::StringFormat("no spell is named '{}'", text));

        return std::move(*spell);
    }

    AplEntry ParseEntry(fkyaml::node& node, std::string const& path)
    {
        if (!node.is_mapping())
            Fail(path, "expected an entry with 'cast' or 'use'");

        AplEntry entry;

        for (auto& [keyNode, value] : node.as_map())
        {
            std::string key = keyNode.get_value<std::string>();
            std::string valuePath = path + "." + key;

            if (key == "cast")
                entry.Spell = ParseSpell(value, valuePath);
            else if (key == "use")
            {
                std::string text = ScalarText(value, valuePath);
                entry.UseItem = ItemRef::Parse(text);
                if (!entry.UseItem)
                    Fail(valuePath, Trinity::StringFormat("no item is named '{}'; use an item name or id, trinket1 "
                        "or trinket2", text));
            }
            else if (key == "on")
            {
                std::string target = ScalarText(value, valuePath);
                if (target == "target")
                    entry.Target = AplTarget::Target;
                else if (target == "self")
                    entry.Target = AplTarget::Self;
                else if (target == "enemy_players")
                    entry.Target = AplTarget::EnemyPlayers;
                else if (target == "enemies")
                    entry.Target = AplTarget::Enemies;
                else if (target == "party")
                    entry.Target = AplTarget::Party;
                else if (target == "pet")
                    entry.Target = AplTarget::Pet;
                else
                    Fail(valuePath, Trinity::StringFormat("expected target, self, enemies, enemy_players, party or "
                        "pet, not '{}'", target));
            }
            else if (key == "if")
            {
                entry.ConditionText = ScalarText(value, valuePath);
                std::string error;
                Optional<AplExpr> condition = RotationExpr::Parse(entry.ConditionText, error);
                if (!condition)
                    Fail(valuePath, error);

                entry.Condition = std::move(*condition);
            }
            else
                Fail(valuePath, "unknown key; an entry has cast or use, on and if");
        }

        if (entry.Spell.has_value() == entry.UseItem.has_value())
            Fail(path, "needs exactly one of 'cast' and 'use'");

        return entry;
    }

    AplProfile ParseProfile(std::string const& content, std::string const& file)
    {
        fkyaml::node root = fkyaml::node::deserialize(content);
        if (!root.is_mapping())
            Fail("", "expected name, class and lists at the top level");

        AplProfile profile;
        profile.File = file;
        bool hasClass = false;
        std::string specName;

        for (auto& [keyNode, value] : root.as_map())
        {
            std::string key = keyNode.get_value<std::string>();

            if (key == "name")
                profile.Name = ScalarText(value, key);
            else if (key == "class")
            {
                std::string name = ScalarText(value, key);
                auto itr = CLASS_NAMES.find(name);
                if (itr == CLASS_NAMES.end())
                    Fail(key, Trinity::StringFormat("unknown class '{}'", name));

                profile.Class = itr->second;
                hasClass = true;
            }
            else if (key == "requires")
                profile.Requires = ParseSpell(value, key);
            else if (key == "spec")
                specName = ScalarText(value, key);
            else if (key == "priority")
            {
                if (!value.is_integer())
                    Fail(key, "expected a whole number");

                profile.Priority = int32(value.get_value<int64_t>());
            }
            else if (key == "auto_attack")
            {
                if (!value.is_boolean())
                    Fail(key, "expected true or false");

                profile.AutoAttack = value.get_value<bool>();
            }
            else if (key == "aoe_enemies")
            {
                if (!value.is_integer() || value.get_value<int64_t>() < 1)
                    Fail(key, "expected a whole number of at least 1");

                profile.AoeEnemies = uint32(value.get_value<int64_t>());
            }
            else if (key == "aoe_radius")
            {
                if (!value.is_integer() && !value.is_float_number())
                    Fail(key, "expected a distance in yards");

                profile.AoeRadius = float(value.is_integer() ? value.get_value<int64_t>() : value.get_value<double>());
            }
            else if (key == "lists")
            {
                if (!value.is_mapping())
                    Fail(key, "expected list names, each with a list of entries");

                for (auto& [listKey, listNode] : value.as_map())
                {
                    std::string listName = listKey.get_value<std::string>();
                    std::string listPath = "lists." + listName;
                    auto const& names = RotationApl::ListNames();
                    if (std::find(names.begin(), names.end(), listName) == names.end())
                        Fail(listPath, "unknown list; lists are precombat, defensives, interrupts, pvp, pve_aoe and "
                            "pve_st");

                    if (!listNode.is_sequence())
                        Fail(listPath, "expected a list of entries, each starting with '- cast:'");

                    std::vector<AplEntry>& entries = profile.Lists[listName];
                    for (auto& entryNode : listNode.as_seq())
                        entries.push_back(ParseEntry(entryNode,
                            Trinity::StringFormat("{}[{}]", listPath, entries.size() + 1)));
                }
            }
            else
                Fail(key, "unknown key; a profile has name, class, spec, requires, priority, auto_attack, "
                    "aoe_enemies, aoe_radius and lists");
        }

        if (!hasClass)
            Fail("class", "missing");

        if (!specName.empty())
        {
            auto const& names = SPEC_NAMES.at(profile.Class);
            auto itr = std::find(names.begin(), names.end(), specName);
            if (itr == names.end())
                Fail("spec", Trinity::StringFormat("'{}' is not a talent tree of this class; use {}, {} or {}", specName,
                    names[0], names[1], names[2]));

            profile.Spec = uint8(itr - names.begin());
        }

        if (profile.Lists.empty())
            Fail("lists", "missing");

        if (profile.Name.empty())
            profile.Name = file;

        return profile;
    }

    std::string DefaultDirectory()
    {
#ifdef ROTATION_BOT_SOURCE_PROFILE_DIR
        return ROTATION_BOT_SOURCE_PROFILE_DIR;
#else
        return "rotation_profiles";
#endif
    }
}

RotationProfiles::LoadResult RotationProfiles::Load(std::string const& configuredDirectory)
{
    LoadResult result;
    result.Directory = configuredDirectory.empty() ? DefaultDirectory() : configuredDirectory;

    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_entry const& item : fs::directory_iterator(result.Directory, ec))
    {
        std::string extension = item.path().extension().string();
        if (item.is_regular_file() && (extension == ".yml" || extension == ".yaml"))
            files.push_back(item.path());
    }

    if (ec)
    {
        result.Errors.push_back(Trinity::StringFormat("can't read the folder: {}", ec.message()));
        return result;
    }

    std::sort(files.begin(), files.end());

    std::map<std::string, AplProfile> loaded;
    for (fs::path const& path : files)
    {
        std::string file = path.filename().string();

        std::ifstream stream(path);
        std::stringstream content;
        content << stream.rdbuf();

        try
        {
            loaded[file] = ParseProfile(content.str(), file);
            ++result.Loaded;
            continue;
        }
        catch (ProfileError const& e)
        {
            result.Errors.push_back(e.Path.empty() ? Trinity::StringFormat("{}: {}", file, e.Message) :
                Trinity::StringFormat("{}: {}: {}", file, e.Path, e.Message));
        }
        catch (std::exception const& e)
        {
            result.Errors.push_back(Trinity::StringFormat("{}: {}", file, e.what()));
        }

        // A typo made while tuning a profile mid-fight shouldn't take the rotation away.
        if (auto old = Profiles.find(file); old != Profiles.end())
        {
            loaded[file] = std::move(old->second);
            ++result.Kept;
        }
    }

    Profiles = std::move(loaded);
    return result;
}

Optional<uint8> RotationProfiles::GetMainTree(Player* player)
{
    // Points spent per talent tree in the active talent group, by the client's tab order.
    uint8 points[3] = {};
    uint32 classMask = player->GetClassMask();
    uint8 group = player->GetActiveTalentGroup();
    for (TalentEntry const* talent : sTalentStore)
    {
        TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TabID);
        if (!tab || !(tab->ClassMask & classMask) || tab->OrderIndex >= 3)
            continue;

        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            if (talent->SpellRank[rank] && player->HasTalent(talent->SpellRank[rank], group))
            {
                points[tab->OrderIndex] += rank + 1;
                break;
            }
        }
    }

    uint8 best = 0;
    for (uint8 i = 1; i < 3; ++i)
        if (points[i] > points[best])
            best = i;

    if (!points[best])
        return {};

    return best;
}

std::string RotationProfiles::GetSpecName(uint8 playerClass, uint8 tree)
{
    auto itr = SPEC_NAMES.find(playerClass);
    return itr != SPEC_NAMES.end() && tree < 3 ? std::string(itr->second[tree]) : "";
}

AplProfile const* RotationProfiles::Find(Player* player)
{
    Optional<uint8> mainTree = GetMainTree(player);
    AplProfile const* best = nullptr;
    for (auto const& [file, profile] : Profiles)
    {
        if (profile.Class != player->GetClass())
            continue;

        if (profile.Requires && !profile.Requires->KnownRank(player))
            continue;

        if (profile.Spec && profile.Spec != mainTree)
            continue;

        if (!best || profile.Priority > best->Priority)
            best = &profile;
    }

    return best;
}
