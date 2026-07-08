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
#include "PlayerFactionPlanner.h"
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

constexpr unsigned long long FACTION_AI_ROOM_SIGNATURE_SEED = 1469598103934665603ull; // FNV-like seed for room-cache invalidation.
constexpr unsigned long long FACTION_AI_ROOM_SIGNATURE_PRIME = 1099511628211ull; // FNV-like multiplier for room-cache invalidation.
constexpr unsigned long long FACTION_AI_TILE_PART_SIGNATURE_SALT = 104729ull; // Salt separating tile parts in the room-cache signature.
constexpr int FACTION_AI_OUTSIDE_ROOM_MIN_TILES = 48; // Edge-touching zone size needed to classify it as outside.
constexpr int FACTION_AI_HALL_ROOM_MIN_TILES = 120; // Large interior zone size needed to classify it as hall/open.
constexpr int FACTION_AI_WIDE_HALL_ROOM_MIN_TILES = 80; // Smaller hall threshold when many openings/windows exist.
constexpr int FACTION_AI_WIDE_HALL_OPENING_FACTOR = 2; // Openings/windows per tile multiplier for wide hall detection.

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

FactionAI::FactionAI(SavedBattleGame *save, UnitFaction faction) : _save(save), _faction(faction), _playerPlanner(0), _roomCacheSignature(0)
{
}

PlayerFactionPlanner *FactionAI::getPlayerPlanner() const
{
	if (_faction != FACTION_PLAYER)
	{
		return 0;
	}
	if (!_playerPlanner)
	{
		_playerPlanner = new PlayerFactionPlanner(_save, this);
	}
	return _playerPlanner;
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
		if (PlayerFactionPlanner *planner = getPlayerPlanner())
		{
			planner->build(unit);
		}
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "FactionAI: planning done for unit=" << unit->getId();
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
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getAssignedTarget(unit) : 0;
}

std::string FactionAI::getAssignmentReason(BattleUnit *unit) const
{
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getAssignmentReason(unit) : std::string();
}

PlayerFactionStrategy FactionAI::getPlayerStrategy() const
{
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getStrategy() : PFS_HOLD_REACTION;
}

const char *FactionAI::getPlayerStrategyName() const
{
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getStrategyName() : "none";
}

int FactionAI::getEnemyContactCount() const
{
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getEnemyContactCount() : 0;
}

unsigned long long FactionAI::calculateRoomCacheSignature() const
{
	unsigned long long signature = FACTION_AI_ROOM_SIGNATURE_SEED;
	signature ^= (unsigned long long)_save->getMapSizeXYZ();
	signature *= FACTION_AI_ROOM_SIGNATURE_PRIME;
	for (int i = 0; i < _save->getMapSizeXYZ(); ++i)
	{
		const Tile *tile = _save->getTile(i);
		for (int part = O_FLOOR; part < O_MAX; ++part)
		{
			signature ^= (unsigned long long)(reinterpret_cast<std::uintptr_t>(tile->getMapData((TilePart)part)) + part * FACTION_AI_TILE_PART_SIGNATURE_SALT);
			signature *= FACTION_AI_ROOM_SIGNATURE_PRIME;
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
		info.isOutside = info.touchesMapEdge && info.tileCount >= FACTION_AI_OUTSIDE_ROOM_MIN_TILES;
		info.isHall = !info.isOutside && (info.tileCount >= FACTION_AI_HALL_ROOM_MIN_TILES || (info.tileCount >= FACTION_AI_WIDE_HALL_ROOM_MIN_TILES && info.openingCount + info.windowCount >= info.tileCount * FACTION_AI_WIDE_HALL_OPENING_FACTOR));
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
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getBestEnemyContactPosition(position) : false;
}

bool FactionAI::getBestEnemyContactPosition(Position *position, const BattleRoomInfo **roomInfo, int *enemiesInRoom, bool *visibleContact) const
{
	PlayerFactionPlanner *planner = getPlayerPlanner();
	return planner ? planner->getBestEnemyContactPosition(position, roomInfo, enemiesInRoom, visibleContact) : false;
}

}
