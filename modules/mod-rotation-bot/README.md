# mod-rotation-bot

A server-side combat rotation for your own character, for PvE and PvP, at every level from 1 to 80. It runs inside the worldserver, so it reads cooldowns, auras, resources and enemy casts straight from the server and works with any client that can connect.

Status: **step 8, more specs**. Rotations are YAML profile files that `.rot reload` applies without a restart. Each profile has defensives and interrupts lists, tried first every tick, then PvP, PvE single-target and PvE AoE lists, picked automatically. Profiles ship for:

| Class | Levelling (from level 1) | Specs, each taking over once you know its key spell |
|---|---|---|
| Warrior | `warrior_leveling.yml` | Arms (Mortal Strike), Fury (Bloodthirst), Protection (Devastate) |
| Paladin | `paladin_leveling.yml` | Retribution (Crusader Strike), Protection (Holy Shield), Holy (most points in Holy) |
| Hunter | `hunter_leveling.yml` | Beast Mastery (Intimidation), Marksmanship (Aimed Shot), Survival (Wyvern Sting) |
| Rogue | `rogue_leveling.yml` | Assassination (Mutilate), Combat (Blade Flurry), Subtlety (Hemorrhage) |
| Priest | `priest_leveling.yml` | Discipline (most points in Discipline), Holy (most points in Holy), Shadow (Shadowform) |
| Death Knight | `death_knight.yml` (from 55) | Blood (Heart Strike, as a tank), Frost (Howling Blast), Unholy (Scourge Strike) |
| Shaman | `shaman_leveling.yml` | Elemental (Elemental Mastery), Enhancement (Stormstrike), Restoration (most points in Restoration) |
| Mage | `mage_leveling.yml` | Arcane (Presence of Mind), Fire (Combustion), Frost (Icy Veins) |
| Warlock | `warlock_leveling.yml` | Affliction (Unstable Affliction), Demonology (Soul Link), Destruction (Conflagrate) |
| Druid | `druid_leveling.yml` | Balance (Moonkin Form), Feral (Mangle (Cat); Cat or Bear, by the form you are in), Restoration (most points in Restoration) |

What profiles don't do: poisons, traps and placing ground effects anywhere but at the target's feet; and nothing that moves you except where a profile says so (Charge in the Arms profile, Killing Spree in the Combat rogue profile). Stealth openers run on a `.rot next` press from stealth; auto-attack never starts from stealth, so the opener starts the fight.

## TrinityCore port

Written for AzerothCore by NotCronky and ported to TrinityCore. The port needs these additions to the core, each marked `// MODULE HOOK`:

- `PlayerScript::OnAfterUpdate(Player*, uint32 diff)`, called at the end of `Player::Update`: drives the tick.
- `Spell::GetRemainingCastTime()`: time left on an enemy's cast (the same accessor as TrinityCore master).
- `SpellHistory::GetRemainingGlobalCooldown(SpellInfo const*)`: the `gcd` value.

fkYAML (MIT, header-only) is bundled in `deps/fkYAML`. Per-character state is kept in the module's manager
and freed on logout, instead of AzerothCore's `Player::CustomData`.

## Commands

| Command | What it does |
|---|---|
| `.rot on` | Turns automatic mode on: the rotation casts on its own every tick. |
| `.rot next` | Casts the single best ability now, once. Bind a macro whose only line is `.rot next`. Silent unless the debug trace is on. Works out of combat, so it can open a fight. |
| `.rot off` | Turns it off. |
| `.rot status` | Shows whether it is on, the profile in use, the tick count, the result of the last tick and whether the debug trace is on. |
| `.rot debug` | Turns the debug trace on or off. The trace prints every cast with the rule that chose it, and why the rotation is waiting when that changes. |
| `.rot mode [auto\|pve\|pvp]` | Shows or sets the mode. `auto` (the default) is PvP against a player target or inside a battleground or arena, and PvE otherwise. |
| `.rot aoe [auto\|on\|off]` | Shows or sets PvE AoE. `auto` (the default) turns it on when the profile's threshold is met. |
| `.rot burst [on\|off]` | Shows or sets burst. Profiles gate big cooldowns on it with `burst`. On by default. |
| `.rot reload` | Reads the profile files again and reports any errors. A file with errors keeps its last good version. |
| `.rot state` | Prints everything the rotation can see right now. Works with the rotation on or off. |

