# mod-progression

Each character plays in a patch of its own, from Vanilla 1.1 to WotLK 3.3.5, and moves to the next one
when the player chooses. The full design is in [doc/PROGRESSION_SYSTEM_PLAN.md](../../doc/PROGRESSION_SYSTEM_PLAN.md).

Status: **phase 1**. Characters have a patch and a level cap, and players move through the patches
with `.patch`. Patch content isn't restricted yet: raids, zones, quests, vendors and the rest are
phases 2 and 3.

## What it does now

- **Every character has a patch.** New characters start in `Progression.StartPatch` (1.1 by default).
  Blood Elves and Draenei start in 2.0 at the earliest and Death Knights in 3.0, because they didn't
  exist before. Characters created before the module was installed get
  `Progression.ExistingCharacterPatch` (3.3.5 by default, so nothing changes for them).
- **Level cap.** No experience past the patch's cap: 60 in Vanilla patches, 70 in TBC, 80 in WotLK.
  Reaching the cap shows a message about `.patch next`.
- **The player chooses when to move on**, and only forward. Before moving, `.patch next` lists what
  each patch on the way adds and what ends, and the move happens only when confirmed.
- **Server cap.** `Progression.ServerCap` limits everyone, e.g. while later patches aren't ready.

## Commands

| Command | Who | What it does |
|---|---|---|
| `.patch` or `.patch info [name]` | Players | The character's patch, the server cap and the next patch |
| `.patch list` | Players | Every patch with its level cap; `>` marks yours |
| `.patch next` | Players | Shows what the next patch brings and ends. Then `.patch next confirm` (within 2 minutes) moves you |
| `.patch advance <version>` | Players | The same for any later patch, e.g. `.patch advance 2.0`, then `.patch advance 2.0 confirm` |
| `.patch set <version> [name]` | Game masters | Moves a character (online) to any patch, forward or back |
| `.patch servercap [version]` | Administrators | Shows or sets the server cap until the next restart |
| `.patch reload` | Administrators | Reads `progression_patch` again |

Versions are written as in `.patch list`: `1.1` ... `1.12`, `2.0` ... `2.4`, `3.0` ... `3.3.5`.

## How a character's patch is stored

As one completed hidden quest: patch 1.1 is quest 90001, 3.3.5 is quest 90022 (`progression_patch.quest_id`).
The quests have flag 1024 (`QUEST_FLAGS_TRACKING`), so they never show in the quest log, and no
quest giver offers them. They load and save with the character like any quest, and quest checks in
the core (the `conditions` table, `access_requirement`) can gate content by patch without code,
which the next phases use.

Only the current patch's quest is kept, so "complete N quests" achievements count one extra quest.

## Data

`sql/world/` creates `progression_patch`, one row per patch: version, name, expansion, level cap,
arena season, hidden quest, and the text shown before advancing (`unlocks`, `removes`). Edit the
rows and type `.patch reload` to change them. The text is shown to players; what each patch actually
allows comes with the next phases.
