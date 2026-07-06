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

struct BattleRoomInfo
{
	int id = -1;
	int tileCount = 0;
	int doorCount = 0;
	int windowCount = 0;
	int openingCount = 0;
	bool touchesMapEdge = false;
	bool isOutside = false;
	bool isHall = false;
	std::vector<Position> entryPositions;
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
	bool roomOutside;
	bool roomHall;
	int enemiesInRoom;
	bool visibleContact;
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
	mutable unsigned long long _roomCacheSignature;
	mutable std::vector<int> _roomIdByTile;
	mutable std::vector<BattleRoomInfo> _roomInfos;

	void buildPlayerPlan(BattleUnit *activeUnit) const;
	unsigned long long calculateRoomCacheSignature() const;
	void ensureRoomCache() const;
	void rebuildRoomCache(unsigned long long signature) const;
	void writeBattleMapLog() const;
	int getRoomId(Position pos) const;
	const BattleRoomInfo *getRoomInfo(int roomId) const;
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
	bool getBestEnemyContactPosition(Position *position, const BattleRoomInfo **roomInfo, int *enemiesInRoom, bool *visibleContact = 0) const;
	int getRoomIdAt(Position pos) const { return getRoomId(pos); }
	const BattleRoomInfo *getRoomInfoAt(Position pos) const { return getRoomInfo(getRoomId(pos)); }
	const std::vector<BattleRoomInfo> &getKnownRooms() const { ensureRoomCache(); return _roomInfos; }
	UnitFaction getFaction() const { return _faction; }
};

}
