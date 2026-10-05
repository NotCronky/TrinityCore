# Bot System Plan (TrinityCore 3.3.5)

Goal: a server-side population of player bots that quest, run dungeons, play battlegrounds and arena, clear raids and trade on the auction house. Players can also log in their own alt characters as bots.

## Requirements

- Written **from scratch** as a module (see `doc/MODULE_SYSTEM_PLAN.md`). Other bot projects are reference material, not a base.
- **At least 1000 concurrent bots.**
- All bots start at **level 1**. The exception is **Death Knights**, which start at 55 like real players.
- Race and class mix matches **live WotLK** within each faction, but the population is **exactly 50% Alliance / 50% Horde**.
- Bots quest, run dungeons, play BGs and arena, and clear raids.
- Questing bots **buy and sell on the auction house**.
- Players can **log in their own alts as bots**.

## Reference projects (for ideas, not copied)

| Project | Worth studying |
|---|---|
| [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) (AzerothCore, GPLv2) | Trigger → strategy → action AI model, per-boss raid strategies, BG logic, altbot commands, random-bot population manager, level-bracket balancing |
| ike3 playerbots (CMaNGOS) | The original design mod-playerbots grew from. A cleaner view of the core AI model |
| trickerer NPCBots (TrinityCore 3.3.5) | Runs on this exact core. Useful for TrinityCore-specific combat and spell handling, though its bots are creatures rather than players |

mod-playerbots is GPLv2, the same license as TrinityCore, so borrowing small pieces with attribution is allowed if that ever saves time.

**Note:** mod-playerbots does **not** contain race/class census data. Its `RandomPlayerbotFactory::CreateRandomBot` creates one character of each class per account, flips a coin for faction, then picks uniformly among the valid races. Its only weighting is spec choice (`AiPlayerbot.RandomClassSpecProb.<class>.<spec>`). Census data has to come from elsewhere (see Population).

## Architecture

```
BotMgr (singleton, updated from a WorldScript)
 ├─ BotPopulationMgr   creates accounts/chars, keeps 50/50, logs bots in/out
 ├─ BotSessionHolder   owns socketless WorldSessions, updates them itself
 ├─ BotScheduler       spreads AI updates across ticks (throttling, LOD)
 └─ per bot: BotAI
      ├─ Triggers   (low health, quest available, queue popped, boss cast X)
      ├─ Strategies (role: tank/heal/dps; mode: quest, dungeon, bg, arena, raid, AH)
      ├─ Actions    (move, cast, loot, accept quest, sell, queue LFG…)
      └─ Packet sink  reads server→client packets (invites, trade, ready checks)
```

### Bots are real `Player` objects on socketless sessions

`WorldSession` already allows a null socket (`SendPacket` returns early, `WorldSession.cpp:208`). However, `WorldSession::Update` returns `false` when there's no socket (`WorldSession.cpp:475`), and that removes the session from the world. So:

- Bot sessions stay **out of** `World::m_sessions`. `BotSessionHolder` updates them instead.
- Bots act mostly by **sending opcodes into their own session**, e.g. `CMSG_QUESTGIVER_ACCEPT_QUEST` or `CMSG_AUCTION_SELL_ITEM`. Every action then goes through the same validation a real client gets, which prevents most cheating bugs and avoids duplicating core logic. Combat and movement can call `Player`/`Unit` methods directly for speed.

### Core hooks needed

These follow the module-system rule: a new virtual hook plus a dispatch call, never edits to logic.

1. `WorldSession::SendPacket`: when the session belongs to a bot, pass the packet to the bot's AI.
2. A `WorldSession::IsBot()` flag, so `Update` and `LogoutPlayer` skip socket-specific handling.
3. Player hooks the AI needs that `PlayerScript` lacks (most interaction can come from the packet sink instead).
4. Bots must never count toward `PlayerLimit`, the `/who` cap, or the login queue.

Everything else lives in `modules/mod-bots/`.

## Population

### Exactly 50/50, census ratios within each faction

