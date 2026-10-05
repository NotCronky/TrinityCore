# mod-bots

Player bots: real characters logged into the world without a game client. The full design is in
[doc/BOT_SYSTEM_PLAN.md](../../doc/BOT_SYSTEM_PLAN.md).

Status: **phase 1**. The server creates and keeps its own bot population online, and players can log
their own alts in as bots. Bots stand where they logged in and do nothing else yet: no AI, movement or
combat.

## Bot population

The server creates `Bots.Population.Count` bot characters (1000 by default) once, and keeps them online:

- **Exactly half Alliance, half Horde**, also while they log in.
- **Race and class follow the live WotLK census** within each faction (`bot_census`: the WarcraftRealms
  census of June 2010, see the SQL file for how race and class were combined). Gender is 50/50.
- **Level 1**, in their race's starting zone, except Death Knights: level 55 in Acherus, as on live.
  mod-progression gives each its starting patch (1.1; 2.0 for Blood Elves and Draenei; 3.0 for Death
  Knights).
- **Names** come from `bot_names` (built by `tools/bot_names.py`), skipping names already taken.
- **Accounts:** `BOT0001`, `BOT0002`, ... with `Bots.Population.CharactersPerAccount` characters each and
  random passwords. They are listed in `bot_accounts` and `bot_characters` (characters database).
- **Logging in:** `Bots.Population.LoginsPerSecond` (10 by default), so 1000 bots take about 100 seconds
  after a restart. Lowering the count logs the extra bots out; the characters are kept.
- **Shutdown:** bots are logged out and the server waits until their saves are written, because closing
  the database drops queued saves.

`.bot population` (administrators and the console) shows the target, created and online bots per faction,
and the average and worst world tick over the last minute.

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
