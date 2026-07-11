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
#include "PlayerFactionPlanner.h"
#include "TileEngine.h"
#include "../Engine/Options.h"
#include "../Mod/RuleItem.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/Tile.h"
#include <algorithm>
#include <climits>
#include <sstream>

namespace OpenXcom
{

namespace
{

constexpr int PLAYER_AI_OPEN_ROOM_TILE_LIMIT = 90; // Размер комнаты, после которого planner трактует ее как открытую зону.
constexpr int PLAYER_AI_BASE_ENEMY_THREAT = 40; // Базовая угроза любого активного hostile.
constexpr int PLAYER_AI_ARMED_THREAT_BONUS = 35; // Бонус угрозы за оружие в основной руке.
constexpr int PLAYER_AI_MELEE_THREAT_BONUS = 20; // Бонус угрозы за melee-оружие.
constexpr int PLAYER_AI_REJECT_SCORE = -100000; // Sentinel-score для невозможного назначения.
constexpr int PLAYER_AI_MIN_PRESSURE_ACCURACY = 30; // Минимальная точность, используемая при оценке pressure оружия.
constexpr int PLAYER_AI_PRESSURE_PERCENT = 100; // База процентной формулы expected pressure.
constexpr int PLAYER_AI_MELEE_CLOSE_RANGE = 3; // Дистанция, на которой melee-актеру выгодна цель.
constexpr int PLAYER_AI_MELEE_CLOSE_ASSIGNMENT_BONUS = 45; // Бонус melee-назначения по близкой цели.
constexpr int PLAYER_AI_MELEE_FAR_ASSIGNMENT_PENALTY = 60; // Штраф melee-назначения по дальней цели.
constexpr int PLAYER_AI_CAN_SHOOT_ASSIGNMENT_BONUS = 150; // Бонус назначения цели, по которой юнит уже может стрелять.
constexpr int PLAYER_AI_CAN_SEE_ASSIGNMENT_BONUS = 30; // Бонус назначения цели, которую юнит видит.
constexpr int PLAYER_AI_UNSEEN_VISIBLE_CONTACT_PENALTY = 80; // Штраф назначения видимого фракции контакта юниту без своей видимости.
constexpr int PLAYER_AI_WOUNDED_HEALTH_LIMIT = 35; // Health, ниже которого цель считается раненой для фокуса.
constexpr int PLAYER_AI_WOUNDED_ASSIGNMENT_BONUS = 65; // Бонус добивания раненой цели.
constexpr int PLAYER_AI_WOUNDED_OVERFOCUS_PENALTY = 25; // Штраф за каждого уже назначенного на раненую цель.
constexpr int PLAYER_AI_NORMAL_OVERFOCUS_PENALTY = 35; // Штраф за каждого уже назначенного на обычную цель.
constexpr int PLAYER_AI_VISIBLE_BY_FOCUS_BONUS = 20; // Focus score за каждого союзника, который видит цель.
constexpr int PLAYER_AI_CAN_SHOOT_BY_FOCUS_BONUS = 35; // Focus score за каждого союзника, который может стрелять по цели.
constexpr int PLAYER_AI_HIDDEN_CONTACT_LIMIT = 32; // Сохраняем полную картину hidden contacts, чтобы распределять поиск и взрывчатку по всему отряду.
constexpr int PLAYER_AI_HIDDEN_FOCUS_BASE_PENALTY = 90; // Базовый штраф focus score для hidden contact.
constexpr int PLAYER_AI_HIDDEN_DISTANCE_PENALTY = 2; // Штраф hidden contact за дистанцию до ближайшего союзника.
constexpr int PLAYER_AI_HIDDEN_THREAT_SCALE_NUM = 2; // Числитель снижения threat hidden contact.
constexpr int PLAYER_AI_HIDDEN_THREAT_SCALE_DEN = 3; // Знаменатель снижения threat hidden contact.
constexpr int PLAYER_AI_ROOM_CONTACT_THREAT_BONUS = 20; // Базовый бонус угрозы за контакт в закрытой комнате.
constexpr int PLAYER_AI_ROOM_EXTRA_ENEMY_THREAT_BONUS = 45; // Бонус угрозы за каждого дополнительного врага в комнате.
constexpr int PLAYER_AI_ROOM_CONTROLLED_OPENING_LIMIT = 2; // Двери/окна, ниже которых комната считается контролируемой/узкой.
constexpr int PLAYER_AI_HALL_EXTRA_ENEMY_THREAT_BONUS = 20; // Бонус угрозы за дополнительных врагов в hall/open zone.
constexpr int PLAYER_AI_LATE_HUNT_TURN = 50; // Ход, после которого включается late hunt при малом числе hostile.
constexpr int PLAYER_AI_SMALL_FORCE_HUNT_TURN = 12; // Малый скрытый остаток нельзя ждать до общего late-hunt turn.
constexpr int PLAYER_AI_SMALL_FORCE_HUNT_LIMIT = 5; // Пять hidden hostile можно искать только при численном паритете; см. smallForceHunt.
constexpr int PLAYER_AI_LONE_GUERRILLA_TURN = 24; // После этого хода один-два бойца должны искать скрытого врага короткими перебежками.
constexpr int PLAYER_AI_LONE_GUERRILLA_HOSTILE_LIMIT = 5; // Максимальный остаток hostile для осторожного lone-guerrilla поиска.
constexpr int PLAYER_AI_HIDDEN_ASSIGN_ALL_TURN = 16; // Ход, после которого hidden target можно назначать всему отряду.
constexpr int PLAYER_AI_DANGEROUS_VISIBLE_THREAT = 130; // Threat видимой цели, позволяющий поднять лимит назначенных стрелков.
constexpr int PLAYER_AI_MANY_ENEMIES_MIN = 5; // Минимум hostile для режима many-enemies pressure.
constexpr int PLAYER_AI_OUTNUMBERED_MARGIN = 8; // Небольшой перевес hostile не должен преждевременно переводить здоровый отряд в survival.
constexpr int PLAYER_AI_BADLY_EXPOSED_MIN_ALLIES = 2; // Минимум засвеченных союзников для squadBadlyExposed.
constexpr int PLAYER_AI_BADLY_EXPOSED_DIVISOR = 3; // Доля отряда, засветка которой считается badly exposed.
constexpr int PLAYER_AI_LAST_ENEMY_LIMIT = 2; // Число hostile, при котором включается hunt-last-enemy.
constexpr int PLAYER_AI_EARLY_PRESSURE_TURN_LIMIT = 2; // Последний ход early hidden pressure.
constexpr int PLAYER_AI_EARLY_PRESSURE_MIN_ALLIES = 5; // Минимум активных союзников для early hidden pressure.
constexpr int PLAYER_AI_SURVIVAL_ALLY_LIMIT = 3; // Размер отряда, при котором survive включается без доп. условий.
constexpr int PLAYER_AI_SURVIVAL_WOUNDED_DIVISOR = 4; // Доля раненых, после которой outnumbered считается survival pressure.
constexpr int PLAYER_AI_LATE_SURVIVAL_TURN = 2; // После этого хода включается late outnumbered pressure.
constexpr int PLAYER_AI_LATE_SURVIVAL_HOSTILE_MARGIN = 10; // Групповой skirmish включается только при критическом, а не временном численном перевесе.
constexpr int PLAYER_AI_ASSIGNMENT_DISTANCE_PENALTY = 3; // Штраф score назначения за каждую клетку дистанции до цели.
constexpr int PLAYER_AI_SMALL_VISIBLE_CONTACT_LIMIT = 2; // Количество видимых контактов, которое planner считает малым.
constexpr int PLAYER_AI_SIEGE_ROOM_CONTACT_LIMIT = 2; // Минимум комнатных контактов для явного siege-room режима.
constexpr int PLAYER_AI_HOLD_REACTION_HOSTILE_LIMIT = 6; // Число hostile, после которого малый видимый контакт удерживается reaction-режимом.
constexpr int PLAYER_AI_VISIBLE_MIN_ASSIGNEES = 3; // Минимум бойцов, которых можно назначить на видимую цель.
constexpr int PLAYER_AI_VISIBLE_EXTRA_ASSIGNEE_DIVISOR = 2; // Доля видящих, но не стреляющих союзников, добавляемая к лимиту видимой цели.
constexpr int PLAYER_AI_HIDDEN_BLIND_ASSIGNEE_LIMIT = 3; // Лимит назначений на hidden contact без видящих/стреляющих союзников.
constexpr int PLAYER_AI_HIDDEN_SEEN_ASSIGNEE_LIMIT = 5; // Лимит назначений на hidden contact с косвенной видимостью/линией огня.
constexpr int PLAYER_AI_HIDDEN_SURVIVE_ASSIGNEE_LIMIT = 2; // Лимит hidden assignments в survival-режиме.
constexpr int PLAYER_AI_HIDDEN_DEFENSIVE_ASSIGNEE_LIMIT = 4; // Лимит hidden assignments в defend/hold режимах.
constexpr int PLAYER_AI_SIEGE_HIDDEN_ASSIGNEE_LIMIT = 5; // Лимит hidden assignments при siege-room стратегии.
constexpr int PLAYER_AI_DANGEROUS_VISIBLE_ASSIGNEE_LIMIT = 5; // Минимальный лимит назначений на опасную видимую цель.
constexpr int PLAYER_AI_WOUNDED_ASSIGNEE_LIMIT = 6; // Минимальный лимит назначений на раненую цель для добивания.
constexpr int PLAYER_AI_ASSIGNMENT_SWITCH_MARGIN = 90; // Новый target должен быть заметно лучше, чтобы ломать commitment в том же ходу.

bool playerAIRoomActsOpen(const BattleRoomInfo &room)
{
	const bool hasControlledEntry = !room.entryPositions.empty() || room.doorCount + room.windowCount > 0;
	return room.isOutside
		|| room.isHall
		|| room.tileCount > PLAYER_AI_OPEN_ROOM_TILE_LIMIT
		|| (!hasControlledEntry && room.openingCount > room.tileCount * PLAYER_AI_ROOM_CONTROLLED_OPENING_LIMIT)
		|| (room.entryPositions.empty() && room.doorCount + room.windowCount == 0);
}

}

PlayerFactionPlanner::PlayerFactionPlanner(SavedBattleGame *save, const FactionAI *factionAI) :
	_save(save), _factionAI(factionAI)
{
	_plan.turn = -1;
	_plan.cycle = 0;
	_plan.strategy = PFS_HOLD_REACTION;
	_plan.activeHostiles = 0;
	_plan.visibleContacts = 0;
	_plan.hiddenContacts = 0;
	_plan.openAreaContacts = 0;
	_plan.roomContacts = 0;
	_plan.woundedAllies = 0;
	_plan.exposedAllies = 0;
}

const char *PlayerFactionPlanner::getStrategyName() const
{
	switch (_plan.strategy)
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

BattleUnit *PlayerFactionPlanner::getAssignedTarget(BattleUnit *unit) const
{
	if (!unit)
	{
		return 0;
	}
	auto it = _plan.assignedTargetByUnitId.find(unit->getId());
	if (it == _plan.assignedTargetByUnitId.end())
	{
		return 0;
	}
	return it->second;
}

std::string PlayerFactionPlanner::getAssignmentReason(BattleUnit *unit) const
{
	if (!unit)
	{
		return std::string();
	}
	auto it = _plan.assignmentReasonByUnitId.find(unit->getId());
	if (it == _plan.assignmentReasonByUnitId.end())
	{
		return std::string();
	}
	return it->second;
}

bool PlayerFactionPlanner::getBestEnemyContactPosition(Position *position) const
{
	return getBestEnemyContactPosition(position, 0, 0);
}

bool PlayerFactionPlanner::getBestEnemyContactPosition(Position *position, const BattleRoomInfo **roomInfo, int *enemiesInRoom, bool *visibleContact) const
{
	if (!position || _plan.enemies.empty())
	{
		return false;
	}
	const PlayerFactionEnemyContact *bestContact = 0;
	int bestScore = PLAYER_AI_REJECT_SCORE;
	for (const auto &contact : _plan.enemies)
	{
		const int score = contact.threatScore + contact.focusScore;
		if (score > bestScore)
		{
			bestScore = score;
			bestContact = &contact;
		}
	}
	if (!bestContact || !bestContact->enemy)
	{
		return false;
	}
	*position = bestContact->enemy->getPosition();
	if (roomInfo)
	{
		*roomInfo = _factionAI->getRoomInfoAt(bestContact->enemy->getPosition());
	}
	if (enemiesInRoom)
	{
		*enemiesInRoom = bestContact->enemiesInRoom;
	}
	if (visibleContact)
	{
		*visibleContact = bestContact->visibleContact;
	}
	return true;
}

bool PlayerFactionPlanner::canSeeEnemy(BattleUnit *actor, BattleUnit *enemy) const
{
	return actor && enemy && enemy->getTile() && _save->getTileEngine()->visible(actor, enemy->getTile());
}

bool PlayerFactionPlanner::canShootEnemy(BattleUnit *actor, BattleUnit *enemy) const
{
	if (!actor || !enemy || !enemy->getTile())
	{
		return false;
	}
	BattleItem *weapon = actor->getMainHandWeapon(false);
	if (!weapon || !_save->canUseWeapon(weapon, actor, false, BA_NONE))
	{
		return false;
	}
	BattleAction action;
	action.actor = actor;
	action.weapon = weapon;
	action.target = enemy->getPosition();
	Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
	Position target;
	return _save->getTileEngine()->canTargetUnit(&origin, enemy->getTile(), &target, actor, false, enemy);
}

int PlayerFactionPlanner::scoreEnemyThreat(BattleUnit *enemy) const
{
	if (!enemy)
	{
		return 0;
	}
	int score = PLAYER_AI_BASE_ENEMY_THREAT;
	score += std::max(0, enemy->getHealth());
	score += std::max(0, enemy->getTimeUnits()) / 2;
	if (enemy->getMainHandWeapon(false))
	{
		score += PLAYER_AI_ARMED_THREAT_BONUS;
	}
	if (enemy->getUtilityWeapon(BT_MELEE))
	{
		score += PLAYER_AI_MELEE_THREAT_BONUS;
	}
	return score;
}

int PlayerFactionPlanner::scoreAssignment(BattleUnit *actor, const PlayerFactionEnemyContact &contact, int assignedCount) const
{
	if (!actor || !contact.enemy)
	{
		return PLAYER_AI_REJECT_SCORE;
	}
	const int distance = Position::distance2d(actor->getPosition(), contact.enemy->getPosition());
	int score = contact.threatScore + contact.focusScore;
	score -= distance * PLAYER_AI_ASSIGNMENT_DISTANCE_PENALTY;
	BattleItem *weapon = actor->getMainHandWeapon(false);
	if (weapon && weapon->getRules())
	{
		const RuleItem *rule = weapon->getRules();
		const UnitStats *stats = actor->getBaseStats();
		const int bestAccuracy = std::max(std::max(rule->getAccuracySnap(), rule->getAccuracyAimed()), rule->getAccuracyAuto());
		const int expectedPressure = std::max(0, rule->getPower()) + bestAccuracy * std::max(PLAYER_AI_MIN_PRESSURE_ACCURACY, (int)stats->firing) / PLAYER_AI_PRESSURE_PERCENT;
		score += expectedPressure / 2;
		if (rule->getBattleType() == BT_MELEE)
		{
			score += distance <= PLAYER_AI_MELEE_CLOSE_RANGE ? PLAYER_AI_MELEE_CLOSE_ASSIGNMENT_BONUS : -PLAYER_AI_MELEE_FAR_ASSIGNMENT_PENALTY;
		}
	}
	if (std::find(contact.canShootBy.begin(), contact.canShootBy.end(), actor) != contact.canShootBy.end())
	{
		score += PLAYER_AI_CAN_SHOOT_ASSIGNMENT_BONUS;
	}
	else if (std::find(contact.visibleBy.begin(), contact.visibleBy.end(), actor) != contact.visibleBy.end())
	{
		score += PLAYER_AI_CAN_SEE_ASSIGNMENT_BONUS;
	}
	else
	{
		score -= PLAYER_AI_UNSEEN_VISIBLE_CONTACT_PENALTY;
	}
	const int focusHealthLimit = std::max(PLAYER_AI_WOUNDED_HEALTH_LIMIT,
		(int)contact.enemy->getBaseStats()->health * 3 / 5);
	const bool wounded = contact.enemy->getHealth() > 0 && contact.enemy->getHealth() <= focusHealthLimit;
	score += wounded ? PLAYER_AI_WOUNDED_ASSIGNMENT_BONUS : 0;
	score -= assignedCount * (wounded ? PLAYER_AI_WOUNDED_OVERFOCUS_PENALTY : PLAYER_AI_NORMAL_OVERFOCUS_PENALTY);
	return score;
}

void PlayerFactionPlanner::build(BattleUnit *activeUnit) const
{
	const int buildTurn = _save->getTurn();
	const bool reuseTurnAssignments = _plan.turn == buildTurn;
	const std::map<int, BattleUnit*> previousAssignments = reuseTurnAssignments
		? _plan.assignedTargetByUnitId
		: std::map<int, BattleUnit*>();
	_plan.turn = buildTurn;
	_plan.cycle++;
	_plan.allies.clear();
	_plan.enemies.clear();
	_plan.assignedTargetByUnitId.clear();
	_plan.assignmentReasonByUnitId.clear();
	_plan.activeHostiles = 0;
	_plan.visibleContacts = 0;
	_plan.hiddenContacts = 0;
	_plan.openAreaContacts = 0;
	_plan.roomContacts = 0;
	_plan.woundedAllies = 0;
	_plan.exposedAllies = 0;

	for (auto* unit : *_save->getUnits())
	{
		if (!unit || unit->isOut())
		{
			continue;
		}
		if (unit->getFaction() == FACTION_PLAYER)
		{
			_plan.allies.push_back(unit);
			if (unit->getHealth() < unit->getBaseStats()->health)
			{
				++_plan.woundedAllies;
			}
		}
	}

	std::vector<PlayerFactionEnemyContact> hiddenContacts;
	for (auto* enemy : *_save->getUnits())
	{
		if (!enemy || enemy->isOut() || enemy->getFaction() != FACTION_HOSTILE)
		{
			continue;
		}
		++_plan.activeHostiles;
		PlayerFactionEnemyContact contact;
		contact.enemy = enemy;
		contact.threatScore = scoreEnemyThreat(enemy);
		contact.focusScore = 0;
		contact.roomId = _factionAI->getRoomIdAt(enemy->getPosition());
		contact.roomSize = 0;
		contact.roomDoors = 0;
		contact.roomWindows = 0;
		contact.roomOpenings = 0;
		contact.roomEntries = 0;
		contact.roomOutside = false;
		contact.roomHall = false;
		contact.enemiesInRoom = 1;
		contact.visibleContact = false;
		contact.threatensAllies = 0;
		contact.canShootAllies = 0;
		if (const BattleRoomInfo *room = _factionAI->getRoomInfoAt(enemy->getPosition()))
		{
			contact.roomSize = room->tileCount;
			contact.roomDoors = room->doorCount;
			contact.roomWindows = room->windowCount;
			contact.roomOpenings = room->openingCount;
			contact.roomEntries = (int)room->entryPositions.size();
			contact.roomOutside = room->isOutside;
			contact.roomHall = room->isHall || (!room->isOutside && playerAIRoomActsOpen(*room));
		}
		for (auto* ally : _plan.allies)
		{
			if (canSeeEnemy(ally, enemy))
			{
				contact.visibleBy.push_back(ally);
				if (canShootEnemy(ally, enemy))
				{
					contact.canShootBy.push_back(ally);
				}
			}
			if (canSeeEnemy(enemy, ally))
			{
				++contact.threatensAllies;
				if (canShootEnemy(enemy, ally))
				{
					++contact.canShootAllies;
				}
			}
		}
		contact.visibleContact = !contact.visibleBy.empty();
		if (contact.visibleContact)
		{
			++_plan.visibleContacts;
			contact.focusScore = PLAYER_AI_VISIBLE_BY_FOCUS_BONUS * (int)contact.visibleBy.size() + PLAYER_AI_CAN_SHOOT_BY_FOCUS_BONUS * (int)contact.canShootBy.size();
			_plan.enemies.push_back(contact);
		}
		else
		{
			++_plan.hiddenContacts;
			int nearestAllyDist = INT_MAX;
			for (auto *ally : _plan.allies)
			{
				nearestAllyDist = std::min(nearestAllyDist, Position::distance2d(ally->getPosition(), enemy->getPosition()));
			}
			contact.focusScore = -PLAYER_AI_HIDDEN_FOCUS_BASE_PENALTY - nearestAllyDist * PLAYER_AI_HIDDEN_DISTANCE_PENALTY;
			contact.threatScore = contact.threatScore * PLAYER_AI_HIDDEN_THREAT_SCALE_NUM / PLAYER_AI_HIDDEN_THREAT_SCALE_DEN;
			hiddenContacts.push_back(contact);
		}
	}
	if (_plan.enemies.empty() && !hiddenContacts.empty())
	{
		std::sort(hiddenContacts.begin(), hiddenContacts.end(), [](const PlayerFactionEnemyContact &a, const PlayerFactionEnemyContact &b)
		{
			return a.threatScore + a.focusScore > b.threatScore + b.focusScore;
		});
		const int hiddenLimit = std::min(PLAYER_AI_HIDDEN_CONTACT_LIMIT, (int)hiddenContacts.size());
		for (int i = 0; i < hiddenLimit; ++i)
		{
			_plan.enemies.push_back(hiddenContacts[i]);
		}
	}

	std::map<int, int> enemiesByRoom;
	for (const auto &contact : _plan.enemies)
	{
		enemiesByRoom[contact.roomId]++;
	}
	for (auto &contact : _plan.enemies)
	{
		contact.enemiesInRoom = enemiesByRoom[contact.roomId];
		if (!contact.roomOutside && !contact.roomHall)
		{
			++_plan.roomContacts;
			contact.threatScore += PLAYER_AI_ROOM_CONTACT_THREAT_BONUS + std::max(0, contact.enemiesInRoom - 1) * PLAYER_AI_ROOM_EXTRA_ENEMY_THREAT_BONUS;
			if (contact.roomDoors + contact.roomWindows <= PLAYER_AI_ROOM_CONTROLLED_OPENING_LIMIT)
			{
				contact.threatScore += PLAYER_AI_ROOM_CONTACT_THREAT_BONUS;
			}
		}
		else if (contact.roomHall)
		{
			++_plan.openAreaContacts;
			contact.threatScore += std::max(0, contact.enemiesInRoom - 1) * PLAYER_AI_HALL_EXTRA_ENEMY_THREAT_BONUS;
		}
		else
		{
			++_plan.openAreaContacts;
		}
	}

	for (auto *ally : _plan.allies)
	{
		for (auto *enemy : *_save->getUnits())
		{
			if (!enemy || enemy->isOut() || enemy->getFaction() != FACTION_HOSTILE)
			{
				continue;
			}
			if (canSeeEnemy(enemy, ally))
			{
				++_plan.exposedAllies;
				break;
			}
		}
	}

	const int activeAllies = (int)_plan.allies.size();
	const bool mostlyHidden = _plan.visibleContacts == 0 && _plan.hiddenContacts > 0;
	const bool manyEnemies = _plan.activeHostiles >= std::max(PLAYER_AI_MANY_ENEMIES_MIN, activeAllies / 2);
	const bool outnumbered = activeAllies > 0 && _plan.activeHostiles >= activeAllies + PLAYER_AI_OUTNUMBERED_MARGIN;
	const bool openContacts = _plan.openAreaContacts > 0 || (_plan.enemies.empty() && _plan.hiddenContacts > 0);
	const bool roomProblem = _plan.roomContacts > 0 && _plan.openAreaContacts == 0;
	const bool dangerousRoomProblem = _plan.roomContacts > 0 && _plan.roomContacts >= _plan.openAreaContacts;
	const bool squadBadlyExposed = _plan.exposedAllies >= std::max(PLAYER_AI_BADLY_EXPOSED_MIN_ALLIES, activeAllies / PLAYER_AI_BADLY_EXPOSED_DIVISOR);
	const bool squadWoundedUnderContact = _plan.woundedAllies > 0 && _plan.exposedAllies > 0;
	const bool lastEnemies = _plan.activeHostiles <= PLAYER_AI_LAST_ENEMY_LIMIT;
	const bool lateHunt = _save->getTurn() >= PLAYER_AI_LATE_HUNT_TURN && activeAllies > 0
		&& _plan.activeHostiles <= std::min(PLAYER_AI_HIDDEN_CONTACT_LIMIT, activeAllies + 1);
	const bool smallForceHunt = mostlyHidden
		&& _save->getTurn() >= PLAYER_AI_SMALL_FORCE_HUNT_TURN
		&& _plan.activeHostiles <= PLAYER_AI_SMALL_FORCE_HUNT_LIMIT
		&& _plan.activeHostiles <= activeAllies + (_plan.activeHostiles <= 4 ? 1 : 0);
	const bool loneGuerrilla = mostlyHidden
		&& _save->getTurn() >= PLAYER_AI_LONE_GUERRILLA_TURN
		&& activeAllies > 0 && activeAllies <= 2
		&& _plan.activeHostiles <= PLAYER_AI_LONE_GUERRILLA_HOSTILE_LIMIT
		&& _plan.activeHostiles <= activeAllies + 1;
	const bool earlyHiddenPressure = _save->getTurn() <= PLAYER_AI_EARLY_PRESSURE_TURN_LIMIT && mostlyHidden && activeAllies >= PLAYER_AI_EARLY_PRESSURE_MIN_ALLIES && _plan.activeHostiles > PLAYER_AI_LAST_ENEMY_LIMIT;
	const bool realSurvivalPressure = activeAllies <= PLAYER_AI_SURVIVAL_ALLY_LIMIT
		|| (outnumbered && (_plan.visibleContacts > 0 || _plan.exposedAllies > 0 || _plan.woundedAllies >= std::max(PLAYER_AI_BADLY_EXPOSED_MIN_ALLIES, activeAllies / PLAYER_AI_SURVIVAL_WOUNDED_DIVISOR)))
		|| (_save->getTurn() > PLAYER_AI_LATE_SURVIVAL_TURN && outnumbered && _plan.activeHostiles >= activeAllies + PLAYER_AI_LATE_SURVIVAL_HOSTILE_MARGIN);
	const bool enableInitialDeployMode = true;
	if (enableInitialDeployMode
		&& _save->getTurn() <= PLAYER_AI_EARLY_PRESSURE_TURN_LIMIT
		&& activeAllies >= PLAYER_AI_EARLY_PRESSURE_MIN_ALLIES
		&& _plan.activeHostiles > PLAYER_AI_LAST_ENEMY_LIMIT
		&& _plan.activeHostiles <= activeAllies + PLAYER_AI_OUTNUMBERED_MARGIN
		&& _plan.hiddenContacts > 0
		&& _plan.visibleContacts == 0
		&& _plan.openAreaContacts > 0
		&& _plan.exposedAllies <= std::max(PLAYER_AI_BADLY_EXPOSED_MIN_ALLIES, activeAllies / PLAYER_AI_BADLY_EXPOSED_DIVISOR))
	{
		_plan.strategy = PFS_INITIAL_DEPLOY;
	}
	else if (loneGuerrilla)
	{
		_plan.strategy = PFS_SKIRMISH;
	}
	else if (lastEnemies || smallForceHunt || lateHunt)
	{
		_plan.strategy = PFS_HUNT_LAST_ENEMY;
	}
	else if (squadBadlyExposed && (outnumbered || squadWoundedUnderContact) && _plan.visibleContacts <= PLAYER_AI_SMALL_VISIBLE_CONTACT_LIMIT)
	{
		_plan.strategy = PFS_RETREAT_REGROUP;
	}
	else if (realSurvivalPressure)
	{
		const bool criticalMinority = activeAllies > PLAYER_AI_SURVIVAL_ALLY_LIMIT
			&& _plan.activeHostiles >= activeAllies + PLAYER_AI_LATE_SURVIVAL_HOSTILE_MARGIN;
		_plan.strategy = criticalMinority ? PFS_SKIRMISH : PFS_SURVIVE;
	}
	else if (dangerousRoomProblem && _plan.roomContacts >= PLAYER_AI_SIEGE_ROOM_CONTACT_LIMIT && (mostlyHidden || _plan.visibleContacts == 0))
	{
		_plan.strategy = PFS_SIEGE_ROOM;
	}
	else if (earlyHiddenPressure)
	{
		_plan.strategy = PFS_DEFEND_LINE;
	}
	else if (mostlyHidden && manyEnemies && openContacts)
	{
		_plan.strategy = PFS_DEFEND_LINE;
	}
	else if (roomProblem && _plan.visibleContacts == 0)
	{
		_plan.strategy = PFS_SIEGE_ROOM;
	}
	else if (_plan.visibleContacts > 0 && _plan.visibleContacts <= PLAYER_AI_SMALL_VISIBLE_CONTACT_LIMIT && _plan.activeHostiles > PLAYER_AI_HOLD_REACTION_HOSTILE_LIMIT)
	{
		_plan.strategy = PFS_HOLD_REACTION;
	}
	else if (_plan.visibleContacts > 0)
	{
		_plan.strategy = PFS_ASSAULT;
	}
	else
	{
		_plan.strategy = PFS_HOLD_REACTION;
	}

	std::map<int, int> assignedCountByEnemyId;
	auto maxAssigneesFor = [&](const PlayerFactionEnemyContact &contact) -> int
	{
		int maxAssignees = contact.visibleContact
			? std::max(PLAYER_AI_VISIBLE_MIN_ASSIGNEES, (int)contact.canShootBy.size() + std::max(0, (int)contact.visibleBy.size() - (int)contact.canShootBy.size()) / PLAYER_AI_VISIBLE_EXTRA_ASSIGNEE_DIVISOR)
			: ((_plan.enemies.size() == 1 || _save->getTurn() >= PLAYER_AI_HIDDEN_ASSIGN_ALL_TURN)
				? (int)_plan.allies.size()
				: std::min((int)_plan.allies.size(), (contact.canShootBy.empty() && contact.visibleBy.empty()) ? PLAYER_AI_HIDDEN_BLIND_ASSIGNEE_LIMIT : PLAYER_AI_HIDDEN_SEEN_ASSIGNEE_LIMIT));
		if (!contact.visibleContact && (_plan.strategy == PFS_DEFEND_LINE || _plan.strategy == PFS_HOLD_REACTION || _plan.strategy == PFS_SURVIVE || _plan.strategy == PFS_SKIRMISH))
		{
			maxAssignees = std::min(maxAssignees, _plan.strategy == PFS_SURVIVE ? PLAYER_AI_HIDDEN_SURVIVE_ASSIGNEE_LIMIT : PLAYER_AI_HIDDEN_DEFENSIVE_ASSIGNEE_LIMIT);
		}
		else if (!contact.visibleContact && _plan.strategy == PFS_SIEGE_ROOM)
		{
			maxAssignees = std::min(maxAssignees, PLAYER_AI_SIEGE_HIDDEN_ASSIGNEE_LIMIT);
		}
		if (contact.visibleContact && (contact.threatScore >= PLAYER_AI_DANGEROUS_VISIBLE_THREAT || contact.canShootBy.size() >= PLAYER_AI_ROOM_CONTROLLED_OPENING_LIMIT))
		{
			maxAssignees = std::max(maxAssignees, PLAYER_AI_DANGEROUS_VISIBLE_ASSIGNEE_LIMIT);
		}
		const int focusHealthLimit = std::max(PLAYER_AI_WOUNDED_HEALTH_LIMIT,
			(int)contact.enemy->getBaseStats()->health * 3 / 5);
		if (contact.enemy->getHealth() > 0 && contact.enemy->getHealth() <= focusHealthLimit)
		{
			maxAssignees = std::max(maxAssignees, PLAYER_AI_WOUNDED_ASSIGNEE_LIMIT);
		}
		return maxAssignees;
	};
	for (auto* ally : _plan.allies)
	{
		PlayerFactionEnemyContact *bestContact = 0;
		int bestScore = PLAYER_AI_REJECT_SCORE;
		bool stickyAssignment = false;
		for (auto &contact : _plan.enemies)
		{
			const bool canShoot = std::find(contact.canShootBy.begin(), contact.canShootBy.end(), ally) != contact.canShootBy.end();
			const bool canSee = std::find(contact.visibleBy.begin(), contact.visibleBy.end(), ally) != contact.visibleBy.end();
			const bool lastEnemyFactionSupport = contact.visibleContact
				&& _plan.strategy == PFS_HUNT_LAST_ENEMY
				&& _plan.activeHostiles <= 2;
			if (!canShoot && !canSee)
			{
				if (contact.visibleContact && !lastEnemyFactionSupport)
				{
					continue;
				}
			}
			const int assignedCount = assignedCountByEnemyId[contact.enemy->getId()];
			const int maxAssignees = maxAssigneesFor(contact);
			if (assignedCount >= maxAssignees && !canShoot)
			{
				continue;
			}
			const int score = scoreAssignment(ally, contact, assignedCount);
			if (score > bestScore)
			{
				bestScore = score;
				bestContact = &contact;
			}
		}
		if (reuseTurnAssignments)
		{
			auto previous = previousAssignments.find(ally->getId());
			PlayerFactionEnemyContact *previousContact = 0;
			if (previous != previousAssignments.end() && previous->second && !previous->second->isOut())
			{
				for (auto &contact : _plan.enemies)
				{
					if (contact.enemy == previous->second)
					{
						previousContact = &contact;
						break;
					}
				}
			}
			if (previousContact)
			{
				const bool previousCanShoot = std::find(previousContact->canShootBy.begin(), previousContact->canShootBy.end(), ally) != previousContact->canShootBy.end();
				const bool previousCanSee = std::find(previousContact->visibleBy.begin(), previousContact->visibleBy.end(), ally) != previousContact->visibleBy.end();
				const bool urgentVisibleSwitch = bestContact && bestContact != previousContact
					&& bestContact->visibleContact && !previousContact->visibleContact;
				const bool urgentThreatSwitch = bestContact && bestContact != previousContact
					&& bestContact->canShootAllies > previousContact->canShootAllies + 1;
				const int previousAssignedCount = assignedCountByEnemyId[previousContact->enemy->getId()];
				const int previousScore = scoreAssignment(ally, *previousContact, previousAssignedCount);
				const bool previousWithinCap = previousCanShoot || previousAssignedCount < maxAssigneesFor(*previousContact);
				const bool previousFactionSupport = previousContact->visibleContact
					&& _plan.strategy == PFS_HUNT_LAST_ENEMY && _plan.activeHostiles <= 2;
				const bool previousStillActionable = previousWithinCap
					&& (previousCanShoot || previousCanSee || !previousContact->visibleContact || previousFactionSupport);
				if (previousStillActionable && !urgentVisibleSwitch && !urgentThreatSwitch
					&& (bestContact == previousContact || previousScore + PLAYER_AI_ASSIGNMENT_SWITCH_MARGIN >= bestScore))
				{
					bestContact = previousContact;
					bestScore = previousScore;
					stickyAssignment = true;
				}
			}
		}
		if (bestContact)
		{
			_plan.assignedTargetByUnitId[ally->getId()] = bestContact->enemy;
			assignedCountByEnemyId[bestContact->enemy->getId()]++;
			std::ostringstream reason;
			reason << "score=" << bestScore
				<< ", sticky=" << stickyAssignment
				<< ", threat=" << bestContact->threatScore
				<< ", focus=" << bestContact->focusScore
				<< ", room=" << bestContact->roomId
				<< ", roomEnemies=" << bestContact->enemiesInRoom
				<< ", roomSize=" << bestContact->roomSize
				<< ", doors=" << bestContact->roomDoors
				<< ", windows=" << bestContact->roomWindows
				<< ", entries=" << bestContact->roomEntries
				<< ", outside=" << bestContact->roomOutside
				<< ", hall=" << bestContact->roomHall
				<< ", visibleBy=" << bestContact->visibleBy.size()
				<< ", canShootBy=" << bestContact->canShootBy.size()
				<< ", threatensAllies=" << bestContact->threatensAllies
				<< ", canShootAllies=" << bestContact->canShootAllies
				<< ", visibleContact=" << bestContact->visibleContact
				<< ", assignedCount=" << assignedCountByEnemyId[bestContact->enemy->getId()];
			_plan.assignmentReasonByUnitId[ally->getId()] = reason.str();
		}
	}

	if (activeUnit)
	{
		logPlan();
	}
}

void PlayerFactionPlanner::logPlan() const
{
	std::ostringstream summary;
	summary << "Player faction plan: turn=" << _plan.turn
		<< ", cycle=" << _plan.cycle
		<< ", strategy=" << getStrategyName()
		<< ", allies=" << _plan.allies.size()
		<< ", activeHostiles=" << _plan.activeHostiles
		<< ", planEnemies=" << _plan.enemies.size()
		<< ", visibleContacts=" << _plan.visibleContacts
		<< ", hiddenContacts=" << _plan.hiddenContacts
		<< ", openContacts=" << _plan.openAreaContacts
		<< ", roomContacts=" << _plan.roomContacts
		<< ", woundedAllies=" << _plan.woundedAllies
		<< ", exposedAllies=" << _plan.exposedAllies;
	_save->appendToAutoBattleLog(summary.str());

	for (const auto &contact : _plan.enemies)
	{
		std::ostringstream line;
		line << "Player enemy contact: enemy=" << contact.enemy->getId()
			<< ", pos=" << contact.enemy->getPosition()
			<< ", threat=" << contact.threatScore
			<< ", focus=" << contact.focusScore
			<< ", room=" << contact.roomId
			<< ", roomEnemies=" << contact.enemiesInRoom
			<< ", roomSize=" << contact.roomSize
			<< ", doors=" << contact.roomDoors
			<< ", windows=" << contact.roomWindows
			<< ", openings=" << contact.roomOpenings
			<< ", entries=" << contact.roomEntries
			<< ", outside=" << contact.roomOutside
			<< ", hall=" << contact.roomHall
			<< ", visibleContact=" << contact.visibleContact
			<< ", visibleBy=" << contact.visibleBy.size()
			<< ", canShootBy=" << contact.canShootBy.size()
			<< ", threatensAllies=" << contact.threatensAllies
			<< ", canShootAllies=" << contact.canShootAllies;
		_save->appendToAutoBattleLog(line.str());
	}
	for (auto* ally : _plan.allies)
	{
		BattleUnit *target = getAssignedTarget(ally);
		std::ostringstream line;
		line << "Player unit assignment: unit=" << ally->getId();
		if (target)
		{
			line << ", target=" << target->getId()
				<< ", targetPos=" << target->getPosition()
				<< ", reason=" << getAssignmentReason(ally);
		}
		else
		{
			line << ", target=none";
		}
		_save->appendToAutoBattleLog(line.str());
	}
}

}
