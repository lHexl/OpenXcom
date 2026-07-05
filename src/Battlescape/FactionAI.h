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

namespace OpenXcom
{

class AIModule;
class SavedBattleGame;
struct BattleAction;

/**
 * Faction-level AI coordinator.
 *
 * For now it delegates the actual decision to the unit's existing AIModule,
 * but all battlescape AI calls pass through this object so faction-wide
 * strategy can be added without touching the per-unit module callers.
 */
class FactionAI
{
private:
	SavedBattleGame *_save;
	UnitFaction _faction;

public:
	FactionAI(SavedBattleGame *save, UnitFaction faction);
	AIModule *getUnitModule(BattleUnit *unit) const;
	void think(BattleUnit *unit, BattleAction *action) const;
	void setWeaponPickedUp(BattleUnit *unit) const;
	UnitFaction getFaction() const { return _faction; }
};

}
