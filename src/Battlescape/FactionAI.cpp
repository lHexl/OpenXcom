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
#include "FactionAI.h"
#include "AIModule.h"
#include "BattlescapeGame.h"
#include "HostileFactionAI.h"
#include "NeutralFactionAI.h"
#include "PlayerFactionAI.h"
#include "TileEngine.h"
#include "../Engine/Options.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/SavedBattleGame.h"
#include <algorithm>
#include <sstream>

namespace OpenXcom
{

FactionAI::FactionAI(SavedBattleGame *save, UnitFaction faction) : _save(save), _faction(faction)
{
	_playerPlan.turn = -1;
	_playerPlan.cycle = 0;
}

AIModule *FactionAI::getUnitModule(BattleUnit *unit) const
{
	if (!unit)
	{
		return 0;
	}
	AIModule *ai = unit->getAIModule();
	if (_faction == FACTION_PLAYER)
	{
		if (!dynamic_cast<PlayerFactionAI*>(ai))
		{
			unit->setAIModule(new PlayerFactionAI(_save, unit, 0));
		}
		PlayerFactionAI *playerAI = dynamic_cast<PlayerFactionAI*>(unit->getAIModule());
		if (playerAI)
		{
			playerAI->setFactionAI(this);
		}
	}
	else if (_faction == FACTION_NEUTRAL)
	{
		if (!dynamic_cast<NeutralFactionAI*>(ai))
		{
			unit->setAIModule(new NeutralFactionAI(_save, unit, 0));
		}
	}
	else if (!dynamic_cast<HostileFactionAI*>(ai))
	{
		unit->setAIModule(new HostileFactionAI(_save, unit, 0));
	}
	return unit->getAIModule();
}

void FactionAI::think(BattleUnit *unit, BattleAction *action) const
{
	if (!unit)
	{
		return;
	}
	getUnitModule(unit);
	if (_faction == FACTION_PLAYER)
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "FactionAI: begin planning for unit=" << unit->getId();
			_save->appendToAutoBattleLog(log.str());
		}
		buildPlayerPlan(unit);
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "FactionAI: planning done for unit=" << unit->getId()
				<< ", assignedTarget=";
			BattleUnit *assigned = getAssignedTarget(unit);
			if (assigned)
			{
				log << assigned->getId();
			}
			else
			{
				log << "none";
			}
			_save->appendToAutoBattleLog(log.str());
		}
	}
	unit->think(action);
	if (_faction == FACTION_PLAYER && Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "FactionAI: unit think done for unit=" << unit->getId()
			<< ", action=" << (int)action->type
			<< ", target=" << action->target;
		_save->appendToAutoBattleLog(log.str());
	}
}

void FactionAI::setWeaponPickedUp(BattleUnit *unit) const
{
	AIModule *ai = getUnitModule(unit);
	if (ai)
	{
		ai->setWeaponPickedUp();
	}
}

BattleUnit *FactionAI::getAssignedTarget(BattleUnit *unit) const
{
	if (!unit)
	{
		return 0;
	}
	auto it = _playerPlan.assignedTargetByUnitId.find(unit->getId());
	if (it == _playerPlan.assignedTargetByUnitId.end())
	{
		return 0;
	}
	return it->second;
}

std::string FactionAI::getAssignmentReason(BattleUnit *unit) const
{
	if (!unit)
	{
		return std::string();
	}
	auto it = _playerPlan.assignmentReasonByUnitId.find(unit->getId());
	if (it == _playerPlan.assignmentReasonByUnitId.end())
	{
		return std::string();
	}
	return it->second;
}

int FactionAI::getEnemyContactCount() const
{
	return (int)_playerPlan.enemies.size();
}

bool FactionAI::getBestEnemyContactPosition(Position *position) const
{
	if (!position || _playerPlan.enemies.empty())
	{
		return false;
	}
	const PlayerFactionEnemyContact *bestContact = 0;
	int bestScore = -100000;
	for (const auto &contact : _playerPlan.enemies)
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
	return true;
}

bool FactionAI::canSeeEnemy(BattleUnit *actor, BattleUnit *enemy) const
{
	return actor && enemy && enemy->getTile() && _save->getTileEngine()->visible(actor, enemy->getTile());
}

bool FactionAI::canShootEnemy(BattleUnit *actor, BattleUnit *enemy) const
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

int FactionAI::scoreEnemyThreat(BattleUnit *enemy) const
{
	if (!enemy)
	{
		return 0;
	}
	int score = 40;
	score += std::max(0, enemy->getHealth());
	score += std::max(0, enemy->getTimeUnits()) / 2;
	if (enemy->getMainHandWeapon(false))
	{
		score += 35;
	}
	if (enemy->getUtilityWeapon(BT_MELEE))
	{
		score += 20;
	}
	return score;
}

