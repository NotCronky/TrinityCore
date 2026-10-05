/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationState.h"

#include "CellImpl.h"
#include "Creature.h"
#include "Group.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellHistory.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "ThreatManager.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

static_assert(RUNE_TYPE_COUNT == NUM_RUNE_TYPES, "RUNE_TYPE_COUNT must match NUM_RUNE_TYPES");

namespace
{
    constexpr uint32 STANDARD_GCD_CATEGORY = 133;

    // Loss-of-control mechanics: IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK without the slows.
    constexpr uint64 CROWD_CONTROL_MECHANIC_MASK = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK &
        ~((1ULL << MECHANIC_SNARE) | (1ULL << MECHANIC_DAZE));
    constexpr uint32 STANDARD_GCD_MS = 1500;

    // Any spell on the standard global cooldown category. The GCD is tracked per category, so this
    // reads the remaining GCD shared by every spell in that category.
    SpellInfo const* GetGcdReferenceSpell()
    {
        static SpellInfo const* spell = []() -> SpellInfo const*
        {
            for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
                if (info && info->StartRecoveryCategory == STANDARD_GCD_CATEGORY &&
                    info->StartRecoveryTime == STANDARD_GCD_MS)
                    return info;
            }

            return nullptr;
        }();

        return spell;
    }

    // Attackable units the rotation should count: anything already fighting, plus enemy players.
    class EnemyCheck
    {
    public:
        EnemyCheck(Player const* player, float range) : _player(player), _range(range) {}

        bool operator()(Unit* unit) const
        {
            if (!unit->IsAlive() || unit->IsTotem() || unit->IsCritter())
                return false;

            if (!unit->IsInCombat() && !unit->IsPlayer())
                return false;

            return _player->IsWithinDistInMap(unit, _range) && _player->IsValidAttackTarget(unit) &&
                _player->CanSeeOrDetect(unit);
        }

    private:
        Player const* _player;
        float _range;
    };

    uint32 DisplayPower(Powers type, uint32 value)
    {
        // Rage and runic power are stored in tenths of what the client shows.
        return (type == POWER_RAGE || type == POWER_RUNIC_POWER) ? value / 10 : value;
    }

    void FillUnit(UnitSnapshot& out, Unit* unit, Player* player, std::unordered_set<ObjectGuid> const& party)
    {
        out.Guid = unit->GetGUID();
        out.Name = unit->GetName();
        out.HealthPct = unit->GetHealthPct();
        out.Level = unit->GetLevel();
        out.Class = unit->GetClass();
        out.IsPlayer = unit->IsPlayer();

        if (Creature* creature = unit->ToCreature())
            out.IsBoss = creature->isWorldBoss() || creature->IsDungeonBoss();

        out.Stunned = unit->HasUnitState(UNIT_STATE_STUNNED);
        out.Rooted = unit->HasUnitState(UNIT_STATE_ROOT);
        out.Feared = unit->HasUnitState(UNIT_STATE_FLEEING);
        out.Confused = unit->HasUnitState(UNIT_STATE_CONFUSED);
        out.Silenced = unit->HasUnitFlag(UNIT_FLAG_SILENCED);
        out.Disarmed = unit->HasUnitFlag(UNIT_FLAG_DISARMED);

        if (Unit* victim = unit->GetVictim())
        {
            out.Victim = victim->GetGUID();
            out.TargetingMe = victim == player;
            out.TargetingParty = victim != player && party.count(out.Victim);
        }

        if (unit != player)
        {
            out.Pos = unit->GetPosition();
            out.Hostile = player->IsValidAttackTarget(unit);
            out.Distance = player->GetDistance(unit);
            out.InMelee = player->IsWithinMeleeRange(unit);
            out.InLos = player->IsWithinLOSInMap(unit);
            out.Facing = player->HasInArc(float(M_PI), unit);
            out.Behind = unit->isInBack(player);

            if (unit->CanHaveThreatList())
            {
                ThreatManager& threat = unit->GetThreatManager();
                Unit* victim = threat.GetCurrentVictim();
                float top = victim ? threat.GetThreat(victim) : 0.0f;
                out.ThreatPct = top > 0.0f ? 100.0f * threat.GetThreat(player) / top : 0.0f;
            }
        }

        for (auto const& [spellId, application] : unit->GetAppliedAuras())
        {
            Aura const* aura = application->GetBase();
            if (aura->GetSpellInfo()->IsPassive())
                continue;

            AuraSnapshot snapshot;
            snapshot.SpellId = spellId;
            snapshot.FirstRankId = sSpellMgr->GetFirstSpellInChain(spellId);
            snapshot.RemainingMs = aura->GetDuration();
            snapshot.Stacks = aura->GetStackAmount();
            snapshot.Charges = aura->GetCharges();
            snapshot.Mine = aura->GetCasterGUID() == player->GetGUID();
            snapshot.Positive = application->IsPositive();
            snapshot.DispelType = uint8(aura->GetSpellInfo()->Dispel);
            snapshot.IsCrowdControl = !snapshot.Positive &&
                (aura->GetSpellInfo()->GetAllEffectsMechanicMask() & CROWD_CONTROL_MECHANIC_MASK) != 0;
            out.Auras.push_back(snapshot);
        }

        for (CurrentSpellTypes type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
        {
            Spell* spell = unit->GetCurrentSpell(type);
            if (!spell)
                continue;

            // Matches the checks Spell::EffectInterruptCast makes, minus the target's immunities.
            SpellInfo const* info = spell->GetSpellInfo();
            bool started = spell->getState() == SPELL_STATE_CASTING ||
                (spell->getState() == SPELL_STATE_PREPARING && spell->GetCastTime() > 0);
            bool kickable = type == CURRENT_GENERIC_SPELL ?
                (info->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) :
                (info->ChannelInterruptFlags & CHANNEL_INTERRUPT_FLAG_INTERRUPT);

            CastSnapshot cast;
            cast.SpellId = info->Id;
            cast.RemainingMs = spell->GetRemainingCastTime();
            cast.Channeled = type == CURRENT_CHANNELED_SPELL;
            cast.Interruptible = started && kickable && info->PreventionType == SPELL_PREVENTION_TYPE_SILENCE;
            cast.IsHeal = info->HasEffect(SPELL_EFFECT_HEAL) || info->HasEffect(SPELL_EFFECT_HEAL_PCT) ||
                info->HasEffect(SPELL_EFFECT_HEAL_MAX_HEALTH) || info->HasAura(SPELL_AURA_PERIODIC_HEAL);
            cast.IsCrowdControl = (info->GetAllEffectsMechanicMask() & CROWD_CONTROL_MECHANIC_MASK) != 0;
            out.Cast = cast;
            break;
        }
    }

    std::string SpellName(uint32 spellId, LocaleConstant locale)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->SpellName[locale] || !*info->SpellName[locale])
            return Trinity::StringFormat("#{}", spellId);

        return info->SpellName[locale];
    }

    std::string FormatSeconds(int32 ms)
    {
        return ms < 0 ? "-" : Trinity::StringFormat("{:.1f}s", ms / 1000.0f);
    }

    // Long chat lines get cut off by the client, so lists are packed into lines of at most this many characters.
    constexpr std::size_t MAX_CHAT_LINE = 200;

    void AppendList(std::vector<std::string>& lines, std::string const& label, std::vector<std::string> const& items)
    {
        std::string line = label + ": ";
        if (items.empty())
        {
            lines.push_back(line + "none");
            return;
        }

        bool first = true;
        for (std::string const& item : items)
        {
            if (!first && line.size() + item.size() + 2 > MAX_CHAT_LINE)
            {
                lines.push_back(line);
                line = "  ";
                first = true;
            }

            line += first ? item : ", " + item;
            first = false;
        }

        lines.push_back(line);
    }

    std::vector<std::string> DescribeAuras(std::vector<AuraSnapshot> const& auras, LocaleConstant locale)
    {
        std::vector<std::string> items;
        for (AuraSnapshot const& aura : auras)
        {
            std::string item = SpellName(aura.SpellId, locale);
            if (aura.Mine)
                item += "*";

            if (aura.Stacks > 1)
                item += Trinity::StringFormat(" x{}", aura.Stacks);

            if (aura.RemainingMs >= 0)
                item += " " + FormatSeconds(aura.RemainingMs);

            items.push_back(item);
        }

        return items;
    }

    std::string DescribeControl(UnitSnapshot const& unit)
    {
        std::string out;
        auto add = [&out](bool active, char const* name)
        {
            if (!active)
                return;

            if (!out.empty())
                out += " ";

            out += name;
        };

        add(unit.Stunned, "stunned");
        add(unit.Rooted, "rooted");
        add(unit.Feared, "feared");
        add(unit.Confused, "confused");
        add(unit.Silenced, "silenced");
        add(unit.Disarmed, "disarmed");
        return out.empty() ? "none" : out;
    }
}

