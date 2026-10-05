# Modules

Every folder here with a `src/` folder is built into the worldserver, the same way as a folder in
`src/server/scripts/`. Modules keep custom features out of the core, so it keeps merging cleanly from
upstream TrinityCore.

`mod-hello/` is a small working example. Copy it to start a new module.

## Layout

```
modules/mod-example/
  src/                          C++ sources, any subfolders
  conf/mod_example.conf.dist    optional: settings
  sql/auth/                     optional: updates for the auth database
  sql/characters/               optional: updates for the characters database
  sql/world/                    optional: updates for the world database
  module.cmake                  optional: extra build settings
```

## Loader function

The module must define one function that registers its scripts. Its name is `Add`, the folder name
with `-` (and any other character not allowed in C++ names) replaced by `_`, then `Scripts`:

| Folder | Function |
|---|---|
| `mod-hello` | `void Addmod_helloScripts()` |
| `mod-individual-progression` | `void Addmod_individual_progressionScripts()` |

This is the same naming AzerothCore uses.

Headers can be included by name from any folder under `src/`.

## Building

Run CMake again after adding or removing a module (`./compile.sh` does this). Each module gets its
own option, named `SCRIPTS_` plus the folder name in capitals with `-` replaced by `_`:

```
-DSCRIPTS_MOD_HELLO=static     built into the worldserver
-DSCRIPTS_MOD_HELLO=dynamic    built as a library in bin/scripts (can be hot-reloaded)
-DSCRIPTS_MOD_HELLO=disabled   not built
-DSCRIPTS_MOD_HELLO=default    follow -DSCRIPTS (the default)
```

A module can't share its name with a folder in `src/server/scripts/`.

## Config

`conf/*.conf.dist` is installed to `etc/worldserver.conf.d/`, and a `.conf` copy is created there
the first time. The worldserver loads every `.conf` in that folder after `worldserver.conf`, so
modules read their settings with `sConfigMgr` as usual. Later installs replace the `.conf.dist` but
never touch the `.conf`, so changed settings are kept. Use a prefix for setting names (e.g.
`Hello.Enable`) so they don't clash with the core or other modules.

Settings must come after a `[worldserver]` line, like in `worldserver.conf`. The config loader only
reads what's under a file's first section, so settings without it are silently ignored. CMake warns
about module configs that are missing it.

## Database updates

`sql/auth/`, `sql/characters/` and `sql/world/` are applied by the worldserver's database updater
like the core's own `sql/updates/`, once per file, and recorded in each database's `updates` table.

- File names must be unique across the core and every module. Use the core's pattern with the module
  name: `2026_10_05_00_mod_hello.sql`.
- Updates from the core and all modules run in file-name order, so the date decides the order.
- Never change a file after it has been applied; add a new one. A changed file is applied again or
  rehashed depending on `Updates.Redundancy` / `Updates.AllowRehash` in `worldserver.conf`.

## module.cmake

Optional. Included after the module's target is created, with these variables set:

| Variable | Value |
|---|---|
| `MODULE_NAME` | the folder name, e.g. `mod-hello` |
| `MODULE_ROOT` | the module's folder |
| `MODULE_TARGET` | the target its sources are built into: `scripts` when static, `scripts_<name>` when dynamic |

Example:

```cmake
target_link_libraries(${MODULE_TARGET} PRIVATE some-library)
target_compile_definitions(${MODULE_TARGET} PRIVATE MOD_EXAMPLE_FEATURE=1)
```

When static, every static module and script shares the `scripts` target, so keep definitions
specific to the module.

## Git

Modules are meant to be their own git repositories (cloned or added as submodules here), so
`modules/.gitignore` ignores everything except this README and `mod-hello`. To keep a module in this
repository instead, add `!/mod-name/` to `modules/.gitignore`.

## Core hooks

When a module needs a hook the core doesn't have, add it to the core as a new virtual method on the
matching script class in `ScriptMgr.h`, plus one dispatch call where it happens. Don't change
surrounding core logic. Keep each hook in its own commit with a `// MODULE HOOK` comment next to the
dispatch call, so it's easy to find when merging from upstream.
