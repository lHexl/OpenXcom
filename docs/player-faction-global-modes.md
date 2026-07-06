# Player Faction Global Modes

This document defines the player faction global modes used by `PlayerFactionPlanner` and executed by `PlayerFactionAI`.

The modes are faction-level intent, not hard scripts. A unit can still fall back to local battle validation when a planned action is invalid, but the global mode should bias roles, movement, target pressure, overwatch, and retreat decisions.

## Shared Role Model

The current player unit roles are inferred from weapon and soldier stats:

- `Marksman`: high firing accuracy with accurate long-range weapons.
- `Heavy`: explosive, waypoint, or heavy weapons that need strength and careful line-of-fire.
- `Assault`: high TU, high reactions, automatic weapons, or short/mid-range pressure.
- `Support`: average shooters, carriers, medikit users, spare weapon users, and units without a clear specialist weapon.
- `Melee`: melee weapon users or units whose best attack is close combat.

Roles are not permanent classes. A unit can change role after picking up a weapon, losing ammo, becoming wounded, or changing tactical context.

## Mode: Initial Deploy

Purpose:

- Survive the dangerous first turns while leaving the craft/start zone.
- Avoid sending the whole squad through the same tile or doorway.
- Establish a first reaction line before committing to assault, room entry, or open-ground pursuit.

When to enter:

- Turn 1-2.
- The squad has several active soldiers.
- More than a couple of hostiles are still active.

Role behavior:

- `Marksman`: move only a short distance to cover or long sight lines; keep TU for reaction fire.
- `Heavy`: avoid point position and avoid blocking the exit; preserve safe firing lanes.
- `Assault`: take nearer covered positions that watch likely approaches, but do not sprint far ahead.
- `Support`: spread away from clustered soldiers, cover side approaches, and prepare mines/explosives if safe.
- `Melee`: stay behind corners or near the group until a safe close-range opportunity appears.

Unit behavior:

- Prefer short covered moves over long patrol paths.
- Penalize crowding near other soldiers and avoid blocking friendly fire lanes.
- Face the best known contact or expected approach point after moving.
- Keep enough TU for reaction fire or a second action.
- Do not freeze the whole squad in overwatch if a safe short deployment move exists.

Exit conditions:

- After turn 2: switch to normal `Defend Line`, `Assault`, `Siege Room`, or `Hunt Last Enemy`.
- If enemies become visible and safe shots exist: switch to `Assault`.
- If the squad is immediately exposed or wounded: switch to `Retreat Regroup` or `Survive`.

## Mode: Defend Line

Purpose:

- Hold a defensible line when enemies are mostly hidden or expected to advance through open ground.
- Avoid walking into unknown enemy vision.
- Win by reaction fire, crossfire, and mine/trap coverage.

When to enter:

- Many active hostiles are still hidden.
- Enemy contacts are in open or broad areas.
- The squad is not ready to assault a room.
- Early/mid battle with enough allies alive.

Role behavior:

- `Marksman`: stay far, keep line of sight lanes, prefer aimed/snap shots, avoid moving closer unless cover improves.
- `Heavy`: hold safe backline angles, avoid firing explosives if allies or walls make the shot risky.
- `Assault`: occupy closer covered tiles near likely enemy approach, preserve TU for reaction fire.
- `Support`: fill gaps, watch secondary approaches, carry or stage proximity mines.
- `Melee`: stay behind corners or doors, do not chase into open ground.

Unit behavior:

- Prefer `AMBUSH` over random patrol.
- Prefer cover and facing toward best contact/door/approach point.
- Keep enough TU for at least a snapshot when possible.
- Use proximity mines near predicted approach paths, not deep inside unknown rooms.
- Do not enter rooms just because no enemy is currently visible.

Exit conditions:

- Several enemies become visible and safe shots exist: switch to `Assault`.
- Contact is concentrated in a closed room: switch to `Siege Room`.
- Squad is badly depleted or exposed: switch to `Survive` or `Retreat Regroup`.
- Only one or two hostiles remain: switch to `Hunt Last Enemy`.

## Mode: Siege Room

Purpose:

- Control all known entrances of a dangerous room or small enclosed area.
- Force enemies to move through doors/windows into reaction fire or mines.
- Avoid sending multiple soldiers into a likely enemy cluster.

When to enter:

- Enemy contacts are inside a non-outside, non-hall room.
- The room has limited entrances.
- Enemies were recently seen or inferred in that room.
- The room is too risky for immediate entry.