int FactionAI::scoreAssignment(BattleUnit *actor, const PlayerFactionEnemyContact &contact, int assignedCount) const
{
	if (!actor || !contact.enemy)
	{
		return -100000;
	}
	const int distance = Position::distance2d(actor->getPosition(), contact.enemy->getPosition());
	int score = contact.threatScore + contact.focusScore;
	score -= distance * 3;
	if (std::find(contact.canShootBy.begin(), contact.canShootBy.end(), actor) != contact.canShootBy.end())
	{
		score += 80;
	}
	else if (std::find(contact.visibleBy.begin(), contact.visibleBy.end(), actor) != contact.visibleBy.end())
	{
		score += 30;
	}
	else
	{
		score -= 80;
	}
	score -= assignedCount * 55;
	return score;
}

void FactionAI::buildPlayerPlan(BattleUnit *activeUnit) const
{
	_playerPlan.turn = _save->getTurn();
	_playerPlan.cycle++;
	_playerPlan.allies.clear();
	_playerPlan.enemies.clear();
	_playerPlan.assignedTargetByUnitId.clear();
	_playerPlan.assignmentReasonByUnitId.clear();

	for (auto* unit : *_save->getUnits())
	{
		if (!unit || unit->isOut())
		{
			continue;
		}
		if (unit->getFaction() == FACTION_PLAYER)
		{
			_playerPlan.allies.push_back(unit);
		}
	}

	for (auto* enemy : *_save->getUnits())
	{
		if (!enemy || enemy->isOut() || enemy->getFaction() != FACTION_HOSTILE)
		{
			continue;
		}
		PlayerFactionEnemyContact contact;
		contact.enemy = enemy;
		contact.threatScore = scoreEnemyThreat(enemy);
		contact.focusScore = 0;
		for (auto* ally : _playerPlan.allies)
		{
			if (canSeeEnemy(ally, enemy))
			{
				contact.visibleBy.push_back(ally);
				if (canShootEnemy(ally, enemy))
				{
					contact.canShootBy.push_back(ally);
				}
			}
		}
		if (!contact.visibleBy.empty())
		{
			contact.focusScore = 20 * (int)contact.visibleBy.size() + 35 * (int)contact.canShootBy.size();
			_playerPlan.enemies.push_back(contact);
		}
	}

	std::map<int, int> assignedCountByEnemyId;
	for (auto* ally : _playerPlan.allies)
	{
		PlayerFactionEnemyContact *bestContact = 0;
		int bestScore = -100000;
		for (auto &contact : _playerPlan.enemies)
		{
			const bool canShoot = std::find(contact.canShootBy.begin(), contact.canShootBy.end(), ally) != contact.canShootBy.end();
			const bool canSee = std::find(contact.visibleBy.begin(), contact.visibleBy.end(), ally) != contact.visibleBy.end();
			if (!canShoot && !canSee)
			{
				continue;
			}
			const int assignedCount = assignedCountByEnemyId[contact.enemy->getId()];
			const int maxAssignees = contact.enemy->getHealth() < 25 ? 1 : 2;
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
		if (bestContact)
		{
			_playerPlan.assignedTargetByUnitId[ally->getId()] = bestContact->enemy;
			assignedCountByEnemyId[bestContact->enemy->getId()]++;
			std::ostringstream reason;
			reason << "score=" << bestScore
				<< ", threat=" << bestContact->threatScore
				<< ", focus=" << bestContact->focusScore
				<< ", visibleBy=" << bestContact->visibleBy.size()
				<< ", canShootBy=" << bestContact->canShootBy.size()
				<< ", assignedCount=" << assignedCountByEnemyId[bestContact->enemy->getId()];
			_playerPlan.assignmentReasonByUnitId[ally->getId()] = reason.str();
		}
	}

	if (activeUnit)
	{
		logPlayerPlan();
	}
}

void FactionAI::logPlayerPlan() const
{
	std::ostringstream summary;
	summary << "Faction player plan: turn=" << _playerPlan.turn
		<< ", cycle=" << _playerPlan.cycle
		<< ", allies=" << _playerPlan.allies.size()
		<< ", visibleEnemies=" << _playerPlan.enemies.size();
	_save->appendToAutoBattleLog(summary.str());

	for (const auto &contact : _playerPlan.enemies)
	{
		std::ostringstream line;
		line << "Faction enemy contact: enemy=" << contact.enemy->getId()
			<< ", pos=" << contact.enemy->getPosition()
			<< ", threat=" << contact.threatScore
			<< ", focus=" << contact.focusScore
			<< ", visibleBy=" << contact.visibleBy.size()
			<< ", canShootBy=" << contact.canShootBy.size();
		_save->appendToAutoBattleLog(line.str());
	}
	for (auto* ally : _playerPlan.allies)
	{
		BattleUnit *target = getAssignedTarget(ally);
		std::ostringstream line;
		line << "Faction unit assignment: unit=" << ally->getId();
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
