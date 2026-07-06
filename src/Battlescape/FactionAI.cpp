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
#include "../Savegame/Tile.h"
#include "../Mod/MapData.h"
#include "Pathfinding.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <queue>
#include <sstream>

namespace OpenXcom
{

namespace
{

std::string factionAIJsonEscape(const std::string &value)
{
	std::ostringstream escaped;
	for (char c : value)
	{
		switch (c)
		{
		case '\\':
			escaped << "\\\\";
			break;
		case '"':
			escaped << "\\\"";
			break;
		case '\n':
			escaped << "\\n";
			break;
		case '\r':
			escaped << "\\r";
			break;
		case '\t':
			escaped << "\\t";
			break;
		default:
			escaped << c;
			break;
		}
	}
	return escaped.str();
}

}

FactionAI::FactionAI(SavedBattleGame *save, UnitFaction faction) : _save(save), _faction(faction), _roomCacheSignature(0)
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

unsigned long long FactionAI::calculateRoomCacheSignature() const
{
	unsigned long long signature = 1469598103934665603ull;
	signature ^= (unsigned long long)_save->getMapSizeXYZ();
	signature *= 1099511628211ull;
	for (int i = 0; i < _save->getMapSizeXYZ(); ++i)
	{
		const Tile *tile = _save->getTile(i);
		for (int part = O_FLOOR; part < O_MAX; ++part)
		{
			signature ^= (unsigned long long)(reinterpret_cast<std::uintptr_t>(tile->getMapData((TilePart)part)) + part * 104729);
			signature *= 1099511628211ull;
		}
	}
	return signature;
}

void FactionAI::ensureRoomCache() const
{
	const unsigned long long signature = calculateRoomCacheSignature();
	if (_roomIdByTile.empty() || _roomCacheSignature != signature)
	{
		rebuildRoomCache(signature);
	}
}

int FactionAI::getRoomId(Position pos) const
{
	ensureRoomCache();
	const Tile *tile = _save->getTile(pos);
	if (!tile)
	{
		return -1;
	}
	return _roomIdByTile[_save->getTileIndex(pos)];
}

const BattleRoomInfo *FactionAI::getRoomInfo(int roomId) const
{
	ensureRoomCache();
	if (roomId < 0 || roomId >= (int)_roomInfos.size())
	{
		return 0;
	}
	return &_roomInfos[roomId];
}

void FactionAI::rebuildRoomCache(unsigned long long signature) const
{
	_roomIdByTile.assign(_save->getMapSizeXYZ(), -1);
	_roomInfos.clear();

	auto isRoomTile = [&](const Tile *tile) -> bool
	{
		return tile && !tile->hasNoFloor(_save);
	};

	auto boundaryPart = [&](const Position &pos, int dir, Position *nextPos) -> TilePart
	{
		*nextPos = pos;
		switch (dir)
		{
		case 0: nextPos->y -= 1; return O_NORTHWALL;
		case 2: nextPos->x += 1; return O_WESTWALL;
		case 4: nextPos->y += 1; return O_NORTHWALL;
		case 6: nextPos->x -= 1; return O_WESTWALL;
		default: return O_OBJECT;
		}
	};

	auto boundaryTile = [&](const Position &pos, int dir) -> const Tile*
	{
		if (dir == 0 || dir == 6)
		{
			return _save->getTile(pos);
		}
		Position nextPos;
		boundaryPart(pos, dir, &nextPos);
		return _save->getTile(nextPos);
	};

	auto bigWallBlocks = [&](const Tile *tile, int dir) -> bool
	{
		const MapData *object = tile ? tile->getMapData(O_OBJECT) : 0;
		if (!object || !object->getBigWall())
		{
			return false;
		}
		int bigWall = object->getBigWall();
		if (bigWall == Pathfinding::BLOCK)
		{
			return true;
		}
		if (dir == 0)
			return bigWall == Pathfinding::BIGWALLNORTH || bigWall == Pathfinding::BIGWALLWESTANDNORTH;
		if (dir == 2)
			return bigWall == Pathfinding::BIGWALLEAST || bigWall == Pathfinding::BIGWALLEASTANDSOUTH;
		if (dir == 4)
			return bigWall == Pathfinding::BIGWALLSOUTH || bigWall == Pathfinding::BIGWALLEASTANDSOUTH;
		if (dir == 6)
			return bigWall == Pathfinding::BIGWALLWEST || bigWall == Pathfinding::BIGWALLWESTANDNORTH;
		return false;
	};

	auto classifyBoundary = [&](const Position &pos, int dir, bool *door, bool *window) -> bool
	{
		*door = false;
		*window = false;
		Position nextPos;
		TilePart part = boundaryPart(pos, dir, &nextPos);
		const Tile *tile = boundaryTile(pos, dir);
		const Tile *fromTile = _save->getTile(pos);
		const Tile *toTile = _save->getTile(nextPos);
		if (!tile || !fromTile || !toTile || !isRoomTile(toTile))
		{
			return true;
		}
		if (bigWallBlocks(fromTile, dir) || bigWallBlocks(toTile, (dir + 4) % 8))
		{
			return true;
		}
		if (fromTile->isDoor(O_OBJECT) || fromTile->isUfoDoor(O_OBJECT) || toTile->isDoor(O_OBJECT) || toTile->isUfoDoor(O_OBJECT))
		{
			*door = true;
			return true;
		}
		const MapData *wall = tile->getMapData(part);
		if (!wall)
		{
			return false;
		}
		*door = tile->isDoor(part) || tile->isUfoDoor(part);
		*window = !*door && wall->getBlock(DT_NONE) == 0;
		return true;
	};

	for (int i = 0; i < _save->getMapSizeXYZ(); ++i)
	{
		if (_roomIdByTile[i] != -1)
		{
			continue;
		}
		const Tile *start = _save->getTile(i);
		if (!isRoomTile(start))
		{
			continue;
		}

		BattleRoomInfo info;
		info.id = (int)_roomInfos.size();
		std::queue<Position> open;
		open.push(start->getPosition());
		_roomIdByTile[i] = info.id;

		auto addEntryPosition = [&](const Position &entryPos)
		{
			if (std::find(info.entryPositions.begin(), info.entryPositions.end(), entryPos) == info.entryPositions.end())
			{
				info.entryPositions.push_back(entryPos);
			}
		};

		while (!open.empty())
		{
			Position pos = open.front();
			open.pop();
			++info.tileCount;
			if (pos.x == 0 || pos.y == 0 || pos.x == _save->getMapSizeX() - 1 || pos.y == _save->getMapSizeY() - 1)
			{
				info.touchesMapEdge = true;
			}

			for (int dir = 0; dir < 8; dir += 2)
			{
				Position nextPos;
				boundaryPart(pos, dir, &nextPos);
				const Tile *nextTile = _save->getTile(nextPos);
				bool door = false;
				bool window = false;
				bool blocked = classifyBoundary(pos, dir, &door, &window);
				if (door)
				{
					++info.doorCount;
					if (nextTile && isRoomTile(nextTile))
					{
						addEntryPosition(nextPos);
					}
				}
				else if (window)
				{
					++info.windowCount;
					if (nextTile && isRoomTile(nextTile))
					{
						addEntryPosition(nextPos);
					}
				}
				else if (!blocked)
				{
					++info.openingCount;
				}
				if (blocked || !isRoomTile(nextTile))
				{
					continue;
				}
				int nextIndex = _save->getTileIndex(nextPos);
				if (_roomIdByTile[nextIndex] == -1)
				{
					_roomIdByTile[nextIndex] = info.id;
					open.push(nextPos);
				}
			}
		}
		info.isOutside = info.touchesMapEdge && info.tileCount >= 48;
		info.isHall = !info.isOutside && (info.tileCount >= 120 || (info.tileCount >= 80 && info.openingCount + info.windowCount >= info.tileCount * 2));
		_roomInfos.push_back(info);
	}

	_roomCacheSignature = signature;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "FactionAI room cache rebuilt: faction=" << (int)_faction
			<< ", signature=" << _roomCacheSignature
			<< ", rooms=" << _roomInfos.size();
		_save->appendToAutoBattleLog(log.str());
	}
	if (_faction == FACTION_PLAYER)
	{
		writeBattleMapLog();
	}
}

