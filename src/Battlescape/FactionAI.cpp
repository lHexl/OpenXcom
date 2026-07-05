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
#include "../Savegame/SavedBattleGame.h"

namespace OpenXcom
{

FactionAI::FactionAI(SavedBattleGame *save, UnitFaction faction) : _save(save), _faction(faction)
{
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
	unit->think(action);
}

void FactionAI::setWeaponPickedUp(BattleUnit *unit) const
{
	AIModule *ai = getUnitModule(unit);
	if (ai)
	{
		ai->setWeaponPickedUp();
	}
}

}
