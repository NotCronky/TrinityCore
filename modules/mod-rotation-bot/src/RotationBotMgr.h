/*
 * mod-rotation-bot: server-side combat rotation for a real player's own character.
 * Released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef MOD_ROTATION_BOT_MGR_H
#define MOD_ROTATION_BOT_MGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Optional.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
struct AplProfile;
struct RotationSettings;
struct RotationState;

// Why a tick did not get to evaluate the rotation. Ready means it did.
enum class TickGate : uint8
{
    Ready,
    Dead,
    Mounted,
    InFlight,
    OnVehicle,
    Controlled,
    Casting,
    Sitting,     // Eating, drinking or sitting down: a cast would stand the player up.
    OutOfCombat  // Only the precombat list runs.
};

char const* TickGateName(TickGate gate);

enum class RotationMode : uint8
{
    Auto, // PvP against a player target or inside a battleground or arena, PvE otherwise.
    Pve,
    Pvp
};

enum class AoeMode : uint8
{
    Auto, // On when the profile's AoE threshold is met.
    On,
    Off
};

struct RotationBotConfig
{
    bool Enabled = true;
    uint32 MinSecurity = 0;
    std::unordered_set<uint32> Accounts; // Empty allows every account.
    uint32 TickIntervalMs = 120;
    std::string ProfileDir; // Empty means the profiles folder in the module's source tree.
    RotationMode DefaultMode = RotationMode::Auto;
    uint32 InterruptDelayMinMs = 150;
    uint32 InterruptDelayMaxMs = 400;
};

// Per-character state, owned by RotationBotMgr and freed on logout. The object itself is only
// touched by the thread updating that player, or by the world thread while maps are idle.
class RotationBotSession
{
public:
    bool Active = false;
    bool Debug = false;
    RotationMode Mode = RotationMode::Auto;
    AoeMode Aoe = AoeMode::Auto;
    bool Burst = true;
    uint32 TickTimerMs = 0;
    uint32 TickCount = 0;
    TickGate LastGate = TickGate::Ready;
    std::string LastTrace; // Last line the debug trace sent to chat.

    // Reaction delay for one enemy's cast: when it was first seen and how long to wait.
    struct Reaction
    {
        uint32 SpellId = 0;
        uint32 SeenAtMs = 0;
        uint32 DelayMs = 0;
    };

    // By caster, for the selected target and every enemy player that is casting.
    std::unordered_map<ObjectGuid, Reaction> Reactions;
};

class RotationBotMgr
{
public:
    static RotationBotMgr& Instance();

    void LoadConfig();

    // Loads the profile files. Returns the summary and any errors, one per line.
    std::vector<std::string> LoadProfiles();
    RotationBotConfig const& GetConfig() const { return _config; }

    // Command handlers. Each returns the message to show the player.
    std::string Enable(Player* player);
    std::string Disable(Player* player);
    std::string Status(Player* player) const;
    std::string ToggleDebug(Player* player);
    std::string Next(Player* player);
    std::vector<std::string> Reload(Player* player);
    std::string SetMode(Player* player, Optional<std::string> const& value);
    std::string SetAoe(Player* player, Optional<std::string> const& value);
    std::string SetBurst(Player* player, Optional<bool> value);
    std::vector<std::string> DescribeState(Player* player) const;

    void OnPlayerUpdate(Player* player, uint32 diff);
    void OnPlayerLogout(Player* player);

private:
    RotationBotMgr() = default;

    // The player's session, or nullptr when they haven't used the rotation since logging in.
    RotationBotSession* FindSession(Player* player) const;
    void EraseSession(Player* player);

    bool IsAllowed(Player* player) const;

    // Why the player can't use the rotation, or nullptr when they can.
    char const* AccessError(Player* player) const;

    // The player's session, created with the configured defaults when there is none.
    RotationBotSession& GetOrCreateSession(Player* player) const;

    // manual is a `.rot next` press: the player supplies the reaction time, so interrupts don't wait.
    RotationSettings ResolveSettings(Player* player, RotationBotSession& session, AplProfile const& profile,
        RotationState const& state, bool manual) const;
    TickGate CheckGate(Player* player) const;
    void Tick(Player* player, RotationBotSession& session);
    void Act(Player* player, RotationBotSession& session, RotationState const& state, bool manual);
    void Trace(Player* player, RotationBotSession& session, std::string const& line, bool always = false) const;

    RotationBotConfig _config;

    // Players on different maps are updated by different threads, so the container is locked.
    // Sessions are held by pointer, so a session stays valid while others are added or removed.
    mutable std::unordered_map<ObjectGuid, std::unique_ptr<RotationBotSession>> _sessions;
    mutable std::shared_mutex _sessionsLock;
};

#define sRotationBotMgr RotationBotMgr::Instance()

#endif