void FactionAI::writeBattleMapLog() const
{
	if (!Options::autoBattleLog || _roomIdByTile.empty())
	{
		return;
	}

	std::string path = _save->getAutoBattleLogTextPath();
	const std::string suffix = ".txt";
	if (path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix)
	{
		path = path.substr(0, path.size() - suffix.size());
	}
	path += "-map.json";

	std::ofstream file(path);
	if (!file)
	{
		return;
	}

	file << "{\n";
	file << "\"schema\":\"oxce-auto-battle-map-v1\",\n";
	file << "\"turn\":" << _save->getTurn() << ",\n";
	file << "\"side\":" << (int)_save->getSide() << ",\n";
	file << "\"signature\":" << _roomCacheSignature << ",\n";
	file << "\"map\":{\"x\":" << _save->getMapSizeX()
		<< ",\"y\":" << _save->getMapSizeY()
		<< ",\"z\":" << _save->getMapSizeZ() << "},\n";

	file << "\"rooms\":[\n";
	for (size_t i = 0; i < _roomInfos.size(); ++i)
	{
		const BattleRoomInfo &room = _roomInfos[i];
		if (i)
		{
			file << ",\n";
		}
		file << "{\"id\":" << room.id
			<< ",\"tiles\":" << room.tileCount
			<< ",\"doors\":" << room.doorCount
			<< ",\"windows\":" << room.windowCount
			<< ",\"openings\":" << room.openingCount
			<< ",\"touches_edge\":" << (room.touchesMapEdge ? "true" : "false")
			<< ",\"outside\":" << (room.isOutside ? "true" : "false")
			<< ",\"hall\":" << (room.isHall ? "true" : "false")
			<< ",\"entries\":[";
		for (size_t entryIndex = 0; entryIndex < room.entryPositions.size(); ++entryIndex)
		{
			const Position &entry = room.entryPositions[entryIndex];
			if (entryIndex)
			{
				file << ",";
			}
			file << "[" << entry.x << "," << entry.y << "," << entry.z << "]";
		}
		file << "]}";
	}
	file << "\n],\n";

	file << "\"units\":[\n";
	bool firstUnit = true;
	for (auto *unit : *_save->getUnits())
	{
		if (!unit)
		{
			continue;
		}
		if (!firstUnit)
		{
			file << ",\n";
		}
		firstUnit = false;
		const Position pos = unit->getPosition();
		file << "{\"id\":" << unit->getId()
			<< ",\"type\":\"" << factionAIJsonEscape(unit->getType()) << "\""
			<< ",\"faction\":" << (int)unit->getFaction()
			<< ",\"out\":" << (unit->isOut() ? "true" : "false")
			<< ",\"pos\":[" << pos.x << "," << pos.y << "," << pos.z << "]"
			<< ",\"room\":" << (unit->getTile() ? _roomIdByTile[_save->getTileIndex(pos)] : -1)
			<< ",\"tu\":" << unit->getTimeUnits()
			<< ",\"health\":" << unit->getHealth()
			<< ",\"stun\":" << unit->getStunlevel()
			<< "}";
	}
	file << "\n],\n";

	file << "\"tiles\":[\n";
	for (int i = 0; i < _save->getMapSizeXYZ(); ++i)
	{
		Tile *tile = _save->getTile(i);
		const Position pos = tile->getPosition();
		if (i)
		{
			file << ",\n";
		}
		file << "{\"i\":" << i
			<< ",\"pos\":[" << pos.x << "," << pos.y << "," << pos.z << "]"
			<< ",\"room\":" << _roomIdByTile[i]
			<< ",\"no_floor\":" << (tile->hasNoFloor(_save) ? "true" : "false")
			<< ",\"danger\":" << (tile->getDangerous() ? "true" : "false")
			<< ",\"parts\":[";
		for (int part = O_FLOOR; part < O_MAX; ++part)
		{
			int mapDataId = -1;
			int mapDataSetId = -1;
			tile->getMapData(&mapDataId, &mapDataSetId, (TilePart)part);
			if (part != O_FLOOR)
			{
				file << ",";
			}
			file << "[" << mapDataSetId << "," << mapDataId << "]";
		}
		file << "]}";
	}
	file << "\n]\n";
	file << "}\n";

	std::ostringstream log;
	log << "FactionAI map snapshot written: " << path;
	_save->appendToAutoBattleLog(log.str());
}

