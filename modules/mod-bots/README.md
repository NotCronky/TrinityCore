# mod-bots

Player bots: real characters logged into the world without a game client. The full design is in
[doc/BOT_SYSTEM_PLAN.md](../../doc/BOT_SYSTEM_PLAN.md).

Status: **phase 0**. A game master can log any character in as a bot and out again. Bots stand where
the character logged out and do nothing else yet: no population, AI, movement or combat.

## Commands

Game masters only, also from the server console.

| Command | What it does |
|---|---|
| `.bot add <character>` | Logs the character in as a bot. It must not be online. |
| `.bot remove <character>` | Logs the bot out and saves it. |
| `.bot removeall` | Logs every bot out. |
| `.bot list` | Every bot and whether it is logging in, in the world or logging out. |

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