Role behavior:

- `Marksman`: hold long angle to door/window, minimum 2 tiles away when using ranged weapons.
- `Heavy`: cover door from a safe line; explosives only when blast and trajectory are safe.
- `Assault`: stand closer to doors and corners if reactions are good, preferably with automatic weapons.
- `Support`: cover secondary doors, move mines to entry points, avoid blocking shooters.
- `Melee`: may stand adjacent to door/corner only if enough TU remains and friendly line-of-fire is not blocked.

Unit behavior:

- Prefer `AMBUSH` and covered repositioning over patrol.
- Face the most likely enemy emergence tile.
- Use all doors/windows of the contact room for ambush distribution.
- If a unit is already inside a risky room without enough support, leave the room if a safe exit exists.
- Do not enter the room until visibility, mines, or enough shooters are prepared.

Exit conditions:

- Enemies leave the room: switch to `Defend Line` or `Assault`.
- Room becomes clear and no hidden threat remains: switch to `Hunt Last Enemy` or `Defend Line`.
- Squad becomes outnumbered or wounded: switch to `Survive`.

## Mode: Hold Reaction

Purpose:

- Stop wasting TU on low-value movement when the best play is overwatch.
- Prevent oscillation and useless repositioning.

When to enter:

- Few visible enemies, but enemy presence is known.
- A shot is poor or unavailable.
- Moving does not improve cover, exposure, or fire lanes enough.
- Units recently oscillated between tiles.

Role behavior:

- `Marksman`: hold long range and reserve TU.
- `Heavy`: hold unless there is a high-value safe explosive shot.
- `Assault`: cover short approach lanes with reaction fire.
- `Support`: watch blind spots and keep out of fire lanes.
- `Melee`: wait behind corners or doors instead of chasing.

Unit behavior:

- Prefer `AMBUSH`.
- Face known contact position or door.
- Reserve enough TU for reaction fire if weapon supports it.
- Move only if cover, exposure, or reaction position becomes clearly better.

Exit conditions:

- Enemy appears in a good shot: switch to `Assault`.
- Contact becomes room-bound: switch to `Siege Room`.
- No progress for too long and few enemies remain: switch to `Hunt Last Enemy`.

## Mode: Assault

Purpose:

- Apply damage when enemies are visible and shots are worthwhile.
- Focus fire dangerous or wounded enemies.
- Move only to create a good shot, then retreat or free fire lanes if possible.

When to enter:

- One or more visible contacts exist.
- Several allies can shoot or support.
- The squad is not badly outnumbered.

Role behavior:

- `Marksman`: shoot from range, avoid closing, prioritize high-accuracy shots.
- `Heavy`: use safe high-damage shots against clustered or thick targets.
- `Assault`: move to clean short/mid-range fire points, shoot, then seek cover if TU remains.
- `Support`: finish wounded enemies, cover exposed allies, avoid blocking lines.
- `Melee`: attack only if target is reachable and survivable.

Unit behavior:

- Prefer `COMBAT`.
- Focus fire until a target is likely dead or no longer worth exposure.
- After partial damage, prefer hit-and-run if return fire is likely.
- Avoid moving into enemy spotting unless the move creates immediate damage or strong cover.

Exit conditions:

- Visible enemies disappear: switch to `Hold Reaction`, `Defend Line`, or `Siege Room`.
- Squad becomes exposed/wounded: switch to `Survive`.
- Only last enemies remain: switch to `Hunt Last Enemy`.

## Mode: Survive

Purpose:

- Keep soldiers alive when the squad is depleted, wounded, or outnumbered.
- Trade tempo for survival.

When to enter:

- Too few allies remain.
- Hostiles outnumber allies.
- Multiple soldiers are wounded/exposed.
- Enemy return fire risk is high.

Role behavior:

- `Marksman`: stay back, shoot only from safe positions.
- `Heavy`: avoid risky explosives; hold or retreat if exposed.
- `Assault`: stop rushing; become close overwatch.
- `Support`: prioritize medikit, smoke/mine staging if available, and safe fallback.
- `Melee`: retreat unless a safe immediate kill exists.

Unit behavior:

- Prefer `ESCAPE` or `AMBUSH`.
- Reject low-damage shots if they leave the unit exposed.
- Move to cover if cover/exposure gain is significant.
- Preserve TU for reaction or next-turn repositioning.