## Tick loop

While it is on, the rotation ticks every `RotationBot.TickIntervalMs` (120 ms by default). A tick does nothing while you are dead, in flight, mounted, on a vehicle, mind-controlled, casting or channelling, or sitting (eating and drinking). Combat means you, or a group member within 40 yards, are in combat, so a healer standing back starts healing as soon as the group is fighting. Out of combat it only runs the profile's `precombat` list (healer profiles top the group up there) (seals, auras, blessings, stances), and never starts a fight; a `.rot next` press out of combat runs `precombat` and then the rest, so it can open one. Auto-repeat spells such as Shoot or Throw don't count as casting.

## State snapshot

Each tick that gets past those checks takes one snapshot, and every condition in that tick reads from it:

- **You:** health, power, combo points, ready runes, form or stance, global cooldown left, whether you are moving, loss of control (stunned, rooted, feared, confused, silenced, disarmed), and your non-passive auras with time left and stacks.
- **Your target:** the unit you have selected, never one picked for you. Health, level, player or boss, distance, melee range, line of sight, facing, whether you are behind it, your threat as a percentage of its current target's, loss of control, its non-passive auras (yours marked), and what it is casting, with time left and whether a kick would interrupt it.
- **Enemies:** attackable units within 40 yards that are in combat, plus enemy players. Totems and critters are left out. Each one records whether it is in crowd control that damage would break.

Passive auras are left out, so a talent never counts as its own proc buff (the Sudden Death talent is passive; the Sudden Death buff it procs is not).

## Profiles

