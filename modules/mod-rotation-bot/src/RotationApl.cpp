/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationApl.h"

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Item.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellHistory.h"
#include "SpellMgr.h"

std::vector<std::string_view> const& RotationApl::ListNames()
{
    static std::vector<std::string_view> const names =
    {
        "precombat", "defensives", "interrupts", "pvp", "pve_aoe", "pve_st"
    };
    return names;
}

std::vector<std::string_view> RotationApl::ListOrder(AplProfile const& profile, RotationSettings const& settings)
{
    bool hasPve = profile.Lists.count("pve_st") || profile.Lists.count("pve_aoe");
    bool hasPvp = profile.Lists.count("pvp");

    std::vector<std::string_view> order;
    if (!settings.InCombat)
    {
        order.push_back("precombat");
        if (!settings.Manual)
            return order;
    }

    order.push_back("defensives");
    order.push_back("interrupts");
    if (hasPvp && (settings.Pvp || !hasPve))
        order.push_back("pvp");
    else
    {
        // The single-target list follows the AoE list, so it fills whatever the AoE list leaves.
        if (settings.Aoe)
            order.push_back("pve_aoe");

        order.push_back("pve_st");
    }

    return order;
}

bool RotationApl::WantsAoe(Player* player, AplProfile const& profile, RotationState const& state)
{
    Position center = state.Target && state.Target->Hostile ? state.Target->Pos : player->GetPosition();

    uint32 count = 0;
    for (EnemySnapshot const& enemy : state.Enemies)
    {
        if (center.GetExactDist(&enemy.Pos) > profile.AoeRadius)
            continue;

        if (enemy.BreakableCc)
            return false;

        ++count;
    }

    return count >= profile.AoeEnemies;
}

namespace
{
    SpellCastTargets MakeTargets(SpellInfo const* spellInfo, Unit* target)
    {
        SpellCastTargets targets;
        uint32 explicitTargets = spellInfo->GetExplicitTargetMask();
        if (explicitTargets & TARGET_FLAG_DEST_LOCATION)
            targets.SetDst(*target);
        else if (explicitTargets & TARGET_FLAG_UNIT_MASK)
            targets.SetUnitTarget(target);
        // Otherwise the spell takes no target, like Whirlwind or Thunder Clap around the caster, and
        // the client sends none. A unit target would make CheckCast range-check it against the
        // spell's "self" range (0 yards plus both combat reaches), which fails at normal melee distance.

        return targets;
    }

    // Where a spell without a target lands: enemies within Radius of the caster, or in a cone in front.
    struct CasterArea
    {
        float Radius = 0.0f;
        bool Cone = false;
    };

    // Whirlwind, Thunder Clap, Frost Nova, Shockwave, Cone of Cold: enemies around or in front of the caster.
    // Also ground effects placed at the caster (Consecration) and auras whose periodic spell does that
    // (Bladestorm), one level deep.
    Optional<CasterArea> GetCasterArea(SpellInfo const* spellInfo, Player* caster, bool followTrigger = true)
    {
        if (spellInfo->GetExplicitTargetMask() & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_DEST_LOCATION | TARGET_FLAG_GAMEOBJECT_MASK))
            return {};

        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if (!effect.Effect)
                continue;

            for (SpellImplicitTargetInfo const* target : { &effect.TargetA, &effect.TargetB })
            {
                SpellTargetSelectionCategories category = target->GetSelectionCategory();
                if ((category == TARGET_SELECT_CATEGORY_AREA || category == TARGET_SELECT_CATEGORY_CONE) &&
                    target->GetCheckType() == TARGET_CHECK_ENEMY)
                    return CasterArea{ effect.CalcRadius(caster), category == TARGET_SELECT_CATEGORY_CONE };
            }

            if (effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA && effect.TargetA.GetReferenceType() == TARGET_REFERENCE_TYPE_CASTER)
                return CasterArea{ effect.CalcRadius(caster), false };

