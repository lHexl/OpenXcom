#pragma once
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
#include <map>
#include <string>
#include <vector>

namespace OpenXcom
{

enum PlayerFactionStrategy : int
{
	PFS_INITIAL_DEPLOY,
	PFS_DEFEND_LINE,
	PFS_SIEGE_ROOM,
	PFS_HUNT_LAST_ENEMY,
	PFS_SURVIVE,
	PFS_RETREAT_REGROUP,
	PFS_ASSAULT,
	PFS_HOLD_REACTION
};

struct PlayerFactionEnemyContact
{
	BattleUnit *enemy;
	std::vector<BattleUnit*> visibleBy;
	std::vector<BattleUnit*> canShootBy;
	int threatScore;
	int focusScore;
	int roomId;
	int roomSize;
	int roomDoors;
	int roomWindows;
	int roomOpenings;
	int roomEntries;
	bool roomOutside;
	bool roomHall;
	int enemiesInRoom;
	bool visibleContact;
};

struct PlayerFactionPlan
{
	int turn;
	int cycle;
	PlayerFactionStrategy strategy;
	int activeHostiles;
	int visibleContacts;
	int hiddenContacts;
	int openAreaContacts;
	int roomContacts;
	int woundedAllies;
	int exposedAllies;
	std::vector<BattleUnit*> allies;
	std::vector<PlayerFactionEnemyContact> enemies;
	std::map<int, BattleUnit*> assignedTargetByUnitId;
	std::map<int, std::string> assignmentReasonByUnitId;
};

class SavedBattleGame;

class PlayerFactionPlanner
{
private:
	SavedBattleGame *_save;
	const FactionAI *_factionAI;
	mutable PlayerFactionPlan _plan;

	bool canSeeEnemy(BattleUnit *actor, BattleUnit *enemy) const;
	bool canShootEnemy(BattleUnit *actor, BattleUnit *enemy) const;
	int scoreEnemyThreat(BattleUnit *enemy) const;
	int scoreAssignment(BattleUnit *actor, const PlayerFactionEnemyContact &contact, int assignedCount) const;
	void logPlan() const;

public:
	PlayerFactionPlanner(SavedBattleGame *save, const FactionAI *factionAI);
	void build(BattleUnit *activeUnit) const;
	BattleUnit *getAssignedTarget(BattleUnit *unit) const;
	std::string getAssignmentReason(BattleUnit *unit) const;
	PlayerFactionStrategy getStrategy() const { return _plan.strategy; }
	const char *getStrategyName() const;
	int getEnemyContactCount() const { return (int)_plan.enemies.size(); }
	bool getBestEnemyContactPosition(Position *position) const;
	bool getBestEnemyContactPosition(Position *position, const BattleRoomInfo **roomInfo, int *enemiesInRoom, bool *visibleContact = 0) const;
};

}
