/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <climits>
#include <algorithm>
#include <sstream>
#include <vector>
#include "PlayerFactionAI.h"
#include "FactionAI.h"
#include "PlayerFactionPlanner.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/BattleUnitStatistics.h"
#include "../Savegame/Node.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "TileEngine.h"
#include "BattlescapeState.h"
#include "Projectile.h"
#include "ProjectileFlyBState.h"
#include "../Savegame/Tile.h"
#include "Pathfinding.h"
#include "../Engine/RNG.h"
#include "../Engine/Logger.h"
#include "../Engine/Game.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleItem.h"
#include "../fmath.h"

namespace OpenXcom
{

namespace
{
struct PendingPlayerGrenadeDanger
{
	SavedBattleGame *save;
	int turn;
	UnitFaction faction;
	Position target;
	int radius;
	int power;
	bool reservesTargets;
	int throwerId;
	BattleItem *item;
};

std::vector<PendingPlayerGrenadeDanger> pendingPlayerGrenadeDangers;

struct ActivePlayerExplosiveHazard
{
	SavedBattleGame *save;
	Position target;
	int radius;
	bool proximity;
};

// Refreshed once at the start of every player-AI think cycle.  Candidate
// scoring asks about explosive danger thousands of times, so rescanning the
// entire map in every query is both wasteful and prone to batch timeouts.
std::vector<ActivePlayerExplosiveHazard> activePlayerExplosiveHazards;

struct PendingPlayerProximityMinePlan
{
	SavedBattleGame *save;
	int turn;
	UnitFaction faction;
	Position contact;
	Position target;
};

std::vector<PendingPlayerProximityMinePlan> pendingPlayerProximityMinePlans;

struct PendingPlayerProximityMineStaging
{
	SavedBattleGame *save;
	int turn;
	int unitId;
	UnitFaction faction;
	Position contact;
	Position target;
};

std::vector<PendingPlayerProximityMineStaging> pendingPlayerProximityMineStagings;

struct PendingPlayerSmokePlan
{
	SavedBattleGame *save;
	int turn;
	UnitFaction faction;
	Position contact;
	Position target;
};

std::vector<PendingPlayerSmokePlan> pendingPlayerSmokePlans;

struct PlayerMovementMemory
{
	SavedBattleGame *save;
	int unitId;
	Position previousPosition;
	Position lastPosition;
	int lastTurn;
	int oscillation;
	int turnMoveCount;
	int turnMoveDistance;
};

std::vector<PlayerMovementMemory> playerMovementMemory;

struct PlayerTurnActionMemory
{
	SavedBattleGame *save;
	int unitId;
	int turn;
	int lastShotsFired;
	bool firedThisTurn;
};

struct PlayerReactionHoldMemory
{
	SavedBattleGame *save;
	int turn;
	int targetId;
	int unitId;
};

enum PlayerDynamicGroupRole
{
	PDGR_POINT,
	PDGR_FIRE,
	PDGR_GUARD
};

enum PlayerDynamicGroupTask
{
	PDGT_MANEUVER,
	PDGT_FIRE_SUPPORT,
	PDGT_RESERVE
};

struct PlayerDynamicGroupInfo
{
	int groupId;
	int groupSize;
	PlayerDynamicGroupRole role;
	PlayerDynamicGroupTask task;
	bool maneuverGroup;
};

struct PlayerDynamicGroupLayoutMemory
{
	SavedBattleGame *save;
	int turn;
	std::vector<int> activeUnitIds;
	std::vector<int> spatialUnitIds;
};

std::vector<PlayerTurnActionMemory> playerTurnActionMemory;
std::vector<PlayerReactionHoldMemory> playerReactionHoldMemory;
std::vector<PlayerDynamicGroupLayoutMemory> playerDynamicGroupLayoutMemory;

std::vector<SavedBattleGame*> playerInitialBattleLogs;

constexpr int PLAYER_AI_PERCENT = 110; // База для процентных расчетов.
constexpr int PLAYER_AI_REJECT_SCORE = -100000; // Sentinel-score для заведомо неприемлемого варианта.
constexpr int PLAYER_AI_MEMORY_LIMIT_SMALL = 64; // Лимит коротких списков памяти/планов, чтобы они не росли между боями.
constexpr int PLAYER_AI_MEMORY_LIMIT_LARGE = 512; // Лимит длинных списков памяти движения/действий юнитов.
constexpr int PLAYER_AI_NO_THROWER_RANK = 999; // Sentinel-rank для юнита, который не участвует в выборе бросающего взрывчатку.
constexpr int PLAYER_AI_WOUNDED_HEALTH_LIMIT = 35; // Здоровье цели, ниже которого она считается раненой для добивания/фокуса.
constexpr int PLAYER_AI_LOW_HEALTH_PERCENT = 90; // Минимальная доля здоровья для роли scout.
constexpr int PLAYER_AI_MARKSMAN_AIMED_ACCURACY = 100; // Aimed accuracy оружия, достаточная для роли marksman.
constexpr int PLAYER_AI_MARKSMAN_SNAP_ACCURACY = 60; // Snap accuracy оружия, достаточная для роли marksman без auto-fire.
constexpr int PLAYER_AI_MARKSMAN_FIRING = 45; // Минимальный firing юнита для роли marksman.
constexpr int PLAYER_AI_ASSAULT_TU = 70; // TU-порог для роли assault.
constexpr int PLAYER_AI_ASSAULT_REACTIONS = 70; // Reactions-порог для роли assault.
constexpr int PLAYER_AI_REACTION_SPECIALIST_MIN_REACTIONS = 55; // Минимальные reactions для reaction guard/reaction specialist.
constexpr int PLAYER_AI_REACTION_SPECIALIST_MIN_TU = 55; // Минимальные TU для reaction guard/reaction specialist.
constexpr int PLAYER_AI_HEAVY_WEAPON_DANGER = 145; // Оценка оружия врага, с которой оно считается тяжелой угрозой.
constexpr int PLAYER_AI_DURABLE_TARGET_HEALTH = 70; // Health-порог крепкой цели для взрывов и оценки урона.
constexpr int PLAYER_AI_VERY_DURABLE_TARGET_HEALTH = 100; // Health-порог особо крепкой цели.
constexpr int PLAYER_AI_DURABLE_ARMOR = 35; // Armor-порог крепкой цели.
constexpr int PLAYER_AI_STRONG_ARMOR = 30; // Armor-порог средней защищенности.
constexpr int PLAYER_AI_HIGH_EXPOSURE = 90; // Exposure, при котором позиция считается сильно опасной.
constexpr int PLAYER_AI_FIRELINE_BREAK_BONUS = 220; // Бонус fallback-позиции за полный разрыв enemy fire line.
constexpr int PLAYER_AI_NEARBY_SPOTTER_LIMIT = 2; // Число spotters, после которого ситуация считается срочной.
constexpr int PLAYER_AI_HEAVY_ENEMY_COUNT = 12; // Масса врагов, при которой включается тяжелая взрывная доктрина.
constexpr int PLAYER_AI_HIDDEN_ENEMY_COUNT = 10; // Число известных скрытых врагов для heavy landing/initial pressure.
constexpr int PLAYER_AI_HIGH_HP_EXPLOSIVE_TARGET = 80; // Health врага, с которого стоит беречь/искать сильную взрывчатку.
constexpr int PLAYER_AI_SLOW_DANGER_EXPLOSIVE_MIN_HEALTH = 40; // Нижняя граница health для медленного, но опасного взрывного таргета.
constexpr int PLAYER_AI_SLOW_DANGER_MAX_TU = 52; // Верхняя граница TU для медленного взрывного таргета.
constexpr int PLAYER_AI_OVERWHELMING_TARGET_HEALTH = 100; // Health врага для признака overwhelming heavy landing.
constexpr int PLAYER_AI_CLEAN_SHOT_BASE_SCORE = 220; // Базовая оценка позиции clean-shot move с линией огня.
constexpr int PLAYER_AI_CLEAN_SHOT_MIN_SCORE = 75; // Минимальная оценка обычного clean-shot move.
constexpr int PLAYER_AI_DEFENSIVE_CLEAN_SHOT_MIN_SCORE = 110; // Минимальная оценка clean-shot move в защитных режимах.
constexpr int PLAYER_AI_CLEAN_SHOT_EXPOSURE_SPIKE = 75; // Максимально допустимый прирост exposure при clean-shot move.
constexpr int PLAYER_AI_CLEAN_SHOT_HIGH_EXPOSURE = 110; // Абсолютный high exposure cutoff при clean-shot move.
constexpr int PLAYER_AI_CLEAN_SHOT_RESERVE_TU = 14; // Минимальный TU-резерв после движения к позиции выстрела.
constexpr int PLAYER_AI_CLEAN_SHOT_VISIBLE_RETURN_RESERVE = 14; // Дополнительный резерв, когда цель видима/позиция под контактом.
constexpr int PLAYER_AI_CLEAN_SHOT_HIDDEN_RETURN_RESERVE = 8; // Минимальный резерв для скрытого контакта или melee fallback.
constexpr int PLAYER_AI_CLEAN_SHOT_CAUTIOUS_HUNT_DISTANCE = 18; // Дистанция, после которой hunt-last-enemy становится осторожнее.
constexpr int PLAYER_AI_AMBUSH_BASE_SYSTEMATIC_SUCCESS = 100; // Базовый score node-засады.
constexpr int PLAYER_AI_AMBUSH_COVER_BONUS = 25; // Бонус node-засады за окно/укрытие.
constexpr int PLAYER_AI_AMBUSH_FAST_PASS_THRESHOLD = 80; // Score node-засады, при котором поиск можно завершить рано.
constexpr int PLAYER_AI_ESCAPE_EXPOSURE_PENALTY = 10; // Штраф/бонус escape за каждого дополнительного/убранного spotter.
constexpr int PLAYER_AI_ESCAPE_FIRE_PENALTY = 40; // Штраф escape за тайл с огнем.
constexpr int PLAYER_AI_ESCAPE_BASE_SYSTEMATIC_SUCCESS = 100; // Базовый score систематического поиска escape-позиции.
constexpr int PLAYER_AI_ESCAPE_BASE_DESPERATE_SUCCESS = 110; // Базовый score desperate escape-поиска.
constexpr int PLAYER_AI_ESCAPE_FAST_PASS_THRESHOLD = 100; // Score escape-позиции, при котором поиск можно завершить рано.
constexpr int PLAYER_AI_FIREPOINT_BASE_SYSTEMATIC_SUCCESS = 100; // Базовый score firepoint-позиции.
constexpr int PLAYER_AI_FIREPOINT_FAST_PASS_THRESHOLD = 125; // Score firepoint-позиции, при котором поиск можно завершить рано.
constexpr int PLAYER_AI_FALLBACK_URGENT_EXPOSURE = 140; // Exposure, с которого fallback cover move расширяет радиус поиска.
constexpr int PLAYER_AI_FALLBACK_EMERGENCY_SCORE = 28; // minScore, ниже которого fallback считается emergency move.
constexpr int PLAYER_AI_FALLBACK_MAX_SAFE_MOVE = 10; // Максимальная дальность fallback при срочном разрыве линии огня.
constexpr int PLAYER_AI_FALLBACK_MAX_NORMAL_MOVE = 4; // Максимальная дальность обычного fallback.
constexpr int PLAYER_AI_FALLBACK_MAX_EMERGENCY_MOVE = 6; // Максимальная дальность emergency fallback.
constexpr int PLAYER_AI_STALK_BROKEN_ROOM_TILE_LIMIT = 90; // Размер комнаты, после которого stalk ambush считает ее разомкнутой/сломанной.
constexpr int PLAYER_AI_STALK_RISKY_ROOM_TILE_LIMIT = 16; // Размер малой комнаты, выше которого entry считается рискованным.
constexpr int PLAYER_AI_STALK_HALL_ROOM_SIZE_BONUS_LIMIT = 60; // Размер контакта, добавляющий room pressure в stalk ambush.
constexpr int PLAYER_AI_STALK_RISKY_MIN_SCORE = 115; // Минимальный score stalk ambush около рискованной комнаты.
constexpr int PLAYER_AI_STALK_NORMAL_MIN_SCORE = 155; // Минимальный score stalk ambush вне рискованной комнаты.
constexpr int PLAYER_AI_GRENADE_TEAM_MIN_SCORE_DEFAULT = 160; // Базовый minScore team-spotted explosive.
constexpr int PLAYER_AI_GRENADE_TEAM_MIN_SCORE_CLUSTER = 120; // MinScore team-spotted explosive по кластеру врагов.
constexpr int PLAYER_AI_GRENADE_TEAM_MIN_SCORE_HIGH = 130; // MinScore team-spotted explosive по очень ценной одиночной цели.
constexpr int PLAYER_AI_GRENADE_TEAM_MIN_SCORE_MEDIUM = 145; // MinScore team-spotted explosive по средней ценной цели.
constexpr int PLAYER_AI_GRENADE_HIGH_HEALTH = 80; // Health, с которого цель считается высокоценной для гранаты.
constexpr int PLAYER_AI_GRENADE_HIGH_VALUE_WEAPON_DANGER = 105; // Опасность оружия врага для high-value grenade target.
constexpr int PLAYER_AI_GRENADE_SCATTER_PENALTY = 180; // Штраф grenade score за риск scatter по союзникам.
constexpr int PLAYER_AI_GRENADE_EVACUATABLE_ALLY_PENALTY = 25; // Штраф за союзника в blast radius, который теоретически может уйти.
constexpr int PLAYER_AI_GRENADE_RISKY_EVACUATION_PENALTY = 160; // Дополнительный штраф за рискованную эвакуацию союзника из blast.
constexpr int PLAYER_AI_WEAPON_DANGER_RADIUS_BONUS = 12; // Вклад blast radius в danger score оружия.
constexpr int PLAYER_AI_THROW_DISTANCE_BIAS = 8; // Смещение старой формулы дальности броска перед переводом voxel distance в tile distance.
constexpr double PLAYER_AI_THROW_DISTANCE_TILE_SCALE = 16.0; // Перевод throw-distance из voxel-like единиц в клетки.
constexpr double PLAYER_AI_THROW_UNDERWATER_FACTOR_BASE = 100.0; // База процентного underwater throw factor.
constexpr int PLAYER_AI_THROW_TARGET_VOXEL_XY = 8; // Центр tile по X/Y для проверки траектории броска.
constexpr int PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE = 2; // Базовая высота target voxel над terrain level.
constexpr double PLAYER_AI_THROW_MIN_CURVATURE = 0.48; // Минимальная кривизна параболы броска.
constexpr double PLAYER_AI_THROW_CURVATURE_STRENGTH_FACTOR = 1.73; // Вес strength/weight в стартовой кривизне броска.
constexpr double PLAYER_AI_THROW_KNEEL_CURVATURE_BONUS = 0.1; // Добавка к кривизне броска для kneeling unit.
constexpr double PLAYER_AI_THROW_MAX_CURVATURE = 5.0; // Верхняя граница перебора кривизны траектории.
constexpr double PLAYER_AI_THROW_CURVATURE_STEP = 0.5; // Шаг перебора кривизны траектории.
constexpr int PLAYER_AI_PICKUP_TU_COST = 4; // Hardcoded TU за поднятие предмета перед использованием/броском.
constexpr int PLAYER_AI_EXPLOSIVE_POWER_SCORE = 2; // Вес power в выборе лучшей взрывчатки.
constexpr int PLAYER_AI_EXPLOSIVE_RADIUS_SCORE = 25; // Вес radius в выборе лучшей взрывчатки.
constexpr int PLAYER_AI_THROWER_THROWING_SCORE = 4; // Вес throwing skill при ранжировании носителей взрывчатки.
constexpr int PLAYER_AI_THROWER_TU_SCORE = 2; // Вес TU при ранжировании носителей взрывчатки.
constexpr int PLAYER_AI_THROWER_RADIUS_SCORE = 20; // Вес blast radius при ранжировании носителей взрывчатки.
constexpr int PLAYER_AI_PENDING_GRENADE_MEMORY_TURNS = 4; // Сколько ходов pending grenade danger считается актуальным.
constexpr int PLAYER_AI_PENDING_MINE_MEMORY_TURNS = 3; // Сколько ходов proximity mine plan/staging считается актуальным.
constexpr int PLAYER_AI_PROXIMITY_DEFAULT_TARGET_SPACING = 7; // Дистанция разделения proximity mine target по умолчанию.
constexpr int PLAYER_AI_PROXIMITY_CONTACT_MEMORY_DISTANCE = 8; // Максимальная дистанция совпадения contact в памяти proximity mine.
constexpr int PLAYER_AI_PROXIMITY_TARGET_MEMORY_DISTANCE = 7; // Максимальная дистанция совпадения target в памяти proximity mine.
constexpr int PLAYER_AI_MOVEMENT_OSCILLATION_DECAY = 1; // На сколько снижается oscillation при нормальном движении/новом ходу.
constexpr int PLAYER_AI_MOVEMENT_OSCILLATION_GAIN = 2; // На сколько растет oscillation при возврате между двумя позициями.
constexpr int PLAYER_AI_INITIAL_CLOSEST_DISTANCE = 100; // Начальный closest distance до поиска ближайшей цели.
constexpr float PLAYER_AI_LEEROY_RUN_STAMINA_FRACTION = 0.4f; // Доля stamina, выше которой mindless melee unit может бежать.
constexpr int PLAYER_AI_MEDIKIT_LOW_ENERGY_PERCENT = 40; // Energy percent, ниже которого AI хочет stimulant.
constexpr int PLAYER_AI_MEDIKIT_HEAL_BASE_CHANCE = 120; // Базовый шанс heal при нулевом effective health.
constexpr int PLAYER_AI_MEDIKIT_HEAL_HEALTH_SCALE = 4; // Снижение heal chance за процент effective health.
constexpr int PLAYER_AI_MEDIKIT_RANDOM_HEAL_CHANCE = 5; // Минимальный случайный heal wish при не срочной ране.
constexpr int PLAYER_AI_MEDIKIT_STUN_BASE_CHANCE = 140; // Базовый шанс stimulant против stun при нулевом health percent.
constexpr int PLAYER_AI_MEDIKIT_STUN_HEALTH_SCALE = 7; // Снижение stun stimulant chance за процент health.
constexpr int PLAYER_AI_MEDIKIT_ENERGY_BASE_CHANCE = 120; // Базовый шанс stimulant для энергии при нулевой energy.
constexpr int PLAYER_AI_MEDIKIT_ENERGY_SCALE = 3; // Снижение energy stimulant chance за процент energy.
constexpr int PLAYER_AI_REACTION_FIRING_TOLERANCE = 8; // Насколько reactions могут отставать/обгонять firing для reaction guard.
constexpr int PLAYER_AI_SCOUT_TU_SCORE = 2; // Вес TU в ранжировании scout-кандидатов.
constexpr int PLAYER_AI_SCOUT_HEALTH_SCORE = 80; // Вес текущего здоровья в scout score.
constexpr int PLAYER_AI_SCOUT_SCORE_MARGIN = 8; // Преимущество другого бойца, чтобы считать его лучшим scout.
constexpr int PLAYER_AI_SCOUT_BETTER_LIMIT = 2; // Сколько лучших scout-кандидатов допускается перед отказом от scout роли.
constexpr int PLAYER_AI_ODDS_INITIAL_ESCAPE = 80; // Множитель escape odds для initial deploy.
constexpr int PLAYER_AI_ODDS_INITIAL_AMBUSH = 230; // Множитель ambush odds для initial deploy.
constexpr int PLAYER_AI_ODDS_INITIAL_COMBAT = 65; // Множитель combat odds для initial deploy.
constexpr int PLAYER_AI_ODDS_INITIAL_PATROL = 18; // Множитель patrol odds для initial deploy.
constexpr int PLAYER_AI_ODDS_INITIAL_FIRE_SUPPORT_AMBUSH = 125; // Доп. ambush множитель marksman/heavy в initial deploy.
constexpr int PLAYER_AI_ODDS_INITIAL_ASSAULT_COMBAT = 110; // Доп. combat множитель assault в initial deploy.
constexpr int PLAYER_AI_ODDS_DEFEND_ESCAPE = 100; // Множитель escape odds для defend line.
constexpr int PLAYER_AI_ODDS_DEFEND_AMBUSH = 180; // Множитель ambush odds для defend line.
constexpr int PLAYER_AI_ODDS_DEFEND_COMBAT = 95; // Множитель combat odds для defend line.
constexpr int PLAYER_AI_ODDS_DEFEND_PATROL = 45; // Множитель patrol odds для defend line.
constexpr int PLAYER_AI_ODDS_DEFEND_FIRE_SUPPORT_AMBUSH = 125; // Доп. ambush множитель marksman/heavy в defend line.
constexpr int PLAYER_AI_ODDS_DEFEND_ASSAULT_COMBAT = 115; // Доп. combat множитель assault в defend line.
constexpr int PLAYER_AI_ODDS_SIEGE_ESCAPE = 100; // Множитель escape odds для siege room.
constexpr int PLAYER_AI_ODDS_SIEGE_AMBUSH = 210; // Множитель ambush odds для siege room.
constexpr int PLAYER_AI_ODDS_SIEGE_COMBAT = 90; // Множитель combat odds для siege room.
constexpr int PLAYER_AI_ODDS_SIEGE_PATROL = 30; // Множитель patrol odds для siege room.
constexpr int PLAYER_AI_ODDS_SIEGE_ENTRY_AMBUSH = 125; // Доп. ambush множитель assault/melee в siege room.
constexpr int PLAYER_AI_ODDS_SIEGE_MARKSMAN_COMBAT = 80; // Доп. combat множитель marksman в siege room.
constexpr int PLAYER_AI_ODDS_HOLD_ESCAPE = 95; // Множитель escape odds для hold reaction.
constexpr int PLAYER_AI_ODDS_HOLD_AMBUSH = 220; // Множитель ambush odds для hold reaction.
constexpr int PLAYER_AI_ODDS_HOLD_COMBAT = 70; // Множитель combat odds для hold reaction.
constexpr int PLAYER_AI_ODDS_HOLD_PATROL = 20; // Множитель patrol odds для hold reaction.
constexpr int PLAYER_AI_ODDS_HOLD_SUPPORT_AMBUSH = 120; // Доп. ambush множитель support/marksman в hold reaction.
constexpr int PLAYER_AI_ODDS_ASSAULT_ESCAPE = 70; // Множитель escape odds для assault.
constexpr int PLAYER_AI_ODDS_ASSAULT_AMBUSH = 75; // Множитель ambush odds для assault.
constexpr int PLAYER_AI_ODDS_ASSAULT_COMBAT = 160; // Множитель combat odds для assault.
constexpr int PLAYER_AI_ODDS_ASSAULT_PATROL = 65; // Множитель patrol odds для assault.
constexpr int PLAYER_AI_ODDS_ASSAULT_FRONT_COMBAT = 125; // Доп. combat множитель assault/melee в assault.
constexpr int PLAYER_AI_ODDS_ASSAULT_FIRE_SUPPORT_COMBAT = 110; // Доп. combat множитель marksman/heavy в assault.
constexpr int PLAYER_AI_ODDS_SURVIVE_CONTACT_ESCAPE = 230; // Escape odds survive при видимом/spotting контакте.
constexpr int PLAYER_AI_ODDS_SURVIVE_SAFE_ESCAPE = 95; // Escape odds survive без прямого контакта.
constexpr int PLAYER_AI_ODDS_SURVIVE_CONTACT_AMBUSH = 145; // Ambush odds survive при прямом контакте.
constexpr int PLAYER_AI_ODDS_SURVIVE_SAFE_AMBUSH = 220; // Ambush odds survive без прямого контакта.
constexpr int PLAYER_AI_ODDS_SURVIVE_COMBAT = 50; // Combat odds survive.
constexpr int PLAYER_AI_ODDS_SURVIVE_SUPPORT_ESCAPE = 120; // Доп. escape множитель support/heavy в survive.
constexpr int PLAYER_AI_ODDS_SURVIVE_ASSAULT_AMBUSH = 115; // Доп. ambush множитель assault в survive.
constexpr int PLAYER_AI_ODDS_SKIRMISH_ESCAPE = 105; // Умеренный отход в режиме боя малыми группами.
constexpr int PLAYER_AI_ODDS_SKIRMISH_AMBUSH = 165; // Засада/удержание укрытия между короткими огневыми контактами.
constexpr int PLAYER_AI_ODDS_SKIRMISH_COMBAT = 135; // Достаточное давление, чтобы меньшинство не переставало наносить урон.
constexpr int PLAYER_AI_ODDS_SKIRMISH_PATROL = 30; // Минимум открытого патрулирования при численном меньшинстве.
constexpr int PLAYER_AI_ODDS_SKIRMISH_SUPPORT_AMBUSH = 120; // Fire-support чаще удерживает дальнее укрытие.
constexpr int PLAYER_AI_ODDS_SKIRMISH_FRONT_COMBAT = 115; // Assault/scout сохраняют возможность короткой атаки.
constexpr int PLAYER_AI_ODDS_RETREAT_ESCAPE = 300; // Множитель escape odds для retreat regroup.
constexpr int PLAYER_AI_ODDS_RETREAT_AMBUSH = 75; // Множитель ambush odds для retreat regroup.
constexpr int PLAYER_AI_ODDS_RETREAT_COMBAT = 35; // Множитель combat odds для retreat regroup.
constexpr int PLAYER_AI_ODDS_RETREAT_FRONT_AMBUSH = 115; // Доп. ambush множитель assault/melee в retreat regroup.
constexpr int PLAYER_AI_ODDS_HUNT_ESCAPE = 70; // Множитель escape odds для hunt last enemy.
constexpr int PLAYER_AI_ODDS_HUNT_VISIBLE_AMBUSH = 120; // Ambush odds hunt при видимом или последнем известном враге.
constexpr int PLAYER_AI_ODDS_HUNT_HIDDEN_AMBUSH = 160; // Ambush odds hunt при скрытом последнем враге.
constexpr int PLAYER_AI_ODDS_HUNT_VISIBLE_COMBAT = 145; // Combat odds hunt при видимом враге.
constexpr int PLAYER_AI_ODDS_HUNT_HIDDEN_COMBAT = 90; // Combat odds hunt при скрытом враге.
constexpr int PLAYER_AI_ODDS_HUNT_VISIBLE_PATROL = 90; // Patrol odds hunt при видимом враге.
constexpr int PLAYER_AI_ODDS_HUNT_HIDDEN_PATROL = 65; // Patrol odds hunt при скрытом враге.
constexpr int PLAYER_AI_ODDS_HUNT_FRONT_PRESSURE = 115; // Доп. patrol/combat множитель assault/melee в hunt.
constexpr int PLAYER_AI_ODDS_HUNT_FIRE_SUPPORT_PATROL = 75; // Доп. patrol множитель marksman/heavy в hunt.
constexpr int PLAYER_AI_ODDS_HUNT_FIRE_SUPPORT_AMBUSH = 115; // Доп. ambush множитель marksman/heavy в hunt.
constexpr int PLAYER_AI_RANGE_DEFAULT = 9; // Базовая preferred engagement range до учета роли.
constexpr int PLAYER_AI_RANGE_MARKSMAN = 13; // Preferred range для marksman.
constexpr int PLAYER_AI_RANGE_HEAVY = 12; // Preferred range для heavy.
constexpr int PLAYER_AI_RANGE_ASSAULT = 8; // Preferred range для assault.
constexpr int PLAYER_AI_RANGE_MELEE = 3; // Preferred range для melee.
constexpr int PLAYER_AI_RANGE_SUPPORT = 10; // Preferred range для support.
constexpr int PLAYER_AI_RANGE_ACCURATE_AIMED = 100; // Aimed accuracy, дающая бонус к preferred range.
constexpr int PLAYER_AI_RANGE_GOOD_FIRING = 60; // Firing skill, нужный для бонуса дальности с точным оружием.
constexpr int PLAYER_AI_RANGE_ACCURATE_BONUS = 1; // Добавка дальности за точное aimed оружие.
constexpr int PLAYER_AI_RANGE_AUTO_AIMED_LIMIT = 90; // Aimed accuracy ниже которой auto weapon считается ближним.
constexpr int PLAYER_AI_RANGE_AUTO_PENALTY = 2; // Снижение дальности для auto-оружия с неточным aimed.
constexpr int PLAYER_AI_RANGE_OVERWEIGHT_BONUS = 2; // Добавка preferred range для оружия тяжелее strength.
constexpr int PLAYER_AI_RANGE_LOW_FIRING = 45; // Firing ниже этого значения уменьшает preferred range.
constexpr int PLAYER_AI_RANGE_HIGH_FIRING = 70; // Firing выше этого значения увеличивает preferred range.
constexpr int PLAYER_AI_RANGE_LOW_FIRING_PENALTY = 2; // Штраф preferred range для низкого firing.
constexpr int PLAYER_AI_RANGE_HIGH_FIRING_BONUS = 1; // Бонус preferred range для высокого firing.
constexpr int PLAYER_AI_RANGE_MIN = 3; // Нижняя граница preferred engagement range.
constexpr int PLAYER_AI_RANGE_MAX = 15; // Верхняя граница preferred engagement range.
constexpr int PLAYER_AI_WEAPON_POWER_SCORE = 3; // Вес raw weapon power в scoreWeaponForUnit.
constexpr int PLAYER_AI_WEAPON_MIN_SKILL = 30; // Минимальный skill, используемый при оценке точности оружия.
constexpr int PLAYER_AI_WEAPON_SNAP_SCALE = 60; // Делитель snap accuracy вклада в scoreWeaponForUnit.
constexpr int PLAYER_AI_WEAPON_AIMED_SCALE = 90; // Делитель aimed accuracy вклада в scoreWeaponForUnit.
constexpr int PLAYER_AI_WEAPON_AUTO_SCALE = 80; // Делитель auto accuracy вклада в scoreWeaponForUnit.
constexpr int PLAYER_AI_WEAPON_MELEE_SCALE = 70; // Делитель melee accuracy вклада в scoreWeaponForUnit.
constexpr int PLAYER_AI_WEAPON_MELEE_PENALTY = 180; // Штраф melee weapon при выборе carried weapon.
constexpr int PLAYER_AI_WEAPON_WAYPOINT_BONUS = 80; // Бонус waypoint/blaster-like оружия.
constexpr int PLAYER_AI_WEAPON_EXPLOSIVE_RADIUS_BONUS = 18; // Бонус оружия за blast radius ammo.
constexpr int PLAYER_AI_WEAPON_OVERWEIGHT_PENALTY = 25; // Штраф за каждый weight сверх strength.
constexpr int PLAYER_AI_WEAPON_WEIGHT_PENALTY = 2; // Базовый штраф за вес оружия.
constexpr int PLAYER_AI_WEAPON_NO_AMMO_PENALTY = 250; // Штраф unloaded ammo weapon.
constexpr int PLAYER_AI_EQUIP_SAFE_REQUIRED_GAIN = 40; // Минимальный выигрыш score для смены оружия без известных врагов.
constexpr int PLAYER_AI_EQUIP_CONTACT_REQUIRED_GAIN = 160; // Минимальный выигрыш score для смены оружия при известных врагах.
constexpr int PLAYER_AI_GRENADE_REJECT_CUTOFF = -50000; // Score ниже которого grenade/proximity target считается reject.
constexpr int PLAYER_AI_GRENADE_PROXIMITY_BASE_SCORE = 20; // Базовый score proximity grenade target.
constexpr int PLAYER_AI_GRENADE_MIN_EVACUATE_TU = 8; // Минимальный TU для выхода из blast radius.
constexpr int PLAYER_AI_GRENADE_EVACUATE_TU_PER_STEP = 6; // TU на шаг эвакуации из blast radius.
constexpr int PLAYER_AI_GRENADE_RISKY_EVACUATE_EXTRA_TU = 10; // Доп. TU запас, чтобы эвакуация не считалась рискованной.
constexpr int PLAYER_AI_GRENADE_ALLY_BASE_PENALTY = 65; // Базовый штраф за союзника в blast radius.
constexpr int PLAYER_AI_GRENADE_ALLY_RADIUS_PENALTY = 18; // Доп. штраф за близость союзника к центру blast.
constexpr int PLAYER_AI_GRENADE_UNIT_BASE_SCORE = 90; // Базовый score врага в blast radius.
constexpr int PLAYER_AI_GRENADE_WOUNDED_HEALTH_BONUS_BASE = 90; // Health benchmark для бонуса по раненым целям.
constexpr int PLAYER_AI_GRENADE_DURABLE_BONUS = 110; // Бонус score за durable target.
constexpr int PLAYER_AI_GRENADE_VERY_DURABLE_BONUS = 90; // Бонус score за very durable target.
constexpr int PLAYER_AI_GRENADE_WEAPON_DANGER_BONUS_CAP = 160; // Максимальный бонус grenade score за опасное оружие врага.
constexpr int PLAYER_AI_GRENADE_WEAPON_DANGER_BONUS_BASE = 70; // База вычитания из enemy weapon danger для бонуса.
constexpr int PLAYER_AI_GRENADE_TARGET_DISTANCE_PENALTY = 8; // Штраф grenade unit score за дистанцию цели от центра blast.
constexpr int PLAYER_AI_GRENADE_OLD_EFFICACY_SCORE = 120; // Вес старой explosiveEfficacy в grenade score.
constexpr int PLAYER_AI_GRENADE_SCATTER_MIN_SCORE = 235; // Минимальный score для scatter-risk throw по неидеальной цели.
constexpr int PLAYER_AI_GRENADE_RISKY_ALLY_MIN_SCORE = 360; // Минимальный score при risky evacuatable allies.
constexpr int PLAYER_AI_GRENADE_SINGLE_EMPTY_TILE_PENALTY = 60; // Штраф single-target throw, если в target tile нет врага.
constexpr int PLAYER_AI_GRENADE_SINGLE_DURABLE_HEALTH = 60; // Health одиночной цели для первого durable bonus.
constexpr int PLAYER_AI_GRENADE_SINGLE_DURABLE_BONUS = 150; // Бонус single-target grenade за durable target.
constexpr int PLAYER_AI_GRENADE_SINGLE_FRAGILE_PENALTY = 400; // Штраф дорогого броска по одной цели, которую выгоднее снять прямым огнем.
constexpr int PLAYER_AI_THROW_DANGER_MARGIN = 1; // Запас вокруг ожидаемой точки падения с учетом scatter/соседней клетки impact.
constexpr int PLAYER_AI_PROXIMITY_LOG_TURN_LIMIT = 3; // Последний ранний ход, когда логируются proximity skips.
constexpr int PLAYER_AI_PROXIMITY_LOG_KNOWN_ENEMIES = 8; // Known enemies для early proximity diagnostics.
constexpr int PLAYER_AI_PROXIMITY_THROW_LIMIT_MIN = 6; // Минимальный throw limit для proximity/grenade throw.
constexpr int PLAYER_AI_PROXIMITY_THROW_ACCURACY_DIVISOR = 12; // Делитель accuracy в throw limit.
constexpr int PLAYER_AI_PROXIMITY_STAGED_THROW_BONUS = 1; // Доп. дальность для staged proximity throw.
constexpr int PLAYER_AI_PROXIMITY_TRIGGER_RADIUS = 1; // Движок проверяет mine только в соседних с движущимся бойцом клетках.
constexpr int PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN = 2; // Радиус поиска уже поставленной mine сверх blast radius.
constexpr int PLAYER_AI_PROXIMITY_EARLY_HOSTILE_MIN = 8; // Минимум hostile для early field mine.
constexpr int PLAYER_AI_PROXIMITY_ROOM_TILE_LIMIT = 160; // Максимальный размер обычной комнаты для mineable room.
constexpr int PLAYER_AI_PROXIMITY_ROOM_ENTRY_LIMIT = 6; // Mines pay off only at a genuinely narrow room approach.
constexpr int PLAYER_AI_PROXIMITY_HALL_TILE_LIMIT = 80; // Максимальный размер hall для mineable proximity room.
constexpr int PLAYER_AI_PROXIMITY_HALL_ENTRY_LIMIT = 4; // Open halls are too easy to route around.
constexpr int PLAYER_AI_PROXIMITY_FIELD_ENTRY_LIMIT = 8; // Entry threshold, после которого early plan считается field mine.
constexpr int PLAYER_AI_PROXIMITY_FIELD_ROOM_TILE_LIMIT = 120; // Room size threshold для field mine.
constexpr int PLAYER_AI_PROXIMITY_FIELD_ALLY_DISTANCE = 7; // Ally distance threshold для field mine.
constexpr int PLAYER_AI_PROXIMITY_WORTH_ENEMIES = 2; // Минимум enemiesInRoom для worthwhile mine contact.
constexpr int PLAYER_AI_PROXIMITY_WORTH_ENTRY_LIMIT = 3; // Entry threshold, делающий contact worthy для mine.
constexpr int PLAYER_AI_PROXIMITY_WORTH_HEALTH = 80; // Health threshold, делающий contact worthy для mine.
constexpr int PLAYER_AI_PROXIMITY_ARMOR_POWER_NUM = 2; // Числитель armor/power threshold для worthy mine contact.
constexpr int PLAYER_AI_PROXIMITY_ARMOR_POWER_DEN = 3; // Знаменатель armor/power threshold для worthy mine contact.
constexpr int PLAYER_AI_PROXIMITY_FIELD_THREAT = 120; // Threat threshold для field mine contact.
constexpr int PLAYER_AI_PROXIMITY_ROOM_THREAT = 145; // Threat threshold для room mine contact.
constexpr int PLAYER_AI_PROXIMITY_BASE_MOVE_RADIUS = 10; // Базовый enemy move radius для mine planning.
constexpr int PLAYER_AI_PROXIMITY_MOVE_RADIUS_ENEMY_CAP = 6; // Максимальная добавка enemy count к move radius.
constexpr int PLAYER_AI_PROXIMITY_MOVE_RADIUS_PER_ENEMY = 2; // Добавка move radius за врага в комнате.
constexpr int PLAYER_AI_PROXIMITY_MOVE_RADIUS_MIN = 6; // Нижняя граница move radius по TU врага.
constexpr int PLAYER_AI_PROXIMITY_MOVE_RADIUS_MAX = 18; // Верхняя граница move radius по TU врага.
constexpr int PLAYER_AI_PROXIMITY_MOVE_RADIUS_TU_DIVISOR = 4; // Делитель TU врага при оценке move radius.
constexpr int PLAYER_AI_PROXIMITY_TACTICAL_BASE_SCORE = 115; // Базовый tactical score для proximity mine candidate.
constexpr int PLAYER_AI_PROXIMITY_RADIUS_SCORE = 10; // Вклад radius в tactical score proximity mine.
constexpr int PLAYER_AI_PROXIMITY_ROOM_ENEMY_SCORE_CAP = 90; // Максимальный бонус tactical score за enemiesInRoom.
constexpr int PLAYER_AI_PROXIMITY_ROOM_ENEMY_SCORE = 40; // Бонус tactical score за врага в комнате.
constexpr int PLAYER_AI_PROXIMITY_THREAT_SCORE_CAP = 90; // Максимальный бонус tactical score за contactThreat.
constexpr int PLAYER_AI_PROXIMITY_THREAT_SCORE_DIVISOR = 3; // Делитель contactThreat для tactical score.
constexpr int PLAYER_AI_PROXIMITY_BEHIND_TARGET_PENALTY = 140; // Штраф mine candidate, если он позади направления на контакт.
constexpr int PLAYER_AI_PROXIMITY_DOT_SCORE_CAP = 95; // Максимальный бонус за продвижение вдоль линии к контакту.
constexpr int PLAYER_AI_PROXIMITY_DOT_SCORE_DIVISOR = 2; // Делитель dot product для score.
constexpr int PLAYER_AI_PROXIMITY_CROSS_PENALTY_CAP = 130; // Максимальный штраф за боковое отклонение от линии контакта.
constexpr int PLAYER_AI_PROXIMITY_CROSS_PENALTY = 3; // Множитель cross deviation penalty.
constexpr int PLAYER_AI_PROXIMITY_PATH_PROGRESS_SCORE = 8; // Бонус за продвижение mine target к contact path.
constexpr int PLAYER_AI_PROXIMITY_THROW_ACCURACY_SCORE_CAP = 60; // Максимальный бонус за throw accuracy.
constexpr int PLAYER_AI_PROXIMITY_THROW_ACCURACY_BASE = 50; // Accuracy baseline для бонуса throw accuracy.
constexpr int PLAYER_AI_PROXIMITY_NARROW_ENTRY_LIMIT = 2; // Entries threshold для narrow-entry bonus.
constexpr int PLAYER_AI_PROXIMITY_NARROW_ENTRY_BONUS = 45; // Tactical bonus за узкий вход.
constexpr int PLAYER_AI_PROXIMITY_ROOM_EXIT_BONUS = 35; // Tactical bonus за target вне той же комнаты/field mine.
constexpr int PLAYER_AI_PROXIMITY_CONTACT_OVERSHOOT_PENALTY = 15; // Штраф за mine target дальше enemy move radius.
constexpr int PLAYER_AI_PROXIMITY_CONTACT_CLOSE_LIMIT = 1; // Дистанция слишком близко к контакту.
constexpr int PLAYER_AI_PROXIMITY_CONTACT_CLOSE_PENALTY = 70; // Штраф за слишком близкий mine target.
constexpr int PLAYER_AI_PROXIMITY_FIELD_ALLY_SAFE_DISTANCE = 7; // Желаемая дистанция field mine от ally anchor.
constexpr int PLAYER_AI_PROXIMITY_FIELD_ALLY_PENALTY = 30; // Штраф за близость field mine к ally anchor.
constexpr int PLAYER_AI_PROXIMITY_NON_HALL_BONUS = 25; // Бонус proximity target в комнате, не hall.
constexpr int PLAYER_AI_PROXIMITY_LOW_THROW_ACCURACY = 65; // Accuracy threshold для too-far throw penalty.
constexpr int PLAYER_AI_PROXIMITY_FIELD_RELAXED_KNOWN_ENEMIES = 12; // Known enemies, при которых field mine разрешает дальний бросок.
constexpr int PLAYER_AI_PROXIMITY_MIN_SCORE = 125; // Минимальный score для немедленной постановки proximity mine.
constexpr int PLAYER_AI_PROXIMITY_STAGE_MIN_SCORE = 130; // Минимальный score staged mine target.
constexpr int PLAYER_AI_PROXIMITY_STAGE_MOVE_MIN_SCORE = 150; // Минимальный score движения для staged mine.
constexpr int PLAYER_AI_SMOKE_MEMORY_TURNS = 5; // Сколько ходов smoke plan считается актуальным, чтобы не задымлять одну точку повторно.
constexpr int PLAYER_AI_SMOKE_CONTACT_MEMORY_DISTANCE = 14; // Дистанция совпадения enemy contact для smoke memory.
constexpr int PLAYER_AI_SMOKE_TARGET_MEMORY_DISTANCE = 10; // Дистанция совпадения smoke target для smoke memory.
constexpr int PLAYER_AI_SMOKE_MIN_RADIUS = 2; // Минимальный radius, при котором grenade считается полезной дымовой.
constexpr int PLAYER_AI_SMOKE_PICKUP_TU_BUFFER = 0; // Дополнительный TU буфер для smoke throw сверх pickup/prime.
constexpr int PLAYER_AI_SMOKE_MIN_THROW_RESERVE = 4; // Минимальный TU остаток после броска дыма.
constexpr int PLAYER_AI_SMOKE_INITIAL_TURN_LIMIT = 3; // Ходы, когда smoke screen особо полезен для выхода из стартовой зоны.
constexpr int PLAYER_AI_SMOKE_INITIAL_KNOWN_ENEMIES = 3; // Known enemies для раннего smoke screen даже без spotting.
constexpr int PLAYER_AI_SMOKE_PRESSURE_EXPOSURE = 90; // Exposure, с которого дым можно бросать под прямым давлением.
constexpr int PLAYER_AI_SMOKE_HIGH_EXPOSURE = 120; // Exposure, с которого поздний дым оправдан потерей огневого действия.
constexpr int PLAYER_AI_SMOKE_SUPPORT_IDLE_MIN_ALLIES = 5; // Размер отряда, при котором idle/hold в начале может инициировать дым.
constexpr int PLAYER_AI_SMOKE_ROOM_ENTRY_LIMIT = 4; // Максимум входов в комнату, при котором smoke breach наиболее полезен.
constexpr int PLAYER_AI_SMOKE_ROOM_TILE_LIMIT = 160; // Максимальный размер комнаты для smoke breach.
constexpr int PLAYER_AI_SMOKE_TARGET_SMOKE_LIMIT = 0; // Если target tile уже задымлен, повторный дым не нужен.
constexpr int PLAYER_AI_SMOKE_MAX_THROW_DISTANCE = 10; // Максимальная дистанция до smoke target, чтобы не бросать дым в дальний контакт.
constexpr int PLAYER_AI_SMOKE_INITIAL_EXIT_SCAN_RADIUS = 4; // Радиус поиска нижнего выхода/рампы для стартового дыма.
constexpr int PLAYER_AI_SMOKE_INITIAL_EXIT_MAX_ALLY_DISTANCE = 3; // Дистанция от союзника до нижнего выхода, чтобы считать tile стартовой зоной.
constexpr int PLAYER_AI_SMOKE_INITIAL_EXIT_MIN_THROW_DISTANCE = 2; // Минимальная дальность стартового дыма, чтобы не класть его прямо в рампу под ногами.
constexpr int PLAYER_AI_SMOKE_INITIAL_COVER_MEMORY_DISTANCE = 8; // Дистанция от unit до стартового smoke plan, где нужно активнее выходить через дым.
constexpr int PLAYER_AI_SMOKE_EXIT_MOVE_DISTANCE = 6; // Максимальная длина принудительного выхода через стартовый дым.
constexpr int PLAYER_AI_SMOKE_EXIT_RESERVE_TU = 14; // Запас TU после выхода через дым, чтобы боец мог присесть/среагировать позже.
constexpr int PLAYER_AI_SMOKE_EXIT_MIN_SCORE = 110; // Минимальный score для принудительного выхода, если Escape ничего полезного не делает.
constexpr int PLAYER_AI_SMOKE_EXIT_LOWER_LEVEL_BONUS = 220; // Бонус клеткам ниже стартовой зоны: это фактический выход из транспорта.
constexpr int PLAYER_AI_SMOKE_ALLY_BLIND_RADIUS = 2; // Радиус вокруг союзника, куда нельзя класть центр дыма.
constexpr int PLAYER_AI_SMOKE_ALLY_BLIND_PENALTY = 180; // Штраф за задымление союзника/огневой позиции.
constexpr int PLAYER_AI_SMOKE_INITIAL_ALLY_BLIND_PENALTY = 35; // Смягченный штраф союзников для стартового дыма у выхода.
constexpr int PLAYER_AI_SMOKE_FIRELINE_SCORE = 90; // Бонус target, который снижает текущие fire lines.
constexpr int PLAYER_AI_SMOKE_SPOTTER_SCORE = 120; // Бонус target при текущем spotting.
constexpr int PLAYER_AI_SMOKE_EXPOSURE_SCORE_DIVISOR = 2; // Делитель current exposure для smoke score.
constexpr int PLAYER_AI_SMOKE_CONTACT_DISTANCE_SCORE = 6; // Вес близости smoke target к contact.
constexpr int PLAYER_AI_SMOKE_SELF_DISTANCE_PENALTY = 7; // Штраф за дальность броска дыма от unit.
constexpr int PLAYER_AI_SMOKE_CENTER_PATH_BONUS = 45; // Бонус за target между unit/отрядом и contact.
constexpr int PLAYER_AI_SMOKE_ENTRY_BONUS = 80; // Бонус entry tile при smoke breach.
constexpr int PLAYER_AI_SMOKE_INITIAL_BONUS = 70; // Бонус раннего выхода из стартовой зоны.
constexpr int PLAYER_AI_SMOKE_SIEGE_BONUS = 85; // Бонус smoke breach для siege room.
constexpr int PLAYER_AI_SMOKE_SURVIVE_BONUS = 100; // Бонус дыма в survive/retreat.
constexpr int PLAYER_AI_SMOKE_GOOD_SHOT_VISIBLE_LIMIT = 1; // Видимые враги, при которых дым запрещен если есть хороший выстрел.
constexpr int PLAYER_AI_SMOKE_MIN_SCORE = 260; // Минимальный score для аварийного броска дыма.
constexpr int PLAYER_AI_SMOKE_BREACH_MIN_SCORE = 200; // Минимальный score для smoke breach у опасной комнаты.
constexpr int PLAYER_AI_SMOKE_INITIAL_MIN_SCORE = 50; // Минимальный score для стартового smoke screen.
constexpr double PLAYER_AI_GEOMETRY_EPSILON = 0.1; // Малый epsilon для проверок вырожденных 2D-векторов.

const char *battleTypeName(BattleType type)
{
	switch (type)
	{
	case BT_FIREARM: return "firearm";
	case BT_AMMO: return "ammo";
	case BT_MELEE: return "melee";
	case BT_GRENADE: return "grenade";
	case BT_PROXIMITYGRENADE: return "proximity_grenade";
	case BT_MEDIKIT: return "medikit";
	case BT_SCANNER: return "scanner";
	case BT_MINDPROBE: return "mind_probe";
	case BT_PSIAMP: return "psi_amp";
	case BT_FLARE: return "flare";
	case BT_CORPSE: return "corpse";
	default: return "none";
	}
}

int playerAIWeaponDanger(const BattleItem *item)
{
	if (!item || !item->getRules())
	{
		return 0;
	}
	auto ruleDanger = [](const RuleItem *rule) -> int
	{
		if (!rule)
		{
			return 0;
		}
		const int accuracy = std::max(std::max(rule->getAccuracySnap(), rule->getAccuracyAimed()), rule->getAccuracyAuto());
		const int radius = std::max(0, rule->getExplosionRadius({ BA_NONE }));
		return std::max(0, rule->getPower()) + accuracy / 2 + radius * PLAYER_AI_WEAPON_DANGER_RADIUS_BONUS;
	};
	int danger = ruleDanger(item->getRules());
	const int weaponAccuracy = std::max(std::max(item->getRules()->getAccuracySnap(),
		item->getRules()->getAccuracyAimed()), item->getRules()->getAccuracyAuto());
	const BattleActionType ammoActions[] = { BA_SNAPSHOT, BA_AIMEDSHOT, BA_AUTOSHOT, BA_LAUNCH };
	for (BattleActionType ammoAction : ammoActions)
	{
		const BattleItem *ammo = item->getAmmoForAction(ammoAction);
		if (ammo && ammo->getRules())
		{
			const int ammoRadius = std::max(0, ammo->getRules()->getExplosionRadius({ ammoAction }));
			const int combinedDanger = std::max(0, ammo->getRules()->getPower())
				+ weaponAccuracy / 2 + ammoRadius * PLAYER_AI_WEAPON_DANGER_RADIUS_BONUS;
			danger = std::max(danger, std::max(ruleDanger(ammo->getRules()), combinedDanger));
		}
	}
	return danger;
}

int playerAIWeaponDirectDanger(const BattleItem *item)
{
	if (!item || !item->getRules())
	{
		return 0;
	}
	auto ruleDanger = [](const RuleItem *rule) -> int
	{
		return rule
			? std::max(0, rule->getPower())
				+ std::max(std::max(rule->getAccuracySnap(), rule->getAccuracyAimed()), rule->getAccuracyAuto()) / 2
			: 0;
	};
	int danger = ruleDanger(item->getRules());
	const int weaponAccuracy = std::max(std::max(item->getRules()->getAccuracySnap(),
		item->getRules()->getAccuracyAimed()), item->getRules()->getAccuracyAuto());
	const BattleActionType ammoActions[] = { BA_SNAPSHOT, BA_AIMEDSHOT, BA_AUTOSHOT, BA_LAUNCH };
	for (BattleActionType ammoAction : ammoActions)
	{
		const BattleItem *ammo = item->getAmmoForAction(ammoAction);
		if (ammo && ammo->getRules())
		{
			const int ammoRadius = std::max(0, ammo->getRules()->getExplosionRadius({ ammoAction }));
			const int combinedDanger = std::max(0, ammo->getRules()->getPower())
				+ weaponAccuracy / 2 + ammoRadius * PLAYER_AI_WEAPON_DANGER_RADIUS_BONUS;
			danger = std::max(danger, std::max(ruleDanger(ammo->getRules()), combinedDanger));
		}
	}
	return danger;
}

int playerAIWeaponBlastRadius(const BattleItem *item)
{
	if (!item || !item->getRules())
	{
		return 0;
	}
	auto ruleRadius = [](const RuleItem *rule) -> int
	{
		if (!rule || rule->getPower() <= 0 || !rule->getDamageType())
		{
			return 0;
		}
		const RuleDamageType *damageType = rule->getDamageType();
		if (damageType->ResistType == DT_SMOKE
			|| (damageType->ToHealth <= 0.0f && damageType->ToStun <= 0.0f))
		{
			return 0;
		}
		return std::max(0, rule->getExplosionRadius({ BA_NONE }));
	};
	int radius = ruleRadius(item->getRules());
	const BattleActionType ammoActions[] = { BA_SNAPSHOT, BA_AIMEDSHOT, BA_AUTOSHOT, BA_LAUNCH };
	for (BattleActionType ammoAction : ammoActions)
	{
		const BattleItem *ammo = item->getAmmoForAction(ammoAction);
		if (ammo && ammo->getRules())
		{
			radius = std::max(radius, ruleRadius(ammo->getRules()));
		}
	}
	return radius;
}

bool playerCanThrowFromTile(SavedBattleGame *save, BattleUnit *unit, BattleItem *item, const Position &from, const Position &target)
{
	if (!save || !unit || !item || !item->getRules())
	{
		return false;
	}
	Tile *originTile = save->getTile(from);
	Tile *targetTile = save->getTile(target);
	if (!originTile || !targetTile)
	{
		return false;
	}
	if (targetTile->getMapData(O_OBJECT)
		&& targetTile->getMapData(O_OBJECT)->getTUCost(MT_WALK) == Pathfinding::INVALID_MOVE_COST
		&& !(targetTile->isBigWall()
			&& (targetTile->getMapData(O_OBJECT)->getBigWall() < 1
				|| targetTile->getMapData(O_OBJECT)->getBigWall() > 3)))
	{
		return false;
	}
	const int xdiff = target.x - from.x;
	const int ydiff = target.y - from.y;
	const int zdiff = target.z - from.z;
	const int compatibilityDistanceSq = xdiff * xdiff + ydiff * ydiff + zdiff * zdiff;
	if (item->getRules()->isOutOfThrowRange(compatibilityDistanceSq, save->getDepth()))
	{
		return false;
	}
	BattleAction action;
	action.actor = unit;
	action.weapon = item;
	action.type = BA_THROW;
	action.target = target;
	Position originVoxel = save->getTileEngine()->getOriginVoxel(action, originTile);
	const int zlevel = originVoxel.z - ((target.z * Position::TileZ + 2) - targetTile->getTerrainLevel());
	const int weight = std::max(1, item->getTotalWeight());
	double maxDistance = (ProjectileFlyBState::getMaxThrowDistance(weight, unit->getBaseStats()->strength, zlevel) + PLAYER_AI_THROW_DISTANCE_BIAS) / PLAYER_AI_THROW_DISTANCE_TILE_SCALE;
	if (save->getDepth() > 0 && Mod::EXTENDED_UNDERWATER_THROW_FACTOR > 0)
	{
		maxDistance = maxDistance * (double)Mod::EXTENDED_UNDERWATER_THROW_FACTOR / PLAYER_AI_THROW_UNDERWATER_FACTOR_BASE;
	}
	const double realDistance = sqrt((double)(xdiff * xdiff) + (double)(ydiff * ydiff));
	if (realDistance > maxDistance)
	{
		return false;
	}
	Position targetVoxel = target.toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY, (PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE + -targetTile->getTerrainLevel()));
	Position targetPos = targetVoxel.toTile();
	double curvature = std::max(PLAYER_AI_THROW_MIN_CURVATURE, PLAYER_AI_THROW_CURVATURE_STRENGTH_FACTOR / sqrt(sqrt((double)unit->getBaseStats()->strength / (double)weight)) + (unit->isKneeled() ? PLAYER_AI_THROW_KNEEL_CURVATURE_BONUS : 0.0));
	std::vector<Position> trajectory;
	while (curvature < PLAYER_AI_THROW_MAX_CURVATURE)
	{
		trajectory.clear();
		const int test = save->getTileEngine()->calculateParabolaVoxel(originVoxel, targetVoxel, true, &trajectory, unit, curvature, Position(0, 0, 0));
		if (!trajectory.empty())
		{
			Position tilePos = Projectile::getPositionFromEnd(trajectory, Projectile::ItemDropVoxelOffset).toTile();
			if (test != V_OUTOFBOUNDS && tilePos == targetPos)
			{
				return true;
			}
		}
		curvature += PLAYER_AI_THROW_CURVATURE_STEP;
	}
	return false;
}

int getPlayerBestExplosiveReserve(BattleUnit *unit, SavedBattleGame *save, bool urgent, int *bestPower = 0, int *bestRadius = 0, const char **bestType = 0)
{
	if (!unit || !save)
	{
		return 0;
	}
	int bestScore = -1;
	int bestReserve = 0;
	if (bestPower)
	{
		*bestPower = 0;
	}
	if (bestRadius)
	{
		*bestRadius = 0;
	}
	if (bestType)
	{
		*bestType = "none";
	}
	for (auto *item : *unit->getInventory())
	{
		if (!item || !item->getRules() || !item->getRules()->isGrenadeOrProxy()
			|| item->getRules()->getBattleType() == BT_PROXIMITYGRENADE
			|| (item->getRules()->getDamageType() && item->getRules()->getDamageType()->ResistType == DT_SMOKE))
		{
			continue;
		}
		if (!urgent && save->getTurn() < item->getRules()->getAIUseDelay(save->getMod()))
		{
			continue;
		}
		BattleAction probe;
		probe.actor = unit;
		probe.weapon = item;
		probe.type = BA_THROW;
		const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(probe));
		const int power = std::max(0, item->getRules()->getPower());
		if (radius <= 0 || power <= 0)
		{
			continue;
		}
		BattleActionCost throwCost(BA_THROW, unit, item);
		if (throwCost.Time <= 0)
		{
			continue;
		}
		const RuleItemUseCost primeCost = unit->getActionTUs(BA_PRIME, item);
		const int reserve = throwCost.Time + primeCost.Time + PLAYER_AI_PICKUP_TU_COST;
		const int score = power * PLAYER_AI_EXPLOSIVE_POWER_SCORE + radius * PLAYER_AI_EXPLOSIVE_RADIUS_SCORE - reserve;
		if (score > bestScore || (score == bestScore && reserve > bestReserve))
		{
			bestScore = score;
			bestReserve = reserve;
			if (bestPower)
			{
				*bestPower = power;
			}
			if (bestRadius)
			{
				*bestRadius = radius;
			}
			if (bestType)
			{
				*bestType = item->getRules()->getType().c_str();
			}
		}
	}
	return bestReserve;
}

int getPlayerExplosiveThrowerRank(BattleUnit *unit, SavedBattleGame *save)
{
	if (!unit || !save || unit->getFaction() != FACTION_PLAYER)
	{
		return PLAYER_AI_NO_THROWER_RANK;
	}
	int unitPower = 0;
	int unitRadius = 0;
	const int unitReserve = getPlayerBestExplosiveReserve(unit, save, true, &unitPower, &unitRadius);
	if (unitReserve <= 0)
	{
		return PLAYER_AI_NO_THROWER_RANK;
	}
	auto throwerScore = [](BattleUnit *candidate, int power, int radius) -> int
	{
		const UnitStats *stats = candidate->getBaseStats();
		return stats->throwing * PLAYER_AI_THROWER_THROWING_SCORE
			+ stats->tu * PLAYER_AI_THROWER_TU_SCORE
			+ stats->strength
			+ power
			+ radius * PLAYER_AI_THROWER_RADIUS_SCORE;
	};
	const int unitScore = throwerScore(unit, unitPower, unitRadius);
	int rank = 0;
	for (auto *other : *save->getUnits())
	{
		if (!other || other == unit || other->isOut() || other->getFaction() != FACTION_PLAYER)
		{
			continue;
		}
		int power = 0;
		int radius = 0;
		if (getPlayerBestExplosiveReserve(other, save, true, &power, &radius) <= 0)
		{
			continue;
		}
		const int otherScore = throwerScore(other, power, radius);
		if (otherScore > unitScore || (otherScore == unitScore && other->getId() < unit->getId()))
		{
			++rank;
		}
	}
	return rank;
}

void logPlayerInitialBattleState(SavedBattleGame *save)
{
	if (!Options::autoBattleLog || !save || std::find(playerInitialBattleLogs.begin(), playerInitialBattleLogs.end(), save) != playerInitialBattleLogs.end())
	{
		return;
	}
	playerInitialBattleLogs.push_back(save);
	if (playerInitialBattleLogs.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		playerInitialBattleLogs.erase(playerInitialBattleLogs.begin(), playerInitialBattleLogs.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}

	int playerUnits = 0;
	int hostileUnits = 0;
	int neutralUnits = 0;
	for (auto *unit : *save->getUnits())
	{
		if (!unit || unit->isOut())
		{
			continue;
		}
		if (unit->getFaction() == FACTION_PLAYER)
		{
			++playerUnits;
		}
		else if (unit->getFaction() == FACTION_HOSTILE)
		{
			++hostileUnits;
		}
		else
		{
			++neutralUnits;
		}
	}
	{
		std::ostringstream log;
		log << "Player faction initial battle: turn=" << save->getTurn()
			<< ", mapSize=(" << save->getMapSizeX() << "," << save->getMapSizeY() << "," << save->getMapSizeZ() << ")"
			<< ", playerUnits=" << playerUnits
			<< ", hostileUnits=" << hostileUnits
			<< ", neutralUnits=" << neutralUnits;
		save->appendToAutoBattleLog(log.str());
	}

	for (auto *unit : *save->getUnits())
	{
		if (!unit || unit->isOut() || unit->getFaction() != FACTION_PLAYER)
		{
			continue;
		}
		const UnitStats *stats = unit->getBaseStats();
		std::ostringstream log;
		log << "Player faction initial unit: id=" << unit->getId()
			<< ", type=" << unit->getType()
			<< ", pos=" << unit->getPosition()
			<< ", dir=" << unit->getDirection()
			<< ", kneel=" << unit->isKneeled()
			<< ", armor=" << (unit->getArmor() ? unit->getArmor()->getType() : "none")
			<< ", hp=" << unit->getHealth()
			<< ", tuNow=" << unit->getTimeUnits()
			<< ", energy=" << unit->getEnergy()
			<< ", morale=" << unit->getMorale();
		if (stats)
		{
			log << ", stats={tu:" << stats->tu
				<< ", stamina:" << stats->stamina
				<< ", health:" << stats->health
				<< ", bravery:" << stats->bravery
				<< ", reactions:" << stats->reactions
				<< ", firing:" << stats->firing
				<< ", throwing:" << stats->throwing
				<< ", strength:" << stats->strength
				<< ", melee:" << stats->melee
				<< "}";
		}
		BattleItem *mainWeapon = unit->getMainHandWeapon(false);
		log << ", mainWeapon=" << (mainWeapon && mainWeapon->getRules() ? mainWeapon->getRules()->getType() : "none")
			<< ", inventory=[";
		bool first = true;
		for (auto *item : *unit->getInventory())
		{
			if (!item || !item->getRules())
			{
				continue;
			}
			const RuleItem *rule = item->getRules();
			BattleAction itemAction;
			itemAction.actor = unit;
			itemAction.weapon = item;
			itemAction.type = rule->getBattleType() == BT_MELEE ? BA_HIT : BA_THROW;
			const int radius = rule->isGrenadeOrProxy() ? rule->getExplosionRadius(BattleActionAttack::GetBeforeShoot(itemAction)) : 0;
			if (!first)
			{
				log << "; ";
			}
			first = false;
			log << rule->getType()
				<< "{type:" << battleTypeName(rule->getBattleType())
				<< ", power:" << rule->getPower()
				<< ", radius:" << radius
				<< ", snap:" << rule->getAccuracySnap()
				<< ", aimed:" << rule->getAccuracyAimed()
				<< ", auto:" << rule->getAccuracyAuto()
				<< ", fuse:" << item->getFuseTimer()
				<< ", ammoQty:" << item->getAmmoQuantity()
				<< "}";
		}
		log << "]";
		save->appendToAutoBattleLog(log.str());
	}

	for (auto *unit : *save->getUnits())
	{
		if (!unit || unit->isOut() || unit->getFaction() != FACTION_HOSTILE)
		{
			continue;
		}
		const UnitStats *stats = unit->getBaseStats();
		BattleItem *weapon = unit->getMainHandWeapon(false);
		std::ostringstream log;
		log << "Player faction initial enemy: id=" << unit->getId()
			<< ", type=" << unit->getType()
			<< ", pos=" << unit->getPosition()
			<< ", dir=" << unit->getDirection()
			<< ", armor=" << (unit->getArmor() ? unit->getArmor()->getType() : "none")
			<< ", hp=" << unit->getHealth()
			<< ", weapon=" << (weapon && weapon->getRules() ? weapon->getRules()->getType() : "none");
		if (stats)
		{
			log << ", stats={tu:" << stats->tu
				<< ", health:" << stats->health
				<< ", reactions:" << stats->reactions
				<< ", firing:" << stats->firing
				<< ", throwing:" << stats->throwing
				<< ", strength:" << stats->strength
				<< "}";
		}
		save->appendToAutoBattleLog(log.str());
	}
}

void cleanupPendingPlayerGrenadeDangers(SavedBattleGame *save)
{
	if (!save)
	{
		pendingPlayerGrenadeDangers.clear();
		return;
	}
	const int turn = save->getTurn();
	pendingPlayerGrenadeDangers.erase(std::remove_if(pendingPlayerGrenadeDangers.begin(), pendingPlayerGrenadeDangers.end(),
		[save, turn](const PendingPlayerGrenadeDanger &danger)
		{
			return danger.save != save || danger.turn != turn;
		}), pendingPlayerGrenadeDangers.end());
	if (pendingPlayerGrenadeDangers.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		pendingPlayerGrenadeDangers.erase(pendingPlayerGrenadeDangers.begin(), pendingPlayerGrenadeDangers.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}
}

void cleanupPendingPlayerProximityMinePlans(SavedBattleGame *save)
{
	if (!save)
	{
		pendingPlayerProximityMinePlans.clear();
		return;
	}
	const int turn = save->getTurn();
	pendingPlayerProximityMinePlans.erase(std::remove_if(pendingPlayerProximityMinePlans.begin(), pendingPlayerProximityMinePlans.end(),
		[save, turn](const PendingPlayerProximityMinePlan &plan)
		{
			return plan.save != save || plan.turn + PLAYER_AI_PENDING_GRENADE_MEMORY_TURNS < turn;
		}), pendingPlayerProximityMinePlans.end());
	if (pendingPlayerProximityMinePlans.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		pendingPlayerProximityMinePlans.erase(pendingPlayerProximityMinePlans.begin(), pendingPlayerProximityMinePlans.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}
}

void cleanupPendingPlayerProximityMineStagings(SavedBattleGame *save)
{
	if (!save)
	{
		pendingPlayerProximityMineStagings.clear();
		return;
	}
	const int turn = save->getTurn();
	pendingPlayerProximityMineStagings.erase(std::remove_if(pendingPlayerProximityMineStagings.begin(), pendingPlayerProximityMineStagings.end(),
		[save, turn](const PendingPlayerProximityMineStaging &plan)
		{
			return plan.save != save || plan.turn + PLAYER_AI_PENDING_MINE_MEMORY_TURNS < turn;
		}), pendingPlayerProximityMineStagings.end());
	if (pendingPlayerProximityMineStagings.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		pendingPlayerProximityMineStagings.erase(pendingPlayerProximityMineStagings.begin(), pendingPlayerProximityMineStagings.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}
}

bool getPlayerProximityMineStaging(SavedBattleGame *save, int unitId, Position *target, Position *contact)
{
	cleanupPendingPlayerProximityMineStagings(save);
	for (const auto &plan : pendingPlayerProximityMineStagings)
	{
		if (plan.unitId == unitId)
		{
			if (target)
			{
				*target = plan.target;
			}
			if (contact)
			{
				*contact = plan.contact;
			}
			return true;
		}
	}
	return false;
}

void recordPlayerProximityMineStaging(SavedBattleGame *save, BattleUnit *unit, UnitFaction faction, const Position &contact, const Position &target)
{
	if (!save || !unit)
	{
		return;
	}
	cleanupPendingPlayerProximityMineStagings(save);
	for (auto &plan : pendingPlayerProximityMineStagings)
	{
		if (plan.unitId == unit->getId())
		{
			plan.turn = save->getTurn();
			plan.faction = faction;
			plan.contact = contact;
			plan.target = target;
			return;
		}
	}
	PendingPlayerProximityMineStaging plan = { save, save->getTurn(), unit->getId(), faction, contact, target };
	pendingPlayerProximityMineStagings.push_back(plan);
}

void clearPlayerProximityMineStaging(SavedBattleGame *save, int unitId)
{
	cleanupPendingPlayerProximityMineStagings(save);
	pendingPlayerProximityMineStagings.erase(std::remove_if(pendingPlayerProximityMineStagings.begin(), pendingPlayerProximityMineStagings.end(),
		[unitId](const PendingPlayerProximityMineStaging &plan)
		{
			return plan.unitId == unitId;
		}), pendingPlayerProximityMineStagings.end());
}

bool isRecentPlayerProximityMinePlan(SavedBattleGame *save, UnitFaction faction, const Position &contact, const Position *target = 0, int targetSpacing = PLAYER_AI_PROXIMITY_DEFAULT_TARGET_SPACING)
{
	cleanupPendingPlayerProximityMinePlans(save);
	for (const auto &plan : pendingPlayerProximityMinePlans)
	{
		if (plan.faction == faction
			&& plan.contact.z == contact.z
			&& Position::distance2d(plan.contact, contact) <= PLAYER_AI_PROXIMITY_CONTACT_MEMORY_DISTANCE
			&& (!target || (plan.target.z == target->z && Position::distance2d(plan.target, *target) <= targetSpacing)))
		{
			return true;
		}
	}
	return false;
}

void recordPlayerProximityMinePlan(SavedBattleGame *save, UnitFaction faction, const Position &contact, const Position &target)
{
	if (!save)
	{
		return;
	}
	cleanupPendingPlayerProximityMinePlans(save);
	for (const auto &plan : pendingPlayerProximityMinePlans)
	{
		if (plan.faction == faction
			&& plan.contact.z == contact.z
			&& Position::distance2d(plan.contact, contact) <= PLAYER_AI_PROXIMITY_CONTACT_MEMORY_DISTANCE
			&& plan.target.z == target.z
			&& Position::distance2d(plan.target, target) <= PLAYER_AI_PROXIMITY_TARGET_MEMORY_DISTANCE)
		{
			return;
		}
	}
	PendingPlayerProximityMinePlan plan = { save, save->getTurn(), faction, contact, target };
	pendingPlayerProximityMinePlans.push_back(plan);
}

void cleanupPendingPlayerSmokePlans(SavedBattleGame *save)
{
	if (!save)
	{
		pendingPlayerSmokePlans.clear();
		return;
	}
	const int turn = save->getTurn();
	pendingPlayerSmokePlans.erase(std::remove_if(pendingPlayerSmokePlans.begin(), pendingPlayerSmokePlans.end(),
		[save, turn](const PendingPlayerSmokePlan &plan)
		{
			return plan.save != save || plan.turn + PLAYER_AI_SMOKE_MEMORY_TURNS < turn;
		}), pendingPlayerSmokePlans.end());
	if (pendingPlayerSmokePlans.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		pendingPlayerSmokePlans.erase(pendingPlayerSmokePlans.begin(), pendingPlayerSmokePlans.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}
}

bool isRecentPlayerSmokePlan(SavedBattleGame *save, UnitFaction faction, const Position &contact, const Position &target)
{
	cleanupPendingPlayerSmokePlans(save);
	for (const auto &plan : pendingPlayerSmokePlans)
	{
		if (plan.faction == faction
			&& plan.contact.z == contact.z
			&& plan.target.z == target.z
			&& Position::distance2d(plan.contact, contact) <= PLAYER_AI_SMOKE_CONTACT_MEMORY_DISTANCE
			&& Position::distance2d(plan.target, target) <= PLAYER_AI_SMOKE_TARGET_MEMORY_DISTANCE)
		{
			return true;
		}
	}
	return false;
}

bool hasRecentPlayerSmokePlanNear(SavedBattleGame *save, UnitFaction faction, const Position &pos, int distance, Position *targetOut = 0)
{
	cleanupPendingPlayerSmokePlans(save);
	for (const auto &plan : pendingPlayerSmokePlans)
	{
		if (plan.save == save
			&& plan.faction == faction
			&& abs(plan.target.z - pos.z) <= 1
			&& Position::distance2d(plan.target, pos) <= distance)
		{
			if (targetOut)
			{
				*targetOut = plan.target;
			}
			return true;
		}
	}
	return false;
}

void clearPlayerProximityMinePlan(SavedBattleGame *save, UnitFaction faction,
	const Position &contact, const Position &target)
{
	cleanupPendingPlayerProximityMinePlans(save);
	pendingPlayerProximityMinePlans.erase(std::remove_if(pendingPlayerProximityMinePlans.begin(), pendingPlayerProximityMinePlans.end(),
		[save, faction, &contact, &target](const PendingPlayerProximityMinePlan &plan)
		{
			return plan.save == save && plan.faction == faction
				&& plan.contact == contact && plan.target == target;
		}), pendingPlayerProximityMinePlans.end());
}

void recordPlayerSmokePlan(SavedBattleGame *save, UnitFaction faction, const Position &contact, const Position &target)
{
	if (!save)
	{
		return;
	}
	cleanupPendingPlayerSmokePlans(save);
	if (isRecentPlayerSmokePlan(save, faction, contact, target))
	{
		return;
	}
	PendingPlayerSmokePlan plan = { save, save->getTurn(), faction, contact, target };
	pendingPlayerSmokePlans.push_back(plan);
}

bool isPlayerSmokeGrenade(const BattleItem *item)
{
	return item
		&& item->getRules()
		&& item->getRules()->isGrenadeOrProxy()
		&& item->getRules()->getBattleType() != BT_PROXIMITYGRENADE
		&& item->getRules()->getDamageType()
		&& item->getRules()->getDamageType()->ResistType == DT_SMOKE;
}

int getPendingPlayerGrenadeDangerDepth(SavedBattleGame *save, UnitFaction faction, const Position &pos, int margin = 0)
{
	cleanupPendingPlayerGrenadeDangers(save);
	int dangerDepth = 0;
	for (const auto &danger : pendingPlayerGrenadeDangers)
	{
		if (danger.faction == faction
			&& abs(danger.target.z - pos.z) <= Options::battleExplosionHeight
			&& Position::distance2d(danger.target, pos) <= danger.radius + margin)
		{
			dangerDepth += danger.radius + margin - Position::distance2d(danger.target, pos) + 1;
		}
	}
	return dangerDepth;
}

bool isPendingPlayerGrenadeDanger(SavedBattleGame *save, UnitFaction faction, const Position &pos)
{
	return getPendingPlayerGrenadeDangerDepth(save, faction, pos) > 0;
}

int playerAIConservativeBlastDamage(SavedBattleGame *save, BattleItem *explosive,
	const Position &center, const BattleUnit *target);

bool isPendingPlayerTimedBlastTarget(SavedBattleGame *save, UnitFaction faction, const BattleUnit *unit)
{
	if (!save || !unit || unit->isOut() || unit->getFaction() == faction)
	{
		return false;
	}
	cleanupPendingPlayerGrenadeDangers(save);
	int reservedDamage = 0;
	for (const auto &danger : pendingPlayerGrenadeDangers)
	{
		if (danger.faction != faction || !danger.reservesTargets || danger.power <= 0
			|| abs(danger.target.z - unit->getPosition().z) > Options::battleExplosionHeight)
		{
			continue;
		}
		const int distance = Position::distance2d(danger.target, unit->getPosition());
		if (distance > danger.radius)
		{
			continue;
		}
		const int predictedDamage = playerAIConservativeBlastDamage(save, danger.item, danger.target, unit);
		if (predictedDamage >= unit->getHealth())
		{
			return true;
		}
		reservedDamage += predictedDamage;
	}
	return reservedDamage >= unit->getHealth();
}

void refreshActivePlayerExplosiveHazards(SavedBattleGame *save)
{
	activePlayerExplosiveHazards.clear();
	if (!save)
	{
		return;
	}
	for (int i = 0; i < save->getMapSizeXYZ(); ++i)
	{
		Tile *tile = save->getTile(i);
		if (!tile)
		{
			continue;
		}
		for (auto *item : *tile->getInventory())
		{
			if (!item || !item->getRules() || item->getFuseTimer() < 0)
			{
				continue;
			}
			const BattleType type = item->getRules()->getBattleType();
			const bool proximity = type == BT_PROXIMITYGRENADE;
			const bool damagingTimedGrenade = type == BT_GRENADE
				&& item->getRules()->getPower() > 0
				&& item->getRules()->getDamageType()
				&& item->getRules()->getDamageType()->ResistType != DT_SMOKE;
			if (!proximity && !damagingTimedGrenade)
			{
				continue;
			}
			BattleAction explosiveAction;
			explosiveAction.weapon = item;
			explosiveAction.type = BA_THROW;
			const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(explosiveAction));
			if (radius > 0)
			{
				activePlayerExplosiveHazards.push_back({ save, tile->getPosition(), radius, proximity });
			}
		}
	}
	if (activePlayerExplosiveHazards.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		activePlayerExplosiveHazards.erase(activePlayerExplosiveHazards.begin(),
			activePlayerExplosiveHazards.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}
}

bool playerAIHasVirtualLineToPosition(SavedBattleGame *save, BattleUnit *movingUnit,
	BattleUnit *observer, const Position &pos, const Position &originVoxel)
{
	Tile *tile = save ? save->getTile(pos) : 0;
	if (!tile || !movingUnit || !observer)
	{
		return false;
	}

	const int targetBaseZ = pos.z * 24 - tile->getTerrainLevel() + movingUnit->getFloatHeight();
	const int standHeight = std::max(4, movingUnit->getStandHeight());
	const int targetHeights[] = {
		std::max(2, standHeight / 4),
		std::max(3, standHeight / 2),
		std::max(3, standHeight - 2)
	};
	const Position lateralSamples[] = {
		Position(0, 0, 0), Position(3, 0, 0), Position(-3, 0, 0),
		Position(0, 3, 0), Position(0, -3, 0)
	};
	for (int height : targetHeights)
	{
		for (const auto &offset : lateralSamples)
		{
			Position targetVoxel = pos.toVoxel() + Position(8, 8, targetBaseZ - pos.z * 24 + height) + offset;
			std::vector<Position> trajectory;
			// Ignore units while testing a future tile: the mover still occupies its
			// old tile, and using it as canTargetUnit's hypothetical cylinder can make
			// that old body falsely block the ray.  Terrain remains fully collidable.
			const VoxelType impact = save->getTileEngine()->calculateLineVoxel(originVoxel,
				targetVoxel, false, &trajectory, movingUnit, movingUnit);
			if (impact == V_EMPTY)
			{
				return true;
			}
			if ((impact == V_OBJECT || impact == V_WESTWALL || impact == V_NORTHWALL)
				&& !trajectory.empty()
				&& Position::distance2d(trajectory.back().toTile(), pos) <= 1)
			{
				// A ready burst can destroy target-side cover with its first round and
				// hit the exposed unit with a later round.  Treat near-target terrain
				// as a reaction lane even if the perfect central ray is initially blocked.
				return true;
			}
		}
	}
	return false;
}

int playerAIConservativeBlastDamage(SavedBattleGame *save, BattleItem *explosive,
	const Position &center, const BattleUnit *target)
{
	if (!save || !explosive || !explosive->getRules() || !target || target->isOut())
	{
		return 0;
	}
	Tile *centerTile = save->getTile(center);
	Tile *targetTile = target->getTile();
	const RuleDamageType *damageType = explosive->getRules()->getDamageType();
	if (!centerTile || !targetTile || !damageType)
	{
		return 0;
	}
	const int distance = Position::distance2d(center, target->getPosition());
	int remainingPower = std::max(0, explosive->getRules()->getPower()
		- (int)std::ceil(distance * damageType->RadiusReduction));
	if (remainingPower <= 0)
	{
		return 0;
	}
	if (distance > 0)
	{
		Position originVoxel = center.toVoxel() + Position(8, 8, std::max(2, 6 - centerTile->getTerrainLevel()));
		Position targetVoxel = target->getPosition().toVoxel()
			+ Position(8, 8, -targetTile->getTerrainLevel() + target->getFloatHeight() + std::max(3, target->getStandHeight() / 2));
		std::vector<Position> trajectory;
		// Units do not block an explosion wave, so only the intended target is
		// considered as a unit collision; walls/floors/objects remain blockers.
		const VoxelType impact = save->getTileEngine()->calculateLineVoxel(originVoxel,
			targetVoxel, false, &trajectory, 0, const_cast<BattleUnit*>(target));
		if (impact != V_EMPTY && impact != V_UNIT)
		{
			return 0;
		}
	}
	remainingPower = target->reduceByResistance(remainingPower, damageType->ResistType);
	const int armor = std::max(std::max(target->getArmor(SIDE_FRONT), target->getArmor(SIDE_LEFT)), target->getArmor(SIDE_RIGHT));
	return std::max(0, remainingPower - (int)std::ceil(armor * damageType->ArmorEffectiveness));
}

int getActivePlayerExplosiveDangerDepth(SavedBattleGame *save, const Position &pos, int margin = 0)
{
	int dangerDepth = 0;
	for (const auto &hazard : activePlayerExplosiveHazards)
	{
		if (hazard.save != save || abs(hazard.target.z - pos.z) > Options::battleExplosionHeight)
		{
			continue;
		}
		const int distance = Position::distance2d(hazard.target, pos);
		if (distance <= hazard.radius + margin)
		{
			dangerDepth += hazard.radius + margin - distance + 1;
		}
	}
	return dangerDepth;
}

bool isActivePlayerExplosiveDanger(SavedBattleGame *save, const Position &pos, int margin = 0)
{
	return getActivePlayerExplosiveDangerDepth(save, pos, margin) > 0;
}

bool isPlayerExplosiveDanger(SavedBattleGame *save, UnitFaction faction, const Position &pos, int margin = 0)
{
	return getPendingPlayerGrenadeDangerDepth(save, faction, pos, margin) > 0 || getActivePlayerExplosiveDangerDepth(save, pos, margin) > 0;
}

bool playerPathCrossesExplosiveDanger(SavedBattleGame *save, BattleUnit *unit, const std::vector<int> &path, BattleActionMove moveType, bool allowMonotonicExit, Position *safeTarget = 0, Position *hazardTarget = 0)
{
	if (!save || !unit || unit->getFaction() != FACTION_PLAYER)
	{
		return false;
	}
	cleanupPendingPlayerGrenadeDangers(save);
	std::vector<std::pair<Position, int> > pendingHazards;
	std::vector<std::pair<Position, int> > activeMines;
	std::vector<std::pair<Position, int> > activeTimedBlasts;
	for (const auto &danger : pendingPlayerGrenadeDangers)
	{
		if (danger.faction == unit->getFaction())
		{
			pendingHazards.push_back(std::make_pair(danger.target, danger.radius));
		}
	}
	for (const auto &hazard : activePlayerExplosiveHazards)
	{
		if (hazard.save != save)
		{
			continue;
		}
		if (hazard.proximity)
		{
			activeMines.push_back(std::make_pair(hazard.target, hazard.radius));
		}
		else
		{
			activeTimedBlasts.push_back(std::make_pair(hazard.target, hazard.radius));
		}
	}
	pendingHazards.insert(pendingHazards.end(), activeTimedBlasts.begin(), activeTimedBlasts.end());
	auto hazardDepthAt = [&](const Position &at, const std::vector<std::pair<Position, int> > &hazards, bool triggerRadiusOnly) -> int
	{
		int depth = 0;
		for (const auto &hazard : hazards)
		{
			if ((triggerRadiusOnly && hazard.first.z != at.z)
				|| (!triggerRadiusOnly && abs(hazard.first.z - at.z) > Options::battleExplosionHeight))
			{
				continue;
			}
			const int radius = triggerRadiusOnly ? PLAYER_AI_PROXIMITY_TRIGGER_RADIUS : hazard.second;
			// Proximity activation checks the eight neighbouring tiles.  Euclidean
			// ceil-distance labels a diagonal neighbour as distance 2, so use the
			// Chebyshev grid metric for the 3x3 trigger area.
			const int distance = triggerRadiusOnly
				? std::max(abs(hazard.first.x - at.x), abs(hazard.first.y - at.y))
				: Position::distance2d(hazard.first, at);
			if (distance <= radius)
			{
				depth += radius - distance + 1;
			}
		}
		return depth;
	};
	auto markedDangerAt = [&](const Position &at) -> int
	{
		Tile *tile = save->getTile(at);
		return tile && tile->getDangerous() ? 1 : 0;
	};
	Position pos = unit->getPosition();
	Position lastSafe = pos;
	Position lastMineBlastSafe = hazardDepthAt(pos, activeMines, false) == 0 ? pos : unit->getPosition();
	const int startPendingDanger = markedDangerAt(pos) + hazardDepthAt(pos, pendingHazards, false);
	const int startMineTriggerDanger = hazardDepthAt(pos, activeMines, true);
	const int startMineBlastDanger = hazardDepthAt(pos, activeMines, false);
	int previousPendingDanger = startPendingDanger;
	int previousMineTriggerDanger = startMineTriggerDanger;
	int previousMineBlastDanger = startMineBlastDanger;
	for (auto direction = path.rbegin(); direction != path.rend(); ++direction)
	{
		const PathfindingStep step = save->getPathfinding()->getTUCost(pos, *direction, unit, 0, moveType);
		if (step.cost.time == Pathfinding::INVALID_MOVE_COST)
		{
			if (safeTarget)
			{
				*safeTarget = lastSafe;
			}
			if (hazardTarget)
			{
				*hazardTarget = pos;
			}
			return true;
		}
		pos = step.pos;
		const int pendingDanger = markedDangerAt(pos) + hazardDepthAt(pos, pendingHazards, false);
		const int mineTriggerDanger = hazardDepthAt(pos, activeMines, true);
		const int mineBlastDanger = hazardDepthAt(pos, activeMines, false);
		const bool unsafePending = allowMonotonicExit && startPendingDanger > 0
			? pendingDanger > previousPendingDanger
			: pendingDanger > 0;
		const bool unsafeMineTrigger = allowMonotonicExit && startMineTriggerDanger > 0
			? mineTriggerDanger > 0 && mineTriggerDanger >= previousMineTriggerDanger
			: mineTriggerDanger > 0;
		const bool movesDeeperIntoMineBlast = allowMonotonicExit && startMineBlastDanger > 0
			&& mineBlastDanger > previousMineBlastDanger;
		const bool unsafe = unsafePending || unsafeMineTrigger || movesDeeperIntoMineBlast;
		if (unsafe)
		{
			if (safeTarget)
			{
				*safeTarget = lastSafe;
			}
			if (hazardTarget)
			{
				*hazardTarget = pos;
			}
			return true;
		}
		lastSafe = pos;
		if (mineBlastDanger == 0)
		{
			lastMineBlastSafe = pos;
		}
		previousPendingDanger = pendingDanger;
		previousMineTriggerDanger = mineTriggerDanger;
		previousMineBlastDanger = mineBlastDanger;
	}
	const bool pendingExitIncomplete = allowMonotonicExit && startPendingDanger > 0 && previousPendingDanger > 0;
	const bool unsafeMineEndpoint = previousMineBlastDanger > 0;
	if (pendingExitIncomplete || unsafeMineEndpoint)
	{
		if (safeTarget)
		{
			*safeTarget = unsafeMineEndpoint ? lastMineBlastSafe : unit->getPosition();
		}
		if (hazardTarget)
		{
			*hazardTarget = pos;
		}
		return true;
	}
	if (safeTarget)
	{
		*safeTarget = lastSafe;
	}
	return false;
}

bool playerMoveCrossesExplosiveDanger(SavedBattleGame *save, BattleUnit *unit, const Position &target, BattleActionMove moveType, bool allowMonotonicExit, Position *safeTarget = 0, Position *hazardTarget = 0)
{
	if (!save || !unit || target == unit->getPosition())
	{
		return false;
	}
	save->getPathfinding()->calculate(unit, target, moveType);
	if (save->getPathfinding()->getStartDirection() == -1)
	{
		save->getPathfinding()->abortPath();
		return false;
	}
	const std::vector<int> path = save->getPathfinding()->copyPath();
	const bool risky = playerPathCrossesExplosiveDanger(save, unit, path, moveType, allowMonotonicExit, safeTarget, hazardTarget);
	save->getPathfinding()->abortPath();
	return risky;
}

void recordPendingPlayerGrenadeDanger(SavedBattleGame *save, UnitFaction faction, const Position &target,
	int radius, int power, bool reservesTargets, BattleUnit *thrower, BattleItem *item)
{
	if (!save || radius <= 0)
	{
		return;
	}
	cleanupPendingPlayerGrenadeDangers(save);
	for (auto &danger : pendingPlayerGrenadeDangers)
	{
		if (danger.faction == faction && danger.target == target && danger.radius == radius && danger.item == item)
		{
			danger.power = std::max(danger.power, power);
			danger.reservesTargets = danger.reservesTargets || reservesTargets;
			return;
		}
	}
	PendingPlayerGrenadeDanger danger = { save, save->getTurn(), faction, target, radius, power, reservesTargets,
		thrower ? thrower->getId() : -1, item };
	pendingPlayerGrenadeDangers.push_back(danger);
}

void clearFailedPendingPlayerGrenadeDanger(SavedBattleGame *save, BattleItem *item)
{
	if (!save || !item)
	{
		return;
	}
	cleanupPendingPlayerGrenadeDangers(save);
	std::vector<Position> failedTargets;
	for (const auto &danger : pendingPlayerGrenadeDangers)
	{
		if (danger.save == save && danger.item == item)
		{
			failedTargets.push_back(danger.target);
		}
	}
	pendingPlayerGrenadeDangers.erase(std::remove_if(pendingPlayerGrenadeDangers.begin(), pendingPlayerGrenadeDangers.end(),
		[save, item](const PendingPlayerGrenadeDanger &danger)
		{
			return danger.save == save && danger.item == item;
		}), pendingPlayerGrenadeDangers.end());
	cleanupPendingPlayerProximityMinePlans(save);
	pendingPlayerProximityMinePlans.erase(std::remove_if(pendingPlayerProximityMinePlans.begin(), pendingPlayerProximityMinePlans.end(),
		[save, &failedTargets](const PendingPlayerProximityMinePlan &plan)
		{
			return plan.save == save && std::find(failedTargets.begin(), failedTargets.end(), plan.target) != failedTargets.end();
		}), pendingPlayerProximityMinePlans.end());
}

int updatePlayerMovementOscillation(SavedBattleGame *save, BattleUnit *unit)
{
	if (!save || !unit)
	{
		return 0;
	}
	const int turn = save->getTurn();
	const Position pos = unit->getPosition();
	for (auto &memory : playerMovementMemory)
	{
		if (memory.save == save && memory.unitId == unit->getId())
		{
			if (memory.lastTurn != turn)
			{
				memory.oscillation = std::max(0, memory.oscillation - PLAYER_AI_MOVEMENT_OSCILLATION_DECAY);
				memory.lastTurn = turn;
				memory.turnMoveCount = 0;
				memory.turnMoveDistance = 0;
			}
			if (pos == memory.lastPosition)
			{
				memory.oscillation = std::max(0, memory.oscillation - PLAYER_AI_MOVEMENT_OSCILLATION_DECAY);
			}
			else
			{
				const int stepDistance = Position::distance2d(pos, memory.lastPosition);
				if (stepDistance > 0)
				{
					++memory.turnMoveCount;
					memory.turnMoveDistance += stepDistance;
				}
				if (pos == memory.previousPosition)
				{
					memory.oscillation += PLAYER_AI_MOVEMENT_OSCILLATION_GAIN;
				}
				else
				{
					memory.oscillation = std::max(0, memory.oscillation - PLAYER_AI_MOVEMENT_OSCILLATION_DECAY);
				}
				memory.previousPosition = memory.lastPosition;
				memory.lastPosition = pos;
			}
			return memory.oscillation;
		}
	}
	PlayerMovementMemory memory = { save, unit->getId(), pos, pos, turn, 0, 0, 0 };
	playerMovementMemory.push_back(memory);
	if (playerMovementMemory.size() > PLAYER_AI_MEMORY_LIMIT_LARGE)
	{
		playerMovementMemory.erase(playerMovementMemory.begin(), playerMovementMemory.end() - PLAYER_AI_MEMORY_LIMIT_LARGE);
	}
	return 0;
}

int getPlayerTurnMoveCount(SavedBattleGame *save, BattleUnit *unit)
{
	if (!save || !unit)
	{
		return 0;
	}
	for (const auto &memory : playerMovementMemory)
	{
		if (memory.save == save && memory.unitId == unit->getId() && memory.lastTurn == save->getTurn())
		{
			return memory.turnMoveCount;
		}
	}
	return 0;
}

int getPlayerTurnMoveDistance(SavedBattleGame *save, BattleUnit *unit)
{
	if (!save || !unit)
	{
		return 0;
	}
	for (const auto &memory : playerMovementMemory)
	{
		if (memory.save == save && memory.unitId == unit->getId() && memory.lastTurn == save->getTurn())
		{
			return memory.turnMoveDistance;
		}
	}
	return 0;
}

bool isPlayerImmediateBacktrack(SavedBattleGame *save, BattleUnit *unit, const Position &target)
{
	if (!save || !unit)
	{
		return false;
	}
	for (const auto &memory : playerMovementMemory)
	{
		if (memory.save == save
			&& memory.unitId == unit->getId()
			&& memory.lastTurn == save->getTurn()
			&& memory.turnMoveCount > 0
			&& memory.lastPosition == unit->getPosition()
			&& memory.previousPosition != memory.lastPosition)
		{
			return target == memory.previousPosition;
		}
	}
	return false;
}

bool updatePlayerFiredThisTurn(SavedBattleGame *save, BattleUnit *unit)
{
	if (!save || !unit || !unit->getStatistics())
	{
		return false;
	}
	const int turn = save->getTurn();
	const int shotsFired = unit->getStatistics()->shotsFiredCounter;
	for (auto &memory : playerTurnActionMemory)
	{
		if (memory.save == save && memory.unitId == unit->getId())
		{
			if (memory.turn != turn)
			{
				memory.turn = turn;
				memory.lastShotsFired = shotsFired;
				memory.firedThisTurn = false;
				return false;
			}
			if (shotsFired > memory.lastShotsFired)
			{
				memory.firedThisTurn = true;
				memory.lastShotsFired = shotsFired;
			}
			return memory.firedThisTurn;
		}
	}
	PlayerTurnActionMemory memory = { save, unit->getId(), turn, shotsFired, false };
	playerTurnActionMemory.push_back(memory);
	if (playerTurnActionMemory.size() > PLAYER_AI_MEMORY_LIMIT_LARGE)
	{
		playerTurnActionMemory.erase(playerTurnActionMemory.begin(), playerTurnActionMemory.end() - PLAYER_AI_MEMORY_LIMIT_LARGE);
	}
	return false;
}

int getPlayerReactionHoldHolder(SavedBattleGame *save, BattleUnit *target)
{
	if (!save || !target)
	{
		return -1;
	}
	for (const auto &memory : playerReactionHoldMemory)
	{
		if (memory.save == save && memory.turn == save->getTurn() && memory.targetId == target->getId())
		{
			for (auto *unit : *save->getUnits())
			{
				if (unit && unit->getId() == memory.unitId && !unit->isOut()
					&& unit->getFaction() == FACTION_PLAYER)
				{
					return memory.unitId;
				}
			}
		}
	}
	return -1;
}

void recordPlayerReactionHoldForTarget(SavedBattleGame *save, BattleUnit *target, BattleUnit *holder)
{
	if (!save || !target || !holder || getPlayerReactionHoldHolder(save, target) >= 0)
	{
		return;
	}
	PlayerReactionHoldMemory memory = { save, save->getTurn(), target->getId(), holder->getId() };
	playerReactionHoldMemory.push_back(memory);
	if (playerReactionHoldMemory.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
	{
		playerReactionHoldMemory.erase(playerReactionHoldMemory.begin(),
			playerReactionHoldMemory.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
	}
}

PlayerDynamicGroupInfo getPlayerDynamicGroupInfo(SavedBattleGame *save, BattleUnit *unit)
{
	PlayerDynamicGroupInfo result = { 0, 1, PDGR_POINT, PDGT_MANEUVER, true };
	if (!save || !unit)
	{
		return result;
	}
	std::vector<BattleUnit*> allies;
	for (auto *ally : *save->getUnits())
	{
		if (ally && !ally->isOut() && ally->getFaction() == FACTION_PLAYER)
		{
			allies.push_back(ally);
		}
	}
	std::sort(allies.begin(), allies.end(), [](const BattleUnit *left, const BattleUnit *right)
	{
		return left->getId() < right->getId();
	});
	std::vector<int> activeUnitIds;
	for (auto *ally : allies)
	{
		activeUnitIds.push_back(ally->getId());
	}
	const PlayerDynamicGroupLayoutMemory *cachedLayout = 0;
	for (const auto &memory : playerDynamicGroupLayoutMemory)
	{
		if (memory.save == save && memory.turn == save->getTurn() && memory.activeUnitIds == activeUnitIds)
		{
			cachedLayout = &memory;
			break;
		}
	}
	std::vector<BattleUnit*> spatiallyGrouped;
	spatiallyGrouped.reserve(allies.size());
	if (cachedLayout)
	{
		for (int unitId : cachedLayout->spatialUnitIds)
		{
			auto found = std::find_if(allies.begin(), allies.end(), [unitId](const BattleUnit *ally)
			{
				return ally->getId() == unitId;
			});
			if (found != allies.end())
			{
				spatiallyGrouped.push_back(*found);
			}
		}
	}
	else
	{
		std::vector<BattleUnit*> remaining = allies;
		while (!remaining.empty())
		{
			std::vector<BattleUnit*> group;
			group.push_back(remaining.front());
			spatiallyGrouped.push_back(remaining.front());
			remaining.erase(remaining.begin());
			while (group.size() < 3 && !remaining.empty())
			{
				auto best = remaining.begin();
				int bestDistance = std::numeric_limits<int>::max();
				for (auto candidate = remaining.begin(); candidate != remaining.end(); ++candidate)
				{
					int distance = 0;
					for (auto *member : group)
					{
						distance += Position::distance2d((*candidate)->getPosition(), member->getPosition())
							+ std::abs((*candidate)->getPosition().z - member->getPosition().z) * 6;
					}
					if (distance < bestDistance || (distance == bestDistance && (*candidate)->getId() < (*best)->getId()))
					{
						bestDistance = distance;
						best = candidate;
					}
				}
				group.push_back(*best);
				spatiallyGrouped.push_back(*best);
				remaining.erase(best);
			}
		}
		PlayerDynamicGroupLayoutMemory memory = { save, save->getTurn(), activeUnitIds, std::vector<int>() };
		for (auto *ally : spatiallyGrouped)
		{
			memory.spatialUnitIds.push_back(ally->getId());
		}
		playerDynamicGroupLayoutMemory.push_back(memory);
		if (playerDynamicGroupLayoutMemory.size() > PLAYER_AI_MEMORY_LIMIT_SMALL)
		{
			playerDynamicGroupLayoutMemory.erase(playerDynamicGroupLayoutMemory.begin(), playerDynamicGroupLayoutMemory.end() - PLAYER_AI_MEMORY_LIMIT_SMALL);
		}
	}
	allies.swap(spatiallyGrouped);
	auto own = std::find(allies.begin(), allies.end(), unit);
	if (own == allies.end())
	{
		return result;
	}
	const int ownIndex = (int)std::distance(allies.begin(), own);
	result.groupId = ownIndex / 3;
	const int groupBegin = result.groupId * 3;
	const int groupEnd = std::min(groupBegin + 3, (int)allies.size());
	result.groupSize = groupEnd - groupBegin;
	int bestGroupId = 0;
	int bestGroupScore = PLAYER_AI_REJECT_SCORE;
	std::vector<int> groupFireScores;
	for (int begin = 0, groupId = 0; begin < (int)allies.size(); begin += 3, ++groupId)
	{
		const int end = std::min(begin + 3, (int)allies.size());
		int score = (end - begin) * 100;
		int fireScore = (end - begin) * 100;
		for (int i = begin; i < end; ++i)
		{
			BattleUnit *candidate = allies[i];
			const UnitStats *stats = candidate->getBaseStats();
			const int maxHealth = std::max(1, stats ? (int)stats->health : candidate->getHealth());
			const int healthPercent = std::max(0, candidate->getHealth()) * 100 / maxHealth;
			score += healthPercent * 3
				+ (stats ? (int)stats->tu : 0)
				+ (stats ? (int)stats->reactions : 0)
				- candidate->getFatalWounds() * 100;
			fireScore += (stats ? (int)stats->firing : 0) * 3
				+ (stats ? (int)stats->reactions : 0)
				+ healthPercent
				- candidate->getFatalWounds() * 80;
		}
		groupFireScores.push_back(fireScore);
		if (score > bestGroupScore)
		{
			bestGroupScore = score;
			bestGroupId = groupId;
		}
	}
	int bestFireGroupId = -1;
	int bestFireGroupScore = PLAYER_AI_REJECT_SCORE;
	for (int groupId = 0; groupId < (int)groupFireScores.size(); ++groupId)
	{
		if (groupId != bestGroupId && groupFireScores[groupId] > bestFireGroupScore)
		{
			bestFireGroupScore = groupFireScores[groupId];
			bestFireGroupId = groupId;
		}
	}
	result.task = result.groupId == bestGroupId
		? PDGT_MANEUVER
		: (result.groupId == bestFireGroupId ? PDGT_FIRE_SUPPORT : PDGT_RESERVE);
	result.maneuverGroup = result.task == PDGT_MANEUVER;

	BattleUnit *point = 0;
	int bestPointScore = PLAYER_AI_REJECT_SCORE;
	for (int i = groupBegin; i < groupEnd; ++i)
	{
		BattleUnit *candidate = allies[i];
		const UnitStats *stats = candidate->getBaseStats();
		const int maxHealth = std::max(1, stats ? (int)stats->health : candidate->getHealth());
		const int healthPercent = std::max(0, candidate->getHealth()) * 100 / maxHealth;
		int score = healthPercent * 3
			+ (stats ? (int)stats->tu : 0) * 2
			+ (stats ? (int)stats->reactions : 0)
			- candidate->getFatalWounds() * 80;
		if (score > bestPointScore)
		{
			bestPointScore = score;
			point = candidate;
		}
	}
	if (unit == point)
	{
		result.role = PDGR_POINT;
		return result;
	}

	BattleUnit *fire = 0;
	int bestFireScore = PLAYER_AI_REJECT_SCORE;
	for (int i = groupBegin; i < groupEnd; ++i)
	{
		BattleUnit *candidate = allies[i];
		if (candidate == point)
		{
			continue;
		}
		const UnitStats *stats = candidate->getBaseStats();
		int score = (stats ? (int)stats->firing : 0) * 3
			+ (stats ? (int)stats->reactions : 0)
			+ std::max(0, candidate->getHealth())
			- candidate->getFatalWounds() * 60;
		if (score > bestFireScore)
		{
			bestFireScore = score;
			fire = candidate;
		}
	}
	result.role = unit == fire ? PDGR_FIRE : PDGR_GUARD;
	return result;
}

const char *getPlayerDynamicGroupRoleName(PlayerDynamicGroupRole role)
{
	switch (role)
	{
	case PDGR_POINT:
		return "point";
	case PDGR_FIRE:
		return "fire";
	case PDGR_GUARD:
	default:
		return "guard";
	}
}

const char *getPlayerDynamicGroupTaskName(PlayerDynamicGroupTask task)
{
	switch (task)
	{
	case PDGT_MANEUVER:
		return "maneuver";
	case PDGT_FIRE_SUPPORT:
		return "fire_support";
	case PDGT_RESERVE:
	default:
		return "reserve";
	}
}

}

/**
 * Sets up a BattleAIState.
 * @param save Pointer to the battle game.
 * @param unit Pointer to the unit.
 * @param node Pointer to the node the unit originates from.
 */
PlayerFactionAI::PlayerFactionAI(SavedBattleGame *save, BattleUnit *unit, Node *node) :
	AIModule(save, unit, node), _save(save), _unit(unit), _aggroTarget(0), _knownEnemies(0), _visibleEnemies(0), _spottingEnemies(0),
	_escapeTUs(0), _ambushTUs(0), _weaponPickedUp(false), _rifle(false), _melee(false), _blaster(false), _grenade(false),
	_didPsi(false), _AIMode(AI_PATROL), _closestDist(PLAYER_AI_INITIAL_CLOSEST_DISTANCE), _fromNode(node), _toNode(0), _foundBaseModuleToDestroy(false),
	_stalkAmbushAction(false), _cleanShotMoveAction(false), _crossLevelRouteMoveAction(false), _controlledProbeMoveAction(false), _fallbackCoverAction(false), _factionSupportMoveAction(false), _factionAI(0)
{
	_traceAI = Options::traceAI;

	_reserve = BA_NONE;
	_intelligence = _unit->getIntelligence();
	_escapeAction = BattleAction();
	_ambushAction = BattleAction();
	_attackAction = BattleAction();
	_patrolAction = BattleAction();
	_psiAction = BattleAction();
	_targetFaction = FACTION_PLAYER;
	if (_unit->getFaction() == FACTION_PLAYER || _unit->getOriginalFaction() == FACTION_NEUTRAL)
	{
		_targetFaction = FACTION_HOSTILE;
	}
}

/**
 * Deletes the BattleAIState.
 */
PlayerFactionAI::~PlayerFactionAI()
{

}

/**
 * Sets the target faction.
 */
void PlayerFactionAI::setTargetFaction(UnitFaction f)
{
	_targetFaction = f;
}

/**
 * Resets the unsaved AI state.
 */
void PlayerFactionAI::reset()
{
	// these variables are not saved in save() and also not initiated in think()
	_escapeTUs = 0;
	_ambushTUs = 0;
}

/**
 * Loads the AI state from a YAML file.
 * @param node YAML node.
 */
void PlayerFactionAI::load(const YAML::YamlNodeReader& reader)
{
	int fromNodeID = reader["fromNode"].readVal(-1);
	int toNodeID = reader["toNode"].readVal(-1);
	_AIMode = reader["AIMode"].readVal(AI_PATROL);
	reader.tryRead("wasHitBy", _wasHitBy);
	reader.tryRead("weaponPickedUp", _weaponPickedUp);
	reader.tryRead("targetFaction", _targetFaction);

	// TODO: Figure out why AI are sometimes left with junk nodes
	if (fromNodeID >= 0 && (size_t)fromNodeID < _save->getNodes()->size())
	{
		_fromNode = _save->getNodes()->at(fromNodeID);
	}
	if (toNodeID >= 0 && (size_t)toNodeID < _save->getNodes()->size())
	{
		_toNode = _save->getNodes()->at(toNodeID);
	}
}

/**
 * Saves the AI state to a YAML file.
 * @return YAML node.
 */
void PlayerFactionAI::save(YAML::YamlNodeWriter writer) const
{
	writer.setAsMap();
	writer.setFlowStyle();
	writer.write("fromNode", _fromNode ? _fromNode->getID() : -1);
	writer.write("toNode", _toNode ? _toNode->getID() : -1);
	writer.write("AIMode", _AIMode);
	writer.write("wasHitBy", _wasHitBy);
	if (_weaponPickedUp)
		writer.write("weaponPickedUp", _weaponPickedUp);
	if (_unit->getOriginalFaction() == FACTION_HOSTILE && _unit->getFaction() == FACTION_NEUTRAL && _targetFaction == FACTION_HOSTILE)
	{
		writer.write("targetFaction", _targetFaction);
	}
}

/**
 * Mindless charge strategy. For mindless units.
 * Consists of running around and charging nearest visible enemy.
 * @param action (possible) AI action to execute after thinking is done.
 */
void PlayerFactionAI::dont_think(BattleAction *action)
{
	_melee = false;
	action->weapon = _unit->getUtilityWeapon(BT_MELEE);

	if (_traceAI)
	{
		Log(LOG_INFO) << "LEEROY: Unit " << _unit->getId() << " of type " << _unit->getType() << " is Leeroy...";
	}
	if (action->weapon)
	{
		if (action->weapon->getRules()->getBattleType() == BT_MELEE)
		{
			if (_save->canUseWeapon(action->weapon, _unit, false, BA_HIT))
			{
				_melee = true;
			}
		}
		else
		{
			action->weapon = 0;
		}
	}

	bool canRun = _melee && _unit->getArmor()->allowsRunning(false) && _unit->getEnergy() > _unit->getBaseStats()->stamina * PLAYER_AI_LEEROY_RUN_STAMINA_FRACTION;
	int visibleEnemiesToAttack = selectNearestTargetLeeroy(canRun);
	if (_traceAI)
	{
		Log(LOG_INFO) << "LEEROY: visibleEnemiesToAttack: " << visibleEnemiesToAttack << " _melee: " << _melee << (canRun ? " run" : "");
	}
	if ((visibleEnemiesToAttack > 0) && _melee)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "LEEROY: LEEROYIN' at someone!";
		}
		meleeActionLeeroy(canRun);
		action->type = _attackAction.type;
		action->run = _attackAction.run;
		action->target = _attackAction.target;
		// if this is a firepoint action, set our facing.
		action->finalFacing = _attackAction.finalFacing;
		action->updateTU();
	}
	else
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "LEEROY: No one to LEEROY!, patrolling...";
		}
		setupPatrol();
		_unit->setCharging(0);
		_reserve = BA_NONE;
		action->type = _patrolAction.type;
		action->target = _patrolAction.target;
	}
}

/**
 * Tries to use self-target medikit if needed and desired (used for AI).
 * @return Was it used?
 */
bool PlayerFactionAI::medikit_think(BattleMediKitType healOrStim)
{
	// 1. sanity checks, division by zero
	BattleUnit* self = _unit;

	if (self->getBaseStats()->stamina <= 0 || self->getBaseStats()->health <= 0)
	{
		return false;
	}

	// 2. quick unit checks (without RNG)
	int totalWounds = self->getFatalWounds();
	int percentHealthLeft = Clamp((self->getHealth() - self->getStunlevel()) * PLAYER_AI_PERCENT / self->getBaseStats()->health, 0, PLAYER_AI_PERCENT);
	int percentEnergyLeft = Clamp(self->getEnergy() * PLAYER_AI_PERCENT / self->getBaseStats()->stamina, 0, PLAYER_AI_PERCENT);

	if (healOrStim == BMT_HEAL)
	{
		if (totalWounds <= 0)
			return false;
	}
	else if (healOrStim == BMT_STIMULANT)
	{
		if (self->getStunlevel() <= 0 && percentEnergyLeft >= PLAYER_AI_MEDIKIT_LOW_ENERGY_PERCENT)
			return false;
	}
	else
	{
		// unsupported medikit type
		return false;
	}

	// 3. quick item checks
	std::vector<BattleItem*> usableMedikits;

	for (auto* item : *self->getInventory())
	{
		const RuleItem* itemRule = item->getRules();
		if (itemRule->getBattleType() == BT_MEDIKIT &&
			(itemRule->getMediKitType() == healOrStim || itemRule->getMediKitType() == BMT_NORMAL) &&
			itemRule->getAllowTargetSelf())
		{
			if (_save->getTurn() < itemRule->getAIUseDelay(_save->getMod()))
			{
				// can't use it yet, too soon
				continue;
			}
			usableMedikits.push_back(item);
		}
	}
	if (usableMedikits.empty())
	{
		// no compatible medikits available
		return false;
	}

	// 4. detailed unit checks (with RNG)
	bool wantsToHeal = false;
	bool wantsToStimStun = false;
	bool wantsToStimEnergy = false;

	if (healOrStim == BMT_HEAL)
	{
		if (totalWounds > 0)
		{
			const bool criticalBleeding = totalWounds >= 2 || percentHealthLeft <= 60;
			if (criticalBleeding || self->getStunlevel() + totalWounds >= self->getHealth())
			{
				// Fatal wounds are cumulative and deterministic; a critically wounded
				// soldier must not gamble the heal on an RNG roll.
				wantsToHeal = true;
			}
			else
			{
				//  0% health left = 120% chance to heal
				// 15% health left =  60% chance to heal
				// 30% health left =   0% chance to heal (actually 5% chance because of random heal wish)
				int chanceToHeal = PLAYER_AI_MEDIKIT_HEAL_BASE_CHANCE - (percentHealthLeft * PLAYER_AI_MEDIKIT_HEAL_HEALTH_SCALE);
				if (chanceToHeal <= 0)
				{
					// 5% for random heal wish (it's not urgent, but you know damage accumulates over time)
					chanceToHeal = PLAYER_AI_MEDIKIT_RANDOM_HEAL_CHANCE;
				}
				wantsToHeal = RNG::percent(chanceToHeal);
			}
		}
		if (!wantsToHeal)
		{
			return false;
		}
	}
	else if (healOrStim == BMT_STIMULANT)
	{
		// 1. do we want to decrease stun level?
		if (self->getStunlevel() > 0)
		{
			if (self->getStunlevel() + totalWounds >= self->getHealth())
			{
				// going to die or pass out unless we do something, so do something!
				wantsToStimStun = true;
			}
			else
			{
				//  0% health left = 140% chance to stim
				// 10% health left =  70% chance to stim
				// 20% health left =   0% chance to stim
				int chanceToStim1 = PLAYER_AI_MEDIKIT_STUN_BASE_CHANCE - (percentHealthLeft * PLAYER_AI_MEDIKIT_STUN_HEALTH_SCALE);
				wantsToStimStun = chanceToStim1 > 0 ? RNG::percent(chanceToStim1) : false;
			}
		}
		// 2. do we want to increase energy?
		if (percentEnergyLeft < PLAYER_AI_MEDIKIT_LOW_ENERGY_PERCENT)
		{
			//  0% energy left = 120% chance to stim
			// 20% energy left =  60% chance to stim
			// 40% energy left =   0% chance to stim
			int chanceToStim2 = PLAYER_AI_MEDIKIT_ENERGY_BASE_CHANCE - (percentEnergyLeft * PLAYER_AI_MEDIKIT_ENERGY_SCALE);
			wantsToStimEnergy = RNG::percent(chanceToStim2);
		}
		if (!wantsToStimStun && !wantsToStimEnergy)
		{
			return false;
		}
	}

	// 5. let's do it
	bool used = false;

	for (auto* medikit : usableMedikits)
	{
		const RuleItem* medikitRule = medikit->getRules();
		{
			if ((wantsToHeal && medikit->getHealQuantity() > 0) ||
				(wantsToStimStun && medikit->getStimulantQuantity() > 0 && medikitRule->getStunRecovery() > 0) ||
				(wantsToStimEnergy && medikit->getStimulantQuantity() > 0 && medikitRule->getEnergyRecovery() > 0))
			{
				BattleAction medikitAction;
				{
					medikitAction.weapon = medikit;
					medikitAction.type = BA_USE;
					medikitAction.actor = self;

					medikitAction.updateTU();

					// yes, hardcoded 4 TUs
					// AI throwing grenades does that for decades and nobody cares, so calm down
					// also, AI pays this cost each time, even if using the same medikit multiple times in a row
					medikitAction.Time += PLAYER_AI_PICKUP_TU_COST; // 4TUs for picking up the medikit

					// sigh, modders...
					//medikitAction.Health = 0;
					//medikitAction.Stun = 0;
				}
				if (!medikitAction.spendTU())
				{
					// not enough TUs, try next item
					continue;
				}
				else
				{
					switch (healOrStim)
					{
					case BMT_HEAL:
						if (_traceAI)
						{
							Log(LOG_INFO) << "  Using medikit (heal). TU*/HP/Stun/Wounds: " <<
								self->getTimeUnits() << "/" << self->getHealth() << "/" << self->getStunlevel() << "/" << totalWounds;
						}
						for (int i = 0; i < BODYPART_MAX; ++i)
						{
							if (self->getFatalWound((UnitBodyPart)i))
							{
								_save->getTileEngine()->medikitUse(&medikitAction, self, BMA_HEAL, (UnitBodyPart)i);
								_save->getTileEngine()->medikitRemoveIfEmpty(&medikitAction);
								used = true;
								break;
							}
						}
						break;
					case BMT_STIMULANT:
						if (_traceAI)
						{
							if (wantsToStimStun)
							{
								Log(LOG_INFO) << "  Using medikit (-stun). TU*/HP/Stun/Wounds: " <<
									self->getTimeUnits() << "/" << self->getHealth() << "/" << self->getStunlevel() << "/" << totalWounds;
							}
							else
							{
								Log(LOG_INFO) << "  Using medikit (+energy). TU*/Energy: " << self->getTimeUnits() << "/" << self->getEnergy();
							}
						}
						_save->getTileEngine()->medikitUse(&medikitAction, self, BMA_STIMULANT, BODYPART_TORSO);
						_save->getTileEngine()->medikitRemoveIfEmpty(&medikitAction);
						used = true;
						break;
					case BMT_PAINKILLER:
					case BMT_NORMAL:
						// not supported
						break;
					}
				}
			}
		}
		if (used)
		{
			// only one use per attempt
			break;
		}
	}

	// 6. if we used something, let's try again
	return used;
}

PlayerFactionAI::PlayerAIRole PlayerFactionAI::getPlayerAIRole(BattleItem *weapon) const
{
	if (!weapon)
	{
		return ROLE_SUPPORT;
	}

	const RuleItem *rule = weapon->getRules();
	const UnitStats *stats = _unit->getBaseStats();
	if (rule->getBattleType() == BT_MELEE)
	{
		return ROLE_MELEE;
	}

	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(BA_SNAPSHOT, _unit, weapon);
	BattleItem *ammo = weapon->getAmmoForAction(BA_SNAPSHOT);
	const bool explosive = ammo && ammo->getRules()->getExplosionRadius(attack) > 0;
	const bool heavy = weapon->getTotalWeight() > stats->strength / 2 || explosive || weapon->getCurrentWaypoints() != 0;
	if (heavy && stats->strength >= weapon->getTotalWeight())
	{
		return ROLE_HEAVY;
	}

	const bool accurateWeapon = rule->getAccuracyAimed() >= PLAYER_AI_MARKSMAN_AIMED_ACCURACY || (rule->getAccuracySnap() >= PLAYER_AI_MARKSMAN_SNAP_ACCURACY && rule->getAccuracyAuto() == 0);
	if (stats->firing >= PLAYER_AI_MARKSMAN_FIRING && accurateWeapon)
	{
		int betterMarksmen = 0;
		for (auto *other : *_save->getUnits())
		{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
			{
				continue;
			}
			BattleItem *otherWeapon = other->getMainHandWeapon(false);
			if (!otherWeapon || !otherWeapon->getRules())
			{
				continue;
			}
			const RuleItem *otherRule = otherWeapon->getRules();
			const bool otherAccurate = otherRule->getAccuracyAimed() >= PLAYER_AI_MARKSMAN_AIMED_ACCURACY
				|| (otherRule->getAccuracySnap() >= PLAYER_AI_MARKSMAN_SNAP_ACCURACY && otherRule->getAccuracyAuto() == 0);
			if (otherAccurate && other->getBaseStats()->firing > stats->firing + 2)
			{
				++betterMarksmen;
			}
		}
		if (betterMarksmen < 4)
		{
			return ROLE_MARKSMAN;
		}
	}

	if (stats->tu >= PLAYER_AI_ASSAULT_TU || stats->reactions >= PLAYER_AI_ASSAULT_REACTIONS
		|| (rule->getAccuracyAuto() > 0 && (stats->tu >= 60 || stats->reactions >= 50)))
	{
		return ROLE_ASSAULT;
	}

	return ROLE_SUPPORT;
}

const char *PlayerFactionAI::getPlayerAIRoleName(PlayerAIRole role) const
{
	switch (role)
	{
	case ROLE_SUPPORT:
		return "support";
	case ROLE_ASSAULT:
		return "assault";
	case ROLE_MARKSMAN:
		return "marksman";
	case ROLE_HEAVY:
		return "heavy";
	case ROLE_MELEE:
		return "melee";
	default:
		return "unknown";
	}
}

PlayerFactionAI::PlayerAITacticalRole PlayerFactionAI::getPlayerTacticalRole(BattleItem *weapon, PlayerAIRole role) const
{
	const UnitStats *stats = _unit->getBaseStats();
	// A grenade, empty hand, or otherwise unusable item must not make its carrier
	// the squad's point scout.  Such units should stay with the line while the
	// weapon-pickup logic rearms them (or while they wait for a safe throw).
	if (scoreWeaponForUnit(weapon) <= PLAYER_AI_REJECT_SCORE)
	{
		return TACTICAL_REACTION_GUARD;
	}
	if (role == ROLE_MARKSMAN || role == ROLE_HEAVY)
	{
		return TACTICAL_FIRE_SUPPORT;
	}
	if (weapon && weapon->getRules()->getBattleType() == BT_FIREARM
		&& stats->reactions >= PLAYER_AI_REACTION_SPECIALIST_MIN_REACTIONS
		&& stats->tu >= PLAYER_AI_REACTION_SPECIALIST_MIN_TU
		&& stats->reactions + PLAYER_AI_REACTION_FIRING_TOLERANCE >= stats->firing)
	{
		return TACTICAL_REACTION_GUARD;
	}
	const int maxHealth = std::max(1, (int)stats->health);
	const int selfScore = (int)stats->tu * PLAYER_AI_SCOUT_TU_SCORE + (int)stats->reactions + _unit->getHealth() * PLAYER_AI_SCOUT_HEALTH_SCORE / maxHealth - (weapon ? weapon->getTotalWeight() : 0);
	int betterScouts = 0;
	for (auto *other : *_save->getUnits())
	{
		if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
		{
			continue;
		}
		BattleItem *otherWeapon = other->getMainHandWeapon(false);
		const UnitStats *otherStats = other->getBaseStats();
		const int otherMaxHealth = std::max(1, (int)otherStats->health);
		const int otherScore = (int)otherStats->tu * PLAYER_AI_SCOUT_TU_SCORE + (int)otherStats->reactions + other->getHealth() * PLAYER_AI_SCOUT_HEALTH_SCORE / otherMaxHealth - (otherWeapon ? otherWeapon->getTotalWeight() : 0);
		if (otherScore > selfScore + PLAYER_AI_SCOUT_SCORE_MARGIN)
		{
			++betterScouts;
		}
	}
	if (role != ROLE_HEAVY && role != ROLE_MARKSMAN && role != ROLE_MELEE && _unit->getHealth() * PLAYER_AI_PERCENT / maxHealth >= PLAYER_AI_LOW_HEALTH_PERCENT && betterScouts < PLAYER_AI_SCOUT_BETTER_LIMIT)
	{
		return TACTICAL_SCOUT;
	}
	if (role == ROLE_SUPPORT)
	{
		return TACTICAL_REACTION_GUARD;
	}
	return TACTICAL_ASSAULT;
}

const char *PlayerFactionAI::getPlayerTacticalRoleName(PlayerAITacticalRole role) const
{
	switch (role)
	{
	case TACTICAL_SCOUT:
		return "scout";
	case TACTICAL_REACTION_GUARD:
		return "reaction_guard";
	case TACTICAL_FIRE_SUPPORT:
		return "fire_support";
	case TACTICAL_ASSAULT:
	default:
		return "assault";
	}
}

PlayerFactionStrategy PlayerFactionAI::getFactionStrategy() const
{
	const PlayerFactionStrategy globalStrategy = _factionAI ? _factionAI->getPlayerStrategy() : PFS_HOLD_REACTION;
	if (_unit->getFaction() != FACTION_PLAYER)
	{
		return globalStrategy;
	}
	int activeAllies = 0;
	for (auto *ally : *_save->getUnits())
	{
		if (ally && !ally->isOut() && ally->getFaction() == FACTION_PLAYER)
		{
			++activeAllies;
		}
	}
	const bool badlyWounded = _unit->getHealth() < std::max(1, _unit->getBaseStats()->health / 2) || _unit->getFatalWounds() > 1;
	if (badlyWounded)
	{
		return PFS_SURVIVE;
	}
	if (activeAllies < 4)
	{
		return globalStrategy;
	}
	const PlayerDynamicGroupInfo group = getPlayerDynamicGroupInfo(_save, _unit);
	// Deployment and an exposed squad-wide retreat require every group to clear
	// the same bottleneck.  In all other modes, give the dynamic groups distinct
	// jobs instead of making the whole faction advance or hold in lockstep.
	if (globalStrategy == PFS_INITIAL_DEPLOY || globalStrategy == PFS_RETREAT_REGROUP)
	{
		return globalStrategy;
	}
	if (group.task == PDGT_MANEUVER)
	{
		if (globalStrategy == PFS_HOLD_REACTION || globalStrategy == PFS_DEFEND_LINE || globalStrategy == PFS_SURVIVE)
		{
			return PFS_SKIRMISH;
		}
		return globalStrategy;
	}
	if (group.task == PDGT_FIRE_SUPPORT)
	{
		if (globalStrategy == PFS_ASSAULT || globalStrategy == PFS_HUNT_LAST_ENEMY
			|| globalStrategy == PFS_SKIRMISH || globalStrategy == PFS_SIEGE_ROOM)
		{
			return PFS_DEFEND_LINE;
		}
		return globalStrategy;
	}
	return globalStrategy == PFS_SURVIVE ? PFS_SURVIVE : PFS_HOLD_REACTION;
}

const char *PlayerFactionAI::getFactionStrategyName(PlayerFactionStrategy strategy) const
{
	switch (strategy)
	{
	case PFS_INITIAL_DEPLOY:
		return "initial_deploy";
	case PFS_DEFEND_LINE:
		return "defend_line";
	case PFS_SIEGE_ROOM:
		return "siege_room";
	case PFS_HUNT_LAST_ENEMY:
		return "hunt_last_enemy";
	case PFS_SURVIVE:
		return "survive";
	case PFS_RETREAT_REGROUP:
		return "retreat_regroup";
	case PFS_SKIRMISH:
		return "skirmish";
	case PFS_ASSAULT:
		return "assault";
	case PFS_HOLD_REACTION:
	default:
		return "hold_reaction";
	}
}

void PlayerFactionAI::applyFactionStrategyToModeOdds(PlayerFactionStrategy strategy, PlayerAIRole role, int *escapeOdds, int *ambushOdds, int *combatOdds, int *patrolOdds) const
{
	auto scale = [](int value, int percent) -> int
	{
		return std::max(0, value * percent / PLAYER_AI_PERCENT);
	};

	switch (strategy)
	{
	case PFS_INITIAL_DEPLOY:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_INITIAL_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_INITIAL_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_INITIAL_COMBAT);
		*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_INITIAL_PATROL);
		if (role == ROLE_MARKSMAN || role == ROLE_HEAVY)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_INITIAL_FIRE_SUPPORT_AMBUSH);
		}
		else if (role == ROLE_ASSAULT)
		{
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_INITIAL_ASSAULT_COMBAT);
		}
		break;
	case PFS_DEFEND_LINE:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_DEFEND_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_DEFEND_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_DEFEND_COMBAT);
		*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_DEFEND_PATROL);
		if (role == ROLE_MARKSMAN || role == ROLE_HEAVY)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_DEFEND_FIRE_SUPPORT_AMBUSH);
		}
		else if (role == ROLE_ASSAULT)
		{
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_DEFEND_ASSAULT_COMBAT);
		}
		break;
	case PFS_SIEGE_ROOM:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_SIEGE_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_SIEGE_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_SIEGE_COMBAT);
		*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_SIEGE_PATROL);
		if (role == ROLE_ASSAULT || role == ROLE_MELEE)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_SIEGE_ENTRY_AMBUSH);
		}
		if (role == ROLE_MARKSMAN)
		{
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_SIEGE_MARKSMAN_COMBAT);
		}
		break;
	case PFS_HOLD_REACTION:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_HOLD_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_HOLD_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_HOLD_COMBAT);
		*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_HOLD_PATROL);
		if (role == ROLE_SUPPORT || role == ROLE_MARKSMAN)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_HOLD_SUPPORT_AMBUSH);
		}
		break;
	case PFS_ASSAULT:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_ASSAULT_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_ASSAULT_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_ASSAULT_COMBAT);
		*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_ASSAULT_PATROL);
		if (role == ROLE_ASSAULT || role == ROLE_MELEE)
		{
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_ASSAULT_FRONT_COMBAT);
		}
		if (role == ROLE_MARKSMAN || role == ROLE_HEAVY)
		{
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_ASSAULT_FIRE_SUPPORT_COMBAT);
		}
		break;
	case PFS_SURVIVE:
		*escapeOdds = scale(*escapeOdds, (_spottingEnemies || _visibleEnemies) ? PLAYER_AI_ODDS_SURVIVE_CONTACT_ESCAPE : PLAYER_AI_ODDS_SURVIVE_SAFE_ESCAPE);
		*ambushOdds = scale(*ambushOdds, (_spottingEnemies || _visibleEnemies) ? PLAYER_AI_ODDS_SURVIVE_CONTACT_AMBUSH : PLAYER_AI_ODDS_SURVIVE_SAFE_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_SURVIVE_COMBAT);
		*patrolOdds = 0;
		if (role == ROLE_SUPPORT || role == ROLE_HEAVY)
		{
			*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_SURVIVE_SUPPORT_ESCAPE);
		}
		else if (role == ROLE_ASSAULT)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_SURVIVE_ASSAULT_AMBUSH);
		}
		break;
	case PFS_SKIRMISH:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_SKIRMISH_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_SKIRMISH_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_SKIRMISH_COMBAT);
		*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_SKIRMISH_PATROL);
		if (role == ROLE_SUPPORT || role == ROLE_MARKSMAN || role == ROLE_HEAVY)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_SKIRMISH_SUPPORT_AMBUSH);
		}
		else if (role == ROLE_ASSAULT || role == ROLE_MELEE)
		{
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_SKIRMISH_FRONT_COMBAT);
		}
		break;
	case PFS_RETREAT_REGROUP:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_RETREAT_ESCAPE);
		*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_RETREAT_AMBUSH);
		*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_RETREAT_COMBAT);
		*patrolOdds = 0;
		if (role == ROLE_ASSAULT || role == ROLE_MELEE)
		{
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_RETREAT_FRONT_AMBUSH);
		}
		break;
	case PFS_HUNT_LAST_ENEMY:
		*escapeOdds = scale(*escapeOdds, PLAYER_AI_ODDS_HUNT_ESCAPE);
		*ambushOdds = scale(*ambushOdds, (_visibleEnemies || _knownEnemies <= 1) ? PLAYER_AI_ODDS_HUNT_VISIBLE_AMBUSH : PLAYER_AI_ODDS_HUNT_HIDDEN_AMBUSH);
		*combatOdds = scale(*combatOdds, _visibleEnemies ? PLAYER_AI_ODDS_HUNT_VISIBLE_COMBAT : PLAYER_AI_ODDS_HUNT_HIDDEN_COMBAT);
		*patrolOdds = scale(*patrolOdds, _visibleEnemies ? PLAYER_AI_ODDS_HUNT_VISIBLE_PATROL : PLAYER_AI_ODDS_HUNT_HIDDEN_PATROL);
		if (role == ROLE_ASSAULT || role == ROLE_MELEE)
		{
			*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_HUNT_FRONT_PRESSURE);
			*combatOdds = scale(*combatOdds, PLAYER_AI_ODDS_HUNT_FRONT_PRESSURE);
		}
		if (role == ROLE_MARKSMAN || role == ROLE_HEAVY)
		{
			*patrolOdds = scale(*patrolOdds, PLAYER_AI_ODDS_HUNT_FIRE_SUPPORT_PATROL);
			*ambushOdds = scale(*ambushOdds, PLAYER_AI_ODDS_HUNT_FIRE_SUPPORT_AMBUSH);
		}
		break;
	default:
		break;
	}

}

int PlayerFactionAI::getPreferredEngagementRange(BattleItem *weapon) const
{
	const PlayerAIRole role = getPlayerAIRole(weapon);
	const UnitStats *stats = _unit->getBaseStats();
	int preferred = PLAYER_AI_RANGE_DEFAULT;
	switch (role)
	{
	case ROLE_MARKSMAN:
		preferred = PLAYER_AI_RANGE_MARKSMAN;
		break;
	case ROLE_HEAVY:
		preferred = PLAYER_AI_RANGE_HEAVY;
		break;
	case ROLE_ASSAULT:
		preferred = PLAYER_AI_RANGE_ASSAULT;
		break;
	case ROLE_MELEE:
		preferred = PLAYER_AI_RANGE_MELEE;
		break;
	case ROLE_SUPPORT:
	default:
		preferred = PLAYER_AI_RANGE_SUPPORT;
		break;
	}

	if (weapon)
	{
		const RuleItem *rule = weapon->getRules();
		if (rule->getAccuracyAimed() >= PLAYER_AI_RANGE_ACCURATE_AIMED && stats->firing >= PLAYER_AI_RANGE_GOOD_FIRING)
		{
			preferred += PLAYER_AI_RANGE_ACCURATE_BONUS;
		}
		if (rule->getAccuracyAuto() > 0 && rule->getAccuracyAimed() < PLAYER_AI_RANGE_AUTO_AIMED_LIMIT)
		{
			preferred -= PLAYER_AI_RANGE_AUTO_PENALTY;
		}
		if (weapon->getTotalWeight() > stats->strength)
		{
			preferred += PLAYER_AI_RANGE_OVERWEIGHT_BONUS;
		}
	}

	if (stats->firing < PLAYER_AI_RANGE_LOW_FIRING)
	{
		preferred -= PLAYER_AI_RANGE_LOW_FIRING_PENALTY;
	}
	else if (stats->firing >= PLAYER_AI_RANGE_HIGH_FIRING)
	{
		preferred += PLAYER_AI_RANGE_HIGH_FIRING_BONUS;
	}
	return Clamp(preferred, PLAYER_AI_RANGE_MIN, PLAYER_AI_RANGE_MAX);
}

int PlayerFactionAI::scoreWeaponForUnit(BattleItem *weapon) const
{
	if (!weapon || !_save->canUseWeapon(weapon, _unit, false, BA_NONE))
	{
		return PLAYER_AI_REJECT_SCORE;
	}

	const RuleItem *rule = weapon->getRules();
	const UnitStats *stats = _unit->getBaseStats();
	if (rule->getBattleType() != BT_FIREARM && rule->getBattleType() != BT_MELEE)
	{
		return PLAYER_AI_REJECT_SCORE;
	}

	int score = rule->getPower() * PLAYER_AI_WEAPON_POWER_SCORE;
	score += rule->getAccuracySnap() * std::max(PLAYER_AI_WEAPON_MIN_SKILL, (int)stats->firing) / PLAYER_AI_WEAPON_SNAP_SCALE;
	score += rule->getAccuracyAimed() * std::max(PLAYER_AI_WEAPON_MIN_SKILL, (int)stats->firing) / PLAYER_AI_WEAPON_AIMED_SCALE;
	score += rule->getAccuracyAuto() * std::max(PLAYER_AI_WEAPON_MIN_SKILL, (int)stats->reactions) / PLAYER_AI_WEAPON_AUTO_SCALE;
	if (rule->getBattleType() == BT_MELEE)
	{
		score += rule->getAccuracyMelee() * std::max(PLAYER_AI_WEAPON_MIN_SKILL, (int)stats->melee) / PLAYER_AI_WEAPON_MELEE_SCALE;
		score -= PLAYER_AI_WEAPON_MELEE_PENALTY;
	}
	if (weapon->getCurrentWaypoints() != 0)
	{
		score += PLAYER_AI_WEAPON_WAYPOINT_BONUS;
	}
	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(BA_SNAPSHOT, _unit, weapon);
	BattleItem *ammo = weapon->getAmmoForAction(BA_SNAPSHOT);
	if (ammo && ammo->getRules()->getExplosionRadius(attack) > 0)
	{
		score += ammo->getRules()->getExplosionRadius(attack) * PLAYER_AI_WEAPON_EXPLOSIVE_RADIUS_BONUS;
	}
	const int weight = weapon->getTotalWeight();
	if (weight > stats->strength)
	{
		score -= (weight - stats->strength) * PLAYER_AI_WEAPON_OVERWEIGHT_PENALTY;
	}
	else
	{
		score -= weight * PLAYER_AI_WEAPON_WEIGHT_PENALTY;
	}
	if (!weapon->haveAnyAmmo() && weapon->isWeaponWithAmmo())
	{
		score -= PLAYER_AI_WEAPON_NO_AMMO_PENALTY;
	}
	return score;
}

BattleItem *PlayerFactionAI::selectBestCarriedWeapon() const
{
	BattleItem *best = _unit->getMainHandWeapon(false);
	int bestScore = scoreWeaponForUnit(best);
	BattleItem *rightHand = _unit->getItem(_save->getMod()->getInventoryRightHand());
	BattleItem *leftHand = _unit->getItem(_save->getMod()->getInventoryLeftHand());
	BattleItem *hands[] = { rightHand, leftHand };
	for (auto *item : hands)
	{
		int score = scoreWeaponForUnit(item);
		if (score > bestScore)
		{
			bestScore = score;
			best = item;
		}
	}
	return best;
}

bool PlayerFactionAI::tryEquipGroundWeapon(BattleItem *item)
{
	if (!item || !item->getTile() || item->getTile()->getPosition() != _unit->getPosition())
	{
		return false;
	}
	const int previousScore = scoreWeaponForUnit(selectBestCarriedWeapon());
	const int itemScore = scoreWeaponForUnit(item);
	const int requiredGain = _knownEnemies ? PLAYER_AI_EQUIP_CONTACT_REQUIRED_GAIN : PLAYER_AI_EQUIP_SAFE_REQUIRED_GAIN;
	if (previousScore > -50000 && itemScore <= previousScore + requiredGain)
	{
		return false;
	}

	const RuleInventory *slot = 0;
	BattleItem *replaced = 0;
	if (!_unit->getItem(_save->getMod()->getInventoryRightHand()))
	{
		slot = _save->getMod()->getInventoryRightHand();
	}
	else if (!_unit->getItem(_save->getMod()->getInventoryLeftHand()))
	{
		slot = _save->getMod()->getInventoryLeftHand();
	}
	else
	{
		BattleItem *rightHand = _unit->getItem(_save->getMod()->getInventoryRightHand());
		BattleItem *leftHand = _unit->getItem(_save->getMod()->getInventoryLeftHand());
		const int rightScore = scoreWeaponForUnit(rightHand);
		const int leftScore = scoreWeaponForUnit(leftHand);
		if (rightScore > -50000 && leftScore > -50000)
		{
			return false;
		}
		if (rightScore <= leftScore)
		{
			slot = _save->getMod()->getInventoryRightHand();
			replaced = rightHand;
		}
		else
		{
			slot = _save->getMod()->getInventoryLeftHand();
			replaced = leftHand;
		}
	}

	const int tuCost = item->getMoveToCost(slot);
	if (_unit->getTimeUnits() < tuCost)
	{
		return false;
	}
	Tile *itemTile = item->getTile();
	if (replaced)
	{
		replaced->moveToOwner(0);
		itemTile->addItem(replaced, _save->getMod()->getInventoryGround());
	}
	if (!_unit->fitItemToInventory(slot, item))
	{
		if (replaced)
		{
			itemTile->removeItem(replaced);
			_unit->fitItemToInventory(slot, replaced);
		}
		return false;
	}
	_unit->spendTimeUnits(tuCost);
	_weaponPickedUp = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction role weapon pickup: unit=" << _unit->getId()
			<< ", item=" << item->getRules()->getType()
			<< ", itemScore=" << itemScore
			<< ", previousScore=" << previousScore
			<< ", replaced=" << (replaced ? replaced->getRules()->getType() : "none")
			<< ", role=" << (int)getPlayerAIRole(item)
			<< ", tuCost=" << tuCost
			<< ", position=" << _unit->getPosition()
			<< ", reason=better_role_weapon_on_current_tile";
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::tryEquipGroundExplosive(BattleItem *item)
{
	if (!item || !item->getTile() || item->getTile()->getPosition() != _unit->getPosition() || !item->getRules()->isGrenadeOrProxy())
	{
		return false;
	}
	BattleAction action;
	action.actor = _unit;
	action.weapon = item;
	action.type = BA_THROW;
	const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
	const int itemScore = std::max(0, item->getRules()->getPower()) * 2 + radius * 45 + (item->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? 80 : 0);
	if (radius <= 0 || itemScore < 180)
	{
		return false;
	}
	const RuleInventory *slot = 0;
	BattleItem *replaced = 0;
	if (item->getRules()->getBattleType() == BT_PROXIMITYGRENADE)
	{
		const RuleInventory *utilitySlots[2] = { _save->getMod()->getInventoryBelt(), _save->getMod()->getInventoryBackpack() };
		for (const RuleInventory *candidateSlot : utilitySlots)
		{
			if (!candidateSlot)
			{
				continue;
			}
			const int tuCost = item->getMoveToCost(candidateSlot);
			if (_unit->getTimeUnits() < tuCost)
			{
				continue;
			}
			if (_unit->fitItemToInventory(candidateSlot, item))
			{
				_unit->spendTimeUnits(tuCost);
				_weaponPickedUp = true;
				_grenade = true;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction proximity mine pocket pickup: unit=" << _unit->getId()
						<< ", item=" << item->getRules()->getType()
						<< ", itemScore=" << itemScore
						<< ", radius=" << radius
						<< ", power=" << item->getRules()->getPower()
						<< ", tuCost=" << tuCost
						<< ", position=" << _unit->getPosition();
					_save->appendToAutoBattleLog(log.str());
				}
				return true;
			}
		}
		return false;
	}
	if (!_unit->getItem(_save->getMod()->getInventoryRightHand()))
	{
		slot = _save->getMod()->getInventoryRightHand();
	}
	else if (!_unit->getItem(_save->getMod()->getInventoryLeftHand()))
	{
		slot = _save->getMod()->getInventoryLeftHand();
	}
	else
	{
		BattleItem *rightHand = _unit->getItem(_save->getMod()->getInventoryRightHand());
		BattleItem *leftHand = _unit->getItem(_save->getMod()->getInventoryLeftHand());
		const int rightScore = scoreWeaponForUnit(rightHand);
		const int leftScore = scoreWeaponForUnit(leftHand);
		if (rightScore <= leftScore)
		{
			slot = _save->getMod()->getInventoryRightHand();
			replaced = rightHand;
		}
		else
		{
			slot = _save->getMod()->getInventoryLeftHand();
			replaced = leftHand;
		}
	}
	const int tuCost = item->getMoveToCost(slot);
	if (_unit->getTimeUnits() < tuCost)
	{
		return false;
	}
	Tile *itemTile = item->getTile();
	if (replaced)
	{
		replaced->moveToOwner(0);
		itemTile->addItem(replaced, _save->getMod()->getInventoryGround());
	}
	if (!_unit->fitItemToInventory(slot, item))
	{
		if (replaced)
		{
			itemTile->removeItem(replaced);
			_unit->fitItemToInventory(slot, replaced);
		}
		return false;
	}
	_unit->spendTimeUnits(tuCost);
	_weaponPickedUp = true;
	_grenade = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction explosive pickup: unit=" << _unit->getId()
			<< ", item=" << item->getRules()->getType()
			<< ", itemScore=" << itemScore
			<< ", radius=" << radius
			<< ", power=" << item->getRules()->getPower()
			<< ", replaced=" << (replaced ? replaced->getRules()->getType() : "none")
			<< ", tuCost=" << tuCost
			<< ", position=" << _unit->getPosition();
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupRoleWeaponPickup(BattleAction *action)
{
	if (_unit->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	const bool underContact = _visibleEnemies || _spottingEnemies;
	BattleItem *currentWeapon = selectBestCarriedWeapon();
	const int currentScore = scoreWeaponForUnit(currentWeapon);
	const bool needsDirectWeapon = currentScore <= PLAYER_AI_REJECT_SCORE;
	if (_knownEnemies && currentScore > -50000 && !underContact)
	{
		return false;
	}
	BattleItem *bestItem = 0;
	Position bestPos;
	const int requiredGain = underContact ? 120 : (_knownEnemies ? 160 : 70);
	int bestScore = currentScore + requiredGain;
	int bestMoveDist = 100000;
	bool hasCarriedExplosive = false;
	bool hasCarriedProximityMine = false;
	for (auto *carried : *_unit->getInventory())
	{
		if (!carried || !carried->getRules()->isGrenadeOrProxy())
		{
			continue;
		}
		if (carried->getRules()->getBattleType() == BT_PROXIMITYGRENADE)
		{
			hasCarriedProximityMine = true;
			hasCarriedExplosive = true;
		}
		else if (_save->getTurn() >= carried->getRules()->getAIUseDelay(_save->getMod()))
		{
			hasCarriedExplosive = true;
		}
	}

	auto considerTile = [&](Tile *tile)
	{
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit) || getSpottingUnits(tile->getPosition()) > 0)
		{
			return;
		}
		if (underContact && tile->getPosition() != _unit->getPosition())
		{
			return;
		}
		for (auto *item : *tile->getInventory())
		{
			if (item && item->getRules()->isGrenadeOrProxy())
			{
				// Direct weapons and carried explosives use deliberately different score
				// scales.  Do not let a high raw HE score displace every reachable rifle
				// when this unit currently has no usable firearm or melee weapon.
				if (needsDirectWeapon)
				{
					continue;
				}
				if (item->getFuseTimer() >= 0)
				{
					continue;
				}
				if (item->getRules()->getBattleType() == BT_PROXIMITYGRENADE && hasCarriedProximityMine)
				{
					continue;
				}
				BattleAction grenadeAction;
				grenadeAction.actor = _unit;
				grenadeAction.weapon = item;
				grenadeAction.type = BA_THROW;
				const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(grenadeAction));
				const int explosiveScore = std::max(0, item->getRules()->getPower()) * 2 + radius * 45 + (item->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? 80 : 0);
				if ((!hasCarriedExplosive && explosiveScore >= 180) || explosiveScore > bestScore)
				{
					const int moveDist = Position::distance2d(_unit->getPosition(), tile->getPosition());
					if (moveDist <= (underContact ? 0 : 6))
					{
						bestItem = item;
						bestPos = tile->getPosition();
						bestScore = explosiveScore;
						bestMoveDist = moveDist;
					}
				}
				continue;
			}
			const int score = scoreWeaponForUnit(item);
			if (score <= bestScore)
			{
				continue;
			}
			const int moveDist = Position::distance2d(_unit->getPosition(), tile->getPosition());
			if (moveDist > 8)
			{
				continue;
			}
			if (_knownEnemies && currentScore > -50000 && moveDist > 2)
			{
				continue;
			}
			bestItem = item;
			bestPos = tile->getPosition();
			bestScore = score;
			bestMoveDist = moveDist;
		}
	};

	considerTile(_save->getTile(_unit->getPosition()));
	if (bestItem && bestPos == _unit->getPosition())
	{
		const bool explosiveItem = bestItem->getRules()->isGrenadeOrProxy();
		if ((explosiveItem && tryEquipGroundExplosive(bestItem)) || tryEquipGroundWeapon(bestItem))
		{
			action->weapon = selectBestCarriedWeapon();
			_attackAction.weapon = action->weapon;
			if (explosiveItem)
			{
				_attackAction.type = BA_RETHINK;
				grenadeAction();
				if (_attackAction.type == BA_RETHINK)
				{
					_attackAction.actor = _unit;
					_attackAction.weapon = action->weapon;
					_attackAction.target = _unit->getPosition();
					_attackAction.type = BA_NONE;
					_AIMode = AI_COMBAT;
				}
			}
			return true;
		}
	}

	for (auto tileIndex : _reachable)
	{
		considerTile(_save->getTile(tileIndex));
	}

	if (!bestItem || bestPos == _unit->getPosition())
	{
		return false;
	}

	_patrolAction.actor = _unit;
	_patrolAction.weapon = action->weapon;
	_patrolAction.target = bestPos;
	_patrolAction.type = BA_WALK;
	if (_attackAction.type == BA_RETHINK)
	{
		_attackAction = _patrolAction;
	}
	_AIMode = AI_COMBAT;
	// This is a deliberate rearm route, not a generic patrol step.  Preserve it
	// through the later anti-oscillation and cautious-advance filters.
	_factionSupportMoveAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction role weapon target: unit=" << _unit->getId()
			<< ", item=" << bestItem->getRules()->getType()
			<< ", itemScore=" << bestScore
			<< ", currentWeapon=" << (currentWeapon ? currentWeapon->getRules()->getType() : "none")
			<< ", currentScore=" << currentScore
			<< ", role=" << (int)getPlayerAIRole(bestItem)
			<< ", target=" << bestPos
			<< ", moveDistance=" << bestMoveDist
			<< ", reason=better_role_weapon_reachable";
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Runs any code the state needs to keep updating every AI cycle.
 * @param action (possible) AI action to execute after thinking is done.
 */
void PlayerFactionAI::think(BattleAction *action)
{
	action->type = BA_RETHINK;
	action->actor = _unit;
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		// A failed throw can leave a fuse-0 item in another squad member's hands,
		// and that carrier may die before receiving another AI cycle.  Sweep the
		// whole active player squad before planning the next action, not just the
		// currently thinking unit.
		for (auto *carrier : *_save->getUnits())
		{
			if (!carrier || carrier->getFaction() != FACTION_PLAYER)
			{
				continue;
			}
			for (auto *item : *carrier->getInventory())
			{
				if (!item || !item->getRules() || !item->getRules()->isGrenadeOrProxy()
					|| item->getFuseTimer() < 0)
				{
					continue;
				}
				const int oldFuse = item->getFuseTimer();
				clearFailedPendingPlayerGrenadeDanger(_save, item);
				item->setFuseTimer(-1);
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction retained explosive disarmed: unit=" << carrier->getId()
						<< ", detectedBy=" << _unit->getId()
						<< ", item=" << item->getRules()->getType()
						<< ", oldFuse=" << oldFuse
						<< ", reason=previous_throw_did_not_leave_inventory";
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
		// The projectile has already finished before the next AI cycle.  Refreshing
		// here captures the grenade's actual landing tile (including throw scatter)
		// before any movement or target scoring is considered.
		refreshActivePlayerExplosiveHazards(_save);
		if (_unit->getFatalWounds() > 0)
		{
			const int woundsBefore = _unit->getFatalWounds();
			const int healthBefore = _unit->getHealth();
			if (medikit_think(BMT_HEAL) && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction self treatment: unit=" << _unit->getId()
					<< ", health=" << healthBefore << "->" << _unit->getHealth()
					<< ", fatalWounds=" << woundsBefore << "->" << _unit->getFatalWounds()
					<< ", remainingTU=" << _unit->getTimeUnits()
					<< ", reason=deterministic_critical_bleeding_control";
				_save->appendToAutoBattleLog(log.str());
			}
		}
	}
	action->weapon = _unit->getFaction() == FACTION_PLAYER ? selectBestCarriedWeapon() : _unit->getMainHandWeapon(false);
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		logPlayerInitialBattleState(_save);
	}
	_cleanShotMoveAction = false;
	_crossLevelRouteMoveAction = false;
	_controlledProbeMoveAction = false;
	_fallbackCoverAction = false;
	_attackAction.diff = _save->getBattleState()->getGame()->getSavedGame()->getDifficultyCoefficient();
	_attackAction.actor = _unit;
	_attackAction.run = false;
	_attackAction.weapon = action->weapon;
	_attackAction.number = action->number;
	_escapeAction.number = action->number;
	_knownEnemies = std::max(0, countKnownTargets());
	_visibleEnemies = selectNearestTarget();
	_spottingEnemies = getSpottingUnits(_unit->getPosition());
	const int movementOscillation = _unit->getFaction() == FACTION_PLAYER ? updatePlayerMovementOscillation(_save, _unit) : 0;
	const int playerTurnMoveCount = _unit->getFaction() == FACTION_PLAYER ? getPlayerTurnMoveCount(_save, _unit) : 0;
	const int playerTurnMoveDistance = _unit->getFaction() == FACTION_PLAYER ? getPlayerTurnMoveDistance(_save, _unit) : 0;
	const bool firedThisTurn = _unit->getFaction() == FACTION_PLAYER ? updatePlayerFiredThisTurn(_save, _unit) : false;
	_melee = (_unit->getUtilityWeapon(BT_MELEE) != 0);
	_rifle = false;
	_blaster = false;
	_reachable = _save->getPathfinding()->findReachable(_unit, BattleActionCost());
	_wasHitBy.clear();
	_foundBaseModuleToDestroy = false;
	_stalkAmbushAction = false;
	_factionSupportMoveAction = false;
	bool deliberateReactionHold = false;

	if (_unit->getCharging() && _unit->getCharging()->isOut())
	{
		_unit->setCharging(0);
	}

	if (_traceAI)
	{
		if (_unit->getFaction() == FACTION_HOSTILE)
		{
			Log(LOG_INFO) << "Unit has " << _visibleEnemies << "/" << _knownEnemies << " known enemies visible, " << _spottingEnemies << " of whom are spotting him. ";
		}
		else
		{
			Log(LOG_INFO) << "Civilian Unit has " << _visibleEnemies << " enemies visible, " << _spottingEnemies << " of whom are spotting him. ";
		}
		std::string AIMode;
		switch (_AIMode)
		{
		case AI_PATROL:
			AIMode = "Patrol";
			break;
		case AI_AMBUSH:
			AIMode = "Ambush";
			break;
		case AI_COMBAT:
			AIMode = "Combat";
			break;
		case AI_ESCAPE:
			AIMode = "Escape";
			break;
		}
		Log(LOG_INFO) << "Currently using " << AIMode << " behaviour";
	}

	if (_unit->isLeeroyJenkins())
	{
		dont_think(action);
		return;
	}

	if (action->weapon)
	{
		const RuleItem *rule = action->weapon->getRules();
		if (_save->canUseWeapon(action->weapon, _unit, false, BA_NONE)) // Note: ammo is not checked here
		{
			if (rule->getBattleType() == BT_FIREARM)
			{
				if (action->weapon->getCurrentWaypoints() != 0)
				{
					_blaster = true;
					_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_AIMEDSHOT, _unit, action->weapon));
				}
				else
				{
					_rifle = true;
					_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_SNAPSHOT, _unit, action->weapon));
				}
			}
			else if (rule->getBattleType() == BT_MELEE)
			{
				_melee = true;
				_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_HIT, _unit, action->weapon));
			}
		}
		else
		{
			action->weapon = 0;
		}
	}

	BattleItem *grenadeItem = _unit->getGrenadeFromBelt(_save);
	_grenade = grenadeItem != 0;
	if (!_grenade)
	{
		for (auto *item : *_unit->getInventory())
		{
			if (item && item->getRules()->isGrenadeOrProxy()
				&& (_unit->getFaction() == FACTION_PLAYER || _save->getTurn() >= item->getRules()->getAIUseDelay(_save->getMod())))
			{
				_grenade = true;
				break;
			}
		}
	}
	const int preferredRange = _unit->getFaction() == FACTION_PLAYER ? getPreferredEngagementRange(action->weapon) : 10;
	const PlayerAIRole playerRole = _unit->getFaction() == FACTION_PLAYER ? getPlayerAIRole(action->weapon) : ROLE_ASSAULT;
	const PlayerAITacticalRole tacticalRole = _unit->getFaction() == FACTION_PLAYER ? getPlayerTacticalRole(action->weapon, playerRole) : TACTICAL_ASSAULT;
	const PlayerFactionStrategy factionStrategy = _unit->getFaction() == FACTION_PLAYER ? getFactionStrategy() : PFS_ASSAULT;
	const PlayerFactionStrategy globalFactionStrategy = (_unit->getFaction() == FACTION_PLAYER && _factionAI) ? _factionAI->getPlayerStrategy() : factionStrategy;
	const PlayerDynamicGroupInfo dynamicGroup = _unit->getFaction() == FACTION_PLAYER ? getPlayerDynamicGroupInfo(_save, _unit) : PlayerDynamicGroupInfo{ 0, 1, PDGR_POINT, PDGT_MANEUVER, true };
	int playerActiveAllies = 0;
	int playerActiveHostiles = 0;
	int playerMaxHostileHealth = 0;
	int playerMaxHostileTU = 0;
	int playerMaxHostileWeaponDanger = 0;
	int playerMaxHostileBlastRadius = 0;
	int playerLocalHostileBlastRadius = 0;
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		for (auto *other : *_save->getUnits())
		{
			if (!other || other->isOut())
			{
				continue;
			}
			if (other->getFaction() == _unit->getFaction())
			{
				++playerActiveAllies;
			}
			else if (other->getFaction() == FACTION_HOSTILE)
			{
				++playerActiveHostiles;
				playerMaxHostileHealth = std::max(playerMaxHostileHealth, other->getHealth());
				playerMaxHostileTU = std::max(playerMaxHostileTU, (int)other->getBaseStats()->tu);
				int hostileBlastRadius = playerAIWeaponBlastRadius(other->getMainHandWeapon(false));
				for (auto *enemyItem : *other->getInventory())
				{
					playerMaxHostileWeaponDanger = std::max(playerMaxHostileWeaponDanger, playerAIWeaponDanger(enemyItem));
					hostileBlastRadius = std::max(hostileBlastRadius, playerAIWeaponBlastRadius(enemyItem));
				}
				playerMaxHostileBlastRadius = std::max(playerMaxHostileBlastRadius, hostileBlastRadius);
				if (hostileBlastRadius > 0
					&& Position::distance2d(_unit->getPosition(), other->getPosition()) <= 24 + hostileBlastRadius)
				{
					playerLocalHostileBlastRadius = std::max(playerLocalHostileBlastRadius, hostileBlastRadius);
				}
			}
		}
	}
	const bool highHostileFirepower = playerMaxHostileWeaponDanger >= PLAYER_AI_HEAVY_WEAPON_DANGER;
	const bool highPressureHostileFirepower = highHostileFirepower && playerActiveHostiles >= playerActiveAllies - 2;
	const bool hostileAreaWeaponThreat = playerLocalHostileBlastRadius >= 4;
	const bool playerHighHpExplosiveReserve = _unit->getFaction() == FACTION_PLAYER
		&& highHostileFirepower
		&& playerMaxHostileHealth >= PLAYER_AI_HIGH_HP_EXPLOSIVE_TARGET
		&& _grenade;
	const bool playerSlowDangerExplosiveReserve = _unit->getFaction() == FACTION_PLAYER
		&& highHostileFirepower
		&& playerMaxHostileHealth >= PLAYER_AI_SLOW_DANGER_EXPLOSIVE_MIN_HEALTH
		&& playerMaxHostileHealth < PLAYER_AI_HIGH_HP_EXPLOSIVE_TARGET
		&& playerMaxHostileTU <= PLAYER_AI_SLOW_DANGER_MAX_TU
		&& playerActiveHostiles >= std::max(PLAYER_AI_HEAVY_ENEMY_COUNT, playerActiveAllies)
		&& _grenade;
	const bool playerHeavyExplosiveDoctrine = playerHighHpExplosiveReserve
		&& playerActiveHostiles >= std::max(PLAYER_AI_HEAVY_ENEMY_COUNT, playerActiveAllies);
	const int playerExplosiveReserveTU = (playerHighHpExplosiveReserve || playerSlowDangerExplosiveReserve) ? getPlayerBestExplosiveReserve(_unit, _save, true) : 0;
	const int playerExplosiveThrowerRank = playerExplosiveReserveTU > 0 ? getPlayerExplosiveThrowerRank(_unit, _save) : PLAYER_AI_NO_THROWER_RANK;
	const bool heavyInitialEnemyPresence = _unit->getFaction() == FACTION_PLAYER
		&& _save->getTurn() == 1
		&& !_visibleEnemies
		&& !_spottingEnemies
		&& ((_knownEnemies >= std::max(PLAYER_AI_HIDDEN_ENEMY_COUNT, playerActiveAllies)
				&& playerActiveHostiles >= playerActiveAllies)
			|| (hostileAreaWeaponThreat
				&& _knownEnemies >= 5
				&& playerActiveHostiles >= 5
				&& playerActiveAllies >= 8));
	const bool overwhelmingHeavyLanding = _unit->getFaction() == FACTION_PLAYER
		&& _save->getTurn() <= 2
		&& !_visibleEnemies
		&& !_spottingEnemies
		&& _knownEnemies >= PLAYER_AI_HIDDEN_ENEMY_COUNT
		&& playerActiveHostiles > playerActiveAllies
		&& highPressureHostileFirepower
		&& (playerMaxHostileHealth >= PLAYER_AI_OVERWHELMING_TARGET_HEALTH || hostileAreaWeaponThreat);
	if (_unit->getFaction() == FACTION_PLAYER && Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction role: unit=" << _unit->getId()
			<< ", role=" << getPlayerAIRoleName(playerRole)
			<< ", tactical=" << getPlayerTacticalRoleName(tacticalRole)
			<< ", strategy=" << getFactionStrategyName(factionStrategy)
			<< ", globalStrategy=" << getFactionStrategyName(globalFactionStrategy)
			<< ", group=" << dynamicGroup.groupId
			<< ", groupSize=" << dynamicGroup.groupSize
			<< ", groupRole=" << getPlayerDynamicGroupRoleName(dynamicGroup.role)
			<< ", groupTask=" << getPlayerDynamicGroupTaskName(dynamicGroup.task)
			<< ", maneuverGroup=" << dynamicGroup.maneuverGroup
			<< ", weapon=" << (action->weapon ? action->weapon->getRules()->getType() : "none")
			<< ", weaponScore=" << scoreWeaponForUnit(action->weapon)
			<< ", preferredRange=" << preferredRange
			<< ", firing=" << _unit->getBaseStats()->firing
			<< ", reactions=" << _unit->getBaseStats()->reactions
			<< ", strength=" << _unit->getBaseStats()->strength
			<< ", tu=" << _unit->getBaseStats()->tu
			<< ", activeAllies=" << playerActiveAllies
			<< ", activeHostiles=" << playerActiveHostiles
			<< ", maxHostileHealth=" << playerMaxHostileHealth
			<< ", maxHostileTU=" << playerMaxHostileTU
			<< ", maxHostileWeaponDanger=" << playerMaxHostileWeaponDanger
			<< ", maxHostileBlastRadius=" << playerMaxHostileBlastRadius
			<< ", localHostileBlastRadius=" << playerLocalHostileBlastRadius
			<< ", highHostileFirepower=" << highHostileFirepower
			<< ", highPressureHostileFirepower=" << highPressureHostileFirepower
			<< ", hostileAreaWeaponThreat=" << hostileAreaWeaponThreat
			<< ", highHpExplosiveReserve=" << playerHighHpExplosiveReserve
			<< ", slowDangerExplosiveReserve=" << playerSlowDangerExplosiveReserve
			<< ", heavyExplosiveDoctrine=" << playerHeavyExplosiveDoctrine
			<< ", explosiveReserveTU=" << playerExplosiveReserveTU
			<< ", explosiveThrowerRank=" << playerExplosiveThrowerRank
			<< ", firedThisTurn=" << firedThisTurn
			<< ", turnMoveCount=" << playerTurnMoveCount
			<< ", turnMoveDistance=" << playerTurnMoveDistance
			<< ", heavyInitialEnemyPresence=" << heavyInitialEnemyPresence
			<< ", overwhelmingHeavyLanding=" << overwhelmingHeavyLanding;
		_save->appendToAutoBattleLog(log.str());
	}

	Tile *currentTile = _save->getTile(_unit->getPosition());
	const bool currentTileGrenadeDanger = currentTile && currentTile->getDangerous();
	const bool pendingGrenadeDanger = _unit->getFaction() == FACTION_PLAYER && isPlayerExplosiveDanger(_save, _unit->getFaction(), _unit->getPosition());
	const bool currentGrenadeDanger = _unit->getFaction() == FACTION_PLAYER && (currentTileGrenadeDanger || pendingGrenadeDanger);
	if (currentGrenadeDanger)
	{
		setupEscape();
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction grenade danger evacuation: unit=" << _unit->getId()
				<< ", from=" << _unit->getPosition()
				<< ", escapeTarget=" << _escapeAction.target
				<< ", escapeType=" << (int)_escapeAction.type
				<< ", escapeTUs=" << _escapeTUs
				<< ", pending=" << pendingGrenadeDanger
				<< ", tileDangerous=" << currentTileGrenadeDanger;
			_save->appendToAutoBattleLog(log.str());
		}
	}

	if (_spottingEnemies && !_escapeTUs)
	{
		setupEscape();
	}

	if (_unit->getFaction() == FACTION_PLAYER && !currentGrenadeDanger && !_escapeTUs
		&& (factionStrategy == PFS_RETREAT_REGROUP
			|| (factionStrategy == PFS_SURVIVE && _unit->getHealth() < _unit->getBaseStats()->health)))
	{
		setupEscape();
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction strategic escape setup: unit=" << _unit->getId()
				<< ", strategy=" << getFactionStrategyName(factionStrategy)
				<< ", escapeTarget=" << _escapeAction.target
				<< ", escapeType=" << (int)_escapeAction.type
				<< ", escapeTUs=" << _escapeTUs
				<< ", health=" << _unit->getHealth()
				<< ", maxHealth=" << _unit->getBaseStats()->health
				<< ", spotting=" << _spottingEnemies;
			_save->appendToAutoBattleLog(log.str());
		}
	}

	if (_knownEnemies && !_melee && !_ambushTUs)
	{
		setupAmbush();
	}
	setupAttack();
	if (_unit->getFaction() == FACTION_PLAYER
		&& _attackAction.type == BA_RETHINK
		&& _spottingEnemies > 0
		&& !currentGrenadeDanger)
	{
		if (setupFallbackCoverMove(_spottingEnemies >= 2 ? 55 : 75, 0, 1, 0) && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction hidden spotter fallback: unit=" << _unit->getId()
				<< ", spotting=" << _spottingEnemies
				<< ", visible=" << _visibleEnemies
				<< ", known=" << _knownEnemies
				<< ", reason=break_line_when_seen_without_attack";
			_save->appendToAutoBattleLog(log.str());
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER
		&& !firedThisTurn
		&& _aggroTarget
		&& !_aggroTarget->isOut()
		&& _aggroTarget->getBaseStats()->health > 0
		&& (_attackAction.type == BA_AUTOSHOT || _attackAction.type == BA_SNAPSHOT || _attackAction.type == BA_AIMEDSHOT)
		&& _attackAction.weapon
		&& _attackAction.weapon->getRules()->getBattleType() == BT_FIREARM)
	{
		BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(_attackAction);
		int shots = 1;
		if (_attackAction.type == BA_AUTOSHOT)
		{
			shots = _attackAction.weapon->getRules()->getConfigAuto()->shots;
		}
		else if (_attackAction.type == BA_SNAPSHOT)
		{
			shots = _attackAction.weapon->getRules()->getConfigSnap()->shots;
		}
		else if (_attackAction.type == BA_AIMEDSHOT)
		{
			shots = _attackAction.weapon->getRules()->getConfigAimed()->shots;
		}
		BattleActionCost shotCost(_attackAction.type, _unit, _attackAction.weapon);
		const int expectedDamage = estimateDirectShotDamage(&_attackAction, _aggroTarget, BattleUnit::getFiringAccuracy(attack, _save->getMod()), shots);
		const int targetHealth = std::max(1, _aggroTarget->getHealth());
		const int targetMaxHealth = std::max(1, (int)_aggroTarget->getBaseStats()->health);
		const int targetArmor = std::max(std::max(_aggroTarget->getArmor(SIDE_FRONT), _aggroTarget->getArmor(SIDE_LEFT)), _aggroTarget->getArmor(SIDE_RIGHT));
		const int currentExposure = getEnemyFireExposure(_unit->getPosition());
		const int currentFireLines = countEnemyFireLines(_unit->getPosition());
		const int afterShotTU = std::max(0, _unit->getTimeUnits() - (int)shotCost.Time);
		int strongestEnemyReactionScore = 0;
		int strongestReactionWeaponDanger = 0;
		for (auto *enemy : *_save->getUnits())
		{
			if (!enemy || enemy->isOut() || enemy->getFaction() == _unit->getFaction())
			{
				continue;
			}
			BattleItem *enemyWeapon = enemy->getMainHandWeapon(false);
			if (!enemyWeapon || enemyWeapon->getRules()->getBattleType() != BT_FIREARM
				|| !_save->canUseWeapon(enemyWeapon, enemy, false, BA_SNAPSHOT))
			{
				continue;
			}
			BattleAction enemyShot;
			enemyShot.actor = enemy;
			enemyShot.weapon = enemyWeapon;
			enemyShot.type = BA_SNAPSHOT;
			enemyShot.target = _unit->getPosition();
			BattleActionCost enemyShotCost(BA_SNAPSHOT, enemy, enemyWeapon);
			if (!enemyShotCost.haveTU())
			{
				continue;
			}
			Position enemyOrigin = _save->getTileEngine()->getOriginVoxel(enemyShot, 0);
			Position selfVoxel;
			if (!_save->getTileEngine()->canTargetUnit(&enemyOrigin, _unit->getTile(), &selfVoxel, enemy, false, _unit))
			{
				continue;
			}
			const int enemyBaseTU = std::max(1, (int)enemy->getBaseStats()->tu);
			const int reactionScore = (int)enemy->getBaseStats()->reactions * enemy->getTimeUnits() / enemyBaseTU;
			if (reactionScore > strongestEnemyReactionScore)
			{
				strongestEnemyReactionScore = reactionScore;
				strongestReactionWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
			}
		}
		const int selfPostShotReactionScore = (int)_unit->getBaseStats()->reactions * afterShotTU
			/ std::max(1, (int)_unit->getBaseStats()->tu);
		const bool attackWouldFinish = expectedDamage >= targetHealth;
		const bool durableTarget = targetHealth >= std::max(70, targetMaxHealth / 2) || targetArmor >= 35;
		const bool poorTrade = expectedDamage < std::max(18, targetHealth / 2);
		const bool exposedOpening = currentFireLines > 0 || currentExposure >= 90 || _spottingEnemies >= 2;
		const bool cannotBreakAfterShot = afterShotTU < 12 || !hasSafeRetreatFromFirePosition(_unit->getPosition(), afterShotTU);
		const bool dangerousReactionTrade = currentFireLines > 0
			&& strongestReactionWeaponDanger >= PLAYER_AI_HEAVY_WEAPON_DANGER
			&& strongestEnemyReactionScore >= selfPostShotReactionScore + 8;
		const bool defensivePoorTrade = durableTarget && poorTrade && exposedOpening && cannotBreakAfterShot;
		const int reactionHoldHolder = dangerousReactionTrade
			? getPlayerReactionHoldHolder(_save, _aggroTarget)
			: -1;
		const bool thisUnitAlreadyHoldsReaction = reactionHoldHolder == _unit->getId();
		const bool targetAlreadyGuardedByReaction = reactionHoldHolder >= 0 && !thisUnitAlreadyHoldsReaction;
		if ((defensivePoorTrade || dangerousReactionTrade) && !attackWouldFinish)
		{
			BattleAction savedAttack = _attackAction;
			const int fallbackScore = dangerousReactionTrade ? 30 : (currentExposure >= 90 ? 95 : 115);
			const int fallbackExposureGain = dangerousReactionTrade ? 0 : (currentExposure >= 90 ? 20 : 35);
			bool fallbackFound = false;
			bool fallbackCrossesReadyFire = false;
			if (!thisUnitAlreadyHoldsReaction && !targetAlreadyGuardedByReaction)
			{
				fallbackFound = setupFallbackCoverMove(fallbackScore, fallbackExposureGain,
					_spottingEnemies > 0 ? 1 : 0, dangerousReactionTrade ? 0 : 6);
				if (fallbackFound && dangerousReactionTrade)
				{
					_save->getPathfinding()->calculate(_unit, _attackAction.target, _attackAction.getMoveType());
					if (_save->getPathfinding()->getStartDirection() == -1)
					{
						fallbackCrossesReadyFire = true;
					}
					else
					{
						const std::vector<int> fallbackPath = _save->getPathfinding()->copyPath();
						Position pathPos = _unit->getPosition();
						for (auto direction = fallbackPath.rbegin(); direction != fallbackPath.rend(); ++direction)
						{
							const PathfindingStep step = _save->getPathfinding()->getTUCost(pathPos, *direction, _unit, 0, _attackAction.getMoveType());
							if (step.cost.time == Pathfinding::INVALID_MOVE_COST || countEnemyFireLines(step.pos) > 0)
							{
								fallbackCrossesReadyFire = true;
								break;
							}
							pathPos = step.pos;
						}
					}
					_save->getPathfinding()->abortPath();
					if (fallbackCrossesReadyFire)
					{
						const Position rejectedFallbackTarget = _attackAction.target;
						_fallbackCoverAction = false;
						_attackAction = savedAttack;
						if (Options::autoBattleLog)
						{
							std::ostringstream log;
							log << "Player faction pre-shot cover break path rejected: unit=" << _unit->getId()
								<< ", target=" << _aggroTarget->getId()
								<< ", coverTarget=" << rejectedFallbackTarget
								<< ", reason=intermediate_tile_remains_in_ready_heavy_fire_line";
							_save->appendToAutoBattleLog(log.str());
						}
					}
				}
			}
			if (thisUnitAlreadyHoldsReaction)
			{
				_ambushAction = savedAttack;
				_ambushAction.type = BA_NONE;
				_ambushAction.target = _unit->getPosition();
				_ambushAction.finalFacing = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition());
				_ambushTUs = std::max(1, (int)shotCost.Time);
				_AIMode = AI_AMBUSH;
				deliberateReactionHold = true;
			}
			else if (targetAlreadyGuardedByReaction)
			{
				// One soldier is enough to make the target spend TU before crossing
				// the lane.  Further shooters must exploit the shared sighting instead
				// of turning a lethal focus volley into a squad-wide idle.
				_attackAction = savedAttack;
				_AIMode = AI_COMBAT;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction guarded target focus volley: unit=" << _unit->getId()
						<< ", target=" << _aggroTarget->getId()
						<< ", action=" << (int)savedAttack.type
						<< ", expectedDamage=" << expectedDamage
						<< ", targetHealth=" << targetHealth
						<< ", fireLines=" << currentFireLines
						<< ", reason=another_soldier_already_holds_this_reaction_lane";
					_save->appendToAutoBattleLog(log.str());
				}
			}
			else if (fallbackFound && !fallbackCrossesReadyFire)
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction pre-shot cover break: unit=" << _unit->getId()
						<< ", oldAction=" << (int)savedAttack.type
						<< ", oldTarget=" << savedAttack.target
						<< ", expectedDamage=" << expectedDamage
						<< ", targetHealth=" << targetHealth
						<< ", targetMaxHealth=" << targetMaxHealth
						<< ", targetArmor=" << targetArmor
						<< ", exposure=" << currentExposure
						<< ", fireLines=" << currentFireLines
						<< ", spotting=" << _spottingEnemies
						<< ", afterShotTU=" << afterShotTU
						<< ", enemyReactionScore=" << strongestEnemyReactionScore
						<< ", selfPostShotReactionScore=" << selfPostShotReactionScore
						<< ", reactionWeaponDanger=" << strongestReactionWeaponDanger
						<< ", dangerousReactionTrade=" << dangerousReactionTrade;
					_save->appendToAutoBattleLog(log.str());
				}
			}
			else if (dangerousReactionTrade)
			{
				// A non-lethal opening shot against a readied heavy weapon commonly
				// trades one soldier for chip damage.  If no line-break exists, keep
				// all TU for reaction fire and make the enemy cross the lane instead.
				_ambushAction = savedAttack;
				_ambushAction.type = BA_NONE;
				_ambushAction.target = _unit->getPosition();
				_ambushAction.finalFacing = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition());
				_ambushTUs = std::max(1, (int)shotCost.Time);
				_AIMode = AI_AMBUSH;
				deliberateReactionHold = true;
				recordPlayerReactionHoldForTarget(_save, _aggroTarget, _unit);
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction pre-shot reaction hold: unit=" << _unit->getId()
						<< ", rejectedAction=" << (int)savedAttack.type
						<< ", target=" << _aggroTarget->getId()
						<< ", expectedDamage=" << expectedDamage
						<< ", targetHealth=" << targetHealth
						<< ", afterShotTU=" << afterShotTU
						<< ", enemyReactionScore=" << strongestEnemyReactionScore
						<< ", selfPostShotReactionScore=" << selfPostShotReactionScore
						<< ", reactionWeaponDanger=" << strongestReactionWeaponDanger
						<< ", reason=force_heavy_enemy_to_spend_tu_and_trigger_our_reaction_first";
					_save->appendToAutoBattleLog(log.str());
				}
			}
			else
			{
				_attackAction = savedAttack;
			}
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER
		&& action->number == 2
		&& firedThisTurn
		&& _aggroTarget
		&& !_aggroTarget->isOut()
		&& _aggroTarget->getBaseStats()->health > 0
		&& (_attackAction.type == BA_AUTOSHOT || _attackAction.type == BA_SNAPSHOT || _attackAction.type == BA_AIMEDSHOT)
		&& _attackAction.weapon)
	{
		BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(_attackAction);
		int shots = 1;
		if (_attackAction.type == BA_AUTOSHOT)
		{
			shots = _attackAction.weapon->getRules()->getConfigAuto()->shots;
		}
		else if (_attackAction.type == BA_SNAPSHOT)
		{
			shots = _attackAction.weapon->getRules()->getConfigSnap()->shots;
		}
		else if (_attackAction.type == BA_AIMEDSHOT)
		{
			shots = _attackAction.weapon->getRules()->getConfigAimed()->shots;
		}
		const int expectedDamage = estimateDirectShotDamage(&_attackAction, _aggroTarget, BattleUnit::getFiringAccuracy(attack, _save->getMod()), shots);
		const int targetHealth = std::max(1, _aggroTarget->getHealth());
		const int targetMaxHealth = std::max(1, (int)_aggroTarget->getBaseStats()->health);
		const int currentExposure = getEnemyFireExposure(_unit->getPosition());
		const int currentFireLines = countEnemyFireLines(_unit->getPosition());
		const bool targetAlreadyWounded = targetHealth < targetMaxHealth - 8;
		const bool attackWouldFinish = expectedDamage >= targetHealth;
		const bool weakFollowup = expectedDamage < targetHealth / 2 && expectedDamage < 18;
		const bool thickTarget = targetHealth > std::max(55, targetMaxHealth / 2);
		const bool overwhelmingReturnFire = currentExposure >= 160 && _spottingEnemies >= 3;
		const bool safeToPostponeKill = targetHealth > std::max(65, targetMaxHealth / 2);
		const bool returnFireLikely = _spottingEnemies > 0 || currentExposure >= 40 || _visibleEnemies > 0;
		const bool poorTrade = expectedDamage * 4 < targetHealth * 3;
		const bool exposedAfterPartialHit = currentExposure >= 90 || _spottingEnemies >= 2;
		const bool shouldBreakLineAfterPeek = currentFireLines > 0 && !attackWouldFinish
			&& (currentExposure >= 70 || _spottingEnemies >= 2 || expectedDamage < targetHealth / 2 || (targetHealth > 55 && expectedDamage < targetHealth * 3 / 4));
		if (shouldBreakLineAfterPeek)
		{
			BattleAction savedAttack = _attackAction;
			if (setupFallbackCoverMove(currentExposure >= 80 ? 85 : 95, 0, 0, 0))
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction hit-and-run line break: unit=" << _unit->getId()
						<< ", oldAction=" << (int)savedAttack.type
						<< ", oldTarget=" << savedAttack.target
						<< ", expectedDamage=" << expectedDamage
						<< ", targetHealth=" << targetHealth
						<< ", exposure=" << currentExposure
						<< ", fireLines=" << currentFireLines
						<< ", spotting=" << _spottingEnemies;
					_save->appendToAutoBattleLog(log.str());
				}
			}
			else
			{
				_attackAction = savedAttack;
			}
		}
		else if (returnFireLikely && !attackWouldFinish
			&& (((targetAlreadyWounded && safeToPostponeKill) || (overwhelmingReturnFire && thickTarget)) && weakFollowup
				|| (exposedAfterPartialHit && poorTrade && (targetAlreadyWounded || thickTarget))))
		{
			BattleAction savedAttack = _attackAction;
			const int requiredExposureGain = currentExposure >= 80 ? 35 : 55;
			const int requiredCoverGain = currentExposure >= 80 ? 8 : 12;
			if (setupFallbackCoverMove(currentExposure >= 80 ? 115 : 135, requiredExposureGain, _spottingEnemies > 0 ? 1 : 0, requiredCoverGain))
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction hit-and-run fallback: unit=" << _unit->getId()
						<< ", oldAction=" << (int)savedAttack.type
						<< ", oldTarget=" << savedAttack.target
						<< ", expectedDamage=" << expectedDamage
						<< ", targetHealth=" << targetHealth
						<< ", targetMaxHealth=" << targetMaxHealth
						<< ", spotting=" << _spottingEnemies
						<< ", visible=" << _visibleEnemies
						<< ", exposure=" << currentExposure
						<< ", fireLines=" << currentFireLines
						<< ", wounded=" << targetAlreadyWounded
						<< ", thick=" << thickTarget;
					_save->appendToAutoBattleLog(log.str());
				}
			}
			else
			{
				_attackAction = savedAttack;
			}
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER
		&& action->number == 2
		&& firedThisTurn
		&& _attackAction.type == BA_RETHINK
		&& !currentGrenadeDanger)
	{
		const int currentExposure = getEnemyFireExposure(_unit->getPosition());
		const int currentFireLines = countEnemyFireLines(_unit->getPosition());
		const bool exposedAfterShot = currentFireLines > 0
			&& (_spottingEnemies > 0 || currentExposure >= 70 || _visibleEnemies > 0);
		if (exposedAfterShot && setupFallbackCoverMove(currentExposure >= 90 ? 85 : 100, 0, 0, 0))
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction post-shot line break: unit=" << _unit->getId()
					<< ", exposure=" << currentExposure
					<< ", fireLines=" << currentFireLines
					<< ", spotting=" << _spottingEnemies
					<< ", visible=" << _visibleEnemies
					<< ", reason=no_useful_followup_attack";
				_save->appendToAutoBattleLog(log.str());
			}
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER
		&& _factionAI
		&& _attackAction.type == BA_RETHINK
		&& playerHeavyExplosiveDoctrine
		&& playerExplosiveThrowerRank >= 6
		&& (_visibleEnemies || _spottingEnemies)
		&& !currentGrenadeDanger)
	{
		Position blastTarget;
		bool hasBlastTarget = false;
		BattleUnit *assignedTarget = _factionAI->getAssignedTarget(_unit);
		if (assignedTarget && !assignedTarget->isOut() && assignedTarget->getTile() && validTarget(assignedTarget, true, true))
		{
			blastTarget = assignedTarget->getPosition();
			hasBlastTarget = true;
		}
		if (!hasBlastTarget)
		{
			hasBlastTarget = _factionAI->getBestEnemyContactPosition(&blastTarget);
		}
		int plannedBlastRadius = 0;
		for (auto *ally : *_save->getUnits())
		{
			if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction())
			{
				continue;
			}
			int explosivePower = 0;
			int explosiveRadius = 0;
			if (getPlayerBestExplosiveReserve(ally, _save, true, &explosivePower, &explosiveRadius) <= 0)
			{
				continue;
			}
			if (getPlayerExplosiveThrowerRank(ally, _save) < 6)
			{
				plannedBlastRadius = std::max(plannedBlastRadius, explosiveRadius);
			}
		}
		const Position current = _unit->getPosition();
		if (hasBlastTarget && plannedBlastRadius >= 3
			&& current.z == blastTarget.z
			&& Position::distance2d(current, blastTarget) <= plannedBlastRadius + 1)
		{
			auto coverScoreAt = [&](const Position &pos) -> int
			{
				Tile *tile = _save->getTile(pos);
				if (!tile)
				{
					return 0;
				}
				int score = 0;
				if (tile->getMapData(O_OBJECT))
				{
					score += 8;
				}
				if (tile->getMapData(O_NORTHWALL))
				{
					score += blastTarget.y < pos.y ? 14 : 5;
				}
				if (tile->getMapData(O_WESTWALL))
				{
					score += blastTarget.x < pos.x ? 14 : 5;
				}
				return score;
			};
			Position bestClearPos = current;
			int bestClearScore = -100000;
			int bestClearMoveTU = 0;
			const int currentSpotters = getSpottingUnits(current);
			const int currentExposure = getEnemyFireExposure(current);
			const int currentFireLines = countEnemyFireLines(current);
			const int currentDist = Position::distance2d(current, blastTarget);
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				const Position pos = tile->getPosition();
				if (pos == current || pos.z != current.z || isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
				{
					continue;
				}
				const int dist = Position::distance2d(pos, blastTarget);
				if (dist <= plannedBlastRadius + 1)
				{
					continue;
				}
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int moveTU = _save->getPathfinding()->getTotalTUCost();
				_save->getPathfinding()->abortPath();
				if (moveTU > std::max(0, _unit->getTimeUnits() - 12))
				{
					continue;
				}
				const int spotters = getSpottingUnits(pos);
				const int exposure = getEnemyFireExposure(pos);
				const int fireLines = countEnemyFireLines(pos);
				if (spotters > currentSpotters || fireLines > currentFireLines || exposure > currentExposure + 35)
				{
					continue;
				}
				int score = (dist - currentDist) * 45;
				score += (currentSpotters - spotters) * 85;
				score += (currentFireLines - fireLines) * 120;
				score += (currentExposure - exposure);
				score += coverScoreAt(pos) * 7;
				score -= moveTU * 3;
				if (score > bestClearScore)
				{
					bestClearScore = score;
					bestClearPos = pos;
					bestClearMoveTU = moveTU;
				}
			}
			if (bestClearPos != current && bestClearScore >= 35)
			{
				_attackAction.actor = _unit;
				_attackAction.weapon = selectBestCarriedWeapon();
				_attackAction.type = BA_WALK;
				_attackAction.target = bestClearPos;
				_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestClearPos, blastTarget);
				_AIMode = AI_COMBAT;
				_factionSupportMoveAction = true;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction blast clearance move: unit=" << _unit->getId()
						<< ", from=" << current
						<< ", target=" << bestClearPos
						<< ", blastTarget=" << blastTarget
						<< ", radius=" << plannedBlastRadius
						<< ", currentDist=" << currentDist
						<< ", newDist=" << Position::distance2d(bestClearPos, blastTarget)
						<< ", score=" << bestClearScore
						<< ", moveTU=" << bestClearMoveTU
						<< ", throwerRank=" << playerExplosiveThrowerRank
						<< ", reason=clear_planned_he_blast";
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}
	setupPatrol();
	if (_unit->getFaction() == FACTION_PLAYER && _attackAction.type == BA_RETHINK)
	{
		setupRoleWeaponPickup(action);
	}
	if (_unit->getFaction() == FACTION_PLAYER && _attackAction.type == BA_RETHINK)
	{
		setupFallbackCoverMove();
	}
	if (_unit->getFaction() == FACTION_PLAYER && heavyInitialEnemyPresence && _attackAction.type == BA_RETHINK && playerTurnMoveCount == 0)
	{
		auto deployCoverScore = [&](const Position &pos) -> int
		{
			Tile *tile = _save->getTile(pos);
			if (!tile)
			{
				return 0;
			}
			int cover = 0;
			if (tile->getMapData(O_OBJECT))
			{
				cover += 8;
			}
			if (tile->getMapData(O_NORTHWALL))
			{
				cover += 6;
			}
			if (tile->getMapData(O_WESTWALL))
			{
				cover += 6;
			}
			return cover;
		};
		const Position current = _unit->getPosition();
		const bool hasPatrolGuide = _save->getTile(_patrolAction.target) != 0;
		const bool carefulHostileDeploy = highPressureHostileFirepower
			|| (highHostileFirepower && _save->getTurn() <= 2 && _knownEnemies >= std::max(6, playerActiveAllies / 2));
		Position deploymentFacingTarget = _patrolAction.target;
		bool hasDeploymentContact = false;
		if (!hasPatrolGuide && _factionAI)
		{
			Position deploymentContact;
			if (_factionAI->getBestEnemyContactPosition(&deploymentContact))
			{
				deploymentFacingTarget = deploymentContact;
				hasDeploymentContact = true;
			}
		}
		auto deployNearbyAllies = [&](const Position &pos, int radius, bool sameLevelOnly) -> int
		{
			int allies = 0;
			for (auto *other : *_save->getUnits())
			{
				if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
				{
					continue;
				}
				if (sameLevelOnly && other->getPosition().z != pos.z)
				{
					continue;
				}
				if (Position::distance2d(pos, other->getPosition()) <= radius && std::abs(other->getPosition().z - pos.z) <= 1)
				{
					++allies;
				}
			}
			return allies;
		};
		auto deployCrowdingPenalty = [&](const Position &pos) -> int
		{
			int penalty = 0;
			for (auto *other : *_save->getUnits())
			{
				if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
				{
					continue;
				}
				const int dist = Position::distance2d(pos, other->getPosition());
				if (dist == 0)
				{
					penalty += 1000;
				}
				else if (dist == 1)
				{
					penalty += 180;
				}
				else if (dist == 2)
				{
					penalty += 85;
				}
				else if (dist == 3)
				{
					penalty += 25;
				}
			}
			return penalty;
		};
		const int currentCloseAllies = deployNearbyAllies(current, 2, true);
		const int currentBlastAllies = deployNearbyAllies(current, 4, false);
		const bool clusteredInitialDeploy = carefulHostileDeploy
			&& (playerMaxHostileHealth >= 80 || hostileAreaWeaponThreat || playerActiveHostiles >= std::max(12, playerActiveAllies))
			&& playerActiveAllies >= 10
			&& (currentCloseAllies >= 3 || currentBlastAllies >= 6);
		int reserveTU = action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM
			? std::max(22, (int)BattleActionCost(BA_SNAPSHOT, _unit, action->weapon).Time)
			: 18;
		int explosiveThrowerRank = playerExplosiveThrowerRank;
		int explosiveReserveTU = 0;
		if (heavyInitialEnemyPresence && (playerHighHpExplosiveReserve || playerSlowDangerExplosiveReserve))
		{
			explosiveReserveTU = playerExplosiveReserveTU;
			if (explosiveReserveTU > 0)
			{
				if (playerHighHpExplosiveReserve || explosiveThrowerRank < 4)
				{
					const int reserveBuffer = explosiveThrowerRank < 4 ? 6 : 0;
					reserveTU = std::max(reserveTU, std::min(_unit->getBaseStats()->tu - 8, explosiveReserveTU + reserveBuffer));
				}
			}
		}
		if (carefulHostileDeploy)
		{
			reserveTU = std::max(reserveTU, tacticalRole == TACTICAL_FIRE_SUPPORT ? 34 : 28);
		}
		const int maxDeployMoveDistance = carefulHostileDeploy
			? (tacticalRole == TACTICAL_FIRE_SUPPORT ? 2 : (tacticalRole == TACTICAL_SCOUT ? 4 : 3))
			: 4;
		const int maxDeployExposure = 60;
		Position bestPos = current;
		int bestScore = 1000000;
		int bestMoveTU = 0;
		int bestCover = deployCoverScore(current);
		int bestCrowding = deployCrowdingPenalty(current);
		int bestNearbyAllies = currentBlastAllies;
		for (auto tileIndex : _reachable)
		{
			Tile *tile = _save->getTile(tileIndex);
			if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
			{
				continue;
			}
			const Position pos = tile->getPosition();
			if (pos == current || pos.z > current.z)
			{
				continue;
			}
			if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
			{
				continue;
			}
			const int moveDist = Position::distance2d(pos, current);
			if (moveDist > maxDeployMoveDistance)
			{
				continue;
			}
			_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL, 0, std::max(0, _unit->getTimeUnits() - reserveTU));
			if (_save->getPathfinding()->getStartDirection() == -1)
			{
				_save->getPathfinding()->abortPath();
				continue;
			}
			const int moveTU = _save->getPathfinding()->getTotalTUCost();
			_save->getPathfinding()->abortPath();
			if (moveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
			{
				continue;
			}
			const int spotters = getSpottingUnits(pos);
			const int exposure = getEnemyFireExposure(pos);
			const int fireLines = countEnemyFireLines(pos);
			if (spotters > 0 || exposure > maxDeployExposure || fireLines > 0)
			{
				continue;
			}
			const int cover = deployCoverScore(pos);
			if (!clusteredInitialDeploy && carefulHostileDeploy && tacticalRole == TACTICAL_FIRE_SUPPORT && moveDist > 1 && cover < bestCover + 6)
			{
				continue;
			}
			if (!clusteredInitialDeploy && carefulHostileDeploy && tacticalRole != TACTICAL_SCOUT && pos.z < current.z && cover < 6 && moveDist > 1)
			{
				continue;
			}
			const int crowding = deployCrowdingPenalty(pos);
			const int nearbyAllies = deployNearbyAllies(pos, 4, false);
			int score = moveTU * 4 + moveDist * 6 + crowding - cover * 5;
			if (pos.z < current.z)
			{
				score -= 130;
			}
			if (hasPatrolGuide)
			{
				score += Position::distance2d(pos, _patrolAction.target) * 3;
			}
			else if (pos.z == current.z)
			{
				score += 40;
			}
			if (score < bestScore)
			{
				bestScore = score;
				bestPos = pos;
				bestMoveTU = moveTU;
				bestCover = cover;
				bestCrowding = crowding;
				bestNearbyAllies = nearbyAllies;
			}
		}
		if (bestPos != current)
		{
			_patrolAction.actor = _unit;
			_patrolAction.weapon = action->weapon;
			_patrolAction.target = bestPos;
			_patrolAction.type = BA_WALK;
			_patrolAction.finalFacing = (hasPatrolGuide || hasDeploymentContact)
				? _save->getTileEngine()->getDirectionTo(bestPos, deploymentFacingTarget)
				: _unit->getDirection();
			_attackAction = _patrolAction;
			_AIMode = AI_COMBAT;
			_factionSupportMoveAction = true;
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction initial deploy move: unit=" << _unit->getId()
					<< ", from=" << current
					<< ", target=" << bestPos
					<< ", moveTU=" << bestMoveTU
					<< ", reserveTU=" << reserveTU
					<< ", explosiveRank=" << explosiveThrowerRank
					<< ", explosiveReserveTU=" << explosiveReserveTU
					<< ", maxMoveDistance=" << maxDeployMoveDistance
					<< ", cover=" << bestCover
					<< ", crowding=" << bestCrowding
					<< ", nearbyAllies=" << bestNearbyAllies
					<< ", currentCloseAllies=" << currentCloseAllies
					<< ", currentBlastAllies=" << currentBlastAllies
					<< ", clusteredInitialDeploy=" << clusteredInitialDeploy
					<< ", score=" << bestScore
					<< ", patrolGuide=" << hasPatrolGuide
					<< ", facingContact=" << hasDeploymentContact
					<< ", facingTarget=" << deploymentFacingTarget
					<< ", activeAllies=" << playerActiveAllies
					<< ", activeHostiles=" << playerActiveHostiles
					<< ", maxHostileHealth=" << playerMaxHostileHealth
					<< ", maxHostileWeaponDanger=" << playerMaxHostileWeaponDanger
					<< ", known=" << _knownEnemies
					<< ", reason=heavy_initial_enemy_presence";
				_save->appendToAutoBattleLog(log.str());
				if (clusteredInitialDeploy && bestNearbyAllies + 2 < currentBlastAllies)
				{
					std::ostringstream clusterLog;
					clusterLog << "Player faction heavy landing cluster break: unit=" << _unit->getId()
						<< ", from=" << current
						<< ", target=" << bestPos
						<< ", currentBlastAllies=" << currentBlastAllies
						<< ", targetBlastAllies=" << bestNearbyAllies
						<< ", currentCloseAllies=" << currentCloseAllies
						<< ", cover=" << bestCover
						<< ", moveTU=" << bestMoveTU
						<< ", maxHostileBlastRadius=" << playerMaxHostileBlastRadius
						<< ", maxHostileHealth=" << playerMaxHostileHealth
						<< ", reason=reduce_initial_blast_cluster";
					_save->appendToAutoBattleLog(clusterLog.str());
				}
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && _factionAI && _attackAction.type == BA_RETHINK && (_knownEnemies || _factionAI->getEnemyContactCount() > 0))
	{
		Position contactPos;
		const BattleRoomInfo *contactRoom = 0;
		int enemiesInRoom = 1;
			bool visibleFactionContact = false;
			if (_factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
			{
			const int roomSize = contactRoom ? contactRoom->tileCount : 1;
			const int roomEntries = contactRoom ? (int)contactRoom->entryPositions.size() : 0;
			const int unitRoomId = _factionAI->getRoomIdAt(_unit->getPosition());
			const bool contactHasControlledEntry = contactRoom && (roomEntries > 0 || contactRoom->doorCount + contactRoom->windowCount > 0);
			const bool brokenRoom = contactRoom && !contactRoom->isOutside && !contactRoom->isHall
				&& (contactRoom->tileCount > 90 || (!contactHasControlledEntry && contactRoom->openingCount > contactRoom->tileCount * 2) || (roomEntries == 0 && contactRoom->doorCount + contactRoom->windowCount == 0));
			const bool smallDangerRoom = contactRoom && !contactRoom->isOutside && !contactRoom->isHall && !brokenRoom;
			const bool riskyRoom = smallDangerRoom && (enemiesInRoom > 1 || contactRoom->tileCount > 16 || contactRoom->doorCount + contactRoom->windowCount <= 2);
			const bool insideDangerRoom = riskyRoom && unitRoomId == contactRoom->id;
			const bool openAreaFight = !contactRoom || contactRoom->isOutside || contactRoom->isHall || brokenRoom;
			const bool huntingHiddenContact = !visibleFactionContact && !_visibleEnemies;
			const bool initialDeploy = factionStrategy == PFS_INITIAL_DEPLOY;
			int activeAllies = playerActiveAllies;
			int activeHostiles = playerActiveHostiles;
			const bool lateHiddenHunt = huntingHiddenContact && (_save->getTurn() >= 30 || activeHostiles <= 3);
			const bool forcedHiddenHunt = huntingHiddenContact && (_save->getTurn() >= 35 || activeHostiles <= 1);
			const bool endgameHiddenHunt = forcedHiddenHunt;
			const bool overwhelmingHiddenContact = huntingHiddenContact && !lateHiddenHunt
				&& activeHostiles >= 12
				&& activeHostiles >= activeAllies - 2
				&& _knownEnemies >= 10;
			if (action->number <= 1 && (!_spottingEnemies || insideDangerRoom) && (insideDangerRoom || (!_visibleEnemies && _attackAction.type == BA_RETHINK)) && setupFactionStalkAmbush(contactPos, contactRoom, enemiesInRoom, action->weapon))
			{
				_AIMode = AI_COMBAT;
			}
			if (_visibleEnemies && !insideDangerRoom)
			{
				goto factionRoomTacticsDone;
			}
			Position bestPos = _unit->getPosition();
			int currentDist = Position::distance2d(bestPos, contactPos);
			int currentSpotters = getSpottingUnits(bestPos);
			int currentExposure = getEnemyFireExposure(bestPos);
			int bestDist = currentDist;
			int bestSpotters = currentSpotters;
			int bestSupportMoveTU = 0;
			auto supportCoverScore = [&](const Position &pos) -> int
			{
				Tile *coverTile = _save->getTile(pos);
				if (!coverTile)
				{
					return 0;
				}
				int cover = 0;
				if (coverTile->getMapData(O_OBJECT))
				{
					cover += 8;
				}
				if (coverTile->getMapData(O_NORTHWALL))
				{
					cover += 6;
				}
				if (coverTile->getMapData(O_WESTWALL))
				{
					cover += 6;
				}
				return cover;
			};
			auto allyCrowdingPenalty = [&](const Position &pos) -> int
			{
				int penalty = 0;
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					const int dist = Position::distance2d(pos, other->getPosition());
					if (dist <= 1)
					{
						penalty += 140;
					}
					else if (dist <= 2)
					{
						penalty += 75;
					}
					else if (dist <= 3)
					{
						penalty += 30;
					}
				}
				return penalty;
			};
			auto tooCloseToAlly = [&](const Position &pos) -> bool
			{
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					if (other->getPosition().z == pos.z && Position::distance2d(pos, other->getPosition()) <= 2)
					{
						return true;
					}
				}
				return false;
			};
			auto allyFireLanePenalty = [&](const Position &pos) -> int
			{
				int penalty = 0;
				const double px = (double)pos.x;
				const double py = (double)pos.y;
				for (auto *other : *_save->getUnits())
				{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
					{
						continue;
					}
					BattleItem *otherWeapon = other->getMainHandWeapon(false);
					if (!otherWeapon || otherWeapon->getRules()->getBattleType() != BT_FIREARM)
					{
						continue;
					}
					const Position otherPos = other->getPosition();
					const double vx = (double)(contactPos.x - otherPos.x);
					const double vy = (double)(contactPos.y - otherPos.y);
					const double lenSq = vx * vx + vy * vy;
					if (lenSq < 0.1)
					{
						continue;
					}
					const double ax = px - otherPos.x;
					const double ay = py - otherPos.y;
					const double t = (ax * vx + ay * vy) / lenSq;
					if (t <= 0.0 || t >= 1.0)
					{
						continue;
					}
					const double dx = ax - vx * t;
					const double dy = ay - vy * t;
					if (dx * dx + dy * dy <= 1.0)
					{
						penalty += 120;
					}
				}
				return penalty;
			};
			const bool hasRangedWeapon = _rifle || _blaster || _grenade || !_melee;
			const bool reactionSpecialist = hasRangedWeapon
				&& _unit->getBaseStats()->reactions >= 45
				&& _unit->getBaseStats()->tu >= 45
				&& _unit->getBaseStats()->reactions + 8 >= _unit->getBaseStats()->firing
				&& playerRole != ROLE_HEAVY;
			const bool tacticalScout = tacticalRole == TACTICAL_SCOUT;
			const bool tacticalFireSupport = tacticalRole == TACTICAL_FIRE_SUPPORT;
			const bool tacticalReactionGuard = tacticalRole == TACTICAL_REACTION_GUARD;
			int desiredOpenDist = huntingHiddenContact
				? (endgameHiddenHunt ? std::max(7, preferredRange - 1) : std::max(5, preferredRange - 4))
				: (hasRangedWeapon ? preferredRange : 4);
			if (lateHiddenHunt && !tacticalFireSupport)
			{
				desiredOpenDist = std::max(4, desiredOpenDist - (forcedHiddenHunt ? 3 : 2));
			}
			if (tacticalScout && huntingHiddenContact && !endgameHiddenHunt)
			{
				desiredOpenDist = std::max(4, desiredOpenDist - 1);
			}
			else if (tacticalFireSupport && huntingHiddenContact)
			{
				desiredOpenDist = std::max(desiredOpenDist, preferredRange + (openAreaFight ? 1 : 0));
			}
			if (playerRole == ROLE_MARKSMAN && huntingHiddenContact)
			{
				desiredOpenDist = std::max(desiredOpenDist, preferredRange - 1);
			}
			else if (reactionSpecialist && contactRoom && !contactRoom->isOutside && !contactRoom->isHall)
			{
				desiredOpenDist = std::min(desiredOpenDist, enemiesInRoom > 1 ? 5 : 4);
			}
			int maxSupportMoveDistance = initialDeploy ? 4 : (endgameHiddenHunt ? (openAreaFight ? 8 : 6) : (huntingHiddenContact ? 4 : (openAreaFight && visibleFactionContact ? 4 : (openAreaFight ? 8 : 6))));
			if (lateHiddenHunt)
			{
				maxSupportMoveDistance = std::max(maxSupportMoveDistance, tacticalFireSupport ? 5 : (tacticalScout ? 10 : 8));
			}
			if (huntingHiddenContact && !endgameHiddenHunt)
			{
				if (tacticalScout)
				{
					maxSupportMoveDistance += openAreaFight ? 2 : 1;
				}
				else if (tacticalFireSupport)
				{
					maxSupportMoveDistance = std::max(2, maxSupportMoveDistance - 1);
				}
			}
			if (overwhelmingHiddenContact)
			{
				maxSupportMoveDistance = std::min(maxSupportMoveDistance, tacticalScout ? 5 : (tacticalReactionGuard ? 3 : 2));
			}
			if (highPressureHostileFirepower && huntingHiddenContact && !lateHiddenHunt)
			{
				maxSupportMoveDistance = std::min(maxSupportMoveDistance, tacticalScout ? 3 : (tacticalReactionGuard ? 2 : 2));
			}
			const bool carefulRoomContact = contactRoom
				&& !contactRoom->isOutside
				&& !contactRoom->isHall
				&& contactRoom->tileCount >= 40
				&& roomEntries <= 2;
			const bool carefulHiddenFirepower = highPressureHostileFirepower
				|| (highHostileFirepower && huntingHiddenContact && !lateHiddenHunt
					&& (overwhelmingHiddenContact || (_save->getTurn() <= 2 && carefulRoomContact)));
			int reserveMoveTU = initialDeploy ? 28 : (forcedHiddenHunt ? (tacticalFireSupport ? 14 : 8) : (endgameHiddenHunt ? 12 : (huntingHiddenContact ? (openAreaFight ? (tacticalScout ? 20 : 26) : (tacticalScout ? 18 : 22)) : (openAreaFight && visibleFactionContact ? 24 : (hasRangedWeapon ? 20 : 12)))));
			if ((playerHeavyExplosiveDoctrine || playerHighHpExplosiveReserve || playerSlowDangerExplosiveReserve)
				&& playerExplosiveReserveTU > 0
				&& playerExplosiveThrowerRank < 2)
			{
				const int explosiveReserveMoveTU = std::min(_unit->getBaseStats()->tu - 4, playerExplosiveReserveTU + (playerExplosiveThrowerRank == 0 ? 8 : 5));
				reserveMoveTU = std::max(reserveMoveTU, explosiveReserveMoveTU);
				if (huntingHiddenContact && !lateHiddenHunt)
				{
					maxSupportMoveDistance = std::min(maxSupportMoveDistance, playerExplosiveThrowerRank == 0 ? 2 : 3);
				}
			}
			int minOpenDistance = huntingHiddenContact ? (endgameHiddenHunt ? 5 : 2) : std::max(3, desiredOpenDist - (playerRole == ROLE_ASSAULT ? 4 : 5));
			if (lateHiddenHunt && !tacticalFireSupport)
			{
				minOpenDistance = std::max(2, std::min(minOpenDistance, forcedHiddenHunt ? 3 : 4));
			}
			if (initialDeploy)
			{
				minOpenDistance = std::max(minOpenDistance, hasRangedWeapon ? 5 : 3);
			}
			if (huntingHiddenContact && playerRole == ROLE_MARKSMAN)
			{
				minOpenDistance = std::max(minOpenDistance, 8);
			}
			if (tacticalFireSupport && huntingHiddenContact)
			{
				minOpenDistance = std::max(minOpenDistance, openAreaFight ? 9 : 6);
			}
			else if (tacticalScout && huntingHiddenContact && !openAreaFight)
			{
				minOpenDistance = std::max(2, std::min(minOpenDistance, 4));
			}
			else if (reactionSpecialist && contactRoom && !contactRoom->isOutside && !contactRoom->isHall)
			{
				minOpenDistance = std::min(minOpenDistance, 3);
			}
			const int maxOpenDistance = huntingHiddenContact ? desiredOpenDist + (endgameHiddenHunt ? 4 : (playerRole == ROLE_MARKSMAN ? 4 : 8)) : desiredOpenDist + (playerRole == ROLE_MARKSMAN || playerRole == ROLE_HEAVY ? 5 : 4);
			const bool crampedInitialDeployment = _save->getTurn() <= 2
				&& huntingHiddenContact
				&& openAreaFight
				&& !_visibleEnemies
				&& !_spottingEnemies
				&& activeAllies >= 10
				&& activeHostiles >= std::max(8, activeAllies - 2)
				&& currentDist > desiredOpenDist + 8;
			int bestSupportScore = openAreaFight
				? abs(bestDist - desiredOpenDist) * 6 + bestSpotters * 40 - supportCoverScore(bestPos) * 5 + allyCrowdingPenalty(bestPos) + allyFireLanePenalty(bestPos)
				: bestDist * 4 + bestSpotters * 20 - supportCoverScore(bestPos) * 3 + allyCrowdingPenalty(bestPos) + allyFireLanePenalty(bestPos);
			if (huntingHiddenContact && currentDist <= desiredOpenDist + 2 && currentSpotters == 0)
			{
				bestSupportScore -= 120;
			}
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				Position pos = tile->getPosition();
				if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
				{
					continue;
				}
				const int moveDist = Position::distance2d(pos, _unit->getPosition());
				const bool earlyDeploymentStep = _save->getTurn() <= 2 && huntingHiddenContact && !_visibleEnemies && !_spottingEnemies;
				const bool allowedEarlyLevelChange = earlyDeploymentStep && pos.z <= _unit->getPosition().z;
				if ((!allowedEarlyLevelChange && pos.z != _unit->getPosition().z) || moveDist > maxSupportMoveDistance)
				{
					continue;
				}
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int pathTU = _save->getPathfinding()->getTotalTUCost();
				const std::vector<int> supportPath = _save->getPathfinding()->copyPath();
				const int maxSupportPathSteps = maxSupportMoveDistance
					+ (pos.z != _unit->getPosition().z ? 3 : 2);
				const bool excessivelyIndirectPath = (int)supportPath.size() > maxSupportPathSteps;
				const bool explosivePathRisk = playerPathCrossesExplosiveDanger(_save, _unit,
					supportPath, BAM_NORMAL, false);
				_save->getPathfinding()->abortPath();
				if (excessivelyIndirectPath || explosivePathRisk
					|| pathTU > std::max(0, _unit->getTimeUnits() - reserveMoveTU))
				{
					continue;
				}
				if ((openAreaFight || initialDeploy) && !crampedInitialDeployment && tooCloseToAlly(pos))
				{
					continue;
				}
				if (riskyRoom && contactRoom && _factionAI->getRoomIdAt(pos) == contactRoom->id)
				{
					continue;
				}
				int spotters = getSpottingUnits(pos);
				int exposure = getEnemyFireExposure(pos);
				const int fireLines = countEnemyFireLines(pos);
				if (!huntingHiddenContact && spotters > bestSpotters)
				{
					continue;
				}
				if (exposure > currentExposure + 60 && exposure > 90)
				{
					continue;
				}
				if (huntingHiddenContact && !tacticalScout && fireLines > 0 && exposure >= currentExposure)
				{
					continue;
				}
				int dist = Position::distance2d(pos, contactPos);
				if (openAreaFight && hasRangedWeapon && dist < minOpenDistance)
				{
					continue;
				}
				const bool tooFarForPreferredOpenRange = openAreaFight && hasRangedWeapon && dist > maxOpenDistance;
				const bool acceptableFarStagingStep = tooFarForPreferredOpenRange && huntingHiddenContact && currentDist > maxOpenDistance + 4 && dist < currentDist;
				if (tooFarForPreferredOpenRange && !acceptableFarStagingStep)
				{
					continue;
				}
				if (endgameHiddenHunt && !forcedHiddenHunt && activeHostiles > 2 && supportCoverScore(pos) < supportCoverScore(bestPos))
				{
					continue;
				}
				if (endgameHiddenHunt && !forcedHiddenHunt && activeHostiles > 2 && dist < currentDist - 2)
				{
					continue;
				}
				if (!huntingHiddenContact && dist > currentDist + (spotters < currentSpotters ? 8 : 4))
				{
					continue;
				}
				const int candidateCover = supportCoverScore(pos);
				if (openAreaFight && visibleFactionContact && candidateCover < 8 && moveDist > 1)
				{
					continue;
				}
				if (carefulHiddenFirepower && !tacticalScout && candidateCover < 6 && moveDist > 1)
				{
					continue;
				}
				int supportScore = openAreaFight
					? abs(dist - desiredOpenDist) * 6 + spotters * 40 + pathTU / 2 - candidateCover * 5 + allyCrowdingPenalty(pos) + allyFireLanePenalty(pos)
					: dist * 4 + spotters * 20 + pathTU / 3 - candidateCover * 3 + allyCrowdingPenalty(pos) + allyFireLanePenalty(pos);
				if (carefulHiddenFirepower && !tacticalScout && candidateCover < 10)
				{
					supportScore += (10 - candidateCover) * (tacticalFireSupport ? 18 : 12);
				}
				supportScore += exposure;
				supportScore += fireLines * (tacticalScout ? 35 : 90);
				if (huntingHiddenContact)
				{
					const int advanceBonus = forcedHiddenHunt ? (tacticalFireSupport ? 8 : 24) : (lateHiddenHunt ? (tacticalScout ? 22 : (tacticalFireSupport ? 6 : 16)) : (tacticalScout ? 16 : (tacticalFireSupport ? 5 : 10)));
					supportScore -= std::max(0, currentDist - dist) * advanceBonus;
					supportScore += spotters * 20;
					if (tacticalReactionGuard && contactRoom && !contactRoom->isOutside && !contactRoom->isHall)
					{
						supportScore -= candidateCover * 2;
						supportScore += abs(dist - std::max(3, desiredOpenDist - 1)) * 3;
					}
					if (tacticalFireSupport)
					{
						supportScore += std::max(0, desiredOpenDist - dist) * 12;
					}
				}
				if (supportScore < bestSupportScore)
				{
					bestSpotters = spotters;
					bestDist = dist;
					bestSupportScore = supportScore;
					bestPos = pos;
					bestSupportMoveTU = pathTU;
				}
			}
			const int requiredSupportImprovement = forcedHiddenHunt ? 2 : (lateHiddenHunt ? 6 : (huntingHiddenContact ? 12 : (visibleFactionContact ? 20 : 16)));
			if (bestPos != _unit->getPosition() && bestSupportScore + requiredSupportImprovement < (openAreaFight
				? abs(currentDist - desiredOpenDist) * 6 + currentSpotters * 40 - supportCoverScore(_unit->getPosition()) * 5 + allyCrowdingPenalty(_unit->getPosition()) + allyFireLanePenalty(_unit->getPosition()) + currentExposure
				: currentDist * 4 + currentSpotters * 20 - supportCoverScore(_unit->getPosition()) * 3 + allyCrowdingPenalty(_unit->getPosition()) + allyFireLanePenalty(_unit->getPosition()) + currentExposure))
			{
				_patrolAction.actor = _unit;
				_patrolAction.weapon = action->weapon;
				_patrolAction.target = bestPos;
				_patrolAction.type = BA_WALK;
				_patrolAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, contactPos);
				_attackAction = _patrolAction;
				_AIMode = AI_COMBAT;
				_factionSupportMoveAction = true;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction support move: unit=" << _unit->getId()
						<< ", contact=" << contactPos
						<< ", contactRoom=" << (contactRoom ? contactRoom->id : -1)
						<< ", roomEnemies=" << enemiesInRoom
						<< ", roomSize=" << roomSize
						<< ", roomDoors=" << (contactRoom ? contactRoom->doorCount : 0)
						<< ", roomWindows=" << (contactRoom ? contactRoom->windowCount : 0)
						<< ", roomEntries=" << roomEntries
						<< ", roomOutside=" << (contactRoom ? contactRoom->isOutside : false)
						<< ", roomHall=" << (contactRoom ? contactRoom->isHall : false)
						<< ", brokenRoom=" << brokenRoom
						<< ", avoidedRoom=" << riskyRoom
						<< ", insideDangerRoom=" << insideDangerRoom
						<< ", openAreaFight=" << openAreaFight
						<< ", visibleFactionContact=" << visibleFactionContact
						<< ", huntingHiddenContact=" << huntingHiddenContact
						<< ", lateHiddenHunt=" << lateHiddenHunt
						<< ", forcedHiddenHunt=" << forcedHiddenHunt
						<< ", overwhelmingHiddenContact=" << overwhelmingHiddenContact
						<< ", highHostileFirepower=" << highHostileFirepower
						<< ", highPressureHostileFirepower=" << highPressureHostileFirepower
						<< ", maxHostileWeaponDanger=" << playerMaxHostileWeaponDanger
						<< ", initialDeploy=" << initialDeploy
						<< ", crampedInitialDeployment=" << crampedInitialDeployment
						<< ", endgameHiddenHunt=" << endgameHiddenHunt
						<< ", activeAllies=" << activeAllies
						<< ", activeHostiles=" << activeHostiles
						<< ", role=" << (int)playerRole
						<< ", tactical=" << getPlayerTacticalRoleName(tacticalRole)
						<< ", reactionSpecialist=" << reactionSpecialist
						<< ", desiredOpenDistance=" << desiredOpenDist
						<< ", minOpenDistance=" << minOpenDistance
						<< ", maxOpenDistance=" << maxOpenDistance
						<< ", target=" << bestPos
						<< ", distance=" << bestDist
						<< ", moveDistance=" << Position::distance2d(bestPos, _unit->getPosition())
						<< ", moveTU=" << bestSupportMoveTU
						<< ", maxMoveDistance=" << maxSupportMoveDistance
						<< ", reserveMoveTU=" << reserveMoveTU
						<< ", explosiveReserveTU=" << playerExplosiveReserveTU
						<< ", explosiveThrowerRank=" << playerExplosiveThrowerRank
						<< ", spotters=" << bestSpotters;
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}
factionRoomTacticsDone:

	if (_psiAction.type != BA_NONE && !_didPsi && _save->getTurn() >= _psiAction.weapon->getRules()->getAIUseDelay(_save->getMod()))
	{
		_didPsi = true;
		action->type = _psiAction.type;
		action->target = _psiAction.target;
		action->number -= 1;
		action->weapon = _psiAction.weapon;
		action->updateTU();
		return;
	}
	else
	{
		_didPsi = false;
	}

	bool evaluate = false;

	switch (_AIMode)
		{
		case AI_PATROL:
			evaluate = (bool)(_spottingEnemies || _visibleEnemies || _knownEnemies || RNG::percent(10));
			break;
		case AI_AMBUSH:
			evaluate = (!_rifle || !_ambushTUs || _visibleEnemies);
			break;
		case AI_COMBAT:
			evaluate = (_attackAction.type == BA_RETHINK);
			break;
		case AI_ESCAPE:
			evaluate = (!_spottingEnemies || !_knownEnemies);
			break;
			}

	if (_weaponPickedUp)
	{
		evaluate = true;
		_weaponPickedUp = false;
	}
	else if (_spottingEnemies > 2
		|| _unit->getHealth() < 2 * _unit->getBaseStats()->health / 3)
	{
		evaluate = true;
	}
	if (_save->isCheating() && _AIMode != AI_COMBAT)
	{
		evaluate = true;
	}
	if (_crossLevelRouteMoveAction && (_visibleEnemies > 0 || _spottingEnemies > 0))
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction cross-level route interrupted by contact: unit=" << _unit->getId()
				<< ", position=" << _unit->getPosition()
				<< ", routeTarget=" << _attackAction.target
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", reason=replan_against_immediate_contact_before_hidden_route";
			_save->appendToAutoBattleLog(log.str());
		}
		_crossLevelRouteMoveAction = false;
		_cleanShotMoveAction = false;
		_attackAction.type = BA_RETHINK;
		evaluate = true;
	}
	// These are completed tactical decisions, not stale modes.  In particular,
	// late-hunt cheating must not turn a deliberate reaction hold back into an
	// immediate low-value shot, nor replace a verified cover break with Escape.
	if (deliberateReactionHold || _fallbackCoverAction || _crossLevelRouteMoveAction)
	{
		evaluate = false;
	}

	if (evaluate)
	{
		evaluateAIMode();
		if (_traceAI)
		{
			std::string AIMode;
			switch (_AIMode)
			{
			case AI_PATROL:
				AIMode = "Patrol";
				break;
			case AI_AMBUSH:
				AIMode = "Ambush";
				break;
			case AI_COMBAT:
				AIMode = "Combat";
				break;
			case AI_ESCAPE:
				AIMode = "Escape";
				break;
			}
			Log(LOG_INFO) << "Re-Evaluated, now using " << AIMode << " behaviour";
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER && currentGrenadeDanger)
	{
		if (_escapeAction.type != BA_WALK
			|| _escapeAction.target == _unit->getPosition()
			|| isPlayerExplosiveDanger(_save, _unit->getFaction(), _escapeAction.target))
		{
			setupEscape();
		}
		const bool validEscape = _escapeAction.type == BA_WALK
			&& _escapeAction.target != _unit->getPosition()
			&& !isPlayerExplosiveDanger(_save, _unit->getFaction(), _escapeAction.target);
		const bool usefulLastAction = _attackAction.type == BA_AIMEDSHOT
			|| _attackAction.type == BA_AUTOSHOT
			|| _attackAction.type == BA_SNAPSHOT
			|| _attackAction.type == BA_HIT
			|| _attackAction.type == BA_LAUNCH
			|| _attackAction.type == BA_PANIC
			|| _attackAction.type == BA_MINDCONTROL;
		if (validEscape)
		{
			_AIMode = AI_ESCAPE;
		}
		else if (usefulLastAction)
		{
			_AIMode = AI_COMBAT;
		}
		else
		{
			_escapeAction.type = BA_NONE;
			_escapeAction.target = _unit->getPosition();
			_escapeTUs = 1;
			_AIMode = AI_ESCAPE;
		}
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction explosive evacuation lock: unit=" << _unit->getId()
				<< ", from=" << _unit->getPosition()
				<< ", target=" << _escapeAction.target
				<< ", action=" << (int)_escapeAction.type
				<< ", escapeTUs=" << _escapeTUs
				<< ", validEscape=" << validEscape
				<< ", usefulLastAction=" << usefulLastAction
				<< ", selectedMode=" << _AIMode
				<< ", reason=explosive_danger_overrides_combat_patrol_and_pickup";
			_save->appendToAutoBattleLog(log.str());
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER
		&& !currentGrenadeDanger
		&& (_visibleEnemies > 0 || _spottingEnemies > 0)
		&& _attackAction.type == BA_RETHINK)
	{
		const int currentExposure = getEnemyFireExposure(_unit->getPosition());
		const int currentFireLines = countEnemyFireLines(_unit->getPosition());
		const int emergencyScore = (currentFireLines > 0 || _spottingEnemies > 0 || currentExposure >= 80) ? 12 : 28;
		if (setupFallbackCoverMove(emergencyScore, 0, 0, 0) && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction emergency contact fallback: unit=" << _unit->getId()
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", exposure=" << currentExposure
				<< ", fireLines=" << currentFireLines
				<< ", target=" << _attackAction.target
				<< ", reason=avoid_idle_under_contact";
			_save->appendToAutoBattleLog(log.str());
		}
	}

	const bool evacuatingGrenadeDanger = currentGrenadeDanger;
	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger
		&& !deliberateReactionHold && _visibleEnemies
		&& _attackAction.type != BA_RETHINK && _attackAction.type != BA_WALK)
	{
		if (_AIMode == AI_ESCAPE && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction override: unit=" << _unit->getId()
				<< " switches Escape to Combat because a visible enemy can be attacked.";
			_save->appendToAutoBattleLog(log.str());
		}
		_AIMode = AI_COMBAT;
	}
	if (_unit->getFaction() == FACTION_PLAYER
		&& !evacuatingGrenadeDanger
		&& _AIMode == AI_ESCAPE
		&& _save->getTurn() <= PLAYER_AI_SMOKE_INITIAL_TURN_LIMIT
		&& hasRecentPlayerSmokePlanNear(_save, _unit->getFaction(), _unit->getPosition(), PLAYER_AI_SMOKE_INITIAL_COVER_MEMORY_DISTANCE))
	{
		const bool uselessEscape = _escapeAction.type == BA_RETHINK
			|| _escapeAction.type == BA_NONE
			|| (_escapeAction.type == BA_WALK && _escapeAction.target == _unit->getPosition());
		if (uselessEscape && setupSmokeExitMove())
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction override: unit=" << _unit->getId()
					<< " exits invalid Escape through starting smoke.";
				_save->appendToAutoBattleLog(log.str());
			}
		}
	}
	_reserve = BA_NONE;

	switch (_AIMode)
	{
	case AI_ESCAPE:
		_unit->setCharging(0);
		action->type = _escapeAction.type;
		action->target = _escapeAction.target;
		// end this unit's turn.
		action->finalAction = true;
		// ignore new targets.
		action->desperate = true;
		// if armor allow runing then run way from there.
		action->run = _escapeAction.run;
		// spin 180 at the end of your route.
		_unit->setHiding(true);
		break;
	case AI_PATROL:
		_unit->setCharging(0);
		if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			if (_unit->getFaction() == FACTION_PLAYER)
			{
				_reserve = BA_SNAPSHOT;
			}
			else switch (_unit->getAggression())
			{
			case 0:
				_reserve = BA_AIMEDSHOT;
				break;
			case 1:
				_reserve = BA_AUTOSHOT;
				break;
			case 2:
				_reserve = BA_SNAPSHOT;
				break;
			default:
				break;
			}
		}
		action->type = _patrolAction.type;
		action->target = _patrolAction.target;
		if (_unit->getFaction() == FACTION_PLAYER && _knownEnemies == 0 && action->type == BA_WALK)
		{
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		break;
	case AI_COMBAT:
		action->type = _attackAction.type;
		action->target = _attackAction.target;
		// this may have changed to a grenade.
		action->weapon = _attackAction.weapon;
		// if this is a firepoint action, set our facing.
		action->finalFacing = _attackAction.finalFacing;
		action->updateTU();
		if (action->weapon && action->type == BA_THROW && action->weapon->getRules()->isGrenadeOrProxy())
		{
			const RuleItemUseCost primeCost = _unit->getActionTUs(BA_PRIME, action->weapon);
			const int throwTU = action->Time;
			const int prepTU = primeCost.Time + 4;
			const bool enoughTU = _unit->getTimeUnits() >= throwTU + prepTU;
			const bool enoughEnergy = _unit->getEnergy() >= action->Energy + primeCost.Energy;
			Tile *throwTargetTile = _save->getTile(action->target);
			bool validThrow = false;
			if (throwTargetTile)
			{
				Position originVoxel = _save->getTileEngine()->getOriginVoxel(*action, 0);
				Position targetVoxel = action->target.toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY,
					PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE - throwTargetTile->getTerrainLevel());
				validThrow = _save->getTileEngine()->validateThrow(*action, originVoxel, targetVoxel, _save->getDepth());
			}
			if (enoughTU && enoughEnergy && validThrow)
			{
				_unit->spendCost(primeCost);
				_unit->spendTimeUnits(4);
				action->weapon->setFuseTimer(action->weapon->getRules()->getFuseTimerDefault());
			}
			else
			{
				clearFailedPendingPlayerGrenadeDanger(_save, action->weapon);
				if (action->weapon->getFuseTimer() >= 0)
				{
					action->weapon->setFuseTimer(-1);
				}
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction explosive action rejected before throw: unit=" << _unit->getId()
						<< ", item=" << action->weapon->getRules()->getType()
						<< ", target=" << action->target
						<< ", throwTU=" << throwTU
						<< ", prepTU=" << prepTU
						<< ", currentTU=" << _unit->getTimeUnits()
						<< ", throwEnergy=" << action->Energy
						<< ", prepEnergy=" << primeCost.Energy
						<< ", currentEnergy=" << _unit->getEnergy()
						<< ", validThrow=" << validThrow
						<< ", reason=" << (!validThrow ? "invalid_final_throw_path" : "not_enough_tu_after_prime");
					_save->appendToAutoBattleLog(log.str());
				}
				action->type = BA_NONE;
				action->target = _unit->getPosition();
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
			}
		}
		// if this is a "find fire point" action, don't increment the AI counter.
		if (action->type == BA_WALK
			&& !_stalkAmbushAction && !_cleanShotMoveAction && !_fallbackCoverAction && !_factionSupportMoveAction
			&& _rifle && _unit->getArmor()->allowsMoving()
			// so long as we can take a shot afterwards.
			&& BattleActionCost(BA_SNAPSHOT, _unit, action->weapon).haveTU())
		{
			action->number -= 1;
		}
		else if (action->type == BA_WALK && (_stalkAmbushAction || _cleanShotMoveAction || _fallbackCoverAction || _factionSupportMoveAction))
		{
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		else if (action->type == BA_LAUNCH)
		{
			action->waypoints = _attackAction.waypoints;
		}
		else if (action->type == BA_AIMEDSHOT || action->type == BA_AUTOSHOT || action->type == BA_SNAPSHOT)
		{
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		break;
	case AI_AMBUSH:
		_unit->setCharging(0);
		action->type = _ambushAction.type;
		action->target = _ambushAction.target;
		// face where we think our target will appear.
		action->finalFacing = _ambushAction.finalFacing;
		// end this unit's turn.
		action->finalAction = true;
		action->kneel = _unit->getArmor()->allowsKneeling(false);
		break;
	default:
		break;
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger
		&& action->type == BA_NONE && hostileAreaWeaponThreat
		&& _AIMode != AI_AMBUSH
		&& _unit->getArmor()->allowsMoving())
	{
		auto localBlastNeighbours = [&](const Position &pos) -> int
		{
			int count = 0;
			for (auto *ally : *_save->getUnits())
			{
				if (!ally || ally == _unit || ally->isOut() || ally->getFaction() != _unit->getFaction())
				{
					continue;
				}
				if (abs(ally->getPosition().z - pos.z) <= Options::battleExplosionHeight
					&& Position::distance2d(ally->getPosition(), pos) <= playerLocalHostileBlastRadius)
				{
					++count;
				}
			}
			return count;
		};
		const int currentNeighbours = localBlastNeighbours(_unit->getPosition());
		if (currentNeighbours >= 2)
		{
			const int currentSpotters = getSpottingUnits(_unit->getPosition());
			const int currentExposure = getEnemyFireExposure(_unit->getPosition());
			const int currentFireLines = countEnemyFireLines(_unit->getPosition());
			auto dispersionPathCrossesNewReactionLane = [&](const std::vector<int> &path, const Position &target) -> bool
			{
				Position pathPos = _unit->getPosition();
				int previousSpotters = currentSpotters;
				int previousExposure = currentExposure;
				int previousFireLines = currentFireLines;
				const bool startedSafe = previousSpotters == 0 && previousExposure == 0 && previousFireLines == 0;
				for (auto direction = path.rbegin(); direction != path.rend(); ++direction)
				{
					const PathfindingStep step = _save->getPathfinding()->getTUCost(pathPos, *direction, _unit, 0, BAM_NORMAL);
					if (step.cost.time == Pathfinding::INVALID_MOVE_COST)
					{
						return true;
					}
					const int stepSpotters = getSpottingUnits(step.pos);
					const int stepExposure = getEnemyFireExposure(step.pos);
					const int stepFireLines = countEnemyFireLines(step.pos);
					const bool finalPathStep = step.pos == target;
					const bool entersReactionLane = startedSafe
						? (stepFireLines > 0 || (finalPathStep && (stepSpotters > 0 || stepExposure > 0)))
						: (stepFireLines > previousFireLines
							|| (finalPathStep && (stepSpotters > previousSpotters || stepExposure > previousExposure + 25)));
					if (entersReactionLane)
					{
						return true;
					}
					pathPos = step.pos;
					previousSpotters = stepSpotters;
					previousExposure = stepExposure;
					previousFireLines = stepFireLines;
				}
				return false;
			};
			int reserveTU = 12;
			if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
			{
				BattleActionCost reactionCost(BA_SNAPSHOT, _unit, action->weapon);
				reserveTU = std::max(reserveTU, (int)reactionCost.Time);
			}
			Position bestDispersion = _unit->getPosition();
			int bestDispersionScore = PLAYER_AI_REJECT_SCORE;
			int bestDispersionNeighbours = currentNeighbours;
			int bestDispersionTU = 0;
			struct DispersionCandidate
			{
				Position pos;
				int score;
				int neighbours;
				int moveTU;
				std::vector<int> path;
			};
			std::vector<DispersionCandidate> dispersionCandidates;
			for (int tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getPosition() == _unit->getPosition() || tile->getDangerous()
					|| (tile->getUnit() && tile->getUnit() != _unit)
					|| tile->getPosition().z != _unit->getPosition().z
					|| Position::distance2d(tile->getPosition(), _unit->getPosition()) > 4
					|| isPlayerExplosiveDanger(_save, _unit->getFaction(), tile->getPosition()))
				{
					continue;
				}
				const int neighbours = localBlastNeighbours(tile->getPosition());
				if (neighbours >= currentNeighbours)
				{
					continue;
				}
				const int spotters = getSpottingUnits(tile->getPosition());
				const int exposure = getEnemyFireExposure(tile->getPosition());
				const int fireLines = countEnemyFireLines(tile->getPosition());
				if (spotters > currentSpotters || fireLines > currentFireLines || exposure > currentExposure + 25)
				{
					continue;
				}
				_save->getPathfinding()->calculate(_unit, tile->getPosition(), BAM_NORMAL);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int moveTU = _save->getPathfinding()->getTotalTUCost();
				const std::vector<int> dispersionPath = _save->getPathfinding()->copyPath();
				const bool pathRisk = playerPathCrossesExplosiveDanger(_save, _unit, dispersionPath, BAM_NORMAL, false);
				_save->getPathfinding()->abortPath();
				if (pathRisk || dispersionPath.size() > 5 || moveTU > 16
					|| moveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
				{
					continue;
				}
				int cover = 0;
				cover += tile->getMapData(O_OBJECT) ? 8 : 0;
				cover += tile->getMapData(O_NORTHWALL) ? 6 : 0;
				cover += tile->getMapData(O_WESTWALL) ? 6 : 0;
				const int score = (currentNeighbours - neighbours) * 400 + cover * 18
					- moveTU * 6 - spotters * 80 - fireLines * 120 - exposure;
				DispersionCandidate candidate = { tile->getPosition(), score, neighbours, moveTU, dispersionPath };
				dispersionCandidates.push_back(candidate);
			}
			std::sort(dispersionCandidates.begin(), dispersionCandidates.end(), [](const DispersionCandidate &left, const DispersionCandidate &right)
			{
				return left.score > right.score;
			});
			// Endpoint exposure is cheap enough to check for every candidate.  Full
			// reaction-lane ray tests are not, so validate only the best alternatives.
			const size_t dispersionProbeLimit = std::min<size_t>(6, dispersionCandidates.size());
			for (size_t i = 0; i < dispersionProbeLimit; ++i)
			{
				const DispersionCandidate &candidate = dispersionCandidates[i];
				if (!dispersionPathCrossesNewReactionLane(candidate.path, candidate.pos))
				{
					bestDispersionScore = candidate.score;
					bestDispersion = candidate.pos;
					bestDispersionNeighbours = candidate.neighbours;
					bestDispersionTU = candidate.moveTU;
					break;
				}
			}
			if (bestDispersion != _unit->getPosition())
			{
				action->type = BA_WALK;
				action->target = bestDispersion;
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction active blast dispersion: unit=" << _unit->getId()
						<< ", from=" << _unit->getPosition()
						<< ", target=" << bestDispersion
						<< ", neighbours=" << currentNeighbours << "->" << bestDispersionNeighbours
						<< ", radius=" << playerLocalHostileBlastRadius
						<< ", moveTU=" << bestDispersionTU
						<< ", reason=break_existing_aoe_cluster_instead_of_idle";
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && action->type == BA_WALK)
	{
		Position safeTarget = _unit->getPosition();
		Position hazardTarget = action->target;
		if (playerMoveCrossesExplosiveDanger(_save, _unit, action->target, action->getMoveType(), evacuatingGrenadeDanger, &safeTarget, &hazardTarget))
		{
			const Position originalTarget = action->target;
			if (!evacuatingGrenadeDanger && safeTarget != _unit->getPosition())
			{
				action->target = safeTarget;
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
			}
			else
			{
				action->type = BA_NONE;
				action->target = _unit->getPosition();
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
			}
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction explosive path clipped: unit=" << _unit->getId()
					<< ", from=" << _unit->getPosition()
					<< ", originalTarget=" << originalTarget
					<< ", safeTarget=" << safeTarget
					<< ", hazardTarget=" << hazardTarget
					<< ", evacuating=" << evacuatingGrenadeDanger
					<< ", resultAction=" << (int)action->type
					<< ", reason=path_crosses_pending_blast_or_active_mine";
				_save->appendToAutoBattleLog(log.str());
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger
		&& action->type == BA_WALK && _knownEnemies && highHostileFirepower && !_fallbackCoverAction)
	{
		_save->getPathfinding()->calculate(_unit, action->target, action->getMoveType());
		if (_save->getPathfinding()->getStartDirection() != -1)
		{
			const std::vector<int> path = _save->getPathfinding()->copyPath();
			Position pathPos = _unit->getPosition();
			Position lastSafe = pathPos;
			Position previousSafe = pathPos;
			int previousSpotters = getSpottingUnits(pathPos);
			int previousExposure = getEnemyFireExposure(pathPos);
			int previousFireLines = countEnemyFireLines(pathPos);
			const bool startedSafe = previousSpotters == 0 && previousExposure == 0 && previousFireLines == 0;
			const bool designatedPointProbe = startedSafe && !_visibleEnemies && !_spottingEnemies
				&& dynamicGroup.role == PDGR_POINT
				&& (dynamicGroup.maneuverGroup || tacticalRole == TACTICAL_SCOUT);
			const int pointProbeMaxHealth = std::max(1, (int)_unit->getBaseStats()->health);
			const bool smallGuerrillaEndgameHunt = factionStrategy == PFS_SKIRMISH
				&& playerActiveAllies <= 2 && _save->getTurn() >= 24;
			const bool validProbeTarget = _aggroTarget && !_aggroTarget->isOut()
				&& _aggroTarget->getFaction() == FACTION_HOSTILE;
			const bool crossLevelPointProbe = validProbeTarget
				&& playerActiveHostiles <= 2 && _crossLevelRouteMoveAction
				&& _aggroTarget->getPosition().z != _unit->getPosition().z;
			const bool sameLevelLastEnemyPointProbe = validProbeTarget
				&& playerActiveHostiles == 1
				&& _aggroTarget->getPosition().z == _unit->getPosition().z
				&& Position::distance2d(action->target, _aggroTarget->getPosition()) + 2
					<= Position::distance2d(_unit->getPosition(), _aggroTarget->getPosition());
			const bool controlledEndgamePointProbe = designatedPointProbe
				&& (factionStrategy == PFS_HUNT_LAST_ENEMY || smallGuerrillaEndgameHunt)
				&& playerTurnMoveCount == 0
				&& _unit->getFatalWounds() == 0
				&& _unit->getHealth() * 4 >= pointProbeMaxHealth * 3
				&& (crossLevelPointProbe || sameLevelLastEnemyPointProbe);
			bool pointProbeUsed = false;
			bool clipped = false;
			Position firstDanger = pathPos;
			for (auto direction = path.rbegin(); direction != path.rend(); ++direction)
			{
				const PathfindingStep step = _save->getPathfinding()->getTUCost(pathPos, *direction, _unit, 0, action->getMoveType());
				if (step.cost.time == Pathfinding::INVALID_MOVE_COST)
				{
					clipped = true;
					firstDanger = pathPos;
					break;
				}
				const int stepSpotters = getSpottingUnits(step.pos);
				const int stepExposure = getEnemyFireExposure(step.pos);
				const int stepFireLines = countEnemyFireLines(step.pos);
				const bool finalPathStep = step.pos == action->target;
				// FireLines includes current enemy TU and therefore measures reaction
				// danger on an intermediate step.  Spotting/exposure without a ready
				// shot matters at the endpoint, but must not forbid a hit-and-run dash
				// through a lane after the enemy has spent its TU.
				const bool entersReactionLane = startedSafe
					? (stepFireLines > 0 || (finalPathStep && (stepSpotters > 0 || stepExposure > 0)))
					: (stepFireLines > previousFireLines
						|| (finalPathStep && (stepSpotters > previousSpotters || stepExposure > previousExposure + 25)));
				if (entersReactionLane)
				{
					const bool firstStepProbe = lastSafe == _unit->getPosition();
					const bool routeProbeAfterLevelTransition = controlledEndgamePointProbe
						&& crossLevelPointProbe && lastSafe != _unit->getPosition()
						&& lastSafe.z != previousSafe.z;
					if (designatedPointProbe && !pointProbeUsed
						&& (firstStepProbe || routeProbeAfterLevelTransition)
						&& (stepFireLines == 0 || (controlledEndgamePointProbe && stepFireLines == 1)))
					{
						// One designated scout may expose exactly one new tile so a hidden
						// reaction lane can be discovered.  During a cross-level endgame hunt,
						// a healthy point soldier may also test exactly one ready line; without
						// that bounded breach the whole squad can deadlock below a ramp or lift.
						pathPos = step.pos;
						lastSafe = pathPos;
						pointProbeUsed = true;
						// A probe must force an immediate re-plan even if this first
						// exposed tile was also the nominal movement endpoint.
						clipped = true;
						firstDanger = pathPos;
						break;
					}
					// The first tile after a level transition can look safe when tested
					// at tile-centre height even though the standing voxel on the ramp is
					// already exposed to the line detected on the following tile.
					if (lastSafe != _unit->getPosition() && lastSafe.z != previousSafe.z)
					{
						lastSafe = previousSafe;
					}
					clipped = true;
					firstDanger = step.pos;
					break;
				}
				pathPos = step.pos;
				previousSafe = lastSafe;
				lastSafe = pathPos;
				previousSpotters = stepSpotters;
				previousExposure = stepExposure;
				previousFireLines = stepFireLines;
			}
			if (clipped)
			{
				const Position originalTarget = action->target;
				if (lastSafe == _unit->getPosition())
				{
					action->type = BA_NONE;
					action->target = _unit->getPosition();
				}
				else
				{
					action->target = lastSafe;
				}
				action->finalAction = !pointProbeUsed;
				action->kneel = !pointProbeUsed && _unit->getArmor()->allowsKneeling(false);
				if (pointProbeUsed)
				{
					_controlledProbeMoveAction = true;
				}
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction reaction path clipped: unit=" << _unit->getId()
						<< ", from=" << _unit->getPosition()
						<< ", originalTarget=" << originalTarget
						<< ", safeTarget=" << lastSafe
						<< ", firstDanger=" << firstDanger
						<< ", pointProbe=" << pointProbeUsed
						<< ", reason=do_not_cross_new_heavy_reaction_lane";
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
		_save->getPathfinding()->abortPath();
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger
		&& action->type == BA_WALK && hostileAreaWeaponThreat && !_fallbackCoverAction
		&& !_controlledProbeMoveAction)
	{
		auto blastNeighbours = [&](const Position &pos) -> int
		{
			int count = 0;
			for (auto *ally : *_save->getUnits())
			{
				if (!ally || ally == _unit || ally->isOut() || ally->getFaction() != _unit->getFaction())
				{
					continue;
				}
				if (abs(ally->getPosition().z - pos.z) <= Options::battleExplosionHeight
					&& Position::distance2d(ally->getPosition(), pos) <= playerLocalHostileBlastRadius)
				{
					++count;
				}
			}
			return count;
		};
		const int currentBlastNeighbours = blastNeighbours(_unit->getPosition());
		const int targetBlastNeighbours = blastNeighbours(action->target);
		const bool initialDeploymentTransit = _save->getTurn() <= 2
			&& (factionStrategy == PFS_INITIAL_DEPLOY || heavyInitialEnemyPresence)
			&& Position::distance2d(_unit->getPosition(), action->target) <= 2
			&& targetBlastNeighbours <= currentBlastNeighbours + 1;
		if (targetBlastNeighbours > std::max(1, currentBlastNeighbours) && !initialDeploymentTransit)
		{
			const int currentSpotters = getSpottingUnits(_unit->getPosition());
			const int targetSpotters = getSpottingUnits(action->target);
			const int currentExposure = getEnemyFireExposure(_unit->getPosition());
			const int targetExposure = getEnemyFireExposure(action->target);
			const int currentFireLines = countEnemyFireLines(_unit->getPosition());
			const int targetFireLines = countEnemyFireLines(action->target);
			const bool improvesImmediateFireSafety = targetFireLines < currentFireLines
				|| (targetFireLines <= currentFireLines && targetSpotters < currentSpotters)
				|| (targetFireLines <= currentFireLines && targetSpotters <= currentSpotters
					&& targetExposure + 25 < currentExposure);
			const bool controlledRouteConvergence = _crossLevelRouteMoveAction
				&& targetBlastNeighbours <= currentBlastNeighbours + 2
				&& targetSpotters <= currentSpotters
				&& targetFireLines <= currentFireLines
				&& targetExposure <= currentExposure + 25;
			if (!improvesImmediateFireSafety && !controlledRouteConvergence)
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction hostile blast spacing hold: unit=" << _unit->getId()
						<< ", rejectedTarget=" << action->target
						<< ", radius=" << playerLocalHostileBlastRadius
						<< ", neighbours=" << currentBlastNeighbours << "->" << targetBlastNeighbours
						<< ", spotters=" << currentSpotters << "->" << targetSpotters
						<< ", exposure=" << currentExposure << "->" << targetExposure
						<< ", fireLines=" << currentFireLines << "->" << targetFireLines
						<< ", reason=do_not_form_new_multi_kill_grenade_cluster";
					_save->appendToAutoBattleLog(log.str());
				}
				action->type = BA_NONE;
				action->target = _unit->getPosition();
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
			}
			else if (controlledRouteConvergence && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction cross-level route spacing convergence allowed: unit=" << _unit->getId()
					<< ", target=" << action->target
					<< ", neighbours=" << currentBlastNeighbours << "->" << targetBlastNeighbours
					<< ", spotters=" << currentSpotters << "->" << targetSpotters
					<< ", exposure=" << currentExposure << "->" << targetExposure
					<< ", fireLines=" << currentFireLines << "->" << targetFireLines
					<< ", reason=point_route_may_cross_safe_choke_without_stalling_below_level";
				_save->appendToAutoBattleLog(log.str());
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger
		&& action->type == BA_WALK && firedThisTurn
		&& (factionStrategy == PFS_SURVIVE || factionStrategy == PFS_RETREAT_REGROUP)
		&& !_fallbackCoverAction && !_factionSupportMoveAction && !_stalkAmbushAction && !_cleanShotMoveAction)
	{
		const int currentSpotters = getSpottingUnits(_unit->getPosition());
		const int targetSpotters = getSpottingUnits(action->target);
		const int currentExposure = getEnemyFireExposure(_unit->getPosition());
		const int targetExposure = getEnemyFireExposure(action->target);
		const int currentFireLines = countEnemyFireLines(_unit->getPosition());
		const int targetFireLines = countEnemyFireLines(action->target);
		const bool verifiedSafer = targetSpotters <= currentSpotters
			&& targetFireLines <= currentFireLines
			&& targetExposure <= currentExposure
			&& (targetSpotters < currentSpotters || targetFireLines < currentFireLines || targetExposure + 25 < currentExposure);
		if (!verifiedSafer)
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction post-shot survival hold: unit=" << _unit->getId()
					<< ", rejectedTarget=" << action->target
					<< ", spotters=" << currentSpotters << "->" << targetSpotters
					<< ", exposure=" << currentExposure << "->" << targetExposure
					<< ", fireLines=" << currentFireLines << "->" << targetFireLines
					<< ", strategy=" << getFactionStrategyName(factionStrategy)
					<< ", reason=do_not_spend_remaining_tu_on_raw_patrol_after_firing";
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK
		&& !_visibleEnemies && !_spottingEnemies && !currentGrenadeDanger
		&& !_fallbackCoverAction
		&& !_crossLevelRouteMoveAction
		&& !_controlledProbeMoveAction
		&& isPlayerImmediateBacktrack(_save, _unit, action->target))
	{
		bool meaningfulHuntProgress = false;
		const int currentBacktrackSpotters = getSpottingUnits(_unit->getPosition());
		const int targetBacktrackSpotters = getSpottingUnits(action->target);
		const int currentBacktrackExposure = getEnemyFireExposure(_unit->getPosition());
		const int targetBacktrackExposure = getEnemyFireExposure(action->target);
		const int currentBacktrackFireLines = countEnemyFireLines(_unit->getPosition());
		const int targetBacktrackFireLines = countEnemyFireLines(action->target);
		const bool meaningfulBacktrackSafety = targetBacktrackSpotters < currentBacktrackSpotters
			|| targetBacktrackFireLines < currentBacktrackFireLines
			|| targetBacktrackExposure + 35 < currentBacktrackExposure;
		Position contactPos(-1, -1, -1);
		const BattleRoomInfo *contactRoom = 0;
		int enemiesInRoom = 0;
		bool visibleFactionContact = false;
		if (getFactionStrategy() == PFS_HUNT_LAST_ENEMY && _factionAI
			&& _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
		{
			meaningfulHuntProgress = Position::distance2d(action->target, contactPos) + 3
				< Position::distance2d(_unit->getPosition(), contactPos);
		}
		if (!meaningfulHuntProgress && !meaningfulBacktrackSafety)
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction immediate backtrack hold: unit=" << _unit->getId()
					<< ", position=" << _unit->getPosition()
					<< ", rejectedTarget=" << action->target
					<< ", contact=" << contactPos
					<< ", known=" << _knownEnemies
					<< ", supportMove=" << _factionSupportMoveAction
					<< ", safety=" << currentBacktrackSpotters << "/" << currentBacktrackFireLines
						<< "/" << currentBacktrackExposure << "->" << targetBacktrackSpotters
						<< "/" << targetBacktrackFireLines << "/" << targetBacktrackExposure
					<< ", mode=" << _AIMode
					<< ", strategy=" << getFactionStrategyName(getFactionStrategy())
					<< ", reason=preserve_tu_instead_of_returning_to_previous_tile";
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
			if (contactPos.x >= 0)
			{
				action->finalFacing = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), contactPos);
			}
			else if (_factionAI
				&& _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
			{
				action->finalFacing = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), contactPos);
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK
		&& !_visibleEnemies && !_spottingEnemies && movementOscillation >= (_knownEnemies ? 2 : 4)
		&& !_crossLevelRouteMoveAction && !_controlledProbeMoveAction)
	{
		bool holdOscillatingMove = true;
		Position contactPos;
		const BattleRoomInfo *contactRoom = 0;
		int enemiesInRoom = 0;
		bool visibleFactionContact = false;
		if (getFactionStrategy() == PFS_HUNT_LAST_ENEMY && _factionAI
			&& _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
		{
			const int currentContactDist = Position::distance2d(_unit->getPosition(), contactPos);
			const int targetContactDist = Position::distance2d(action->target, contactPos);
			holdOscillatingMove = targetContactDist + 3 >= currentContactDist;
		}
		if (holdOscillatingMove)
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction anti-oscillation ambush: unit=" << _unit->getId()
					<< ", position=" << _unit->getPosition()
					<< ", rejectedTarget=" << action->target
					<< ", contact=" << contactPos
					<< ", oscillation=" << movementOscillation
					<< ", known=" << _knownEnemies
					<< ", visible=" << _visibleEnemies
					<< ", spotting=" << _spottingEnemies
					<< ", mode=" << _AIMode
					<< ", strategy=" << getFactionStrategyName(getFactionStrategy())
					<< ", reason=hold_reaction_instead_of_looping";
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK
		&& _knownEnemies && !_visibleEnemies && !_spottingEnemies && !_factionSupportMoveAction && !_stalkAmbushAction && !_cleanShotMoveAction && !_fallbackCoverAction)
	{
		const PlayerFactionStrategy strategy = getFactionStrategy();
		const int baseTU = std::max(1, (int)_unit->getBaseStats()->tu);
		const int spentTU = baseTU - _unit->getTimeUnits();
		const bool earlyCautiousStrategy = strategy == PFS_INITIAL_DEPLOY
			|| strategy == PFS_DEFEND_LINE
			|| strategy == PFS_HOLD_REACTION
			|| strategy == PFS_SIEGE_ROOM
			|| strategy == PFS_SKIRMISH
			|| strategy == PFS_HUNT_LAST_ENEMY;
		const int stagingHoldThreshold = (strategy == PFS_HUNT_LAST_ENEMY || strategy == PFS_SKIRMISH) ? std::max(12, baseTU / 2) : 8;
		const bool alreadyStagedThisTurn = spentTU >= stagingHoldThreshold && (_save->getTurn() <= 2 || strategy == PFS_HUNT_LAST_ENEMY || strategy == PFS_SKIRMISH);
		if (earlyCautiousStrategy && alreadyStagedThisTurn)
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction staging reserve hold: unit=" << _unit->getId()
					<< ", strategy=" << getFactionStrategyName(strategy)
					<< ", position=" << _unit->getPosition()
					<< ", rejectedTarget=" << action->target
					<< ", spentTU=" << spentTU
					<< ", currentTU=" << _unit->getTimeUnits()
					<< ", known=" << _knownEnemies
					<< ", visible=" << _visibleEnemies
					<< ", spotting=" << _spottingEnemies
					<< ", mode=" << _AIMode
					<< ", reason=hold_reaction_after_staging_move";
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK && (_visibleEnemies || _spottingEnemies))
	{
		const bool firePointMove = _AIMode == AI_COMBAT
			&& _attackAction.type == BA_WALK
			&& action->weapon
			&& BattleActionCost(BA_SNAPSHOT, _unit, action->weapon).haveTU();
		auto defensiveCoverScore = [&](const Position &pos) -> int
		{
			Tile *tile = _save->getTile(pos);
			if (!tile)
			{
				return 0;
			}
			int cover = 0;
			if (tile->getMapData(O_OBJECT))
			{
				cover += 8;
			}
			if (tile->getMapData(O_NORTHWALL))
			{
				cover += 6;
			}
			if (tile->getMapData(O_WESTWALL))
			{
				cover += 6;
			}
			return cover;
		};
		const int targetSpotters = getSpottingUnits(action->target);
		const int currentCover = defensiveCoverScore(_unit->getPosition());
		const int targetCover = defensiveCoverScore(action->target);
		const int currentExposure = getEnemyFireExposure(_unit->getPosition());
		const int targetExposure = getEnemyFireExposure(action->target);
		const int currentFireLines = countEnemyFireLines(_unit->getPosition());
		const int targetFireLines = countEnemyFireLines(action->target);
		const int defensiveMoveDistance = Position::distance2d(action->target, _unit->getPosition());
		int adjacentAllies = 0;
		for (auto *other : *_save->getUnits())
		{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
			{
				continue;
			}
			if (other->getPosition().z == action->target.z && Position::distance2d(action->target, other->getPosition()) <= 1)
			{
				++adjacentAllies;
			}
		}
		const bool saferSpottingMove = targetSpotters < _spottingEnemies && (targetCover >= currentCover || targetSpotters + 1 < _spottingEnemies || defensiveMoveDistance <= 2);
		const bool betterCoverMove = targetSpotters <= _spottingEnemies && targetCover >= currentCover + 6;
		const bool breaksFireLine = currentFireLines == 0 || targetFireLines == 0 || (targetFireLines < currentFireLines && targetExposure + 40 < currentExposure);
		bool forwardHiddenContactMove = false;
		if (_spottingEnemies && !_visibleEnemies && _knownEnemies && !_fallbackCoverAction && !_cleanShotMoveAction && _factionAI)
		{
			Position contactPos;
			const BattleRoomInfo *contactRoom = 0;
			int enemiesInRoom = 0;
			bool visibleFactionContact = false;
			if (_factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
			{
				const int currentContactDist = Position::distance2d(_unit->getPosition(), contactPos);
				const int targetContactDist = Position::distance2d(action->target, contactPos);
				const bool meaningfulSafetyGain = targetFireLines < currentFireLines
					|| targetExposure + 35 < currentExposure
					|| targetCover >= currentCover + 12;
				forwardHiddenContactMove = targetContactDist < currentContactDist
					&& playerTurnMoveDistance >= 3
					&& !meaningfulSafetyGain;
			}
		}
		const bool defensiveMove = defensiveMoveDistance <= 4 && adjacentAllies == 0 && breaksFireLine && !forwardHiddenContactMove && (saferSpottingMove || betterCoverMove || targetFireLines < currentFireLines);
		const bool firePointIntoDanger = firePointMove
			&& (targetSpotters > 0 || targetExposure > currentExposure + 15 || targetFireLines > currentFireLines)
			&& !_fallbackCoverAction
			&& !(defensiveMoveDistance <= 3 && targetSpotters <= _spottingEnemies && targetCover >= currentCover + 12 && targetExposure <= currentExposure + 30 && breaksFireLine);
		const bool contactOscillation = movementOscillation >= 2 && playerTurnMoveCount >= 2;
		const bool materialSafetyGain = targetSpotters < _spottingEnemies
			|| targetFireLines < currentFireLines
			|| targetExposure + 25 < currentExposure
			|| targetCover >= currentCover + 12;
		const bool lowValueOscillatingFallback = contactOscillation
			&& _fallbackCoverAction
			&& !materialSafetyGain;
		const bool lowValueOscillatingContactMove = contactOscillation
			&& !_fallbackCoverAction
			&& !firePointMove
			&& !defensiveMove
			&& !materialSafetyGain;
		const bool holdContactMove = ((!firePointMove || firePointIntoDanger) && !defensiveMove && !_fallbackCoverAction)
			|| lowValueOscillatingFallback
			|| lowValueOscillatingContactMove;
		if (holdContactMove)
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction caution: unit=" << _unit->getId()
					<< " holds position instead of walking under contact"
					<< ", visible=" << _visibleEnemies
					<< ", spotting=" << _spottingEnemies
					<< ", targetSpotters=" << targetSpotters
					<< ", currentCover=" << currentCover
					<< ", targetCover=" << targetCover
					<< ", currentExposure=" << currentExposure
					<< ", targetExposure=" << targetExposure
					<< ", currentFireLines=" << currentFireLines
					<< ", targetFireLines=" << targetFireLines
					<< ", moveDistance=" << defensiveMoveDistance
					<< ", adjacentAllies=" << adjacentAllies
					<< ", firePointMove=" << firePointMove
					<< ", firePointIntoDanger=" << firePointIntoDanger
					<< ", forwardHiddenContactMove=" << forwardHiddenContactMove
					<< ", oscillation=" << movementOscillation
					<< ", turnMoveCount=" << playerTurnMoveCount
					<< ", fallbackCover=" << _fallbackCoverAction
					<< ", materialSafetyGain=" << materialSafetyGain
					<< ", lowValueOscillatingFallback=" << lowValueOscillatingFallback
					<< ", mode=" << _AIMode;
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
			Position contactPos;
			const BattleRoomInfo *contactRoom = 0;
			int enemiesInRoom = 0;
			bool visibleFactionContact = false;
			if (_factionAI && _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
			{
				action->finalFacing = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), contactPos);
			}
		}
		else if ((defensiveMove || _fallbackCoverAction) && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction defensive move allowed: unit=" << _unit->getId()
				<< ", target=" << action->target
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", targetSpotters=" << targetSpotters
				<< ", currentCover=" << currentCover
				<< ", targetCover=" << targetCover
				<< ", currentFireLines=" << currentFireLines
				<< ", targetFireLines=" << targetFireLines
				<< ", moveDistance=" << defensiveMoveDistance
				<< ", adjacentAllies=" << adjacentAllies
				<< ", forwardHiddenContactMove=" << forwardHiddenContactMove
				<< ", oscillation=" << movementOscillation
				<< ", turnMoveCount=" << playerTurnMoveCount
				<< ", fallbackCover=" << _fallbackCoverAction
				<< ", materialSafetyGain=" << materialSafetyGain
				<< ", mode=" << _AIMode;
			_save->appendToAutoBattleLog(log.str());
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK
		&& _knownEnemies && !_fallbackCoverAction && !_cleanShotMoveAction && !_factionSupportMoveAction
		&& !_controlledProbeMoveAction
		&& (!_stalkAmbushAction || (overwhelmingHeavyLanding && _save->getTurn() <= 1 && playerTurnMoveCount > 0))
		&& !currentGrenadeDanger && !firedThisTurn)
	{
		if (overwhelmingHeavyLanding && playerTurnMoveCount > 0 && action->target != _unit->getPosition())
		{
			auto blastCrowdAt = [&](const Position &pos, int radius) -> int
			{
				int crowd = 0;
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					if (std::abs(other->getPosition().z - pos.z) <= 1
						&& Position::distance2d(other->getPosition(), pos) <= radius)
					{
						++crowd;
					}
				}
				return crowd;
			};
			Position contactPos;
			const BattleRoomInfo *contactRoom = 0;
			int enemiesInRoom = 0;
			bool visibleFactionContact = false;
			const bool hasContact = _factionAI
				&& _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact);
			const int blastCrowdRadius = std::max(4, playerMaxHostileBlastRadius);
			const int currentBlastCrowd = blastCrowdAt(_unit->getPosition(), blastCrowdRadius);
			const int targetBlastCrowd = blastCrowdAt(action->target, blastCrowdRadius);
			const int dispersionMoveDistance = Position::distance2d(action->target, _unit->getPosition());
			const bool safeDispersionStep = currentBlastCrowd >= 5
				&& targetBlastCrowd + 2 < currentBlastCrowd
				&& getSpottingUnits(action->target) == 0
				&& countEnemyFireLines(action->target) == 0
				&& getEnemyFireExposure(action->target) <= getEnemyFireExposure(_unit->getPosition()) + 25;
			if (safeDispersionStep)
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction siege landing dispersion allowed: unit=" << _unit->getId()
						<< ", target=" << action->target
						<< ", position=" << _unit->getPosition()
						<< ", currentBlastCrowd=" << currentBlastCrowd
						<< ", targetBlastCrowd=" << targetBlastCrowd
						<< ", moveDistance=" << dispersionMoveDistance
						<< ", radius=" << blastCrowdRadius
						<< ", turnMoveCount=" << playerTurnMoveCount
						<< ", contact=" << (hasContact ? contactPos : Position(-1, -1, -1))
						<< ", reason=reduce_landing_grenade_cluster";
					_save->appendToAutoBattleLog(log.str());
				}
			}
			else
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction siege landing hold: unit=" << _unit->getId()
						<< ", target=" << action->target
						<< ", position=" << _unit->getPosition()
						<< ", turnMoveCount=" << playerTurnMoveCount
						<< ", turnMoveDistance=" << playerTurnMoveDistance
						<< ", currentTU=" << _unit->getTimeUnits()
						<< ", supportMove=" << _factionSupportMoveAction
						<< ", activeAllies=" << playerActiveAllies
						<< ", activeHostiles=" << playerActiveHostiles
						<< ", maxHostileHealth=" << playerMaxHostileHealth
						<< ", maxHostileWeaponDanger=" << playerMaxHostileWeaponDanger
						<< ", currentBlastCrowd=" << currentBlastCrowd
						<< ", targetBlastCrowd=" << targetBlastCrowd
						<< ", moveDistance=" << dispersionMoveDistance
						<< ", contact=" << (hasContact ? contactPos : Position(-1, -1, -1))
						<< ", reason=overwhelming_heavy_landing";
					_save->appendToAutoBattleLog(log.str());
				}
				action->type = BA_NONE;
				action->target = _unit->getPosition();
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
				if (hasContact)
				{
					action->finalFacing = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), contactPos);
				}
			}
		}
		else if ((playerHeavyExplosiveDoctrine || playerHighHpExplosiveReserve) && playerExplosiveReserveTU > 0 && playerExplosiveThrowerRank < 2 && playerTurnMoveCount > 0)
		{
			const int moveDistance = Position::distance2d(action->target, _unit->getPosition());
			_save->getPathfinding()->calculate(_unit, action->target, action->getMoveType());
			const int estimatedMoveTU = _save->getPathfinding()->getStartDirection() != -1
				? _save->getPathfinding()->getTotalTUCost()
				: _unit->getTimeUnits() + 1;
			_save->getPathfinding()->abortPath();
			const int reserveTU = std::min(_unit->getBaseStats()->tu - 4, playerExplosiveReserveTU + 8);
			if (estimatedMoveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction heavy explosive reserve hold: unit=" << _unit->getId()
						<< ", target=" << action->target
						<< ", moveDistance=" << moveDistance
						<< ", estimatedMoveTU=" << estimatedMoveTU
						<< ", reserveTU=" << reserveTU
						<< ", currentTU=" << _unit->getTimeUnits()
						<< ", turnMoveCount=" << playerTurnMoveCount
						<< ", turnMoveDistance=" << playerTurnMoveDistance
						<< ", known=" << _knownEnemies
						<< ", visible=" << _visibleEnemies
						<< ", spotting=" << _spottingEnemies
						<< ", maxHostileHealth=" << playerMaxHostileHealth
						<< ", throwerRank=" << playerExplosiveThrowerRank
						<< ", supportMove=" << _factionSupportMoveAction;
					_save->appendToAutoBattleLog(log.str());
				}
				action->type = BA_NONE;
				action->target = _unit->getPosition();
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
			}
		}
		const bool earlyContactPressure = _save->getTurn() <= 2 && playerActiveHostiles >= 5 && _knownEnemies >= 5;
		const bool lateSurvivalBreakout = factionStrategy == PFS_SURVIVE && (playerActiveAllies <= 3 || _save->getTurn() >= 45);
		const bool finalHuntBreakout = factionStrategy == PFS_HUNT_LAST_ENEMY && (playerActiveHostiles <= 3 || _save->getTurn() >= 35);
		const bool hiddenContactPressure = !_visibleEnemies
			&& !lateSurvivalBreakout
			&& !finalHuntBreakout
			&& playerActiveHostiles >= 12
			&& playerActiveHostiles >= playerActiveAllies - 2
			&& _knownEnemies >= 10;
		const int repeatedMoveLimit = earlyContactPressure ? 4 : (hiddenContactPressure ? (tacticalRole == TACTICAL_SCOUT ? 8 : (tacticalRole == TACTICAL_REACTION_GUARD ? 5 : 4)) : 7);
		const int moveDistance = Position::distance2d(action->target, _unit->getPosition());
		const int proposedTurnMoveDistance = playerTurnMoveDistance + moveDistance;
		const bool firstLongContactMove = playerTurnMoveCount == 0
			&& highHostileFirepower
			&& !lateSurvivalBreakout
			&& !finalHuntBreakout
			&& proposedTurnMoveDistance > repeatedMoveLimit;
		const bool repeatedContactMove = playerTurnMoveCount >= 2 || proposedTurnMoveDistance > repeatedMoveLimit;
		if ((_spottingEnemies || earlyContactPressure || hiddenContactPressure || firstLongContactMove) && repeatedContactMove)
		{
			const int currentSpotters = getSpottingUnits(_unit->getPosition());
			const int targetSpotters = getSpottingUnits(action->target);
			const int currentExposure = getEnemyFireExposure(_unit->getPosition());
			const int targetExposure = getEnemyFireExposure(action->target);
			const int currentFireLines = countEnemyFireLines(_unit->getPosition());
			const int targetFireLines = countEnemyFireLines(action->target);
			const bool materiallySafer = targetSpotters < currentSpotters
				|| targetFireLines < currentFireLines
				|| targetExposure + 45 < currentExposure
				|| (!hiddenContactPressure && moveDistance <= 1 && targetSpotters <= currentSpotters && targetExposure <= currentExposure && targetFireLines <= currentFireLines);
			if (!materiallySafer)
			{
				bool clippedFirstAdvance = false;
				if (firstLongContactMove)
				{
					const Position originalTarget = action->target;
					Position limitedTarget = _unit->getPosition();
					_save->getPathfinding()->calculate(_unit, originalTarget, action->getMoveType());
					if (_save->getPathfinding()->getStartDirection() != -1)
					{
						const std::vector<int> path = _save->getPathfinding()->copyPath();
						Position pathPos = _unit->getPosition();
						for (auto direction = path.rbegin(); direction != path.rend(); ++direction)
						{
							const PathfindingStep step = _save->getPathfinding()->getTUCost(pathPos, *direction, _unit, 0, action->getMoveType());
							if (step.cost.time == Pathfinding::INVALID_MOVE_COST
								|| Position::distance2d(_unit->getPosition(), step.pos) > repeatedMoveLimit)
							{
								break;
							}
							pathPos = step.pos;
							limitedTarget = pathPos;
						}
					}
					_save->getPathfinding()->abortPath();
					if (limitedTarget != _unit->getPosition())
					{
						int reserveTU = 18;
						if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
						{
							BattleActionCost snapCost(BA_SNAPSHOT, _unit, action->weapon);
							if (snapCost.Time > 0)
							{
								reserveTU = std::max(reserveTU, (int)snapCost.Time);
							}
						}
						_save->getPathfinding()->calculate(_unit, limitedTarget, action->getMoveType());
						const bool pathReady = _save->getPathfinding()->getStartDirection() != -1;
						const int limitedMoveTU = pathReady ? _save->getPathfinding()->getTotalTUCost() : 100000;
						const bool riskyPath = pathReady && playerPathCrossesExplosiveDanger(_save, _unit,
							_save->getPathfinding()->copyPath(), action->getMoveType(), false);
						_save->getPathfinding()->abortPath();
						const int limitedSpotters = getSpottingUnits(limitedTarget);
						const int limitedExposure = getEnemyFireExposure(limitedTarget);
						const int limitedFireLines = countEnemyFireLines(limitedTarget);
						const bool safeLimitedStep = limitedSpotters <= currentSpotters
							&& limitedFireLines <= currentFireLines
							&& limitedExposure <= currentExposure + (currentExposure > 0 ? 35 : 0);
						if (pathReady && !riskyPath && safeLimitedStep
							&& limitedMoveTU <= std::max(0, _unit->getTimeUnits() - reserveTU))
						{
							action->target = limitedTarget;
							action->finalAction = true;
							action->kneel = _unit->getArmor()->allowsKneeling(false);
							clippedFirstAdvance = true;
							if (Options::autoBattleLog)
							{
								std::ostringstream log;
								log << "Player faction first advance clipped: unit=" << _unit->getId()
									<< ", originalTarget=" << originalTarget
									<< ", limitedTarget=" << limitedTarget
									<< ", moveTU=" << limitedMoveTU
									<< ", reserveTU=" << reserveTU
									<< ", spotters=" << currentSpotters << "->" << limitedSpotters
									<< ", exposure=" << currentExposure << "->" << limitedExposure
									<< ", fireLines=" << currentFireLines << "->" << limitedFireLines
									<< ", reason=bounded_safe_first_step_instead_of_raw_long_patrol";
								_save->appendToAutoBattleLog(log.str());
							}
						}
					}
				}
				if (!clippedFirstAdvance && Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction repeated advance hold: unit=" << _unit->getId()
						<< ", target=" << action->target
						<< ", turnMoveCount=" << playerTurnMoveCount
						<< ", turnMoveDistance=" << playerTurnMoveDistance
						<< ", proposedTurnMoveDistance=" << proposedTurnMoveDistance
						<< ", earlyContactPressure=" << earlyContactPressure
						<< ", hiddenContactPressure=" << hiddenContactPressure
						<< ", lateSurvivalBreakout=" << lateSurvivalBreakout
						<< ", finalHuntBreakout=" << finalHuntBreakout
						<< ", repeatedMoveLimit=" << repeatedMoveLimit
						<< ", currentSpotters=" << currentSpotters
						<< ", targetSpotters=" << targetSpotters
						<< ", currentExposure=" << currentExposure
						<< ", targetExposure=" << targetExposure
						<< ", currentFireLines=" << currentFireLines
						<< ", targetFireLines=" << targetFireLines
						<< ", mode=" << _AIMode;
					_save->appendToAutoBattleLog(log.str());
				}
				if (!clippedFirstAdvance)
				{
					action->type = BA_NONE;
					action->target = _unit->getPosition();
					action->finalAction = true;
					action->kneel = _unit->getArmor()->allowsKneeling(false);
				}
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK && _knownEnemies && (_visibleEnemies || _spottingEnemies))
	{
		const int moveDistance = Position::distance2d(action->target, _unit->getPosition());
		int reserveTU = 12;
		if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			reserveTU = (_visibleEnemies || _spottingEnemies) ? 20 : 12;
			BattleActionCost snapCost(BA_SNAPSHOT, _unit, action->weapon);
			if (snapCost.Time > 0)
			{
				reserveTU = std::max(reserveTU, (_visibleEnemies || _spottingEnemies) ? (int)snapCost.Time : (int)snapCost.Time / 2);
			}
		}
		if (playerHeavyExplosiveDoctrine && playerExplosiveReserveTU > 0)
		{
			reserveTU = std::max(reserveTU, std::min(_unit->getBaseStats()->tu - 4, playerExplosiveReserveTU + 8));
		}
		const int estimatedMoveTU = moveDistance * 5;
		if (!_fallbackCoverAction && !_cleanShotMoveAction
			&& estimatedMoveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction reserve hold: unit=" << _unit->getId()
					<< " holds position to keep TU reserve"
					<< ", target=" << action->target
					<< ", moveDistance=" << moveDistance
					<< ", estimatedMoveTU=" << estimatedMoveTU
					<< ", reserveTU=" << reserveTU
					<< ", currentTU=" << _unit->getTimeUnits()
					<< ", known=" << _knownEnemies
					<< ", visible=" << _visibleEnemies
					<< ", spotting=" << _spottingEnemies
					<< ", mode=" << _AIMode;
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger && action->type == BA_WALK
		&& !_stalkAmbushAction && !_cleanShotMoveAction && !_fallbackCoverAction && !_factionSupportMoveAction
		&& !_visibleEnemies && !_spottingEnemies && _factionAI && _factionAI->getEnemyContactCount() > 0)
	{
		Position contactPos;
		const BattleRoomInfo *contactRoom = 0;
		int enemiesInRoom = 1;
		bool visibleFactionContact = false;
		if (_factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
		{
			auto cautiousCoverScore = [&](const Position &pos) -> int
			{
				Tile *tile = _save->getTile(pos);
				if (!tile)
				{
					return 0;
				}
				int cover = 0;
				if (tile->getMapData(O_OBJECT))
				{
					cover += 8;
				}
				if (tile->getMapData(O_NORTHWALL))
				{
					cover += 6;
				}
				if (tile->getMapData(O_WESTWALL))
				{
					cover += 6;
				}
				return cover;
			};
			auto allyCrowdingPenaltyAt = [&](const Position &pos) -> int
			{
				int penalty = 0;
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					const int dist = Position::distance2d(pos, other->getPosition());
					if (dist == 0)
					{
						penalty += 120;
					}
					else if (dist == 1)
					{
						penalty += 35;
					}
				}
				return penalty;
			};
			const PlayerFactionStrategy strategy = getFactionStrategy();
			const bool lateHunt = strategy == PFS_HUNT_LAST_ENEMY && (playerActiveHostiles <= 3 || _save->getTurn() >= 35);
			const bool forcedHunt = strategy == PFS_HUNT_LAST_ENEMY && (playerActiveHostiles <= 1 || _save->getTurn() >= 45);
			const int activeHostiles = playerActiveHostiles;
			bool durableVisibleContact = false;
			if (visibleFactionContact)
			{
				Tile *contactTile = _save->getTile(contactPos);
				BattleUnit *contactUnit = contactTile ? contactTile->getUnit() : 0;
				if (contactUnit && validTarget(contactUnit, true, true))
				{
					const int armor = std::max(std::max(contactUnit->getArmor(SIDE_FRONT), contactUnit->getArmor(SIDE_LEFT)), contactUnit->getArmor(SIDE_RIGHT));
					BattleItem *enemyWeapon = contactUnit->getMainHandWeapon(false);
					const int enemyWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
					durableVisibleContact = contactUnit->getHealth() >= 70 || armor >= 35 || enemyWeaponDanger >= 135;
				}
			}
			int reserveTU = 18;
			if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
			{
				BattleActionCost snapCost(BA_SNAPSHOT, _unit, action->weapon);
				if (snapCost.Time > 0)
				{
					reserveTU = std::max(reserveTU, (int)snapCost.Time);
				}
			}
			if (playerHeavyExplosiveDoctrine && playerExplosiveReserveTU > 0)
			{
				reserveTU = std::max(reserveTU, std::min(_unit->getBaseStats()->tu - 4, playerExplosiveReserveTU + 8));
			}
			int maxMoveTU = std::max(8, std::min(activeHostiles > 2 ? 18 : 28, _unit->getTimeUnits() - reserveTU));
			if (strategy == PFS_HUNT_LAST_ENEMY || strategy == PFS_SKIRMISH)
			{
				const int baseTU = std::max(1, (int)_unit->getBaseStats()->tu);
				const int spentTU = baseTU - _unit->getTimeUnits();
				const int stagingThreshold = std::max(12, baseTU / 2);
				maxMoveTU = std::min(maxMoveTU, std::max(4, stagingThreshold - spentTU - 1));
			}
			const int currentContactDist = Position::distance2d(_unit->getPosition(), contactPos);
			const int currentTargetDist = Position::distance2d(_unit->getPosition(), action->target);
			const int currentCover = cautiousCoverScore(_unit->getPosition());
			const int cautiousCurrentSpotters = getSpottingUnits(_unit->getPosition());
			const int cautiousCurrentExposure = getEnemyFireExposure(_unit->getPosition());
			const int cautiousCurrentFireLines = countEnemyFireLines(_unit->getPosition());
			const bool survivalWithdrawal = strategy == PFS_SURVIVE || strategy == PFS_RETREAT_REGROUP;
			int survivalMoveBudgetRemaining = maxMoveTU;
			if (survivalWithdrawal)
			{
				const int baseTU = std::max(1, (int)_unit->getBaseStats()->tu);
				const int spentTU = std::max(0, baseTU - _unit->getTimeUnits());
				// A retreat is a displacement followed by a defended position, not a
				// chain of hops which consumes the whole turn.  Permit a longer escape
				// only while the unit is currently in a live firing lane.
				const int turnBudget = (cautiousCurrentFireLines > 0 || cautiousCurrentSpotters > 0)
					? std::max(18, baseTU / 2)
					: std::max(14, baseTU / 3);
				survivalMoveBudgetRemaining = std::max(0, turnBudget - spentTU);
				maxMoveTU = std::min(maxMoveTU, survivalMoveBudgetRemaining);
			}
			Position bestPos = _unit->getPosition();
			int bestScore = 1000000;
			int bestMoveTU = 0;
			int bestCover = currentCover;
			int bestContactDist = currentContactDist;
			const bool forcedPointAdvance = forcedHunt && currentContactDist > 6
				&& dynamicGroup.role == PDGR_POINT && dynamicGroup.maneuverGroup;
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile)
				{
					continue;
				}
				const Position pos = tile->getPosition();
				if (pos == _unit->getPosition())
				{
					continue;
				}
				const bool cautiousRampStep = strategy == PFS_INITIAL_DEPLOY
					&& pos.z == _unit->getPosition().z - 1;
				if ((!cautiousRampStep && pos.z != _unit->getPosition().z) || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
				{
					continue;
				}
				const int stepDist = Position::distance2d(pos, _unit->getPosition());
				const int maxStepDist = lateHunt ? 6 : (tacticalRole == TACTICAL_SCOUT ? 5 : 4);
				if (stepDist > maxStepDist)
				{
					continue;
				}
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL, 0, maxMoveTU);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int moveTU = _save->getPathfinding()->getTotalTUCost();
				const bool riskyPath = playerPathCrossesExplosiveDanger(_save, _unit, _save->getPathfinding()->copyPath(), BAM_NORMAL, false);
				_save->getPathfinding()->abortPath();
				if (moveTU > maxMoveTU || riskyPath)
				{
					continue;
				}
				const int contactDist = Position::distance2d(pos, contactPos);
				const int targetDist = Position::distance2d(pos, action->target);
				const int cover = cautiousCoverScore(pos);
				const bool defensiveHiddenStandoff = !visibleFactionContact && highHostileFirepower
					&& (strategy == PFS_SKIRMISH || strategy == PFS_DEFEND_LINE
						|| strategy == PFS_HOLD_REACTION || strategy == PFS_SIEGE_ROOM
						|| strategy == PFS_SURVIVE);
				const int hiddenStandoffDistance = tacticalRole == TACTICAL_FIRE_SUPPORT
					|| tacticalRole == TACTICAL_REACTION_GUARD
					? std::max(6, preferredRange / 2)
					: 4;
				if (defensiveHiddenStandoff
					&& (contactDist < hiddenStandoffDistance
						|| (contactDist <= hiddenStandoffDistance + 2 && cover < currentCover)))
				{
					continue;
				}
				const bool progresses = survivalWithdrawal
					? contactDist > currentContactDist
					: (visibleFactionContact
						? (contactDist < currentContactDist || targetDist < currentTargetDist)
						: (contactDist < currentContactDist));
				if (forcedPointAdvance && contactDist >= currentContactDist)
				{
					continue;
				}
				if (!forcedHunt && !progresses && cover < currentCover + 6)
				{
					continue;
				}
				if (!survivalWithdrawal && !forcedHunt && !visibleFactionContact
					&& contactDist > currentContactDist && cover < currentCover + 10)
				{
					continue;
				}
				const int spotters = getSpottingUnits(pos);
				const int exposure = getEnemyFireExposure(pos);
				const int fireLines = countEnemyFireLines(pos);
				const bool strictSurvivalSafetyGain = fireLines < cautiousCurrentFireLines
					|| spotters < cautiousCurrentSpotters
					|| exposure + 35 < cautiousCurrentExposure
					|| cover >= currentCover + 12;
				const bool materialFireSafetyGain = fireLines < cautiousCurrentFireLines
					|| spotters < cautiousCurrentSpotters
					|| exposure + 35 < cautiousCurrentExposure;
				if (isPlayerImmediateBacktrack(_save, _unit, pos)
					&& !materialFireSafetyGain && cover < currentCover + 10)
				{
					continue;
				}
				if (survivalWithdrawal && currentCover >= 8 && cover + 6 < currentCover
					&& !materialFireSafetyGain)
				{
					continue;
				}
				if (survivalWithdrawal && contactDist < currentContactDist && !strictSurvivalSafetyGain)
				{
					continue;
				}
				if (!lateHunt && (spotters > 0 || fireLines > 0 || exposure > 60))
				{
					continue;
				}
				if (!forcedHunt && durableVisibleContact && activeHostiles > 10 && contactDist <= 8 && cover < std::max(8, currentCover + 10))
				{
					continue;
				}
				int score = contactDist * 12 + targetDist * 2 + moveTU * 2 + allyCrowdingPenaltyAt(pos);
				score -= cover * 8;
				score -= std::max(0, currentContactDist - contactDist) * 18;
				score += spotters * 45;
				score += exposure;
				score += fireLines * 75;
				if (durableVisibleContact && activeHostiles >= 6 && contactDist <= 12)
				{
					score += (12 - contactDist) * 35;
					if (cover < 8)
					{
						score += (8 - cover) * (activeHostiles > 10 ? 18 : 10);
					}
				}
				if (cover + 6 < currentCover && contactDist < currentContactDist)
				{
					score += 70;
				}
				if (!visibleFactionContact && contactDist > currentContactDist)
				{
					score += (contactDist - currentContactDist) * 60;
				}
				if (survivalWithdrawal)
				{
					// In survive/retreat the contact-distance term has the opposite
					// meaning: separation is valuable and approach is acceptable only
					// for the strict safety gain checked above.
					score += std::max(0, currentContactDist - contactDist) * 250;
					score -= std::max(0, contactDist - currentContactDist) * 95;
				}
				if (score < bestScore)
				{
					bestScore = score;
					bestPos = pos;
					bestMoveTU = moveTU;
					bestCover = cover;
					bestContactDist = contactDist;
				}
			}
			if (bestPos != _unit->getPosition())
			{
				const bool cautiousContactProbeReplan = !visibleFactionContact
					&& dynamicGroup.maneuverGroup && activeHostiles <= 3
					&& (strategy == PFS_HUNT_LAST_ENEMY || strategy == PFS_SKIRMISH)
					&& bestContactDist + 2 <= currentContactDist
					&& bestMoveTU <= 12
					&& _unit->getTimeUnits() - bestMoveTU >= reserveTU + 4;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction cautious patrol step: unit=" << _unit->getId()
						<< ", originalTarget=" << action->target
						<< ", newTarget=" << bestPos
						<< ", contact=" << contactPos
						<< ", currentContactDist=" << currentContactDist
						<< ", newContactDist=" << bestContactDist
						<< ", moveTU=" << bestMoveTU
						<< ", maxMoveTU=" << maxMoveTU
						<< ", currentCover=" << currentCover
						<< ", newCover=" << bestCover
						<< ", activeHostiles=" << activeHostiles
						<< ", durableVisibleContact=" << durableVisibleContact
						<< ", enemiesInRoom=" << enemiesInRoom
						<< ", visibleFactionContact=" << visibleFactionContact
						<< ", strategy=" << getFactionStrategyName(strategy)
						<< ", lateHunt=" << lateHunt
						<< ", forcedHunt=" << forcedHunt
						<< ", replanAfterProbe=" << cautiousContactProbeReplan;
					_save->appendToAutoBattleLog(log.str());
				}
				action->target = bestPos;
				action->finalAction = !cautiousContactProbeReplan;
				action->kneel = !cautiousContactProbeReplan && _unit->getArmor()->allowsKneeling(false);
				if (cautiousContactProbeReplan)
				{
					_controlledProbeMoveAction = true;
				}
			}
			else if (survivalWithdrawal)
			{
				const int originalContactDist = Position::distance2d(action->target, contactPos);
				const int originalCover = cautiousCoverScore(action->target);
				const int originalSpotters = getSpottingUnits(action->target);
				const int originalExposure = getEnemyFireExposure(action->target);
				const int originalFireLines = countEnemyFireLines(action->target);
				const bool originalStrictSafetyGain = originalFireLines < cautiousCurrentFireLines
					|| originalSpotters < cautiousCurrentSpotters
					|| originalExposure + 35 < cautiousCurrentExposure
					|| originalCover >= currentCover + 12;
				_save->getPathfinding()->calculate(_unit, action->target, action->getMoveType());
				const bool originalPathReady = _save->getPathfinding()->getStartDirection() != -1;
				const int originalMoveTU = originalPathReady ? _save->getPathfinding()->getTotalTUCost() : 100000;
				_save->getPathfinding()->abortPath();
				const bool originalAbandonsGoodCover = currentCover >= 8 && originalCover + 6 < currentCover
					&& !(originalFireLines < cautiousCurrentFireLines
						|| originalSpotters < cautiousCurrentSpotters
						|| originalExposure + 35 < cautiousCurrentExposure);
				if (!originalPathReady || originalMoveTU > survivalMoveBudgetRemaining
					|| originalAbandonsGoodCover
					|| (originalContactDist <= currentContactDist && !originalStrictSafetyGain))
				{
					if (Options::autoBattleLog)
					{
						std::ostringstream log;
						log << "Player faction survival advance rejected: unit=" << _unit->getId()
							<< ", position=" << _unit->getPosition()
							<< ", rejectedTarget=" << action->target
							<< ", contact=" << contactPos
							<< ", distance=" << currentContactDist << "->" << originalContactDist
							<< ", cover=" << currentCover << "->" << originalCover
							<< ", spotters=" << cautiousCurrentSpotters << "->" << originalSpotters
							<< ", exposure=" << cautiousCurrentExposure << "->" << originalExposure
							<< ", fireLines=" << cautiousCurrentFireLines << "->" << originalFireLines
							<< ", moveTU=" << originalMoveTU
							<< ", moveBudgetRemaining=" << survivalMoveBudgetRemaining
							<< ", abandonsGoodCover=" << originalAbandonsGoodCover
							<< ", strategy=" << getFactionStrategyName(strategy)
							<< ", reason=survival_mode_must_withdraw_or_gain_strict_safety";
						_save->appendToAutoBattleLog(log.str());
					}
					action->type = BA_NONE;
					action->target = _unit->getPosition();
					action->finalAction = true;
					action->kneel = _unit->getArmor()->allowsKneeling(false);
				}
			}
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER && !evacuatingGrenadeDanger
		&& action->type == BA_WALK && _knownEnemies && !_visibleEnemies && !_spottingEnemies
		&& !_factionSupportMoveAction && !_stalkAmbushAction && !_cleanShotMoveAction && !_fallbackCoverAction
		&& !_controlledProbeMoveAction)
	{
		const PlayerFactionStrategy strategy = getFactionStrategy();
		if (strategy == PFS_HUNT_LAST_ENEMY || strategy == PFS_SKIRMISH)
		{
			const int baseTU = std::max(1, (int)_unit->getBaseStats()->tu);
			const int spentTU = baseTU - _unit->getTimeUnits();
			const int stagingHoldThreshold = std::max(12, baseTU / 2);
			if (spentTU < stagingHoldThreshold)
			{
				_save->getPathfinding()->calculate(_unit, action->target, action->getMoveType());
				const bool pathReady = _save->getPathfinding()->getStartDirection() != -1;
				const int proposedMoveTU = pathReady ? _save->getPathfinding()->getTotalTUCost() : 0;
				const std::vector<int> proposedPath = pathReady
					? _save->getPathfinding()->copyPath()
					: std::vector<int>();
				_save->getPathfinding()->abortPath();
				if (pathReady && spentTU + proposedMoveTU >= stagingHoldThreshold)
				{
					const int prefixBudget = std::max(0, stagingHoldThreshold - spentTU - 1);
					Position prefixPos = _unit->getPosition();
					int prefixTU = 0;
					for (auto direction = proposedPath.rbegin(); direction != proposedPath.rend(); ++direction)
					{
						const PathfindingStep step = _save->getPathfinding()->getTUCost(prefixPos, *direction,
							_unit, 0, action->getMoveType());
						if (step.cost.time == Pathfinding::INVALID_MOVE_COST
							|| prefixTU + step.cost.time > prefixBudget
							|| isPlayerExplosiveDanger(_save, _unit->getFaction(), step.pos))
						{
							break;
						}
						prefixTU += step.cost.time;
						prefixPos = step.pos;
					}
					if (Options::autoBattleLog)
					{
						std::ostringstream log;
						log << "Player faction predictive staging reserve clip: unit=" << _unit->getId()
							<< ", strategy=" << getFactionStrategyName(strategy)
							<< ", position=" << _unit->getPosition()
							<< ", rejectedTarget=" << action->target
							<< ", prefixTarget=" << prefixPos
							<< ", spentTU=" << spentTU
							<< ", proposedMoveTU=" << proposedMoveTU
							<< ", prefixTU=" << prefixTU
							<< ", threshold=" << stagingHoldThreshold
							<< ", reason=advance_only_to_last_prefix_that_preserves_reaction_budget";
						_save->appendToAutoBattleLog(log.str());
					}
					if (prefixPos == _unit->getPosition())
					{
						action->type = BA_NONE;
					}
					action->target = prefixPos;
					action->finalAction = true;
					action->kneel = _unit->getArmor()->allowsKneeling(false);
				}
			}
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER
		&& action->type == BA_NONE
		&& _spottingEnemies > 0
		&& !_visibleEnemies
		&& !currentGrenadeDanger
		&& playerActiveHostiles >= 12
		&& playerMaxHostileHealth >= 80
		&& highPressureHostileFirepower
		&& setupFallbackCoverMove(45, 0, 1, 0))
	{
		*action = _attackAction;
		action->finalAction = true;
		action->kneel = _unit->getArmor()->allowsKneeling(false);
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction heavy landing cancelled attack fallback: unit=" << _unit->getId()
				<< ", target=" << action->target
				<< ", spotting=" << _spottingEnemies
				<< ", known=" << _knownEnemies
				<< ", activeHostiles=" << playerActiveHostiles
				<< ", maxHostileHealth=" << playerMaxHostileHealth
				<< ", strategy=" << getFactionStrategyName(getFactionStrategy())
				<< ", reason=break_hidden_heavy_enemy_line_after_attack_move_was_cancelled";
			_save->appendToAutoBattleLog(log.str());
		}
	}

	// Planned shots have a projectile friendly-fire guard, but automatic reaction
	// shots are created later by TileEngine and bypass it.  Keep ordinary squad
	// endpoints staggered so a soldier is not left directly in front of a ready
	// rifle toward the current contact.  This is deliberately a monotonic
	// post-process: it may only reduce risky lanes without worsening fire safety.
	if (_unit->getFaction() == FACTION_PLAYER
		&& !evacuatingGrenadeDanger && !_controlledProbeMoveAction
		&& ((action->type == BA_WALK && action->finalAction
				&& _AIMode != AI_ESCAPE && !_fallbackCoverAction && !_factionSupportMoveAction
				&& !_stalkAmbushAction && !_cleanShotMoveAction)
			|| action->type == BA_NONE))
	{
		Position reactionContact(-1, -1, -1);
		bool hasReactionContact = false;
		if (_aggroTarget && !_aggroTarget->isOut() && _aggroTarget->getFaction() == FACTION_HOSTILE)
		{
			reactionContact = _aggroTarget->getPosition();
			hasReactionContact = true;
		}
		else if (_factionAI)
		{
			const BattleRoomInfo *contactRoom = 0;
			int enemiesInRoom = 0;
			bool visibleContact = false;
			hasReactionContact = _factionAI->getBestEnemyContactPosition(
				&reactionContact, &contactRoom, &enemiesInRoom, &visibleContact);
		}
		if (hasReactionContact)
		{
			auto directReactionReady = [&](BattleUnit *shooter, BattleItem *weapon, int remainingTU) -> bool
			{
				if (!shooter || shooter->isOut() || shooter->getFaction() != FACTION_PLAYER
					|| !weapon || !weapon->getRules()
					|| weapon->getRules()->getBattleType() != BT_FIREARM
					|| weapon->getRules()->getAccuracySnap() <= 0
					|| weapon->getArcingShot(BA_SNAPSHOT)
					|| !weapon->getAmmoForAction(BA_SNAPSHOT)
					|| !_save->canUseWeapon(weapon, shooter, false, BA_SNAPSHOT))
				{
					return false;
				}
				BattleActionCost snapCost(BA_SNAPSHOT, shooter, weapon);
				return snapCost.Time > 0 && snapCost.Time <= remainingTU;
			};
			auto blocksReactionLane = [&](const Position &shooterPos, const Position &blockerPos,
				const Position &contactPos) -> bool
			{
				if (shooterPos.z != blockerPos.z || std::abs(shooterPos.z - contactPos.z) > 1)
				{
					return false;
				}
				const double vx = (double)(contactPos.x - shooterPos.x);
				const double vy = (double)(contactPos.y - shooterPos.y);
				const double lenSq = vx * vx + vy * vy;
				if (lenSq <= PLAYER_AI_GEOMETRY_EPSILON)
				{
					return false;
				}
				const double ax = (double)(blockerPos.x - shooterPos.x);
				const double ay = (double)(blockerPos.y - shooterPos.y);
				const double along = ax * vx + ay * vy;
				const double t = along / lenSq;
				if (t <= 0.0 || t >= 1.05)
				{
					return false;
				}
				const double forward = along / std::sqrt(lenSq);
				if (forward < 0.5 || forward > 12.5)
				{
					return false;
				}
				const double dx = ax - vx * t;
				const double dy = ay - vy * t;
				return dx * dx + dy * dy <= 1.45;
			};
			auto reactionOutgoingLaneCount = [&](const Position &unitPos, int unitRemainingTU) -> int
			{
				int count = 0;
				BattleItem *unitReactionWeapon = action->weapon ? action->weapon : _unit->getMainHandWeapon(false);
				if (!directReactionReady(_unit, unitReactionWeapon, unitRemainingTU))
				{
					return count;
				}
				Position unitReactionContact = reactionContact;
				BattleUnit *unitAssignedTarget = _factionAI ? _factionAI->getAssignedTarget(_unit) : 0;
				if (unitAssignedTarget && !unitAssignedTarget->isOut()
					&& unitAssignedTarget->getFaction() == FACTION_HOSTILE)
				{
					unitReactionContact = unitAssignedTarget->getPosition();
				}
				for (auto *ally : *_save->getUnits())
				{
					if (!ally || ally == _unit || ally->isOut() || ally->getFaction() != FACTION_PLAYER)
					{
						continue;
					}
					bool blocked = blocksReactionLane(unitPos, ally->getPosition(), unitReactionContact)
						|| (unitReactionContact != reactionContact
							&& blocksReactionLane(unitPos, ally->getPosition(), reactionContact));
					if (!blocked)
					{
						for (auto *visible : *_unit->getVisibleUnits())
						{
							if (visible && !visible->isOut() && visible->getFaction() == FACTION_HOSTILE
								&& blocksReactionLane(unitPos, ally->getPosition(), visible->getPosition()))
							{
								blocked = true;
								break;
							}
						}
					}
					if (blocked)
					{
						++count;
					}
				}
				return count;
			};
			auto reactionIncomingLaneCount = [&](const Position &unitPos) -> int
			{
				int count = 0;
				for (auto *shooter : *_save->getUnits())
				{
					if (!shooter || shooter == _unit || shooter->isOut() || shooter->getFaction() != FACTION_PLAYER)
					{
						continue;
					}
					BattleItem *weapon = shooter->getMainHandWeapon(false);
					Position shooterContact = reactionContact;
					BattleUnit *shooterAssignedTarget = _factionAI ? _factionAI->getAssignedTarget(shooter) : 0;
					if (shooterAssignedTarget && !shooterAssignedTarget->isOut()
						&& shooterAssignedTarget->getFaction() == FACTION_HOSTILE)
					{
						shooterContact = shooterAssignedTarget->getPosition();
					}
					if (!directReactionReady(shooter, weapon, shooter->getTimeUnits()))
					{
						continue;
					}
					bool blocked = (shooter->checkViewSector(shooterContact)
						&& blocksReactionLane(shooter->getPosition(), unitPos, shooterContact))
						|| (shooterContact != reactionContact && shooter->checkViewSector(reactionContact)
							&& blocksReactionLane(shooter->getPosition(), unitPos, reactionContact));
					if (!blocked)
					{
						for (auto *visible : *shooter->getVisibleUnits())
						{
							if (visible && !visible->isOut() && visible->getFaction() == FACTION_HOSTILE
								&& shooter->checkViewSector(visible->getPosition())
								&& blocksReactionLane(shooter->getPosition(), unitPos, visible->getPosition()))
							{
								blocked = true;
								break;
							}
						}
					}
					if (blocked)
					{
						++count;
					}
				}
				return count;
			};
			auto reactionFriendlyLaneRisk = [&](const Position &unitPos, int unitRemainingTU) -> int
			{
				// A unit firing through an ally is the primary hazard.  Packing it in
				// the high digits makes candidate selection lexicographic: first drive
				// outgoing blockers to zero, then avoid standing in another rifle lane.
				return reactionOutgoingLaneCount(unitPos, unitRemainingTU) * 1000
					+ reactionIncomingLaneCount(unitPos);
			};

			const Position currentPos = _unit->getPosition();
			const Position proposedPos = action->type == BA_WALK ? action->target : currentPos;
			int proposedMoveTU = 0;
			bool proposedPathReady = true;
			if (action->type == BA_WALK && proposedPos != currentPos)
			{
				_save->getPathfinding()->calculate(_unit, proposedPos, action->getMoveType());
				proposedPathReady = _save->getPathfinding()->getStartDirection() != -1;
				if (proposedPathReady)
				{
					proposedMoveTU = _save->getPathfinding()->getTotalTUCost();
				}
				_save->getPathfinding()->abortPath();
			}
			const int proposedRemainingTU = std::max(0, _unit->getTimeUnits() - proposedMoveTU);
			const int proposedLaneCount = proposedPathReady
				? reactionFriendlyLaneRisk(proposedPos, proposedRemainingTU)
				: 0;
			if (proposedLaneCount > 0)
			{
				const int proposedSpotters = getSpottingUnits(proposedPos);
				const int proposedFireLines = countEnemyFireLines(proposedPos);
				const int proposedExposure = getEnemyFireExposure(proposedPos);
				auto formationCoverAt = [&](const Position &pos) -> int
				{
					Tile *tile = _save->getTile(pos);
					if (!tile)
					{
						return 0;
					}
					return (tile->getMapData(O_OBJECT) ? 8 : 0)
						+ (tile->getMapData(O_NORTHWALL) ? 6 : 0)
						+ (tile->getMapData(O_WESTWALL) ? 6 : 0);
				};
				const int proposedCover = formationCoverAt(proposedPos);
				auto blastNeighboursAt = [&](const Position &pos) -> int
				{
					int neighbours = 0;
					if (!hostileAreaWeaponThreat)
					{
						return neighbours;
					}
					for (auto *ally : *_save->getUnits())
					{
						if (ally && ally != _unit && !ally->isOut() && ally->getFaction() == FACTION_PLAYER
							&& std::abs(ally->getPosition().z - pos.z) <= Options::battleExplosionHeight
							&& Position::distance2d(ally->getPosition(), pos) <= playerLocalHostileBlastRadius)
						{
							++neighbours;
						}
					}
					return neighbours;
				};
				const int proposedBlastNeighbours = blastNeighboursAt(proposedPos);
				auto safeFormationPath = [&](const std::vector<int> &path, const Position &target) -> bool
				{
					if (playerPathCrossesExplosiveDanger(_save, _unit, path, BAM_NORMAL, false))
					{
						return false;
					}
					Position pathPos = currentPos;
					int previousSpotters = getSpottingUnits(pathPos);
					int previousExposure = getEnemyFireExposure(pathPos);
					int previousFireLines = countEnemyFireLines(pathPos);
					const bool startedSafe = previousSpotters == 0 && previousExposure == 0 && previousFireLines == 0;
					for (auto direction = path.rbegin(); direction != path.rend(); ++direction)
					{
						const PathfindingStep step = _save->getPathfinding()->getTUCost(pathPos, *direction, _unit, 0, BAM_NORMAL);
						if (step.cost.time == Pathfinding::INVALID_MOVE_COST)
						{
							return false;
						}
						if (highHostileFirepower)
						{
							const int stepSpotters = getSpottingUnits(step.pos);
							const int stepExposure = getEnemyFireExposure(step.pos);
							const int stepFireLines = countEnemyFireLines(step.pos);
							const bool finalStep = step.pos == target;
							const bool worsensLane = startedSafe
								? (stepFireLines > 0 || (finalStep && (stepSpotters > 0 || stepExposure > 0)))
								: (stepFireLines > previousFireLines
									|| (finalStep && (stepSpotters > previousSpotters || stepExposure > previousExposure + 25)));
							if (worsensLane)
							{
								return false;
							}
							previousSpotters = stepSpotters;
							previousExposure = stepExposure;
							previousFireLines = stepFireLines;
						}
						pathPos = step.pos;
					}
					return pathPos == target;
				};

				BattleItem *ownReactionWeapon = action->weapon ? action->weapon : _unit->getMainHandWeapon(false);
				const bool preserveOwnReaction = directReactionReady(_unit, ownReactionWeapon, _unit->getTimeUnits());
				int ownReactionReserve = 0;
				if (preserveOwnReaction)
				{
					BattleActionCost ownSnapCost(BA_SNAPSHOT, _unit, ownReactionWeapon);
					ownReactionReserve = std::max(0, (int)ownSnapCost.Time);
				}
				Position bestFormationPos = proposedPos;
				int bestFormationLaneCount = proposedLaneCount;
				int bestFormationScore = PLAYER_AI_REJECT_SCORE;
				if (action->type == BA_WALK)
				{
					const int currentLaneCount = reactionFriendlyLaneRisk(currentPos, _unit->getTimeUnits());
					if (currentLaneCount < proposedLaneCount
						&& getSpottingUnits(currentPos) <= proposedSpotters
						&& countEnemyFireLines(currentPos) <= proposedFireLines
						&& getEnemyFireExposure(currentPos) <= proposedExposure
						&& blastNeighboursAt(currentPos) <= proposedBlastNeighbours)
					{
						bestFormationPos = currentPos;
						bestFormationLaneCount = currentLaneCount;
						bestFormationScore = (proposedLaneCount - currentLaneCount) * 1000
							- Position::distance2d(currentPos, proposedPos) * 40;
					}
				}
				for (int dx = -1; dx <= 1; ++dx)
				{
					for (int dy = -1; dy <= 1; ++dy)
					{
						if (dx == 0 && dy == 0)
						{
							continue;
						}
						const Position candidatePos = proposedPos + Position(dx, dy, 0);
						Tile *tile = _save->getTile(candidatePos);
						if (!tile || tile->getDangerous()
							|| (tile->getUnit() && tile->getUnit() != _unit)
							|| isPlayerExplosiveDanger(_save, _unit->getFaction(), candidatePos))
						{
							continue;
						}
						_save->getPathfinding()->calculate(_unit, candidatePos, BAM_NORMAL);
						if (_save->getPathfinding()->getStartDirection() == -1)
						{
							_save->getPathfinding()->abortPath();
							continue;
						}
						const int moveTU = _save->getPathfinding()->getTotalTUCost();
						const std::vector<int> candidatePath = _save->getPathfinding()->copyPath();
						_save->getPathfinding()->abortPath();
						if (moveTU > std::max(0, _unit->getTimeUnits() - ownReactionReserve)
							|| !safeFormationPath(candidatePath, candidatePos))
						{
							continue;
						}
						const int candidateSpotters = getSpottingUnits(candidatePos);
						const int candidateFireLines = countEnemyFireLines(candidatePos);
						const int candidateExposure = getEnemyFireExposure(candidatePos);
						if (candidateSpotters > proposedSpotters || candidateFireLines > proposedFireLines
							|| candidateExposure > proposedExposure
							|| blastNeighboursAt(candidatePos) > proposedBlastNeighbours)
						{
							continue;
						}
						const int candidateLaneCount = reactionFriendlyLaneRisk(
							candidatePos, std::max(0, _unit->getTimeUnits() - moveTU));
						if (candidateLaneCount >= proposedLaneCount)
						{
							continue;
						}
						const int cover = formationCoverAt(candidatePos);
						if (action->type == BA_NONE && proposedCover >= 8 && cover + 4 < proposedCover)
						{
							continue;
						}
						const int score = (proposedLaneCount - candidateLaneCount) * 1000
							- Position::distance2d(candidatePos, proposedPos) * 60
							- std::max(0, Position::distance2d(candidatePos, reactionContact)
								- Position::distance2d(proposedPos, reactionContact)) * 35
							- moveTU * 4 + cover * 5;
						if (score > bestFormationScore)
						{
							bestFormationScore = score;
							bestFormationPos = candidatePos;
							bestFormationLaneCount = candidateLaneCount;
						}
					}
				}
				if (bestFormationScore == PLAYER_AI_REJECT_SCORE && action->type == BA_WALK)
				{
					const int currentLaneCount = reactionFriendlyLaneRisk(currentPos, _unit->getTimeUnits());
					if (currentLaneCount < proposedLaneCount
						&& getSpottingUnits(currentPos) <= proposedSpotters
						&& countEnemyFireLines(currentPos) <= proposedFireLines
						&& getEnemyFireExposure(currentPos) <= proposedExposure
						&& blastNeighboursAt(currentPos) <= proposedBlastNeighbours)
					{
						bestFormationScore = 0;
						bestFormationPos = currentPos;
						bestFormationLaneCount = currentLaneCount;
					}
				}
				if (bestFormationScore != PLAYER_AI_REJECT_SCORE)
				{
					const Position originalTarget = action->target;
					action->type = bestFormationPos == currentPos ? BA_NONE : BA_WALK;
					action->target = bestFormationPos;
					action->run = false;
					action->strafe = false;
					action->sneak = false;
					action->finalAction = true;
					action->kneel = _unit->getArmor()->allowsKneeling(false);
					if (Options::autoBattleLog)
					{
						std::ostringstream log;
						log << "Player faction reaction friendly-fire formation adjustment: unit=" << _unit->getId()
							<< ", from=" << currentPos
							<< ", originalTarget=" << originalTarget
							<< ", newTarget=" << bestFormationPos
							<< ", contact=" << reactionContact
							<< ", riskyLanes=" << proposedLaneCount << "->" << bestFormationLaneCount
							<< ", outgoing=" << proposedLaneCount / 1000 << "->" << bestFormationLaneCount / 1000
							<< ", incoming=" << proposedLaneCount % 1000 << "->" << bestFormationLaneCount % 1000
							<< ", reason=stagger_ready_rifles_instead_of_leaving_ally_in_reaction_ray";
						_save->appendToAutoBattleLog(log.str());
					}
				}
				else if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction reaction friendly-fire formation unresolved: unit=" << _unit->getId()
						<< ", position=" << currentPos
						<< ", proposedTarget=" << proposedPos
						<< ", contact=" << reactionContact
						<< ", riskyLanes=" << proposedLaneCount
						<< ", outgoing=" << proposedLaneCount / 1000
						<< ", incoming=" << proposedLaneCount % 1000
						<< ", reason=no_strictly_safer_sidestep_without_worsening_enemy_fire";
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}

	if (_controlledProbeMoveAction && action->type == BA_WALK
		&& action->target != _unit->getPosition())
	{
		// The whole purpose of a bounded probe is to reveal one tile and think
		// again with the new visibility state.  No later formation/hold filter may
		// silently turn it into a final patrol action.
		action->finalAction = false;
		action->kneel = false;
	}

	if (action->type == BA_WALK)
	{
		// if we're moving, we'll have to re-evaluate our escape/ambush position.
		if (action->target != _unit->getPosition())
		{
			const bool exhausted = (_unit->getEnergy() <= 0);
			if (exhausted)
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction invalid walk suppressed: unit=" << _unit->getId()
						<< ", from=" << _unit->getPosition()
						<< ", target=" << action->target
						<< ", tu=" << _unit->getTimeUnits()
						<< ", energy=" << _unit->getEnergy()
						<< ", knownEnemies=" << _knownEnemies
						<< ", visibleEnemies=" << _visibleEnemies
						<< ", spottingEnemies=" << _spottingEnemies;
					_save->appendToAutoBattleLog(log.str());
				}
				action->type = BA_NONE;
				action->target = _unit->getPosition();
				action->finalAction = true;
				action->kneel = _unit->getArmor()->allowsKneeling(false);
			}
		}
		if (action->type == BA_WALK && action->target != _unit->getPosition())
		{
			_escapeTUs = 0;
			_ambushTUs = 0;
		}
		else
		{
			action->type = BA_NONE;
		}
	}
	if (_unit->getFaction() == FACTION_PLAYER && _factionSupportMoveAction)
	{
		Position pendingMineTarget;
		Position pendingMineContact;
		if (getPlayerProximityMineStaging(_save, _unit->getId(), &pendingMineTarget, &pendingMineContact)
			&& (action->type != BA_WALK || action->target != _attackAction.target))
		{
			clearPlayerProximityMineStaging(_save, _unit->getId());
			clearPlayerProximityMinePlan(_save, _unit->getFaction(), pendingMineContact, pendingMineTarget);
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction proximity mine staging cancelled: unit=" << _unit->getId()
					<< ", mineTarget=" << pendingMineTarget
					<< ", contact=" << pendingMineContact
					<< ", plannedMoveTarget=" << _attackAction.target
					<< ", resultingAction=" << (int)action->type
					<< ", resultingTarget=" << action->target
					<< ", reason=staging_route_was_clipped_or_cancelled";
				_save->appendToAutoBattleLog(log.str());
			}
		}
	}
}


/*
 * sets the "was hit" flag to true.
 */
void PlayerFactionAI::setWasHitBy(BattleUnit *attacker)
{
	if (attacker->getFaction() != _unit->getFaction() && !getWasHitBy(attacker->getId()))
		_wasHitBy.push_back(attacker->getId());
}

/*
 * Sets the "unit picked up a weapon" flag.
 */
void PlayerFactionAI::setWeaponPickedUp()
{
	_weaponPickedUp = true;
}

/*
 * Gets whether the unit was hit.
 * @return if it was hit.
 */
bool PlayerFactionAI::getWasHitBy(int attacker) const
{
	return std::find(_wasHitBy.begin(), _wasHitBy.end(), attacker) != _wasHitBy.end();
}
/*
 * Sets up a patrol action.
 * this is mainly going from node to node, moving about the map.
 * handles node selection, and fills out the _patrolAction with useful data.
 */
void PlayerFactionAI::setupPatrol()
{
	_patrolAction.clearTU();
	if (_toNode != 0 && _unit->getPosition() == _toNode->getPosition())
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Patrol destination reached!";
		}
		// destination reached
		// head off to next patrol node
		_fromNode = _toNode;
		freePatrolTarget();
		_toNode = 0;
		// take a peek through window before walking to the next node
		int dir = _save->getTileEngine()->faceWindow(_unit->getPosition());
		if (dir != -1 && dir != _unit->getDirection())
		{
			_unit->lookAt(dir);
			while (_unit->getStatus() == STATUS_TURNING)
			{
				_unit->turn();
			}
		}
	}

	if (_fromNode == 0)
	{
		// assume closest node as "from node"
		// on same level to avoid strange things, and the node has to match unit size or it will freeze
		int closest = 1000000;
		for (auto* node : *_save->getNodes())
		{
			if (node->isDummy())
			{
				continue;
			}
			int d = Position::distanceSq(_unit->getPosition(), node->getPosition());
			if (_unit->getPosition().z == node->getPosition().z
				&& d < closest
				&& (!(node->getType() & Node::TYPE_SMALL) || _unit->getArmor()->getSize() == 1))
			{
				_fromNode = node;
				closest = d;
			}
		}
	}
	int triesLeft = 5;

	while (_toNode == 0 && triesLeft)
	{
		triesLeft--;
		// look for a new node to walk towards
		bool scout = true;
		if (_save->getMissionType() != "STR_BASE_DEFENSE")
		{
			// after turn 20 or if the morale is low, everyone moves out the UFO and scout
			// also anyone standing in fire should also probably move
			if (_save->isCheating() || !_fromNode || _fromNode->getRank() == 0 ||
				(_save->getTile(_unit->getPosition()) && _save->getTile(_unit->getPosition())->getFire()))
			{
				scout = true;
			}
			else
			{
				scout = false;
			}
		}

		// in base defense missions, the smaller aliens walk towards target nodes - or if there, shoot objects around them
		else if (_unit->getArmor()->getSize() == 1 && _unit->getOriginalFaction() == FACTION_HOSTILE &&
				_attackAction.weapon &&
				_attackAction.weapon->getRules()->getAccuracySnap() &&
				!_attackAction.weapon->getRules()->getArcingShot() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT) &&
				!_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getArcingShot() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getDamageType()->isDirect() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getDamageType()->ToTile > 0.01f)
		{
			// can i shoot an object?
			if (_fromNode->isTarget() &&
				_save->canUseWeapon(_attackAction.weapon, _unit, false, BA_SNAPSHOT) &&
				_save->getModuleMap()[_fromNode->getPosition().x / 10][_fromNode->getPosition().y / 10].second > 0)
			{
				// scan this room for objects to destroy
				int x = (_unit->getPosition().x/10)*10;
				int y = (_unit->getPosition().y/10)*10;
				for (int i = x; i < x+9; i++)
				for (int j = y; j < y+9; j++)
				{
					MapData *md = _save->getTile(Position(i, j, 1))->getMapData(O_OBJECT);
					if (md && md->isBaseModule())
					{
						_patrolAction.actor = _unit;
						_patrolAction.target = Position(i, j, 1);
						_patrolAction.weapon = _attackAction.weapon;
						_patrolAction.type = BA_SNAPSHOT;
						_patrolAction.updateTU();
						_foundBaseModuleToDestroy = _save->getMod()->getAIDestroyBaseFacilities();
						return;
					}
				}
			}
			else
			{
				// find closest high value target which is not already allocated
				int closest = 1000000;
				BattleUnit* nodeunit = nullptr;
				for (auto* node : *_save->getNodes())
				{
					if (node->isDummy())
					{
						continue;
					}

					nodeunit = _save->getTile(node->getPosition())->getUnit();
					if (nodeunit && nodeunit->getFaction() == _unit->getFaction())
					{
						continue;
					}

					if (node->isTarget() && !node->isAllocated() && _save->getModuleMap()[node->getPosition().x / 10][node->getPosition().y / 10].second > 0)
					{
						int d = Position::distanceSq(_unit->getPosition(), node->getPosition());
						if (!_toNode ||  (d < closest && node != _fromNode))
						{
							_toNode = node;
							closest = d;
						}
					}
				}
			}
		}

		if (_toNode == 0)
		{
			_toNode = _save->getPatrolNode(scout, _unit, _fromNode);
			if (_toNode == 0)
			{
				_toNode = _save->getPatrolNode(!scout, _unit, _fromNode);
			}
		}

		if (_toNode != 0)
		{
			_save->getPathfinding()->calculate(_unit, _toNode->getPosition(), BAM_NORMAL);
			if (_save->getPathfinding()->getStartDirection() == -1)
			{
				_toNode = 0;
			}
			_save->getPathfinding()->abortPath();
		}
	}

	if (_toNode != 0)
	{
		_toNode->allocateNode();
		_patrolAction.actor = _unit;
		_patrolAction.type = BA_WALK;
		_patrolAction.target = _toNode->getPosition();
		if (_unit->getFaction() == FACTION_PLAYER && _knownEnemies == 0)
		{
			const Position originalTarget = _patrolAction.target;
			const Position currentPos = _unit->getPosition();
			const int maxScoutDistance = _save->getTurn() <= 2 ? 4 : 5;
			int bestScore = -100000;
			Position bestPos = currentPos;
			auto allyCrowdingPenalty = [&](const Position &pos) -> int
			{
				int penalty = 0;
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					const int dist = Position::distance2d(pos, other->getPosition());
					if (dist <= 1)
					{
						penalty += 45;
					}
					else if (dist <= 2)
					{
						penalty += 18;
					}
				}
				return penalty;
			};
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				Position pos = tile->getPosition();
				if (pos.z != currentPos.z || Position::distance2d(pos, currentPos) > maxScoutDistance)
				{
					continue;
				}
				const int moveDist = Position::distance2d(pos, currentPos);
				if (_knownEnemies && moveDist * 6 > std::max(0, _unit->getTimeUnits() - 20))
				{
					continue;
				}
				if (getSpottingUnits(pos) > 0)
				{
					continue;
				}
				int cover = 0;
				if (tile->getMapData(O_OBJECT))
				{
					cover += 8;
				}
				if (tile->getMapData(O_NORTHWALL))
				{
					cover += 6;
				}
				if (tile->getMapData(O_WESTWALL))
				{
					cover += 6;
				}
				const int score = 200 - Position::distance2d(pos, originalTarget) * 4 - moveDist * 2 + cover * 4 - allyCrowdingPenalty(pos);
				if (score > bestScore)
				{
					bestScore = score;
					bestPos = pos;
				}
			}
			if (bestPos != currentPos)
			{
				_patrolAction.target = bestPos;
				freePatrolTarget();
				_toNode = 0;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction cautious patrol: unit=" << _unit->getId()
						<< ", originalTarget=" << originalTarget
						<< ", limitedTarget=" << bestPos
						<< ", maxDistance=" << maxScoutDistance
						<< ", score=" << bestScore;
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}
	else
	{
		_patrolAction.type = BA_RETHINK;
	}
}

/**
 * Try to set up an ambush action
 * The idea is to check within a 11x11 tile square for a tile which is not seen by our aggroTarget,
 * but that can be reached by him. we then intuit where we will see the target first from our covered
 * position, and set that as our final facing.
 * Fills out the _ambushAction with useful data.
 */
void PlayerFactionAI::setupAmbush()
{
	_ambushAction.type = BA_RETHINK;
	int bestScore = 0;
	_ambushTUs = 0;
	std::vector<int> path;

	if (selectClosestKnownEnemy())
	{
		Position origin = _save->getTileEngine()->getSightOriginVoxel(_aggroTarget);

		// we'll use node positions for this, as it gives map makers a good degree of control over how the units will use the environment.
		for (const auto* node : *_save->getNodes())
		{
			if (node->isDummy())
			{
				continue;
			}
			Position pos = node->getPosition();
			Tile *tile = _save->getTile(pos);
			if (tile == 0 || Position::distance2d(pos, _unit->getPosition()) > 10 || pos.z != _unit->getPosition().z || tile->getDangerous() ||
				std::find(_reachableWithAttack.begin(), _reachableWithAttack.end(), _save->getTileIndex(pos))  == _reachableWithAttack.end())
				continue; // just ignore unreachable tiles

			if (_traceAI)
			{
				// colour all the nodes in range purple.
				tile->setPreview(10);
				tile->setMarkerColor(13);
			}

			// make sure we can't be seen here.
			Position target;
			if (!_save->getTileEngine()->canTargetUnit(&origin, tile, &target, _aggroTarget, false, _unit) && !getSpottingUnits(pos))
			{
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				int ambushTUs = _save->getPathfinding()->getTotalTUCost();
				// make sure we can move here
				if (_save->getPathfinding()->getStartDirection() != -1)
				{
					int score = PLAYER_AI_AMBUSH_BASE_SYSTEMATIC_SUCCESS;
					score -= ambushTUs;

					// make sure our enemy can reach here too.
					_save->getPathfinding()->calculate(_aggroTarget, pos, BAM_NORMAL);

					if (_save->getPathfinding()->getStartDirection() != -1)
					{
						// ideally we'd like to be behind some cover, like say a window or a low wall.
						if (_save->getTileEngine()->faceWindow(pos) != -1)
						{
							score += PLAYER_AI_AMBUSH_COVER_BONUS;
						}
						if (score > bestScore)
						{
							path = _save->getPathfinding()->copyPath();
							bestScore = score;
							_ambushTUs = (pos == _unit->getPosition()) ? 1 : ambushTUs;
							_ambushAction.target = pos;
							if (bestScore > PLAYER_AI_AMBUSH_FAST_PASS_THRESHOLD)
							{
								break;
							}
						}
					}
				}
			}
		}

		if (bestScore > 0)
		{
			_ambushAction.type = BA_WALK;
			// i should really make a function for this
			origin = _ambushAction.target.toVoxel() +
				// 4 because -2 is eyes and 2 below that is the rifle (or at least that's my understanding)
				Position(8,8, _unit->getHeight() + _unit->getFloatHeight() - _save->getTile(_ambushAction.target)->getTerrainLevel() - 4);
			Position currentPos = _aggroTarget->getPosition();
			_save->getPathfinding()->setUnit(_aggroTarget);
			size_t tries = path.size();
			// hypothetically walk the target through the path.
			while (tries > 0)
			{
				currentPos = _save->getPathfinding()->getTUCost(currentPos, path.back(), _aggroTarget, 0, BAM_NORMAL).pos;
				path.pop_back();
				Tile *tile = _save->getTile(currentPos);
				Position target;
				// do a virtual fire calculation
				if (_save->getTileEngine()->canTargetUnit(&origin, tile, &target, _unit, false, _aggroTarget))
				{
					// if we can virtually fire at the hypothetical target, we know which way to face.
					_ambushAction.finalFacing = _save->getTileEngine()->getDirectionTo(_ambushAction.target, currentPos);
					break;
				}
				--tries;
			}
			if (_traceAI)
			{
				Log(LOG_INFO) << "Ambush estimation will move to " << _ambushAction.target;
			}
			return;
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Ambush estimation failed";
	}
}

/**
 * Try to set up a combat action
 * This will either be a psionic, grenade, or weapon attack,
 * or potentially just moving to get a line of sight to a target.
 * Fills out the _attackAction with useful data.
 */
void PlayerFactionAI::setupAttack()
{
	_attackAction.type = BA_RETHINK;
	_psiAction.type = BA_NONE;

	bool sniperAttack = false;

	// if enemies are known to us but not necessarily visible, we can attack them with a blaster launcher or psi or a sniper attack.
	if (_knownEnemies)
	{
		if (psiAction())
		{
			// at this point we can save some time with other calculations - the unit WILL make a psionic attack this turn.
			return;
		}
		if (_blaster)
		{
			wayPointAction();
		}
		else if (_unit->getUnitRules()) // xcom soldiers (under mind control) lack unit rules!
		{
			// don't always act on spotter information unless modder says so
			if (RNG::percent(_unit->getUnitRules()->getSniperPercentage()))
			{
				sniperAttack = sniperAction();
			}
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER
		&& _grenade
		&& setupSmokeScreen())
	{
		return;
	}

	if (_unit->getFaction() == FACTION_PLAYER
		&& _grenade
		&& _factionAI
		&& !_visibleEnemies
		&& _knownEnemies > 0)
	{
		BattleUnit *assignedTarget = _factionAI->getAssignedTarget(_unit);
		if (assignedTarget && !assignedTarget->isOut() && assignedTarget->getTile()
			&& !isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), assignedTarget)
			&& validTarget(assignedTarget, true, true))
		{
			BattleUnit *savedAggro = _aggroTarget;
			BattleAction savedAction = _attackAction;
			_aggroTarget = assignedTarget;
			_attackAction.target = assignedTarget->getPosition();
			grenadeAction(80, true);
			if (_attackAction.type != BA_RETHINK)
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction hidden contact explosive attack: unit=" << _unit->getId()
						<< ", targetUnit=" << assignedTarget->getId()
						<< ", target=" << assignedTarget->getPosition()
						<< ", actionTarget=" << _attackAction.target
						<< ", weapon=" << (_attackAction.weapon ? _attackAction.weapon->getRules()->getType() : "none");
					_save->appendToAutoBattleLog(log.str());
				}
				return;
			}
			_aggroTarget = savedAggro;
			_attackAction = savedAction;
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER
		&& _grenade
		&& !_visibleEnemies
		&& !_spottingEnemies
		&& setupProximityMineAmbush())
	{
		return;
	}

	if (_unit->getFaction() == FACTION_PLAYER && _grenade && _factionAI && _knownEnemies)
	{
		BattleUnit *assignedTarget = _factionAI->getAssignedTarget(_unit);
		bool targetVisibleToFaction = false;
		if (assignedTarget && !assignedTarget->isOut() && assignedTarget->getTile() && validTarget(assignedTarget, true, true))
		{
			for (auto *ally : *_save->getUnits())
			{
				if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction())
				{
					continue;
				}
				if (_save->getTileEngine()->visible(ally, assignedTarget->getTile()))
				{
					targetVisibleToFaction = true;
					break;
				}
			}
		}
		int bestExplosivePower = 0;
		int bestExplosiveRadius = 0;
		const bool urgentPlayerExplosive = _unit->getFaction() == FACTION_PLAYER
			&& _knownEnemies >= 8
			&& (_visibleEnemies > 0 || _spottingEnemies > 0 || targetVisibleToFaction);
		for (auto *item : *_unit->getInventory())
		{
			if (!item || !item->getRules()->isGrenadeOrProxy() || item->getRules()->getBattleType() == BT_PROXIMITYGRENADE
				|| (!urgentPlayerExplosive && _save->getTurn() < item->getRules()->getAIUseDelay(_save->getMod())))
			{
				continue;
			}
			BattleAction explosiveProbe;
			explosiveProbe.actor = _unit;
			explosiveProbe.weapon = item;
			explosiveProbe.type = BA_THROW;
			const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(explosiveProbe));
			if (radius > 0 && item->getRules()->getPower() > bestExplosivePower)
			{
				bestExplosivePower = std::max(0, item->getRules()->getPower());
				bestExplosiveRadius = radius;
			}
		}
		int targetArmor = 0;
		int enemyWeaponDanger = 0;
		int nearbyTargets = 0;
		if (assignedTarget && assignedTarget->getBaseStats())
		{
			targetArmor = std::max(std::max(assignedTarget->getArmor(SIDE_FRONT), assignedTarget->getArmor(SIDE_LEFT)), assignedTarget->getArmor(SIDE_RIGHT));
			BattleItem *enemyWeapon = assignedTarget->getMainHandWeapon(false);
			enemyWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
			for (auto *enemy : *_save->getUnits())
			{
				if (enemy && !enemy->isOut() && enemy->getTile() && validTarget(enemy, true, true)
					&& abs(enemy->getPosition().z - assignedTarget->getPosition().z) <= Options::battleExplosionHeight
					&& Position::distance2d(enemy->getPosition(), assignedTarget->getPosition()) <= std::max(3, bestExplosiveRadius))
				{
					++nearbyTargets;
				}
			}
		}
		const bool teamGrenadeWorthy = assignedTarget
			&& !isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), assignedTarget)
			&& bestExplosivePower > 0
			&& (nearbyTargets >= 2
				|| assignedTarget->getHealth() >= std::max(65, bestExplosivePower / 2)
				|| targetArmor >= std::max(25, bestExplosivePower / 2)
				|| enemyWeaponDanger >= bestExplosivePower + 25);
		if (targetVisibleToFaction && teamGrenadeWorthy)
		{
			int teamGrenadeMinScore = PLAYER_AI_GRENADE_TEAM_MIN_SCORE_DEFAULT;
			if (nearbyTargets >= 2)
			{
				teamGrenadeMinScore = PLAYER_AI_GRENADE_TEAM_MIN_SCORE_CLUSTER;
			}
			else if (assignedTarget->getHealth() >= std::max(PLAYER_AI_GRENADE_HIGH_HEALTH, bestExplosivePower)
				|| targetArmor >= std::max(PLAYER_AI_DURABLE_ARMOR, bestExplosivePower * 3 / 4)
				|| enemyWeaponDanger >= bestExplosivePower + 55)
			{
				teamGrenadeMinScore = PLAYER_AI_GRENADE_TEAM_MIN_SCORE_HIGH;
			}
			else if (assignedTarget->getHealth() >= std::max(75, bestExplosivePower * 2 / 3)
				|| targetArmor >= std::max(PLAYER_AI_STRONG_ARMOR, bestExplosivePower / 2)
				|| enemyWeaponDanger >= bestExplosivePower + PLAYER_AI_WOUNDED_HEALTH_LIMIT)
			{
				teamGrenadeMinScore = PLAYER_AI_GRENADE_TEAM_MIN_SCORE_MEDIUM;
			}
			BattleUnit *savedAggro = _aggroTarget;
			BattleAction savedAction = _attackAction;
			_aggroTarget = assignedTarget;
			_attackAction.target = assignedTarget->getPosition();
			grenadeAction(teamGrenadeMinScore, true);
			if (_attackAction.type != BA_RETHINK)
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction team-spotted explosive attack: unit=" << _unit->getId()
						<< ", targetUnit=" << assignedTarget->getId()
						<< ", target=" << assignedTarget->getPosition()
						<< ", actionTarget=" << _attackAction.target
						<< ", weapon=" << (_attackAction.weapon ? _attackAction.weapon->getRules()->getType() : "none")
						<< ", targetHealth=" << assignedTarget->getHealth()
						<< ", targetArmor=" << targetArmor
						<< ", enemyWeaponDanger=" << enemyWeaponDanger
						<< ", nearbyTargets=" << nearbyTargets
						<< ", bestExplosivePower=" << bestExplosivePower
						<< ", minScore=" << teamGrenadeMinScore;
					_save->appendToAutoBattleLog(log.str());
				}
				return;
			}
			_aggroTarget = savedAggro;
			_attackAction = savedAction;
		}
	}

	// if we CAN see someone, that makes them a viable target for "regular" attacks.
	// This is skipped if sniperAction has already chosen an attack action
	const int visibleTargetCount = selectNearestTarget();
	if (!sniperAttack && (visibleTargetCount > 0 || _aggroTarget))
	{
		if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
		{
			BattleUnit *assignedTarget = _factionAI->getAssignedTarget(_unit);
			if (assignedTarget && !assignedTarget->isOut() && validTarget(assignedTarget, true, true)
				&& !isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), assignedTarget)
				&& assignedTarget->getTile() && _save->getTileEngine()->visible(_unit, assignedTarget->getTile()))
			{
				_aggroTarget = assignedTarget;
				_attackAction.target = assignedTarget->getPosition();
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction assigned visible target override: unit=" << _unit->getId()
						<< ", targetUnit=" << assignedTarget->getId()
						<< ", target=" << assignedTarget->getPosition()
						<< ", nearestWasReplaced=1";
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
		// if we have both types of weapon, make a determination on which to use.
		if (_melee && _rifle)
		{
			selectMeleeOrRanged();
		}
		if (_grenade)
		{
			grenadeAction();
		}
		if (_melee)
		{
			meleeAction();
		}
		if (_rifle)
		{
			projectileAction();
			if (_unit->getFaction() == FACTION_PLAYER && _attackAction.type == BA_RETHINK && _aggroTarget)
			{
				if (!setupCleanShotMove(_aggroTarget))
				{
					setupSharedCleanShotMove(_aggroTarget);
				}
			}
		}
	}
	else if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
	{
		BattleUnit *assignedTarget = _factionAI->getAssignedTarget(_unit);
		const PlayerFactionStrategy strategy = getFactionStrategy();
		const bool defensiveHiddenContact = strategy == PFS_SURVIVE || strategy == PFS_RETREAT_REGROUP || strategy == PFS_SKIRMISH;
		if (assignedTarget && !assignedTarget->isOut()
			&& !isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), assignedTarget)
			&& _rifle && (!defensiveHiddenContact || _visibleEnemies > 0 || _spottingEnemies > 0))
		{
			_aggroTarget = assignedTarget;
			if (!_attackAction.weapon)
			{
				_attackAction.weapon = selectBestCarriedWeapon();
			}
			if (!setupCleanShotMove(assignedTarget))
			{
				setupSharedCleanShotMove(assignedTarget);
			}
		}
		else if (_rifle)
		{
			setupSharedCleanShotMove();
		}
	}

	if (_attackAction.type != BA_RETHINK)
	{
		if (_traceAI)
		{
			if (_attackAction.type != BA_WALK)
			{
				Log(LOG_INFO) << "Attack estimation desires to shoot at " << _attackAction.target;
			}
			else
			{
				Log(LOG_INFO) << "Attack estimation desires to move to " << _attackAction.target;
			}
		}
		return;
	}
	else if (_unit->getFaction() == FACTION_PLAYER
		&& _grenade
		&& setupSmokeScreen())
	{
		return;
	}
	else if (_unit->getFaction() == FACTION_PLAYER
		&& _grenade
		&& !_spottingEnemies
		&& setupProximityMineAmbush())
	{
		return;
	}
	else if (_spottingEnemies || _unit->getAggression() < RNG::generate(0, 3))
	{
		// if enemies can see us, or if we're feeling lucky, we can try to spot the enemy.
		if (findFirePoint())
		{
			if (_traceAI)
			{
				Log(LOG_INFO) << "Attack estimation desires to move to " << _attackAction.target;
			}
			return;
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Attack estimation failed";
	}
}

bool PlayerFactionAI::setupFactionStalkAmbush(const Position &contactPos, const BattleRoomInfo *contactRoom, int enemiesInRoom, BattleItem *weapon)
{
	if (!_factionAI || _reachable.empty())
	{
		return false;
	}
	BattleItem *reactionWeapon = weapon ? weapon : selectBestCarriedWeapon();
	const PlayerAIRole role = getPlayerAIRole(reactionWeapon);
	const PlayerAITacticalRole tacticalRole = getPlayerTacticalRole(reactionWeapon, role);
	const UnitStats *stats = _unit->getBaseStats();
	const bool canReactionShoot = reactionWeapon
		&& reactionWeapon->getRules()->getBattleType() == BT_FIREARM
		&& _save->canUseWeapon(reactionWeapon, _unit, false, BA_SNAPSHOT)
		&& reactionWeapon->getAmmoForAction(BA_SNAPSHOT);
	const int snapTU = canReactionShoot ? (int)BattleActionCost(BA_SNAPSHOT, _unit, reactionWeapon).Time : 0;
	const bool reactionSpecialist = canReactionShoot
		&& stats->reactions >= PLAYER_AI_REACTION_SPECIALIST_MIN_REACTIONS
		&& stats->tu >= PLAYER_AI_REACTION_SPECIALIST_MIN_TU
		&& stats->reactions + 8 >= stats->firing
		&& role != ROLE_HEAVY;
	const int currentDist = Position::distance2d(_unit->getPosition(), contactPos);
	const int roomSize = contactRoom ? contactRoom->tileCount : 1;
	const int roomEntries = contactRoom ? (int)contactRoom->entryPositions.size() : 0;
	const bool contactHasControlledEntry = contactRoom && (roomEntries > 0 || contactRoom->doorCount + contactRoom->windowCount > 0);
	const bool brokenRoom = contactRoom && !contactRoom->isOutside && !contactRoom->isHall
		&& (contactRoom->tileCount > PLAYER_AI_STALK_BROKEN_ROOM_TILE_LIMIT || (!contactHasControlledEntry && contactRoom->openingCount > contactRoom->tileCount * 2) || (roomEntries == 0 && contactRoom->doorCount + contactRoom->windowCount == 0));
	const bool smallDangerRoom = contactRoom && !contactRoom->isOutside && !contactRoom->isHall && !brokenRoom;
	const bool riskyRoom = smallDangerRoom && (enemiesInRoom > 1 || contactRoom->tileCount > PLAYER_AI_STALK_RISKY_ROOM_TILE_LIMIT || contactRoom->doorCount + contactRoom->windowCount <= 2);
	const bool rangedAmbush = _rifle || _blaster || (_grenade && !_melee);
	const int minDoorDist = riskyRoom
		? (reactionSpecialist || tacticalRole == TACTICAL_REACTION_GUARD ? 1 : (tacticalRole == TACTICAL_FIRE_SUPPORT ? 4 : (rangedAmbush ? 2 : 0)))
		: (rangedAmbush ? 2 : 0);
	const int maxDoorDist = riskyRoom
		? (reactionSpecialist || tacticalRole == TACTICAL_REACTION_GUARD ? 4 : (tacticalRole == TACTICAL_FIRE_SUPPORT ? 10 : 7))
		: 7;
	const int desiredDist = riskyRoom
		? (reactionSpecialist || tacticalRole == TACTICAL_REACTION_GUARD ? 2 : (tacticalRole == TACTICAL_FIRE_SUPPORT ? 7 : (rangedAmbush ? 3 : 1)))
		: (contactRoom && contactRoom->isHall ? (tacticalRole == TACTICAL_FIRE_SUPPORT ? 11 : 6) : (enemiesInRoom > 1 ? (tacticalRole == TACTICAL_FIRE_SUPPORT ? 12 : 7) : (tacticalRole == TACTICAL_FIRE_SUPPORT ? 10 : 5)));
	const int roomPressure = enemiesInRoom * 20 + (roomSize > PLAYER_AI_STALK_HALL_ROOM_SIZE_BONUS_LIMIT ? 10 : 0);
	if (!riskyRoom && currentDist <= desiredDist + 5)
	{
		return false;
	}
	int bestScore = PLAYER_AI_REJECT_SCORE;
	Position bestPos = _unit->getPosition();
	Position bestFace = contactPos;
	int bestReactionScore = 0;
	int bestMoveTU = 0;

	auto nearestEntryTo = [&](const Position &pos, Position *entry) -> int
	{
		int bestDist = Position::distance2d(pos, contactPos);
		Position bestEntry = contactPos;
		if (contactRoom)
		{
			for (const auto &candidate : contactRoom->entryPositions)
			{
				if (candidate.z != pos.z)
				{
					continue;
				}
				const int dist = Position::distance2d(pos, candidate);
				if (dist < bestDist)
				{
					bestDist = dist;
					bestEntry = candidate;
				}
			}
		}
		if (entry)
		{
			*entry = bestEntry;
		}
		return bestDist;
	};

	auto coverScoreAt = [&](const Position &pos) -> int
	{
		int score = 0;
		Tile *tile = _save->getTile(pos);
		if (!tile)
		{
			return 0;
		}
		if (tile->getMapData(O_OBJECT))
		{
			score += 8;
		}
		if (tile->getMapData(O_NORTHWALL))
		{
			score += contactPos.y < pos.y ? 16 : 6;
		}
		if (tile->getMapData(O_WESTWALL))
		{
			score += contactPos.x < pos.x ? 16 : 6;
		}
		const Position adjacent[4] = { Position(0, -1, 0), Position(1, 0, 0), Position(0, 1, 0), Position(-1, 0, 0) };
		for (const auto &offset : adjacent)
		{
			Tile *adjacentTile = _save->getTile(pos + offset);
			if (!adjacentTile)
			{
				continue;
			}
			if (adjacentTile->getMapData(O_OBJECT))
			{
				score += 4;
			}
			if (adjacentTile->getMapData(O_NORTHWALL) || adjacentTile->getMapData(O_WESTWALL))
			{
				score += 3;
			}
		}
		return score;
	};

	for (auto tileIndex : _reachable)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		Position pos = tile->getPosition();
		if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
		{
			continue;
		}
		if (pos.z != _unit->getPosition().z)
		{
			continue;
		}
		if (riskyRoom && contactRoom && _factionAI->getRoomIdAt(pos) == contactRoom->id)
		{
			continue;
		}
		_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
		if (pos != _unit->getPosition() && _save->getPathfinding()->getStartDirection() == -1)
		{
			_save->getPathfinding()->abortPath();
			continue;
		}
		const int moveTU = pos == _unit->getPosition() ? 0 : _save->getPathfinding()->getTotalTUCost();
		_save->getPathfinding()->abortPath();
		if (canReactionShoot && moveTU + snapTU > _unit->getTimeUnits())
		{
			continue;
		}
		int dist = Position::distance2d(pos, contactPos);
		Position entryPos;
		const int entryDist = nearestEntryTo(pos, &entryPos);
		const int currentEntryDist = nearestEntryTo(_unit->getPosition(), 0);
		if (riskyRoom)
		{
			if (entryDist < minDoorDist || entryDist > maxDoorDist || entryDist > currentEntryDist + 2)
			{
				continue;
			}
		}
		else if (dist < (brokenRoom ? 6 : 2) || dist > currentDist + (brokenRoom ? 0 : -1) || dist > desiredDist + (brokenRoom ? 4 : 8))
		{
			continue;
		}
		int spotters = getSpottingUnits(pos);
		int fireLines = countEnemyFireLines(pos);
		int cover = coverScoreAt(pos);
		if (cover < (riskyRoom ? 12 : 20))
		{
			continue;
		}
		if (fireLines > 0 && tacticalRole != TACTICAL_SCOUT && cover < 24)
		{
			continue;
		}
		const int remainingTU = std::max(0, _unit->getTimeUnits() - moveTU);
		const int reactionScore = _unit->getBaseStats()->tu > 0
			? (int)(_unit->getBaseStats()->reactions * remainingTU / _unit->getBaseStats()->tu)
			: 0;
		const int minReactionScore = riskyRoom
			? (reactionSpecialist ? 30 : (role == ROLE_MARKSMAN ? 24 : 22))
			: (enemiesInRoom > 1 ? (role == ROLE_MARKSMAN ? 26 : 32) : (role == ROLE_MARKSMAN ? 24 : 26));
		if (canReactionShoot && reactionScore < minReactionScore)
		{
			continue;
		}
		int score = 100;
		score -= abs((riskyRoom ? entryDist : dist) - desiredDist) * 8;
		score -= spotters * (35 + roomPressure);
		score -= fireLines * (tacticalRole == TACTICAL_FIRE_SUPPORT ? 95 : 60);
		score += cover;
		score += std::min(80, reactionScore * 2);
		if (tacticalRole == TACTICAL_FIRE_SUPPORT)
		{
			score += std::min(60, dist * 4);
			if (riskyRoom)
			{
				score += entryDist * 6;
			}
		}
		if (reactionSpecialist || tacticalRole == TACTICAL_REACTION_GUARD)
		{
			score += std::max(0, 45 - entryDist * 8);
			score += std::min(45, reactionScore);
		}
		if (tacticalRole == TACTICAL_SCOUT)
		{
			score -= std::max(0, dist - desiredDist) * 4;
			score += std::max(0, 45 - moveTU);
		}
		if (canReactionShoot)
		{
			score += 35;
			score -= std::max(0, snapTU - remainingTU / 2);
		}
		score -= moveTU / 2;
		if (pos == _unit->getPosition() && canReactionShoot)
		{
			score += 25;
		}
		if (riskyRoom)
		{
			score += std::max(0, 45 - entryDist * 8);
			score += std::min(35, (currentEntryDist - entryDist) * 4);
			if (contactRoom && contactRoom->entryPositions.size() > 1)
			{
				score += 15;
			}
		}
		else if (dist < currentDist)
		{
			score += std::min(30, (currentDist - dist) * 3);
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestFace = riskyRoom ? (pos == entryPos ? contactPos : entryPos) : contactPos;
			bestReactionScore = reactionScore;
			bestMoveTU = moveTU;
		}
	}

	if (bestScore < (riskyRoom ? PLAYER_AI_STALK_RISKY_MIN_SCORE : PLAYER_AI_STALK_NORMAL_MIN_SCORE) || bestPos == _unit->getPosition())
	{
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = weapon;
	_attackAction.target = bestPos;
	_attackAction.type = BA_WALK;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, bestFace);
	_stalkAmbushAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction stalk ambush: unit=" << _unit->getId()
			<< ", contact=" << contactPos
			<< ", target=" << bestPos
			<< ", score=" << bestScore
			<< ", roomEnemies=" << enemiesInRoom
			<< ", roomSize=" << roomSize
			<< ", roomDoors=" << (contactRoom ? contactRoom->doorCount : 0)
			<< ", roomWindows=" << (contactRoom ? contactRoom->windowCount : 0)
		<< ", roomEntries=" << roomEntries
		<< ", roomHall=" << (contactRoom ? contactRoom->isHall : false)
		<< ", brokenRoom=" << brokenRoom
		<< ", role=" << (int)role
		<< ", tactical=" << getPlayerTacticalRoleName(tacticalRole)
		<< ", reactionSpecialist=" << reactionSpecialist
		<< ", minDoorDist=" << minDoorDist
		<< ", maxDoorDist=" << maxDoorDist
		<< ", riskyRoom=" << riskyRoom
		<< ", reactionWeapon=" << (reactionWeapon ? reactionWeapon->getRules()->getType() : "none")
		<< ", snapTU=" << snapTU
		<< ", moveTU=" << bestMoveTU
		<< ", reactionScore=" << bestReactionScore
		<< ", facing=" << bestFace;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupHiddenExplosiveStaging(BattleUnit *target)
{
	if (_unit->getFaction() != FACTION_PLAYER || !target || target->isOut() || !_factionAI || _reachable.empty()
		|| target->getPosition().z != _unit->getPosition().z || getPlayerExplosiveThrowerRank(_unit, _save) >= 6)
	{
		return false;
	}

	BattleItem *explosive = 0;
	int bestPower = 0;
	for (auto *item : *_unit->getInventory())
	{
		if (!item || !item->getRules() || !item->getRules()->isGrenadeOrProxy()
			|| item->getRules()->getBattleType() == BT_PROXIMITYGRENADE || isPlayerSmokeGrenade(item))
		{
			continue;
		}
		if (item->getRules()->getPower() > bestPower)
		{
			bestPower = item->getRules()->getPower();
			explosive = item;
		}
	}
	if (!explosive)
	{
		return false;
	}

	const Position current = _unit->getPosition();
	const int currentDist = Position::distance2d(current, target->getPosition());
	if (currentDist <= 10)
	{
		return false;
	}
	BattleAction throwAction;
	throwAction.actor = _unit;
	throwAction.weapon = explosive;
	throwAction.type = BA_THROW;
	throwAction.target = target->getPosition();
	throwAction.updateTU();
	throwAction.Time += PLAYER_AI_PICKUP_TU_COST;
	throwAction += _unit->getActionTUs(BA_PRIME, explosive);
	const int reserveTU = std::min(_unit->getTimeUnits() - 4, std::max(18, (int)throwAction.Time));
	if (reserveTU <= 0 || _unit->getTimeUnits() - reserveTU < 4)
	{
		return false;
	}

	const int currentExposure = getEnemyFireExposure(current);
	const int currentFireLines = countEnemyFireLines(current);
	Position bestPos = current;
	int bestScore = PLAYER_AI_REJECT_SCORE;
	int bestMoveTU = 0;
	int bestDist = currentDist;
	for (auto tileIndex : _reachable)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		const Position pos = tile->getPosition();
		if (pos == current || pos.z != current.z || isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
		{
			continue;
		}
		const int dist = Position::distance2d(pos, target->getPosition());
		if (dist >= currentDist - 2 || dist <= 7)
		{
			continue;
		}
		_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL, 0, std::max(0, _unit->getTimeUnits() - reserveTU));
		if (_save->getPathfinding()->getStartDirection() == -1)
		{
			_save->getPathfinding()->abortPath();
			continue;
		}
		const int moveTU = _save->getPathfinding()->getTotalTUCost();
		_save->getPathfinding()->abortPath();
		const int spotters = getSpottingUnits(pos);
		const int exposure = getEnemyFireExposure(pos);
		const int fireLines = countEnemyFireLines(pos);
		if (spotters > 0 || fireLines > currentFireLines || exposure > std::max(70, currentExposure + 25))
		{
			continue;
		}
		int cover = 0;
		if (tile->getMapData(O_OBJECT)) cover += 8;
		if (tile->getMapData(O_NORTHWALL)) cover += 6;
		if (tile->getMapData(O_WESTWALL)) cover += 6;
		int score = (currentDist - dist) * 55 + cover * 7;
		score -= moveTU * 3;
		score -= exposure;
		score -= fireLines * 60;
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestMoveTU = moveTU;
			bestDist = dist;
		}
	}
	if (bestPos == current || bestScore < 80)
	{
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = selectBestCarriedWeapon();
	_attackAction.type = BA_WALK;
	_attackAction.target = bestPos;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, target->getPosition());
	_AIMode = AI_COMBAT;
	_factionSupportMoveAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction hidden explosive staging: unit=" << _unit->getId()
			<< ", targetUnit=" << target->getId()
			<< ", target=" << target->getPosition()
			<< ", moveTarget=" << bestPos
			<< ", distance=" << currentDist << "->" << bestDist
			<< ", moveTU=" << bestMoveTU
			<< ", reserveTU=" << reserveTU
			<< ", score=" << bestScore;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Attempts to find cover, and move toward it.
 * The idea is to check within a 11x11 tile square for a tile which is not seen by our aggroTarget.
 * If there is no such tile, we run away from the target.
 * Fills out the _escapeAction with useful data.
 */
void PlayerFactionAI::setupEscape()
{
	int unitsSpottingMe = getSpottingUnits(_unit->getPosition());
	Tile *currentTile = _save->getTile(_unit->getPosition());
	const bool explosiveEvacuation = _unit->getFaction() == FACTION_PLAYER
		&& ((currentTile && currentTile->getDangerous()) || isPlayerExplosiveDanger(_save, _unit->getFaction(), _unit->getPosition()));
	int currentTilePreference = 15;
	int tries = -1;
	bool coverFound = false;
	selectNearestTarget();
	_escapeTUs = 0;

	int dist = _aggroTarget ? Position::distance2d(_unit->getPosition(), _aggroTarget->getPosition()) : 0;

	int bestTileScore = -100000;
	int score = -100000;
	Position bestTile(0, 0, 0);
	bool run = false;

	Tile *tile = 0;

	// weights of various factors in choosing a tile to which to withdraw
	std::vector<Position> randomTileSearch = _save->getTileSearch();
	RNG::shuffle(randomTileSearch);

	while (tries < 150 && !coverFound)
	{
		_escapeAction.target = _unit->getPosition(); // start looking in a direction away from the enemy
		_escapeAction.run = _unit->getArmor()->allowsRunning(false) && (tries & 1); // every odd try, i.e. roughly 50%

		if (!_save->getTile(_escapeAction.target))
		{
			_escapeAction.target = _unit->getPosition(); // cornered at the edge of the map perhaps?
		}

		score = 0;

		if (tries == -1)
		{
			// you know, maybe we should just stay where we are and not risk reaction fire...
			// or maybe continue to wherever we were running to and not risk looking stupid
			if (_save->getTile(_unit->lastCover) != 0)
			{
				_escapeAction.target = _unit->lastCover;
			}
		}
		else if (tries < 121)
		{
			// looking for cover
			_escapeAction.target.x += randomTileSearch[tries].x;
			_escapeAction.target.y += randomTileSearch[tries].y;
			score = PLAYER_AI_ESCAPE_BASE_SYSTEMATIC_SUCCESS;
			if (_escapeAction.target == _unit->getPosition())
			{
				if (unitsSpottingMe > 0)
				{
					// maybe don't stay in the same spot? move or something if there's any point to it?
					_escapeAction.target.x += RNG::generate(-20,20);
					_escapeAction.target.y += RNG::generate(-20,20);
				}
				else
				{
					score += currentTilePreference;
				}
			}
		}
		else
		{
			if (tries == 121)
			{
				if (_traceAI)
				{
					Log(LOG_INFO) << "best score after systematic search was: " << bestTileScore;
				}
			}

			score = PLAYER_AI_ESCAPE_BASE_DESPERATE_SUCCESS; // ruuuuuuun
			_escapeAction.target = _unit->getPosition();
			_escapeAction.target.x += RNG::generate(-10,10);
			_escapeAction.target.y += RNG::generate(-10,10);
			_escapeAction.target.z = _unit->getPosition().z + RNG::generate(-1,1);
			if (_escapeAction.target.z < 0)
			{
				_escapeAction.target.z = 0;
			}
			else if (_escapeAction.target.z >= _save->getMapSizeZ())
			{
				_escapeAction.target.z = _unit->getPosition().z;
			}
		}

		tries++;

		// THINK, DAMN YOU
		tile = _save->getTile(_escapeAction.target);
		int distanceFromTarget = _aggroTarget ? Position::distance2d(_aggroTarget->getPosition(), _escapeAction.target) : 0;
		if (dist >= distanceFromTarget)
		{
			score -= (distanceFromTarget - dist) * 10;
		}
		else
		{
			score += (distanceFromTarget - dist) * 10;
		}
		int spotters = 0;
		if (!tile)
		{
			score = -100001; // no you can't quit the battlefield by running off the map.
		}
		else
		{
			spotters = getSpottingUnits(_escapeAction.target);
			if (std::find(_reachable.begin(), _reachable.end(), _save->getTileIndex(_escapeAction.target))  == _reachable.end())
				continue; // just ignore unreachable tiles
			if (explosiveEvacuation && (tile->getDangerous() || isPlayerExplosiveDanger(_save, _unit->getFaction(), _escapeAction.target)))
			{
				continue;
			}

			if (_spottingEnemies || spotters)
			{
				if (_spottingEnemies <= spotters)
				{
					score -= (1 + spotters - _spottingEnemies) * PLAYER_AI_ESCAPE_EXPOSURE_PENALTY; // that's for giving away our position
				}
				else
				{
					score += (_spottingEnemies - spotters) * PLAYER_AI_ESCAPE_EXPOSURE_PENALTY;
				}
			}
			if (tile->getFire())
			{
				score -= PLAYER_AI_ESCAPE_FIRE_PENALTY;
			}
			if (tile->getDangerous())
			{
				score -= PLAYER_AI_ESCAPE_BASE_SYSTEMATIC_SUCCESS;
			}
			if (_unit->getFaction() == FACTION_PLAYER && isPlayerExplosiveDanger(_save, _unit->getFaction(), _escapeAction.target))
			{
				score -= PLAYER_AI_ESCAPE_BASE_SYSTEMATIC_SUCCESS * 3;
			}

			if (_traceAI)
			{
				tile->setMarkerColor(score < 0 ? 3 : (score < PLAYER_AI_ESCAPE_FAST_PASS_THRESHOLD/2 ? 8 : (score < PLAYER_AI_ESCAPE_FAST_PASS_THRESHOLD ? 9 : 5)));
				tile->setPreview(10);
				tile->setTUMarker(score);
			}

		}

		if (tile && score > bestTileScore)
		{
			// calculate TUs to tile; we could be getting this from findReachable() somehow but that would break something for sure...
			_save->getPathfinding()->calculate(_unit, _escapeAction.target, _escapeAction.getMoveType());
			if (_escapeAction.target == _unit->getPosition() || _save->getPathfinding()->getStartDirection() != -1)
			{
				const bool riskyPath = _unit->getFaction() == FACTION_PLAYER
					&& playerPathCrossesExplosiveDanger(_save, _unit, _save->getPathfinding()->copyPath(), _escapeAction.getMoveType(), explosiveEvacuation);
				if (riskyPath)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				bestTileScore = score;
				bestTile = _escapeAction.target;
				run = _escapeAction.run;
				_escapeTUs = _save->getPathfinding()->getTotalTUCost();
				if (_escapeAction.target == _unit->getPosition())
				{
					_escapeTUs = 1;
				}
				if (_traceAI)
				{
					tile->setMarkerColor(score < 0 ? 7 : (score < PLAYER_AI_ESCAPE_FAST_PASS_THRESHOLD/2 ? 10 : (score < PLAYER_AI_ESCAPE_FAST_PASS_THRESHOLD ? 4 : 5)));
					tile->setPreview(10);
					tile->setTUMarker(score);
				}
			}
			_save->getPathfinding()->abortPath();
			if (bestTileScore > PLAYER_AI_ESCAPE_FAST_PASS_THRESHOLD) coverFound = true; // good enough, gogogo
		}
	}
	_escapeAction.target = bestTile;
	_escapeAction.run = run;
	if (_traceAI)
	{
		_save->getTile(_escapeAction.target)->setMarkerColor(13);
	}

	if (bestTileScore <= -100000)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Escape estimation failed.";
		}
		_escapeAction.type = BA_RETHINK; // do something, just don't look dumbstruck :P
		return;
	}
	else
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Escape estimation completed after " << tries << " tries, " << Position::distance2d(_unit->getPosition(), bestTile) << " squares or so away.";
		}
		_escapeAction.type = BA_WALK;
	}
}

/**
 * Counts how many targets, both xcom and civilian are known to this unit
 * @return how many targets are known to us.
 */
int PlayerFactionAI::countKnownTargets() const
{
	int knownEnemies = 0;

	if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
	{
		int activeHostiles = 0;
		for (auto *bu : *_save->getUnits())
		{
			if (bu && !bu->isOut() && bu->getFaction() == FACTION_HOSTILE)
			{
				++activeHostiles;
			}
		}
		return std::max(activeHostiles, _factionAI->getEnemyContactCount());
	}

	if (_unit->getFaction() == FACTION_HOSTILE)
	{
		for (auto* bu : *_save->getUnits())
		{
			if (validTarget(bu, true, true))
			{
				++knownEnemies;
			}
		}
	}
	return knownEnemies;
}

/*
 * counts how many enemies (xcom only) are spotting any given position.
 * @param pos the Position to check for spotters.
 * @return spotters.
 */
int PlayerFactionAI::getSpottingUnits(const Position& pos) const
{
	// if we don't actually occupy the position being checked, we need to do a virtual LOF check.
	bool checking = pos != _unit->getPosition();
	int tally = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, false, false))
		{
			int dist = Position::distance2d(pos, bu->getPosition());
			if (dist > 20) continue;
			Position originVoxel = _save->getTileEngine()->getSightOriginVoxel(bu);
			originVoxel.z -= 2;
			Position targetVoxel;
			if (checking)
			{
				if (playerAIHasVirtualLineToPosition(_save, _unit, bu, pos, originVoxel))
				{
					tally++;
				}
			}
			else
			{
				if (_save->getTileEngine()->canTargetUnit(&originVoxel, _save->getTile(pos), &targetVoxel, bu, false))
				{
					tally++;
				}
			}
		}
	}
	return tally;
}

int PlayerFactionAI::getEnemyFireExposure(const Position& pos) const
{
	Tile *tile = _save->getTile(pos);
	if (!tile)
	{
		return 1000;
	}
	int exposure = 0;
	for (auto *enemy : *_save->getUnits())
	{
		if (!enemy || enemy->isOut() || enemy->getFaction() == _unit->getFaction())
		{
			continue;
		}
		BattleItem *weapon = enemy->getMainHandWeapon(false);
		if (!weapon || weapon->getRules()->getBattleType() != BT_FIREARM || !_save->canUseWeapon(weapon, enemy, false, BA_SNAPSHOT))
		{
			continue;
		}
		const int dist = Position::distance2d(pos, enemy->getPosition());
		if (dist > 24)
		{
			continue;
		}
		BattleAction action;
		action.actor = enemy;
		action.weapon = weapon;
		action.target = pos;
		Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
		Position targetVoxel;
		const bool virtualPosition = _unit->getFaction() == FACTION_PLAYER && pos != _unit->getPosition();
		const bool canFire = virtualPosition
			? playerAIHasVirtualLineToPosition(_save, _unit, enemy, pos, origin)
			: _save->getTileEngine()->canTargetUnit(&origin, tile, &targetVoxel, enemy, false);
		if (canFire)
		{
			exposure += 30 + std::max(0, weapon->getRules()->getPower()) / 3 + std::max(0, weapon->getRules()->getAccuracySnap()) / 5;
		}
	}
	return exposure;
}

int PlayerFactionAI::countEnemyFireLines(const Position& pos) const
{
	Tile *tile = _save->getTile(pos);
	if (!tile)
	{
		return 99;
	}
	int lines = 0;
	for (auto *enemy : *_save->getUnits())
	{
		if (!enemy || enemy->isOut() || enemy->getFaction() == _unit->getFaction())
		{
			continue;
		}
		BattleItem *weapon = enemy->getMainHandWeapon(false);
		if (!weapon || weapon->getRules()->getBattleType() != BT_FIREARM || !_save->canUseWeapon(weapon, enemy, false, BA_SNAPSHOT))
		{
			continue;
		}
		const int dist = Position::distance2d(pos, enemy->getPosition());
		if (dist > 24)
		{
			continue;
		}
		BattleActionCost cost(BA_SNAPSHOT, enemy, weapon);
		if (!cost.haveTU() || weapon->getRules()->isOutOfRange(dist * dist))
		{
			continue;
		}
		BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(cost);
		int blastRadius = 0;
		if (attack.damage_item)
		{
			blastRadius = attack.damage_item->getRules()->getExplosionRadius(attack);
		}
		BattleAction action;
		action.actor = enemy;
		action.weapon = weapon;
		action.target = pos;
		Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
		Position targetVoxel;
		const bool virtualPosition = _unit->getFaction() == FACTION_PLAYER && pos != _unit->getPosition();
		const bool canFire = virtualPosition
			? playerAIHasVirtualLineToPosition(_save, _unit, enemy, pos, origin)
			: _save->getTileEngine()->canTargetUnit(&origin, tile, &targetVoxel, enemy, false);
		if (canFire)
		{
			lines += blastRadius > 0 ? std::max(1, blastRadius / 2) : 1;
			continue;
		}
		const bool arcingExplosive = blastRadius > 0
			&& (weapon->getArcingShot(BA_SNAPSHOT) || attack.damage_item->getRules()->getArcingShot())
			&& std::abs(pos.z - enemy->getPosition().z) <= 2
			&& dist <= 14 + blastRadius / 2;
		if (arcingExplosive)
		{
			++lines;
		}
	}
	return lines;
}

bool PlayerFactionAI::hasSafeRetreatFromFirePosition(const Position& firePos, int maxRetreatTU) const
{
	if (maxRetreatTU <= 0)
	{
		return false;
	}

	auto isSafeRetreatTile = [&](const Position &pos) -> bool
	{
		Tile *tile = _save->getTile(pos);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			return false;
		}
		if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
		{
			return false;
		}
		return countEnemyFireLines(pos) == 0;
	};

	const Position current = _unit->getPosition();
	if (current != firePos && Position::distance2d(current, firePos) <= 3 && Position::distance2d(current, firePos) * 6 <= maxRetreatTU && isSafeRetreatTile(current))
	{
		return true;
	}

	for (int dir = 0; dir < 8; ++dir)
	{
		PathfindingStep first = _save->getPathfinding()->getTUCost(firePos, dir, _unit, 0, BAM_NORMAL);
		if (first.cost.time >= Pathfinding::INVALID_MOVE_COST || first.cost.time > maxRetreatTU)
		{
			continue;
		}
		if (isSafeRetreatTile(first.pos))
		{
			return true;
		}
		const int remainingTU = maxRetreatTU - first.cost.time;
		for (int dir2 = 0; dir2 < 8; ++dir2)
		{
			PathfindingStep second = _save->getPathfinding()->getTUCost(first.pos, dir2, _unit, 0, BAM_NORMAL);
			if (second.cost.time >= Pathfinding::INVALID_MOVE_COST || second.cost.time > remainingTU)
			{
				continue;
			}
			if (isSafeRetreatTile(second.pos))
			{
				return true;
			}
		}
	}
	return false;
}

int PlayerFactionAI::scoreTargetPriority(BattleUnit *target, bool assigned, bool visible, int distance) const
{
	if (!target)
	{
		return -100000;
	}
	int score = 60;
	score -= distance * 4;
	if (assigned)
	{
		score += visible ? 120 : 35;
	}
	if (visible)
	{
		score += 45;
	}
	if (_unit->getFaction() == FACTION_PLAYER && target->getOriginalFaction() == FACTION_PLAYER)
	{
		return -100000;
	}
	if (target->getHealth() > 0)
	{
		score += std::max(0, 80 - target->getHealth()) * 2;
		if (target->getHealth() <= 35)
		{
			score += 120;
		}
	}
	score += target->getFatalWounds() * 20;
	score += std::max(0, target->getTimeUnits()) / 2;
	BattleItem *enemyWeapon = target->getMainHandWeapon(false);
	if (enemyWeapon && enemyWeapon->getRules())
	{
		const RuleItem *rule = enemyWeapon->getRules();
		score += playerAIWeaponDirectDanger(enemyWeapon);
		if (rule->getBattleType() == BT_MELEE)
		{
			score += distance <= 5 ? 90 : 25;
		}
	}
	if (_attackAction.weapon)
	{
		const int preferred = getPreferredEngagementRange(_attackAction.weapon);
		score -= abs(distance - preferred) * 3;
	}
	return score;
}

int PlayerFactionAI::estimateDirectShotDamage(BattleAction *action, BattleUnit *target, int accuracy, int shots) const
{
	if (!action || !action->weapon || !target || accuracy <= 0 || shots <= 0)
	{
		return 0;
	}
	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(*action);
	if (!attack.damage_item)
	{
		return 0;
	}
	if (attack.damage_item->getRules()->getExplosionRadius(attack) > 0)
	{
		return attack.damage_item->getRules()->getPower() * accuracy * shots / 100;
	}
	const RuleDamageType *damageType = attack.damage_item->getRules()->getDamageType();
	const int rawPower = std::max(0, attack.damage_item->getRules()->getPower());
	const int power = damageType ? target->reduceByResistance(rawPower, damageType->ResistType) : rawPower;
	const int rawArmor = std::max(std::max(target->getArmor(SIDE_FRONT), target->getArmor(SIDE_LEFT)), target->getArmor(SIDE_RIGHT));
	const int armor = damageType ? (int)std::ceil(rawArmor * damageType->ArmorEffectiveness) : rawArmor;
	const int baseDamage = std::max(0, power - armor / 2);
	const int chipDamage = power > armor ? std::max(1, (power - armor) / 3) : 0;
	const int expectedPerHit = std::max(baseDamage / 2, chipDamage);
	return expectedPerHit * accuracy * shots / 100;
}

/**
 * Selects the nearest known living target we can see/reach and returns the number of visible enemies.
 * This function includes civilians as viable targets.
 * @return viable targets.
 */
int PlayerFactionAI::selectNearestTarget()
{
	int tally = 0;
	_closestDist= 100;
	_aggroTarget = 0;
	Position target;
	BattleUnit *assignedTarget = _factionAI ? _factionAI->getAssignedTarget(_unit) : 0;
	int bestTargetScore = -100000;
	for (auto* bu : *_save->getUnits())
	{
		const bool assigned = assignedTarget == bu;
		const bool visible = bu && bu->getTile() && _save->getTileEngine()->visible(_unit, bu->getTile());
		const bool sharedPlayerContact = _unit->getFaction() == FACTION_PLAYER && _factionAI;
		if (validTarget(bu, true, true) && (visible || assigned || sharedPlayerContact))
		{
			if (_unit->getFaction() == FACTION_PLAYER && isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), bu))
			{
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction target reserved for timed blast: unit=" << _unit->getId()
						<< ", target=" << bu->getId()
						<< ", position=" << bu->getPosition();
					_save->appendToAutoBattleLog(log.str());
				}
				continue;
			}
			if (visible)
			{
				tally++;
			}
			int dist = Position::distance2d(_unit->getPosition(), bu->getPosition());
			{
				bool valid = false;
				if (_rifle || !_melee)
				{
					BattleAction action;
					action.actor = _unit;
					action.weapon = _attackAction.weapon;
					action.target = bu->getPosition();
					Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
					valid = _save->getTileEngine()->canTargetUnit(&origin, bu->getTile(), &target, _unit, false);
				}
				else
				{
					if (selectPointNearTarget(bu, _unit->getTimeUnits()))
					{
						int dir = _save->getTileEngine()->getDirectionTo(_attackAction.target, bu->getPosition());
						valid = _save->getTileEngine()->validMeleeRange(_attackAction.target, dir, _unit, bu, 0);
					}
				}
				if (valid)
				{
					const int targetScore = scoreTargetPriority(bu, assigned, visible, dist);
					if (targetScore > bestTargetScore || (targetScore == bestTargetScore && dist < _closestDist))
					{
						bestTargetScore = targetScore;
						_closestDist = dist;
						_aggroTarget = bu;
					}
				}
			}
		}
	}
	if (_aggroTarget)
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction target accepted: unit=" << _unit->getId()
				<< ", target=" << _aggroTarget->getId()
				<< ", score=" << bestTargetScore
				<< ", distance=" << _closestDist
				<< ", assigned=" << (assignedTarget == _aggroTarget)
				<< ", reason=" << (_factionAI ? _factionAI->getAssignmentReason(_unit) : "");
			_save->appendToAutoBattleLog(log.str());
		}
		return tally;
	}

	return 0;
}

/**
 * Selects the nearest known living target we can see/reach and returns the number of visible enemies.
 * This function includes civilians as viable targets.
 * Note: Differs from selectNearestTarget() in calling selectPointNearTargetLeeroy().
 * @return viable targets.
 */
int PlayerFactionAI::selectNearestTargetLeeroy(bool canRun)
{
	int tally = 0;
	_closestDist = 100;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, true) &&
			_save->getTileEngine()->visible(_unit, bu->getTile()))
		{
			tally++;
			int dist = Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist < _closestDist)
			{
				bool valid = false;
				if (selectPointNearTargetLeeroy(bu, canRun))
				{
					int dir = _save->getTileEngine()->getDirectionTo(_attackAction.target, bu->getPosition());
					valid = _save->getTileEngine()->validMeleeRange(_attackAction.target, dir, _unit, bu, 0);
				}
				if (valid)
				{
					_closestDist = dist;
					_aggroTarget = bu;
				}
			}
		}
	}
	if (_aggroTarget)
	{
		return tally;
	}

	return 0;
}

/**
 * Selects the nearest known living Xcom unit.
 * used for ambush calculations
 * @return if we found one.
 */
bool PlayerFactionAI::selectClosestKnownEnemy()
{
	_aggroTarget = 0;
	int minDist = 255;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, false))
		{
			int dist = Position::distance2d(bu->getPosition(), _unit->getPosition());
			if (dist < minDist)
			{
				minDist = dist;
				_aggroTarget = bu;
			}
		}
	}
	return _aggroTarget != 0;
}

/**
 * Selects a random known living Xcom or civilian unit.
 * @return if we found one.
 */
bool PlayerFactionAI::selectRandomTarget()
{
	int farthest = -100;
	_aggroTarget = 0;

	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, true))
		{
			int dist = RNG::generate(0,20) - Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist > farthest)
			{
				farthest = dist;
				_aggroTarget = bu;
			}
		}
	}
	return _aggroTarget != 0;
}

/**
 * Selects a point near enough to our target to perform a melee attack.
 * @param target Pointer to a target.
 * @param maxTUs Maximum time units the path to the target can cost.
 * @return True if a point was found.
 */
bool PlayerFactionAI::selectPointNearTarget(BattleUnit *target, int maxTUs)
{
	int size = _unit->getArmor()->getSize();
	int sizeTarget = target->getArmor()->getSize();
	int dirTarget = target->getDirection();
	float dodgeChanceDiff = target->getArmor()->getMeleeDodge(target) * target->getArmor()->getMeleeDodgeBackPenalty() * _attackAction.diff / 160.0f;
	bool returnValue = false;
	int distance = 1000;
	for (int z = -1; z <= 1; ++z)
	{
		for (int x = -size; x <= sizeTarget; ++x)
		{
			for (int y = -size; y <= sizeTarget; ++y)
			{
				if (x || y) // skip the unit itself
				{
					Position checkPath = target->getPosition() + Position (x, y, z);
					if (_save->getTile(checkPath) == 0 || std::find(_reachable.begin(), _reachable.end(), _save->getTileIndex(checkPath))  == _reachable.end())
						continue;
					int dir = _save->getTileEngine()->getDirectionTo(checkPath, target->getPosition());
					bool valid = _save->getTileEngine()->validMeleeRange(checkPath, dir, _unit, target, 0);
					bool fitHere = _save->setUnitPosition(_unit, checkPath, true);

					if (valid && fitHere && !_save->getTile(checkPath)->getDangerous())
					{
						_save->getPathfinding()->calculate(_unit, checkPath, BAM_NORMAL, 0, maxTUs);

						//for 100% dodge diff and on 4th difficulty it will allow aliens to move 10 squares around to made attack from behind.
						int distanceCurrent = _save->getPathfinding()->getPath().size() - dodgeChanceDiff * _save->getTileEngine()->getArcDirection(dir - 4, dirTarget);
						if (_save->getPathfinding()->getStartDirection() != -1 && distanceCurrent < distance)
						{
							_attackAction.target = checkPath;
							returnValue = true;
							distance = distanceCurrent;
						}
						_save->getPathfinding()->abortPath();
					}
				}
			}
		}
	}
	return returnValue;
}

/**
 * Selects a point near enough to our target to perform a melee attack.
 * Note: Differs from selectPointNearTarget() in that it doesn't consider:
 *  - remaining TUs (charge even if not enough TUs to attack)
 *  - dangerous tiles (grenades? pfff!)
 *  - melee dodge (not intelligent enough to attack from behind)
 * @param target Pointer to a target.
 * @return True if a point was found.
 */
bool PlayerFactionAI::selectPointNearTargetLeeroy(BattleUnit *target, bool canRun)
{
	int size = _unit->getArmor()->getSize();
	int targetsize = target->getArmor()->getSize();
	bool returnValue = false;
	unsigned int distance = 1000;
	for (int z = -1; z <= 1; ++z)
	{
		for (int x = -size; x <= targetsize; ++x)
		{
			for (int y = -size; y <= targetsize; ++y)
			{
				if (x || y) // skip the unit itself
				{
					Position checkPath = target->getPosition() + Position(x, y, z);
					if (_save->getTile(checkPath) == 0)
						continue;
					int dir = _save->getTileEngine()->getDirectionTo(checkPath, target->getPosition());
					bool valid = _save->getTileEngine()->validMeleeRange(checkPath, dir, _unit, target, 0);
					bool fitHere = _save->setUnitPosition(_unit, checkPath, true);

					if (valid && fitHere)
					{
						_save->getPathfinding()->calculate(_unit, checkPath, canRun ? BAM_RUN : BAM_NORMAL, 0, 100000); // disregard unit's TUs.
						if (_save->getPathfinding()->getStartDirection() != -1 && _save->getPathfinding()->getPath().size() < distance)
						{
							_attackAction.target = checkPath;
							returnValue = true;
							distance = _save->getPathfinding()->getPath().size();
						}
						_save->getPathfinding()->abortPath();
					}
				}
			}
		}
	}
	return returnValue;
}

/**
 * Selects a target from a list of units seen by spotter units for out-of-LOS actions and populates _attackAction with the relevant data
 * @return True if we have a target selected
 */
bool PlayerFactionAI::selectSpottedUnitForSniper()
{
	_aggroTarget = 0;

	// Create a list of spotted targets and the type of attack we'd like to use on each
	std::vector<std::pair<BattleUnit*, BattleAction>> spottedTargets;

	// Get the TU costs for each available attack type
	BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, _attackAction.weapon);

	BattleActionCost costThrow;
	// Only want to check throwing if we have a grenade, the default constructor (line above) conveniently returns false from haveTU()
	if (_grenade)
	{
		// We know we have a grenade, now we need to know if we have the TUs to throw it
		costThrow.type = BA_THROW;
		costThrow.actor = _attackAction.actor;
		costThrow.weapon = _unit->getGrenadeFromBelt(_save);
		costThrow.updateTU();
		costThrow.Time += 4; // Vanilla TUs for AI picking up grenade from belt
		costThrow += _attackAction.actor->getActionTUs(BA_PRIME, costThrow.weapon);
	}

	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, true)
			&& !(_unit->getFaction() == FACTION_PLAYER && isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), bu))
			&& bu->getTurnsLeftSpottedForSnipersByFaction(_unit->getFaction()))
		{
			// Determine which firing mode to use based on how many hits we expect per turn and the unit's intelligence/aggression
			_aggroTarget = bu;
			_attackAction.type = BA_RETHINK;
			_attackAction.target = bu->getPosition();
			extendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, true);

			BattleAction chosenAction = _attackAction;
			if (chosenAction.type == BA_THROW)
				chosenAction.weapon = costThrow.weapon;

			if (_attackAction.type != BA_RETHINK)
			{
				std::pair<BattleUnit*, BattleAction> spottedTarget;
				spottedTarget = std::make_pair(bu, chosenAction);
				spottedTargets.push_back(spottedTarget);
			}
		}
	}

	int numberOfTargets = static_cast<int>(spottedTargets.size());

	if (numberOfTargets) // Now that we have a list of valid targets, pick one and return.
	{
		int pick = RNG::generate(0, numberOfTargets - 1);
		_aggroTarget = spottedTargets.at(pick).first;
		_attackAction.target = _aggroTarget->getPosition();
		_attackAction.type = spottedTargets.at(pick).second.type;
		_attackAction.weapon = spottedTargets.at(pick).second.weapon;
	}
	else // We didn't find a suitable target
	{
		// Make sure we reset anything we might have changed while testing for targets
		_aggroTarget = 0;
		_attackAction.type = BA_RETHINK;
		_attackAction.weapon = _unit->getMainHandWeapon(false);
	}

	return _aggroTarget != 0;
}

/**
 * Scores a firing mode for a particular target based on a accuracy / TUs ratio
 * @param action Pointer to the BattleAction determining the firing mode
 * @param target Pointer to the BattleUnit we're trying to target
 * @param checkLOF Set to true if you want to check for a valid line of fire
 * @return The calculated score
 */
int PlayerFactionAI::scoreFiringMode(BattleAction *action, BattleUnit *target, bool checkLOF)
{
	// Sanity check first, if the passed action has no type or weapon, return 0.
	if (!action->type || !action->weapon)
	{
		return 0;
	}
	auto* weapon = action->weapon->getRules();

	// Get base accuracy for the action
	int accuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(*action), _save->getMod());
	int distanceSq = _unit->distance3dToUnitSq(target);
	int distance = (int)std::ceil(sqrt(float(distanceSq)));

	{
		int upperLimit, lowerLimit;
		int dropoff = weapon->calculateLimits(upperLimit, lowerLimit, _save->getDepth(), action->type);

		if (distance > upperLimit)
		{
			accuracy -= (distance - upperLimit) * dropoff;
		}
		else if (distance < lowerLimit)
		{
			accuracy -= (lowerLimit - distance) * dropoff;
		}
	}

	bool outOfRange = action->type == BA_THROW
		? weapon->isOutOfThrowRange(distanceSq, _save->getDepth())
		: weapon->isOutOfRange(distanceSq);

	if (outOfRange)
	{
		accuracy = 0;
	}

	if (accuracy > 0 && directProjectileRiskyForAllies(action, target))
	{
		return 0;
	}
	if (accuracy > 0 && autoShotRiskyForAllies(action, target))
	{
		return 0;
	}

	int roleScoreModifier = 100;
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		const PlayerAIRole role = getPlayerAIRole(action->weapon);
		const int preferred = getPreferredEngagementRange(action->weapon);
		const int rangeDelta = abs(distance - preferred);
		roleScoreModifier -= rangeDelta * (role == ROLE_MARKSMAN || role == ROLE_HEAVY ? 3 : 2);
		if (role == ROLE_MARKSMAN && action->type == BA_AIMEDSHOT)
		{
			roleScoreModifier += 18;
		}
		else if (role == ROLE_ASSAULT && action->type == BA_AUTOSHOT && distance <= preferred)
		{
			roleScoreModifier += 12;
		}
		else if (role == ROLE_SUPPORT && distance > preferred + 3)
		{
			roleScoreModifier -= 18;
		}
		if (action->type == BA_AUTOSHOT && distance <= preferred + 1)
		{
			roleScoreModifier += 35;
		}
		if (action->type == BA_SNAPSHOT && target && target->getHealth() > 0 && target->getHealth() <= 35)
		{
			roleScoreModifier += 18;
		}
		if (action->type == BA_AUTOSHOT && distance > preferred + 2)
		{
			roleScoreModifier -= 25;
		}
		roleScoreModifier = Clamp(roleScoreModifier, 35, 140);
	}

	int numberOfShots = 1;
	if (action->type == BA_AIMEDSHOT)
	{
		numberOfShots = weapon->getConfigAimed()->shots;
	}
	else if (action->type == BA_SNAPSHOT)
	{
		numberOfShots = weapon->getConfigSnap()->shots;
	}
	else if (action->type == BA_AUTOSHOT)
	{
		numberOfShots = weapon->getConfigAuto()->shots;
	}

	int tuCost = _unit->getActionTUs(action->type, action->weapon).Time;
	// Need to include TU cost of getting grenade from belt + priming if we're checking throwing
	if (action->type == BA_THROW && _grenade)
	{
		// FIXME: why not just use action->weapon ?
		auto* grenadeItem = _unit->getGrenadeFromBelt(_save);
		tuCost = _unit->getActionTUs(action->type, grenadeItem).Time;
		tuCost += 4;
		tuCost += _unit->getActionTUs(BA_PRIME, grenadeItem).Time;
	}
	int tuTotal = _unit->getBaseStats()->tu;

	// Return a score of zero if this firing mode doesn't exist for this weapon
	if (!tuCost)
	{
		return 0;
	}

	if (checkLOF)
	{
		Position origin = _save->getTileEngine()->getOriginVoxel((*action), 0);
		Position targetPosition;

		if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
		{
			targetPosition = target->getPosition().toVoxel() + Position (8,8, (2 + -target->getTile()->getTerrainLevel()));
			if (!_save->getTileEngine()->validateThrow((*action), origin, targetPosition, _save->getDepth()))
			{
				return 0;
			}
		}
		else
		{
			if (!_save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetPosition, _unit, false, target))
			{
				return 0;
			}
		}
	}

	int damageScoreModifier = 100;
	if (_unit->getFaction() == FACTION_PLAYER && target && action->type != BA_THROW)
	{
		const int expectedDamage = estimateDirectShotDamage(action, target, accuracy, numberOfShots);
		const int targetHealth = std::max(1, target->getHealth());
		const int targetArmor = std::max(std::max(target->getArmor(SIDE_FRONT), target->getArmor(SIDE_LEFT)), target->getArmor(SIDE_RIGHT));
		const bool durableTarget = targetHealth >= 70 || targetArmor >= 35;
		if (expectedDamage <= 0)
		{
			damageScoreModifier = 25;
		}
		else if (expectedDamage < 4 && targetHealth > 20)
		{
			damageScoreModifier = 45;
		}
		else if (expectedDamage < 8 && targetHealth > 45)
		{
			damageScoreModifier = 70;
		}
		else if (expectedDamage >= targetHealth)
		{
			damageScoreModifier = 145;
		}
		else if (expectedDamage >= targetHealth / 2)
		{
			damageScoreModifier = 125;
		}
		if (_spottingEnemies > 0 && expectedDamage < 10 && targetHealth > 35)
		{
			damageScoreModifier = std::min(damageScoreModifier, 45);
		}
		if (durableTarget && expectedDamage < targetHealth)
		{
			if (action->type == BA_AUTOSHOT && distance <= getPreferredEngagementRange(action->weapon) + 4)
			{
				damageScoreModifier += 35;
			}
			else if (action->type == BA_SNAPSHOT && expectedDamage >= 12)
			{
				damageScoreModifier += 15;
			}
			else if (action->type == BA_AIMEDSHOT && expectedDamage < targetHealth / 2)
			{
				damageScoreModifier -= 20;
			}
			damageScoreModifier = Clamp(damageScoreModifier, 25, 170);
		}
	}

	return accuracy * numberOfShots * tuTotal * roleScoreModifier * damageScoreModifier / tuCost / 10000;
}

/**
 * Selects an AI mode based on a number of factors, some RNG and the results of the rest of the determinations.
 */
void PlayerFactionAI::evaluateAIMode()
{
	if ((_unit->getCharging() && _attackAction.type != BA_RETHINK))
	{
		_AIMode = AI_COMBAT;
		return;
	}
	// don't try to run away as often if we're a melee type, and really don't try to run away if we have a viable melee target, or we still have 50% or more TUs remaining.
	int escapeOdds = 15;
	if (_melee)
	{
		escapeOdds = 12;
	}
	if (_unit->getFaction() == FACTION_HOSTILE && (_unit->getTimeUnits() > _unit->getBaseStats()->tu / 2 || _unit->getCharging()))
	{
		escapeOdds = 5;
	}
	int ambushOdds = 12;
	int combatOdds = 20;
	// we're less likely to patrol if we see enemies.
	int patrolOdds = _visibleEnemies ? 15 : 30;

	// the enemy sees us, we should take retreat into consideration, and forget about patrolling for now.
	if (_spottingEnemies)
	{
		patrolOdds = 0;
		if (_escapeTUs == 0)
		{
			setupEscape();
		}
	}

	// melee/blaster units shouldn't consider ambush
	if (!_rifle || _ambushTUs == 0)
	{
		ambushOdds = 0;
		if (_melee)
		{
			combatOdds *= 1.3;
		}
	}

	// if we KNOW there are enemies around...
	if (_knownEnemies)
	{
		if (_knownEnemies == 1)
		{
			combatOdds *= 1.2;
		}

		if (_escapeTUs == 0)
		{
			if (selectClosestKnownEnemy())
			{
				setupEscape();
			}
			else
			{
				escapeOdds = 0;
			}
		}
	}
	else if (_unit->getFaction() == FACTION_HOSTILE)
	{
		combatOdds = 0;
		escapeOdds = 0;
	}

	// take our current mode into consideration
	switch (_AIMode)
	{
	case AI_PATROL:
		patrolOdds *= 1.1;
		break;
	case AI_AMBUSH:
		ambushOdds *= 1.1;
		break;
	case AI_COMBAT:
		combatOdds *= 1.1;
		break;
	case AI_ESCAPE:
		escapeOdds *= 1.1;
		break;
	}

	// take our overall health into consideration
	if (_unit->getHealth() < _unit->getBaseStats()->health / 3)
	{
		escapeOdds *= 1.7;
		combatOdds *= 0.6;
		ambushOdds *= 0.75;
	}
	else if (_unit->getHealth() < 2 * (_unit->getBaseStats()->health / 3))
	{
		escapeOdds *= 1.4;
		combatOdds *= 0.8;
		ambushOdds *= 0.8;
	}
	else if (_unit->getHealth() < _unit->getBaseStats()->health)
	{
		escapeOdds *= 1.1;
	}

	// take our aggression into consideration
	switch (_unit->getAggression())
	{
	case 0:
		escapeOdds *= 1.4;
		combatOdds *= 0.7;
		break;
	case 1:
		ambushOdds *= 1.1;
		break;
	case 2:
		combatOdds *= 1.4;
		escapeOdds *= 0.7;
		break;
	default:
		combatOdds *= Clamp(1.2 + (_unit->getAggression() / 10.0), 0.1, 2.0);
		escapeOdds *= Clamp(0.9 - (_unit->getAggression() / 10.0), 0.1, 2.0);
		break;
	}

	if (_AIMode == AI_COMBAT)
	{
		ambushOdds *= 1.5;
	}

	// factor in the spotters.
	if (_spottingEnemies)
	{
		escapeOdds = 10 * escapeOdds * (_spottingEnemies + 10) / 100;
		combatOdds = 5 * combatOdds * (_spottingEnemies + 20) / 100;
	}
	else
	{
		escapeOdds /= 2;
	}

	// factor in visible enemies.
	if (_visibleEnemies)
	{
		combatOdds = 10 * combatOdds * (_visibleEnemies + 10) /100;
		if (_closestDist < 5)
		{
			ambushOdds = 0;
		}
	}
	// make sure we have an ambush lined up, or don't even consider it.
	if (_ambushTUs)
	{
		ambushOdds *= 1.7;
	}
	else
	{
		ambushOdds = 0;
	}

	// factor in mission type
	if (_save->getMissionType() == "STR_BASE_DEFENSE")
	{
		escapeOdds *= 0.75;
		ambushOdds *= 0.6;
	}

	// no weapons, not psychic? don't pick combat or ambush
	if (!_melee && !_rifle && !_blaster && !_grenade && _unit->getBaseStats()->psiSkill == 0)
	{
		combatOdds = 0;
		ambushOdds = 0;
	}
	if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
	{
		BattleItem *roleWeapon = _unit->getMainHandWeapon(false);
		if (!roleWeapon)
		{
			roleWeapon = selectBestCarriedWeapon();
		}
		const PlayerAIRole role = getPlayerAIRole(roleWeapon);
		const PlayerFactionStrategy strategy = getFactionStrategy();
		const int beforeEscape = escapeOdds;
		const int beforeAmbush = ambushOdds;
		const int beforeCombat = combatOdds;
		const int beforePatrol = patrolOdds;
		applyFactionStrategyToModeOdds(strategy, role, &escapeOdds, &ambushOdds, &combatOdds, &patrolOdds);
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction mode odds: unit=" << _unit->getId()
				<< ", strategy=" << getFactionStrategyName(strategy)
				<< ", role=" << getPlayerAIRoleName(role)
				<< ", escape=" << beforeEscape << "->" << escapeOdds
				<< ", ambush=" << beforeAmbush << "->" << ambushOdds
				<< ", combat=" << beforeCombat << "->" << combatOdds
				<< ", patrol=" << beforePatrol << "->" << patrolOdds
				<< ", visible=" << _visibleEnemies
				<< ", known=" << _knownEnemies
				<< ", spotting=" << _spottingEnemies;
			_save->appendToAutoBattleLog(log.str());
		}
	}
	// generate a random number to represent our decision.
	int decision = RNG::generate(1, std::max(1, patrolOdds + ambushOdds + escapeOdds + combatOdds));

	if (decision > escapeOdds)
	{
		if (decision > escapeOdds + ambushOdds)
		{
			if (decision > escapeOdds + ambushOdds + combatOdds)
			{
				_AIMode = AI_PATROL;
			}
			else
			{
				_AIMode = AI_COMBAT;
			}
		}
		else
		{
			_AIMode = AI_AMBUSH;
		}
	}
	else
	{
		_AIMode = AI_ESCAPE;
	}

	if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
	{
		const PlayerFactionStrategy strategy = getFactionStrategy();
		const bool badlyWounded = _unit->getHealth() < std::max(1, _unit->getBaseStats()->health / 2);
		const bool usefulAttack = _attackAction.type != BA_RETHINK;
		int activeAllies = 0;
		for (auto *other : *_save->getUnits())
		{
			if (other && !other->isOut() && other->getFaction() == _unit->getFaction())
			{
				++activeAllies;
			}
		}
		const bool plannedGrenadeThrow = usefulAttack
			&& _attackAction.type == BA_THROW
			&& _attackAction.weapon
			&& _attackAction.weapon->getRules()->isGrenadeOrProxy();
		if (plannedGrenadeThrow)
		{
			_AIMode = AI_COMBAT;
		}
		else if (strategy == PFS_HUNT_LAST_ENEMY && usefulAttack && _cleanShotMoveAction)
		{
			_AIMode = AI_COMBAT;
		}
		else if (strategy == PFS_SKIRMISH && usefulAttack && (_visibleEnemies || _spottingEnemies))
		{
			_AIMode = AI_COMBAT;
		}
		else if (strategy == PFS_SKIRMISH && _ambushTUs && !_visibleEnemies && !_spottingEnemies)
		{
			_AIMode = AI_AMBUSH;
		}
		else if ((strategy == PFS_SURVIVE || strategy == PFS_RETREAT_REGROUP) && _escapeTUs && (_spottingEnemies || badlyWounded || !usefulAttack))
		{
			_AIMode = AI_ESCAPE;
		}
		else if ((strategy == PFS_INITIAL_DEPLOY || strategy == PFS_DEFEND_LINE || strategy == PFS_SIEGE_ROOM || strategy == PFS_HOLD_REACTION) && _ambushTUs && !_visibleEnemies)
		{
			_AIMode = AI_AMBUSH;
		}
		else if (strategy == PFS_ASSAULT && usefulAttack && _visibleEnemies)
		{
			_AIMode = AI_COMBAT;
		}
		else if (strategy == PFS_HUNT_LAST_ENEMY && !_visibleEnemies && _ambushTUs && (activeAllies <= 3 || badlyWounded))
		{
			_AIMode = AI_AMBUSH;
		}
		else if (strategy == PFS_HUNT_LAST_ENEMY && !_visibleEnemies && _AIMode == AI_ESCAPE && !badlyWounded && !_spottingEnemies)
		{
			_AIMode = _toNode || _foundBaseModuleToDestroy ? AI_PATROL : AI_AMBUSH;
		}
		if (!_visibleEnemies
			&& !_spottingEnemies
			&& (strategy == PFS_INITIAL_DEPLOY
				|| strategy == PFS_HOLD_REACTION
				|| (_save->getTurn() <= 2 && (strategy == PFS_DEFEND_LINE || strategy == PFS_SIEGE_ROOM)))
			&& !_factionSupportMoveAction
			&& !_stalkAmbushAction
			&& !_cleanShotMoveAction
			&& _attackAction.type != BA_THROW)
		{
			_AIMode = _ambushTUs ? AI_AMBUSH : AI_ESCAPE;
			_attackAction.type = BA_RETHINK;
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction patrol suppressed: unit=" << _unit->getId()
					<< ", strategy=" << getFactionStrategyName(strategy)
					<< ", oldPatrolTarget=" << (_toNode ? _toNode->getPosition() : _patrolAction.target)
					<< ", ambushTUs=" << _ambushTUs
					<< ", escapeTUs=" << _escapeTUs;
				_save->appendToAutoBattleLog(log.str());
			}
		}
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction mode selected: unit=" << _unit->getId()
				<< ", strategy=" << getFactionStrategyName(strategy)
				<< ", mode=" << _AIMode
				<< ", attack=" << (int)_attackAction.type
				<< ", ambushTUs=" << _ambushTUs
				<< ", escapeTUs=" << _escapeTUs
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", wounded=" << badlyWounded;
			_save->appendToAutoBattleLog(log.str());
		}
	}

	// if the aliens are cheating, or the unit is charging, enforce combat as a priority.
	if ((_unit->getFaction() == FACTION_HOSTILE && _save->isCheating()) || _unit->getCharging() != 0)
	{
		_AIMode = AI_COMBAT;
	}


	// enforce the validity of our decision, and try fallback behaviour according to priority.
	if (_AIMode == AI_COMBAT)
	{
		const bool plannedTacticalMove = _attackAction.type == BA_WALK
			&& (_cleanShotMoveAction || _fallbackCoverAction || _factionSupportMoveAction || _stalkAmbushAction);
		if (plannedTacticalMove)
		{
			return;
		}
		auto* xtile = _save->getTile(_attackAction.target);
		bool throwingGrenadeOrProxy = _attackAction.type == BA_THROW && _attackAction.weapon && _attackAction.weapon->getRules()->isGrenadeOrProxy();
		if (xtile && (xtile->getUnit() || throwingGrenadeOrProxy)) // https://openxcom.org/forum/index.php?topic=12145.0
		{
			if (_attackAction.type != BA_RETHINK)
			{
				return;
			}
			if (findFirePoint())
			{
				return;
			}
		}
		else if (selectRandomTarget() && findFirePoint())
		{
			return;
		}
		_AIMode = AI_PATROL;
	}

	if (_AIMode == AI_PATROL)
	{
		if (_toNode || _foundBaseModuleToDestroy)
		{
			return;
		}
		// base defense mission protocol: patrol action becomes an attack action when base modules are sighted
		if (_patrolAction.type == BA_SNAPSHOT)
		{
			return;
		}
		_AIMode = AI_AMBUSH;
	}

	if (_AIMode == AI_AMBUSH)
	{
		if (_ambushTUs != 0)
		{
			return;
		}
		_AIMode = AI_ESCAPE;
	}
}

/**
 * Find a position where we can see our target, and move there.
 * check the 11x11 grid for a position nearby where we can potentially target him.
 * @return True if a possible position was found.
 */
bool PlayerFactionAI::findFirePoint()
{
	if (!selectClosestKnownEnemy())
		return false;
	std::vector<Position> randomTileSearch = _save->getTileSearch(); // copy!
	RNG::shuffle(randomTileSearch);
	Position target;
	bool waitIfOutsideWeaponRange = _unit->getGeoscapeSoldier() ? false : _unit->getUnitRules()->waitIfOutsideWeaponRange();
	bool extendedFireModeChoiceEnabled = _save->getMod()->getAIExtendedFireModeChoice();
	int bestScore = 0;
	_attackAction.type = BA_RETHINK;
	for (const auto& randomPosition : randomTileSearch)
	{
		Position pos = _unit->getPosition() + randomPosition;
		Tile *tile = _save->getTile(pos);
		if (tile == 0  ||
			std::find(_reachableWithAttack.begin(), _reachableWithAttack.end(), _save->getTileIndex(pos))  == _reachableWithAttack.end())
			continue;
		int score = 0;
		// i should really make a function for this
		Position origin = pos.toVoxel() +
			// 4 because -2 is eyes and 2 below that is the rifle (or at least that's my understanding)
			Position(8,8, _unit->getHeight() + _unit->getFloatHeight() - tile->getTerrainLevel() - 4);

		if (_save->getTileEngine()->canTargetUnit(&origin, _aggroTarget->getTile(), &target, _unit, false))
		{
			_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
			// can move here
			if (_save->getPathfinding()->getStartDirection() != -1)
			{
				int spotters = getSpottingUnits(pos);
				if (_unit->getFaction() == FACTION_PLAYER && spotters > 0)
				{
					continue;
				}
				score = PLAYER_AI_FIREPOINT_BASE_SYSTEMATIC_SUCCESS - spotters * 10;
				score += _unit->getTimeUnits() - _save->getPathfinding()->getTotalTUCost();
				if (!_aggroTarget->checkViewSector(pos))
				{
					score += 10;
				}

				// Extended behavior: if we have a limited-range weapon, bump up the score for getting closer to the target, down for further
				if (!waitIfOutsideWeaponRange && extendedFireModeChoiceEnabled)
				{
					int distanceToTargetSq = _unit->distance3dToUnitSq(_aggroTarget);
					int distanceToTarget = (int)std::ceil(sqrt(float(distanceToTargetSq)));
					if (_attackAction.weapon && _attackAction.weapon->getRules()->isOutOfRange(distanceToTargetSq)) // make sure we can get the ruleset before checking the range
					{
						int proposedDistance = Position::distance2d(pos, _aggroTarget->getPosition());
						proposedDistance = std::max(proposedDistance, 1);
						score = score * distanceToTarget / proposedDistance;
					}
				}

				if (score > bestScore)
				{
					bestScore = score;
					_attackAction.target = pos;
					_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(pos, _aggroTarget->getPosition());
					if (score > PLAYER_AI_FIREPOINT_FAST_PASS_THRESHOLD)
					{
						break;
					}
				}
			}
		}
	}

	if (bestScore > 70)
	{
		_attackAction.type = BA_WALK;
		if (_traceAI)
		{
			Log(LOG_INFO) << "Firepoint found at " << _attackAction.target << ", with a score of: " << bestScore;
		}
		return true;
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Firepoint failed, best estimation was: " << _attackAction.target << ", with a score of: " << bestScore;
	}

	return false;
}

bool PlayerFactionAI::setupCleanShotMove(BattleUnit *target)
{
	if (_unit->getFaction() != FACTION_PLAYER || !target || target->isOut()
		|| isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), target)
		|| !_attackAction.weapon || (_reachableWithAttack.empty() && _reachable.empty()))
	{
		if (_unit->getFaction() == FACTION_PLAYER && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction clean shot move rejected early: unit=" << _unit->getId()
				<< ", target=" << (target ? target->getId() : -1)
				<< ", hasWeapon=" << (_attackAction.weapon != 0)
				<< ", reachable=" << _reachable.size()
				<< ", reachableWithAttack=" << _reachableWithAttack.size();
			_save->appendToAutoBattleLog(log.str());
		}
		return false;
	}

	const PlayerAIRole role = getPlayerAIRole(_attackAction.weapon);
	const PlayerAITacticalRole tacticalRole = getPlayerTacticalRole(_attackAction.weapon, role);
	const int preferredRange = getPreferredEngagementRange(_attackAction.weapon);
	const Position current = _unit->getPosition();
	const int currentSpotters = getSpottingUnits(current);
	const int currentExposure = getEnemyFireExposure(current);
	const int currentFireLines = countEnemyFireLines(current);
	Tile *currentTile = _save->getTile(current);
	int currentCover = 0;
	if (currentTile)
	{
		currentCover += currentTile->getMapData(O_OBJECT) ? 8 : 0;
		currentCover += currentTile->getMapData(O_NORTHWALL) ? 6 : 0;
		currentCover += currentTile->getMapData(O_WESTWALL) ? 6 : 0;
	}
	const int currentDist = Position::distance2d(current, target->getPosition());
	const bool meleeRole = role == ROLE_MELEE || _attackAction.weapon->getRules()->getBattleType() == BT_MELEE;
	const PlayerFactionStrategy strategy = getFactionStrategy();
	int activeAllies = 0;
	int activeHostiles = 0;
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		for (auto *other : *_save->getUnits())
		{
			if (!other || other->isOut())
			{
				continue;
			}
			if (other->getFaction() == _unit->getFaction())
			{
				++activeAllies;
			}
			else if (other->getFaction() == FACTION_HOSTILE)
			{
				++activeHostiles;
			}
		}
	}
	int bestScore = -100000;
	Position bestPos = _unit->getPosition();
	int bestSpotters = currentSpotters;
	int bestDist = currentDist;
	const bool targetVisible = target->getTile() && _save->getTileEngine()->visible(_unit, target->getTile());
	const bool defensiveHiddenContact = !targetVisible
		&& (strategy == PFS_INITIAL_DEPLOY
			|| strategy == PFS_DEFEND_LINE
			|| strategy == PFS_SIEGE_ROOM
			|| strategy == PFS_HOLD_REACTION
			|| strategy == PFS_SKIRMISH
			|| strategy == PFS_SURVIVE);
	const bool cautiousLastEnemyHunt = strategy == PFS_HUNT_LAST_ENEMY
		&& !targetVisible
		&& (activeHostiles <= 5 || activeAllies <= 3 || _unit->getHealth() < _unit->getBaseStats()->health || currentDist > PLAYER_AI_CLEAN_SHOT_CAUTIOUS_HUNT_DISTANCE);
	const bool lateSmallGuerrillaCrossLevelHunt = strategy == PFS_SKIRMISH
		&& !targetVisible && activeAllies <= 2 && activeHostiles <= 2 && _save->getTurn() >= 24;

	if (!targetVisible && target->getPosition().z != current.z
		&& (strategy == PFS_HUNT_LAST_ENEMY || lateSmallGuerrillaCrossLevelHunt))
	{
		const bool endgameCrossLevelDiscipline = (cautiousLastEnemyHunt && activeHostiles <= 5)
			|| lateSmallGuerrillaCrossLevelHunt;
		int crossLevelReserveTU = endgameCrossLevelDiscipline ? 18 : 8;
		if (endgameCrossLevelDiscipline && _attackAction.weapon && _attackAction.weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			BattleActionCost snapCost(BA_SNAPSHOT, _unit, _attackAction.weapon);
			if (snapCost.Time > 0)
			{
				crossLevelReserveTU = std::max(crossLevelReserveTU, (int)snapCost.Time);
			}
		}
		const PlayerDynamicGroupInfo crossLevelGroup = getPlayerDynamicGroupInfo(_save, _unit);
		if (endgameCrossLevelDiscipline && crossLevelGroup.role == PDGR_POINT && crossLevelGroup.maneuverGroup)
		{
			struct CrossLevelNodeCandidate
			{
				Node *node;
				int targetMetric;
			};
			std::vector<CrossLevelNodeCandidate> nodeCandidates;
			for (auto *node : *_save->getNodes())
			{
				if (!node || node->isDummy() || node->getPosition().z != target->getPosition().z)
				{
					continue;
				}
				Tile *nodeTile = _save->getTile(node->getPosition());
				if (!nodeTile || nodeTile->getDangerous()
					|| (nodeTile->getUnit() && nodeTile->getUnit() != _unit))
				{
					continue;
				}
				CrossLevelNodeCandidate candidate = {
					node,
					Position::distance2d(node->getPosition(), target->getPosition()) * 10
				};
				nodeCandidates.push_back(candidate);
			}
			std::sort(nodeCandidates.begin(), nodeCandidates.end(), [](const CrossLevelNodeCandidate &left, const CrossLevelNodeCandidate &right)
			{
				return left.targetMetric < right.targetMetric;
			});
			std::vector<int> bestRoutePath;
			Position bestRouteGoal = current;
			int bestRouteScore = std::numeric_limits<int>::max();
			const size_t nodeProbeLimit = std::min<size_t>(10, nodeCandidates.size());
			for (size_t i = 0; i < nodeProbeLimit; ++i)
			{
				const Position goal = nodeCandidates[i].node->getPosition();
				_save->getPathfinding()->calculate(_unit, goal, BAM_NORMAL);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int fullRouteTU = _save->getPathfinding()->getTotalTUCost();
				const std::vector<int> routePath = _save->getPathfinding()->copyPath();
				_save->getPathfinding()->abortPath();
				const int routeScore = fullRouteTU + nodeCandidates[i].targetMetric * 2;
				if (!routePath.empty() && routeScore < bestRouteScore)
				{
					bestRouteScore = routeScore;
					bestRouteGoal = goal;
					bestRoutePath = routePath;
				}
			}
			const int routeMoveBudget = std::max(0, std::min(28, _unit->getTimeUnits() - crossLevelReserveTU));
			Position routeMoveTarget = current;
			Position routePos = current;
			int routeMoveTU = 0;
			for (auto direction = bestRoutePath.rbegin(); direction != bestRoutePath.rend(); ++direction)
			{
				const PathfindingStep step = _save->getPathfinding()->getTUCost(routePos, *direction, _unit, 0, BAM_NORMAL);
				if (step.cost.time == Pathfinding::INVALID_MOVE_COST
					|| routeMoveTU + step.cost.time > routeMoveBudget
					|| isPlayerExplosiveDanger(_save, _unit->getFaction(), step.pos))
				{
					break;
				}
				routeMoveTU += step.cost.time;
				routePos = step.pos;
				routeMoveTarget = routePos;
			}
			if (routeMoveTarget != current)
			{
				_attackAction.actor = _unit;
				_attackAction.type = BA_WALK;
				_attackAction.target = routeMoveTarget;
				_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(routeMoveTarget, target->getPosition());
				_AIMode = AI_COMBAT;
				_cleanShotMoveAction = true;
				_crossLevelRouteMoveAction = true;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction cross-level node route: unit=" << _unit->getId()
						<< ", targetUnit=" << target->getId()
						<< ", target=" << target->getPosition()
						<< ", from=" << current
						<< ", routeGoal=" << bestRouteGoal
						<< ", moveTarget=" << routeMoveTarget
						<< ", moveTU=" << routeMoveTU
						<< ", reserveTU=" << crossLevelReserveTU
						<< ", reason=follow_real_multilevel_path_instead_of_xy_local_minimum";
					_save->appendToAutoBattleLog(log.str());
				}
				return true;
			}
		}
		const int currentMetric = Position::distance2d(current, target->getPosition()) * 10
			+ std::abs(current.z - target->getPosition().z) * 90;
		int bestLevelScore = PLAYER_AI_REJECT_SCORE;
		Position bestLevelPos = current;
		int bestLevelTU = 0;
		for (auto tileIndex : _reachable)
		{
			Tile *tile = _save->getTile(tileIndex);
			if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
			{
				continue;
			}
			const Position pos = tile->getPosition();
			if (pos == current || isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
			{
				continue;
			}
			_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL, 0, std::max(0, _unit->getTimeUnits() - crossLevelReserveTU));
			if (_save->getPathfinding()->getStartDirection() == -1)
			{
				_save->getPathfinding()->abortPath();
				continue;
			}
			const int moveTU = _save->getPathfinding()->getTotalTUCost();
			_save->getPathfinding()->abortPath();
			const int metric = Position::distance2d(pos, target->getPosition()) * 10
				+ std::abs(pos.z - target->getPosition().z) * 90;
			const int verticalGain = std::abs(current.z - target->getPosition().z) - std::abs(pos.z - target->getPosition().z);
			const int spotters = getSpottingUnits(pos);
			const int exposure = getEnemyFireExposure(pos);
			const int fireLines = countEnemyFireLines(pos);
			if (endgameCrossLevelDiscipline && (spotters > 0 || fireLines > 0 || exposure > 60))
			{
				continue;
			}
			if (metric >= currentMetric && verticalGain <= 0)
			{
				continue;
			}
			if (spotters > currentSpotters + 2 || exposure > currentExposure + 160)
			{
				continue;
			}
			int score = (currentMetric - metric) * 8 + verticalGain * 500;
			score -= moveTU * 3;
			score -= spotters * 35;
			score -= exposure / 2;
			score -= fireLines * 45;
			if (score > bestLevelScore)
			{
				bestLevelScore = score;
				bestLevelPos = pos;
				bestLevelTU = moveTU;
			}
		}
		if (bestLevelPos != current && bestLevelScore > 0)
		{
			_attackAction.actor = _unit;
			_attackAction.type = BA_WALK;
			_attackAction.target = bestLevelPos;
			_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestLevelPos, target->getPosition());
			_AIMode = AI_COMBAT;
			_cleanShotMoveAction = true;
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction cross-level hunt move: unit=" << _unit->getId()
					<< ", targetUnit=" << target->getId()
					<< ", target=" << target->getPosition()
					<< ", from=" << current
					<< ", moveTarget=" << bestLevelPos
					<< ", score=" << bestLevelScore
					<< ", moveTU=" << bestLevelTU
					<< ", reserveTU=" << crossLevelReserveTU
					<< ", cautious=" << cautiousLastEnemyHunt
					<< ", endgameDiscipline=" << endgameCrossLevelDiscipline;
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
	}

	const std::vector<int> &reachableTiles = _reachableWithAttack.empty() ? _reachable : _reachableWithAttack;
	for (auto tileIndex : reachableTiles)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		Position pos = tile->getPosition();
		if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
		{
			continue;
		}
		if (pos.z != _unit->getPosition().z)
		{
			continue;
		}

		_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
		const int moveTU = _save->getPathfinding()->getTotalTUCost();
		const bool canMove = _save->getPathfinding()->getStartDirection() != -1;
		_save->getPathfinding()->abortPath();
		if (!canMove)
		{
			continue;
		}

		BattleActionCost fireCost(BA_SNAPSHOT, _unit, _attackAction.weapon);
		const int returnReserve = (targetVisible || currentSpotters > 0 || currentExposure > 0) ? PLAYER_AI_CLEAN_SHOT_VISIBLE_RETURN_RESERVE : PLAYER_AI_CLEAN_SHOT_HIDDEN_RETURN_RESERVE;
		const int reserveTU = meleeRole ? PLAYER_AI_CLEAN_SHOT_HIDDEN_RETURN_RESERVE : std::max(PLAYER_AI_CLEAN_SHOT_RESERVE_TU, (int)fireCost.Time + returnReserve);
		if (moveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
		{
			continue;
		}

		Position origin = pos.toVoxel() + Position(8, 8, _unit->getHeight() + _unit->getFloatHeight() - tile->getTerrainLevel() - 4);
		Position targetVoxel;
		const bool hasLine = meleeRole || _save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetVoxel, _unit, false, target);
		if (!hasLine)
		{
			continue;
		}
		bool allyInLane = false;
		if (!meleeRole)
		{
			const double vx = (double)(target->getPosition().x - pos.x);
			const double vy = (double)(target->getPosition().y - pos.y);
			const double lenSq = vx * vx + vy * vy;
			if (lenSq > PLAYER_AI_GEOMETRY_EPSILON)
			{
				for (auto *ally : *_save->getUnits())
				{
					if (!ally || ally == _unit || ally == target || ally->isOut() || ally->getFaction() != _unit->getFaction() || ally->getPosition().z != pos.z)
					{
						continue;
					}
					const double ax = (double)(ally->getPosition().x - pos.x);
					const double ay = (double)(ally->getPosition().y - pos.y);
					const double t = (ax * vx + ay * vy) / lenSq;
					if (t <= 0.0 || t >= 1.05)
					{
						continue;
					}
					const double dx = ax - vx * t;
					const double dy = ay - vy * t;
					if (dx * dx + dy * dy <= 0.55)
					{
						allyInLane = true;
						break;
					}
				}
			}
		}
		if (allyInLane)
		{
			continue;
		}

		const int spotters = getSpottingUnits(pos);
		const int exposure = getEnemyFireExposure(pos);
		const int fireLines = countEnemyFireLines(pos);
		const bool saferImmediateBacktrack = spotters < currentSpotters
			|| fireLines < currentFireLines
			|| exposure + 25 < currentExposure;
		if (isPlayerImmediateBacktrack(_save, _unit, pos) && !saferImmediateBacktrack)
		{
			continue;
		}
		const int retreatTU = std::max(0, _unit->getTimeUnits() - moveTU - (int)fireCost.Time);
		const bool needsRetreatRoute = !meleeRole && hasLine && currentFireLines == 0 && (fireLines > 0 || spotters > currentSpotters || exposure > currentExposure + 25);
		const bool canReturnToCurrentCover = currentFireLines == 0 && current != pos && moveTU <= retreatTU;
		const bool hasRetreatRoute = !needsRetreatRoute || canReturnToCurrentCover || hasSafeRetreatFromFirePosition(pos, retreatTU);
		if (spotters > currentSpotters && spotters > 0)
		{
			continue;
		}
		if (exposure > currentExposure + PLAYER_AI_CLEAN_SHOT_EXPOSURE_SPIKE && exposure > PLAYER_AI_CLEAN_SHOT_HIGH_EXPOSURE)
		{
			continue;
		}
		if (targetVisible && fireLines > 0 && exposure >= currentExposure && currentExposure > 0)
		{
			continue;
		}
		if (!hasRetreatRoute)
		{
			continue;
		}
		const int dist = Position::distance2d(pos, target->getPosition());
		const int moveDistance = Position::distance2d(pos, _unit->getPosition());
		if (!targetVisible && getFactionStrategy() == PFS_HUNT_LAST_ENEMY && currentDist > 25 && moveDistance <= 1 && currentDist - dist <= 0)
		{
			continue;
		}
		if (!meleeRole && dist < std::max(3, preferredRange - 5))
		{
			continue;
		}
		if (tacticalRole == TACTICAL_FIRE_SUPPORT && !meleeRole && dist < std::max(7, preferredRange - 2))
		{
			continue;
		}

		int cover = 0;
		if (tile->getMapData(O_OBJECT))
		{
			cover += 8;
		}
		if (tile->getMapData(O_NORTHWALL))
		{
			cover += 6;
		}
		if (tile->getMapData(O_WESTWALL))
		{
			cover += 6;
		}
		int score = PLAYER_AI_CLEAN_SHOT_BASE_SCORE - abs(dist - preferredRange) * (role == ROLE_MARKSMAN || role == ROLE_HEAVY ? 5 : 3);
		score -= moveTU * 2;
		score -= spotters * 45;
		score -= exposure;
		score -= fireLines * 80;
		if (tacticalRole == TACTICAL_FIRE_SUPPORT)
		{
			score -= std::max(0, preferredRange - dist) * 18;
			score += std::min(35, dist * 2);
		}
		else if (tacticalRole == TACTICAL_SCOUT)
		{
			score += hasRetreatRoute ? 25 : -60;
			score -= std::max(0, moveDistance - 4) * 10;
		}
		else if (tacticalRole == TACTICAL_REACTION_GUARD)
		{
			score += std::max(0, 5 - dist) * 4;
			score += cover * 2;
		}
		if (hasRetreatRoute && needsRetreatRoute)
		{
			score += 35;
		}
		score += cover * 5;
		if (spotters < currentSpotters)
		{
			score += 45;
		}
		if (dist >= currentDist && role == ROLE_MARKSMAN
			&& !(cautiousLastEnemyHunt && !targetVisible && currentDist > preferredRange + 8))
		{
			score += 20;
		}
		if (defensiveHiddenContact || cautiousLastEnemyHunt)
		{
			const bool farCautiousHunt = cautiousLastEnemyHunt && currentDist > preferredRange + 8;
			const int requiredHuntProgress = currentDist > 25 ? 3 : 2;
			const bool safeHuntProgress = farCautiousHunt
				&& currentDist - dist >= requiredHuntProgress
				&& spotters <= currentSpotters
				&& fireLines <= currentFireLines
				&& exposure <= currentExposure + 25;
			const bool improvesSafety = spotters < currentSpotters
				|| fireLines < currentFireLines
				|| exposure + 25 < currentExposure
				|| cover >= currentCover + 4
				|| safeHuntProgress;
			const bool keepsDistance = meleeRole || (farCautiousHunt
				? dist >= std::max(4, preferredRange + 2)
				: dist >= std::max(4, currentDist - (cautiousLastEnemyHunt ? 1 : 2)));
			if (!improvesSafety || !keepsDistance)
			{
				continue;
			}
			score -= moveDistance * 12;
			score += cover * 4;
			if (spotters == 0)
			{
				score += 35;
			}
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestSpotters = spotters;
			bestDist = dist;
		}
	}

	const bool committedLastEnemyHunt = cautiousLastEnemyHunt && activeHostiles == 1;
	const int requiredScore = (defensiveHiddenContact || (cautiousLastEnemyHunt && !committedLastEnemyHunt))
		? PLAYER_AI_DEFENSIVE_CLEAN_SHOT_MIN_SCORE
		: PLAYER_AI_CLEAN_SHOT_MIN_SCORE;
	if (bestScore <= requiredScore || bestPos == _unit->getPosition())
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction clean shot move rejected: unit=" << _unit->getId()
				<< ", targetUnit=" << target->getId()
				<< ", bestScore=" << bestScore
				<< ", bestPos=" << bestPos
				<< ", current=" << _unit->getPosition()
				<< ", reachable=" << _reachable.size()
				<< ", reachableWithAttack=" << _reachableWithAttack.size()
				<< ", strategy=" << getFactionStrategyName(strategy)
				<< ", requiredScore=" << requiredScore
				<< ", defensiveHiddenContact=" << defensiveHiddenContact
				<< ", cautiousLastEnemyHunt=" << cautiousLastEnemyHunt;
			_save->appendToAutoBattleLog(log.str());
		}
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = _attackAction.weapon;
	_attackAction.type = BA_WALK;
	_attackAction.target = bestPos;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, target->getPosition());
	_AIMode = AI_COMBAT;
	_cleanShotMoveAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction clean shot move: unit=" << _unit->getId()
			<< ", targetUnit=" << target->getId()
			<< ", target=" << target->getPosition()
			<< ", moveTarget=" << bestPos
			<< ", score=" << bestScore
			<< ", dist=" << bestDist
			<< ", spotters=" << bestSpotters
			<< ", role=" << (int)role
			<< ", tactical=" << getPlayerTacticalRoleName(tacticalRole)
			<< ", reserveMode=snap"
			<< ", reason=free_fire_lane_and_keep_shot_reserve";
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupSharedCleanShotMove(BattleUnit *excludedTarget)
{
	if (_unit->getFaction() != FACTION_PLAYER || !_factionAI || !_attackAction.weapon)
	{
		return false;
	}

	std::vector<std::pair<int, BattleUnit*> > candidates;
	for (auto *enemy : *_save->getUnits())
	{
		if (!enemy || enemy == excludedTarget || enemy->isOut() || enemy->getFaction() != FACTION_HOSTILE
			|| isPendingPlayerTimedBlastTarget(_save, _unit->getFaction(), enemy)
			|| !enemy->getTile() || !validTarget(enemy, true, true))
		{
			continue;
		}
		const int distanceScore = Position::distance2d(_unit->getPosition(), enemy->getPosition())
			+ std::abs(_unit->getPosition().z - enemy->getPosition().z) * 8;
		candidates.push_back(std::make_pair(distanceScore, enemy));
	}
	std::sort(candidates.begin(), candidates.end(), [](const std::pair<int, BattleUnit*> &left, const std::pair<int, BattleUnit*> &right)
	{
		return left.first < right.first;
	});

	BattleUnit *savedAggro = _aggroTarget;
	const int limit = std::min(3, (int)candidates.size());
	for (int i = 0; i < limit; ++i)
	{
		BattleUnit *candidate = candidates[i].second;
		if (setupCleanShotMove(candidate))
		{
			_aggroTarget = candidate;
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction shared clean shot target: unit=" << _unit->getId()
					<< ", targetUnit=" << candidate->getId()
					<< ", target=" << candidate->getPosition()
					<< ", rank=" << i
					<< ", distanceScore=" << candidates[i].first
					<< ", excludedTarget=" << (excludedTarget ? excludedTarget->getId() : -1);
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
	}
	_aggroTarget = savedAggro;
	return false;
}

bool PlayerFactionAI::setupFallbackCoverMove(int minScore, int minExposureGain, int minSpotterGain, int minCoverGain)
{
	if (_unit->getFaction() != FACTION_PLAYER || _reachable.empty() || (!_visibleEnemies && !_spottingEnemies))
	{
		return false;
	}
	Position facePos = _unit->getPosition();
	BattleUnit *assigned = _factionAI ? _factionAI->getAssignedTarget(_unit) : 0;
	if (_aggroTarget && !_aggroTarget->isOut())
	{
		facePos = _aggroTarget->getPosition();
	}
	else if (assigned && !assigned->isOut())
	{
		facePos = assigned->getPosition();
	}
	else if (_factionAI)
	{
		_factionAI->getBestEnemyContactPosition(&facePos);
	}

	auto coverScoreAt = [&](const Position &pos) -> int
	{
		Tile *tile = _save->getTile(pos);
		if (!tile)
		{
			return 0;
		}
		int score = 0;
		if (tile->getMapData(O_OBJECT))
		{
			score += 10;
		}
		if (tile->getMapData(O_NORTHWALL))
		{
			score += facePos.y < pos.y ? 16 : 6;
		}
		if (tile->getMapData(O_WESTWALL))
		{
			score += facePos.x < pos.x ? 16 : 6;
		}
		const Position adjacent[4] = { Position(0, -1, 0), Position(1, 0, 0), Position(0, 1, 0), Position(-1, 0, 0) };
		for (const auto &offset : adjacent)
		{
			Tile *adjacentTile = _save->getTile(pos + offset);
			if (!adjacentTile)
			{
				continue;
			}
			if (adjacentTile->getMapData(O_OBJECT))
			{
				score += 3;
			}
			if (adjacentTile->getMapData(O_NORTHWALL) || adjacentTile->getMapData(O_WESTWALL))
			{
				score += 2;
			}
		}
		return score;
	};

	auto allyCrowdingPenalty = [&](const Position &pos) -> int
	{
		int penalty = 0;
		for (auto *other : *_save->getUnits())
		{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
			{
				continue;
			}
			const int dist = Position::distance2d(pos, other->getPosition());
			if (dist <= 1)
			{
				penalty += 120;
			}
			else if (dist <= 2)
			{
				penalty += 35;
			}
		}
		return penalty;
	};

	auto allyLanePenalty = [&](const Position &pos) -> int
	{
		int penalty = 0;
		const double px = (double)pos.x;
		const double py = (double)pos.y;
		for (auto *other : *_save->getUnits())
		{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
			{
				continue;
			}
			BattleItem *otherWeapon = other->getMainHandWeapon(false);
			if (!otherWeapon || otherWeapon->getRules()->getBattleType() != BT_FIREARM)
			{
				continue;
			}
			const Position otherPos = other->getPosition();
			const double vx = (double)(facePos.x - otherPos.x);
			const double vy = (double)(facePos.y - otherPos.y);
			const double lenSq = vx * vx + vy * vy;
			if (lenSq < 0.1)
			{
				continue;
			}
			const double ax = px - otherPos.x;
			const double ay = py - otherPos.y;
			const double t = (ax * vx + ay * vy) / lenSq;
			if (t <= 0.0 || t >= 1.0)
			{
				continue;
			}
			const double dx = ax - vx * t;
			const double dy = ay - vy * t;
			if (dx * dx + dy * dy <= 1.0)
			{
				penalty += 90;
			}
		}
		return penalty;
	};

	const Position current = _unit->getPosition();
	const int currentSpotters = getSpottingUnits(current);
	const int currentExposure = getEnemyFireExposure(current);
	const int currentFireLines = countEnemyFireLines(current);
	const int currentCover = coverScoreAt(current);
	const int currentCrowding = allyCrowdingPenalty(current);
	const int currentLanePenalty = allyLanePenalty(current);
	const int currentDist = Position::distance2d(current, facePos);
	const int turnMoveCount = getPlayerTurnMoveCount(_save, _unit);
	const int turnMoveDistance = getPlayerTurnMoveDistance(_save, _unit);
	const int preferredRange = getPreferredEngagementRange(_attackAction.weapon);
	const bool ranged = _attackAction.weapon && _attackAction.weapon->getRules()->getBattleType() == BT_FIREARM;
	const bool urgentLineBreak = currentFireLines > 0 || currentExposure >= PLAYER_AI_FALLBACK_URGENT_EXPOSURE || currentSpotters >= PLAYER_AI_NEARBY_SPOTTER_LIMIT;
	const bool emergencyThreatMove = minScore <= PLAYER_AI_FALLBACK_EMERGENCY_SCORE && (_visibleEnemies > 0 || _spottingEnemies > 0);
	const int maxFallbackDistance = urgentLineBreak
		? std::min(PLAYER_AI_FALLBACK_MAX_SAFE_MOVE, std::max(PLAYER_AI_FALLBACK_MAX_NORMAL_MOVE, _unit->getTimeUnits() / 5))
		: (emergencyThreatMove ? PLAYER_AI_FALLBACK_MAX_EMERGENCY_MOVE : PLAYER_AI_FALLBACK_MAX_NORMAL_MOVE);
	int bestScore = 0;
	Position bestPos = current;
	int bestSpotters = currentSpotters;
	int bestExposure = currentExposure;
	int bestFireLines = currentFireLines;
	int bestCover = currentCover;

	for (auto tileIndex : _reachable)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		const Position pos = tile->getPosition();
		if (isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
		{
			continue;
		}
		const int moveDist = Position::distance2d(pos, current);
		const bool emergencyLevelBreak = emergencyThreatMove && pos.z < current.z && moveDist <= 3;
		if ((!emergencyLevelBreak && pos.z != current.z) || moveDist == 0 || moveDist > maxFallbackDistance)
		{
			continue;
		}
		if (moveDist * 6 > _unit->getTimeUnits())
		{
			continue;
		}
		const int spotters = getSpottingUnits(pos);
		const int exposure = getEnemyFireExposure(pos);
		const int fireLines = countEnemyFireLines(pos);
		const int cover = coverScoreAt(pos);
		const int dist = Position::distance2d(pos, facePos);
		if (ranged && dist < 2)
		{
			continue;
		}
		if (!emergencyThreatMove && currentFireLines > 0 && fireLines > 0 && exposure >= currentExposure - 25 && spotters >= currentSpotters)
		{
			continue;
		}
		int score = 0;
		score += (currentSpotters - spotters) * 95;
		score += (currentExposure - exposure);
		score += (currentFireLines - fireLines) * 130;
		if (currentFireLines > 0 && fireLines == 0)
		{
			score += PLAYER_AI_FIRELINE_BREAK_BONUS;
		}
		else if (currentFireLines > 0 && fireLines > 0)
		{
			score -= fireLines * 85;
		}
		score += (cover - currentCover) * 5;
		score -= moveDist * 8;
		score += (currentCrowding - allyCrowdingPenalty(pos));
		score += (currentLanePenalty - allyLanePenalty(pos));
		if (ranged)
		{
			score -= abs(dist - preferredRange) * 2;
			score += dist >= currentDist ? 12 : -18;
		}
		else
		{
			score += dist <= currentDist ? 10 : -8;
		}
		if (emergencyLevelBreak)
		{
			score += 35;
		}
		if (spotters > currentSpotters)
		{
			score -= 140;
		}
		if (exposure > currentExposure && spotters >= currentSpotters)
		{
			score -= 90;
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestSpotters = spotters;
			bestExposure = exposure;
			bestFireLines = fireLines;
			bestCover = cover;
		}
	}

	const int spotterGain = currentSpotters - bestSpotters;
	const int exposureGain = currentExposure - bestExposure;
	const int fireLineGain = currentFireLines - bestFireLines;
	const int coverGain = bestCover - currentCover;
	const int crowdingGain = currentCrowding - allyCrowdingPenalty(bestPos);
	const int laneGain = currentLanePenalty - allyLanePenalty(bestPos);
	bool fallbackPathUnsafe = false;
	Position unsafeFallbackStep = current;
	if (bestPos != current)
	{
		_save->getPathfinding()->calculate(_unit, bestPos, BAM_NORMAL);
		if (_save->getPathfinding()->getStartDirection() == -1)
		{
			fallbackPathUnsafe = true;
		}
		else
		{
			const std::vector<int> path = _save->getPathfinding()->copyPath();
			Position pathPos = current;
			for (auto direction = path.rbegin(); direction != path.rend(); ++direction)
			{
				const PathfindingStep step = _save->getPathfinding()->getTUCost(pathPos, *direction, _unit, 0, BAM_NORMAL);
				if (step.cost.time == Pathfinding::INVALID_MOVE_COST)
				{
					fallbackPathUnsafe = true;
					unsafeFallbackStep = pathPos;
					break;
				}
				const int stepSpotters = getSpottingUnits(step.pos);
				const int stepExposure = getEnemyFireExposure(step.pos);
				const int stepFireLines = countEnemyFireLines(step.pos);
				const bool worsensReadyFire = currentFireLines == 0
					? stepFireLines > 0
					: stepFireLines > currentFireLines;
				if (worsensReadyFire || stepSpotters > currentSpotters
					|| stepExposure > currentExposure + 35)
				{
					fallbackPathUnsafe = true;
					unsafeFallbackStep = step.pos;
					break;
				}
				pathPos = step.pos;
			}
		}
		_save->getPathfinding()->abortPath();
	}
	const bool endpointWorsensImmediateSafety = bestFireLines > currentFireLines
		|| bestSpotters > currentSpotters
		|| (bestExposure > currentExposure + 25 && fireLineGain <= 0 && spotterGain <= 0);
	const bool crossLevelWithoutMaterialGain = bestPos.z != current.z
		&& fireLineGain <= 0 && spotterGain <= 0 && exposureGain < 25;
	const bool repeatedLowPressureFallback = turnMoveCount >= 2
		&& currentFireLines == 0
		&& bestFireLines == 0
		&& currentExposure <= 45
		&& bestExposure <= currentExposure
		&& spotterGain <= 1
		&& coverGain < 10
		&& crowdingGain < 80
		&& laneGain < 80;
	if (bestPos == current || bestScore < minScore || fallbackPathUnsafe
		|| endpointWorsensImmediateSafety || crossLevelWithoutMaterialGain
		|| (!emergencyThreatMove && currentFireLines > 0 && bestFireLines > 0 && exposureGain < std::max(70, currentExposure / 2))
		|| (spotterGain < minSpotterGain && exposureGain < minExposureGain && coverGain < minCoverGain && fireLineGain <= 0)
		|| repeatedLowPressureFallback)
	{
		if (Options::autoBattleLog && (repeatedLowPressureFallback || fallbackPathUnsafe
			|| endpointWorsensImmediateSafety || crossLevelWithoutMaterialGain))
		{
			std::ostringstream log;
			log << "Player faction fallback safety hold: unit=" << _unit->getId()
				<< ", position=" << current
				<< ", rejectedTarget=" << bestPos
				<< ", score=" << bestScore
				<< ", spotters=" << currentSpotters << "->" << bestSpotters
				<< ", exposure=" << currentExposure << "->" << bestExposure
				<< ", fireLines=" << currentFireLines << "->" << bestFireLines
				<< ", cover=" << currentCover << "->" << bestCover
				<< ", crowdingGain=" << crowdingGain
				<< ", laneGain=" << laneGain
				<< ", pathUnsafe=" << fallbackPathUnsafe
				<< ", unsafeStep=" << unsafeFallbackStep
				<< ", endpointWorse=" << endpointWorsensImmediateSafety
				<< ", crossLevelWithoutGain=" << crossLevelWithoutMaterialGain
				<< ", turnMoveCount=" << turnMoveCount
				<< ", turnMoveDistance=" << turnMoveDistance;
			_save->appendToAutoBattleLog(log.str());
		}
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = selectBestCarriedWeapon();
	_attackAction.type = BA_WALK;
	_attackAction.target = bestPos;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, facePos);
	_AIMode = AI_COMBAT;
	_fallbackCoverAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction fallback cover move: unit=" << _unit->getId()
			<< ", target=" << bestPos
			<< ", face=" << facePos
			<< ", score=" << bestScore
			<< ", spotters=" << currentSpotters << "->" << bestSpotters
			<< ", exposure=" << currentExposure << "->" << bestExposure
			<< ", fireLines=" << currentFireLines << "->" << bestFireLines
			<< ", cover=" << currentCover << "->" << bestCover
			<< ", maxMove=" << maxFallbackDistance;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupSmokeExitMove()
{
	if (_unit->getFaction() != FACTION_PLAYER || _reachable.empty() || _save->getTurn() > PLAYER_AI_SMOKE_INITIAL_TURN_LIMIT)
	{
		return false;
	}
	const Position current = _unit->getPosition();
	Position smokeTarget;
	if (!hasRecentPlayerSmokePlanNear(_save, _unit->getFaction(), current, PLAYER_AI_SMOKE_INITIAL_COVER_MEMORY_DISTANCE, &smokeTarget))
	{
		return false;
	}

	Position facePos = smokeTarget;
	if (_factionAI)
	{
		_factionAI->getBestEnemyContactPosition(&facePos);
	}
	const int currentSmokeDist = Position::distance2d(current, smokeTarget);
	const int currentContactDist = Position::distance2d(current, facePos);
	const int currentSpotters = getSpottingUnits(current);
	const int currentExposure = getEnemyFireExposure(current);
	const int currentFireLines = countEnemyFireLines(current);
	const int exitReserve = current.z > smokeTarget.z ? 0 : PLAYER_AI_SMOKE_EXIT_RESERVE_TU;
	const int moveBudget = std::max(0, _unit->getTimeUnits() - exitReserve);
	int bestScore = -100000;
	Position bestPos = current;
	int bestSpotters = currentSpotters;
	int bestExposure = currentExposure;
	int bestFireLines = currentFireLines;

	auto crowdingPenalty = [&](const Position &pos) -> int
	{
		int penalty = 0;
		for (auto *other : *_save->getUnits())
		{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
			{
				continue;
			}
			const int dist = Position::distance2d(pos, other->getPosition());
			if (dist <= 1)
			{
				penalty += 90;
			}
			else if (dist <= 2)
			{
				penalty += 25;
			}
		}
		return penalty;
	};

	for (auto tileIndex : _reachable)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		const Position pos = tile->getPosition();
		if (pos.z > current.z || isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
		{
			continue;
		}
		const int moveDist = Position::distance2d(pos, current);
		if (moveDist == 0 || moveDist > PLAYER_AI_SMOKE_EXIT_MOVE_DISTANCE || moveDist * 6 > moveBudget)
		{
			continue;
		}
		const int smokeDist = Position::distance2d(pos, smokeTarget);
		const bool levelExit = pos.z < current.z;
		const bool closerToSmoke = smokeDist < currentSmokeDist;
		if (!levelExit && !closerToSmoke)
		{
			continue;
		}
		const int spotters = getSpottingUnits(pos);
		const int exposure = getEnemyFireExposure(pos);
		const int fireLines = countEnemyFireLines(pos);
		const int contactDist = Position::distance2d(pos, facePos);
		if (spotters > currentSpotters + 1 && exposure > currentExposure + 40)
		{
			continue;
		}
		if (current.z <= smokeTarget.z
			&& contactDist > currentContactDist + 1
			&& exposure >= currentExposure
			&& fireLines >= currentFireLines
			&& spotters >= currentSpotters)
		{
			continue;
		}

		int score = 0;
		if (levelExit)
		{
			score += PLAYER_AI_SMOKE_EXIT_LOWER_LEVEL_BONUS;
		}
		score += std::max(0, currentSmokeDist - smokeDist) * 35;
		score += (currentContactDist - contactDist) * 12;
		score += moveDist * 10;
		score += (currentSpotters - spotters) * 90;
		score += currentExposure - exposure;
		score += (currentFireLines - fireLines) * 120;
		score -= crowdingPenalty(pos);
		if (smokeDist <= PLAYER_AI_SMOKE_MIN_RADIUS)
		{
			score += 35;
		}
		if (fireLines > currentFireLines)
		{
			score -= 120;
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestSpotters = spotters;
			bestExposure = exposure;
			bestFireLines = fireLines;
		}
	}

	if (bestPos == current || bestScore < PLAYER_AI_SMOKE_EXIT_MIN_SCORE)
	{
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = selectBestCarriedWeapon();
	_attackAction.type = BA_WALK;
	_attackAction.target = bestPos;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, facePos);
	_AIMode = AI_COMBAT;
	_factionSupportMoveAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction smoke exit move: unit=" << _unit->getId()
			<< ", smokeTarget=" << smokeTarget
			<< ", target=" << bestPos
			<< ", face=" << facePos
			<< ", score=" << bestScore
			<< ", spotters=" << currentSpotters << "->" << bestSpotters
			<< ", exposure=" << currentExposure << "->" << bestExposure
			<< ", fireLines=" << currentFireLines << "->" << bestFireLines;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupSmokeScreen()
{
	if (_unit->getFaction() != FACTION_PLAYER || !_factionAI || !_grenade)
	{
		return false;
	}
	if (_attackAction.type != BA_RETHINK)
	{
		return false;
	}
	cleanupPendingPlayerSmokePlans(_save);
	int smokePlansThisTurn = 0;
	for (const auto &plan : pendingPlayerSmokePlans)
	{
		if (plan.save == _save && plan.turn == _save->getTurn() && plan.faction == _unit->getFaction())
		{
			++smokePlansThisTurn;
		}
	}
	if (smokePlansThisTurn >= 2)
	{
		return false;
	}

	BattleItem *smoke = 0;
	int smokeRadius = 0;
	BattleAction smokeAction;
	for (auto *item : *_unit->getInventory())
	{
		if (!isPlayerSmokeGrenade(item))
		{
			continue;
		}
		BattleAction probe;
		probe.actor = _unit;
		probe.weapon = item;
		probe.type = BA_THROW;
		probe.updateTU();
		probe.Time += PLAYER_AI_PICKUP_TU_COST + PLAYER_AI_SMOKE_PICKUP_TU_BUFFER;
		probe += _unit->getActionTUs(BA_PRIME, item);
		if (!probe.haveTU() || _unit->getTimeUnits() - probe.Time < PLAYER_AI_SMOKE_MIN_THROW_RESERVE)
		{
			continue;
		}
		const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(probe));
		if (radius < PLAYER_AI_SMOKE_MIN_RADIUS)
		{
			continue;
		}
		if (!smoke || radius > smokeRadius)
		{
			smoke = item;
			smokeRadius = radius;
			smokeAction = probe;
		}
	}
	if (!smoke)
	{
		return false;
	}

	const PlayerFactionStrategy strategy = getFactionStrategy();
	Position contactPos;
	const BattleRoomInfo *contactRoom = 0;
	int enemiesInRoom = 0;
	bool visibleContact = false;
	const bool hasContact = _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleContact);
	if (!hasContact)
	{
		return false;
	}
	if (contactPos.z != _unit->getPosition().z)
	{
		Position projected(contactPos.x, contactPos.y, _unit->getPosition().z);
		if (!_save->getTile(projected))
		{
			return false;
		}
		contactPos = projected;
	}

	const Position current = _unit->getPosition();
	const int currentSpotters = getSpottingUnits(current);
	const int currentExposure = getEnemyFireExposure(current);
	const int currentFireLines = countEnemyFireLines(current);
	int activeAllies = 0;
	for (auto *other : *_save->getUnits())
	{
		if (other && !other->isOut() && other->getFaction() == _unit->getFaction())
		{
			++activeAllies;
		}
	}
	const bool directPressure = currentSpotters > 0 || currentFireLines > 0 || currentExposure >= PLAYER_AI_SMOKE_PRESSURE_EXPOSURE;
	const bool highPressure = currentExposure >= PLAYER_AI_SMOKE_HIGH_EXPOSURE || currentSpotters >= PLAYER_AI_NEARBY_SPOTTER_LIMIT || currentFireLines >= 2;
	const bool earlyDeploySmoke = _save->getTurn() <= PLAYER_AI_SMOKE_INITIAL_TURN_LIMIT
		&& _knownEnemies >= PLAYER_AI_SMOKE_INITIAL_KNOWN_ENEMIES
		&& !_visibleEnemies
		&& activeAllies >= PLAYER_AI_SMOKE_SUPPORT_IDLE_MIN_ALLIES;
	const bool roomBreachSmoke = strategy == PFS_SIEGE_ROOM
		&& contactRoom
		&& !contactRoom->isOutside
		&& contactRoom->tileCount <= PLAYER_AI_SMOKE_ROOM_TILE_LIMIT
		&& (int)contactRoom->entryPositions.size() > 0
		&& (int)contactRoom->entryPositions.size() <= PLAYER_AI_SMOKE_ROOM_ENTRY_LIMIT;

	bool allowedByStrategy = false;
	switch (strategy)
	{
	case PFS_INITIAL_DEPLOY:
	case PFS_DEFEND_LINE:
		allowedByStrategy = earlyDeploySmoke || highPressure;
		break;
	case PFS_SIEGE_ROOM:
		allowedByStrategy = roomBreachSmoke || highPressure;
		break;
	case PFS_SURVIVE:
	case PFS_RETREAT_REGROUP:
	case PFS_SKIRMISH:
		allowedByStrategy = highPressure;
		break;
	case PFS_HOLD_REACTION:
		allowedByStrategy = highPressure && !_visibleEnemies;
		break;
	case PFS_ASSAULT:
		allowedByStrategy = false;
		break;
	case PFS_HUNT_LAST_ENEMY:
	default:
		allowedByStrategy = false;
		break;
	}
	if (!allowedByStrategy)
	{
		return false;
	}
	if (_visibleEnemies > PLAYER_AI_SMOKE_GOOD_SHOT_VISIBLE_LIMIT && !highPressure)
	{
		return false;
	}

	std::vector<Position> candidates;
	auto addCandidate = [&](const Position &pos)
	{
		Tile *tile = _save->getTile(pos);
		if (!tile)
		{
			return;
		}
		if (tile->getSmoke() > PLAYER_AI_SMOKE_TARGET_SMOKE_LIMIT)
		{
			return;
		}
		if (isRecentPlayerSmokePlan(_save, _unit->getFaction(), contactPos, pos))
		{
			return;
		}
		for (const auto &existing : candidates)
		{
			if (existing == pos)
			{
				return;
			}
		}
		candidates.push_back(pos);
	};

	const int pathX = (contactPos.x > current.x) - (contactPos.x < current.x);
	const int pathY = (contactPos.y > current.y) - (contactPos.y < current.y);
	if (pathX != 0 || pathY != 0)
	{
		for (int step = 2; step <= std::min(8, std::max(2, Position::distance2d(current, contactPos) - 2)); ++step)
		{
			addCandidate(current + Position(pathX * step, pathY * step, 0));
		}
		const Position sideA(-pathY, pathX, 0);
		const Position sideB(pathY, -pathX, 0);
		const Position center = current + Position(pathX * std::min(5, std::max(2, Position::distance2d(current, contactPos) / 2)), pathY * std::min(5, std::max(2, Position::distance2d(current, contactPos) / 2)), 0);
		addCandidate(center + sideA);
		addCandidate(center + sideB);
	}
	if (earlyDeploySmoke && current.z > 0)
	{
		for (int dx = -PLAYER_AI_SMOKE_INITIAL_EXIT_SCAN_RADIUS; dx <= PLAYER_AI_SMOKE_INITIAL_EXIT_SCAN_RADIUS; ++dx)
		{
			for (int dy = -PLAYER_AI_SMOKE_INITIAL_EXIT_SCAN_RADIUS; dy <= PLAYER_AI_SMOKE_INITIAL_EXIT_SCAN_RADIUS; ++dy)
			{
				Position lower(current.x + dx, current.y + dy, current.z - 1);
				Tile *tile = _save->getTile(lower);
				const int lowerDistance = Position::distance2d(current, lower);
				if (!tile || lowerDistance < PLAYER_AI_SMOKE_INITIAL_EXIT_MIN_THROW_DISTANCE || lowerDistance > PLAYER_AI_SMOKE_INITIAL_EXIT_SCAN_RADIUS)
				{
					continue;
				}
				bool nearStartAlly = false;
				for (auto *ally : *_save->getUnits())
				{
					if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction())
					{
						continue;
					}
					if (ally->getPosition().z == current.z && Position::distance2d(ally->getPosition(), lower) <= PLAYER_AI_SMOKE_INITIAL_EXIT_MAX_ALLY_DISTANCE)
					{
						nearStartAlly = true;
						break;
					}
				}
				if (nearStartAlly)
				{
					addCandidate(lower);
				}
			}
		}
	}
	if (roomBreachSmoke && contactRoom)
	{
		for (const auto &entry : contactRoom->entryPositions)
		{
			if (entry.z == current.z)
			{
				addCandidate(entry);
				const int entryPathX = (contactPos.x > entry.x) - (contactPos.x < entry.x);
				const int entryPathY = (contactPos.y > entry.y) - (contactPos.y < entry.y);
				addCandidate(entry + Position(entryPathX, entryPathY, 0));
			}
		}
	}
	addCandidate(contactPos);

	int bestScore = PLAYER_AI_REJECT_SCORE;
	Position bestTarget;
	int bestThrowDistance = 0;
	int bestAllyPenalty = 0;
	int bestSmoke = 0;
	Position originVoxel = _save->getTileEngine()->getOriginVoxel(smokeAction, 0);
	for (const auto &target : candidates)
	{
		Tile *tile = _save->getTile(target);
		if (!tile)
		{
			continue;
		}
		const int throwDist = Position::distance2d(current, target);
		if (throwDist > PLAYER_AI_SMOKE_MAX_THROW_DISTANCE)
		{
			continue;
		}
		smokeAction.target = target;
		Position targetVoxel = target.toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY, (PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE + -tile->getTerrainLevel()));
		if (!_save->getTileEngine()->validateThrow(smokeAction, originVoxel, targetVoxel, _save->getDepth()))
		{
			continue;
		}
		int allyPenalty = 0;
		for (auto *ally : *_save->getUnits())
		{
			if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction() || ally->getPosition().z != target.z)
			{
				continue;
			}
			const int dist = Position::distance2d(ally->getPosition(), target);
			if (dist <= PLAYER_AI_SMOKE_ALLY_BLIND_RADIUS)
			{
				allyPenalty += earlyDeploySmoke ? PLAYER_AI_SMOKE_INITIAL_ALLY_BLIND_PENALTY : PLAYER_AI_SMOKE_ALLY_BLIND_PENALTY;
			}
		}
		int score = 0;
		score += currentSpotters * PLAYER_AI_SMOKE_SPOTTER_SCORE;
		score += currentFireLines * PLAYER_AI_SMOKE_FIRELINE_SCORE;
		score += currentExposure / PLAYER_AI_SMOKE_EXPOSURE_SCORE_DIVISOR;
		score += std::max(0, smokeRadius - Position::distance2d(target, contactPos)) * PLAYER_AI_SMOKE_CONTACT_DISTANCE_SCORE;
		score -= throwDist * PLAYER_AI_SMOKE_SELF_DISTANCE_PENALTY;
		score -= allyPenalty;
		score -= tile->getSmoke() * 10;
		if (earlyDeploySmoke)
		{
			score += PLAYER_AI_SMOKE_INITIAL_BONUS;
		}
		if (roomBreachSmoke)
		{
			score += PLAYER_AI_SMOKE_SIEGE_BONUS;
			if (contactRoom && std::find(contactRoom->entryPositions.begin(), contactRoom->entryPositions.end(), target) != contactRoom->entryPositions.end())
			{
				score += PLAYER_AI_SMOKE_ENTRY_BONUS;
			}
		}
		if (strategy == PFS_SURVIVE || strategy == PFS_RETREAT_REGROUP || strategy == PFS_SKIRMISH)
		{
			score += PLAYER_AI_SMOKE_SURVIVE_BONUS;
		}
		if (pathX != 0 || pathY != 0)
		{
			const int ux = target.x - current.x;
			const int uy = target.y - current.y;
			const int cx = contactPos.x - current.x;
			const int cy = contactPos.y - current.y;
			if ((ux * cx + uy * cy) > 0 && Position::distance2d(target, current) < Position::distance2d(contactPos, current))
			{
				score += PLAYER_AI_SMOKE_CENTER_PATH_BONUS;
			}
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestTarget = target;
			bestThrowDistance = throwDist;
			bestAllyPenalty = allyPenalty;
			bestSmoke = tile->getSmoke();
		}
	}

	const int requiredScore = roomBreachSmoke ? PLAYER_AI_SMOKE_BREACH_MIN_SCORE : (earlyDeploySmoke ? PLAYER_AI_SMOKE_INITIAL_MIN_SCORE : PLAYER_AI_SMOKE_MIN_SCORE);
	if (bestScore < requiredScore)
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction smoke skipped: unit=" << _unit->getId()
				<< ", reason=low_score"
				<< ", strategy=" << getFactionStrategyName(strategy)
				<< ", bestScore=" << bestScore
				<< ", requiredScore=" << requiredScore
				<< ", candidates=" << candidates.size()
				<< ", contact=" << contactPos
				<< ", roomBreach=" << roomBreachSmoke
				<< ", earlyDeploy=" << earlyDeploySmoke;
			_save->appendToAutoBattleLog(log.str());
		}
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = smoke;
	_attackAction.target = bestTarget;
	_attackAction.type = BA_THROW;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(current, contactPos);
	_AIMode = AI_COMBAT;
	recordPlayerSmokePlan(_save, _unit->getFaction(), contactPos, bestTarget);
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction smoke screen: unit=" << _unit->getId()
			<< ", item=" << smoke->getRules()->getType()
			<< ", target=" << bestTarget
			<< ", contact=" << contactPos
			<< ", score=" << bestScore
			<< ", requiredScore=" << requiredScore
			<< ", strategy=" << getFactionStrategyName(strategy)
			<< ", radius=" << smokeRadius
			<< ", throwDist=" << bestThrowDistance
			<< ", spotters=" << currentSpotters
			<< ", exposure=" << currentExposure
			<< ", fireLines=" << currentFireLines
			<< ", roomBreach=" << roomBreachSmoke
			<< ", earlyDeploy=" << earlyDeploySmoke
			<< ", targetSmoke=" << bestSmoke
			<< ", allyPenalty=" << bestAllyPenalty;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Decides if it worth our while to create an explosion here.
 * Return value in same range as number affected targets but not equal exactly to that value.
 * @param targetPos The target's position.
 * @param attackingUnit The attacking unit.
 * @param radius How big the explosion will be.
 * @param diff Game difficulty.
 * @param grenade Is the explosion coming from a grenade?
 * @return Value greater than zero if it is worthwhile creating an explosion in the target position. Bigger value better target.
 */
int PlayerFactionAI::explosiveEfficacy(Position targetPos, BattleUnit *attackingUnit, int radius, int diff, bool grenade) const
{
	Tile *targetTile = _save->getTile(targetPos);

	// don't throw grenades at flying enemies.
	if (grenade && targetPos.z > 0 && targetTile->hasNoFloor(_save))
	{
		return false;
	}

	if (diff == -1)
	{
		diff = _save->getBattleState()->getGame()->getSavedGame()->getDifficultyCoefficient();
	}
	int distance = Position::distance2d(attackingUnit->getPosition(), targetPos);
	int injurylevel = attackingUnit->getBaseStats()->health - attackingUnit->getHealth();
	int desperation = (100 - attackingUnit->getMorale()) / 10;
	int enemiesAffected = 0;
	// if we're below 1/3 health, let's assume things are dire, and increase desperation.
	if (injurylevel > (attackingUnit->getBaseStats()->health / 3) * 2)
		desperation += 3;

	int efficacy = AIW_SCALE * desperation;

	// don't go kamikaze unless we're already doomed.
	if (abs(attackingUnit->getPosition().z - targetPos.z) <= Options::battleExplosionHeight && distance <= radius)
	{
		if (attackingUnit->getFaction() == FACTION_PLAYER)
		{
			return 0;
		}
		efficacy -= AIW_SCALE * 4;
	}

	// allow difficulty to have its influence
	efficacy += AIW_SCALE * diff/2;

	// account for the unit we're targetting
	BattleUnit *target = targetTile->getUnit();
	if (target && !targetTile->getDangerous())
	{
		if (attackingUnit->getFaction() == FACTION_PLAYER && target->getFaction() == attackingUnit->getFaction())
		{
			return 0;
		}
		++enemiesAffected;
		efficacy += getTargetAttackWeight(target);
	}

	for (auto* bu : *_save->getUnits())
	{
			// don't grenade dead guys
		if (!bu->isOut() &&
			// don't count ourself twice
			bu != attackingUnit &&
			// don't count the target twice
			bu != target &&
			// don't count units that probably won't be affected cause they're out of range
			abs(bu->getPosition().z - targetPos.z) <= Options::battleExplosionHeight &&
			Position::distance2d(bu->getPosition(), targetPos) <= radius)
		{
			if (bu->getTile()->getDangerous())
			{
				// don't count people who were already grenaded this turn
				continue;
			}

			auto weight = getTargetAttackWeight(bu);

			if (weight == 0)
			{
				// AI do not know anything about this unit
				continue;
			}

			// trace a line from the grenade origin to the unit we're checking against
			Position voxelPosA = Position (targetPos.toVoxel() + TileEngine::voxelTileCenter);
			Position voxelPosB = Position (bu->getPosition().toVoxel() + TileEngine::voxelTileCenter);
			std::vector<Position> traj;
			int collidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosB, false, &traj, target, bu);

			if (collidesWith == V_UNIT && traj.front().toTile() == bu->getPosition())
			{
				if (attackingUnit->getFaction() == FACTION_PLAYER && bu->getFaction() == attackingUnit->getFaction())
				{
					return 0;
				}
				if (bu->getFaction() == _targetFaction)
				{
					++enemiesAffected;
				}

				efficacy += weight;
			}
		}
	}
	bool valuableSinglePlayerTarget = false;
	if (grenade && attackingUnit->getFaction() == FACTION_PLAYER && enemiesAffected == 1 && target && target->getFaction() != attackingUnit->getFaction())
	{
		BattleItem *enemyWeapon = target->getMainHandWeapon(false);
		const int enemyWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
		const int armor = std::max(std::max(target->getArmor(SIDE_FRONT), target->getArmor(SIDE_LEFT)), target->getArmor(SIDE_RIGHT));
		valuableSinglePlayerTarget = target->getHealth() >= 60 || armor >= std::max(1, grenade ? radius * 8 : 0) || enemyWeaponDanger >= 115;
	}
	// don't throw grenades at single weak targets, unless morale is in the danger zone
	// or we're halfway towards panicking while bleeding to death.
	if (grenade && desperation < 6 && enemiesAffected < 2 && !valuableSinglePlayerTarget)
	{
		return 0;
	}

	if (enemiesAffected >= 10)
	{
		// Ignore loses if we can kill lot of enemies.
		return enemiesAffected;
	}
	else if (efficacy > 0)
	{
		// We kill more enemies than allies. Scale back to number of targets, can round down to zero
		return efficacy / AIW_SCALE;
	}
	else
	{
		return 0;
	}
}

bool PlayerFactionAI::explosiveProjectileRiskyForAllies(BattleAction *action, int radius, const Position *originPosition, bool logRejection, std::string *rejectReason, bool *hardReject) const
{
	if (hardReject)
	{
		*hardReject = false;
	}
	if (!action || !action->actor || !action->weapon || radius <= 0 || action->actor->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	Tile *targetTile = _save->getTile(action->target);
	if (!targetTile)
	{
		if (rejectReason && rejectReason->empty())
		{
			*rejectReason = "missing_target_tile";
		}
		if (hardReject)
		{
			*hardReject = true;
		}
		return true;
	}

	BattleAction testAction = *action;
	Position actorPosition = originPosition ? *originPosition : action->actor->getPosition();
	Position originVoxel = _save->getTileEngine()->getOriginVoxel(testAction, _save->getTile(actorPosition));
	const bool thrownProjectile = action->type == BA_THROW;
	Position targetVoxel = thrownProjectile
		? action->target.toVoxel() + Position(8, 8, 1 + -targetTile->getTerrainLevel())
		: action->target.toVoxel() + TileEngine::voxelTileCenter;

	BattleUnit *targetUnit = targetTile->getUnit();
	if (!thrownProjectile)
	{
		if (targetUnit && targetUnit != action->actor)
		{
			_save->getTileEngine()->canTargetUnit(&originVoxel, targetTile, &targetVoxel, action->actor, false, targetUnit);
		}
		else if (targetTile->getMapData(O_OBJECT) != 0)
		{
			_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_OBJECT, &targetVoxel, action->actor, false);
		}
		else if (targetTile->getMapData(O_NORTHWALL) != 0)
		{
			_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_NORTHWALL, &targetVoxel, action->actor, false);
		}
		else if (targetTile->getMapData(O_WESTWALL) != 0)
		{
			_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_WESTWALL, &targetVoxel, action->actor, false);
		}
		else if (targetTile->getMapData(O_FLOOR) != 0)
		{
			_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_FLOOR, &targetVoxel, action->actor, false);
		}
	}

	double throwCurvature = 0.0;
	if (thrownProjectile)
	{
		int throwTest = V_OUTOFBOUNDS;
		if (!_save->getTileEngine()->validateThrow(testAction, originVoxel, targetVoxel, _save->getDepth(), &throwCurvature, &throwTest))
		{
			if (rejectReason && rejectReason->empty())
			{
				*rejectReason = "no_throw_path";
			}
			if (hardReject)
			{
				*hardReject = true;
			}
			return true;
		}
	}

	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(*action);
	constexpr int projectileAccuracyPercent = 100;
	double accuracy = BattleUnit::getFiringAccuracy(attack, _save->getMod()) / (double)projectileAccuracyPercent;
	if (action->actor->getMorale() < 50)
	{
		// Panic-suppressed autobattle units can still use the same half-accuracy
		// projectile path as a berserk action.  Model that tail instead of letting
		// a morale-zero soldier throw HE as if fully composed.
		accuracy *= 0.5;
	}
	int upperLimit = 0;
	int lowerLimit = 0;
	int dropoff = action->weapon->getRules()->calculateLimits(upperLimit, lowerLimit, _save->getDepth(), action->type);
	const int xdiff = originVoxel.x - targetVoxel.x;
	const int ydiff = originVoxel.y - targetVoxel.y;
	const int zdiff = originVoxel.z - targetVoxel.z;
	const double realDistance = sqrt((double)(xdiff * xdiff) + (double)(ydiff * ydiff) + (double)(zdiff * zdiff));
	const double distanceTiles = realDistance / 16.0;
	if (distanceTiles > upperLimit)
	{
		accuracy = std::max(0.0, accuracy - (dropoff * (distanceTiles - upperLimit)) / (double)projectileAccuracyPercent);
	}
	else if (distanceTiles < lowerLimit)
	{
		accuracy = std::max(0.0, accuracy - (dropoff * (lowerLimit - distanceTiles)) / (double)projectileAccuracyPercent);
	}

	const int xDist = abs(originVoxel.x - targetVoxel.x);
	const int yDist = abs(originVoxel.y - targetVoxel.y);
	const int zDist = abs(originVoxel.z - targetVoxel.z);
	int xyShift;
	if (Options::oxceUniformShootingSpread)
	{
		if (xDist <= yDist)
			xyShift = xDist / 4 + yDist;
		else
			xyShift = xDist + yDist / 4;
		xyShift = (int)(xyShift * 0.839);
	}
	else
	{
		if (xDist / 2 <= yDist)
			xyShift = xDist / 4 + yDist;
		else
			xyShift = (xDist + yDist) / 2;
	}
	const int zShift = (xyShift <= zDist) ? (xyShift / 2 + zDist) : (xyShift + zDist / 2);
	int worstDeviationRoll = projectileAccuracyPercent - (int)(accuracy * projectileAccuracyPercent);
	if (worstDeviationRoll >= 0)
		worstDeviationRoll += 50;
	else
		worstDeviationRoll += 10;
	const int deviation = std::max(1, zShift * worstDeviationRoll / 200);
	const int quarterDeviation = std::max(1, deviation / 4);
	const int halfDeviation = std::max(1, deviation / 2);
	const int diagonalDeviation = std::max(1, deviation / 3);
	const int verticalDeviation = std::max(1, deviation / 8);
	bool probeNearCentralThrow = false;
	if (thrownProjectile && !isPlayerSmokeGrenade(action->weapon))
	{
		const Position throwDelta = action->target - actorPosition;
		const int throwSpan = std::max(abs(throwDelta.x), abs(throwDelta.y));
		const Position corridorOffsets[] = {
			Position(0, 0, 0), Position(1, 0, 0), Position(-1, 0, 0),
			Position(0, 1, 0), Position(0, -1, 0)
		};
		for (int step = 1; step <= std::min(4, throwSpan - 1) && !probeNearCentralThrow; ++step)
		{
			const Position corridorCenter(actorPosition.x + throwDelta.x * step / throwSpan,
				actorPosition.y + throwDelta.y * step / throwSpan, actorPosition.z);
			for (const auto &offset : corridorOffsets)
			{
				Tile *corridorTile = _save->getTile(corridorCenter + offset);
				if (corridorTile && corridorTile->getMapData(O_OBJECT))
				{
					probeNearCentralThrow = true;
					break;
				}
			}
		}
	}

	std::vector<Position> targetSamples;
	targetSamples.push_back(targetVoxel);
	size_t nearCentralThrowSampleEnd = targetSamples.size();
	if (thrownProjectile && probeNearCentralThrow)
	{
		// A nearly perfect throw can be less safe than either the central arc or
		// the edge of the miss cloud.  A one- or two-voxel angular change may clip
		// the top of nearby cover and drop a fuse-0 grenade beside the thrower.
		// Explicitly probe that discontinuity; boundary-only sampling misses it.
		const int nearDeviations[] = { 1, std::min(2, halfDeviation) };
		int previousNearDeviation = 0;
		for (int nearDeviation : nearDeviations)
		{
			if (nearDeviation <= 0 || nearDeviation == previousNearDeviation)
			{
				continue;
			}
			previousNearDeviation = nearDeviation;
			targetSamples.push_back(targetVoxel + Position(nearDeviation, 0, 0));
			targetSamples.push_back(targetVoxel + Position(-nearDeviation, 0, 0));
			targetSamples.push_back(targetVoxel + Position(0, nearDeviation, 0));
			targetSamples.push_back(targetVoxel + Position(0, -nearDeviation, 0));
			targetSamples.push_back(targetVoxel + Position(nearDeviation, nearDeviation, 0));
			targetSamples.push_back(targetVoxel + Position(nearDeviation, -nearDeviation, 0));
			targetSamples.push_back(targetVoxel + Position(-nearDeviation, nearDeviation, 0));
			targetSamples.push_back(targetVoxel + Position(-nearDeviation, -nearDeviation, 0));
		}
		nearCentralThrowSampleEnd = targetSamples.size();
	}
	targetSamples.push_back(targetVoxel + Position(quarterDeviation, 0, 0));
	targetSamples.push_back(targetVoxel + Position(-quarterDeviation, 0, 0));
	targetSamples.push_back(targetVoxel + Position(0, quarterDeviation, 0));
	targetSamples.push_back(targetVoxel + Position(0, -quarterDeviation, 0));
	targetSamples.push_back(targetVoxel + Position(0, 0, verticalDeviation));
	targetSamples.push_back(targetVoxel + Position(0, 0, -verticalDeviation));
	if (thrownProjectile)
	{
		// Projectile::applyAccuracy treats deviation as the diameter of the XY
		// cloud.  Sample the real boundary and representative diagonals; the old
		// full-deviation/Z samples described trajectories the engine cannot roll.
		targetSamples.push_back(targetVoxel + Position(halfDeviation, 0, 0));
		targetSamples.push_back(targetVoxel + Position(-halfDeviation, 0, 0));
		targetSamples.push_back(targetVoxel + Position(0, halfDeviation, 0));
		targetSamples.push_back(targetVoxel + Position(0, -halfDeviation, 0));
		// A one-third component on each axis stays inside the uniform spread
		// circle while covering the diagonal boundary much better than 1/4.
		targetSamples.push_back(targetVoxel + Position(diagonalDeviation, diagonalDeviation, 0));
		targetSamples.push_back(targetVoxel + Position(diagonalDeviation, -diagonalDeviation, 0));
		targetSamples.push_back(targetVoxel + Position(-diagonalDeviation, diagonalDeviation, 0));
		targetSamples.push_back(targetVoxel + Position(-diagonalDeviation, -diagonalDeviation, 0));
		const int xyBoundary[] = { -halfDeviation, halfDeviation };
		const int zBoundary[] = { -verticalDeviation, verticalDeviation };
		for (int xy : xyBoundary)
		{
			for (int z : zBoundary)
			{
				targetSamples.push_back(targetVoxel + Position(xy, 0, z));
				targetSamples.push_back(targetVoxel + Position(0, xy, z));
			}
		}
	}

	auto blastWouldHitAlly = [&](const Position &impactTile) -> bool
	{
		for (auto* unit : *_save->getUnits())
		{
			if (!unit || unit->getStatus() == STATUS_DEAD || unit->getStatus() == STATUS_IGNORE_ME
				|| (unit->getFaction() != action->actor->getFaction() && unit->getOriginalFaction() != FACTION_PLAYER))
			{
				continue;
			}
			Position unitPosition = (unit == action->actor) ? actorPosition : unit->getPosition();
			const int blastDx = std::abs(unitPosition.x - impactTile.x);
			const int blastDy = std::abs(unitPosition.y - impactTile.y);
			if (abs(unitPosition.z - impactTile.z) <= Options::battleExplosionHeight
				&& std::max(blastDx, blastDy) <= radius)
			{
				return true;
			}
		}
		return false;
	};

	int riskySamples = 0;
	int checkedSamples = 0;
	bool invalidThrowSample = false;
	std::string firstReason;
	Position firstImpactTile;
	int firstImpact = V_EMPTY;
	for (size_t i = 0; i < targetSamples.size(); ++i)
	{
		const auto &sample = targetSamples[i];
		std::vector<Position> trajectory;
		int impact = V_EMPTY;
		if (thrownProjectile)
		{
			const Position delta = sample - targetVoxel;
			impact = _save->getTileEngine()->calculateParabolaVoxel(originVoxel, targetVoxel, true, &trajectory, action->actor, throwCurvature, delta);
		}
		else
		{
			impact = _save->getTileEngine()->calculateLineVoxel(originVoxel, sample, true, &trajectory, action->actor);
		}
		Position impactTile = sample.toTile();
		if (thrownProjectile && !trajectory.empty())
		{
			impactTile = Projectile::getPositionFromEnd(trajectory, Projectile::ItemDropVoxelOffset).toTile();
		}
		else if (impact != V_EMPTY && !trajectory.empty())
		{
			impactTile = trajectory.front().toTile();
		}
		bool risky = false;
		if (thrownProjectile && impact != V_FLOOR && impact != V_UNIT && impact != V_OBJECT
			&& (i == 0 || impact != V_OUTOFBOUNDS))
		{
			// ProjectileFlyBState refuses these impacts after the AI has already
			// primed the grenade, leaving a fuse-0 item in the carrier's inventory.
			risky = true;
			invalidThrowSample = true;
			if (firstReason.empty())
			{
				firstReason = "throw_can_fail_and_retain_fuse";
				firstImpactTile = impactTile;
				firstImpact = impact;
			}
		}
		for (const auto &voxel : trajectory)
		{
			Tile *tile = _save->getTile(voxel.toTile());
			BattleUnit *unit = tile ? tile->getUnit() : 0;
			if (unit && unit != action->actor && unit->getFaction() == action->actor->getFaction())
			{
				risky = true;
				if (firstReason.empty())
				{
					firstReason = "ally_on_trajectory";
					firstImpactTile = voxel.toTile();
					firstImpact = impact;
				}
				break;
			}
		}
		if (!risky && impact != V_EMPTY && blastWouldHitAlly(impactTile))
		{
			risky = true;
			if (firstReason.empty())
			{
				firstReason = "ally_in_impact_blast";
				firstImpactTile = impactTile;
				firstImpact = impact;
			}
		}
		if (risky && thrownProjectile && i > 0 && i < nearCentralThrowSampleEnd)
		{
			// These samples represent common near-centre rolls, not a remote tail.
			// Never trade a squad casualty for explosive efficacy when a tiny
			// deviation can make the grenade hit nearby cover.
			if (logRejection && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction explosive shot rejected: unit=" << action->actor->getId()
					<< ", origin=" << actorPosition
					<< ", target=" << action->target
					<< ", radius=" << radius
					<< ", accuracy=" << (int)(accuracy * projectileAccuracyPercent)
					<< ", reason=" << (firstReason.empty() ? "near_central_scatter_risk" : firstReason)
					<< ", impact=" << impact
					<< ", impactTile=" << impactTile
					<< ", nearCentral=true";
				_save->appendToAutoBattleLog(log.str());
			}
			if (rejectReason && rejectReason->empty())
			{
				*rejectReason = firstReason.empty() ? "near_central_scatter_risk" : firstReason;
			}
			if (hardReject)
			{
				*hardReject = true;
			}
			return true;
		}
		if (risky)
		{
			if (i == 0)
			{
				bool centralHardReject = firstReason == "ally_on_trajectory"
					|| firstReason == "ally_in_impact_blast"
					|| firstReason == "throw_can_fail_and_retain_fuse"
					|| firstImpactTile == actorPosition;
				if (centralHardReject && logRejection && Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction explosive shot rejected: unit=" << action->actor->getId()
						<< ", origin=" << actorPosition
						<< ", target=" << action->target
						<< ", radius=" << radius
						<< ", accuracy=" << (int)(accuracy * projectileAccuracyPercent)
						<< ", reason=" << firstReason
						<< ", impact=" << firstImpact
						<< ", impactTile=" << firstImpactTile
						<< ", central=true";
					_save->appendToAutoBattleLog(log.str());
				}
				if (centralHardReject)
				{
					if (rejectReason && rejectReason->empty())
					{
						*rejectReason = firstReason.empty() ? "hard_projectile_risk" : firstReason;
					}
					if (hardReject)
					{
						*hardReject = true;
					}
					return true;
				}
			}
			++riskySamples;
		}
		++checkedSamples;
	}
	const bool riskyBySpread = thrownProjectile
		? riskySamples > 0
		: riskySamples * 2 >= std::max(1, checkedSamples - 1);
	if (invalidThrowSample)
	{
		if (rejectReason)
		{
			*rejectReason = "throw_can_fail_and_retain_fuse";
		}
		if (hardReject)
		{
			*hardReject = true;
		}
		return true;
	}
	if (riskyBySpread && logRejection && Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction explosive shot rejected: unit=" << action->actor->getId()
			<< ", origin=" << actorPosition
			<< ", target=" << action->target
			<< ", radius=" << radius
			<< ", accuracy=" << (int)(accuracy * projectileAccuracyPercent)
			<< ", reason=" << firstReason
			<< ", impact=" << firstImpact
			<< ", impactTile=" << firstImpactTile
			<< ", riskySamples=" << riskySamples
			<< ", checkedSamples=" << checkedSamples;
		_save->appendToAutoBattleLog(log.str());
	}
	if (riskyBySpread && rejectReason && rejectReason->empty())
	{
		*rejectReason = firstReason.empty() ? "scatter_risk" : firstReason;
	}
	return riskyBySpread;
}

bool PlayerFactionAI::directProjectileRiskyForAllies(BattleAction *action, BattleUnit *target, bool logRejection) const
{
	if (!action || !action->actor || !action->weapon || !target || action->actor->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
	{
		return false;
	}

	const Position from = action->actor->getPosition();
	const Position to = target->getPosition();
	const double vx = (double)(to.x - from.x);
	const double vy = (double)(to.y - from.y);
	const double lenSq = vx * vx + vy * vy;
	auto protectedAlly = [&](BattleUnit *unit) -> bool
	{
		return unit && unit != action->actor && unit != target && !unit->isOut()
			&& (unit->getFaction() == action->actor->getFaction() || unit->getOriginalFaction() == FACTION_PLAYER);
	};
	if (lenSq > PLAYER_AI_GEOMETRY_EPSILON)
	{
		for (auto *ally : *_save->getUnits())
		{
			if (!protectedAlly(ally))
			{
				continue;
			}
			if (ally->getPosition().z != from.z)
			{
				continue;
			}
			const double ax = (double)(ally->getPosition().x - from.x);
			const double ay = (double)(ally->getPosition().y - from.y);
			const double t = (ax * vx + ay * vy) / lenSq;
			if (t <= 0.0 || t >= 1.05)
			{
				continue;
			}
			const double dx = ax - vx * t;
			const double dy = ay - vy * t;
			const double lateralSq = dx * dx + dy * dy;
			// Accuracy does not make an ally physically narrower.  Aimed fire must
			// never pass a lane that the snap-shot guard already considers unsafe.
			const double directLaneThreshold = 1.45;
			if (lateralSq <= directLaneThreshold)
			{
				if (logRejection && Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction direct shot rejected: unit=" << action->actor->getId()
						<< ", targetUnit=" << target->getId()
						<< ", target=" << action->target
						<< ", fireMode=" << (int)action->type
						<< ", ally=" << ally->getId()
						<< ", allyPos=" << ally->getPosition()
						<< ", lateralSq=" << (int)(lateralSq * 100)
						<< ", reason=ally_in_direct_fire_lane";
					_save->appendToAutoBattleLog(log.str());
				}
				return true;
			}
		}
	}

	Position originVoxel = _save->getTileEngine()->getOriginVoxel(*action, _save->getTile(action->actor->getPosition()));
	Position targetVoxel;
	if (!_save->getTileEngine()->canTargetUnit(&originVoxel, target->getTile(), &targetVoxel, action->actor, false, target))
	{
		return false;
	}

	std::vector<Position> trajectory;
	int impact = _save->getTileEngine()->calculateLineVoxel(originVoxel, targetVoxel, true, &trajectory, action->actor);
	for (const auto &voxel : trajectory)
	{
		Tile *tile = _save->getTile(voxel.toTile());
		BattleUnit *unit = tile ? tile->getOverlappingUnit(_save) : 0;
		if (!unit || unit == action->actor || unit == target)
		{
			Position tilePos = voxel.toTile();
			for (auto *ally : *_save->getUnits())
			{
				if (!protectedAlly(ally))
				{
					continue;
				}
				if (ally->getPosition().z == tilePos.z
					&& Position::distance2d(ally->getPosition(), tilePos) <= 0)
				{
					if (logRejection && Options::autoBattleLog)
					{
						std::ostringstream log;
						log << "Player faction direct shot rejected: unit=" << action->actor->getId()
							<< ", targetUnit=" << target->getId()
							<< ", target=" << action->target
							<< ", fireMode=" << (int)action->type
							<< ", ally=" << ally->getId()
							<< ", allyPos=" << ally->getPosition()
							<< ", voxel=" << voxel
							<< ", impact=" << impact
							<< ", reason=ally_near_trajectory";
						_save->appendToAutoBattleLog(log.str());
					}
					return true;
				}
			}
			continue;
		}
		if (unit->getFaction() == action->actor->getFaction() || unit->getOriginalFaction() == FACTION_PLAYER)
		{
			if (logRejection && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction direct shot rejected: unit=" << action->actor->getId()
					<< ", targetUnit=" << target->getId()
					<< ", target=" << action->target
					<< ", fireMode=" << (int)action->type
					<< ", ally=" << unit->getId()
					<< ", allyPos=" << unit->getPosition()
					<< ", voxel=" << voxel
					<< ", impact=" << impact
					<< ", reason=ally_on_trajectory";
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
	}

	return false;
}

bool PlayerFactionAI::autoShotRiskyForAllies(BattleAction *action, BattleUnit *target, bool logRejection) const
{
	if (!action || !action->actor || !action->weapon || !target || action->type != BA_AUTOSHOT || action->actor->getFaction() != FACTION_PLAYER)
	{
		return false;
	}
	if (action->weapon->getArcingShot(action->type))
	{
		return false;
	}

	const Position from = action->actor->getPosition();
	const Position to = target->getPosition();
	const double vx = (double)(to.x - from.x);
	const double vy = (double)(to.y - from.y);
	const double lenSq = vx * vx + vy * vy;
	if (lenSq < PLAYER_AI_GEOMETRY_EPSILON)
	{
		return false;
	}

	for (auto *ally : *_save->getUnits())
	{
		if (!ally || ally == action->actor || ally == target || ally->isOut()
			|| (ally->getFaction() != action->actor->getFaction() && ally->getOriginalFaction() != FACTION_PLAYER))
		{
			continue;
		}
		if (ally->getPosition().z != from.z)
		{
			continue;
		}
		const double ax = (double)(ally->getPosition().x - from.x);
		const double ay = (double)(ally->getPosition().y - from.y);
		const double t = (ax * vx + ay * vy) / lenSq;
		if (t <= 0.0 || t >= 1.15)
		{
			continue;
		}
		const double closestX = vx * t;
		const double closestY = vy * t;
		const double dx = ax - closestX;
		const double dy = ay - closestY;
		const double lateralSq = dx * dx + dy * dy;
		const int actorDist = Position::distance2d(from, ally->getPosition());
		if (lateralSq <= 2.25 || (actorDist <= 2 && t > 0.0))
		{
			if (logRejection && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction auto shot rejected: unit=" << action->actor->getId()
					<< ", targetUnit=" << target->getId()
					<< ", target=" << action->target
					<< ", ally=" << ally->getId()
					<< ", allyPos=" << ally->getPosition()
					<< ", lateralSq=" << (int)(lateralSq * 100)
					<< ", actorDist=" << actorDist
					<< ", reason=ally_in_auto_fire_cone";
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
	}

	return false;
}

bool PlayerFactionAI::projectileRiskyForAllies(BattleAction *action, BattleUnit *target, bool logRejection) const
{
	return directProjectileRiskyForAllies(action, target, logRejection) || autoShotRiskyForAllies(action, target, logRejection);
}

/**
 * Attempts to take a melee attack/charge an enemy we can see.
 * Melee targetting: we can see an enemy, we can move to it so we're charging blindly toward an enemy.
 */
void PlayerFactionAI::meleeAction()
{
	BattleActionCost attackCost(BA_HIT, _unit, _unit->getUtilityWeapon(BT_MELEE));
	if (!attackCost.haveTU())
	{
		// cannot make a melee attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	if (_aggroTarget != 0 && !_aggroTarget->isOut())
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
			return;
		}
	}
	int chargeReserve = std::min(_unit->getTimeUnits() - attackCost.Time, 2 * (_unit->getEnergy() - attackCost.Energy));
	int distance = (chargeReserve / 4) + 1;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		int newDistance = Position::distance2d(_unit->getPosition(), bu->getPosition());
		if (newDistance > 20 ||
			!validTarget(bu, true, true))
			continue;
		//pick closest living unit that we can move to
		if ((newDistance < distance || newDistance == 1) && !bu->isOut())
		{
			if (newDistance == 1 || selectPointNearTarget(bu, chargeReserve))
			{
				_aggroTarget = bu;
				_attackAction.type = BA_WALK;
				_unit->setCharging(_aggroTarget);
				distance = newDistance;
			}

		}
	}
	if (_aggroTarget != 0)
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
		}
	}
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "PlayerFactionAI::meleeAction:" << " [target]: " << (_aggroTarget->getId()) << " at: "  << _attackAction.target; }
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "CHARGE!"; }
}

/**
 * Attempts to take a melee attack/charge an enemy we can see.
 * Melee targetting: we can see an enemy, we can move to it so we're charging blindly toward an enemy.
 * Note: Differs from meleeAction() in calling selectPointNearTargetLeeroy() and ignoring some more checks.
 */
void PlayerFactionAI::meleeActionLeeroy(bool canRun)
{
	if (_aggroTarget != 0 && !_aggroTarget->isOut())
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
			return;
		}
	}
	int distance = 1000;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		int newDistance = Position::distance2d(_unit->getPosition(), bu->getPosition());
		if (!validTarget(bu, true, true))
			continue;
		//pick closest living unit
		if ((newDistance < distance || newDistance == 1) && !bu->isOut())
		{
			if (newDistance == 1 || selectPointNearTargetLeeroy(bu, canRun))
			{
				_aggroTarget = bu;
				_attackAction.type = BA_WALK;
				_attackAction.run = canRun;
				_unit->setCharging(_aggroTarget);
				distance = newDistance;
			}

		}
	}
	if (_aggroTarget != 0)
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
		}
	}
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "PlayerFactionAI::meleeAction:" << " [target]: " << (_aggroTarget->getId()) << " at: " << _attackAction.target; }
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "CHARGE!"; }
}

/**
 * Attempts to fire a waypoint projectile at an enemy we, or one of our teammates sees.
 *
 * Waypoint targeting: pick from any units currently spotted by our allies.
 */
void PlayerFactionAI::wayPointAction()
{
	BattleActionCost attackCost(BA_LAUNCH, _unit, _attackAction.weapon);
	if (!attackCost.haveTU())
	{
		// cannot make a launcher attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (_aggroTarget != 0) break; // loop finished
		if (!validTarget(bu, true, true))
		{
			continue;
		}
		_save->getPathfinding()->calculate(_unit, bu->getPosition(), BAM_MISSILE, bu, -1);
		BattleItem* ammo = _attackAction.weapon->getAmmoForAction(BA_LAUNCH);
		if (_save->getPathfinding()->getStartDirection() != -1 &&
			explosiveEfficacy(bu->getPosition(), _unit, ammo->getRules()->getExplosionRadius({ BA_LAUNCH, _unit, _attackAction.weapon, ammo }), _attackAction.diff))
		{
			_aggroTarget = bu;
		}
		_save->getPathfinding()->abortPath();
	}

	if (_aggroTarget != 0)
	{
		_attackAction.type = BA_LAUNCH;
		_attackAction.updateTU();
		if (!_attackAction.haveTU())
		{
			_attackAction.type = BA_RETHINK;
			return;
		}
		_attackAction.waypoints.clear();

		int PathDirection;
		int CollidesWith;
		int maxWaypoints = _attackAction.weapon->getCurrentWaypoints();
		if (maxWaypoints == -1)
		{
			maxWaypoints = 6 + (_attackAction.diff * 2);
		}
		Position LastWayPoint = _unit->getPosition();
		Position LastPosition = _unit->getPosition();
		Position CurrentPosition = _unit->getPosition();
		Position DirectionVector;

		_save->getPathfinding()->calculate(_unit, _aggroTarget->getPosition(), BAM_MISSILE, _aggroTarget, -1);
		PathDirection = _save->getPathfinding()->dequeuePath();
		while (PathDirection != -1 && (int)_attackAction.waypoints.size() < maxWaypoints)
		{
			LastPosition = CurrentPosition;
			_save->getPathfinding()->directionToVector(PathDirection, &DirectionVector);
			CurrentPosition = CurrentPosition + DirectionVector;
			Position voxelPosA ((CurrentPosition.x * 16)+8, (CurrentPosition.y * 16)+8, (CurrentPosition.z * 24)+16);
			Position voxelPosb ((LastWayPoint.x * 16)+8, (LastWayPoint.y * 16)+8, (LastWayPoint.z * 24)+16);
			CollidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosb, false, nullptr, _unit);
			if (CollidesWith > V_EMPTY && CollidesWith < V_UNIT)
			{
				_attackAction.waypoints.push_back(LastPosition);
				LastWayPoint = LastPosition;
			}
			else if (CollidesWith == V_UNIT)
			{
				BattleUnit* target = _save->getTile(CurrentPosition)->getOverlappingUnit(_save);
				if (target == _aggroTarget)
				{
					_attackAction.waypoints.push_back(CurrentPosition);
					LastWayPoint = CurrentPosition;
				}
			}
			PathDirection = _save->getPathfinding()->dequeuePath();
		}
		_attackAction.target = _attackAction.waypoints.front();
		if (LastWayPoint != _aggroTarget->getPosition())
		{
			_attackAction.type = BA_RETHINK;
		}
	}
}

/**
 * Attempts to fire at an enemy spotted for us.
 *
 */
bool PlayerFactionAI::sniperAction()
{
	if (_traceAI) { Log(LOG_INFO) << "Attempting sniper action..."; }

	if (selectSpottedUnitForSniper())
	{
		_visibleEnemies = std::max(_visibleEnemies, 1); // Make sure we count at least our target as visible, otherwise we might not shoot!
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
			if (_traceAI) { Log(LOG_INFO) << "Sniper action rejected because friendly unit is too close to the firing line."; }
			return false;
		}

		if (_traceAI) { Log(LOG_INFO) << "Target for sniper found at (" << _attackAction.target.x << "," << _attackAction.target.y << "," << _attackAction.target.z << ")."; }
		return true;
	}

	if (_traceAI) { Log(LOG_INFO) << "No valid target found or not enough TUs for sniper action."; }
	return false;
}

/**
 * Attempts to fire at an enemy we can see.
 *
 * Regular targeting: we can see an enemy, we have a gun, let's try to shoot.
 */
void PlayerFactionAI::projectileAction()
{
	_attackAction.target = _aggroTarget->getPosition();
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		Tile *targetTile = _aggroTarget->getTile();
		Position originVoxel = _save->getTileEngine()->getOriginVoxel(_attackAction, _save->getTile(_unit->getPosition()));
		Position targetVoxel;
		bool factionVisible = !_factionAI;
		if (_factionAI)
		{
			for (BattleUnit *ally : *_save->getUnits())
			{
				if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction())
				{
					continue;
				}
				const std::vector<BattleUnit*> *visibleUnits = ally->getVisibleUnits();
				if (visibleUnits && std::find(visibleUnits->begin(), visibleUnits->end(), _aggroTarget) != visibleUnits->end())
				{
					factionVisible = true;
					break;
				}
			}
		}
		if (!factionVisible || !targetTile
			|| !_save->getTileEngine()->canTargetUnit(&originVoxel, targetTile, &targetVoxel, _unit, false, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction direct attack rejected: unit=" << _unit->getId()
					<< ", targetUnit=" << _aggroTarget->getId()
					<< ", target=" << _aggroTarget->getPosition()
					<< ", weapon=" << (_attackAction.weapon ? _attackAction.weapon->getRules()->getType() : "none")
					<< ", factionVisible=" << factionVisible
					<< ", reason=" << (!factionVisible ? "hidden_contact_not_a_direct_fire_target" : "no_current_weapon_line_of_fire");
				_save->appendToAutoBattleLog(log.str());
			}
			return;
		}
	}
	int distance = Position::distance2d(_unit->getPosition(), _attackAction.target);
	auto testEffect = [&](BattleActionCost& cost)
	{
		if (cost.haveTU())
		{
			BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(cost);
			if (attack.damage_item == nullptr)
			{
				cost.clearTU();
			}
			else
			{
				int radius = attack.damage_item->getRules()->getExplosionRadius(attack);
				BattleAction riskAction = _attackAction;
				riskAction.type = cost.type;
				riskAction.actor = cost.actor;
				riskAction.weapon = cost.weapon;
				if (radius != 0 && explosiveProjectileRiskyForAllies(&riskAction, radius))
				{
					cost.clearTU();
				}
				else if (radius != 0 && explosiveEfficacy(_attackAction.target, _unit, radius, _attackAction.diff) == 0)
				{
					cost.clearTU();
				}
				else if (radius == 0 && directProjectileRiskyForAllies(&riskAction, _aggroTarget))
				{
					cost.clearTU();
				}
				else if (radius == 0 && autoShotRiskyForAllies(&riskAction, _aggroTarget))
				{
					cost.clearTU();
				}
			}
		}
	};

	_attackAction.type = BA_RETHINK;

	BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, _attackAction.weapon);

	testEffect(costAuto);
	testEffect(costSnap);
	testEffect(costAimed);

	// Is the unit willingly waiting outside of weapon's range (e.g. ninja camouflaged in ambush)?
	bool waitIfOutsideWeaponRange = _unit->getGeoscapeSoldier() ? false : _unit->getUnitRules()->waitIfOutsideWeaponRange();

	// Do we want to use the extended firing mode scoring?
	bool extendedFireModeChoiceEnabled = _save->getMod()->getAIExtendedFireModeChoice();
	if (!waitIfOutsideWeaponRange && extendedFireModeChoiceEnabled)
	{
		// Note: this will also check for the weapon's max range
		BattleActionCost costThrow; // Not actually checked here, just passed to extendedFireModeChoice as a necessary argument
		extendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, false);
		if (_attackAction.type != BA_RETHINK && projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}

	// Do we want to check if the weapon is in range?
	bool aiRespectsMaxRange = _save->getMod()->getAIRespectMaxRange();
	if (!waitIfOutsideWeaponRange && aiRespectsMaxRange)
	{
		// If we want to check and it's not in range, perhaps we should re-think shooting
		int distanceSq = _unit->distance3dToPositionSq(_attackAction.target);
		if (_attackAction.weapon->getRules()->isOutOfRange(distanceSq))
		{
			return;
		}
	}

	// vanilla
	if (distance < 4)
	{
		if (costAuto.haveTU())
		{
			_attackAction.type = BA_AUTOSHOT;
			if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
			{
				_attackAction.type = BA_RETHINK;
			}
			return;
		}
		if (!costSnap.haveTU())
		{
			if (costAimed.haveTU())
			{
				_attackAction.type = BA_AIMEDSHOT;
				if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
				{
					_attackAction.type = BA_RETHINK;
				}
			}
			return;
		}
		_attackAction.type = BA_SNAPSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}


	if (distance > 12)
	{
		if (costAimed.haveTU())
		{
			_attackAction.type = BA_AIMEDSHOT;
			if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
			{
				_attackAction.type = BA_RETHINK;
			}
			return;
		}
		if (distance < 20 && costSnap.haveTU())
		{
			_attackAction.type = BA_SNAPSHOT;
			if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
			{
				_attackAction.type = BA_RETHINK;
			}
			return;
		}
	}

	if (costSnap.haveTU())
	{
		_attackAction.type = BA_SNAPSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}
	if (costAimed.haveTU())
	{
		_attackAction.type = BA_AIMEDSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}
	if (costAuto.haveTU())
	{
		_attackAction.type = BA_AUTOSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
	}
}

void PlayerFactionAI::extendedFireModeChoice(BattleActionCost& costAuto, BattleActionCost& costSnap, BattleActionCost& costAimed, BattleActionCost& costThrow, bool checkLOF)
{
	std::vector<BattleActionType> attackOptions = { };
	if (costAimed.haveTU())
	{
		attackOptions.push_back(BA_AIMEDSHOT);
	}
	if (costAuto.haveTU())
	{
		attackOptions.push_back(BA_AUTOSHOT);
	}
	if (costSnap.haveTU())
	{
		attackOptions.push_back(BA_SNAPSHOT);
	}
	if (costThrow.haveTU())
	{
		attackOptions.push_back(BA_THROW);
	}

	BattleActionType chosenAction = BA_RETHINK;
	BattleAction testAction = _attackAction;
	int score = 0;
	for (auto& i : attackOptions)
	{
		testAction.type = i;
		if (i == BA_THROW)
		{
			if (_grenade)
			{
				testAction.weapon = _unit->getGrenadeFromBelt(_save);
			}
			else
			{
				continue;
			}
		}
		else
		{
			testAction.weapon = _attackAction.weapon;
		}
		int newScore = scoreFiringMode(&testAction, _aggroTarget, checkLOF);

		// Add a random factor to the firing mode score based on intelligence
		// An intelligence value of 10 will decrease this random factor to 0
		// Default values for and intelligence value of 0 will make this a 50% to 150% roll
		int intelligenceModifier = _unit->getFaction() == FACTION_PLAYER
			? 0
			: _save->getMod()->getAIFireChoiceIntelCoeff() * std::max(10 - _unit->getIntelligence(), 0);
		newScore = newScore * (100 + RNG::generate(-intelligenceModifier, intelligenceModifier)) / 100;

		// More aggressive units get a modifier to the score for autoshots
		// Aggression = 0 lowers the score, aggro = 1 is no modifier, aggro > 1 bumps up the score by 5% (configurable) for each increment over 1
		if (i == BA_AUTOSHOT)
		{
			newScore = newScore * (100 + (_unit->getAggression() - 1) * _save->getMod()->getAIFireChoiceAggroCoeff()) / 100;
		}

		if (newScore > score)
		{
			score = newScore;
			chosenAction = i;
		}

		if (_traceAI)
		{
			Log(LOG_INFO) << "Evaluate option " << (int)i << ", score = " << newScore;
		}
	}

	_attackAction.type = chosenAction;
	if (_attackAction.type != BA_RETHINK && projectileRiskyForAllies(&_attackAction, _aggroTarget))
	{
		_attackAction.type = BA_RETHINK;
	}
}

int PlayerFactionAI::scorePlayerGrenadeTarget(BattleItem *grenade, const Position &targetPos, int radius, bool proximity, std::string *rejectReason) const
{
	auto reject = [&](const std::string &reason) -> int
	{
		if (rejectReason && rejectReason->empty())
		{
			*rejectReason = reason;
		}
		return -100000;
	};
	if (!grenade || radius <= 0 || !_save->getTile(targetPos))
	{
		return reject(!grenade ? "missing_grenade" : (radius <= 0 ? "no_radius" : "missing_target_tile"));
	}
	Tile *targetTile = _save->getTile(targetPos);
	if (!proximity && targetPos.z > 0 && targetTile->hasNoFloor(_save))
	{
		return reject("grenade_would_fall_to_lower_level");
	}
	BattleAction action;
	action.actor = _unit;
	action.weapon = grenade;
	action.type = BA_THROW;
	action.target = targetPos;
	action.updateTU();
	const int power = std::max(0, grenade->getRules()->getPower());
	// Player AI primes every ordinary grenade to fuse 0 as part of the throw.  The
	// carried item is still unprimed here (fuse -1), so inspecting its current fuse
	// incorrectly assumes that allies will get another chance to move.
	const bool instantBlast = !proximity;
	const int allySafetyRadius = radius + (instantBlast ? 1 : 0);
	const int throwDistance = Position::distance2d(_unit->getPosition(), targetPos);
	if (instantBlast && !isPlayerSmokeGrenade(grenade)
		&& targetPos.z != _unit->getPosition().z
		&& (throwDistance <= radius + 3 || _visibleEnemies == 0))
	{
		// Throws across a ramp/level edge are highly seed-sensitive: a small
		// combined horizontal/vertical error can catch the lip and return the
		// explosive beside the thrower.  Keep the short/blind restriction while
		// still allowing validated long throws at visible targets.
		return reject(throwDistance <= radius + 3
			? "short_cross_level_throw_can_return_into_squad"
			: "blind_cross_level_throw_can_return_into_squad");
	}
	if (instantBlast && !isPlayerSmokeGrenade(grenade) && power >= 80
		&& targetPos.z == _unit->getPosition().z
		&& throwDistance <= radius + 3)
	{
		const Position from = _unit->getPosition();
		const Position delta = targetPos - from;
		const int span = std::max(abs(delta.x), abs(delta.y));
		bool nearThrowObject = false;
		for (int step = 1; step <= std::min(4, span - 1) && !nearThrowObject; ++step)
		{
			const Position corridorCenter(from.x + delta.x * step / span,
				from.y + delta.y * step / span, from.z);
			for (int dx = -1; dx <= 1 && !nearThrowObject; ++dx)
			{
				for (int dy = -1; dy <= 1; ++dy)
				{
					Tile *corridorTile = _save->getTile(corridorCenter + Position(dx, dy, 0));
					if (corridorTile && corridorTile->getMapData(O_OBJECT))
					{
						nearThrowObject = true;
						break;
					}
				}
			}
		}
		if (nearThrowObject)
		{
			return reject("near_thrower_object_can_catch_high_explosive");
		}
	}
	std::string projectileRiskReason;
	bool hardProjectileRisk = false;
	const bool projectileScatterRisk = !proximity && explosiveProjectileRiskyForAllies(&action, radius, 0, false, &projectileRiskReason, &hardProjectileRisk);
	if (projectileScatterRisk && hardProjectileRisk)
	{
		return reject(projectileRiskReason.empty() ? "ally_projectile_or_scatter_risk" : projectileRiskReason);
	}

	const int throwerPostThrowTU = proximity ? std::max(0, _unit->getTimeUnits() - action.Time - _unit->getActionTUs(BA_PRIME, grenade).Time - PLAYER_AI_PICKUP_TU_COST) : 0;
	int enemiesAffected = 0;
	int highValueEnemiesAffected = 0;
	int evacuatableAllies = 0;
	int riskyEvacuatableAllies = 0;
	BattleUnit *singleAffectedEnemy = 0;
	int score = proximity ? PLAYER_AI_GRENADE_PROXIMITY_BASE_SCORE : 0;
	auto canEvacuateBlast = [&](BattleUnit *unit) -> bool
	{
		if (!unit || unit->isOut() || unit->getFaction() != _unit->getFaction())
		{
			return false;
		}
		const int dist = Position::distance2d(unit->getPosition(), targetPos);
		if (dist > allySafetyRadius)
		{
			return true;
		}
		const int requiredSteps = allySafetyRadius - dist + 1;
		const int availableTU = (unit == _unit && proximity) ? throwerPostThrowTU : unit->getTimeUnits();
		return availableTU >= std::max(PLAYER_AI_GRENADE_MIN_EVACUATE_TU, requiredSteps * PLAYER_AI_GRENADE_EVACUATE_TU_PER_STEP);
	};
	auto riskyBlastEvacuation = [&](BattleUnit *unit) -> bool
	{
		if (!unit || unit->isOut() || unit->getFaction() != _unit->getFaction())
		{
			return false;
		}
		const int dist = Position::distance2d(unit->getPosition(), targetPos);
		if (dist > allySafetyRadius)
		{
			return false;
		}
		const int requiredSteps = allySafetyRadius - dist + 1;
		const int requiredTU = std::max(PLAYER_AI_GRENADE_MIN_EVACUATE_TU, requiredSteps * PLAYER_AI_GRENADE_EVACUATE_TU_PER_STEP);
		return dist <= std::max(1, allySafetyRadius / 2) || unit->getTimeUnits() < requiredTU + PLAYER_AI_GRENADE_RISKY_EVACUATE_EXTRA_TU || getSpottingUnits(unit->getPosition()) > 0;
	};
	for (auto *bu : *_save->getUnits())
	{
		if (!bu || bu->getStatus() == STATUS_DEAD || bu->getStatus() == STATUS_IGNORE_ME)
		{
			continue;
		}
		if (abs(bu->getPosition().z - targetPos.z) > Options::battleExplosionHeight)
		{
			continue;
		}
		const int dist = Position::distance2d(bu->getPosition(), targetPos);
		const bool protectedAlly = bu->getFaction() == _unit->getFaction()
			|| (bu->getStatus() == STATUS_UNCONSCIOUS && bu->getOriginalFaction() == FACTION_PLAYER);
		if (protectedAlly)
		{
			if (dist > allySafetyRadius)
			{
				continue;
			}
			if (proximity && bu != _unit)
			{
				// Only the thrower is guaranteed another AI action after placing the
				// mine.  A squadmate's remaining TU may belong to an action that has
				// already finished earlier in the faction turn, so it is not a valid
				// promise that they will evacuate before the enemy triggers the mine.
				return reject("ally_inside_proximity_blast");
			}
			if (!proximity && bu == _unit)
			{
				return reject(instantBlast ? "thrower_in_instant_blast_margin" : "thrower_in_blast");
			}
			if (instantBlast)
			{
				return reject("ally_in_instant_blast_margin");
			}
			if (!canEvacuateBlast(bu))
			{
				return reject("ally_cannot_evacuate_blast");
			}
			++evacuatableAllies;
			if (!proximity && riskyBlastEvacuation(bu))
			{
				++riskyEvacuatableAllies;
			}
			score -= PLAYER_AI_GRENADE_ALLY_BASE_PENALTY + std::max(0, radius - dist) * PLAYER_AI_GRENADE_ALLY_RADIUS_PENALTY;
			continue;
		}
		if (!bu->isOut() && validTarget(bu, true, true))
		{
			if (dist > radius)
			{
				continue;
			}
			const int predictedBlastDamage = playerAIConservativeBlastDamage(_save, grenade, targetPos, bu);
			if (predictedBlastDamage <= 0)
			{
				continue;
			}
			++enemiesAffected;
			singleAffectedEnemy = bu;
			const int armor = std::max(std::max(bu->getArmor(SIDE_FRONT), bu->getArmor(SIDE_LEFT)), bu->getArmor(SIDE_RIGHT));
			BattleItem *enemyWeapon = bu->getMainHandWeapon(false);
			const int enemyWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
			int unitScore = PLAYER_AI_GRENADE_UNIT_BASE_SCORE;
			unitScore += predictedBlastDamage;
			unitScore += std::max(0, PLAYER_AI_GRENADE_WOUNDED_HEALTH_BONUS_BASE - bu->getHealth());
			if (bu->getHealth() > PLAYER_AI_DURABLE_TARGET_HEALTH || armor >= power / 2)
			{
				unitScore += PLAYER_AI_GRENADE_DURABLE_BONUS;
			}
			if (bu->getHealth() > PLAYER_AI_VERY_DURABLE_TARGET_HEALTH || armor >= power)
			{
				unitScore += PLAYER_AI_GRENADE_VERY_DURABLE_BONUS;
			}
			if (enemyWeaponDanger > PLAYER_AI_PERCENT)
			{
				unitScore += std::min(PLAYER_AI_GRENADE_WEAPON_DANGER_BONUS_CAP, enemyWeaponDanger - PLAYER_AI_GRENADE_WEAPON_DANGER_BONUS_BASE);
			}
			if (bu->getHealth() > PLAYER_AI_DURABLE_TARGET_HEALTH || armor >= power / 2 || enemyWeaponDanger > PLAYER_AI_GRENADE_HIGH_VALUE_WEAPON_DANGER)
			{
				++highValueEnemiesAffected;
			}
			unitScore -= dist * PLAYER_AI_GRENADE_TARGET_DISTANCE_PENALTY;
			score += unitScore;
		}
	}

	if (!proximity)
	{
		const int oldEfficacy = explosiveEfficacy(targetPos, _unit, radius, _attackAction.diff, true);
		score += oldEfficacy * PLAYER_AI_GRENADE_OLD_EFFICACY_SCORE;
		if (enemiesAffected == 0)
		{
			return reject("no_enemies_in_blast");
		}
		if (projectileScatterRisk)
		{
			score -= PLAYER_AI_GRENADE_SCATTER_PENALTY;
			if (enemiesAffected < 2 && highValueEnemiesAffected == 0)
			{
				return reject(projectileRiskReason.empty() ? "scatter_risk_low_value_target" : projectileRiskReason);
			}
			if (score < PLAYER_AI_GRENADE_SCATTER_MIN_SCORE && enemiesAffected < 3 && highValueEnemiesAffected < 2)
			{
				return reject(projectileRiskReason.empty() ? "scatter_risk_margin_too_low" : projectileRiskReason);
			}
		}
		if (riskyEvacuatableAllies > 0)
		{
			score -= riskyEvacuatableAllies * PLAYER_AI_GRENADE_RISKY_EVACUATION_PENALTY;
			if (enemiesAffected < 2 && score < PLAYER_AI_GRENADE_RISKY_ALLY_MIN_SCORE)
			{
				return reject("ally_blast_margin_too_low");
			}
		}
		if (enemiesAffected == 1)
		{
			BattleUnit *centerTarget = _save->getTile(targetPos)->getUnit();
			if (!centerTarget || centerTarget != singleAffectedEnemy)
			{
				score -= PLAYER_AI_GRENADE_SINGLE_EMPTY_TILE_PENALTY;
			}
			if (singleAffectedEnemy)
			{
				const int armor = std::max(std::max(singleAffectedEnemy->getArmor(SIDE_FRONT), singleAffectedEnemy->getArmor(SIDE_LEFT)), singleAffectedEnemy->getArmor(SIDE_RIGHT));
				const bool durableSingleTarget = singleAffectedEnemy->getHealth() >= PLAYER_AI_GRENADE_SINGLE_DURABLE_HEALTH || armor >= power / 2;
				if (durableSingleTarget)
				{
					score += PLAYER_AI_GRENADE_SINGLE_DURABLE_BONUS;
				}
				else
				{
					score -= PLAYER_AI_GRENADE_SINGLE_FRAGILE_PENALTY;
				}
				if (singleAffectedEnemy->getHealth() >= PLAYER_AI_VERY_DURABLE_TARGET_HEALTH || armor >= power)
				{
					score += PLAYER_AI_GRENADE_SINGLE_DURABLE_BONUS;
				}
			}
		}
	}
	if (evacuatableAllies > 0)
	{
		score -= evacuatableAllies * PLAYER_AI_GRENADE_EVACUATABLE_ALLY_PENALTY;
	}
	return score;
}

bool PlayerFactionAI::setupProximityMineAmbush()
{
	if (_unit->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	BattleItem *carriedMine = 0;
	for (auto *item : *_unit->getInventory())
	{
		if (item && item->getRules() && item->getRules()->getBattleType() == BT_PROXIMITYGRENADE)
		{
			carriedMine = item;
			break;
		}
	}
	if (!carriedMine)
	{
		return false;
	}

	const bool logEarlyProximitySkips = Options::autoBattleLog
		&& _save->getTurn() <= PLAYER_AI_PROXIMITY_LOG_TURN_LIMIT
		&& _knownEnemies >= PLAYER_AI_PROXIMITY_LOG_KNOWN_ENEMIES
		&& !_visibleEnemies
		&& !_spottingEnemies;
	auto logSkip = [&](const std::string &reason, const Position &contactPos = Position(-1, -1, -1), const BattleRoomInfo *room = 0, int enemiesInRoom = 0)
	{
		if (!logEarlyProximitySkips)
		{
			return;
		}
		std::ostringstream log;
		log << "Player faction proximity mine skip: unit=" << _unit->getId()
			<< ", reason=" << reason
			<< ", position=" << _unit->getPosition()
			<< ", tu=" << _unit->getTimeUnits()
			<< ", visible=" << _visibleEnemies
			<< ", spotting=" << _spottingEnemies
			<< ", contact=" << contactPos
			<< ", roomEnemies=" << enemiesInRoom;
		if (room)
		{
			log << ", roomSize=" << room->tileCount
				<< ", roomEntries=" << room->entryPositions.size()
				<< ", roomOutside=" << room->isOutside
				<< ", roomHall=" << room->isHall;
		}
		_save->appendToAutoBattleLog(log.str());
	};

	auto isPlacedProximityMine = [&](const BattleItem *item) -> bool
	{
		return item
			&& item->getRules()
			&& item->getRules()->getBattleType() == BT_PROXIMITYGRENADE
			&& !item->getOwner()
			&& item->getSlot() == _save->getMod()->getInventoryGround()
			&& item->getFuseTimer() >= 0;
	};

	auto allyUnsafeForMine = [&](BattleUnit *ally, const Position &pos, int radius, int margin = 0, int throwerAvailableTU = -1) -> bool
	{
		if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction() || ally->getPosition().z != pos.z)
		{
			return false;
		}
		const int dist = Position::distance2d(ally->getPosition(), pos);
		if (ally == _unit)
		{
			if (dist > radius)
			{
				return false;
			}
			const int requiredSteps = std::max(1, radius - dist + 1);
			const int availableTU = throwerAvailableTU >= 0 ? throwerAvailableTU : ally->getTimeUnits();
			return availableTU < std::max(PLAYER_AI_GRENADE_MIN_EVACUATE_TU, requiredSteps * PLAYER_AI_GRENADE_EVACUATE_TU_PER_STEP);
		}
		// Other soldiers may already have completed their actions this faction
		// turn even though their TU value is still non-zero.  Treat every one of
		// them inside the blast margin as fixed; only the current thrower can be
		// relied upon to take the immediate evacuation action.
		return dist <= radius + margin;
	};

	Position stagedTarget;
	Position stagedContact;
	if (!_spottingEnemies && getPlayerProximityMineStaging(_save, _unit->getId(), &stagedTarget, &stagedContact))
	{
		BattleAction stagedAction;
		stagedAction.actor = _unit;
		stagedAction.weapon = carriedMine;
		stagedAction.type = BA_THROW;
		stagedAction.target = stagedTarget;
		stagedAction.updateTU();
		stagedAction.Time += PLAYER_AI_PICKUP_TU_COST;
		stagedAction += _unit->getActionTUs(BA_PRIME, carriedMine);
		const int stagedRadius = carriedMine->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(stagedAction));
		const int throwAccuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(stagedAction), _save->getMod());
		const int throwLimit = std::max(PLAYER_AI_PROXIMITY_THROW_LIMIT_MIN, throwAccuracy / PLAYER_AI_PROXIMITY_THROW_ACCURACY_DIVISOR);
		const int effectiveThrowLimit = throwLimit + PLAYER_AI_PROXIMITY_STAGED_THROW_BONUS;
		const int throwDist = Position::distance2d(stagedTarget, _unit->getPosition());
		const int throwerPostThrowTU = std::max(0, _unit->getTimeUnits() - stagedAction.Time);
		std::string blockedReason;
		bool blocked = !stagedAction.haveTU() || stagedRadius <= 0 || throwDist > effectiveThrowLimit;
		if (blocked)
		{
			blockedReason = !stagedAction.haveTU() ? "no_tu" : (stagedRadius <= 0 ? "no_radius" : "throw_too_far");
		}
		Tile *targetTile = _save->getTile(stagedTarget);
		if (!targetTile || targetTile->getMapData(O_OBJECT)
			|| (targetTile->getUnit() && targetTile->getUnit()->getFaction() == _unit->getFaction()))
		{
			blocked = true;
			blockedReason = !targetTile ? "missing_target_tile"
				: (targetTile->getMapData(O_OBJECT) ? "object_on_mine_target" : "friendly_on_target");
		}
		for (auto *ally : *_save->getUnits())
		{
			if (allyUnsafeForMine(ally, stagedTarget, stagedRadius, PLAYER_AI_PROXIMITY_STAGED_THROW_BONUS, throwerPostThrowTU))
			{
				blocked = true;
				blockedReason = ally == _unit ? "thrower_in_blast_zone" : "ally_cannot_evacuate";
				break;
			}
		}
		for (int dx = -(stagedRadius + PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN); dx <= stagedRadius + PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN && !blocked; ++dx)
		{
			for (int dy = -(stagedRadius + PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN); dy <= stagedRadius + PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN && !blocked; ++dy)
			{
				Position check(stagedTarget.x + dx, stagedTarget.y + dy, stagedTarget.z);
				if (Position::distance2d(check, stagedTarget) > stagedRadius + PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN)
				{
					continue;
				}
				Tile *tile = _save->getTile(check);
				if (!tile)
				{
					continue;
				}
				for (auto *item : *tile->getInventory())
				{
					if (isPlacedProximityMine(item))
					{
						blocked = true;
						blockedReason = "nearby_existing_mine";
						break;
					}
				}
			}
		}
		if (!blocked)
		{
			Position originVoxel = _save->getTileEngine()->getOriginVoxel(stagedAction, 0);
			Position targetVoxel = stagedTarget.toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY, (PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE + -targetTile->getTerrainLevel()));
			if (!_save->getTileEngine()->validateThrow(stagedAction, originVoxel, targetVoxel, _save->getDepth()))
			{
				blocked = true;
				blockedReason = "invalid_throw";
			}
		}
		if (!blocked)
		{
			_attackAction.actor = _unit;
			_attackAction.weapon = carriedMine;
			_attackAction.target = stagedTarget;
			_attackAction.type = BA_THROW;
			recordPendingPlayerGrenadeDanger(_save, _unit->getFaction(), stagedTarget,
				stagedRadius + PLAYER_AI_THROW_DANGER_MARGIN, 0, false, _unit, carriedMine);
			recordPlayerProximityMinePlan(_save, _unit->getFaction(), stagedContact, stagedTarget);
			clearPlayerProximityMineStaging(_save, _unit->getId());
			_rifle = false;
			_melee = false;
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction proximity mine staged placement: unit=" << _unit->getId()
					<< ", item=" << carriedMine->getRules()->getType()
					<< ", target=" << stagedTarget
					<< ", contact=" << stagedContact
					<< ", radius=" << stagedRadius
					<< ", throwDist=" << throwDist
					<< ", throwAccuracy=" << throwAccuracy;
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction proximity mine staged placement blocked: unit=" << _unit->getId()
				<< ", reason=" << blockedReason
				<< ", target=" << stagedTarget
				<< ", contact=" << stagedContact
				<< ", radius=" << stagedRadius
				<< ", throwDist=" << throwDist
				<< ", throwLimit=" << throwLimit
				<< ", throwAccuracy=" << throwAccuracy
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", tu=" << _unit->getTimeUnits();
			_save->appendToAutoBattleLog(log.str());
		}
		if (blockedReason == "no_tu")
		{
			return false;
		}
		clearPlayerProximityMineStaging(_save, _unit->getId());
		clearPlayerProximityMinePlan(_save, _unit->getFaction(), stagedContact, stagedTarget);
	}

	if (!_factionAI)
	{
		logSkip("no_faction_ai");
		return false;
	}
	Position contactPos;
	const BattleRoomInfo *room = 0;
	int enemiesInRoom = 0;
	bool visibleContact = false;
	if (!_factionAI->getBestEnemyContactPosition(&contactPos, &room, &enemiesInRoom, &visibleContact) || !room)
	{
		logSkip("no_room_contact", contactPos, room, enemiesInRoom);
		return false;
	}
	int activeAllies = 0;
	int activeHostiles = 0;
	for (auto *other : *_save->getUnits())
	{
		if (!other || other->isOut())
		{
			continue;
		}
		if (other->getFaction() == _unit->getFaction())
		{
			++activeAllies;
		}
		else if (other->getFaction() == FACTION_HOSTILE)
		{
			++activeHostiles;
		}
	}
	const bool earlyPressureFieldMine = _save->getTurn() <= PLAYER_AI_PROXIMITY_LOG_TURN_LIMIT
		&& _knownEnemies >= PLAYER_AI_PROXIMITY_LOG_KNOWN_ENEMIES
		&& !_visibleEnemies
		&& !_spottingEnemies
		&& activeHostiles >= std::max(PLAYER_AI_PROXIMITY_EARLY_HOSTILE_MIN, activeAllies / 2);
	bool projectedFieldContact = false;
	if (contactPos.z != _unit->getPosition().z)
	{
		if (!earlyPressureFieldMine)
		{
			logSkip("different_z", contactPos, room, enemiesInRoom);
			return false;
		}
		Position projectedContact(contactPos.x, contactPos.y, _unit->getPosition().z);
		if (!_save->getTile(projectedContact))
		{
			logSkip("different_z_no_projection", contactPos, room, enemiesInRoom);
			return false;
		}
		contactPos = projectedContact;
		projectedFieldContact = true;
	}
	Position allyAnchor;
	int allyAnchorCount = 0;
	int bestAllyDistance = 100000;
	for (auto *ally : *_save->getUnits())
	{
		if (!ally || ally->isOut() || ally->getFaction() != _unit->getFaction() || ally->getPosition().z != contactPos.z)
		{
			continue;
		}
		const int dist = Position::distance2d(ally->getPosition(), contactPos);
		if (dist < bestAllyDistance)
		{
			bestAllyDistance = dist;
			allyAnchor = ally->getPosition();
		}
		++allyAnchorCount;
	}
	if (allyAnchorCount == 0)
	{
		logSkip("no_ally_anchor", contactPos, room, enemiesInRoom);
		return false;
	}
	const int entries = (int)room->entryPositions.size();
	const bool mineableRoom = !room->isOutside
		&& ((!room->isHall && room->tileCount <= PLAYER_AI_PROXIMITY_ROOM_TILE_LIMIT && entries > 0 && entries <= PLAYER_AI_PROXIMITY_ROOM_ENTRY_LIMIT)
			|| (room->isHall && room->tileCount <= PLAYER_AI_PROXIMITY_HALL_TILE_LIMIT && entries > 0 && entries <= PLAYER_AI_PROXIMITY_HALL_ENTRY_LIMIT));
	if (visibleContact || _visibleEnemies > 0)
	{
		logSkip("visible_contact", contactPos, room, enemiesInRoom);
		return false;
	}
	int contactThreat = 0;
	int contactArmor = 0;
	int contactHealth = 0;
	if (Tile *contactTile = _save->getTile(contactPos))
	{
		if (BattleUnit *contactUnit = contactTile->getUnit())
		{
			if (validTarget(contactUnit, true, true))
			{
				const int minePower = std::max(0, carriedMine->getRules()->getPower());
				contactHealth = contactUnit->getHealth();
				contactArmor = std::max(std::max(contactUnit->getArmor(SIDE_FRONT), contactUnit->getArmor(SIDE_LEFT)), contactUnit->getArmor(SIDE_RIGHT));
				BattleItem *enemyWeapon = contactUnit->getMainHandWeapon(false);
				const int enemyWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
				contactThreat = contactHealth + contactArmor + enemyWeaponDanger;
				(void)minePower;
			}
		}
	}
	const bool fieldMine = earlyPressureFieldMine
		&& (projectedFieldContact
			|| !mineableRoom
			|| room->isOutside
			|| room->isHall
			|| entries > PLAYER_AI_PROXIMITY_FIELD_ENTRY_LIMIT
			|| room->tileCount > PLAYER_AI_PROXIMITY_FIELD_ROOM_TILE_LIMIT
			|| bestAllyDistance >= PLAYER_AI_PROXIMITY_FIELD_ALLY_DISTANCE);
	if (fieldMine)
	{
		// An open-field mine reserves a large blast circle but does not reserve a
		// route for every squad.  Until the planner owns those corridors, such a
		// mine either kills allies or repeatedly clips their advance.  Keep mines
		// for actual room entries/chokepoints, where the blocked route is intended.
		logSkip("field_mine_without_reserved_squad_corridor", contactPos, room, enemiesInRoom);
		return false;
	}
	if (enemiesInRoom < PLAYER_AI_PROXIMITY_WORTH_ENEMIES)
	{
		logSkip("single_contact_not_worth_squad_corridor", contactPos, room, enemiesInRoom);
		return false;
	}
	if (!mineableRoom && !fieldMine)
	{
		logSkip("room_not_mineable", contactPos, room, enemiesInRoom);
		return false;
	}
	bool mineWorthyContact = enemiesInRoom >= PLAYER_AI_PROXIMITY_WORTH_ENEMIES
		|| fieldMine
		|| entries <= PLAYER_AI_PROXIMITY_WORTH_ENTRY_LIMIT
		|| contactHealth >= PLAYER_AI_PROXIMITY_WORTH_HEALTH
		|| contactArmor >= std::max(0, carriedMine->getRules()->getPower() * PLAYER_AI_PROXIMITY_ARMOR_POWER_NUM / PLAYER_AI_PROXIMITY_ARMOR_POWER_DEN)
		|| contactThreat >= (fieldMine ? PLAYER_AI_PROXIMITY_FIELD_THREAT : PLAYER_AI_PROXIMITY_ROOM_THREAT);
	if (!mineWorthyContact)
	{
		logSkip("low_value_contact", contactPos, room, enemiesInRoom);
		return false;
	}

	int enemyMoveRadius = PLAYER_AI_PROXIMITY_BASE_MOVE_RADIUS + std::min(PLAYER_AI_PROXIMITY_MOVE_RADIUS_ENEMY_CAP, enemiesInRoom * PLAYER_AI_PROXIMITY_MOVE_RADIUS_PER_ENEMY);
	if (Tile *contactTile = _save->getTile(contactPos))
	{
		if (BattleUnit *contactUnit = contactTile->getUnit())
		{
			if (contactUnit->getFaction() == FACTION_HOSTILE && contactUnit->getBaseStats())
			{
				enemyMoveRadius = std::max(PLAYER_AI_PROXIMITY_MOVE_RADIUS_MIN, std::min(PLAYER_AI_PROXIMITY_MOVE_RADIUS_MAX, contactUnit->getBaseStats()->tu / PLAYER_AI_PROXIMITY_MOVE_RADIUS_TU_DIVISOR));
			}
		}
	}

	auto tileHasProximityMine = [&](const Position &pos) -> bool
	{
		Tile *tile = _save->getTile(pos);
		if (!tile)
		{
			return true;
		}
		for (auto *item : *tile->getInventory())
		{
			if (isPlacedProximityMine(item))
			{
				return true;
			}
		}
		return false;
	};

	auto tileHasNearbyProximityMine = [&](const Position &pos, int radius) -> bool
	{
		for (int dx = -radius; dx <= radius; ++dx)
		{
			for (int dy = -radius; dy <= radius; ++dy)
			{
				Position check(pos.x + dx, pos.y + dy, pos.z);
				if (Position::distance2d(check, pos) > radius)
				{
					continue;
				}
				Tile *tile = _save->getTile(check);
				if (!tile)
				{
					continue;
				}
				for (auto *item : *tile->getInventory())
				{
					if (isPlacedProximityMine(item))
					{
						return true;
					}
				}
			}
		}
		return false;
	};

	auto allyTooCloseToMine = [&](const Position &pos, int radius, int throwerAvailableTU) -> bool
	{
		for (auto *ally : *_save->getUnits())
		{
			if (allyUnsafeForMine(ally, pos, radius, 1, throwerAvailableTU))
			{
				return true;
			}
		}
		return false;
	};

	std::vector<Position> candidates;
	auto addCandidate = [&](const Position &pos)
	{
		Tile *candidateTile = _save->getTile(pos);
		if (pos.z != _unit->getPosition().z || !candidateTile || candidateTile->getMapData(O_OBJECT)
			|| isPlayerExplosiveDanger(_save, _unit->getFaction(), pos) || tileHasProximityMine(pos))
		{
			return;
		}
		for (const auto &existing : candidates)
		{
			if (existing == pos)
			{
				return;
			}
		}
		candidates.push_back(pos);
	};

	if (fieldMine)
	{
		auto addLineMineCandidates = [&](const Position &from, int minStep, int maxStep)
		{
			int pathX = (contactPos.x > from.x) - (contactPos.x < from.x);
			int pathY = (contactPos.y > from.y) - (contactPos.y < from.y);
			if (pathX == 0 && pathY == 0)
			{
				return;
			}
			const Position path(pathX, pathY, 0);
			const Position sideA(-pathY, pathX, 0);
			const Position sideB(pathY, -pathX, 0);
			const int contactDistance = Position::distance2d(from, contactPos);
			const int lastStep = std::min(maxStep, std::max(minStep, contactDistance - 2));
			for (int step = minStep; step <= lastStep; ++step)
			{
				Position pathTile = from + Position(path.x * step, path.y * step, 0);
				if (Position::distance2d(pathTile, contactPos) <= enemyMoveRadius + 6)
				{
					addCandidate(pathTile);
					if (step >= minStep + 1)
					{
						addCandidate(pathTile + sideA);
						addCandidate(pathTile + sideB);
					}
				}
			}
		};
		addLineMineCandidates(allyAnchor, 5, 12);
		addLineMineCandidates(_unit->getPosition(), 4, 8);
		const Position unitPos = _unit->getPosition();
		const int unitContactDistance = Position::distance2d(unitPos, contactPos);
		const bool compactField = room->tileCount <= 160 && entries <= 12;
		if (projectedFieldContact || compactField || bestAllyDistance >= 10)
		{
			for (int dx = -7; dx <= 7; ++dx)
			{
				for (int dy = -7; dy <= 7; ++dy)
				{
					Position ringTile(unitPos.x + dx, unitPos.y + dy, unitPos.z);
					const int throwDistance = Position::distance2d(ringTile, unitPos);
					if (throwDistance < 5 || throwDistance > 7)
					{
						continue;
					}
					const int contactDistance = Position::distance2d(ringTile, contactPos);
					if (contactDistance >= unitContactDistance || contactDistance > enemyMoveRadius + 8)
					{
						continue;
					}
					if (Position::distance2d(ringTile, allyAnchor) < std::max(5, carriedMine->getRules()->getExplosionRadius({ BA_THROW, _unit, carriedMine }) + 1))
					{
						continue;
					}
					addCandidate(ringTile);
				}
			}
		}
		for (int dx = -3; dx <= 3; ++dx)
		{
			for (int dy = -3; dy <= 3; ++dy)
			{
				if (Position::distance2d(Position(0, 0, 0), Position(dx, dy, 0)) > 3)
				{
					continue;
				}
				Position nearContact(contactPos.x + dx, contactPos.y + dy, contactPos.z);
				if (Position::distance2d(nearContact, allyAnchor) >= std::max(5, carriedMine->getRules()->getExplosionRadius({ BA_THROW, _unit, carriedMine }) + 1))
				{
					addCandidate(nearContact);
				}
			}
		}
	}
	else for (const auto &entry : room->entryPositions)
	{
		if (entry.z != _unit->getPosition().z || Position::distance2d(entry, contactPos) > enemyMoveRadius + 2)
		{
			continue;
		}
		if (Position::distance2d(entry, allyAnchor) > Position::distance2d(contactPos, allyAnchor) + 2)
		{
			continue;
		}
		int pathX = (allyAnchor.x > entry.x) - (allyAnchor.x < entry.x);
		int pathY = (allyAnchor.y > entry.y) - (allyAnchor.y < entry.y);
		if (pathX == 0 && pathY == 0)
		{
			pathX = (entry.x > contactPos.x) - (entry.x < contactPos.x);
			pathY = (entry.y > contactPos.y) - (entry.y < contactPos.y);
		}
		if (pathX != 0 || pathY != 0)
		{
			const Position path(pathX, pathY, 0);
			const Position sideA(-pathY, pathX, 0);
			const Position sideB(pathY, -pathX, 0);
			for (int step = 0; step <= 3; ++step)
			{
				Position pathTile = entry + Position(path.x * step, path.y * step, 0);
				if (Position::distance2d(pathTile, contactPos) <= enemyMoveRadius + 4
					&& Position::distance2d(pathTile, allyAnchor) <= Position::distance2d(entry, allyAnchor) + 1)
				{
					addCandidate(pathTile);
					if (step > 0)
					{
						addCandidate(pathTile + sideA);
						addCandidate(pathTile + sideB);
					}
				}
			}
		}
	}
	if (candidates.empty())
	{
		logSkip("no_candidate_tiles", contactPos, room, enemiesInRoom);
		return false;
	}

	BattleAction bestAction;
	bestAction.type = BA_RETHINK;
	bestAction.actor = _unit;
	int bestScore = -100000;
	int bestRadius = 0;
	int proximityCandidateCount = (int)candidates.size();
	int proximityBlockedOccupied = 0;
	int proximityBlockedNearbyMine = 0;
	int proximityBlockedAlly = 0;
	int proximityBlockedNoTU = 0;
	int proximityBlockedTooFar = 0;
	int proximityBlockedInvalidThrow = 0;
	int proximityBlockedScore = 0;
	Position bestStageMineTarget;
	BattleItem *bestStageMine = 0;
	int bestStageScore = -100000;
	int bestStageRadius = 0;
	int bestStageThrowLimit = 0;
	int bestStageThrowTU = 0;

	for (auto *mine : *_unit->getInventory())
	{
		if (!mine || !mine->getRules() || mine->getRules()->getBattleType() != BT_PROXIMITYGRENADE)
		{
			continue;
		}
		BattleAction action;
		action.actor = _unit;
		action.weapon = mine;
		action.type = BA_THROW;
		action.updateTU();
		action.Time += PLAYER_AI_PICKUP_TU_COST;
		action += _unit->getActionTUs(BA_PRIME, mine);
		if (!action.haveTU())
		{
			++proximityBlockedNoTU;
			continue;
		}
		const int radius = mine->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
		if (radius <= 0)
		{
			continue;
		}
		const int throwAccuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(action), _save->getMod());
		const int throwLimit = std::max(PLAYER_AI_PROXIMITY_THROW_LIMIT_MIN, throwAccuracy / PLAYER_AI_PROXIMITY_THROW_ACCURACY_DIVISOR);
		const int effectiveThrowLimit = throwLimit + (fieldMine ? PLAYER_AI_PROXIMITY_STAGED_THROW_BONUS : 0);
		Position originVoxel = _save->getTileEngine()->getOriginVoxel(action, 0);
		for (const auto &target : candidates)
		{
			Tile *tile = _save->getTile(target);
			if (!tile || (tile->getUnit() && tile->getUnit()->getFaction() == _unit->getFaction()))
			{
				++proximityBlockedOccupied;
				continue;
			}
			const int throwDist = Position::distance2d(target, _unit->getPosition());
			const int throwerPostThrowTU = std::max(0, _unit->getTimeUnits() - action.Time);
			if (tileHasNearbyProximityMine(target, radius + PLAYER_AI_PROXIMITY_NEARBY_MINE_MARGIN))
			{
				++proximityBlockedNearbyMine;
				continue;
			}
			if (allyTooCloseToMine(target, radius, throwerPostThrowTU))
			{
				++proximityBlockedAlly;
				continue;
			}
			const int contactDist = Position::distance2d(target, contactPos);
			const int pathProgress = Position::distance2d(contactPos, allyAnchor) - Position::distance2d(target, allyAnchor);
			int tacticalScore = PLAYER_AI_PROXIMITY_TACTICAL_BASE_SCORE
				+ radius * PLAYER_AI_PROXIMITY_RADIUS_SCORE
				+ std::min(PLAYER_AI_PROXIMITY_ROOM_ENEMY_SCORE_CAP, enemiesInRoom * PLAYER_AI_PROXIMITY_ROOM_ENEMY_SCORE)
				+ std::min(PLAYER_AI_PROXIMITY_THREAT_SCORE_CAP, contactThreat / PLAYER_AI_PROXIMITY_THREAT_SCORE_DIVISOR);
			if (fieldMine)
			{
				const Position unitPos = _unit->getPosition();
				const int vx = contactPos.x - unitPos.x;
				const int vy = contactPos.y - unitPos.y;
				const int wx = target.x - unitPos.x;
				const int wy = target.y - unitPos.y;
				const int dot = vx * wx + vy * wy;
				const int cross = std::abs(vx * wy - vy * wx);
				const int unitContactDist = std::max(1, Position::distance2d(unitPos, contactPos));
				if (dot <= 0)
				{
					tacticalScore -= PLAYER_AI_PROXIMITY_BEHIND_TARGET_PENALTY;
				}
				else
				{
					tacticalScore += std::min(PLAYER_AI_PROXIMITY_DOT_SCORE_CAP, dot / PLAYER_AI_PROXIMITY_DOT_SCORE_DIVISOR);
					tacticalScore -= std::min(PLAYER_AI_PROXIMITY_CROSS_PENALTY_CAP, cross * PLAYER_AI_PROXIMITY_CROSS_PENALTY / unitContactDist);
				}
			}
			tacticalScore += std::max(0, pathProgress) * PLAYER_AI_PROXIMITY_PATH_PROGRESS_SCORE;
			tacticalScore += std::min(PLAYER_AI_PROXIMITY_THROW_ACCURACY_SCORE_CAP, throwAccuracy - PLAYER_AI_PROXIMITY_THROW_ACCURACY_BASE);
			tacticalScore += entries <= PLAYER_AI_PROXIMITY_NARROW_ENTRY_LIMIT ? PLAYER_AI_PROXIMITY_NARROW_ENTRY_BONUS : 0;
			tacticalScore += (fieldMine || _factionAI->getRoomIdAt(target) != room->id) ? PLAYER_AI_PROXIMITY_ROOM_EXIT_BONUS : 0;
			tacticalScore -= std::max(0, contactDist - enemyMoveRadius) * PLAYER_AI_PROXIMITY_CONTACT_OVERSHOOT_PENALTY;
			tacticalScore -= contactDist <= PLAYER_AI_PROXIMITY_CONTACT_CLOSE_LIMIT ? PLAYER_AI_PROXIMITY_CONTACT_CLOSE_PENALTY : 0;
			tacticalScore -= fieldMine ? std::max(0, PLAYER_AI_PROXIMITY_FIELD_ALLY_SAFE_DISTANCE - Position::distance2d(target, allyAnchor)) * PLAYER_AI_PROXIMITY_FIELD_ALLY_PENALTY : 0;
			if (!room->isHall)
			{
				tacticalScore += PLAYER_AI_PROXIMITY_NON_HALL_BONUS;
			}
			const bool lowAccuracyTooFar = throwAccuracy < PLAYER_AI_PROXIMITY_LOW_THROW_ACCURACY
				&& throwDist > (fieldMine ? std::max(5, effectiveThrowLimit - 1) : 4);
			const bool relaxedFieldMineThrow = fieldMine && _knownEnemies >= PLAYER_AI_PROXIMITY_FIELD_RELAXED_KNOWN_ENEMIES;
			if (throwDist > effectiveThrowLimit || (lowAccuracyTooFar && !relaxedFieldMineThrow))
			{
				++proximityBlockedTooFar;
				if (tacticalScore > bestStageScore)
				{
					bestStageScore = tacticalScore;
					bestStageMineTarget = target;
					bestStageMine = mine;
					bestStageRadius = radius;
					bestStageThrowLimit = effectiveThrowLimit;
					bestStageThrowTU = action.Time;
				}
				continue;
			}
			action.target = target;
			Position targetVoxel = target.toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY, (PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE + -tile->getTerrainLevel()));
			if (!_save->getTileEngine()->validateThrow(action, originVoxel, targetVoxel, _save->getDepth()))
			{
				++proximityBlockedInvalidThrow;
				if (tacticalScore > bestStageScore)
				{
					bestStageScore = tacticalScore;
					bestStageMineTarget = target;
					bestStageMine = mine;
					bestStageRadius = radius;
					bestStageThrowLimit = effectiveThrowLimit;
					bestStageThrowTU = action.Time;
				}
				continue;
			}
			int score = scorePlayerGrenadeTarget(mine, target, radius, true);
			if (score < PLAYER_AI_GRENADE_REJECT_CUTOFF)
			{
				++proximityBlockedScore;
				continue;
			}
			score += tacticalScore;
			score -= throwDist * 2;
			if (score > bestScore)
			{
				bestScore = score;
				bestAction = action;
				bestAction.target = target;
				bestRadius = radius;
			}
		}
	}

	if (bestAction.type == BA_RETHINK || bestScore < PLAYER_AI_PROXIMITY_MIN_SCORE)
	{
		if (bestStageMine && bestStageScore >= PLAYER_AI_PROXIMITY_STAGE_MIN_SCORE && !_reachable.empty())
		{
			auto coverScoreAt = [&](const Position &pos) -> int
			{
				Tile *tile = _save->getTile(pos);
				if (!tile)
				{
					return 0;
				}
				int score = 0;
				if (tile->getMapData(O_OBJECT))
				{
					score += 8;
				}
				if (tile->getMapData(O_NORTHWALL))
				{
					score += contactPos.y < pos.y ? 14 : 5;
				}
				if (tile->getMapData(O_WESTWALL))
				{
					score += contactPos.x < pos.x ? 14 : 5;
				}
				return score;
			};
			Position bestStagePos;
			int bestStageMoveScore = -100000;
			int bestStageMoveTU = 0;
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				Position pos = tile->getPosition();
				if (pos == _unit->getPosition() || pos.z != _unit->getPosition().z || isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
				{
					continue;
				}
				const int futureThrowDist = Position::distance2d(pos, bestStageMineTarget);
				if (futureThrowDist > bestStageThrowLimit)
				{
					continue;
				}
				if (futureThrowDist <= bestStageRadius)
				{
					continue;
				}
				if (Position::distance2d(pos, contactPos) <= bestStageRadius + 2)
				{
					continue;
				}
				if (!playerCanThrowFromTile(_save, _unit, bestStageMine, pos, bestStageMineTarget))
				{
					continue;
				}
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int moveTU = _save->getPathfinding()->getTotalTUCost();
				_save->getPathfinding()->abortPath();
				const int postMoveTU = _unit->getTimeUnits() - moveTU;
				const bool canThrowAfterMove = postMoveTU >= bestStageThrowTU;
				if (canThrowAfterMove)
				{
					if (moveTU > std::max(10, _unit->getTimeUnits() / 2))
					{
						continue;
					}
				}
				else
				{
					if (moveTU > std::max(18, _unit->getTimeUnits() - 18) || postMoveTU < 18)
					{
						continue;
					}
				}
				if (!canThrowAfterMove && (_visibleEnemies > 0 || _spottingEnemies > 0))
				{
					continue;
				}
				const int spotters = getSpottingUnits(pos);
				if (spotters > 0)
				{
					continue;
				}
				const int exposure = getEnemyFireExposure(pos);
				if (exposure > 70)
				{
					continue;
				}
				int score = bestStageScore + coverScoreAt(pos) * 5;
				score -= moveTU * 2;
				score -= exposure;
				score -= abs(futureThrowDist - std::min(bestStageThrowLimit, bestStageRadius + 1)) * 14;
				score += std::max(0, Position::distance2d(_unit->getPosition(), bestStageMineTarget) - futureThrowDist) * 6;
				if (!canThrowAfterMove)
				{
					score -= 55;
					score += std::min(35, postMoveTU);
				}
				if (score > bestStageMoveScore)
				{
					bestStageMoveScore = score;
					bestStagePos = pos;
					bestStageMoveTU = moveTU;
				}
			}
			if (bestStageMoveScore >= PLAYER_AI_PROXIMITY_STAGE_MOVE_MIN_SCORE)
			{
				_attackAction.actor = _unit;
				_attackAction.weapon = selectBestCarriedWeapon();
				_attackAction.target = bestStagePos;
				_attackAction.type = BA_WALK;
				_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestStagePos, contactPos);
				_AIMode = AI_COMBAT;
				_factionSupportMoveAction = true;
				recordPlayerProximityMineStaging(_save, _unit, _unit->getFaction(), contactPos, bestStageMineTarget);
				recordPlayerProximityMinePlan(_save, _unit->getFaction(), contactPos, bestStageMineTarget);
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction proximity mine staging: unit=" << _unit->getId()
						<< ", mine=" << bestStageMine->getRules()->getType()
						<< ", mineTarget=" << bestStageMineTarget
						<< ", moveTarget=" << bestStagePos
						<< ", score=" << bestStageMoveScore
						<< ", moveTU=" << bestStageMoveTU
						<< ", contact=" << contactPos
						<< ", radius=" << bestStageRadius
						<< ", throwLimit=" << bestStageThrowLimit
						<< ", roomEnemies=" << enemiesInRoom
						<< ", roomSize=" << room->tileCount
						<< ", roomEntries=" << entries
						<< ", fieldMine=" << fieldMine;
					_save->appendToAutoBattleLog(log.str());
				}
				return true;
			}
		}
		if (logEarlyProximitySkips)
		{
			std::ostringstream log;
			log << "Player faction proximity mine diagnostics: unit=" << _unit->getId()
				<< ", contact=" << contactPos
				<< ", fieldMine=" << fieldMine
				<< ", candidates=" << proximityCandidateCount
				<< ", occupied=" << proximityBlockedOccupied
				<< ", nearbyMine=" << proximityBlockedNearbyMine
				<< ", allyUnsafe=" << proximityBlockedAlly
				<< ", noTU=" << proximityBlockedNoTU
				<< ", tooFar=" << proximityBlockedTooFar
				<< ", invalidThrow=" << proximityBlockedInvalidThrow
				<< ", scoreRejected=" << proximityBlockedScore
				<< ", bestScore=" << bestScore
				<< ", bestStageScore=" << bestStageScore
				<< ", bestStageTarget=" << bestStageMineTarget
				<< ", bestStageThrowLimit=" << bestStageThrowLimit
				<< ", roomEnemies=" << enemiesInRoom
				<< ", roomSize=" << room->tileCount
				<< ", roomEntries=" << entries;
			_save->appendToAutoBattleLog(log.str());
		}
		logSkip(bestAction.type == BA_RETHINK ? "no_throw_solution" : "score_too_low", contactPos, room, enemiesInRoom);
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = bestAction.weapon;
	_attackAction.target = bestAction.target;
	_attackAction.type = BA_THROW;
	recordPendingPlayerGrenadeDanger(_save, _unit->getFaction(), bestAction.target,
		bestRadius + PLAYER_AI_THROW_DANGER_MARGIN, 0, false, _unit, bestAction.weapon);
	recordPlayerProximityMinePlan(_save, _unit->getFaction(), contactPos, bestAction.target);
	_rifle = false;
	_melee = false;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction proximity mine ambush: unit=" << _unit->getId()
			<< ", item=" << bestAction.weapon->getRules()->getType()
			<< ", target=" << bestAction.target
			<< ", score=" << bestScore
			<< ", radius=" << bestRadius
			<< ", contact=" << contactPos
			<< ", enemyMoveRadius=" << enemyMoveRadius
			<< ", roomEnemies=" << enemiesInRoom
			<< ", roomSize=" << room->tileCount
			<< ", roomEntries=" << entries
			<< ", fieldMine=" << fieldMine;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Evaluates whether to throw a grenade at an enemy (or group of enemies) we can see.
 */
void PlayerFactionAI::grenadeAction(int minScore, bool teamSpotted)
{
	BattleAction bestAction;
	bestAction.type = BA_RETHINK;
	bestAction.actor = _unit;
	int bestScore = -100000;
	std::string bestReason;
	int grenadeTargetsTried = 0;
	int grenadeItemsDelayed = 0;
	int grenadeNoTU = 0;
	int grenadeNoRadius = 0;
	int grenadeMissingBaseTile = 0;
	int grenadeNoThrowPath = 0;
	int grenadeScoreRejected = 0;
	int grenadeProximityIgnored = 0;
	BattleItem *bestRejectedItem = 0;
	Position bestRejectedTarget(-1, -1, -1);
	int bestRejectedScore = -1000000;
	std::string bestRejectedReason;
	const int explosiveThrowerRank = _unit->getFaction() == FACTION_PLAYER ? getPlayerExplosiveThrowerRank(_unit, _save) : 999;

	auto noteGrenadeReject = [&](BattleItem *grenade, const Position &target, const std::string &reason, int score)
	{
		if (!grenade)
		{
			return;
		}
		const int grenadePower = grenade->getRules() ? std::max(0, grenade->getRules()->getPower()) : 0;
		const int bestPower = bestRejectedItem && bestRejectedItem->getRules() ? std::max(0, bestRejectedItem->getRules()->getPower()) : 0;
		if (!bestRejectedItem || score > bestRejectedScore || (score == bestRejectedScore && grenadePower > bestPower))
		{
			bestRejectedItem = grenade;
			bestRejectedTarget = target;
			bestRejectedScore = score;
			bestRejectedReason = reason;
		}
	};

	auto tryGrenadeTarget = [&](BattleItem *grenade, const Position &baseTarget, const std::string &reason)
	{
		if (!grenade || !grenade->getRules()->isGrenadeOrProxy())
		{
			return;
		}
		const bool proximity = grenade->getRules()->getBattleType() == BT_PROXIMITYGRENADE;
		const bool urgentPlayerExplosive = _unit->getFaction() == FACTION_PLAYER
			&& (_knownEnemies >= 8 || teamSpotted)
			&& (_visibleEnemies > 0 || _spottingEnemies > 0 || teamSpotted);
		if (!proximity && !urgentPlayerExplosive && _save->getTurn() < grenade->getRules()->getAIUseDelay(_save->getMod()))
		{
			++grenadeItemsDelayed;
			noteGrenadeReject(grenade, baseTarget, "ai_use_delay", -100000);
			return;
		}
		if (proximity)
		{
			if (reason.find("visible") != std::string::npos)
			{
				++grenadeProximityIgnored;
				return;
			}
			Position contactPos;
			const BattleRoomInfo *contactRoom = 0;
			int enemiesInRoom = 0;
			bool visibleContact = false;
			const bool hasContact = _factionAI && _factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleContact);
			const int entries = contactRoom ? (int)contactRoom->entryPositions.size() : 0;
			const bool mineableRoom = contactRoom && !contactRoom->isOutside
				&& ((!contactRoom->isHall && contactRoom->tileCount <= 120 && entries > 0 && entries <= 24)
					|| (contactRoom->isHall && contactRoom->tileCount <= 40 && entries > 0 && entries <= 12));
			if (!hasContact || !mineableRoom || enemiesInRoom < PLAYER_AI_PROXIMITY_WORTH_ENEMIES)
			{
				++grenadeProximityIgnored;
				return;
			}
			if (isRecentPlayerProximityMinePlan(_save, _unit->getFaction(), contactPos, &baseTarget, 7))
			{
				++grenadeProximityIgnored;
				return;
			}
		}
		BattleAction action;
		action.weapon = grenade;
		action.type = BA_THROW;
		action.actor = _unit;
		action.target = baseTarget;
		action.updateTU();
		action.Time += PLAYER_AI_PICKUP_TU_COST;
		action += _unit->getActionTUs(BA_PRIME, grenade);
		if (!action.haveTU())
		{
			++grenadeNoTU;
			noteGrenadeReject(grenade, baseTarget, "no_tu_after_prime", -100000);
			return;
		}
		if (!_save->getTile(baseTarget))
		{
			++grenadeMissingBaseTile;
			noteGrenadeReject(grenade, baseTarget, "missing_base_tile", -100000);
			return;
		}
		++grenadeTargetsTried;
		int radius = grenade->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
		if (radius <= 0)
		{
			++grenadeNoRadius;
			noteGrenadeReject(grenade, baseTarget, "no_radius", -100000);
			return;
		}
		const int throwAccuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(action), _save->getMod());
		const int throwLimit = std::max(PLAYER_AI_PROXIMITY_THROW_LIMIT_MIN, throwAccuracy / PLAYER_AI_PROXIMITY_THROW_ACCURACY_DIVISOR);
		std::vector<std::pair<Position, int>> shifts;
		if (proximity)
		{
			const Position candidates[9] = { Position(0, 0, 0), Position(1, 0, 0), Position(0, 1, 0), Position(-1, 0, 0), Position(0, -1, 0), Position(1, 1, 0), Position(1, -1, 0), Position(-1, 1, 0), Position(-1, -1, 0) };
			for (const auto &candidate : candidates)
			{
				Position shifted = baseTarget + candidate;
				if (shifted.x >= 0 && shifted.x < _save->getMapSizeX() && shifted.y >= 0 && shifted.y < _save->getMapSizeY() && shifted.z >= 0 && shifted.z < _save->getMapSizeZ())
				{
					shifts.push_back(std::make_pair(candidate, _unit->distance3dToPositionSq(shifted)));
				}
			}
			std::sort(shifts.begin(), shifts.end(), [](auto& left, auto& right) {
				return left.second < right.second;
			});
		}
		else
		{
			for (int dz = -2; dz <= 0; ++dz)
			{
				for (int dx = -3; dx <= 3; ++dx)
				{
					for (int dy = -3; dy <= 3; ++dy)
					{
						Position candidate(dx, dy, dz);
						Position shifted = baseTarget + candidate;
						if (shifted.x >= 0 && shifted.x < _save->getMapSizeX() && shifted.y >= 0 && shifted.y < _save->getMapSizeY() && shifted.z >= 0 && shifted.z < _save->getMapSizeZ())
						{
							shifts.push_back(std::make_pair(candidate, abs(dx) + abs(dy) + abs(dz) * 2));
						}
					}
				}
			}
			std::sort(shifts.begin(), shifts.end(), [](auto& left, auto& right) {
				return left.second < right.second;
			});
		}
		BattleUnit *baseTargetUnit = 0;
		Tile *baseTile = _save->getTile(baseTarget);
		if (baseTile)
		{
			baseTargetUnit = baseTile->getUnit();
		}
		bool grenadeSolvesBadDirectFire = false;
		if (!proximity && baseTargetUnit && validTarget(baseTargetUnit, true, true))
		{
			BattleItem *directWeapon = selectBestCarriedWeapon();
			if (directWeapon && directWeapon != grenade && directWeapon->getRules()->getBattleType() == BT_FIREARM)
			{
				BattleAction directAction;
				directAction.actor = _unit;
				directAction.weapon = directWeapon;
				directAction.type = BA_SNAPSHOT;
				directAction.target = baseTargetUnit->getPosition();
				Position shotOrigin = _save->getTileEngine()->getOriginVoxel(directAction, 0);
				Position shotTarget = baseTargetUnit->getPosition().toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY, 10);
				Tile *directTile = baseTargetUnit->getTile();
				const bool directLine = directTile && _save->getTileEngine()->canTargetUnit(&shotOrigin, directTile, &shotTarget, _unit, false, baseTargetUnit);
				BattleActionAttack directAttack = BattleActionAttack::GetBeforeShoot(directAction);
				const int directPower = directAttack.damage_item ? std::max(0, directAttack.damage_item->getRules()->getPower()) : 0;
				const int armor = std::max(std::max(baseTargetUnit->getArmor(SIDE_FRONT), baseTargetUnit->getArmor(SIDE_LEFT)), baseTargetUnit->getArmor(SIDE_RIGHT));
				const bool weakDirectHit = directPower > 0 && (directPower + 20 < armor || (baseTargetUnit->getHealth() > 55 && directPower < armor + 35));
				grenadeSolvesBadDirectFire = !directLine || projectileRiskyForAllies(&directAction, baseTargetUnit, false) || weakDirectHit;
			}
		}
		bool hadValidThrowPath = false;
		for (auto& shift : shifts)
		{
			Position targetTile = baseTarget + shift.first;
			Tile *tile = _save->getTile(targetTile);
			if (!tile)
			{
				continue;
			}
			BattleAction probeAction = action;
			probeAction.target = targetTile;
			Position originVoxel = _save->getTileEngine()->getOriginVoxel(probeAction, 0);
			Position targetVoxel = targetTile.toVoxel() + Position(PLAYER_AI_THROW_TARGET_VOXEL_XY, PLAYER_AI_THROW_TARGET_VOXEL_XY, (PLAYER_AI_THROW_TARGET_VOXEL_Z_BASE + -tile->getTerrainLevel()));
			if (_save->getTileEngine()->validateThrow(probeAction, originVoxel, targetVoxel, _save->getDepth()))
			{
				hadValidThrowPath = true;
				std::string rejectReason;
				int score = scorePlayerGrenadeTarget(grenade, targetTile, radius, proximity, &rejectReason);
				const int throwDist = Position::distance2d(targetTile, _unit->getPosition());
				if (proximity)
				{
					score += 70;
					score -= Position::distance2d(targetTile, _unit->getPosition()) * 2;
					if (reason.find("room_entry") != std::string::npos)
					{
						score += 170;
						score += radius * 12;
					}
					else if (reason.find("hidden_contact") != std::string::npos)
					{
						score += 105;
						score += radius * 8;
					}
					if (getSpottingUnits(_unit->getPosition()) > 0 && score < 180)
					{
						score -= 45;
					}
				}
				else
				{
					if (throwDist > throwLimit + 3)
					{
						score = -100000;
						if (rejectReason.empty())
						{
							rejectReason = "throw_too_far_for_accuracy";
						}
					}
					else if (throwDist > throwLimit || (throwAccuracy < 65 && throwDist > 6))
					{
						score -= std::max(0, throwDist - throwLimit) * 90;
						score -= std::max(0, 65 - throwAccuracy) * 6;
						score -= std::max(0, throwDist - 6) * 35;
					}
					const int currentSpotters = getSpottingUnits(_unit->getPosition());
					const int currentExposure = getEnemyFireExposure(_unit->getPosition());
					if (grenadeSolvesBadDirectFire)
					{
						score += 110;
					}
					if (currentSpotters > 0 && score < 160)
					{
						score -= currentSpotters * (grenadeSolvesBadDirectFire ? 15 : 35);
					}
					if (currentExposure > 0 && score < 190)
					{
						score -= std::min(grenadeSolvesBadDirectFire ? 25 : 45, currentExposure / (grenadeSolvesBadDirectFire ? 6 : 4));
					}
					if (isPlayerExplosiveDanger(_save, _unit->getFaction(), targetTile))
					{
						score -= teamSpotted ? 220 : 150;
					}
				}
				if (score < PLAYER_AI_GRENADE_REJECT_CUTOFF)
				{
					++grenadeScoreRejected;
					noteGrenadeReject(grenade, targetTile, rejectReason.empty() ? "score_rejected" : rejectReason, score);
					continue;
				}
				if (score > bestScore)
				{
					bestScore = score;
					bestAction = probeAction;
					bestReason = reason;
				}
			}
		}
		if (!hadValidThrowPath)
		{
			++grenadeNoThrowPath;
			noteGrenadeReject(grenade, baseTarget, "no_throw_path", -100001);
		}
	};

	for (auto *item : *_unit->getInventory())
	{
		if (!item || !item->getRules()->isGrenadeOrProxy()
			|| item->getRules()->getBattleType() == BT_PROXIMITYGRENADE
			|| isPlayerSmokeGrenade(item))
		{
			continue;
		}
		if (_aggroTarget && !_aggroTarget->isOut())
		{
			tryGrenadeTarget(item, _aggroTarget->getPosition(), "visible_or_assigned_target");
		}
		for (auto *bu : *_save->getUnits())
		{
			if (validTarget(bu, true, true) && bu->getTile() && _save->getTileEngine()->visible(_unit, bu->getTile()))
			{
				tryGrenadeTarget(item, bu->getPosition(), "visible_enemy_cluster");
			}
		}
		if (_factionAI)
		{
			Position contactPos;
			const BattleRoomInfo *room = 0;
			int enemiesInRoom = 0;
			bool visibleContact = false;
			if (_factionAI->getBestEnemyContactPosition(&contactPos, &room, &enemiesInRoom, &visibleContact))
			{
				if (!visibleContact)
				{
					tryGrenadeTarget(item, contactPos, item->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? "hidden_contact_sensor_grenade" : "hidden_contact_explosive");
				}
				if (room && item->getRules()->getBattleType() == BT_PROXIMITYGRENADE)
				{
					for (const auto &entry : room->entryPositions)
					{
						if (entry.z == _unit->getPosition().z && Position::distance2d(entry, _unit->getPosition()) <= 22)
						{
							tryGrenadeTarget(item, entry, "room_entry_sensor_grenade");
						}
					}
				}
			}
		}
	}

	const int requiredScore = bestAction.type != BA_RETHINK && bestAction.weapon && bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? 120 : minScore;
	if (bestAction.type != BA_RETHINK && bestScore >= requiredScore)
	{
		_attackAction.weapon = bestAction.weapon;
		_attackAction.target = bestAction.target;
		_attackAction.type = BA_THROW;
		const int dangerRadius = bestAction.weapon->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(bestAction));
		const int dangerPower = std::max(0, bestAction.weapon->getRules()->getPower());
		const int dangerSafetyRadius = dangerRadius + PLAYER_AI_THROW_DANGER_MARGIN;
		recordPendingPlayerGrenadeDanger(_save, _unit->getFaction(), bestAction.target,
			dangerSafetyRadius, dangerPower, true, _unit, bestAction.weapon);
		if (bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE && _factionAI)
		{
			Position contactPos;
			const BattleRoomInfo *contactRoom = 0;
			int enemiesInRoom = 0;
			bool visibleContact = false;
			if (_factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleContact))
			{
				recordPlayerProximityMinePlan(_save, _unit->getFaction(), contactPos, bestAction.target);
			}
		}
		_rifle = false;
		_melee = false;
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction explosive action: unit=" << _unit->getId()
				<< ", item=" << bestAction.weapon->getRules()->getType()
				<< ", target=" << bestAction.target
				<< ", score=" << bestScore
				<< ", reason=" << bestReason
				<< ", requiredScore=" << requiredScore
				<< ", teamSpotted=" << teamSpotted
				<< ", explosiveThrowerRank=" << explosiveThrowerRank
				<< ", proximity=" << (bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE)
				<< ", pendingRadius=" << dangerSafetyRadius;
			_save->appendToAutoBattleLog(log.str());
		}
	}
	else if (_unit->getFaction() == FACTION_PLAYER && bestAction.type == BA_RETHINK && bestRejectedItem
		&& bestRejectedItem->getRules()->getBattleType() != BT_PROXIMITYGRENADE
		&& (bestRejectedReason.find("throw_too_far") != std::string::npos
			|| bestRejectedReason.find("no_throw_path") != std::string::npos
			|| bestRejectedReason.find("trajectory") != std::string::npos))
	{
		Tile *targetTile = _save->getTile(bestRejectedTarget);
		BattleUnit *targetUnit = targetTile ? targetTile->getUnit() : 0;
		bool highValueTarget = teamSpotted || _spottingEnemies > 0 || _visibleEnemies > 0;
		int targetThreat = 0;
		if (targetUnit && validTarget(targetUnit, true, true))
		{
			const int armor = std::max(std::max(targetUnit->getArmor(SIDE_FRONT), targetUnit->getArmor(SIDE_LEFT)), targetUnit->getArmor(SIDE_RIGHT));
			BattleItem *enemyWeapon = targetUnit->getMainHandWeapon(false);
			const int enemyWeaponDanger = playerAIWeaponDirectDanger(enemyWeapon);
			targetThreat = targetUnit->getHealth() + armor + enemyWeaponDanger;
			highValueTarget = highValueTarget
				|| targetUnit->getHealth() >= 70
				|| armor >= 30
				|| enemyWeaponDanger >= 120;
		}
		if (highValueTarget && !_reachable.empty())
		{
			BattleAction stagedThrow;
			stagedThrow.actor = _unit;
			stagedThrow.weapon = bestRejectedItem;
			stagedThrow.type = BA_THROW;
			stagedThrow.target = bestRejectedTarget;
			stagedThrow.updateTU();
			stagedThrow.Time += PLAYER_AI_PICKUP_TU_COST;
			stagedThrow += _unit->getActionTUs(BA_PRIME, bestRejectedItem);
			const int radius = bestRejectedItem->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(stagedThrow));
			const int throwAccuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(stagedThrow), _save->getMod());
			const int throwLimit = std::max(PLAYER_AI_PROXIMITY_THROW_LIMIT_MIN, throwAccuracy / PLAYER_AI_PROXIMITY_THROW_ACCURACY_DIVISOR);
			const int currentThrowDist = Position::distance2d(_unit->getPosition(), bestRejectedTarget);
			Position bestStagePos;
			int bestStageScore = -100000;
			int bestStageMoveTU = 0;
			int bestStageDist = currentThrowDist;
			auto stageCoverScore = [&](const Position &pos) -> int
			{
				Tile *tile = _save->getTile(pos);
				if (!tile)
				{
					return 0;
				}
				int cover = 0;
				if (tile->getMapData(O_OBJECT))
				{
					cover += 8;
				}
				if (tile->getMapData(O_NORTHWALL))
				{
					cover += bestRejectedTarget.y < pos.y ? 14 : 5;
				}
				if (tile->getMapData(O_WESTWALL))
				{
					cover += bestRejectedTarget.x < pos.x ? 14 : 5;
				}
				return cover;
			};
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				const Position pos = tile->getPosition();
				if (pos == _unit->getPosition() || pos.z != _unit->getPosition().z || isPlayerExplosiveDanger(_save, _unit->getFaction(), pos))
				{
					continue;
				}
				const int futureThrowDist = Position::distance2d(pos, bestRejectedTarget);
				if (futureThrowDist > throwLimit || futureThrowDist >= currentThrowDist - 1)
				{
					continue;
				}
				if (radius > 0 && futureThrowDist <= radius + 1)
				{
					continue;
				}
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				if (_save->getPathfinding()->getStartDirection() == -1)
				{
					_save->getPathfinding()->abortPath();
					continue;
				}
				const int moveTU = _save->getPathfinding()->getTotalTUCost();
				_save->getPathfinding()->abortPath();
				if (moveTU > std::max(0, _unit->getTimeUnits() - stagedThrow.Time))
				{
					continue;
				}
				const int spotters = getSpottingUnits(pos);
				const int exposure = getEnemyFireExposure(pos);
				const int fireLines = countEnemyFireLines(pos);
				if (spotters > 0 || fireLines > 0 || exposure > 70)
				{
					continue;
				}
				const int cover = stageCoverScore(pos);
				int score = targetThreat + std::max(0, currentThrowDist - futureThrowDist) * 18 + cover * 8;
				score -= moveTU * 3;
				score -= exposure;
				score -= abs(futureThrowDist - std::min(throwLimit, std::max(radius + 2, 5))) * 12;
				if (score > bestStageScore)
				{
					bestStageScore = score;
					bestStagePos = pos;
					bestStageMoveTU = moveTU;
					bestStageDist = futureThrowDist;
				}
			}
			if (bestStageScore >= 120)
			{
				_attackAction.actor = _unit;
				_attackAction.weapon = selectBestCarriedWeapon();
				_attackAction.target = bestStagePos;
				_attackAction.type = BA_WALK;
				_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestStagePos, bestRejectedTarget);
				_AIMode = AI_COMBAT;
				_factionSupportMoveAction = true;
				_rifle = false;
				_melee = false;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction grenade staging move: unit=" << _unit->getId()
						<< ", grenade=" << bestRejectedItem->getRules()->getType()
						<< ", target=" << bestRejectedTarget
						<< ", moveTarget=" << bestStagePos
						<< ", reason=" << bestRejectedReason
						<< ", score=" << bestStageScore
						<< ", moveTU=" << bestStageMoveTU
						<< ", throwDist=" << bestStageDist
						<< ", currentThrowDist=" << currentThrowDist
						<< ", throwLimit=" << throwLimit
						<< ", radius=" << radius
						<< ", targetThreat=" << targetThreat
						<< ", teamSpotted=" << teamSpotted;
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}
	else if (Options::autoBattleLog && bestAction.type != BA_RETHINK)
	{
		std::ostringstream log;
		log << "Player faction explosive candidate rejected: unit=" << _unit->getId()
			<< ", item=" << bestAction.weapon->getRules()->getType()
			<< ", target=" << bestAction.target
			<< ", score=" << bestScore
			<< ", reason=" << bestReason
			<< ", requiredScore=" << requiredScore
			<< ", teamSpotted=" << teamSpotted
			<< ", proximity=" << (bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE);
		_save->appendToAutoBattleLog(log.str());
	}
	else if (Options::autoBattleLog)
	{
		int carriedExplosives = 0;
		for (auto *item : *_unit->getInventory())
		{
			if (item && item->getRules()->isGrenadeOrProxy())
			{
				++carriedExplosives;
			}
		}
		if (carriedExplosives > 0)
		{
			std::ostringstream log;
			log << "Player faction explosive no target: unit=" << _unit->getId()
				<< ", carriedExplosives=" << carriedExplosives
				<< ", aggroTarget=" << (_aggroTarget ? _aggroTarget->getId() : -1)
				<< ", known=" << _knownEnemies
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", requiredScore=" << minScore
				<< ", teamSpotted=" << teamSpotted
				<< ", targetsTried=" << grenadeTargetsTried
				<< ", delayed=" << grenadeItemsDelayed
				<< ", noTU=" << grenadeNoTU
				<< ", noRadius=" << grenadeNoRadius
				<< ", missingBaseTile=" << grenadeMissingBaseTile
				<< ", noThrowPath=" << grenadeNoThrowPath
				<< ", scoreRejected=" << grenadeScoreRejected
				<< ", proximityIgnored=" << grenadeProximityIgnored
				<< ", bestRejectedItem=" << (bestRejectedItem ? bestRejectedItem->getRules()->getType() : "none")
				<< ", bestRejectedTarget=" << bestRejectedTarget
				<< ", bestRejectedScore=" << bestRejectedScore
				<< ", bestRejectedReason=" << (bestRejectedReason.empty() ? "none" : bestRejectedReason);
			_save->appendToAutoBattleLog(log.str());
		}
	}
}

/**
 * Attempts a psionic attack on an enemy we "know of".
 *
 * Psionic targetting: pick from any of the "exposed" units.
 * Exposed means they have been previously spotted, and are therefore "known" to the AI,
 * regardless of whether we can see them or not, because we're psychic.
 * @return True if a psionic attack is performed.
 */
bool PlayerFactionAI::psiAction()
{
	BattleItem *item = _unit->getUtilityWeapon(BT_PSIAMP);
	if (!item)
	{
		return false;
	}

	const int costLength = 3;
	BattleActionCost cost[costLength] =
	{
		BattleActionCost(BA_USE, _unit, item),
		BattleActionCost(BA_PANIC, _unit, item),
		BattleActionCost(BA_MINDCONTROL, _unit, item),
	};
	bool have = false;
	for (int j = 0; j < costLength; ++j)
	{
		if (cost[j].Time > 0)
		{
			cost[j].Time += _escapeTUs;
			cost[j].Energy += _escapeTUs / 2;
			have |= cost[j].haveTU();
		}
	}
	bool LOSRequired = item->getRules()->isLOSRequired();

	_aggroTarget = 0;
		// don't let mind controlled soldiers mind control other soldiers.
	if (_unit->getOriginalFaction() == _unit->getFaction()
		// and we have the required 25 TUs and can still make it to cover
		&& have
		// and we didn't already do a psi action this round
		&& !_didPsi)
	{
		int weightToAttack = 0;
		BattleActionType typeToAttack = BA_NONE;

		for (auto* bu : *_save->getUnits())
		{
			// don't target tanks
			if (bu->getArmor()->getSize() == 1 &&
				validTarget(bu, true, false) &&
				// they must be player units
				bu->getOriginalFaction() != _unit->getFaction() &&
				(!LOSRequired ||
				std::find(_unit->getVisibleUnits()->begin(), _unit->getVisibleUnits()->end(), bu) != _unit->getVisibleUnits()->end()))
			{
				BattleUnit *victim = bu;
				if (item->getRules()->isOutOfRange(_unit->distance3dToUnitSq(victim)))
				{
					continue;
				}
				for (int j = 0; j < costLength; ++j)
				{
					// can't use this attack.
					if (!cost[j].haveTU())
					{
						continue;
					}

					int weightToAttackMe = _save->getTileEngine()->psiAttackCalculate({ cost[j].type, _unit, item, item }, victim);

					// low chance we hit this target.
					if (weightToAttackMe < 0)
					{
						continue;
					}

					// different bonus per attack.
					if (cost[j].type == BA_MINDCONTROL)
					{
						// target cannot be mind controlled
						if (victim->getUnitRules() && !victim->getUnitRules()->canBeMindControlled()) continue;

						int controlOdds = 40;
						int morale = victim->getMorale();
						int bravery = victim->reduceByBravery(10);
						if (bravery > 6)
							controlOdds -= 15;
						if (bravery < 4)
							controlOdds += 15;
						if (morale >= 40)
						{
							if (morale - 10 * bravery < 50)
								controlOdds -= 15;
						}
						else
						{
							controlOdds += 15;
						}
						if (!morale)
						{
							controlOdds = 100;
						}
						if (RNG::percent(controlOdds))
						{
							weightToAttackMe += 60;
						}
						else
						{
							continue;
						}
					}
					else if (cost[j].type == BA_USE)
					{
						if (RNG::percent(80 - _attackAction.diff * 10)) // Star gods have mercy on us.
						{
							continue;
						}
						BattleActionAttack attack = BattleActionAttack{ BA_USE, _unit, item, item };
						int radius = item->getRules()->getExplosionRadius(attack);
						if (radius > 0)
						{
							int efficity = explosiveEfficacy(victim->getPosition(), _unit, radius, _attackAction.diff);
							if (efficity)
							{
								weightToAttackMe += 2 * efficity * _intelligence; //bonus for boom boom.
							}
							else
							{
								continue;
							}
						}
						else
						{
							weightToAttackMe += item->getRules()->getPowerBonus(attack);
						}
					}
					else if (cost[j].type == BA_PANIC)
					{
						// target cannot be panicked
						if (victim->getUnitRules() && !victim->getUnitRules()->canPanic()) continue;

						weightToAttackMe += 40;
					}

					if (weightToAttackMe > weightToAttack)
					{
						typeToAttack = cost[j].type;
						weightToAttack = weightToAttackMe;
						_aggroTarget = victim;
					}
				}
			}
		}

		if (!_aggroTarget || !weightToAttack) return false;

		if (_visibleEnemies && _attackAction.weapon)
		{
			BattleActionType actions[] = {
				BA_AIMEDSHOT,
				BA_AUTOSHOT,
				BA_SNAPSHOT,
				BA_HIT,
			};
			for (BattleActionType action : actions)
			{
				auto* ammo = _attackAction.weapon->getAmmoForAction(action);
				if (!ammo)
				{
					continue;
				}

				int weightPower = ammo->getRules()->getPowerBonus({ action, _attackAction.actor, _attackAction.weapon, ammo });
				if (action == BA_HIT)
				{
					// prefer psi over melee
					weightPower /= 2;
				}
				else
				{
					// prefer machine guns
					weightPower *= _attackAction.weapon->getActionConf(action)->shots;
				}
				if (weightPower >= weightToAttack)
				{
					return false;
				}
			}
		}
		else if (RNG::generate(35, 155) >= weightToAttack)
		{
			return false;
		}

		if (_traceAI)
		{
			Log(LOG_INFO) << "making a psionic attack this turn";
		}

		_psiAction.type = typeToAttack;
		_psiAction.target = _aggroTarget->getPosition();
		_psiAction.weapon = item;
		return true;
	}
	return false;
}

/**
 * Performs a melee attack action.
 */
void PlayerFactionAI::meleeAttack()
{
	_unit->lookAt(_aggroTarget->getPosition() + Position(_unit->getArmor()->getSize()-1, _unit->getArmor()->getSize()-1, 0), false);
	while (_unit->getStatus() == STATUS_TURNING)
		_unit->turn();
	if (_traceAI) { Log(LOG_INFO) << "Attack unit: " << _aggroTarget->getId(); }
	_attackAction.target = _aggroTarget->getPosition();
	_attackAction.type = BA_HIT;
	_attackAction.weapon = _unit->getUtilityWeapon(BT_MELEE);
}


/**
 *
 * @param target
 * @return
 */
AIAttackWeight PlayerFactionAI::getTargetAttackWeight(BattleUnit* target) const
{
	AIAttackWeight weight = AIW_IGNORED;

	if (target->getFaction() == _unit->getFaction())
	{
		// friendly target have negative weight, used for AoE attacks.
		weight = target->getAITargetWeightAsFriendly(_save->getMod());
	}
	else if (
		_intelligence < target->getTurnsSinceSpottedByFaction(_unit->getFaction()) &&
		(!_unit->isSniper() || !target->getTurnsLeftSpottedForSnipersByFaction(_unit->getFaction())))
	{
		// ignore units that we don't "know" about...
		// ... unless we are a sniper and the spotters know about them
		weight = AIW_IGNORED;
	}
	else if (target->getFaction() == FACTION_HOSTILE || _unit->getFaction() == FACTION_HOSTILE)
	{
		if (target->getFaction() == _targetFaction)
		{
			// enemy unit, full weight
			weight = target->getAITargetWeightAsHostile(_save->getMod());
		}
		else
		{
			// if its not xcom unit then its civilian, less value that xcom
			weight = target->getAITargetWeightAsHostileCivilians(_save->getMod());
		}
	}
	else if (target->getFaction() == FACTION_NEUTRAL || _unit->getFaction() == FACTION_NEUTRAL)
	{
		// if its not alien then its xcom or civilian, humans do not shoot each other, usually...
		weight = target->getAITargetWeightAsNeutral(_save->getMod());
	}

	weight = (AIAttackWeight)ModScript::scriptFunc2<ModScript::AiCalculateTargetWeight>(
		_unit->getArmor(),
		weight, weight,
		_unit, target, _save
	);

	return weight;
}

/**
 * Validates a target.
 * @param target the target we want to validate.
 * @param assessDanger do we care if this unit was previously targetted with a grenade?
 * @param includeCivs do we include civilians in the threat assessment?
 * @return whether this target is someone we would like to kill.
 */
bool PlayerFactionAI::validTarget(BattleUnit *target, bool assessDanger, bool includeCivs) const
{
	// ignore units that:
	// 1. are dead/unconscious
	// 2. are dangerous (they have been grenaded)
	// 3. are hostile/neutral units marked as ignored by the AI
	if (target->isOut() ||
		(assessDanger && target->getTile()->getDangerous()) ||
		(target->getFaction() != FACTION_PLAYER && target->isIgnoredByAI()))
	{
		return false;
	}

	if (includeCivs)
	{
		return  getTargetAttackWeight(target) > AIW_IGNORED;
	}
	else
	{
		return  getTargetAttackWeight(target) > _save->getMod()->getAITargetWeightThreatThreshold();
	}
}

/**
 * Checks the alien's reservation setting.
 * @return the reserve setting.
 */
BattleActionType PlayerFactionAI::getReserveMode()
{
	return _reserve;
}

/**
 * We have a dichotomy on our hands: we have a ranged weapon and melee capability.
 * let's make a determination on which one we'll be using this round.
 */
void PlayerFactionAI::selectMeleeOrRanged()
{
	BattleItem *range = _attackAction.weapon;
	BattleItem *melee = _unit->getUtilityWeapon(BT_MELEE);

	if (!melee || !melee->haveAnyAmmo())
	{
		// no idea how we got here, but melee is definitely out of the question.
		_melee = false;
		return;
	}
	if (!range || !range->haveAnyAmmo())
	{
		_rifle = false;
		return;
	}

	const RuleItem *meleeRule = melee->getRules();

	int meleeOdds = 10;

	int dmg = _aggroTarget->reduceByResistance(meleeRule->getPowerBonus(BattleActionAttack::GetBeforeShoot(BA_HIT, _unit, melee)), meleeRule->getDamageType()->ResistType);

	if (dmg > 50)
	{
		meleeOdds += (dmg - 50) / 2;
	}
	if ( _visibleEnemies > 1 )
	{
		meleeOdds -= 20 * (_visibleEnemies - 1);
	}

	if (meleeOdds > 0 && _unit->getHealth() >= 2 * _unit->getBaseStats()->health / 3)
	{
		if (_unit->getAggression() == 0)
		{
			meleeOdds -= 20;
		}
		else if (_unit->getAggression() > 1)
		{
			meleeOdds += 10 * _unit->getAggression();
		}

		if (RNG::percent(meleeOdds))
		{
			_rifle = false;
			_attackAction.weapon = melee;
			_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_HIT, _unit, melee));
			return;
		}
	}
	_melee = false;
}

/**
 * Checks nearby nodes to see if they'd make good grenade targets
 * @param action contains our details one weapon and user, and we set the target for it here.
 * @return if we found a viable node or not.
 */
bool PlayerFactionAI::getNodeOfBestEfficacy(BattleAction *action, int radius)
{
	int bestScore = 2;
	Position originVoxel = _save->getTileEngine()->getSightOriginVoxel(_unit);
	Position targetVoxel;
	for (const auto* node : *_save->getNodes())
	{
		if (node->isDummy())
		{
			continue;
		}
		int dist = Position::distance2d(node->getPosition(), _unit->getPosition());
		if (dist <= 20 && dist > radius &&
			_save->getTileEngine()->canTargetTile(&originVoxel, _save->getTile(node->getPosition()), O_FLOOR, &targetVoxel, _unit, false))
		{
			int nodePoints = 0;
			for (auto* bu : *_save->getUnits())
			{
				dist = Position::distance2d(node->getPosition(), bu->getPosition());
				if (!bu->isOut() && dist < radius)
				{
					Position targetOriginVoxel = _save->getTileEngine()->getSightOriginVoxel(bu);
					if (_save->getTileEngine()->canTargetTile(&targetOriginVoxel, _save->getTile(node->getPosition()), O_FLOOR, &targetVoxel, bu, false))
					{
						if ((_unit->getFaction() == FACTION_HOSTILE && bu->getFaction() != FACTION_HOSTILE) ||
							(_unit->getFaction() == FACTION_NEUTRAL && bu->getFaction() == FACTION_HOSTILE))
						{
							if (bu->getTurnsSinceSpottedByFaction(_unit->getFaction()) <= _intelligence)
							{
								nodePoints++;
							}
						}
						else
						{
							nodePoints -= 2;
						}
					}
				}
			}
			if (nodePoints > bestScore)
			{
				bestScore = nodePoints;
				action->target = node->getPosition();
			}
		}
	}
	return bestScore > 2;
}

BattleUnit* PlayerFactionAI::getTarget()
{
	return _aggroTarget;
}

void PlayerFactionAI::freePatrolTarget()
{
	if (_toNode)
	{
		_toNode->freeNode();
	}
}

}