A profile is a YAML file in the profile folder (`RotationBot.ProfileDir`; by default `profiles/` in this module's source tree, so you can edit it in place). Edit a file and type `.rot reload`; errors are shown in chat with the file, the place and the column.

```yaml
name: Warrior Arms
class: warrior          # warrior, paladin, hunter, rogue, priest, death_knight, shaman, mage, warlock, druid
requires: Mortal Strike # optional: the profile only applies once you know this spell
spec: arms              # optional: ...or once this talent tree has the most of your points
priority: 10            # optional, default 0: among the profiles that apply, the highest wins
auto_attack: true       # optional: start melee auto-attack on your target once in range
aoe_enemies: 3          # optional, default 3: automatic AoE starts at this many enemies...
aoe_radius: 8           # optional, default 8: ...within this many yards of your target

lists:
  pvp:
    - cast: Rend                            # spellbook name or spell id
      on: target                            # target (default), self or enemy_players
      if: target.my_aura("Rend").remains < 1 # optional condition
    - cast: Mortal Strike
  defensives:
    - use: pvp_trinket                      # an item instead of a spell; see below
      on: self
      if: pvp and player.hard_cc and player.cc_remains > 2
```

An entry either casts a spell (`cast:`) or uses an item's on-use effect (`use:`). `use:` takes an item name or id (searched in your bags and equipment), `trinket1` or `trinket2` for the equipped trinket slots, or `pvp_trinket` for whichever equipped trinket is a PvP trinket (Medallion, Insignia, Titan-Forged Rune, ...).

A value containing a colon followed by a space, such as `Power Word: Shield`, must be quoted, or YAML reads the colon as a new key: `cast: 'Power Word: Shield'`, and for conditions `if: 'not player.aura("Power Word: Fortitude")'` (single quotes outside, so the double quotes inside stay).

Ground-targeted spells (Blizzard, Rain of Fire, Death and Decay, Hurricane, Volley) land at the feet of the unit the entry picks.

`on:` picks the unit:

- `target`: your selected target, when it is hostile.
- `self`: you.
- `party`: you and your group members within 40 yards, lowest health first; the first one the condition and the server's checks accept gets it. For heals, buffs and dispels. Solo, it is just you.
- `enemies`: each attackable unit within 40 yards that is fighting, plus enemy players, lowest health first. For taunts and off-target abilities.
- `pet`: your pet or guardian.
- `enemy_players`: each enemy player within 40 yards, lowest health first; the first one the condition and the server's checks accept gets it. Inside the condition, `unit` is the player being tried. Your selection doesn't change: only that one ability goes to the other player, and the debug trace names them. Use it for kill windows on low players and for kicking a healer you don't have targeted.

A profile has up to six lists. Every tick they are tried in this order, missing ones skipped:

0. `precombat`: only out of combat, and then nothing else runs (except on a `.rot next` press).
1. `defensives`
2. `interrupts`: only once a random reaction delay has passed since the cast was first seen, tracked separately for every caster (`RotationBot.InterruptDelayMinMs`/`MaxMs`, 150–400 ms by default). A `.rot next` press doesn't wait, because you supply the reaction time yourself.
3. In PvP: `pvp`. In PvE: `pve_aoe` while AoE is on, then `pve_st`. A profile without lists for the current mode uses the other mode's.

Automatic AoE is on when at least `aoe_enemies` enemies are within `aoe_radius` yards of your target (of you, without a target), and off whenever one of them is in crowd control that damage would break, such as Polymorph or Sap.

When several profiles match your class, the ones whose `requires` spell you don't know, or whose `spec` tree isn't the one with the most of your talent points, are dropped and the highest `priority` wins. `spec` takes the class's tree names: `arms`, `fury`, `protection`; `holy`, `protection`, `retribution`; `beast_mastery`, `marksmanship`, `survival`; `assassination`, `combat`, `subtlety`; `discipline`, `holy`, `shadow`; `blood`, `frost`, `unholy`; `elemental`, `enhancement`, `restoration`; `arcane`, `fire`, `frost`; `affliction`, `demonology`, `destruction`; `balance`, `feral`, `restoration`. Healer profiles use `spec`, so they start at level 10 with the first talent point; the others use `requires`, because their lists lean on the spec's key spell. `.rot status` shows the tree with the most points. That is how a levelling profile hands over to a spec profile once you learn the spec's key spell.

The first entry that passes all of these is cast:

1. You know the spell. A name covers every rank, and the highest rank you know is cast; entries for spells you haven't learned are skipped.
2. Its condition holds.
3. It would hit something. Spells without a target that hit enemies around you (Whirlwind, Thunder Clap,
   Frost Nova, Consecration, Fan of Knives, and auras such as Bladestorm through the spell they trigger) or
   in a cone in front of you (Cone of Cold, Shockwave) wait until the entry's target is within the spell's
   radius (and in front, for cones), or with `on: self`, until any enemy in combat is. The radius comes from
   the spell data, so profiles don't need range conditions for these.
4. The server would let you cast it right now: cooldown, global cooldown, power, stance, aura states (Overpower, Execute, Victory Rush), range, line of sight and facing. Spells the target is immune to (Divine Shield, Ice Block, Hand of Protection against physical abilities) are skipped, and so is crowd control that diminishing returns would make it immune to. Spells with a cast time are skipped while you move. Heroic Strike and Cleave aren't queued again while one is already queued.

`target` entries need a hostile target selected. The rotation never moves you, turns you or changes your target.

### Conditions

Conditions combine values with `and`, `or`, `not`, comparisons (`<`, `<=`, `>`, `>=`, `==`, `!=`), arithmetic (`+ - * /`) and parentheses. `true` is 1 and `false` is 0. Spell names go in quotes and match every rank and every spell with that name; a number is a spell id and matches its ranks. Start a condition with `not` rather than `!`, which YAML reads as a tag.

| Value | Meaning |
|---|---|
| `player.X`, `target.X` | Any of: `health_pct`, `level`, `is_player`, `is_boss`, `stunned`, `rooted`, `feared`, `confused`, `silenced`, `disarmed`, `casting`, `cast_remains` (seconds), `cast_interruptible`, `cast_channeled`, `cast_heal` (heals directly or over time), `cast_cc` (stun, fear, polymorph, root, charm, sleep and other loss of control) |
| `player.aura("Name")`, `target.aura("Name")` | Aura from anyone; with several names, any of them (`player.aura("Seal of Command", "Seal of Vengeance")`). Add `.remains` (seconds; 0 when missing, infinite when it never expires), `.stacks` or `.charges`; on its own it means `.up`. |
| `player.my_aura("Name")`, `target.my_aura("Name")` | The same, counting only auras you cast. |
| `pet.X` | Anything `target.X` can do, for your pet or guardian; `pet.exists` is false without one. |
| `player.auto_repeat` | Auto Shot, Shoot or Throw is running. |
| `player.mainhand_enchanted`, `player.offhand_enchanted` | A temporary enchant (poison, weapon imbue) is on that weapon. |
| `player.totem_fire`, `totem_earth`, `totem_water`, `totem_air` | One of your totems of that element is up within 30 yards. |
| `unit.X` | Anything `target.X` can do, for the unit the entry is being tried on (the candidate with `on: enemy_players`). |
| `unit.targeting_me`, `unit.targeting_party` | It is attacking you, or another member of your group. |
| `unit.attackers` | How many enemies are attacking it. |
| `unit.most_attacked` | Group members only: nobody in the group has more attackers (at least one). Finds the tank. |
| `party_below(pct)` | How many of you and your group members are below that health percentage. |
| `party_my_aura("Name", ...)` | How many group members (you included) carry that aura from you, such as `party_my_aura("Beacon of Light") == 0`. |
| `player.hard_cc`, `target.hard_cc` | Stunned, feared or confused (polymorph, sap, blind and the like). |
| `player.cc_remains`, `target.cc_remains` | Seconds left on the longest crowd control effect; 0 without one. |
| `target.dr("Name")` | The diminishing returns level that spell would land at, read from the server: 0 full duration, 1 half, 2 quarter, 3 immune. 0 for spells without diminishing returns. |
| `target.cooldown("Name")` | Seconds left on an enemy player's cooldown, such as `target.cooldown("Divine Shield") > 0`. 0 for creatures. |
| `target.dispellable("magic")` | How many auras of that type (`magic`, `curse`, `disease`, `poison`) a dispel would remove: buffs on a hostile unit (purge), debuffs on anyone else. |
| `player.casting("Name")`, `target.casting("Name")` | Casting or channelling that spell. |
| `enemies(r)`, `player.enemies(r)` | Enemies within `r` yards of you: attackable units in combat plus enemy players, not totems or critters. |
| `target.enemies(r)` | Enemies within `r` yards of the target, the target included. |
| `player.power`, `max_power`, `power_pct` | Rage and runic power in the units the client shows. |
| `player.combo_points`, `runes_blood`, `runes_frost`, `runes_unholy`, `runes_death`, `moving`, `gcd` (seconds left), `in_combat` | |
| `target.exists`, `hostile`, `distance`, `in_melee`, `los`, `facing`, `behind`, `threat_pct` | `facing` means you face it; `behind` means you are behind it; `threat_pct` is -1 without a threat list. |
| `cooldown("Name")` | Seconds left on the cooldown of the highest rank you know; 0 when ready. |
| `known("Name")` | You know some rank of the spell. |
| `pvp`, `pve` | The mode this tick, after `auto` is decided. |
| `aoe` | AoE is on this tick. |
| `burst` | Burst is on (`.rot burst`). |

3.3.5a has no pandemic window: refreshing a debuff early throws the time left away, so refresh in its last second (`.remains < 1`).

## Configuration

Installing the server puts `mod_rotation_bot.conf.dist` in `etc/worldserver.conf.d/` and creates `mod_rotation_bot.conf` next to it the first time; change settings there. Every option is described in the file. `.reload config` applies changes.

To see every tick in the log, add this line to `worldserver.conf`:

```
Logger.module.rotationbot=2,Console Server
```

## Roadmap

1. Skeleton: loads, `.rot on/off`, logs ticks. **(done)**
2. State snapshot and a `.rot debug` decision trace. **(done)**
3. Priority-list evaluator with a hard-coded Arms Warrior (PvP) list. **(done)**
4. Spec lists moved to hot-reloadable data files. **(done)**
5. PvE tuning, known-spell and rank detection, levelling lists for 1–80. **(done for warriors)**
6. Defensives and interrupts. **(done for warriors)**
7. PvP: diminishing returns tracking, CC, trinket, target swapping. **(done for warriors)**
8. More specs. **(done: every class)**
9. Optional AIO UI.