AuraSnapshot const* UnitSnapshot::FindAura(uint32 spellId, bool mineOnly) const
{
    uint32 firstRankId = sSpellMgr->GetFirstSpellInChain(spellId);
    for (AuraSnapshot const& aura : Auras)
        if (aura.FirstRankId == firstRankId && (aura.Mine || !mineOnly))
            return &aura;

    return nullptr;
}

int32 UnitSnapshot::CrowdControlRemainingMs() const
{
    int32 longest = 0;
    for (AuraSnapshot const& aura : Auras)
        if (aura.IsCrowdControl)
            longest = aura.RemainingMs < 0 ? std::numeric_limits<int32>::max() : std::max(longest, aura.RemainingMs);

    return longest;
}

uint32 UnitSnapshot::Dispellable(uint8 dispelType) const
{
    uint32 count = 0;
    for (AuraSnapshot const& aura : Auras)
        if (aura.DispelType == dispelType && aura.Positive == Hostile)
            ++count;

    return count;
}

uint32 RotationState::EnemiesNear(Position const& center, float radius) const
{
    uint32 count = 0;
    for (EnemySnapshot const& enemy : Enemies)
        if (center.GetExactDist(&enemy.Pos) <= radius)
            ++count;

    return count;
}

bool RotationStateBuilder::IsGroupInCombat(Player* player)
{
    if (player->IsInCombat())
        return true;

    Group* group = player->GetGroup();
    if (!group)
        return false;

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member && member != player && member->IsInWorld() && member->IsAlive() && member->IsInCombat() &&
            member->IsInMap(player) && player->IsWithinDistInMap(member, ENEMY_SEARCH_RADIUS))
            return true;
    }

    return false;
}

