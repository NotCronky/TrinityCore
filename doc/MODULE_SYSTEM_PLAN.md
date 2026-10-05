# Module System Plan (TrinityCore 3.3.5)

Goal: let custom features live outside the core as modules, while the core keeps merging cleanly from official TrinityCore.

## What the core already provides

| Need | Already in TrinityCore |
|---|---|
| Find and build script folders automatically | `cmake/macros/ConfigureScripts.cmake` (`GetScriptModuleList`) builds every folder under `src/server/scripts/*` as a script module. Each one can be `static`, `dynamic` or `disabled` via `SCRIPTS_<NAME>`. |
| Event hooks | `ScriptMgr` has about 30 hook classes, such as `PlayerScript`, `WorldScript`, `CommandScript`, `UnitScript` and `GuildScript` (`src/server/game/Scripting/ScriptMgr.h`). |
| SQL updates | The `updates_include` table already lists `$/sql/custom/*` as `RELEASED`. The DB updater will apply files from any folder added to that table. |
| Per-module config | `worldserver.conf.d/` is loaded automatically (`src/server/worldserver/Main.cpp`, `LoadAdditionalDir`). |
| Hot reload | `ScriptReloadMgr` works for dynamic script modules. It only watches `src/server/scripts/<module>` (`ScriptReloadMgr::GetSourceDirectory()`), so modules stored elsewhere need a small patch. |

The module system mostly connects these pieces. It doesn't need a new framework.

## Design

### 1. Module folder layout

```
modules/
  mod-transmog/
    src/                     # C++; must define void AddMod_transmogScripts()
    sql/{auth,characters,world}/
    conf/mod-transmog.conf.dist
    module.cmake             # optional: extra libs/includes, deps
```

Each module is its own git repo, added as a submodule or plain clone. `modules/*` goes in `.gitignore` apart from a README, so the core repo stays clean.

### 2. CMake: the only real core change

In `cmake/macros/ConfigureScripts.cmake`, about 20 lines:

- `GetScriptModuleList` also scans `${CMAKE_SOURCE_DIR}/modules/*`.
- `GetPathToScriptModule` returns `modules/<name>/src` for those entries.
- Module names are made safe for C++ identifiers (`mod-transmog` → `Mod_transmog`), so the generated `Add<Name>Scripts()` call and the `SCRIPTS_MOD_TRANSMOG` option are valid.
- If `module.cmake` exists, it is included so a module can link extra libraries.

Every module then becomes its own build target, `scripts_mod_transmog`. Static, dynamic and minimal builds work per module with no new code paths.

### 3. Install step for SQL and config

- Add a CMake install rule that copies `modules/*/conf/*.conf.dist` into `etc/worldserver.conf.d/`. Modules then read their settings with e.g. `sConfigMgr->GetBoolDefault("Transmog.Enable", false)`.
- For SQL, the updater needs each module's folder registered. The least invasive way is to have a module's first SQL file register itself, e.g. `INSERT IGNORE INTO updates_include VALUES ('$/modules/mod-transmog/sql/world','RELEASED')`. A cleaner option is a short loop in `DBUpdater` that adds `modules/*/sql/<db>` automatically, at a cost of about 15 lines in one file.

### 4. Hot reload (optional, phase 2)

Change `ScriptReloadMgr::GetSourceDirectory()` so it maps a module name to `modules/<name>/src` when that folder exists. Only needed when using `SCRIPTS=dynamic` with hot swapping.

### 5. Missing hooks, where most conflicts will come from

Modules will eventually need a hook TrinityCore doesn't have, such as `OnPlayerBeforeLoot`. Rules:

- Add a new hook as a new virtual method plus a dispatch call in `ScriptMgr`. Never change upstream logic around it.
- Put each hook in its own small commit, marked with a `// MODULE HOOK` comment so it's easy to find during conflicts.
- Write hooks generically and consider sending them upstream. Any hook TrinityCore merges is one less patch to carry.

## Git workflow for staying current

```
upstream/3.3.5  ──●──●──●──●──●─────────►  (TrinityCore, never edited)
                    \           \
your/3.3.5-mods  ────●(modsys)───●(merge)─►  module system + hooks
                                             modules/ = separate repos
```

- Keep a branch that exactly matches upstream. Do all work on `3.3.5-mods`.
- Pull upstream with `git fetch origin && git merge origin/3.3.5`, weekly or as preferred. For a tidy patch series, rebase instead.
- **Core changes budget:** `ConfigureScripts.cmake`, `src/server/scripts/CMakeLists.txt` (install rule), optionally `DBUpdater.cpp` and `ScriptReloadMgr.cpp`, plus hook commits. All gameplay code stays in `modules/`.
- Add a CI job, such as a GitHub Action, that builds `3.3.5-mods` merged with the latest upstream every night, so conflicts and API breaks are found while they're small.
- Upstream sometimes changes APIs that modules call. Module code should only use the `ScriptMgr` hook classes and public `Player`/`Creature` methods, never internal code.

## Phases

1. **CMake discovery plus a sample `mod-hello` module** (a `PlayerScript` login message with its own `.conf.dist`). Proves static and dynamic builds work.
2. **Automatic SQL and config install** for modules.
3. **Hot-reload path fix** and the nightly upstream-merge CI job.
4. **A process for adding hooks**, plus a module template repo.

## Prior art

AzerothCore already ships a mature system like this (`modules/` folder with many `mod-*` repos). Its design is worth borrowing from, but its modules won't drop into TrinityCore because the hook APIs differ.
