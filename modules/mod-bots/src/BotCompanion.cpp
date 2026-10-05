/*
 * mod-bots: player bots, real characters logged in on sessions without a network connection.
 */

#include "BotCompanion.h"

#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Bag.h"
#include "Item.h"
#include "Mail.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "RotationProfiles.h" // mod-rotation-bot: the talent tree with the most points
#include "StringFormat.h"
#include "Trainer.h"

#include <algorithm>
#include <array>
#include <map>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr float FOLLOW_DISTANCE = 2.5f;
    constexpr float RANGED_DISTANCE = 25.0f;
    // Further than this, or on another map, the bot is brought to its owner.
    constexpr float CATCH_UP_DISTANCE = 80.0f;

    // Within this distance of the owner a bot sits down to eat and drink; further away it follows instead.
    constexpr float EAT_DISTANCE = 20.0f;

    constexpr uint32 BAG = 41599; // Frostweave Bag: 20 slots, no level requirement
    constexpr uint32 SUPPLY_STACK = 20;

    // Vendor food and water by required level, lowest first.
    std::vector<uint32> const FOOD = { 4540, 4541, 4542, 4544, 4601, 8950, 27855, 29449, 35950 };
    std::vector<uint32> const WATER = { 159, 1179, 1205, 1708, 1645, 8766, 28399, 27860, 33445 };

    // The best item of the list the bot's level allows.
    uint32 BestForLevel(Player* bot, std::vector<uint32> const& items)
    {
        uint32 best = 0;
        for (uint32 entry : items)
            if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry); item && item->RequiredLevel <= bot->GetLevel())
                best = entry;
        return best;
    }

    // Eats or drinks the best item of the list it carries.
    bool Consume(Player* bot, std::vector<uint32> const& items)
    {
        for (auto itr = items.rbegin(); itr != items.rend(); ++itr)
        {
            Item* item = bot->GetItemByEntry(*itr);
            if (!item || bot->CanUseItem(item) != EQUIP_ERR_OK)
                continue;

            uint32 spellId = item->GetTemplate()->Effects[0].SpellID > 0 ? uint32(item->GetTemplate()->Effects[0].SpellID) : 0;
            if (spellId && bot->CastSpell(bot, spellId, CastSpellExtraArgs(TRIGGERED_NONE).SetCastItem(item)) == SPELL_CAST_OK)
                return true;
        }
        return false;
    }

    // Talent tree names per class, in the client's tab order.
    std::unordered_map<uint8, std::array<char const*, 3>> const SPEC_NAMES =
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
        { CLASS_DRUID,        { "balance", "feral", "restoration" } },
    };

    uint8 GetTree(Player* bot)
    {
        return RotationProfiles::GetMainTree(bot).value_or(0);
    }

    // The group members near the bot (the bot too), or just the owner and the bot without a group.
    std::vector<Player*> GetParty(Player* bot, Player* owner)
    {
        std::vector<Player*> party;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                if (Player* member = itr->GetSource(); member && member->IsInWorld() && member->IsInMap(bot) &&
                    member->IsAlive() && bot->IsWithinDistInMap(member, 60.0f))
                    party.push_back(member);
        }
        else
            party = { owner, bot };

        return party;
    }

    // Which unit a tank should fight: a mob hitting someone else in the group (nearest first, so its
    // taunts can take it), what it already fights, or what it was sent to pull. It never starts a fight
    // on its own: the owner sends it with .bot pull.
    Unit* PickTankTarget(Player* bot, Player* owner, Unit* pull)
    {
        auto valid = [bot](Unit* unit) { return unit && unit->IsAlive() && bot->IsValidAttackTarget(unit); };

        Unit* loose = nullptr;
        for (Player* member : GetParty(bot, owner))
        {
            if (member == bot)
                continue;

            for (Unit* attacker : member->getAttackers())
                if (valid(attacker) && (!loose || bot->GetDistance(attacker) < bot->GetDistance(loose)))
                    loose = attacker;
        }

        if (loose)
            return loose;

        if (valid(bot->GetVictim()))
            return bot->GetVictim();

        if (valid(pull))
            return pull;

        for (Unit* attacker : bot->getAttackers())
            if (valid(attacker))
                return attacker;

        return nullptr;
    }

    // Which unit the bot should fight: what its owner fights, what a tank of the group fights, or what is
    // attacking the owner or the bot.
    Unit* PickTarget(Player* bot, Player* owner)
    {
        auto valid = [bot](Unit* unit) { return unit && unit->IsAlive() && bot->IsValidAttackTarget(unit); };

        if (valid(owner->GetVictim()))
            return owner->GetVictim();

        for (Player* member : GetParty(bot, owner))
            if (member != bot && member->IsInCombat() && BotCompanion::GetRole(member) == BotCompanion::Role::Tank &&
                valid(member->GetVictim()))
                return member->GetVictim();

        if (Unit* selected = owner->GetSelectedUnit(); valid(selected) && selected->IsInCombat() && owner->IsInCombat())
            return selected;

        for (Unit* attacker : owner->getAttackers())
            if (valid(attacker))
                return attacker;

        for (Unit* attacker : bot->getAttackers())
            if (valid(attacker))
                return attacker;

        return nullptr;
    }

    // Item stats a role wants, and stats it would waste. Unlisted stats count a little.
    float StatWeight(BotCompanion::Role role, bool strength, uint32 stat)
    {
        switch (stat)
        {
            case ITEM_MOD_STAMINA: return 1.0f;
            case ITEM_MOD_STRENGTH: return role == BotCompanion::Role::Melee && strength ? 2.0f : -1.0f;
            case ITEM_MOD_AGILITY: return role == BotCompanion::Role::Melee ? (strength ? 0.5f : 2.0f) : (role == BotCompanion::Role::Ranged && !strength ? 2.0f : -1.0f);
            case ITEM_MOD_INTELLECT: return role == BotCompanion::Role::Melee ? -1.0f : 2.0f;
            case ITEM_MOD_SPIRIT: return role == BotCompanion::Role::Healer ? 1.0f : (role == BotCompanion::Role::Melee ? -1.0f : 0.3f);
            case ITEM_MOD_SPELL_POWER: case ITEM_MOD_SPELL_HEALING_DONE: case ITEM_MOD_SPELL_DAMAGE_DONE:
            case ITEM_MOD_MANA_REGENERATION:
                return role == BotCompanion::Role::Melee ? -1.0f : 1.5f;
            case ITEM_MOD_ATTACK_POWER: case ITEM_MOD_RANGED_ATTACK_POWER: case ITEM_MOD_EXPERTISE_RATING:
            case ITEM_MOD_ARMOR_PENETRATION_RATING:
                return role == BotCompanion::Role::Melee || (role == BotCompanion::Role::Ranged && !strength) ? 1.0f : -1.0f;
            default:
                return 0.5f;
        }
    }
}

