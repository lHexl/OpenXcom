# Player Faction AI Planning

## Goal

Make the player faction AI operate as one coordinated side instead of a set of isolated unit AIs.

The current player autobattle behavior is still mostly inherited from the original per-unit AI. Each unit evaluates enemies, movement, attacks, escape, and patrol independently. This produces simple behavior: several units can over-focus the same weak target, ignore allies' line of fire, move without a shared plan, or fail to use specialists as a team.

The target architecture is:

- `FactionAI` owns the faction-wide tactical plan.
- `PlayerFactionAI` executes the assigned task for one unit.
- Every player unit can use shared information about all allied units, visible enemies, assigned targets, dangerous tiles, and planned movement.
- Existing per-unit logic remains as fallback while faction planning is introduced step by step.

## Current State

The new faction split already exists:

- `FactionAI` selects which AI implementation belongs to each faction.
- `PlayerFactionAI` is a copy of the old base `AIModule`.
- `HostileFactionAI` is a separate copy for hostile units.
- `NeutralFactionAI` is a separate copy for neutral units.

Current flow:

1. `BattlescapeGame::handleAI(unit)` asks `FactionAI` for the unit AI.
2. `FactionAI::think(unit, action)` currently delegates to `unit->think(action)`.
3. `BattleUnit::think()` calls the unit's `AIModule`.
4. `PlayerFactionAI::think()` does all decision making locally for that one unit.

Important current player AI decision points:

- `PlayerFactionAI::think()` initializes state, calculates visible enemies, builds attack/patrol/escape candidates, then chooses an AI mode.
- `PlayerFactionAI::selectNearestTarget()` chooses the closest visible valid target for this unit.
- `PlayerFactionAI::setupAttack()` chooses psi, blaster, sniper, grenade, melee, projectile, or movement-to-firepoint actions.
- `PlayerFactionAI::evaluateAIMode()` chooses patrol, ambush, combat, or escape using local unit state and randomness.
- `PlayerFactionAI::findFirePoint()` searches a useful firing position for the current unit.
- `PlayerFactionAI::setupEscape()` searches a safer tile for the current unit.

The main limitation: there is no shared player-side plan. Every unit independently sees the tactical world.

## Proposed Architecture

### Faction-Level Plan

Add a persistent player plan to `FactionAI`.

Suggested structures:

```cpp
struct FactionUnitInfo
{
	BattleUnit *unit;
	Position position;
	int timeUnits;
	int health;
	bool canShoot;
	bool canMelee;
	bool hasGrenade;
	bool hasPsi;
	bool wounded;
	bool out;
};

struct EnemyContact
{
	BattleUnit *enemy;
	Position position;
	std::vector<BattleUnit*> visibleBy;
	std::vector<BattleUnit*> canShootBy;
	int threatScore;
	int focusScore;
};

enum PlayerTaskType
{
	TASK_NONE,
	TASK_SHOOT,
	TASK_MOVE_TO_FIREPOINT,
	TASK_FLANK,
	TASK_HEAL,
	TASK_PICKUP_WEAPON,
	TASK_THROW_GRENADE,
	TASK_PSI,
	TASK_MELEE,
	TASK_ESCAPE,
	TASK_RESERVE,
	TASK_END_TURN
};

struct PlayerTask
{
	PlayerTaskType type;
	BattleUnit *actor;
	BattleUnit *targetUnit;
	Position targetPosition;
	BattleItem *weapon;
	int priority;
	std::string reason;
};

struct PlayerFactionPlan
{
	int turn;
	int actionCycle;
	std::vector<FactionUnitInfo> allies;
	std::vector<EnemyContact> enemies;
	std::map<int, PlayerTask> taskByUnitId;
	std::map<int, int> assignedTargetByUnitId;
	std::map<int, Position> assignedMoveByUnitId;
};
```

The exact containers can be adjusted to existing project style, but the key point is that `FactionAI` stores one shared plan for the player faction.

### Plan Lifecycle

`FactionAI` should rebuild or refresh the player plan when needed:

- At the start of the player faction turn.
- After a unit moves.
- After a unit fires or throws.
- After a unit dies, falls unconscious, panics, or changes faction.
- After visibility/FOV changes.
- When an assigned task becomes invalid.

Initial safe version:

- Rebuild the plan at the start of every `FactionAI::think()` for `FACTION_PLAYER`.
- Later optimize by caching and invalidating only on state changes.

## Implementation Phases

### Phase 1: Shared Target Selection

This is the safest first improvement.

Add to `FactionAI`:

- collect all active player units;
- collect all visible/known enemy contacts;
- calculate which player units can see or shoot each enemy;
- assign a preferred target to each player unit.

Target assignment rules for the first version:

1. Prefer enemies that are visible to several allies.
2. Prefer enemies that can be shot immediately.
3. Prefer dangerous enemies:
   - armed enemies;
   - enemies close to player units;
   - enemies with high health or melee threat;
   - enemies spotting many player units.
4. Avoid assigning too many units to a nearly dead or low-priority enemy.
5. Prefer targets with better chance to hit and lower friendly-fire risk.

Change `PlayerFactionAI::selectNearestTarget()`:

- ask `FactionAI` for this unit's assigned target;
- if assigned target is valid and visible/shootable, use it;
- otherwise fall back to the old nearest-target logic.

Expected effect:

- player units focus fire more intelligently;
- fewer wasted actions;
- old behavior remains available when the plan cannot decide.

### Phase 2: Roles

Add role assignment before choosing tasks.

Suggested roles:

- `Shooter`: stays or moves to shoot assigned target.
- `Assault`: closes distance, uses melee/short-range weapons, flanks.
- `Spotter`: moves to reveal enemies for snipers/blasters.
- `Medic`: heals wounded or fatal-wounded allies.
- `Grenadier`: throws grenade when enemy cluster is valuable and safe.
- `Psi`: uses panic/mind-control against high-value targets.
- `Reserve`: keeps TUs for reaction or holds position.
- `Escape`: retreats when exposed or badly wounded.

Role assignment should use all allied units:

- wounded allies create medic tasks;
- visible enemy clusters create grenadier tasks;
- high-accuracy shooters get priority shooting tasks;
- low-health units avoid assault roles;
- units with no good attack become spotters or reserve.

### Phase 3: Faction Task Queue

Instead of each unit deciding from scratch, `FactionAI` should create a task for each unit.

Example:

```text
unit #3: TASK_SHOOT enemy #14, reason: 72 hit score, assigned focus target
unit #5: TASK_THROW_GRENADE position (42,31,0), reason: 2 enemies in blast, no allies
unit #7: TASK_HEAL ally #2, reason: fatal wounds and reachable
unit #9: TASK_MOVE_TO_FIREPOINT position (38,29,0), reason: can shoot enemy #14 after moving
```

`PlayerFactionAI` becomes an executor:

- if task is `TASK_SHOOT`, validate weapon, line of fire, TU, and fire mode;
- if task is `TASK_MOVE_TO_FIREPOINT`, move to assigned position;
- if task is `TASK_HEAL`, execute medikit logic;
- if task is invalid, request a fallback or use old logic.

This keeps the risky action execution inside existing battle mechanics while moving strategic choice into `FactionAI`.

### Phase 4: Shared Danger Map

Add a faction-level tile score map.

For each relevant tile, estimate:

- enemy line of fire;
- number of enemies that can see/shoot the tile;
- number of allies that can support the tile;
- distance to assigned target;
- cover or concealment;
- fire/smoke/dangerous tile penalties;
- whether the tile blocks another ally's path or line of fire;
- whether the tile is inside blast radius of planned grenade/explosive actions.

Use this map in:

- `findFirePoint()`;
- `setupEscape()`;
- flanking movement;
- spotter movement;
- reserve positioning.

Initial implementation can score only a local radius around each unit. Full-map scoring can come later if performance is acceptable.

### Phase 5: Coordinated Movement

Movement should avoid blocking allies.

Rules:

- Do not assign two units to the same destination.
- Avoid moving through a tile already planned as another unit's destination.
- Prefer positions with line of fire that do not cross allied units.
- Keep melee units from standing directly in front of shooters unless the melee attack happens immediately.
- Keep wounded units behind healthier units when possible.

Store planned destinations in `PlayerFactionPlan::assignedMoveByUnitId`.

### Phase 6: Better Logging

The autobattle log should include faction-level reasoning.

Recommended text log entries:

```text
Faction plan turn=3 cycle=12
Allies active=7 wounded=2 shooters=5 medics=1 grenadiers=2
Enemy #14 score threat=86 focus=122 visibleBy=[1,3,5] canShootBy=[3,5]
Unit #3 role=Shooter task=Shoot target=#14 reason=best hit score 72, focus fire
Unit #5 role=Grenadier task=Throw target=(42,31,0) reason=2 enemies in radius, allies safe
Unit #7 role=Medic task=Heal ally=#2 reason=fatal wounds reachable
```

Recommended JSONL fields:

```json
{
  "event": "faction_plan_unit_task",
  "turn": 3,
  "cycle": 12,
  "unit_id": 3,
  "role": "Shooter",
  "task": "Shoot",
  "target_unit_id": 14,
  "priority": 92,
  "reason": "best hit score 72, focus fire"
}
```

This is important because the AI will become harder to reason about without structured logs.

## First Concrete Code Changes

Recommended first implementation step:

1. Extend `FactionAI.h` with player plan structs.
2. Add `FactionAI::buildPlayerPlan()`.
3. Add `FactionAI::getAssignedTarget(BattleUnit *unit)`.
4. In `FactionAI::think()`, build the player plan before calling the player unit AI.
5. Add a pointer/reference from `PlayerFactionAI` to `FactionAI`, or expose the current player plan through a small interface.
6. Change `PlayerFactionAI::selectNearestTarget()` to prefer `FactionAI` assigned target.
7. Add logging for target assignment.
8. Build and test with the existing `1.sav` autobattle.

Safe fallback rule:

If the faction plan gives no valid target or task, `PlayerFactionAI` must use the old copied logic unchanged.

## Design Notes

### Keep `PlayerFactionAI` as Executor

Do not immediately delete the old AI logic. It contains a lot of battle-specific validation:

- weapon usability;
- TU costs;
- melee range;
- psi conditions;
- grenade and explosive checks;
- blaster/waypoint logic;
- pathfinding details;
- medikit logic.

The safer path is to make `FactionAI` decide intent and let `PlayerFactionAI` execute it using existing mechanics.

### Avoid Big Rewrites at First

The project already has many side effects around visibility, FOV, pathfinding, TU spending, animation states, and battle actions. A large rewrite would be fragile.

Use incremental replacement:

1. shared target assignment;
2. role assignment;
3. task execution;
4. shared danger map;
5. coordinated movement.

### Performance

Faction planning can become expensive if it repeatedly tests every unit, enemy, weapon, and tile.

Initial limits:

- score only active player units;
- score only visible or known enemies;
- score movement positions within a small radius;
- cache per-cycle line-of-fire and visibility checks;
- rebuild the plan only when needed after the simple version works.

## Success Criteria

The player faction AI is improved when:

- multiple units coordinate fire on high-value targets;
- units stop over-assigning attacks to weak/dead targets;
- medics reliably prioritize wounded allies;
- grenades are used only when allies are safe;
- movement positions do not block allied fire as often;
- wounded units retreat or avoid exposed tiles;
- logs clearly explain why each unit acted.

## Loss-Minimization Focus

The next development priority is not just winning the battle, but reducing X-COM casualties. Hard saves like `1.sav` have weak armor and weak weapons, so a single exposed move can cost a soldier.

Immediate tactical goals:

- Prefer keeping a soldier alive over taking a low-value shot.
- Avoid moving into tiles that known enemies can see or shoot.
- Avoid "repositioning while enemies are visible" unless the move creates an immediate shot with enough remaining TU.
- Prefer positions where other allies can support the soldier.
- Do not use explosive or arcing attacks when allies are in the blast area or likely trajectory.
- Avoid shooting when the line of fire is likely to cross an allied unit.
- Concentrate fire from the safest available shooters first, then stop exposing additional units once the target is likely dead.

Concrete next changes:

1. Add danger scoring for candidate movement tiles using enemy spotting/line-of-fire checks.
2. Use this danger score in support movement and `findFirePoint()`.
3. Reject support moves into enemy-spotted tiles unless there is no safer way to keep the unit useful.
4. Make firepoint movement require low exposure after the move, especially for weakly armored soldiers.
5. Add structured log lines for danger decisions: selected position, enemy spotters, reason accepted/rejected.
6. Add friendly-fire checks for direct shots and explosive trajectories.
7. Add end-of-turn survival logic: if no good shot exists, hold/kneel/reserve instead of moving under threat.

Explosive weapon safety:

- Use the weapon/ammo explosion radius from item rules instead of hardcoded rocket values.
- Estimate the actual projectile origin, target voxel, central trajectory impact, and several deviation samples derived from the soldier's current firing accuracy and weapon range limits.
- Reject player-faction explosive shots when the central path crosses an ally or detonates on the shooter.
- Treat spread/blast danger as a sampled risk, not as a blanket ban on every possible miss.
- Current `1.sav` finding: the old winning run depended on a rocket that killed its own shooter. Blocking that self-hit prevents friendly fire, but the faction then loses too much tempo. Next work should add safe repositioning and stronger focus-fire so the team can win without suicide rockets.

Stalking, Rooms, And Ambushes:

- Track enemy contact zones at faction level, but do not run expensive room flood-fill inside every unit action. The first attempt caused instability during repeated planning in the same turn.
- Use a safer room approximation first: group recently seen enemies by local sectors or cached connected zones, then refine later.
- A first `stalk ambush` action was added for player units with known-but-not-visible enemies. It scores same-level reachable tiles by distance to contact, enemy spotters, and nearby cover.
- Testing showed that loose ambush movement increases enemy shots and worsens `1.sav`. Current thresholds are intentionally strict, so the behavior is available but does not fire unless the tile is clearly better.
- Next iteration should add persistent contact memory: last seen position, turn seen, number of allies who saw it, and whether enemies were clustered there.
- For room entry, assign roles at faction level: spotter near doorway, shooters in covered overwatch positions, and only then advance one unit.