bool FactionAI::getBestEnemyContactPosition(Position *position) const
{
	return getBestEnemyContactPosition(position, 0, 0);
}

bool FactionAI::getBestEnemyContactPosition(Position *position, const BattleRoomInfo **roomInfo, int *enemiesInRoom, bool *visibleContact) const
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
	if (roomInfo)
	{
		*roomInfo = getRoomInfo(bestContact->roomId);
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
	BattleItem *weapon = actor->getMainHandWeapon(false);
	if (weapon && weapon->getRules())
	{
		const RuleItem *rule = weapon->getRules();
		const UnitStats *stats = actor->getBaseStats();
		const int bestAccuracy = std::max(std::max(rule->getAccuracySnap(), rule->getAccuracyAimed()), rule->getAccuracyAuto());
		const int expectedPressure = std::max(0, rule->getPower()) + bestAccuracy * std::max(30, (int)stats->firing) / 100;
		score += expectedPressure / 2;
		if (rule->getBattleType() == BT_MELEE)
		{
			score += distance <= 3 ? 45 : -60;
		}
	}
	if (std::find(contact.canShootBy.begin(), contact.canShootBy.end(), actor) != contact.canShootBy.end())
	{
		score += 150;
	}
	else if (std::find(contact.visibleBy.begin(), contact.visibleBy.end(), actor) != contact.visibleBy.end())
	{
		score += 30;
	}
	else
	{
		score -= 80;
	}
	const bool wounded = contact.enemy->getHealth() > 0 && contact.enemy->getHealth() <= 35;
	score += wounded ? 65 : 0;
	score -= assignedCount * (wounded ? 25 : 35);
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

	std::vector<PlayerFactionEnemyContact> hiddenContacts;
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
		contact.roomId = getRoomId(enemy->getPosition());
		contact.roomSize = 0;
		contact.roomDoors = 0;
		contact.roomWindows = 0;
		contact.roomOpenings = 0;
		contact.roomOutside = false;
		contact.roomHall = false;
		contact.enemiesInRoom = 1;
		contact.visibleContact = false;
		if (const BattleRoomInfo *room = getRoomInfo(contact.roomId))
		{
			contact.roomSize = room->tileCount;
			contact.roomDoors = room->doorCount;
			contact.roomWindows = room->windowCount;
			contact.roomOpenings = room->openingCount;
			contact.roomOutside = room->isOutside;
			contact.roomHall = room->isHall;
		}
		for (auto* ally : _playerPlan.allies)
		{
			if (canSeeEnemy(ally, enemy))
			{
				contact.visibleBy.push_back(ally);
			}
			if (canShootEnemy(ally, enemy))
			{
				contact.canShootBy.push_back(ally);
			}
		}
		contact.visibleContact = !contact.visibleBy.empty();
		if (contact.visibleContact)
		{
			contact.focusScore = 20 * (int)contact.visibleBy.size() + 35 * (int)contact.canShootBy.size();
			_playerPlan.enemies.push_back(contact);
		}
		else
		{
			int nearestAllyDist = 100000;
			for (auto *ally : _playerPlan.allies)
			{
				nearestAllyDist = std::min(nearestAllyDist, Position::distance2d(ally->getPosition(), enemy->getPosition()));
			}
			contact.focusScore = -90 - nearestAllyDist * 2;
			contact.threatScore = contact.threatScore * 2 / 3;
			hiddenContacts.push_back(contact);
		}
	}
	if (_playerPlan.enemies.empty() && !hiddenContacts.empty())
	{
		std::sort(hiddenContacts.begin(), hiddenContacts.end(), [](const PlayerFactionEnemyContact &a, const PlayerFactionEnemyContact &b)
		{
			return a.threatScore + a.focusScore > b.threatScore + b.focusScore;
		});
		const int hiddenLimit = std::min(1, (int)hiddenContacts.size());
		for (int i = 0; i < hiddenLimit; ++i)
		{
			_playerPlan.enemies.push_back(hiddenContacts[i]);
		}
	}

	std::map<int, int> enemiesByRoom;
	for (const auto &contact : _playerPlan.enemies)
	{
		enemiesByRoom[contact.roomId]++;
	}
	for (auto &contact : _playerPlan.enemies)
	{
		contact.enemiesInRoom = enemiesByRoom[contact.roomId];
		if (!contact.roomOutside && !contact.roomHall)
		{
			contact.threatScore += 20 + std::max(0, contact.enemiesInRoom - 1) * 45;
			if (contact.roomDoors + contact.roomWindows <= 2)
			{
				contact.threatScore += 20;
			}
		}
		else if (contact.roomHall)
		{
			contact.threatScore += std::max(0, contact.enemiesInRoom - 1) * 20;
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
				if (contact.visibleContact)
				{
					continue;
				}
			}
			const int assignedCount = assignedCountByEnemyId[contact.enemy->getId()];
			int maxAssignees = contact.visibleContact ? 2 : (int)_playerPlan.allies.size();
			if (contact.visibleContact && (contact.threatScore >= 130 || contact.canShootBy.size() >= 2))
			{
				maxAssignees = 3;
			}
			if (contact.enemy->getHealth() > 0 && contact.enemy->getHealth() <= 35)
			{
				maxAssignees = 4;
			}
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
				<< ", room=" << bestContact->roomId
				<< ", roomEnemies=" << bestContact->enemiesInRoom
				<< ", roomSize=" << bestContact->roomSize
				<< ", doors=" << bestContact->roomDoors
				<< ", windows=" << bestContact->roomWindows
				<< ", entries=" << (getRoomInfo(bestContact->roomId) ? getRoomInfo(bestContact->roomId)->entryPositions.size() : 0)
				<< ", outside=" << bestContact->roomOutside
				<< ", hall=" << bestContact->roomHall
				<< ", visibleBy=" << bestContact->visibleBy.size()
				<< ", canShootBy=" << bestContact->canShootBy.size()
				<< ", visibleContact=" << bestContact->visibleContact
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
			<< ", room=" << contact.roomId
			<< ", roomEnemies=" << contact.enemiesInRoom
			<< ", roomSize=" << contact.roomSize
			<< ", doors=" << contact.roomDoors
			<< ", windows=" << contact.roomWindows
			<< ", openings=" << contact.roomOpenings
			<< ", entries=" << (getRoomInfo(contact.roomId) ? getRoomInfo(contact.roomId)->entryPositions.size() : 0)
			<< ", outside=" << contact.roomOutside
			<< ", hall=" << contact.roomHall
			<< ", visibleContact=" << contact.visibleContact
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