```
faction = (allianceCount <= hordeCount) ? ALLIANCE : HORDE
(race, class) ~ CensusWeights[faction]   // joint distribution, valid combos only
```

- The split is enforced on both **created** characters and **online** bots. The login and logout scheduler always picks from the smaller online faction, so the world stays 50/50 at every moment, not only in total.
- Within a faction the real mix holds: e.g. Blood Elf paladins stay common on Horde, and Draenei stay the only Alliance shamans.
- Gender is 50/50 unless the census source gives a split.

### Census data (task: source it)

Weights go in a DB table rather than code, so they can be tuned without rebuilding:

```sql
CREATE TABLE bot_census_weights (
  race   TINYINT UNSIGNED NOT NULL,
  class  TINYINT UNSIGNED NOT NULL,
  weight FLOAT NOT NULL,          -- relative weight, normalised per faction at load
  PRIMARY KEY (race, class)
);
```

Source: a WarcraftRealms census snapshot from the WotLK era (late 2009 to 2010) via the Wayback Machine. WarcraftRealms published race totals, class totals and per-race class breakdowns, which give the joint race/class distribution directly. Wowpedia's WoW Census page notes a 2010 snapshot with Blood Elf at 17% and Draenei at 10% of characters, a useful sanity check. If only separate race and class totals can be found, approximate the joint distribution by multiplying them and renormalising over valid combinations.

### Patches (see `doc/PROGRESSION_SYSTEM_PLAN.md`)

- Every bot has a patch, which only increases. Online bots follow the patch of the character the player is logged in on.
- Race and class combinations that didn't exist yet at a bot's patch get zero weight, and the faction sampler renormalises. Blood Elves and Draenei appear from 2.0, Death Knights from 3.0. As later patches unlock them, the population manager creates more bots of under-represented combinations until the population matches the census.

### Death Knights

- From patch 3.0, DKs are created at **level 55** using their census weight. They're the only exception to the level-1 rule.
- They start in Acherus. The DK starting chain (Ebon Hold) is heavily scripted, with vehicles, phasing and the Light's Hope battle, so it's hard to automate:
  - **Early phases:** run the chain for the bot by granting quest completions and phase state, then send it to its capital at 58. Reuse the core's own quest-completion paths.
  - **Later:** a dedicated scripted strategy that actually plays the chain.

### Character creation and login

- Bot accounts are flagged in `bot_accounts`, with up to 10 characters each (the client limit).
- Names come from race-appropriate syllable generators, checked with `ObjectMgr::CheckPlayerName` and against existing characters.
- Bots log in gradually (a few per second) and are spread across all starting zones.
- Config sets the target online count (default 1000) and a day/night curve, so bots log in and out like a lived-in realm.

## Activity systems

### Questing (the base layer, build first)

- A **zone planner** picks the best zone for the bot's level and faction from `quest_template` level ranges.
- **Quest selection:** walk to givers via `creature_queststarter` / `gameobject_queststarter`, accept everything level-appropriate.
- **Objectives:** `quest_poi` / `quest_poi_points` give objective areas. Kill and collect objectives use `creature_loot_template` / `gameobject_loot_template` to find which mobs drop what.
- **Movement:** `PathGenerator` (MMaps required), flight paths, hearthstone, boats and zeppelins.
- Train spells and talents, equip upgrades, repair, sell junk, level two professions.
- **Quest blacklist and override table** for escorts, vehicle quests and scripted events bots can't handle yet.

### Auction house (part of questing)

TrinityCore's built-in `AuctionHouseBot` (`src/server/game/AuctionHouseBot/`) creates items from nothing. Bot trading **replaces** it, or the two will distort each other's prices.

- **Selling:** looted greens, cloth, herbs, ore, crafted goods. Each bot prices from a **market price table** updated from recent sales, falling back to a multiple of vendor price.
- **Buying:** gear upgrades for its spec, bags, and consumables before dungeons and raids.
- Gold in and out is logged per day so inflation can be watched and tuned.

