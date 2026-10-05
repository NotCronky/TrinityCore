/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationBotMgr.h"
#include "RotationApl.h"
#include "RotationProfiles.h"
#include "RotationState.h"

#include "Random.h"

#include "Chat.h"
#include "Config.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>

namespace
{
    constexpr uint32 MIN_TICK_INTERVAL_MS = 50;
    constexpr uint32 MAX_TICK_INTERVAL_MS = 1000;

    char const* ModeName(RotationMode mode)
    {
        switch (mode)
        {
            case RotationMode::Auto: return "auto";
            case RotationMode::Pve:  return "pve";
            case RotationMode::Pvp:  return "pvp";
        }

        return "unknown";
    }

    Optional<RotationMode> ParseMode(std::string_view text)
    {
        for (RotationMode mode : { RotationMode::Auto, RotationMode::Pve, RotationMode::Pvp })
            if (text == ModeName(mode))
                return mode;

        return {};
    }

    char const* AoeName(AoeMode mode)
    {
        switch (mode)
        {
            case AoeMode::Auto: return "auto";
            case AoeMode::On:   return "on";
            case AoeMode::Off:  return "off";
        }

        return "unknown";
    }

    Optional<AoeMode> ParseAoe(std::string_view text)
    {
        for (AoeMode mode : { AoeMode::Auto, AoeMode::On, AoeMode::Off })
            if (text == AoeName(mode))
                return mode;

        return {};
    }
}

char const* TickGateName(TickGate gate)
{
    switch (gate)
    {
        case TickGate::Ready:       return "ready";
        case TickGate::Dead:        return "dead";
        case TickGate::Mounted:     return "mounted";
        case TickGate::InFlight:    return "in flight";
        case TickGate::OnVehicle:   return "on a vehicle";
        case TickGate::Controlled:  return "controlled";
        case TickGate::Casting:     return "casting";
        case TickGate::Sitting:     return "sitting";
        case TickGate::OutOfCombat: return "out of combat";
    }

    return "unknown";
}

RotationBotMgr& RotationBotMgr::Instance()
{
    static RotationBotMgr instance;
    return instance;
}

void RotationBotMgr::LoadConfig()
{
    RotationBotConfig config;
    config.Enabled = sConfigMgr->GetBoolDefault("RotationBot.Enable", true);
    config.MinSecurity = uint32(std::max(0, sConfigMgr->GetIntDefault("RotationBot.MinSecurity", 0)));
    config.ProfileDir = sConfigMgr->GetStringDefault("RotationBot.ProfileDir", "");

    std::string mode = sConfigMgr->GetStringDefault("RotationBot.DefaultMode", "auto");
    if (Optional<RotationMode> parsed = ParseMode(mode))
        config.DefaultMode = *parsed;
    else
        TC_LOG_ERROR("module", "mod-rotation-bot: RotationBot.DefaultMode '{}' is not auto, pve or pvp; using auto.",
            mode);

    config.TickIntervalMs = uint32(std::clamp(sConfigMgr->GetIntDefault("RotationBot.TickIntervalMs", 120),
        int32(MIN_TICK_INTERVAL_MS), int32(MAX_TICK_INTERVAL_MS)));

    config.InterruptDelayMinMs = uint32(std::max(0, sConfigMgr->GetIntDefault("RotationBot.InterruptDelayMinMs", 150)));
    config.InterruptDelayMaxMs = uint32(std::max(0, sConfigMgr->GetIntDefault("RotationBot.InterruptDelayMaxMs", 400)));
    if (config.InterruptDelayMaxMs < config.InterruptDelayMinMs)
    {
        TC_LOG_ERROR("module", "mod-rotation-bot: RotationBot.InterruptDelayMaxMs is below InterruptDelayMinMs; "
            "using the minimum for both.");
        config.InterruptDelayMaxMs = config.InterruptDelayMinMs;
    }

    std::string accounts = sConfigMgr->GetStringDefault("RotationBot.Accounts", "");
    for (std::string_view token : Trinity::Tokenize(accounts, ',', false))
    {
        if (Optional<uint32> id = Trinity::StringTo<uint32>(token))
            config.Accounts.insert(*id);
        else
            TC_LOG_ERROR("module", "mod-rotation-bot: RotationBot.Accounts has an invalid account id '{}'.", token);
    }

    _config = std::move(config);
}

bool RotationBotMgr::IsAllowed(Player* player) const
{
    WorldSession* session = player->GetSession();
    if (uint32(session->GetSecurity()) < _config.MinSecurity)
        return false;

    return _config.Accounts.empty() || _config.Accounts.count(session->GetAccountId());
}

char const* RotationBotMgr::AccessError(Player* player) const
{
    if (!_config.Enabled)
        return "Rotation bot is disabled on this server.";

    if (!IsAllowed(player))
        return "You are not allowed to use the rotation bot.";

    return nullptr;
}