            if (followTrigger && effect.TriggerSpell)
                if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    if (Optional<CasterArea> area = GetCasterArea(triggered, caster, false))
                        return area;
        }

        return {};
    }

    // Like Spell::SelectImplicitConeTargets.
    constexpr float CONE_ANGLE = float(M_PI) / 2;

    bool InArea(Player* caster, Unit* unit, CasterArea const& area)
    {
        return caster->IsWithinDistInMap(unit, area.Radius) && (!area.Cone || caster->HasInArc(CONE_ANGLE, unit));
    }

    // An enemy in combat (or an enemy player) the spell would hit.
    class EnemyInAreaCheck
    {
    public:
        EnemyInAreaCheck(Player* caster, CasterArea const& area) : _caster(caster), _area(area) { }

        bool operator()(Unit* unit) const
        {
            return unit->IsAlive() && (unit->IsInCombat() || unit->IsPlayer()) && !unit->IsTotem() &&
                _caster->IsValidAttackTarget(unit) && InArea(_caster, unit, _area);
        }

    private:
        Player* _caster;
        CasterArea _area;
    };
}

SpellCastResult RotationApl::Cast(Player* player, AplChoice const& choice)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(choice.SpellId);
    // A normal cast, as if the player had pressed the button; the item makes it the item's on-use spell.
    return player->CastSpell(MakeTargets(spellInfo, choice.Target), spellInfo->Id,
        CastSpellExtraArgs(TRIGGERED_NONE).SetCastItem(choice.CastItem));
}

SpellCastResult RotationApl::CanCast(Player* player, SpellInfo const* spellInfo, Unit* target, Item* castItem)
{
    // Cheap checks first; Spell::CheckCast below repeats some of them.
    if (!player->GetSpellHistory()->IsReady(spellInfo, castItem ? castItem->GetEntry() : 0) ||
        player->GetSpellHistory()->HasGlobalCooldown(spellInfo))
        return SPELL_FAILED_NOT_READY;

    // Heroic Strike and Cleave stay queued until the next swing; queueing again would only replace them.
    if ((spellInfo->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING) ||
        spellInfo->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_2)) && player->GetCurrentSpell(CURRENT_MELEE_SPELL))
        return SPELL_FAILED_SPELL_IN_PROGRESS;

    if (player->isMoving() && (spellInfo->IsChanneled() || spellInfo->CalcCastTime() > 0))
        return SPELL_FAILED_MOVING;

    // CheckCast lets a spell land on an immune target (Divine Shield, Ice Block, Hand of
    // Protection against physical abilities) and do nothing.
    if (target != player && target->IsImmunedToSpell(spellInfo, player))
        return SPELL_FAILED_IMMUNE;

    // Crowd control into a full set of diminishing returns does nothing.
    if (target != player && GetDiminishingLevel(target, spellInfo) >= DIMINISHING_LEVEL_IMMUNE)
        return SPELL_FAILED_IMMUNE;

    // A spell without a target is cast wherever the caster stands, so it only goes out when it would hit:
    // its target in reach (and in front, for cones), or with no hostile target, any enemy in reach.
    if (Optional<CasterArea> area = GetCasterArea(spellInfo, player))
    {
        if (target != player && player->IsValidAttackTarget(target))
        {
            if (!InArea(player, target, *area))
                return SPELL_FAILED_OUT_OF_RANGE;
        }
        else
        {
            Unit* enemy = nullptr;
            EnemyInAreaCheck check(player, *area);
            Trinity::UnitSearcher<EnemyInAreaCheck> searcher(player, enemy, check);
            // Searched a bit wider: large creatures are in reach from further away (their combat reach).
            Cell::VisitAllObjects(player, searcher, area->Radius + 15.0f);
            if (!enemy)
                return SPELL_FAILED_OUT_OF_RANGE;
        }
    }

    // CheckCast only knows the power cost once the spell is prepared, so check it here. Passing no
    // Spell keeps spell mods from being registered, so proc charges are not touched.
    if (!castItem && spellInfo->PowerType < MAX_POWERS && spellInfo->PowerType != POWER_RUNE &&
        !player->GetCommandStatus(CHEAT_POWER))
    {
        Powers powerType = Powers(spellInfo->PowerType);
        int32 cost = spellInfo->CalcPowerCost(player, spellInfo->GetSchoolMask());
        if (int32(player->GetPower(powerType)) < cost)
            return SPELL_FAILED_NO_POWER;
    }

    // Covers stance, aura states (Overpower, Execute, Victory Rush), runes, reagents, range, line
    // of sight and, for players, facing.
    Spell spell(player, spellInfo, TRIGGERED_NONE);
    spell.m_CastItem = castItem;
    if (castItem)
    {
        // Set like Spell::prepare does, so CheckCast sees the item's cooldowns.
        spell.m_castItemGUID = castItem->GetGUID();
        spell.m_castItemEntry = castItem->GetEntry();
    }
    spell.m_targets = MakeTargets(spellInfo, target);
    return spell.CheckCast(true);
}

