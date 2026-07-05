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
#include "../Savegame/BattleUnit.h"
#include <map>
#include <string>
#include <vector>

namespace OpenXcom
{

class AIModule;
class SavedBattleGame;
struct BattleAction;

struct PlayerFactionEnemyContact
{
	BattleUnit *enemy;
	std::vector<BattleUnit*> visibleBy;
	std::vector<BattleUnit*> canShootBy;
	int threatScore;
	int focusScore;
};

struct PlayerFactionPlan
{
	int turn;
	int cycle;
	std::vector<BattleUnit*> allies;
	std::vector<PlayerFactionEnemyContact> enemies;
	std::map<int, BattleUnit*> assignedTargetByUnitId;
	std::map<int, std::string> assignmentReasonByUnitId;
};

/**
 * Faction-level AI coordinator.
 *
 * All battlescape AI calls pass through this object so faction-wide strategy
 * can coordinate per-unit modules.
 */
class FactionAI
{
private:
	SavedBattleGame *_save;
	UnitFaction _faction;
	mutable PlayerFactionPlan _playerPlan;

	void buildPlayerPlan(BattleUnit *activeUnit) const;
	bool canSeeEnemy(BattleUnit *actor, BattleUnit *enemy) const;
	bool canShootEnemy(BattleUnit *actor, BattleUnit *enemy) const;
	int scoreEnemyThreat(BattleUnit *enemy) const;
	int scoreAssignment(BattleUnit *actor, const PlayerFactionEnemyContact &contact, int assignedCount) const;
	void logPlayerPlan() const;

public:
	FactionAI(SavedBattleGame *save, UnitFaction faction);
	AIModule *getUnitModule(BattleUnit *unit) const;
	void think(BattleUnit *unit, BattleAction *action) const;
	void setWeaponPickedUp(BattleUnit *unit) const;
	BattleUnit *getAssignedTarget(BattleUnit *unit) const;
	std::string getAssignmentReason(BattleUnit *unit) const;
	int getEnemyContactCount() const;
	bool getBestEnemyContactPosition(Position *position) const;
	UnitFaction getFaction() const { return _faction; }
};

}