RotationState RotationStateBuilder::Build(Player* player)
{
    RotationState state;
    state.TakenAtMs = getMSTime();

    state.InCombat = IsGroupInCombat(player);

    // Group members first: enemies need to know who belongs to the player's group.
    std::vector<Player*> members;
    if (Group* group = player->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member != player && member->IsInWorld() && member->IsAlive() &&
                member->IsInMap(player) && player->IsWithinDistInMap(member, ENEMY_SEARCH_RADIUS))
                members.push_back(member);
        }
    }

    std::unordered_set<ObjectGuid> partyGuids = { player->GetGUID() };
    for (Player* member : members)
        partyGuids.insert(member->GetGUID());

    FillUnit(state.Self, player, player, partyGuids);
    state.PowerType = player->GetPowerType();
    state.Power = DisplayPower(state.PowerType, player->GetPower(state.PowerType));
    state.MaxPower = DisplayPower(state.PowerType, player->GetMaxPower(state.PowerType));
    state.ComboPoints = player->GetComboPoints();
    state.Form = player->GetShapeshiftForm();
    state.Moving = player->isMoving();

    if (SpellInfo const* gcdSpell = GetGcdReferenceSpell())
        state.GcdRemainingMs = player->GetSpellHistory()->GetRemainingGlobalCooldown(gcdSpell);

    state.AutoRepeat = player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;

    auto enchanted = [player](uint8 slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item && item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0;
    };

    state.MainHandEnchanted = enchanted(EQUIPMENT_SLOT_MAINHAND);
    state.OffHandEnchanted = enchanted(EQUIPMENT_SLOT_OFFHAND);

    for (uint8 i = 0; i < state.Totems.size(); ++i)
    {
        ObjectGuid guid = player->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE + i];
        Creature* totem = guid.IsEmpty() ? nullptr : ObjectAccessor::GetCreature(*player, guid);
        state.Totems[i] = totem && totem->IsAlive() && player->IsWithinDistInMap(totem, TOTEM_RANGE);
    }

    if (player->GetClass() == CLASS_DEATH_KNIGHT)
    {
        for (uint8 i = 0; i < MAX_RUNES; ++i)
            if (!player->GetRuneCooldown(i))
                ++state.ReadyRunes[player->GetCurrentRune(i)];
    }

    if (Unit* target = player->GetSelectedUnit())
    {
        state.Target.emplace();
        FillUnit(*state.Target, target, player, partyGuids);
    }

    if (Guardian* pet = player->GetGuardianPet())
    {
        if (pet->IsAlive() && pet->IsInWorld())
        {
            state.Pet.emplace();
            FillUnit(*state.Pet, pet, player, partyGuids);
        }
    }

    std::list<Unit*> enemies;
    EnemyCheck check(player, ENEMY_SEARCH_RADIUS);
    Trinity::UnitListSearcher<EnemyCheck> searcher(player, enemies, check);
    Cell::VisitAllObjects(player, searcher, ENEMY_SEARCH_RADIUS);

    state.Enemies.reserve(enemies.size());
    for (Unit* unit : enemies)
    {
        EnemySnapshot enemy;
        enemy.Guid = unit->GetGUID();
        enemy.Pos = unit->GetPosition();
        enemy.HealthPct = unit->GetHealthPct();
        enemy.IsPlayer = unit->IsPlayer();
        enemy.BreakableCc = unit->HasBreakableByDamageCrowdControlAura();
        state.Enemies.push_back(enemy);

        state.EnemyUnits.emplace_back();
        FillUnit(state.EnemyUnits.back(), unit, player, partyGuids);
    }

    state.Party.push_back(state.Self);
    for (Player* member : members)
    {
        state.Party.emplace_back();
        FillUnit(state.Party.back(), member, player, partyGuids);
    }

    // Attackers, from the enemies' victims.
    std::unordered_map<ObjectGuid, uint32> attackers;
    for (UnitSnapshot const& enemy : state.EnemyUnits)
        if (!enemy.Victim.IsEmpty())
            ++attackers[enemy.Victim];

    auto countAttackers = [&attackers](UnitSnapshot& unit)
    {
        auto itr = attackers.find(unit.Guid);
        unit.Attackers = itr != attackers.end() ? itr->second : 0;
    };

    countAttackers(state.Self);
    if (state.Target)
        countAttackers(*state.Target);

    if (state.Pet)
        countAttackers(*state.Pet);

    uint32 mostAttackers = 0;
    for (UnitSnapshot& member : state.Party)
    {
        countAttackers(member);
        mostAttackers = std::max(mostAttackers, member.Attackers);
    }

    for (UnitSnapshot& member : state.Party)
        member.MostAttacked = mostAttackers && member.Attackers == mostAttackers;

    auto byHealth = [](UnitSnapshot const& a, UnitSnapshot const& b) { return a.HealthPct < b.HealthPct; };
    std::sort(state.EnemyUnits.begin(), state.EnemyUnits.end(), byHealth);
    std::sort(state.Party.begin(), state.Party.end(), byHealth);

    return state;
}

