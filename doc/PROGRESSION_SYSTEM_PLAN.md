# Progression System Plan (TrinityCore 3.3.5)

Goal: every character lives in a specific game patch, from Vanilla 1.1 to WotLK 3.3.5. The **player decides** when to move to the next patch, e.g. staying on 1.1 until level 60 and best-in-slot. **Bots follow the logged-in character's patch**, so the world around the player matches the patch they chose.

Builds on `doc/MODULE_SYSTEM_PLAN.md` (it ships as `modules/mod-progression/`) and `doc/BOT_SYSTEM_PLAN.md` (bots obey the same locks).

## Decisions

| Question | Decision |
|---|---|
| Patch range | Full journey: Vanilla 1.1 → TBC → WotLK 3.3.5 |
| Mode | **Individual**: each character has its own patch |
| Advancing | **Player-chosen**. Nothing advances automatically, not even boss kills |
| Bots | Follow the **currently logged-in character's** patch |

## Reference: mod-individual-progression

[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) (AzerothCore, MIT) covers the same Vanilla → WotLK range with 19 states, about 5,000 lines of C++ and 150 SQL files.

### Borrow

| Idea | How it works there |
|---|---|
| **Progress stored as hidden quests** | Reaching state *N* rewards hidden quests `66000+1 … 66000+N` (`UpdateProgressionState`). Anything that already checks "quest rewarded" (the `conditions` table, `access_requirement`) can then gate content **with no code**. |
| **Per-player visibility of NPCs and objects** | Creatures and objects carry an AI whose `CanBeSeen(player)` checks the viewer's state, so two players in the same spot see different worlds without phasing. |
| **XP capped per state** | `OnPlayerGiveXP` sets XP to zero at 60 until Outland unlocks, and so on. |
| **Map entry gated per state** | `OnPlayerBeforeTeleport` blocks BWL, AQ, Outland, Northrend and others until the right state. |
| **Late starts for later races and classes** | Config settings start Blood Elves and Draenei at TBC and Death Knights at WotLK. |
| **Vanilla content data** | Restored Naxxramas 40, Onyxia 60, vanilla AV, attunements, AQ war effort, vanilla item stats, quest XP tables. MIT-licensed, so it can be adapted for TrinityCore with attribution. |

### Do differently

- **Advancement is player-chosen** rather than triggered by boss kills.
- **Content is data-driven.** IPP has 39 near-identical `npc_ipp_*` / `gobject_ipp_*` script classes and a hardcoded `ProgressionState` enum. Here, a patch table and a content-range table drive everything.
- **Real patch granularity.** IPP groups content into raid tiers. This plan follows the actual patches (1.1 … 1.12, 2.0 … 2.4, 3.0 … 3.3.5), so "stay on 1.1" means exactly that.
- **Bots** are found through the bot system's `bot_accounts` table, not an account-name regex (`^RNDBOT.*`).
- **TrinityCore hooks.** IPP relies on AzerothCore-only hooks (`CanBeSeen`, `OnPlayerBeforeTeleport`, `OnPlayerCanEquipItem`…). The equivalents have to be added to TrinityCore (see Core hooks).

## What can be locked

- **Server only:** zones, dungeons and raids, quests, NPCs and objects, vendors and their stock, loot, level cap, honor and arena systems, dual spec, the dungeon finder, battlegrounds, world events and invasions.
- **Needs client changes too:** talent trees, class spells and their tooltips. The client is modded through `~/wow-3.4.3-mods` (see "Era talents and spells" below).
- **Server-wide only:** item stats, because the client caches item data by item ID. As in IPP, Vanilla-era items get their Vanilla stats globally. Few TBC or WotLK characters use them, so the side effects are small.

## Patch table

```sql
CREATE TABLE progression_patch (
  id           TINYINT UNSIGNED PRIMARY KEY,  -- order of progression
  name         VARCHAR(48),                   -- "1.6 Assault on Blackwing Lair"
  expansion    TINYINT UNSIGNED,              -- 0 vanilla, 1 TBC, 2 WotLK
  level_cap    TINYINT UNSIGNED,              -- 60 / 70 / 80
  arena_season TINYINT UNSIGNED,              -- 0 = none
  quest_id     INT UNSIGNED                   -- hidden quest marking "reached this patch"
);
```