RotationBotSession* RotationBotMgr::FindSession(Player* player) const
{
    std::shared_lock<std::shared_mutex> lock(_sessionsLock);
    auto itr = _sessions.find(player->GetGUID());
    return itr != _sessions.end() ? itr->second.get() : nullptr;
}

RotationBotSession& RotationBotMgr::GetOrCreateSession(Player* player) const
{
    if (RotationBotSession* session = FindSession(player))
        return *session;

    auto session = std::make_unique<RotationBotSession>();
    session->Mode = _config.DefaultMode;

    std::unique_lock<std::shared_mutex> lock(_sessionsLock);
    return *_sessions.try_emplace(player->GetGUID(), std::move(session)).first->second;
}

void RotationBotMgr::EraseSession(Player* player)
{
    std::unique_lock<std::shared_mutex> lock(_sessionsLock);
    _sessions.erase(player->GetGUID());
}

void RotationBotMgr::OnPlayerLogout(Player* player)
{
    EraseSession(player);
}

std::string RotationBotMgr::SetMode(Player* player, Optional<std::string> const& value)
{
    if (char const* error = AccessError(player))
        return error;

    RotationBotSession& session = GetOrCreateSession(player);
    if (value)
    {
        Optional<RotationMode> mode = ParseMode(*value);
        if (!mode)
            return "Usage: .rot mode auto|pve|pvp";

        session.Mode = *mode;
    }

    return Trinity::StringFormat("Rotation mode: {}.", ModeName(session.Mode));
}

std::string RotationBotMgr::SetAoe(Player* player, Optional<std::string> const& value)
{
    if (char const* error = AccessError(player))
        return error;

    RotationBotSession& session = GetOrCreateSession(player);
    if (value)
    {
        Optional<AoeMode> mode = ParseAoe(*value);
        if (!mode)
            return "Usage: .rot aoe auto|on|off";

        session.Aoe = *mode;
    }

    return Trinity::StringFormat("Rotation AoE: {}.", AoeName(session.Aoe));
}

std::string RotationBotMgr::SetBurst(Player* player, Optional<bool> value)
{
    if (char const* error = AccessError(player))
        return error;

    RotationBotSession& session = GetOrCreateSession(player);
    if (value)
        session.Burst = *value;

    return Trinity::StringFormat("Rotation burst: {}.", session.Burst ? "on" : "off");
}

RotationSettings RotationBotMgr::ResolveSettings(Player* player, RotationBotSession& session,
    AplProfile const& profile, RotationState const& state, bool manual) const
{
    RotationSettings settings;
    settings.Burst = session.Burst;
    settings.Manual = manual;
    settings.InCombat = state.InCombat;

    // Reaction delay: an interrupt may only go out a random time after a cast was first seen.
    // The same caster casting the same spell again counts as the same cast until a tick sees it
    // not casting.
    std::unordered_map<ObjectGuid, RotationBotSession::Reaction> reactions;
    auto track = [&](UnitSnapshot const& caster)
    {
        if (!caster.Cast || reactions.count(caster.Guid))
            return;

        RotationBotSession::Reaction reaction;
        auto old = session.Reactions.find(caster.Guid);
        if (old != session.Reactions.end() && old->second.SpellId == caster.Cast->SpellId)
            reaction = old->second;
        else
        {
            reaction.SpellId = caster.Cast->SpellId;
            reaction.SeenAtMs = state.TakenAtMs;
            reaction.DelayMs = urand(_config.InterruptDelayMinMs, _config.InterruptDelayMaxMs);
        }

        if (getMSTimeDiff(reaction.SeenAtMs, state.TakenAtMs) >= reaction.DelayMs)
            settings.Reacted.insert(caster.Guid);

        reactions[caster.Guid] = reaction;
    };

    if (state.Target)
        track(*state.Target);

    for (UnitSnapshot const& enemy : state.EnemyUnits)
        track(enemy);

    session.Reactions = std::move(reactions);

    switch (session.Mode)
    {
        case RotationMode::Pve:
            settings.Pvp = false;
            break;
        case RotationMode::Pvp:
            settings.Pvp = true;
            break;
        case RotationMode::Auto:
            settings.Pvp = (state.Target && state.Target->IsPlayer && state.Target->Hostile) ||
                player->InBattleground() || player->InArena();
            break;
    }

    switch (session.Aoe)
    {
        case AoeMode::On:
            settings.Aoe = true;
            break;
        case AoeMode::Off:
            settings.Aoe = false;
            break;
        case AoeMode::Auto:
            settings.Aoe = RotationApl::WantsAoe(player, profile, state);
            break;
    }

    return settings;
}

