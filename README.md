# TrinityCore 3.3.5 with modules, bots and progression

This is a fork of [TrinityCore](https://github.com/TrinityCore/TrinityCore) for WotLK 3.3.5a. It keeps
TrinityCore unchanged except for a few small additions, and puts its own features in modules, so it can
keep taking TrinityCore's updates.

**Use the `3.3.5-mods` branch.** `3.3.5` is an unchanged copy of TrinityCore's 3.3.5 branch.

## Why GitHub says this branch is thousands of commits ahead and behind

GitHub compares every branch with TrinityCore's default branch, `master`, which is the modern retail
version of TrinityCore. TrinityCore's `3.3.5` and `master` split years ago and have developed separately
since, so the numbers count those two histories, not changes made here. Compared with TrinityCore's
`3.3.5` branch, `3.3.5-mods` only adds this fork's own commits.

Don't use **Sync fork** on `3.3.5-mods`: it would try to merge retail TrinityCore into it. Take
TrinityCore's updates like this instead:

```bash
git remote add upstream https://github.com/TrinityCore/TrinityCore.git   # once
git fetch upstream
git merge upstream/3.3.5
```

## What's different from TrinityCore

| Feature | Status |
|---|---|
| **Module system.** Features live in `modules/<name>/` with their own config and database updates, so the core stays close to TrinityCore. See [modules/README.md](modules/README.md). | Done |
| **mod-rotation-bot.** A server-side combat rotation for your own character, every class and spec from level 1 to 80, PvE and PvP, with editable YAML profiles. Ported from the AzerothCore module. See [its README](modules/mod-rotation-bot/README.md). | Done. Warrior tested in game, other classes not yet |
| **Progression.** Each character plays in a patch of its own, from Vanilla 1.1 to WotLK 3.3.5, and moves to the next one when the player chooses. See [the plan](doc/PROGRESSION_SYSTEM_PLAN.md). | Planned |
| **Player bots.** At least 1000 bots that quest, run dungeons, battlegrounds, arenas and raids, trade on the auction house and follow your character's patch. See [the plan](doc/BOT_SYSTEM_PLAN.md). | Planned |

Changes to the core itself are kept small: the module loader in CMake, module SQL in the database updater,
and a few hooks. Each hook is marked `// MODULE HOOK` in the source.

## Building

Build and install with the scripts in the repository root. Requirements are the same as TrinityCore's
(see [Requirements](#requirements) below); the scripts use clang and install to `server/`.

| Script | What it does |
|---|---|
| `./rebuild.sh` | Deletes `build/`, configures, compiles everything and installs |
| `./compile.sh` | Recompiles only what changed and installs |
| `./extract-data.sh` | Extracts dbc, maps, vmaps and mmaps from a 3.3.5a client into `server/data`, with custom MPQs moved aside while it runs |

Module settings are installed to `server/etc/worldserver.conf.d/`.

## Credits

- [TrinityCore](https://github.com/TrinityCore/TrinityCore) and its contributors. Everything below the
  line is TrinityCore's own README.
- mod-rotation-bot was ported from its AzerothCore version.
- [fkYAML](https://github.com/fktn-k/fkYAML) (MIT) is bundled with mod-rotation-bot.
- [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots), [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  and [vMangos](https://github.com/vmangos/core) are references for the bot and progression plans.

Report problems with this fork's changes in this repository's issues, not TrinityCore's.

--------------

# ![logo](https://community.trinitycore.org/public/style_images/1_trinitycore.png) About TrinityCore (3.3.5)

[![Average time to resolve an issue](https://isitmaintained.com/badge/resolution/TrinityCore/TrinityCore.svg)](https://isitmaintained.com/project/TrinityCore/TrinityCore "Average time to resolve an issue") [![Percentage of issues still open](https://isitmaintained.com/badge/open/TrinityCore/TrinityCore.svg)](https://isitmaintained.com/project/TrinityCore/TrinityCore "Percentage of issues still open")

--------------


* [Build Status](#build-status)
* [Introduction](#introduction)
* [Requirements](#requirements)
* [Install](#install)
* [Reporting issues](#reporting-issues)
* [Submitting fixes](#submitting-fixes)
* [Copyright](#copyright)
* [Authors &amp; Contributors](#authors--contributors)
* [Links](#links)



## Build Status

master | 3.3.5 | cata_classic
:------------: | :------------: | :------------:
[![master Build Status](https://circleci.com/gh/TrinityCore/TrinityCore/tree/master.svg?style=shield)](https://circleci.com/gh/TrinityCore/TrinityCore/tree/master) | [![3.3.5 Build Status](https://circleci.com/gh/TrinityCore/TrinityCore/tree/3.3.5.svg?style=shield)](https://circleci.com/gh/TrinityCore/TrinityCore/tree/3.3.5) | [![cata_classic Build Status](https://circleci.com/gh/TrinityCore/TrinityCore/tree/cata_classic.svg?style=shield)](https://circleci.com/gh/TrinityCore/TrinityCore/tree/cata_classic)
[![master Build status](https://ci.appveyor.com/api/projects/status/54d0u1fxe50ad80o/branch/master?svg=true)](https://ci.appveyor.com/project/DDuarte/trinitycore/branch/master) | [![Build status](https://ci.appveyor.com/api/projects/status/54d0u1fxe50ad80o/branch/3.3.5?svg=true)](https://ci.appveyor.com/project/DDuarte/trinitycore/branch/3.3.5) | [![Build status](https://ci.appveyor.com/api/projects/status/54d0u1fxe50ad80o/branch/cata_classic?svg=true)](https://ci.appveyor.com/project/DDuarte/trinitycore/branch/cata_classic)
[![master Windows Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/win-x64-build.yml/badge.svg?branch=master&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22Windows%20x64%22+branch%3Amaster+event%3Apush) | [![3.3.5 Windows Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/win-x64-build.yml/badge.svg?branch=3.3.5&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22Windows%20x64%22+branch%3A3.3.5+event%3Apush) | [![cata_classic GCC Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/gcc-build.yml/badge.svg?branch=cata_classic&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3AGCC+branch%3Acata_classic+event%3Apush)
[![master Ubuntu Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/linux-build.yml/badge.svg?branch=master&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22Ubuntu%20x64%22+branch%3Amaster+event%3Apush) | [![3.3.5 Ubuntu Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/linux-build.yml/badge.svg?branch=3.3.5&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22Ubuntu%20x64%22+branch%3A3.3.5+event%3Apush) | [![cata_classic GCC Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/gcc-build.yml/badge.svg?branch=cata_classic&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3AGCC+branch%3Acata_classic+event%3Apush)
[![master macOS arm64 Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/macos-arm-build.yml/badge.svg?branch=master&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22macOS%20arm64%22+branch%3Amaster+event%3Apush) | [![3.3.5 macOS arm64 Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/macos-arm-build.yml/badge.svg?branch=3.3.5&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22macOS%20arm64%22+branch%3A3.3.5+event%3Apush) | [![cata_classic macOS arm64 Build status](https://github.com/TrinityCore/TrinityCore/actions/workflows/macos-arm-build.yml/badge.svg?branch=cata_classic&event=push)](https://github.com/TrinityCore/TrinityCore/actions?query=workflow%3A%22macOS%20arm64%22+branch%3Acata_classic+event%3Apush)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/435/badge.svg)](https://scan.coverity.com/projects/435) | [![Coverity Scan Build Status](https://scan.coverity.com/projects/4656/badge.svg)](https://scan.coverity.com/projects/4656) |

## Introduction

TrinityCore is a *MMORPG* Framework based mostly in C++.

It is derived from *MaNGOS*, the *Massive Network Game Object Server*, and is
based on the code of that project with extensive changes over time to optimize,
improve and cleanup the codebase at the same time as improving the in-game
mechanics and functionality.

It is completely open source; community involvement is highly encouraged.

If you wish to contribute ideas or code, please visit our site linked below or
make pull requests to our [Github repository](https://github.com/TrinityCore/TrinityCore/pulls).

For further information on the TrinityCore project, please visit our project
website at [TrinityCore.org](https://www.trinitycore.org).

## Requirements


Software requirements are available in the [wiki](https://trinitycore.info/en/install/requirements) for
Windows, Linux and macOS.


## Install

Detailed installation guides are available in the [wiki](https://trinitycore.info/en/home) for
Windows, Linux and macOS.


## Reporting issues

Issues can be reported via the [Github issue tracker](https://github.com/TrinityCore/TrinityCore/labels/Branch-3.3.5a).

Please take the time to review existing issues before submitting your own to
prevent duplicates.

In addition, thoroughly read through the [issue tracker guide](https://community.trinitycore.org/topic/37-the-trinitycore-issuetracker-and-you/) to ensure
your report contains the required information. Incorrect or poorly formed
reports are wasteful and are subject to deletion.


## Submitting fixes

C++ fixes are submitted as pull requests via Github. For more information on how to
properly submit a pull request, read the [how-to: maintain a remote fork](https://community.trinitycore.org/topic/9002-howto-maintain-a-remote-fork-for-pull-requests-tortoisegit/).
For SQL only fixes, open a ticket; if a bug report exists for the bug, post on an existing ticket.


## Copyright

License: GPL 2.0

Read file [COPYING](COPYING).


## Authors &amp; Contributors

Read file [AUTHORS](AUTHORS).


## Links

* [Website](https://www.trinitycore.org)
* [Wiki](https://www.trinitycore.info)
* [Forums](https://talk.trinitycore.org/)
* [Discord](https://discord.trinitycore.org/)
