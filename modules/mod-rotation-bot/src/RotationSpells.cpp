/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationSpells.h"

#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "StringConvert.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace
{
    std::string ToLower(std::string_view text)
    {
        std::string lower(text);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return lower;
    }

    // Lower-case enUS spell name to every spell id with it. The spell store never changes after
    // startup, so it is built once, by the first profile load (on the world thread).
    std::unordered_map<std::string, std::vector<uint32>> const& GetNameIndex()
    {
        static std::unordered_map<std::string, std::vector<uint32>> const index = []
        {
            std::unordered_map<std::string, std::vector<uint32>> names;
            for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (info && info->SpellName[LOCALE_enUS] && *info->SpellName[LOCALE_enUS])
                    names[ToLower(info->SpellName[LOCALE_enUS])].push_back(id);
            }

            return names;
        }();

        return index;
    }
}

namespace
{
    // Lower-case item name to every item entry with it. Built once, like the spell name index.
    std::unordered_map<std::string, std::vector<uint32>> const& GetItemNameIndex()
    {
        static std::unordered_map<std::string, std::vector<uint32>> const index = []
        {
            std::unordered_map<std::string, std::vector<uint32>> names;
            for (auto const& [entry, item] : sObjectMgr->GetItemTemplateStore())
                names[ToLower(item.Name1)].push_back(entry);

            return names;
        }();

        return index;
    }

    // The on-use spell every PvP trinket shares.
    constexpr uint32 SPELL_PVP_TRINKET = 42292;

    uint32 GetOnUseSpell(Item const* item)
    {
        for (ItemEffect const& effect : item->GetTemplate()->Effects)
            if (effect.SpellID > 0 && effect.TriggerType == ITEM_SPELLTRIGGER_ON_USE)
                return uint32(effect.SpellID);

        return 0;
    }
}

Optional<ItemRef> ItemRef::Parse(std::string_view text)
{
    ItemRef ref;
    ref._text = std::string(text);

    std::string lower = ToLower(text);
    if (lower == "pvp_trinket")
    {
        ref._pvpTrinket = true;
        return ref;
    }

    if (lower == "trinket1" || lower == "trinket2")
    {
        ref._slot = lower == "trinket1" ? EQUIPMENT_SLOT_TRINKET1 : EQUIPMENT_SLOT_TRINKET2;
        return ref;
    }

    if (Optional<uint32> entry = Trinity::StringTo<uint32>(text))
    {
        if (!sObjectMgr->GetItemTemplate(*entry))
            return {};

        ref._entries.push_back(*entry);
        return ref;
    }

    auto const& index = GetItemNameIndex();
    auto itr = index.find(lower);
    if (itr == index.end())
        return {};

    ref._entries = itr->second;
    return ref;
}

Item* ItemRef::Find(Player* player, uint32& spellId) const
{
    auto usable = [player, &spellId](Item* item)
    {
        if (!item || player->CanUseItem(item) != EQUIP_ERR_OK)
            return false;

        spellId = GetOnUseSpell(item);
        return spellId != 0;
    };

    if (_pvpTrinket)
    {
        for (uint8 slot : { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 })
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (usable(item) && spellId == SPELL_PVP_TRINKET)
                return item;
        }

        return nullptr;
    }

    if (_slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, _slot);
        return usable(item) ? item : nullptr;
    }

    for (uint32 entry : _entries)
        if (Item* item = player->GetItemByEntry(entry); usable(item))
            return item;

    return nullptr;
}

Optional<SpellRef> SpellRef::FromId(uint32 spellId)
{
    if (!sSpellMgr->GetSpellInfo(spellId))
        return {};

    SpellRef ref;
    ref._text = std::to_string(spellId);
    ref._firstRankId = sSpellMgr->GetFirstSpellInChain(spellId);
    return ref;
}

Optional<SpellRef> SpellRef::Parse(std::string_view text)
{
    if (Optional<uint32> id = Trinity::StringTo<uint32>(text))
        return FromId(*id);

    auto const& index = GetNameIndex();
    auto itr = index.find(ToLower(text));
    if (itr == index.end())
        return {};

    SpellRef ref;
    ref._text = std::string(text);
    ref._ids.insert(itr->second.begin(), itr->second.end());

    for (uint32 id : itr->second)
        if (!sSpellMgr->GetSpellInfo(id)->IsPassive())
            ref._castable.push_back(id);

    std::stable_sort(ref._castable.begin(), ref._castable.end(), [](uint32 a, uint32 b)
    {
        return sSpellMgr->GetSpellRank(a) > sSpellMgr->GetSpellRank(b);
    });

    return ref;
}

uint32 SpellRef::KnownRank(Player* player) const
{
    if (_firstRankId)
    {
        uint32 last = sSpellMgr->GetLastSpellInChain(_firstRankId);
        for (uint32 rank = last; rank; rank = sSpellMgr->GetPrevSpellInChain(rank))
            if (player->HasSpell(rank))
                return rank;

        return 0;
    }

    for (uint32 id : _castable)
        if (player->HasSpell(id))
            return id;

    return 0;
}

SpellInfo const* SpellRef::Representative() const
{
    if (_firstRankId)
        return sSpellMgr->GetSpellInfo(_firstRankId);

    if (!_castable.empty())
        return sSpellMgr->GetSpellInfo(_castable.front());

    return _ids.empty() ? nullptr : sSpellMgr->GetSpellInfo(*_ids.begin());
}

uint8 GetDiminishingLevel(Unit* target, SpellInfo const* spellInfo)
{
    if (!target || !spellInfo)
        return 0;

    // The same checks Spell::DoSpellHitOnUnit makes before counting a hit toward diminishing returns.
    DiminishingGroup group = spellInfo->GetDiminishingReturnsGroupForSpell(false);
    if (group == DIMINISHING_NONE)
        return 0;

    DiminishingReturnsType type = spellInfo->GetDiminishingReturnsGroupType(false);
    if (type == DRTYPE_NONE || (type == DRTYPE_PLAYER && !target->IsCharmedOwnedByPlayerOrPlayer()))
        return 0;

    return uint8(std::min(target->GetDiminishing(group), DIMINISHING_LEVEL_IMMUNE));
}

bool SpellRef::Matches(uint32 spellId) const
{
    if (_firstRankId)
        return sSpellMgr->GetFirstSpellInChain(spellId) == _firstRankId;

    return _ids.count(spellId) != 0;
}