### Dungeons

- Bots queue through the real **LFG system** (`src/server/game/DungeonFinding/`), so groups mixing players and bots work normally.
- Role-based combat AI (tank threat and positioning, healer triage, DPS assist-the-tank) plus **per-boss strategy scripts** where generic AI fails.

### Battlegrounds and arena

- Bots join through the normal BG queue, which also keeps BG faction balance.
- A per-BG objective planner: WSG flag carry and escort, AB/AV node control, EotS flag and towers, SotA/IoC vehicles last.
- Arena: bots form teams with bots of similar rating. Use focus-target, CC, line-of-sight pillar and trinket logic.

### Raids (the hardest part, build last)

- **Each boss needs a strategy script:** positioning, spreading, soaking, interrupts, add priority.
- Bots form raids under a player or bot raid leader. Real lockouts, loot rules and gear checks apply.
- In patch order: MC / Onyxia → BWL → ZG → AQ20 / AQ40 → Naxx 40 → Karazhan / Gruul / Magtheridon → SSC / TK → Hyjal / BT → ZA → Sunwell → Naxx 80 / OS / EoE → Ulduar → ToC / Onyxia 80 → ICC → Ruby Sanctum.

## Alt characters as bots

Both modes use the same `BotAI`:

- **Companion:** `.bot add <altname>` logs an alt from *your own account* into the world as a bot in your group. It follows you, fights with you, takes chat commands (`follow`, `stay`, `attack`, `tank`, `loot`…) and can queue with you.
- **Autonomous:** `.bot release <altname>` lets the alt join the general bot population until you log it out or log in on it yourself.

Safety rules: only alts on the same or a linked account, never while the alt is logged in by a person, bot-owned gold and items logged, and a config option to disable the feature. Alt bots don't count toward the 50/50 population target.

## Scaling to 1000+ bots

Design for **1000–3000 concurrent bots** and benchmark from phase 1.

- **Map threading:** raise `MapUpdate.Threads` (default `1`, `worldserver.conf.dist:480`) to roughly the core count. 1000 bots across ~60 zones and instances makes parallel map updates essential.
- **AI level of detail:** a bot near a real player updates every tick. A bot nobody can see updates every 1–5 seconds, with simplified movement such as straight-line moves between waypoints when unobserved.
- **Time-sliced scheduler:** `BotScheduler` gives each bot a slot, so AI cost per world tick stays flat instead of spiking.
- **Pathfinding cache:** cache common routes (quest hub → objective area, town → flight master).
- **Grids:** bots spread out keep many grids loaded, so expect more memory. Keep `GridUnload = 1` so empty grids still unload.
- **DB load:** stagger character saves, batch AH writes, and use async queries for login.
- **Metrics:** log AI time per tick, bots per map, and world tick time (`.server info`). Fail the benchmark if the world diff rises above about 100 ms at 1000 bots.

## Phases

| Phase | Deliverable |
|---|---|
| 0 | Module skeleton, core hooks, socketless bot session that logs in, stands in Elwynn and logs out cleanly |
| 1 | Population manager: bot accounts, census table, 50/50 enforcement (created and online), staggered login, DK creation at 55. **Benchmark: 1000 idle bots** |
| 2 | Generic combat AI per class and spec, plus companion alt bots (`.bot add`) |
| 3 | Autonomous questing 1–80: zone planner, quests, trainers, gear, travel. **Benchmark: 1000 questing bots** |
| 4 | Auction house economy (replace AHBot) and professions |
| 5 | LFG dungeons with generic role AI and essential boss scripts |
| 6 | Battlegrounds, then arena |
| 7 | Raids, tier by tier. Scripted DK starting chain |
| 8 | Ongoing: scaling, AI quality, more boss scripts |

## Open items

- Find the census snapshot and fill `bot_census_weights`.
- Server hardware (cores and RAM), to set realistic benchmark targets.
- Whether bots should chat, form guilds, or respond to whispers (realism features, low priority).