std::string RotationStateBuilder::Summarize(RotationState const& state)
{
    std::string out = Trinity::StringFormat("hp {:.0f}% power {}/{} gcd {}ms{}", state.Self.HealthPct, state.Power,
        state.MaxPower, state.GcdRemainingMs, state.Moving ? " moving" : "");

    if (state.Target)
        out += Trinity::StringFormat(" | target {:.0f}% {:.1f}yd{}{}{}", state.Target->HealthPct,
            state.Target->Distance, state.Target->InMelee ? " melee" : "", state.Target->InLos ? "" : " no-los",
            state.Target->Cast ? " casting" : "");
    else
        out += " | no target";

    out += Trinity::StringFormat(" | enemies {} | party {}", state.Enemies.size(), state.Party.size());
    return out;
}

std::vector<std::string> RotationStateBuilder::Describe(Player* player, RotationState const& state)
{
    LocaleConstant locale = player->GetSession()->GetSessionDbcLocale();
    std::vector<std::string> lines;

    lines.push_back(Trinity::StringFormat("Self: {:.0f}% hp, power {}/{} ({:.0f}%), combo {}, form {}, gcd {}ms{}",
        state.Self.HealthPct, state.Power, state.MaxPower, state.PowerPct(), state.ComboPoints, uint32(state.Form),
        state.GcdRemainingMs, state.Moving ? ", moving" : ""));
    lines.push_back("Self control: " + DescribeControl(state.Self));
    AppendList(lines, "Self auras (* = mine)", DescribeAuras(state.Self.Auras, locale));

    std::vector<std::string> cooldowns;
    for (auto const& [spellId, playerSpell] : player->GetSpellMap())
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (playerSpell.state == PLAYERSPELL_REMOVED || !info || info->IsPassive())
            continue;

        if (uint32 remaining = player->GetSpellHistory()->GetRemainingCooldown(info))
            cooldowns.push_back(SpellName(spellId, locale) + " " + FormatSeconds(remaining));
    }

    AppendList(lines, "Cooldowns", cooldowns);

    if (!state.Target)
        lines.push_back("Target: none");
    else
    {
        UnitSnapshot const& target = *state.Target;
        lines.push_back(Trinity::StringFormat("Target: {} (level {}{}{}), {:.0f}% hp, {}",
            target.Name, target.Level, target.IsPlayer ? ", player" : "", target.IsBoss ? ", boss" : "",
            target.HealthPct, target.Hostile ? "hostile" : "not attackable"));
        lines.push_back(Trinity::StringFormat("Target position: {:.1f}yd{}{}{}{}", target.Distance,
            target.InMelee ? ", in melee" : "", target.InLos ? ", in LoS" : ", no LoS",
            target.Facing ? ", facing" : ", not facing", target.Behind ? ", behind" : ""));

        if (target.ThreatPct >= 0.0f)
            lines.push_back(Trinity::StringFormat("Threat: {:.0f}% of the current victim's", target.ThreatPct));

        if (target.Cast)
            lines.push_back(Trinity::StringFormat("Target casting: {} {} left{}{}",
                SpellName(target.Cast->SpellId, locale), FormatSeconds(target.Cast->RemainingMs),
                target.Cast->Channeled ? ", channel" : "",
                target.Cast->Interruptible ? ", interruptible" : ", not interruptible") +
                (target.Cast->IsHeal ? ", heal" : "") + (target.Cast->IsCrowdControl ? ", crowd control" : ""));

        lines.push_back("Target control: " + DescribeControl(target));
        AppendList(lines, "Target auras (* = mine)", DescribeAuras(target.Auras, locale));
    }

    uint32 players = 0;
    uint32 crowdControlled = 0;
    for (EnemySnapshot const& enemy : state.Enemies)
    {
        players += enemy.IsPlayer;
        crowdControlled += enemy.BreakableCc;
    }

    std::string near = Trinity::StringFormat("Enemies within {:.0f}yd: {} ({} players, {} in breakable CC); "
        "within 8yd of me: {}", ENEMY_SEARCH_RADIUS, state.Enemies.size(), players, crowdControlled,
        state.EnemiesNear(player->GetPosition(), 8.0f));

    if (state.Target)
        near += Trinity::StringFormat(", within 8yd of target: {}", state.EnemiesNear(state.Target->Pos, 8.0f));

    lines.push_back(near);

    std::vector<std::string> enemyPlayers;
    for (UnitSnapshot const& enemy : state.EnemyUnits)
    {
        if (!enemy.IsPlayer)
            continue;

        std::string item = Trinity::StringFormat("{} {:.0f}% {:.0f}yd", enemy.Name, enemy.HealthPct, enemy.Distance);
        if (enemy.Cast)
            item += " casting " + SpellName(enemy.Cast->SpellId, locale);

        enemyPlayers.push_back(item);
    }

    AppendList(lines, "Enemy players", enemyPlayers);

    std::vector<std::string> party;
    for (UnitSnapshot const& member : state.Party)
    {
        std::string item = Trinity::StringFormat("{} {:.0f}%", member.Name, member.HealthPct);
        if (member.Attackers)
            item += Trinity::StringFormat(" ({} attacking{})", member.Attackers, member.MostAttacked ? ", most" : "");

        party.push_back(item);
    }

    AppendList(lines, "Party", party);
    return lines;
}
