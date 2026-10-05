# mod-bots

Player bots: real characters logged into the world without a game client. The full design is in
[doc/BOT_SYSTEM_PLAN.md](../../doc/BOT_SYSTEM_PLAN.md).

Status: **phase 3, step 1**. The server creates and keeps its own bot population online and they quest on
their own around where they are; players' alts come along as companions that follow and fight.

## Questing (the server's own bots)

With `Bots.Questing.Enable = 1`, each of the server's bots, once a second (four times a second in a fight):

1. **When dead, does a corpse run:** releases after 5 seconds, runs back from the graveyard as a ghost and
   takes its body back (the server's reclaim delay applies); if that takes over 5 minutes, or the body is
   on another map, it comes back to life where it is.
2. **Fights** whatever is attacking it, with mod-rotation-bot.
3. **Eats and drinks** below 60% health or mana, until full.
4. **Sells and repairs:** with fewer than 4 free bag slots or gear under 25% durability, it walks to the
   nearest friendly vendor (one that repairs, if it needs that), sells junk and gear and goods it won't
   use (keeping quest items, consumables, reagents, bags and upgrades), and repairs.
5. **Loots** corpses it tapped within 30 yards: everything, and the money (like a client: `CMSG_LOOT`,
   `CMSG_AUTOSTORE_LOOT_ITEM`).
6. **Hands in** finished quests whose quest ender is within 250 yards, picking the reward that scores best
   for its class and role, and wears anything in its bags that is better than what it has on.
7. **Takes quests** from the nearest quest giver within 250 yards that has quests it can do.
8. **Hunts** what its quests need: creatures to kill or loot quest items from, and objects to use or take
   quest items from (crates, plants...), whichever is nearer; when none are there, it walks to where they
   spawn.
9. **Hands in quests further away** once nothing is left to hunt where it is. Talk-to quests ("Report to
   Goldshire") are complete as soon as they're taken, so before this rule every bot left its starting area
   straight away.
10. **Grinds** mobs around its level (not elites, critters or civilians) that nobody else has tapped.
11. **Travels** to a quest giver of its planned zone with quests for it, when there is nothing to do nearby.

**Zone planner:** at startup, each zone (a quest's QuestSortID) gets a level range from the quests bots can
do there (from the 20th to the 80th percentile of their levels, widened a little), a count of quests per
faction, and its quest givers. A bot levels in the nearest zone on its continent, within 1500 yards, that
suits its level and has at least 5 quests for its faction; it plans again on level up, every 10 minutes,
and when its zone has nothing new and nothing unfinished for it. While it waits at a quest creature's spawn
for one to come back, it grinds what is around.

Bots don't move in step: walking to a spawn point or a quest hub, each aims for a random spot within 6
yards of it (talking to an NPC stays exact), and each decides every 1 to 1.5 seconds rather than all
together.

The server's own bots open their fights with the rotation's best ability (like `.rot next`): out of combat,
mod-rotation-bot only buffs, so a ranged bot used to stand at its target without starting the fight.

A quest giver or ender it finds nobody at (event or phased NPCs listed in the database), or gets no quest
from, is skipped for 10 minutes.

When it levels up, it learns the class spells a trainer would teach and spends its talent points (in the
tree it has most points in, or one picked from its guid at first).

**Quests it skips for now:** escorts, events, spell-cast credit, timed and repeatable quests, quests handed
in to an object, and items from objects other than chests. It doesn't sell, train
professions, use the auction house, take flight paths or boats, or move to the next zone's quest hub
other than by walking on the same continent; those are later steps of phase 3.

The data comes from the world database at startup: creature spawns, `creature_queststarter` and
`creature_questender`, and quest items in `creature_loot_template` (`QuestRequired = 1`).

### Questing test (2026-10-06, 1000 bots, one map thread)

| | |
|---|---|
| In 5 minutes | 1328 quests handed in, 665 more taken; 389 experience per bot; 27 reached level 2 |
| World tick | 1 ms average, 20-26 ms worst per minute |
| worldserver | 1.8 GB memory, 41% of one CPU core |
| Errors | none |

500 bots per faction share a few starting zones, so they compete for the same creatures; levelling
speeds up as they spread out.

## Companions

A bot you add in game (`.bot add <alt>`) is your companion:

- **Joins your group** (making one if you have none) and leaves it when it logs out.
- **Follows you**, and is brought to you when it falls more than 80 yards behind or is on another map,
  so it comes along through instance portals.
- **Fights what you fight:** your target once you're in combat, or whatever attacks you or it. Melee
  bots stand at the target, ranged bots 25 yards away, healers stay with you. Spells come from
  mod-rotation-bot, which must be built too (companions turn it on for themselves).
- **Comes back to life** next to you once the fight is over, if it died.
- **Eats and drinks** out of combat below 60% health or mana (or when told to with `.bot eat`), and stays
  seated while it lasts unless you walk more than 20 yards away or a fight starts.
- **Nobody starts a fight on their own.** `.bot pull` sends your tank bots (Protection warriors and
  paladins, Blood death knights) at your target; `.bot pull <name>` sends one bot, tank or not. The other
  bots join in once you or a tank are fighting, on the tank's target.
- **Tanks pick up loose mobs** in a fight: a mob hitting someone else in the group, nearest first, so
  their taunts can take it.

Every bot gets a Frostweave Bag (20 slots) in each empty bag slot and a stack of food, and of water if it
uses mana, for its level when it logs in; companions are topped up every 30 seconds out of combat. An
alt gives the bags, food and water back when it logs out: anything in those bags is mailed to it first.
That removes all vendor food and water of the kinds bots use from the alt, including any it bought
itself.
- `.bot stay <name|all>` keeps them where they are; `.bot follow <name|all>` brings them along again.

`.bot gear <name|all> [spec]` gets a bot ready for its level: it learns the class spells a trainer would
teach, resets its talents and spends them in the spec (the tree name, e.g. `arms`, `holy`,
`beast_mastery`, or 1-3; by default the tree it has most points in), deepest talents first so the key
talent comes as early as possible, and equips the best gear it can use for its level and role (dungeon
item level: about level + 6 up to 60, 116 at 70, 190 at 80). Old gear goes to its bags, or by mail when
they are full; nothing is destroyed. Rings and ammunition aren't picked yet.

## Bot population

The server creates `Bots.Population.Count` bot characters (1000 by default) once, and keeps them online:

- **Exactly half Alliance, half Horde**, also while they log in.
- **Race and class follow the live WotLK census** within each faction (`bot_census`: the WarcraftRealms
  census of June 2010, see the SQL file for how race and class were combined). Gender is 50/50.
- **Level 1**, in their race's starting zone, except Death Knights: level 55 in Acherus, as on live.
  mod-progression gives each its starting patch (1.1; 2.0 for Blood Elves and Draenei; 3.0 for Death
  Knights).
- **Bots follow the players' patch** (with mod-progression): the online bots are the ones that can exist in
  the patch most online players are in (with nobody online, the last one; at first `Progression.StartPatch`).
  A bot's race and class must exist by then (Blood Elves and Draenei from 2.0, Death Knights from 3.0) and
  its own patch must not be later; bots never go back, and those behind move up to it once online. Bots
  that don't fit log out, and when too few fit, more characters are made from the races and classes of
  that patch (the census without the others); the rest wait offline for later patches.
- **Names** come from `bot_names` (built by `tools/bot_names.py`), skipping names already taken.
- **Accounts:** `BOT0001`, `BOT0002`, ... with `Bots.Population.CharactersPerAccount` characters each and
  random passwords. They are listed in `bot_accounts` and `bot_characters` (characters database).
- **Logging in:** `Bots.Population.LoginsPerSecond` (10 by default), so 1000 bots take about 100 seconds
  after a restart. Lowering the count logs the extra bots out; the characters are kept.
- **Shutdown:** bots are logged out and the server waits until their saves are written, because closing
  the database drops queued saves.

`.bot population reset confirm` (administrators and the console) starts the population over: every bot of
the population logs out, their characters are deleted (players' characters and alts are never touched; the
bot accounts are kept), and the population is made again from level 1 with fresh names, for the patch it
follows. Without `confirm` it only says what it would do.

`.bot population` (administrators and the console) shows the target, created and online bots per faction,
the average and worst world tick over the last minute, and how many questing bots are doing what
(fighting, hunting, travelling, dead...) and the busiest planned zones. `.bot info <name>` shows one bot:
its activity, zone, position, destination, fight target and quest log.

### Load test (2026-10-05, one map thread, bots idle)

| | |
|---|---|
| 1000 bots created | 100 accounts, 500 Alliance and 500 Horde, 128 Death Knights (12.8%; census 12.7%) |
| All online | 110 seconds at 10 per second, 50/50 throughout |
| World tick, 1000 idle bots | 1 ms average, 5 ms worst over a minute |
| worldserver | 1.7 GB memory, 17% of one CPU core |
| Shutdown | 6 seconds, every bot saved |

## Commands

| Command | What it does |
|---|---|
| `.bot add <character>` | Logs the character in as a bot. It must not be online. |
| `.bot remove <character>` | Logs the bot out and saves it. |
| `.bot removeall` | Logs out every bot you added. |
| `.bot list` | The bots you added and whether each is logging in, in the world or logging out. |
| `.bot gear <name\|all> [spec]` | Class spells, talents in the spec, and gear for the bot's level. |
| `.bot stay <name\|all>` / `.bot follow <name\|all>` | Companions stay put, or follow you again. |
| `.bot pull [name]` | Your tank bots (or the named bot) attack your target. |
| `.bot eat [name\|all]` | Bots out of combat sit down to eat, and drink if they use mana. |

- **In game, everyone can use them, but only for characters on their own account.** Nobody can log in,
  remove or see another account's characters as bots, game masters included.
- **Your bots log out when you do**, when you log out of the game or switch characters.
- **The server console** can add and remove any character's bot, and lists every bot. Bots added there
  belong to no account, so nobody logging out takes them with them.

If a player logs into a character that is online as a bot, the login is refused ("character already
logged in"), the bot logs out, and the player can log in a moment later.

## How it works

A bot is a normal `WorldSession` without a socket, owned by `BotMgr`:

- **It acts like a client.** It queues client packets on its session (`CMSG_CHAR_ENUM`, then
  `CMSG_PLAYER_LOGIN`, teleport replies), and the server processes them exactly like a real client's,
  on the same threads: thread-unsafe packets on the world thread, the rest on the map's thread.
- **Server packets go to the bot instead of a socket.** It only keeps the few it reacts to; the rest are
  dropped without copying.
- **`BotMgr` runs on the world thread after the maps have updated**: it updates the bot sessions,
  logs bots in and out, and at shutdown logs every bot out before the maps unload, so they are saved.
- **Teleports:** a bot confirms far teleports (new map) and near teleports like a client would.

## Core changes

Each is marked `// MODULE HOOK`:

- `WorldSession::SetBotPacketHandler` / `IsBot`: a session without a socket sends its packets to the
  handler, processes queued packets, skips the idle-connection check, isn't removed for having no
  socket, and doesn't mark the account offline when deleted (a bot can share an account with a player).
- `WorldSession::KickPlayer` on a bot sets `IsForcedExit`, and `BotMgr` logs it out.
- `PlayerScript::CanLogin(WorldSession*, ObjectGuid)`, checked before a character is loaded; refusing
  sends `SMSG_CHARACTER_LOGIN_FAILED` with `CHAR_LOGIN_DUPLICATE_CHARACTER`.

## Next

Phase 1: bot accounts and characters created by the server, the race and class census, exactly 50/50
Alliance and Horde, logging bots in gradually, and a load test with 1000 idle bots.