std::vector<std::string> RotationBotMgr::LoadProfiles()
{
    RotationProfiles::LoadResult result = RotationProfiles::Load(_config.ProfileDir);

    std::vector<std::string> lines;
    lines.push_back(Trinity::StringFormat("Loaded {} profile{} from {}.", result.Loaded, result.Loaded == 1 ? "" : "s",
        result.Directory));

    if (result.Kept)
        lines.push_back(Trinity::StringFormat("Kept the previous version of {} file{} with errors.", result.Kept,
            result.Kept == 1 ? "" : "s"));

    for (std::string const& error : result.Errors)
        lines.push_back("Error: " + error);

    for (std::string const& line : lines)
    {
        if (result.Errors.empty())
            TC_LOG_INFO("module", "mod-rotation-bot: {}", line);
        else
            TC_LOG_ERROR("module", "mod-rotation-bot: {}", line);
    }

    return lines;
}

std::vector<std::string> RotationBotMgr::Reload(Player* player)
{
    if (char const* error = AccessError(player))
        return { error };

    return LoadProfiles();
}

std::string RotationBotMgr::Enable(Player* player)
{
    if (char const* error = AccessError(player))
        return error;

    RotationBotSession* session = &GetOrCreateSession(player);
    if (session->Active)
        return "Rotation bot is already on.";

    session->Active = true;
    session->TickTimerMs = 0;
    session->TickCount = 0;
    session->LastGate = TickGate::Ready;
    session->LastTrace.clear();

    TC_LOG_INFO("module", "mod-rotation-bot: on for {}.", player->GetName());
    return "Rotation bot on.";
}

std::string RotationBotMgr::Disable(Player* player)
{
    RotationBotSession* session = FindSession(player);
    if (!session || !session->Active)
        return "Rotation bot is already off.";

    TC_LOG_INFO("module", "mod-rotation-bot: off for {} after {} ticks.", player->GetName(), session->TickCount);
    // Keep the session so the debug setting survives turning the rotation off and on.
    session->Active = false;
    return "Rotation bot off.";
}

std::string RotationBotMgr::ToggleDebug(Player* player)
{
    if (char const* error = AccessError(player))
        return error;

    RotationBotSession* session = &GetOrCreateSession(player);
    session->Debug = !session->Debug;
    session->LastTrace.clear();
    return session->Debug ? "Rotation debug trace on." : "Rotation debug trace off.";
}

std::string RotationBotMgr::Next(Player* player)
{
    if (char const* error = AccessError(player))
        return error;

    RotationBotSession* session = &GetOrCreateSession(player);

    // A press may open a fight, so being out of combat does not stop it. The reply stays silent
    // because the command is meant to be spammed from a macro; the debug trace explains a press.
    TickGate gate = CheckGate(player);
    if (gate != TickGate::Ready && gate != TickGate::OutOfCombat)
    {
        Trace(player, *session, TickGateName(gate));
        return "";
    }

    Act(player, *session, RotationStateBuilder::Build(player), true);
    return "";
}

std::vector<std::string> RotationBotMgr::DescribeState(Player* player) const
{
    if (char const* error = AccessError(player))
        return { error };

    std::vector<std::string> lines = RotationStateBuilder::Describe(player, RotationStateBuilder::Build(player));
    lines.insert(lines.begin(), Trinity::StringFormat("Tick gate: {}", TickGateName(CheckGate(player))));
    return lines;
}

std::string RotationBotMgr::Status(Player* player) const
{
    RotationBotSession const* session = FindSession(player);
    AplProfile const* profile = RotationProfiles::Find(player);
    std::string profileText = profile ? Trinity::StringFormat("{} ({})", profile->Name, profile->File) : "none";
    if (Optional<uint8> tree = RotationProfiles::GetMainTree(player))
        profileText += Trinity::StringFormat(". Talents: mostly {}", RotationProfiles::GetSpecName(player->GetClass(),
            *tree));

    RotationMode mode = session ? session->Mode : _config.DefaultMode;
    std::string settings = Trinity::StringFormat("Mode: {}. AoE: {}. Burst: {}. Debug trace: {}.", ModeName(mode),
        AoeName(session ? session->Aoe : AoeMode::Auto), !session || session->Burst ? "on" : "off",
        session && session->Debug ? "on" : "off");

    if (!session || !session->Active)
        return Trinity::StringFormat("Rotation bot: off. Profile: {}. {}", profileText, settings);

    return Trinity::StringFormat("Rotation bot: on. Profile: {}. {} Ticks: {} every {} ms. Last tick: {}.",
        profileText, settings, session->TickCount, _config.TickIntervalMs, TickGateName(session->LastGate));
}

