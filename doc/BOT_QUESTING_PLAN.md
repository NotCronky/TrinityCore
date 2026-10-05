# Bot Questing Plan: bots that play like people

**On hold** (2026-10-06): the bot population and questing are switched off while companions and the
progression system are finished. This plan is for when they come back.

Phase 3 of `doc/BOT_SYSTEM_PLAN.md` made the server's bots quest from 1 upwards. They level, but they
don't look like players: in Northshire, 200 bots do the same quests in the same order, take the same
paths and react at the same instant. This plan replaces the questing *behaviour*. The questing *data*
(`BotQuestData`: givers, enders, spawns, drops, zones) and the rotation stay.

## Why they all look the same

1. **They all start together.** A reset logs in 1000 level-1 bots within 100 seconds, about 125 to 200 per
   starting zone. On a live server, people arrive over days, so a starting zone holds a handful at
   different stages.
2. **One fixed rule decides everything.** `BotQuester::Think` is a fixed priority chain: hand in, take
   quests, hunt the *nearest* target. Same rules + same place = same choices. The jitter added in
   `08b7ac8334` only blurs the timing.
3. **They act like machines.** Quests are accepted and handed in the same tick the bot arrives, targets
   are picked instantly, they run straight to the point and never stop, look around or make mistakes.
4. **Questing is all they do.** No trips to trainers or town, no breaks, no gathering, no groups, no chat.
   Real players spend a large share of their time on these.

## Design

### 1. Personality

Every bot gets a personality when it is created, stored in a new `bot_personality` table (characters
database) so it survives restarts. Each trait is 0–100:

| Trait | Changes |
|---|---|
| Pace | Reaction time (0.3–2 s), how long it reads quest text, how often it takes breaks |
| Thoroughness | Takes every quest, or skips some (low-reward, far, group quests); finishes or abandons hard ones |
| Grinding | Kills mobs in its way, or runs past them; grinds when out of quests or moves zone |
| Social | Joins groups, emotes, chats, buffs passers-by |
| Wanderlust | Follows its race's route, or tries other starting zones and continents (a human levelling in Dun Morogh) |
| Tidiness | Sells and repairs early, or lets bags fill up |
| Skill | Pulls one mob at a time or adds by accident; dies more or less |

Traits are drawn so most bots are average and a few are extreme. A play schedule (see 3) is part of it.

### 2. Choosing what to do: scores instead of a fixed order

`Think` lists what the bot could do now (hand in, take quests, hunt objective A or B, grind, sell, train,
rest, take a break, gather, travel, join a group...). Each gets a score: how much it's needed × the
personality's weight × a little randomness. The bot picks the best and **commits** to it: it keeps
going until it is done or something scores much higher (being attacked, bags full), so it doesn't
flip between choices.

What this changes in practice:

- **Different order.** One bot does the wolves first, another the kobolds, a third hands in first.
- **Crowd awareness.** Each map keeps a cheap count of bots per spawn area and per target. A bot prefers
  quiet spots and doesn't run to a mob three others are already heading for. This alone breaks up most
  of the Northshire crowd, and wastes less time waiting for respawns.
- **Spots, not points.** Hunting picks a spawn *area* and wanders it, rather than walking to the nearest
  spawn point.
- **Distractions.** A mob crossing its path, a herb it can gather, a lower-scoring quest nearby: low-pace
  or high-grinding bots get side-tracked now and then.

### 3. Arriving and leaving like players: play sessions

- **More characters than online slots.** E.g. 1500 characters for 1000 online (`Bots.Population.Count`
  stays the number online). Each bot plays sessions of 1–4 hours at its preferred times, then walks to an
  inn or city and logs out; another logs in. Faces change, and levels spread out naturally over days.
- **Gradual start after a reset:** bots arrive over a configurable time (e.g. 6 hours) instead of 100
  seconds, so starting zones fill up gradually.
- Logged-out bots stay where they logged out, like players, and continue from there.

### 4. Acting like a person

A small movement and timing layer (`BotMotion`) every activity goes through:

- **Reaction time** before each new action, from Pace.
- **Reading time:** standing at a quest giver 3–15 s before accepting, a few seconds before handing in.
  Real players do this, and it breaks the lockstep at every quest hub.