Optional<AplChoice> RotationApl::Evaluate(Player* player, AplProfile const& profile, RotationState const& state,
    RotationSettings const& settings)
{
    AplContext context{ player, state, settings };

    // The units an entry may be tried on, as snapshot and live unit, in the order to try them.
    struct Candidate
    {
        UnitSnapshot const* Snapshot;
        Unit* Live;
    };

    std::vector<Candidate> selected;
    if (state.Target && state.Target->Hostile)
        if (Unit* target = player->GetSelectedUnit())
            selected.push_back({ &*state.Target, target });

    std::vector<Candidate> self = { { &state.Self, player } };

    std::vector<Candidate> enemies;
    std::vector<Candidate> enemyPlayers;
    for (UnitSnapshot const& enemy : state.EnemyUnits)
    {
        if (Unit* unit = ObjectAccessor::GetUnit(*player, enemy.Guid))
        {
            enemies.push_back({ &enemy, unit });
            if (enemy.IsPlayer)
                enemyPlayers.push_back({ &enemy, unit });
        }
    }

    std::vector<Candidate> party;
    for (UnitSnapshot const& member : state.Party)
        if (Unit* unit = member.Guid == player->GetGUID() ? player : ObjectAccessor::GetUnit(*player, member.Guid))
            party.push_back({ &member, unit });

    std::vector<Candidate> pet;
    if (state.Pet)
        if (Unit* unit = player->GetGuardianPet())
            pet.push_back({ &*state.Pet, unit });

    auto candidatesFor = [&](AplTarget target) -> std::vector<Candidate> const&
    {
        switch (target)
        {
            case AplTarget::Self:         return self;
            case AplTarget::Target:       return selected;
            case AplTarget::EnemyPlayers: return enemyPlayers;
            case AplTarget::Enemies:      return enemies;
            case AplTarget::Party:        return party;
            case AplTarget::Pet:          return pet;
        }

        return selected;
    };

    for (std::string_view listName : ListOrder(profile, settings))
    {
        auto list = profile.Lists.find(listName);
        if (list == profile.Lists.end())
            continue;

        bool isInterrupts = listName == "interrupts";

        for (std::size_t i = 0; i < list->second.size(); ++i)
        {
            AplEntry const& entry = list->second[i];

            uint32 spellId = 0;
            Item* castItem = nullptr;
            if (entry.Spell)
                spellId = entry.Spell->KnownRank(player);
            else
                castItem = entry.UseItem->Find(player, spellId);

            SpellInfo const* spellInfo = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            if (!spellInfo)
                continue;

            std::vector<Candidate> const& candidates = candidatesFor(entry.Target);

            for (Candidate const& candidate : candidates)
            {
                if (isInterrupts)
                {
                    // Self entries (a stance swap for a kick) wait on the selected target's cast.
                    ObjectGuid caster = entry.Target == AplTarget::Self ?
                        (state.Target ? state.Target->Guid : ObjectGuid::Empty) : candidate.Snapshot->Guid;
                    if (!settings.CanReactTo(caster))
                        continue;
                }

                context.Unit = candidate.Snapshot;
                if (entry.Condition && entry.Condition(context) == 0.0)
                    continue;

                if (CanCast(player, spellInfo, candidate.Live, castItem) != SPELL_CAST_OK)
                    continue;

                AplChoice choice;
                choice.ListName = list->first;
                choice.Entry = &entry;
                choice.Index = i;
                choice.SpellId = spellId;
                choice.CastItem = castItem;
                choice.Target = candidate.Live;
                return choice;
            }
        }
    }

    return {};
}