namespace BotCompanion
{
    Role GetRole(Player* bot)
    {
        uint8 tree = GetTree(bot);
        switch (bot->GetClass())
        {
            case CLASS_WARRIOR:
                return tree == 2 ? Role::Tank : Role::Melee;
            case CLASS_DEATH_KNIGHT:
                return tree == 0 ? Role::Tank : Role::Melee;
            case CLASS_ROGUE:
                return Role::Melee;
            case CLASS_PALADIN:
                return tree == 0 ? Role::Healer : (tree == 1 ? Role::Tank : Role::Melee);
            case CLASS_PRIEST:
                return tree == 2 ? Role::Ranged : Role::Healer;
            case CLASS_SHAMAN:
                return tree == 1 ? Role::Melee : (tree == 2 ? Role::Healer : Role::Ranged);
            case CLASS_DRUID:
                return tree == 1 ? Role::Melee : (tree == 2 ? Role::Healer : Role::Ranged);
            default: // Hunter, mage, warlock
                return Role::Ranged;
        }
    }

    bool EatAndDrink(Player* bot)
    {
        if (!bot->IsAlive() || bot->IsInCombat())
            return false;

        bot->GetMotionMaster()->Clear();
        bot->StopMoving();

        bool ate = bot->HasAuraType(SPELL_AURA_MOD_REGEN) || Consume(bot, FOOD);
        bool drank = bot->GetPowerType() != POWER_MANA || bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN) || Consume(bot, WATER);
        return ate || drank;
    }

    void GiveSupplies(Player* bot, std::vector<uint8>& givenBags)
    {
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        {
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                continue;

            uint16 dest;
            if (bot->CanEquipNewItem(slot, dest, BAG, false) == EQUIP_ERR_OK && bot->EquipNewItem(dest, BAG, true))
                givenBags.push_back(slot);
        }

        uint32 food = BestForLevel(bot, FOOD);
        if (food && bot->GetItemCount(food) < SUPPLY_STACK)
            bot->AddItem(food, SUPPLY_STACK - bot->GetItemCount(food));

        uint32 water = bot->GetPowerType() == POWER_MANA ? BestForLevel(bot, WATER) : 0;
        if (water && bot->GetItemCount(water) < SUPPLY_STACK)
            bot->AddItem(water, SUPPLY_STACK - bot->GetItemCount(water));
    }

    void RemoveSupplies(Player* bot, std::vector<uint8> const& givenBags)
    {
        for (uint8 slot : givenBags)
        {
            Item* bagItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!bagItem || bagItem->GetEntry() != BAG)
                continue;

            // Anything picked up into the bag is mailed to the character before the bag goes.
            if (Bag* bag = bagItem->ToBag())
            {
                for (uint32 i = 0; i < bag->GetBagSize(); ++i)
                {
                    Item* item = bag->GetItemByPos(uint8(i));
                    if (!item)
                        continue;

                    bot->MoveItemFromInventory(slot, uint8(i), true);
                    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                    item->DeleteFromInventoryDB(trans);
                    item->SaveToDB(trans);
                    MailDraft("Items from a bot bag", "These were in a bag the bot was lent.").AddItem(item).SendMailTo(trans, bot,
                        MailSender(bot, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED | MAIL_CHECK_MASK_NOT_RETURNABLE);
                    CharacterDatabase.CommitTransaction(trans);
                }
            }

            bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        }

        for (std::vector<uint32> const* list : { &FOOD, &WATER })
            for (uint32 entry : *list)
                if (uint32 count = bot->GetItemCount(entry))
                    bot->DestroyItemCount(entry, count, true);
    }

    void JoinGroup(Player* bot, Player* owner)
    {
        if (bot->GetGroup())
            return;

        if (Group* group = owner->GetGroup())
        {
            if (!group->IsFull())
                group->AddMember(bot);
            return;
        }

        Group* group = new Group();
        if (!group->Create(owner))
        {
            delete group;
            return;
        }

        sGroupMgr->AddGroup(group);
        group->AddMember(bot);
    }

    void Update(Player* bot, Player* owner, bool staying, ObjectGuid& pullTarget)
    {
        if (bot->IsBeingTeleported() || owner->IsBeingTeleported())
            return;

        // Dead: back on its feet once the fight is over (the owner doesn't have to run it back).
        if (!bot->IsAlive())
        {
            if (owner->IsAlive() && !owner->IsInCombat())
            {
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();
                bot->TeleportTo(owner->GetMapId(), owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(),
                    owner->GetOrientation());
            }
            return;
        }

        if (!staying && (bot->GetMapId() != owner->GetMapId() || bot->GetDistance(owner) > CATCH_UP_DISTANCE))
        {
            if (bot->IsInCombat() && bot->GetMapId() == owner->GetMapId())
                return;

            bot->TeleportTo(owner->GetMapId(), owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(),
                owner->GetOrientation());
            return;
        }

        Role role = GetRole(bot);
        MotionMaster* motion = bot->GetMotionMaster();

        // A unit the owner sent the bot to attack, until it can't be attacked any more.
        Unit* pull = pullTarget.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*bot, pullTarget);
        if (!pull || !pull->IsAlive() || !bot->IsValidAttackTarget(pull))
        {
            pullTarget.Clear();
            pull = nullptr;
        }

        Unit* target = role == Role::Tank ? PickTankTarget(bot, owner, pull) : PickTarget(bot, owner);
        if (pull && !target)
            target = pull;
        if (target && role != Role::Healer)
        {
            // mod-rotation-bot casts at the selected target.
            bot->SetSelection(target->GetGUID());
            bool melee = role == Role::Melee || role == Role::Tank;
            bool newTarget = bot->GetVictim() != target;
            if (newTarget)
                bot->Attack(target, melee);

            if (motion->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE || newTarget)
            {
                motion->Clear();
                if (melee)
                    motion->MoveChase(target);
                else
                    motion->MoveChase(target, RANGED_DISTANCE);
            }
            return;
        }

        // Healers stay with the owner and heal (their rotation picks the group member); others stop fighting.
        if (target)
            bot->SetSelection(target->GetGUID());
        else
        {
            if (bot->GetVictim())
                bot->AttackStop();
            bot->SetSelection(ObjectGuid::Empty);
        }

        if (staying)
        {
            if (motion->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
                motion->Clear();
            return;
        }

        // Out of combat: eat and drink below 60%, and stay seated while it lasts unless the owner walks off.
        if (!target && !bot->IsInCombat() && !owner->IsInCombat() && bot->GetDistance(owner) < EAT_DISTANCE)
        {
            bool usesMana = bot->GetPowerType() == POWER_MANA;
            bool eating = bot->HasAuraType(SPELL_AURA_MOD_REGEN);
            bool drinking = bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN);
            // Seated while the food or drink lasts, also when sent to eat at full health (.bot eat). Only one
            // can start at a time, so the other follows on a later update: a mana user that eats also drinks,
            // and one that drinks also eats while below full health.
            if (eating || drinking)
            {
                if (eating && !drinking && usesMana)
                    Consume(bot, WATER);
                else if (drinking && !eating && bot->GetHealthPct() < 100.0f)
                    Consume(bot, FOOD);
                return;
            }

            bool ate = !eating && bot->GetHealthPct() < 60.0f && Consume(bot, FOOD);
            bool drank = !drinking && usesMana && bot->GetPowerPct(POWER_MANA) < 60.0f && Consume(bot, WATER);
            if (ate || drank)
            {
                motion->Clear();
                bot->StopMoving();
                return;
            }
        }

        if (motion->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
        {
            motion->Clear();
            // Spread around the owner by their group position.
            float angle = float(M_PI) * 0.5f + (bot->GetGUID().GetCounter() % 5) * float(M_PI) * 0.25f;
            motion->MoveFollow(owner, role == Role::Healer ? FOLLOW_DISTANCE * 3 : FOLLOW_DISTANCE, ChaseAngle(angle));
        }
    }

    Optional<uint8> ParseSpec(uint8 playerClass, std::string const& text)
    {
        auto itr = SPEC_NAMES.find(playerClass);
        if (itr == SPEC_NAMES.end())
            return {};

        if (text == "1" || text == "2" || text == "3")
            return uint8(text[0] - '1');

        for (uint8 tree = 0; tree < 3; ++tree)
            if (text == itr->second[tree])
                return tree;

        return {};
    }

    std::string GetSpecName(uint8 playerClass, uint8 tree)
    {
        auto itr = SPEC_NAMES.find(playerClass);
        return itr != SPEC_NAMES.end() && tree < 3 ? itr->second[tree] : "?";
    }

    namespace
    {
        void LearnClassSpells(Player* bot)
        {
            std::vector<Trainer::Trainer const*> const& trainers = sObjectMgr->GetClassTrainers(bot->GetClass());
            bool learned;
            do
            {
                learned = false;
                for (Trainer::Trainer const* trainer : trainers)
                {
                    if (!trainer->IsTrainerValidForPlayer(bot))
                        continue;

                    for (Trainer::Spell const& spell : trainer->GetSpells())
                    {
                        if (!trainer->CanTeachSpell(bot, &spell))
                            continue;

                        if (spell.IsCastable())
                            bot->CastSpell(bot, spell.SpellId, true);
                        else
                            bot->LearnSpell(spell.SpellId, false);
                        learned = true;
                    }
                }
            } while (learned);
        }

        // Spends every talent point, deepest talent first in the chosen tree so its key talents (Mortal
        // Strike, Shadowform...) come as early as the points allow, then in the other trees.
        uint32 SpendTalents(Player* bot, uint8 tree)
        {
            bot->ResetTalents(true);
            uint8 group = bot->GetActiveTalentGroup();

            std::array<std::vector<TalentEntry const*>, 3> trees;
            for (TalentEntry const* talent : sTalentStore)
                if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TabID))
                    if ((tab->ClassMask & bot->GetClassMask()) && tab->OrderIndex < 3)
                        trees[tab->OrderIndex].push_back(talent);

            for (auto& talents : trees)
                std::sort(talents.begin(), talents.end(), [](TalentEntry const* a, TalentEntry const* b)
                {
                    return a->TierID != b->TierID ? a->TierID > b->TierID : a->ColumnIndex < b->ColumnIndex;
                });

            std::array<uint8, 3> order = { tree, uint8((tree + 1) % 3), uint8((tree + 2) % 3) };
            uint32 spent = 0;
            while (bot->GetFreeTalentPoints() > 0)
            {
                bool learned = false;
                for (uint8 t : order)
                {
                    for (TalentEntry const* talent : trees[t])
                    {
                        int8 rank = -1;
                        for (int8 r = MAX_TALENT_RANK - 1; r >= 0; --r)
                            if (talent->SpellRank[r] && bot->HasTalent(talent->SpellRank[r], group))
                            {
                                rank = r;
                                break;
                            }

                        uint8 next = uint8(rank + 1);
                        if (next < MAX_TALENT_RANK && talent->SpellRank[next] && bot->LearnTalent(talent->ID, next))
                        {
                            learned = true;
                            ++spent;
                            break;
                        }
                    }

                    if (learned)
                        break;
                }

                if (!learned)
                    break;
            }

            bot->SendTalentsInfoData(false);
            return spent;
        }

        // The armor type a class wears at a level: mail and plate from 40, like live.
        uint32 PreferredArmor(Player* bot)
        {
            bool from40 = bot->GetLevel() >= 40;
            switch (bot->GetClass())
            {
                case CLASS_WARRIOR: case CLASS_PALADIN: return from40 ? ITEM_SUBCLASS_ARMOR_PLATE : ITEM_SUBCLASS_ARMOR_MAIL;
                case CLASS_DEATH_KNIGHT: return ITEM_SUBCLASS_ARMOR_PLATE;
                case CLASS_HUNTER: case CLASS_SHAMAN: return from40 ? ITEM_SUBCLASS_ARMOR_MAIL : ITEM_SUBCLASS_ARMOR_LEATHER;
                case CLASS_ROGUE: case CLASS_DRUID: return ITEM_SUBCLASS_ARMOR_LEATHER;
                default: return ITEM_SUBCLASS_ARMOR_CLOTH;
            }
        }

        // The best item level a character of the level should have: dungeon gear for the level. Item
        // required levels can't be trusted for this (many raid items have none in the database).
        uint32 MaxItemLevel(uint8 level)
        {
            if (level <= 60)
                return level + 6;
            if (level <= 70)
                return 66 + (level - 60) * 5;
            return 116 + uint32((level - 70) * 7.4f);
        }

        bool IsPlaceholder(ItemTemplate const& item)
        {
            for (char const* word : { "Test", "Deprecated", "Monster", "[PH]", "OLD", "QA", "zzOLD", "Unused", "NPC" })
                if (item.Name1.find(word) != std::string::npos)
                    return true;
            return false;
        }

        float Score(Player* bot, ItemTemplate const& item, BotCompanion::Role role, bool strength)
        {
            float score = 0.0f;
            for (uint32 i = 0; i < item.StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
                score += StatWeight(role, strength, item.ItemStat[i].ItemStatType) * item.ItemStat[i].ItemStatValue;

            if (item.Class == ITEM_CLASS_ARMOR && item.SubClass >= ITEM_SUBCLASS_ARMOR_CLOTH &&
                item.SubClass <= ITEM_SUBCLASS_ARMOR_PLATE && item.InventoryType != INVTYPE_CLOAK)
            {
                if (item.SubClass != PreferredArmor(bot))
                    return -1.0f; // Wrong armor type for the class: never
            }

            // Weapons and items with only small stats still upgrade with item level.
            return score + item.ItemLevel * 0.5f;
        }

        struct SlotRule
        {
            uint8 Slot;
            std::vector<uint32> Types;
        };

        uint32 EquipGear(Player* bot, BotCompanion::Role role, uint8 tree)
        {
            bool strength = bot->GetClass() == CLASS_WARRIOR || bot->GetClass() == CLASS_DEATH_KNIGHT ||
                bot->GetClass() == CLASS_PALADIN;
            bool tank = (bot->GetClass() == CLASS_WARRIOR && tree == 2) || (bot->GetClass() == CLASS_PALADIN && tree == 1);

            std::vector<SlotRule> rules =
            {
                { EQUIPMENT_SLOT_HEAD, { INVTYPE_HEAD } }, { EQUIPMENT_SLOT_NECK, { INVTYPE_NECK } },
                { EQUIPMENT_SLOT_SHOULDERS, { INVTYPE_SHOULDERS } }, { EQUIPMENT_SLOT_CHEST, { INVTYPE_CHEST, INVTYPE_ROBE } },
                { EQUIPMENT_SLOT_WAIST, { INVTYPE_WAIST } }, { EQUIPMENT_SLOT_LEGS, { INVTYPE_LEGS } },
                { EQUIPMENT_SLOT_FEET, { INVTYPE_FEET } }, { EQUIPMENT_SLOT_WRISTS, { INVTYPE_WRISTS } },
                { EQUIPMENT_SLOT_HANDS, { INVTYPE_HANDS } }, { EQUIPMENT_SLOT_FINGER1, { INVTYPE_FINGER } },
                { EQUIPMENT_SLOT_FINGER2, { INVTYPE_FINGER } }, { EQUIPMENT_SLOT_TRINKET1, { INVTYPE_TRINKET } },
                { EQUIPMENT_SLOT_TRINKET2, { INVTYPE_TRINKET } }, { EQUIPMENT_SLOT_BACK, { INVTYPE_CLOAK } },
                { EQUIPMENT_SLOT_MAINHAND, { INVTYPE_WEAPON, INVTYPE_2HWEAPON, INVTYPE_WEAPONMAINHAND } },
                { EQUIPMENT_SLOT_OFFHAND, { } },
                { EQUIPMENT_SLOT_RANGED, { INVTYPE_RANGED, INVTYPE_RANGEDRIGHT, INVTYPE_THROWN, INVTYPE_RELIC } },
            };

            // Tanks and healing paladins/shamans want a shield, casters an off-hand item, dual-wielders a weapon.
            std::vector<uint32>& offhand = rules[15].Types;
            if (tank || (role == BotCompanion::Role::Healer && (bot->GetClass() == CLASS_PALADIN || bot->GetClass() == CLASS_SHAMAN)))
                offhand = { INVTYPE_SHIELD };
            else if (role != BotCompanion::Role::Melee)
                offhand = { INVTYPE_HOLDABLE };
            else if (bot->CanDualWield())
                offhand = { INVTYPE_WEAPON, INVTYPE_WEAPONOFFHAND };
            if (tank)
                rules[14].Types = { INVTYPE_WEAPON, INVTYPE_WEAPONMAINHAND };

            // Empty every slot first, so a two-hander and an off-hand don't block each other. Old items go
            // into the bags, or by mail when they're full (like Player::AutoUnequipOffhandIfNeed): nothing
            // is ever destroyed.
            for (SlotRule const& rule : rules)
            {
                Item* old = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, rule.Slot);
                if (!old)
                    continue;

                ItemPosCountVec dest;
                if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, old, false) == EQUIP_ERR_OK)
                {
                    bot->RemoveItem(INVENTORY_SLOT_BAG_0, rule.Slot, true);
                    bot->StoreItem(dest, old, true);
                    continue;
                }

                bot->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, rule.Slot, true);
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                old->DeleteFromInventoryDB(trans);
                old->SaveToDB(trans);
                MailDraft("Your old gear", "Replaced by .bot gear").AddItem(old).SendMailTo(trans, bot,
                    MailSender(bot, MAIL_STATIONERY_GM), MAIL_CHECK_MASK_COPIED | MAIL_CHECK_MASK_NOT_RETURNABLE);
                CharacterDatabase.CommitTransaction(trans);
            }

            std::vector<uint32> taken; // Unique-equipped rings and trinkets: a different one in each slot
            uint32 equipped = 0;
            for (SlotRule const& rule : rules)
            {
                if (rule.Types.empty() || bot->GetItemByPos(INVENTORY_SLOT_BAG_0, rule.Slot))
                    continue;

                if (rule.Slot == EQUIPMENT_SLOT_OFFHAND)
                    if (Item* mainhand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                        if (mainhand->GetTemplate()->InventoryType == INVTYPE_2HWEAPON)
                            continue;

                ItemTemplate const* best = nullptr;
                float bestScore = 0.0f;
                uint32 maxItemLevel = MaxItemLevel(bot->GetLevel());
                for (auto const& [entry, item] : sObjectMgr->GetItemTemplateStore())
                {
                    if (item.Quality < ITEM_QUALITY_UNCOMMON || item.Quality > ITEM_QUALITY_EPIC || item.RequiredLevel > bot->GetLevel() ||
                        item.ItemLevel > maxItemLevel ||
                        std::find(rule.Types.begin(), rule.Types.end(), item.InventoryType) == rule.Types.end() ||
                        std::find(taken.begin(), taken.end(), entry) != taken.end() || IsPlaceholder(item) ||
                        bot->CanUseItem(&item) != EQUIP_ERR_OK)
                        continue;

                    float score = Score(bot, item, role, strength);
                    if (score > bestScore)
                    {
                        best = &item;
                        bestScore = score;
                    }
                }

                if (!best)
                    continue;

                uint16 dest;
                if (bot->CanEquipNewItem(rule.Slot, dest, best->ItemId, false) != EQUIP_ERR_OK)
                    continue;

                bot->EquipNewItem(dest, best->ItemId, true);
                taken.push_back(best->ItemId);
                ++equipped;
            }

            return equipped;
        }
    }

    std::string Equip(Player* bot, Optional<uint8> tree)
    {
        uint8 chosen = tree.value_or(GetTree(bot));
        LearnClassSpells(bot);
        uint32 talents = SpendTalents(bot, chosen);
        Role role = GetRole(bot);
        uint32 items = EquipGear(bot, role == Role::Tank ? Role::Melee : role, chosen);
        bot->SaveToDB();

        return Trinity::StringFormat("{}: level {} {}, {} talent points, {} items equipped.", bot->GetName(), bot->GetLevel(),
            GetSpecName(bot->GetClass(), chosen), talents, items);
    }
}
