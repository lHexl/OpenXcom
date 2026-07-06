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
#include <string>
#include <vector>

namespace OpenXcom
{

class AIModule;
class PlayerFactionPlanner;
class SavedBattleGame;
struct BattleAction;
enum PlayerFactionStrategy : int;

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
	mutable PlayerFactionPlanner *_playerPlanner;
	mutable unsigned long long _roomCacheSignature;
	mutable std::vector<int> _roomIdByTile;
	mutable std::vector<BattleRoomInfo> _roomInfos;

	PlayerFactionPlanner *getPlayerPlanner() const;
	unsigned long long calculateRoomCacheSignature() const;
	void ensureRoomCache() const;
	void rebuildRoomCache(unsigned long long signature) const;
	void writeBattleMapLog() const;
	int getRoomId(Position pos) const;
	const BattleRoomInfo *getRoomInfo(int roomId) const;

public:
	FactionAI(SavedBattleGame *save, UnitFaction faction);
	AIModule *getUnitModule(BattleUnit *unit) const;
	void think(BattleUnit *unit, BattleAction *action) const;
	void setWeaponPickedUp(BattleUnit *unit) const;
	BattleUnit *getAssignedTarget(BattleUnit *unit) const;
	std::string getAssignmentReason(BattleUnit *unit) const;
	PlayerFactionStrategy getPlayerStrategy() const;
	const char *getPlayerStrategyName() const;
	int getEnemyContactCount() const;
	bool getBestEnemyContactPosition(Position *position) const;
	bool getBestEnemyContactPosition(Position *position, const BattleRoomInfo **roomInfo, int *enemiesInRoom, bool *visibleContact = 0) const;
	int getRoomIdAt(Position pos) const { return getRoomId(pos); }
	const BattleRoomInfo *getRoomInfoAt(Position pos) const { return getRoomInfo(getRoomId(pos)); }
	const std::vector<BattleRoomInfo> &getKnownRooms() const { ensureRoomCache(); return _roomInfos; }
	UnitFaction getFaction() const { return _faction; }
};

}