- **Routes, not lines:** long walks go through a few offset waypoints on the navmesh; it stops now and
  then, turns, sometimes jumps while running long distances.
- **Facing** the NPC it talks to and the mob it fights; sitting to eat (already done).
- **Mounts** once it has riding (level 40 up to TBC, 30 in 3.0 and 3.1, 20 from 3.2, following the bot's patch): mounts for long walks, dismounts near the target.
- **Mistakes**, from Skill: pulling a second mob, fighting a mob a bit too high, dying sometimes and
  doing the corpse run.

### 5. Groups

Bots on the same quests nearby, with Social high enough, form groups of 2–3 (5 for elite quests and
group quests like Hogger or the Defias Brotherhood chain). The leader picks what to do; the others follow
and fight its target, as companions do. Kill credit is shared and quest items drop for each member, so
a group of three needs a third of the kills: the biggest fix for crowded zones. Groups break up when
levels drift apart, someone's session ends, or the quests are done.

Later: bots invite a real player doing the same quest nearby, and accept a player's invite.

### 6. Life in town

- **Trainers:** instead of learning spells on level up, bots walk to a trainer every 2 levels or so, when
  passing a town.
- **Hearthstone and inns:** binds at the inn of the hub it quests from, hearths back to hand in and sell,
  logs out at inns.
- **Flight paths:** learns them as it passes and flies between hubs; boats and zeppelins between
  continents.
- **Town time:** mailbox, bank, auction house (phase 4 of the bot plan), standing around the inn for a
  while. Towns look busy instead of empty while bots work outside.

### 7. Professions and gathering

Two professions chosen by class and personality (gatherers most often, like real players). Gatherers
detour to herb and ore nodes and skin their kills, which makes paths look natural; crafters craft at
their trainer's level; some bots fish now and then. Materials go to the auction house in phase 4.

### 8. Social (configurable, off by default to start)

- Emotes: wave at passing players, cheer, `/dance` in towns, "ding" on level up.
- General and LookingForGroup chat from templates: "LF1M Hogger", "where are the kobolds", selling
  materials. Low rate, so channels don't flood.
- Buffing players who pass by (Fortitude, Mark of the Wild), short answers to whispers, accepting duels
  outside cities.
- Guilds of bots later.

## How to tell it worked

A `.bot crowd` report and an in-game check:

- Most bots on one spawn area or one mob, per zone (target: no more than 3–4 on one area).
- How many bots start the same activity within the same second (target: near zero).
- Level spread after 1 and 6 hours (today, nearly everyone is the same level).
- Standing in Northshire, Goldshire and Razor Hill for 10 minutes: does it look like a server?

Each step keeps the 1000-bot benchmark: world tick average under 5 ms, worst under 50 ms. Crowd counts
are per map and updated when bots move between areas, so they stay cheap; pauses and reading time cost
nothing.

## Steps

| Step | What | Fixes |
|---|---|---|
| 1 | Personality table; scored choices with commitment; crowd awareness; spawn areas; reaction and reading time | Same order, same targets, lockstep, crowding |
| 2 | Play sessions (more characters than slots, logging out at inns), gradual arrival after a reset | Everyone starting together, nobody ever leaving |
| 3 | Questing groups of 2–3, group quests with 5 | Crowding, elite quests skipped, nobody plays together |
| 4 | Town life: trainers by walking, hearthstone, inns, flight paths, boats, mounts | Empty towns, levelling by walking only |
| 5 | Movement polish: routes, stops, jumps, mistakes | Robotic running and fighting |
| 6 | Gathering and crafting professions, fishing | Straight paths, no professions (needed for the auction house) |
| 7 | Emotes, chat, buffs, whispers | Silent server |

Steps 1 and 2 change the most of what you see for the least work, so they come first. The auction house
(bot plan phase 4) fits after step 6, since it needs professions and town visits.

## Decisions

- **Online count vs characters:** recommended `Bots.Population.Count` = bots online, plus
  `Bots.Population.Characters` (default 1.5× that) for the pool that rotates through sessions.
- **Arrival after a reset:** recommended over 6 hours (configurable; 0 keeps today's 100 seconds).
- **Chat:** recommended off by default until the templates read well.