Default rows. Exact content boundaries get verified during the data work.

| id | Patch | Main content unlocked | Cap |
|---|---|---|---|
| 1 | 1.1 | Launch world, launch dungeons, Molten Core, Onyxia | 60 |
| 2 | 1.2 Mysteries of Maraudon | Maraudon | 60 |
| 3 | 1.3 Ruins of the Dire Maul | Dire Maul, Azuregos, Lord Kazzak | 60 |
| 4 | 1.4 The Call to War | Honor system and Vanilla PvP ranks | 60 |
| 5 | 1.5 Battlegrounds | Alterac Valley (Vanilla version), Warsong Gulch | 60 |
| 6 | 1.6 Assault on Blackwing Lair | Blackwing Lair, Darkmoon Faire | 60 |
| 7 | 1.7 Rise of the Blood God | Zul'Gurub, Arathi Basin | 60 |
| 8 | 1.8 Dragons of Nightmare | Emerald dragons, Silithus revamp | 60 |
| 9 | 1.9 The Gates of Ahn'Qiraj | AQ war effort, gong event, AQ20, AQ40 | 60 |
| 10 | 1.10 Storms of Azeroth | Dungeon set 2 quests | 60 |
| 11 | 1.11 Shadow of the Necropolis | Naxxramas 40, Scourge invasion | 60 |
| 12 | 1.12 Drums of War | World PvP objectives (Silithus, Eastern Plaguelands towers) | 60 |
| 13 | 2.0 | Outland, Blood Elves and Draenei, Karazhan, Gruul, Magtheridon, SSC/TK (attunement), arena season 1 | 70 |
| 14 | 2.1 | Black Temple, Ogri'la, Skettis, Netherwing, arena season 2 | 70 |
| 15 | 2.2 | Minor (kept so patch numbers match) | 70 |
| 16 | 2.3 Gods of Zul'Aman | Zul'Aman, arena season 3 | 70 |
| 17 | 2.4 Fury of the Sunwell | Sunwell Plateau, Isle of Quel'Danas, Magisters' Terrace, arena season 4 | 70 |
| 18 | 3.0 | Northrend, Death Knights, Naxxramas 80, OS, EoE, Wintergrasp, arena season 5 | 80 |
| 19 | 3.1 | Ulduar, dual spec, Argent Tournament, Strand of the Ancients, season 6 | 80 |
| 20 | 3.2 | Trial of the Crusader, Onyxia 80, Isle of Conquest, season 7 | 80 |
| 21 | 3.3 | ICC and its 5-mans, Random Dungeon Finder, season 8 | 80 |
| 22 | 3.3.5 | Ruby Sanctum | 80 |

### Content-range table (replaces IPP's per-NPC scripts)

```sql
CREATE TABLE progression_content (
  type       TINYINT UNSIGNED,   -- creature spawn, gameobject spawn, quest, map, vendor item, lfg dungeon, battleground, game event
  entry      INT UNSIGNED,       -- spawn guid / quest id / map id / item id …
  min_patch  TINYINT UNSIGNED,   -- first patch it exists in
  max_patch  TINYINT UNSIGNED,   -- last patch it exists in (255 = forever)
  PRIMARY KEY (type, entry)
);
```

`max_patch` handles content that **goes away**: Naxxramas 40 and Onyxia 60 end at 1.12, the AQ war effort ends once the gates open, Vanilla PvP ranks end at 2.0, the pre-3.0 Scourge invasion and so on. The table is loaded into memory at startup, so checks are lookups, not queries.

## Characters

### Effective patch

```
effective = min(characterPatch, Progression.ServerCap)
```

- **`characterPatch`:** stored per character and changed only by the player (or a GM).
- **`Progression.ServerCap`:** an admin setting, default 22 (no cap). It lets the server owner hold everyone back, e.g. while data for later patches isn't finished.

### Starting patch