void RotationBotMgr::OnPlayerUpdate(Player* player, uint32 diff)
{
    RotationBotSession* session = FindSession(player);
    if (!session || !session->Active)
        return;

    // A config reload can turn the module off, or take the player off the allowed list, mid-session.
    if (!_config.Enabled || !IsAllowed(player))
    {
        EraseSession(player);
        return;
    }

    session->TickTimerMs += diff;
    if (session->TickTimerMs < _config.TickIntervalMs)
        return;

    session->TickTimerMs = 0;
    Tick(player, *session);
}

TickGate RotationBotMgr::CheckGate(Player* player) const
{
    if (!player->IsAlive())
        return TickGate::Dead;

    if (player->IsInFlight())
        return TickGate::InFlight;

    if (player->IsMounted())
        return TickGate::Mounted;

    if (player->GetVehicle())
        return TickGate::OnVehicle;

    if (player->IsCharmed())
        return TickGate::Controlled;

    // Auto-repeat (Shoot, Throw, Auto Shot) runs alongside the rotation, so it doesn't count as casting.
    if (player->IsNonMeleeSpellCast(false, false, true))
        return TickGate::Casting;

    if (player->IsSitState())
        return TickGate::Sitting;

    if (!RotationStateBuilder::IsGroupInCombat(player))
        return TickGate::OutOfCombat;

    return TickGate::Ready;
}

void RotationBotMgr::Tick(Player* player, RotationBotSession& session)
{
    ++session.TickCount;
    session.LastGate = CheckGate(player);

    // Out of combat only the precombat list runs (buffs, seals, auras); skip the snapshot when the
    // profile has none.
    if (session.LastGate == TickGate::OutOfCombat)
    {
        AplProfile const* profile = RotationProfiles::Find(player);
        if (profile && profile->Lists.count("precombat"))
        {
            Act(player, session, RotationStateBuilder::Build(player), false);
            return;
        }
    }

    if (session.LastGate != TickGate::Ready)
    {
        TC_LOG_DEBUG("module.rotationbot", "tick {} for {}: {}", session.TickCount, player->GetName(),
            TickGateName(session.LastGate));
        Trace(player, session, TickGateName(session.LastGate));
        return;
    }

    RotationState state = RotationStateBuilder::Build(player);
    TC_LOG_DEBUG("module.rotationbot", "tick {} for {}: {}", session.TickCount, player->GetName(),
        RotationStateBuilder::Summarize(state));

    Act(player, session, state, false);
}

void RotationBotMgr::Act(Player* player, RotationBotSession& session, RotationState const& state, bool manual)
{
    AplProfile const* profile = RotationProfiles::Find(player);
    if (!profile)
    {
        Trace(player, session, "no profile for this character");
        return;
    }

    // Abilities cast by the server don't start auto-attack the way a client keypress does. Out of
    // combat only a `.rot next` press may start a fight.
    // Never from stealth either: the opener starts the fight, not a white hit.
    if (profile->AutoAttack && (state.InCombat || manual) && !player->HasStealthAura() && state.Target &&
        state.Target->Hostile && state.Target->InMelee)
        if (Unit* target = player->GetSelectedUnit())
            if (player->GetVictim() != target)
                player->Attack(target, true);

    RotationSettings settings = ResolveSettings(player, session, *profile, state, manual);
    Optional<AplChoice> choice = RotationApl::Evaluate(player, *profile, state, settings);
    if (!choice)
    {
        // Waiting out the global cooldown is the normal case; only report being stuck with it ready.
        if (!state.GcdRemainingMs)
            Trace(player, session, "nothing to cast");

        return;
    }

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(choice->SpellId);
    SpellCastResult result = RotationApl::Cast(player, *choice);

    std::string action = choice->CastItem ? "use " + choice->CastItem->GetTemplate()->Name1 :
        std::string(spellInfo->SpellName[player->GetSession()->GetSessionDbcLocale()]);

    // Name the unit when it isn't the obvious one, such as a kick on a healer you don't have targeted.
    if (choice->Target != player && choice->Target != player->GetSelectedUnit())
        action += " on " + choice->Target->GetName();

    std::string line = Trinity::StringFormat("{} {} ({} #{}{}{})", result == SPELL_CAST_OK ? "cast" : "failed", action,
        choice->ListName, choice->Index + 1, choice->Entry->ConditionText.empty() ? "" : ": ",
        choice->Entry->ConditionText);

    if (result != SPELL_CAST_OK)
        line += Trinity::StringFormat(", result {}", uint32(result));

    TC_LOG_DEBUG("module.rotationbot", "{}: {}", player->GetName(), line);
    Trace(player, session, line, true);
}

void RotationBotMgr::Trace(Player* player, RotationBotSession& session, std::string const& line, bool always) const
{
    // Repeats are dropped unless asked for, so a rotation that keeps waiting for the same reason stays quiet.
    if (!session.Debug || (!always && line == session.LastTrace))
        return;

    session.LastTrace = line;
    ChatHandler(player->GetSession()).SendSysMessage("[rot] " + line);
}