Exit conditions:

- Threat count drops and squad stabilizes: switch to `Defend Line` or `Hunt Last Enemy`.
- A safe visible target appears: briefly switch to `Assault`.

## Mode: Retreat Regroup

Purpose:

- Pull units out of rooms, open kill zones, blast danger, or bad crossfire.
- Rebuild a safer line before continuing.

When to enter:

- Unit or squad is inside a dangerous room.
- Pending explosive/proximity danger exists.
- Several enemies can spot or shoot the squad.
- Current positions block each other and no good attack exists.

Role behavior:

- `Marksman`: fall back first to long lanes.
- `Heavy`: retreat away from allies if carrying dangerous explosives.
- `Assault`: cover retreat routes and move last if healthy.
- `Support`: help wounded units and occupy safe fallback points.
- `Melee`: cover corners while retreating.

Unit behavior:

- Prefer `ESCAPE`.
- Avoid new contact unless already committed.
- Face enemy approach after moving.
- Stop retreat once a covered reaction line is established.

Exit conditions:

- Fallback line is formed: switch to `Defend Line` or `Hold Reaction`.
- Enemy follows into kill zone: switch to `Assault`.

## Mode: Hunt Last Enemy

Purpose:

- End the battle once remaining enemies are few.
- Search efficiently without turning every unit into a reckless scout.

When to enter:

- Active hostiles are low.
- Battle has dragged on too long.
- Most contacts are stale or hidden.

Role behavior:

- `Marksman`: advance slowly to long sight lines.
- `Heavy`: follow behind, avoid point position.
- `Assault`: lead bounded advances between cover.
- `Support`: cover rear and secondary routes.
- `Melee`: check corners only with nearby support.

Unit behavior:

- Prefer `COMBAT` and purposeful `PATROL` over static ambush.
- Move in small bounds, not full TU runs.
- Keep enough TU for reaction when entering new sight lines.
- Avoid entering closed rooms alone.

Exit conditions:

- Multiple enemies appear: switch to `Assault`, `Survive`, or `Siege Room`.
- Squad is ambushed or wounded: switch to `Survive`.

## Dynamic Switching Rules

The planner recalculates every player unit think cycle. A mode can change rapidly, but should be stable enough to avoid oscillation.

Priority order:

1. `Hunt Last Enemy` if very few hostiles remain or turn count is high.
2. `Survive` if allies are too few or badly outnumbered.
3. `Defend Line` if many enemies are hidden in open/broad areas.
4. `Siege Room` if the key contact is inside a dangerous room.
5. `Hold Reaction` if enemies are known but movement/shot quality is poor.
6. `Assault` if visible enemies can be attacked safely.
7. `Retreat Regroup` should override locally when a unit is in grenade danger, exposed inside a room, or heavily spotted.

Future refinement:

- Track mode persistence per turn so a mode must remain valid for several cycles before switching, except emergency `Survive` or `Retreat Regroup`.
- Add faction-level task slots: entry watcher, door watcher, mine layer, long overwatch, close overwatch, scout, medic.
- Add structured JSONL events for `mode_changed`, `role_policy`, and `unit_mode_decision`.

## Current Implementation Notes

- `PlayerFactionPlanner` now selects a faction strategy every player AI cycle from visible contacts, hidden contacts, room contacts, active hostile count, wounded allies, and exposed allies.
- `PlayerFactionAI` applies the strategy to local mode odds and can force local modes when the global mode is clear: defensive modes favor `AMBUSH`, `Assault` favors `COMBAT`, and `Survive`/`Retreat Regroup` favor `ESCAPE`.
- Role behavior is currently expressed through weapon/stat-derived roles, mode odds, preferred engagement range, clean-shot movement scoring, support movement scoring, and stalk ambush placement.
- Defensive room/open-area behavior uses faction room data for ambush positions, mine targets, danger-room avoidance, and support movement.
- Support and ambush movement explicitly face the best known contact/entry after moving, so reaction fire has a better chance to trigger in the intended direction.
- Hidden last-enemy clean-shot movement rejects only no-progress one-tile shuffles at long distance, preserving useful hidden-contact positioning while blocking the observed edge-map oscillation.
- `Initial Deploy` is documented and wired as a strategy type, but automatic selection is currently disabled in `PlayerFactionPlanner` after repeated full-suite runs produced process crashes without logs. Re-enable it only with a narrower trigger and another 36-run stability check.