| Character | Starts at |
|---|---|
| Most races | 1.1, configurable (`Progression.StartPatch`) |
| Blood Elf, Draenei | 2.0 (they didn't exist before) |
| Death Knight | 3.0, at level 55 |

Optional (`Progression.AltsStartAtAccountPatch`): new characters start at the highest patch on the account.

### Moving to the next patch (player-chosen)

- A **command** `.patch next` (or `.patch advance <patch>`) and a **"Keeper of Time" NPC** in each capital with a gossip menu.
- **Forward only** for players. Going back would leave characters above the level cap with gear that shouldn't exist yet. GMs can still move characters in either direction.
- **No requirements by default.** The player decides when they're done with a patch, whether that means level 60 and best-in-slot or skipping straight ahead. Optional config: require the patch's level cap before advancing.
- **Skipping patches** is allowed (e.g. 1.1 → 2.0). Everything in between unlocks, apart from content that ended before the target patch.
- **Warn about what ends.** The confirmation lists content that **disappears**, e.g. "Advancing to 2.0: you'll lose access to Naxxramas 40, Onyxia 60 and Vanilla PvP ranks." Confirm twice.
- Advancing rewards the hidden quests up to the new patch and refreshes visibility at once, with no relog needed.

### Storage

- **Implemented in phase 1:** the character's patch *is* one completed hidden quest (`progression_patch.quest_id`, IDs 90001–90022, flag `QUEST_FLAGS_TRACKING` so the client never shows it), borrowing IPP's trick. It loads and saves with the character, so there is no separate table to keep in sync. Only the current patch's quest is kept, because TrinityCore counts every completed quest toward "complete N quests" achievements; one hidden quest adds one to the count instead of up to 22.
- "Reached patch *N* or later" is therefore a set of quests (*N* … 22): conditions combine them with `ElseGroup` (OR), and a generator script builds those rows from `progression_content`. Existing TrinityCore tables can then gate content with no code:
  - `access_requirement.quest_done_A/H` blocks entry to a dungeon or raid.
  - `conditions` (TrinityCore 3.3.5 source types 1–3 loot, 14–15 gossip, 19 quest available, 23 vendor) filters loot, gossip (e.g. the dual-spec option), quest availability and vendor items.
- Content with a `max_patch` needs "not in patch *N* or later": negated `conditions` rows on each of those quests, combined with AND.
- `access_requirement` takes a single quest, so it can't express a patch range; instance entry uses the map-entry hook below instead.

## Enforcement points

| What | How | Core change? |
|---|---|---|
| Level cap (60 / 70 / 80) | `PlayerScript::OnGiveXP` sets XP to zero at the patch's cap. Quest XP at the cap converts to gold, which needs a hook where the core computes quest XP. | Small hook |
| Raids and dungeons | `access_requirement` with the hidden quests | None |
| Continents (Outland, Northrend) and anything else | A hook in `MapManager::PlayerCannotEnter` (also blocks the Dark Portal and the Northrend boats and zeppelins) | One hook |
| NPCs and objects that appear or disappear | Per-player visibility: a hook in `WorldObject::CanNeverSee`, consulted **only** for spawns listed in `progression_content` (kept in a hash set). This replaces IPP's `CanBeSeen`. | One hook (hot path, so keep it cheap) |
| Quests | `conditions` source type 19, generated from `progression_content` | None |
| Vendors (tier, PvP and badge gear) | `conditions` source type 23 | None |
| Loot (emblems by patch, Vanilla drops) | `conditions` on loot templates, or a loot hook that swaps emblem IDs by patch | None or small |
| Honor and arena | Vanilla rank system for 1.4–1.12 (IPP has an implementation). Arena from 2.0 with the season from `arena_season` | Module code |
| Dungeon finder | Hide dungeons above the player's patch, and disable random dungeons before 3.3. Needs a hook in `LFGMgr`'s lock-status calculation | One hook |
| Battlegrounds | AV and WSG from 1.5, AB from 1.7, EotS from 2.0, SotA from 3.1, IoC from 3.2. Hook in the BG list or join handler | Small hook |
| World events and invasions | Shown per player through the visibility hook | None |
| Item equip (optional) | Block equipping items from a later patch received by trade or mail. Hook in `Player::CanEquipItem` | Optional hook |

### Core hooks to add (follow the module-system hook rule)

1. `OnPlayerCanEnterMap(Player*, mapId)` in `MapManager::PlayerCannotEnter`.
2. `OnIsVisibleForPlayer(WorldObject const* obj, Player const* viewer)` in `WorldObject::CanNeverSee`, for creatures and gameobjects only.
3. `OnLfgDungeonLockStatus(Player*, dungeonId, uint32& lockStatus)` in `LFGMgr`.
4. `OnBattlegroundListFilter` / `OnCanJoinBattleground`.
5. `OnQuestComputeXP(Player*, Quest const*, uint32& xp)` for the level-cap gold conversion.
6. Optional: `OnCanEquipItem(Player*, ItemTemplate const*)`, `OnCanGroupInvite(Player*, Player*)`.

These are reusable by other modules, so they're good candidates to offer upstream.

## Groups with mixed patches

- **Instance entry:** every group member must meet the requirement themselves (the default `access_requirement` behaviour).
- **Visibility:** each player sees content for their own patch. Visibility filtering only applies to listed spawns, never to players, so group members always see each other. Phasing is deliberately **not** used, since phase masks would hide players from each other.
- **Config `Progression.GroupRule`:** `none`, `same_expansion` (default), or `same_patch`.

## Bots follow the logged-in character

### Rule

Each bot has its own **patch, which only ever increases**. The bot population manager keeps the online bots matched to the patch of the character the player is logged in on.

- **One player online:** every online bot is at that character's patch.
- **Player advances** (e.g. 1.12 → 2.0): online bots advance with them. They can now level to 70 and go to Outland, and newly created bots can be Blood Elves or Draenei. The population gains TBC races gradually, as it did on live realms.
- **Player switches to an alt on an earlier patch:** bots at higher patches are logged out gradually and replaced by bots at the alt's patch. Bots never move backwards, since that would leave them above the level cap. If not enough exist, new bots are created at level 1 at that patch.
- **Several players online on different patches:** the online bot pool is split between their patches in proportion to the number of players on each, each part kept 50/50 Alliance/Horde.
- **Companion and alt bots** (`.bot add`) always use their master's patch while in the group.
- **Bots in a player's group, dungeon or raid** use the player's patch for that instance.

### Keeping the switch smooth

Logging 1000 bots out and in on every character switch is expensive, so:

- Each patch keeps a **warm pool** of already-created bot characters. A switch only logs characters out and in, with no character creation. The switch is staggered over a minute or two, starting with bots near the player.
- `Bot.SwitchBehaviour`: `gradual` (default) or `instant` (log everyone over in one go and accept the spike).

### Other bot integration

- **Planners read the patch:** the questing zone planner, dungeon picker, raid picker, BG/arena logic and AH buying all filter by the bot's patch, using the same in-memory `progression_content` lookups.
- **Census and character creation:** combinations that didn't exist yet at the bot's patch get zero weight (Blood Elves and Draenei before 2.0, Death Knights before 3.0), and the 50/50 sampler renormalises. Once later patches unlock them, the population manager gradually creates more bots of under-represented combinations until the population matches the census.
- **Raid AI** is only needed for raids the bot's patch allows. Build raid strategies in patch order: MC/Onyxia → BWL → ZG → AQ → Naxx 40 → Karazhan …
- **The auction house** only lists and buys items that exist at the bot's patch. The AH has to be divided by patch, otherwise a 1.1 player would see 3.3 items. Options: per-patch auction listings filtered when browsing (needs a hook in the AH list handler), or one AH where bots only post items valid for the player's current patch. Recommendation: filter listings by the viewer's patch.

## Era talents and spells

Each character uses the talent trees and class spells of its expansion: **Vanilla (1.12)**, **TBC (2.4.3)** or **WotLK (3.3.5)**. Talents follow the expansion rather than every patch, because the final trees of each expansion are the ones with complete, well-tested data. Talents reset when a character moves to a new expansion, as they did on live.

### Setup this has to work with

```
WotLK Classic client 3.4.3 (build 54261, modded via ~/wow-3.4.3-mods)
        │  modern protocol, DB2 data in CASC
HermesProxy (~/hermes-wotlk)  translates protocol; ships per-spell CSVs and DB2 hotfixes
        │  3.3.5a protocol
TrinityCore 3.3.5 (this server)  Talent.dbc / Spell.dbc + spell_dbc table + spell scripts
```

Three layers must agree on every talent and spell:

| Layer | Holds | How to change it |
|---|---|---|
| **Server** | `Talent.dbc`, `TalentTab.dbc`, `Spell.dbc` | New spells through the `spell_dbc` table (already loaded over `Spell.dbc`, `DBCStores.cpp:400`). Talents need an equivalent DB overlay for `sTalentStore`/`sTalentTabStore`, or a module-owned era talent table that `Player::LearnTalent` checks through a hook. |
| **Proxy** | Per-spell lists in `~/hermes-wotlk/CSV` (`AuraSpells3`, `SpellEffectPoints3`, `PassiveSpells3`, `LearnSpells`…) and DB2 hotfixes in `CSV/Hotfix/` (already includes `Spell`, `SpellName`, `SpellEffect`, `SpellMisc`…) | Add rows for every new spell ID. The local HermesProxy is a stripped binary, so any **code** change needs the HermesProxy source. CSV changes don't. |
| **Client** | `Talent.db2`, `TalentTab.db2`, `Spell*.db2` | Inject edited DB2s with `~/wow-3.4.3-mods` (`ngdp_build.py`), or send them as HermesProxy hotfixes. |

### One client, three eras at once

The client has a single `Talent.db2`, but characters on different eras share it. Two ways to handle this:

- **A. Hotfixes per character (try first).** HermesProxy sends the character's era rows for `Talent`, `TalentTab` and the class spells as DB2 hotfixes on login. Talent IDs can then be reused across eras, and the stock talent UI works unchanged. **To prove:** that the 3.4.3 client accepts hotfixes for `Talent`/`TalentTab`, and that they can be re-sent when switching characters. Moving to a new expansion would require a relog, which is fine because talents reset at that point anyway.
- **B. All eras in the client data (fallback).** Vanilla and TBC trees are added as extra `TalentTab`/`Talent` rows with their own IDs, all shipped in the modded client. The talent UI (`Blizzard_TalentUI`, editable with Mod Studio) shows only the tabs for the character's era. The server reports the era through a hidden marker spell or aura the UI can read. More client work, but no dependency on hotfix behaviour.

### Spells

- Class spells that changed between eras get **era-specific spell IDs** in a custom range, e.g. Vanilla Fireball rank 11. The server, the proxy CSVs and the client all get the same rows. Shared WotLK spell rows are never edited.
- Trainers teach the version for the character's era (`npc_trainer` rows filtered by `progression_content`). Moving era unlearns the old versions and teaches the new ones.
- Damage, healing and effects are calculated by the server, so most behaviour comes from `spell_dbc` plus spell scripts. The client data matters for tooltips, cast bars, costs, range and cooldown display.
- **Scope:** player class spells and talents only, about 9 classes × 2 older eras. Creature spells stay as they are.

### Data sources

| Era | Talents and spells | Content per patch |
|---|---|---|
| Vanilla | **WoW Classic Era (1.14/1.15) client DB2s.** Vanilla trees and tooltips already in the modern DB2 format the 3.4.3 client reads; Mod Studio can read another install (`wowmods installs add … --product wow_classic_era`). Cross-check values against vMangos (1.12.1). | **vMangos** (`~/vmangos`): `patch_min`/`patch_max` on creatures, equipment and other tables, for patches 1.2–1.12. GPLv2, compatible. Spawn GUIDs differ from TrinityCore's, so match by entry and position. |
| TBC | TBC Classic (2.5.x) client DB2s if available, otherwise CMaNGOS-TBC 2.4.3 data | CMaNGOS-TBC and IPP |
| WotLK | Already in this server and client | IPP plus this plan's WotLK table |

### Work on `~/wow-3.4.3-mods`

- `tools/db2write.py` can't write string fields yet (`"string fields aren't supported by the writer"`). `SpellName`, talent descriptions and spell tooltips need it.
- `tools/dbd/` has 19 table definitions, mostly character models. Add `Talent`, `TalentTab`, `Spell`, `SpellName`, `SpellEffect`, `SpellMisc`, `SpellLevels`, `SpellCooldowns`, `SpellPower`, `SkillLineAbility` and others from WoWDBDefs.
- A build step that generates the era rows once and writes them to all three layers (`spell_dbc` SQL, HermesProxy CSVs, client DB2s), so they can't drift apart.
- The client's playerbots panel (`playerbots/`) is written for mod-playerbots' chat commands. Either the new bot system keeps compatible command names or the panel gets updated.

### Alternative client route

`~/custom-343-client-plan.md` describes a native client (a WoWee fork) that reads gameplay data from the **3.3.5a DBCs** and only uses 3.4.3 for art. With that client, server and client read the same DBC files, so era talents and spells only need editing once and HermesProxy drops out. If that client becomes the main one, option A/B above simplifies to "ship the era DBCs".

## Vanilla and TBC restoration (data work)

Most of the work is **data**, and Vanilla is the biggest part:

- **Naxxramas 40:** shares map 533 with Naxx 80. IPP uses separate creature entries and its own scripts (`src/naxx40Scripts`, about 23 files) and picks the version by level. Port the approach to TrinityCore.
- **Onyxia 60:** the map is shared with the level-80 version from 3.2. Pick the version by patch.
- **Vanilla Alterac Valley,** attunements (Onyxia, MC, BWL, Karazhan, SSC/TK, Hyjal, BT), the AQ war effort and gong event, the Scourge invasion, dungeon set 2, and the Vanilla PvP ranks.
- **Vanilla item stats** (applied globally, see client limits), Vanilla creature stats and respawns, and an optional Vanilla quest XP table.
- **TBC:** attunements, keys (Shattered Halls, Tempest Keep, Black Temple), raid health restorations, the Isle of Quel'Danas phases.
- IPP's 150 SQL files assume AzerothCore table layouts, so they must be **converted** to TrinityCore's schema, not applied directly. Keep generator scripts that build `conditions` and `access_requirement` rows from `progression_content`, so the data lives in one place.

## Commands

```
.patch info [player]                 current patch, server cap, what the next patch unlocks/removes
.patch next                          player: advance one patch (with confirmation)
.patch advance <patch>               player: advance to a later patch (with confirmation)
.patch set <player> <patch>          GM: set any patch, forwards or backwards
.patch servercap <patch>             GM: set the server-wide cap
.patch reload                        GM: reload progression_patch / progression_content
```

## Phases

| Phase | Deliverable |
|---|---|
| 1 | Module skeleton, `progression_patch` / `character_progression`, hidden quests, starting patches per race and class, `.patch` commands with confirmation, level cap via `OnGiveXP` |
| 2 | Map and instance gating (`access_requirement` plus the map-entry hook), BG and dungeon finder filtering |
| 3 | `progression_content` plus the visibility hook. Vendor and loot conditions. "Keeper of Time" NPC |
| 4 | Bot integration: bot patch, following the logged-in character, warm pools per patch, census renormalisation, AH filtering by patch. **Done (2026-10-06)** except the AH (no bot AH yet) and splitting the population between players in different patches (it follows the patch most online players are in). |
| 5 | Vanilla 1.1–1.12 content restoration (Onyxia 60, MC, BWL, ZG, AQ, Naxx 40, Vanilla AV, PvP ranks) |
| 6 | TBC 2.0–2.4 restoration (attunements, keys, raid tuning, Quel'Danas) |
| 7 | WotLK 3.0–3.3.5 patch-specific content (Argent Tournament, Dalaran changes, Onyxia 80, ICC) |
| 8 | Era talents and spells: prove the hotfix route (A) or fall back to B, extend `db2write.py`, generate Vanilla then TBC talents and class spells into all three layers |
| 9 (optional) | Vanilla quest XP, other Vanilla rule restorations |

Phase 8's proof of concept (one class's Vanilla tree through HermesProxy hotfixes) can start early and in parallel, because its result decides how much client work follows.

The bot plan's phases 3 (questing) and 7 (raids) should follow this patch order. Bots quest in Vanilla zones first and raid MC before anything else.
