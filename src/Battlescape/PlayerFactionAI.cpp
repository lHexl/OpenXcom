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
#include <climits>
#include <algorithm>
#include <sstream>
#include "PlayerFactionAI.h"
#include "FactionAI.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/Node.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "TileEngine.h"
#include "BattlescapeState.h"
#include "../Savegame/Tile.h"
#include "Pathfinding.h"
#include "../Engine/RNG.h"
#include "../Engine/Logger.h"
#include "../Engine/Game.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleItem.h"
#include "../fmath.h"

namespace OpenXcom
{


/**
 * Sets up a BattleAIState.
 * @param save Pointer to the battle game.
 * @param unit Pointer to the unit.
 * @param node Pointer to the node the unit originates from.
 */
PlayerFactionAI::PlayerFactionAI(SavedBattleGame *save, BattleUnit *unit, Node *node) :
	AIModule(save, unit, node), _save(save), _unit(unit), _aggroTarget(0), _knownEnemies(0), _visibleEnemies(0), _spottingEnemies(0),
	_escapeTUs(0), _ambushTUs(0), _weaponPickedUp(false), _rifle(false), _melee(false), _blaster(false), _grenade(false),
	_didPsi(false), _AIMode(AI_PATROL), _closestDist(100), _fromNode(node), _toNode(0), _foundBaseModuleToDestroy(false),
	_stalkAmbushAction(false), _cleanShotMoveAction(false), _fallbackCoverAction(false), _factionAI(0)
{
	_traceAI = Options::traceAI;

	_reserve = BA_NONE;
	_intelligence = _unit->getIntelligence();
	_escapeAction = BattleAction();
	_ambushAction = BattleAction();
	_attackAction = BattleAction();
	_patrolAction = BattleAction();
	_psiAction = BattleAction();
	_targetFaction = FACTION_PLAYER;
	if (_unit->getFaction() == FACTION_PLAYER || _unit->getOriginalFaction() == FACTION_NEUTRAL)
	{
		_targetFaction = FACTION_HOSTILE;
	}
}

/**
 * Deletes the BattleAIState.
 */
PlayerFactionAI::~PlayerFactionAI()
{

}

/**
 * Sets the target faction.
 */
void PlayerFactionAI::setTargetFaction(UnitFaction f)
{
	_targetFaction = f;
}

/**
 * Resets the unsaved AI state.
 */
void PlayerFactionAI::reset()
{
	// these variables are not saved in save() and also not initiated in think()
	_escapeTUs = 0;
	_ambushTUs = 0;
}

/**
 * Loads the AI state from a YAML file.
 * @param node YAML node.
 */
void PlayerFactionAI::load(const YAML::YamlNodeReader& reader)
{
	int fromNodeID = reader["fromNode"].readVal(-1);
	int toNodeID = reader["toNode"].readVal(-1);
	_AIMode = reader["AIMode"].readVal(AI_PATROL);
	reader.tryRead("wasHitBy", _wasHitBy);
	reader.tryRead("weaponPickedUp", _weaponPickedUp);
	reader.tryRead("targetFaction", _targetFaction);

	// TODO: Figure out why AI are sometimes left with junk nodes
	if (fromNodeID >= 0 && (size_t)fromNodeID < _save->getNodes()->size())
	{
		_fromNode = _save->getNodes()->at(fromNodeID);
	}
	if (toNodeID >= 0 && (size_t)toNodeID < _save->getNodes()->size())
	{
		_toNode = _save->getNodes()->at(toNodeID);
	}
}

/**
 * Saves the AI state to a YAML file.
 * @return YAML node.
 */
void PlayerFactionAI::save(YAML::YamlNodeWriter writer) const
{
	writer.setAsMap();
	writer.setFlowStyle();
	writer.write("fromNode", _fromNode ? _fromNode->getID() : -1);
	writer.write("toNode", _toNode ? _toNode->getID() : -1);
	writer.write("AIMode", _AIMode);
	writer.write("wasHitBy", _wasHitBy);
	if (_weaponPickedUp)
		writer.write("weaponPickedUp", _weaponPickedUp);
	if (_unit->getOriginalFaction() == FACTION_HOSTILE && _unit->getFaction() == FACTION_NEUTRAL && _targetFaction == FACTION_HOSTILE)
	{
		writer.write("targetFaction", _targetFaction);
	}
}

/**
 * Mindless charge strategy. For mindless units.
 * Consists of running around and charging nearest visible enemy.
 * @param action (possible) AI action to execute after thinking is done.
 */
void PlayerFactionAI::dont_think(BattleAction *action)
{
	_melee = false;
	action->weapon = _unit->getUtilityWeapon(BT_MELEE);

	if (_traceAI)
	{
		Log(LOG_INFO) << "LEEROY: Unit " << _unit->getId() << " of type " << _unit->getType() << " is Leeroy...";
	}
	if (action->weapon)
	{
		if (action->weapon->getRules()->getBattleType() == BT_MELEE)
		{
			if (_save->canUseWeapon(action->weapon, _unit, false, BA_HIT))
			{
				_melee = true;
			}
		}
		else
		{
			action->weapon = 0;
		}
	}

	bool canRun = _melee && _unit->getArmor()->allowsRunning(false) && _unit->getEnergy() > _unit->getBaseStats()->stamina * 0.4f;
	int visibleEnemiesToAttack = selectNearestTargetLeeroy(canRun);
	if (_traceAI)
	{
		Log(LOG_INFO) << "LEEROY: visibleEnemiesToAttack: " << visibleEnemiesToAttack << " _melee: " << _melee << (canRun ? " run" : "");
	}
	if ((visibleEnemiesToAttack > 0) && _melee)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "LEEROY: LEEROYIN' at someone!";
		}
		meleeActionLeeroy(canRun);
		action->type = _attackAction.type;
		action->run = _attackAction.run;
		action->target = _attackAction.target;
		// if this is a firepoint action, set our facing.
		action->finalFacing = _attackAction.finalFacing;
		action->updateTU();
	}
	else
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "LEEROY: No one to LEEROY!, patrolling...";
		}
		setupPatrol();
		_unit->setCharging(0);
		_reserve = BA_NONE;
		action->type = _patrolAction.type;
		action->target = _patrolAction.target;
	}
}

/**
 * Tries to use self-target medikit if needed and desired (used for AI).
 * @return Was it used?
 */
bool PlayerFactionAI::medikit_think(BattleMediKitType healOrStim)
{
	// 1. sanity checks, division by zero
	BattleUnit* self = _unit;

	if (self->getBaseStats()->stamina <= 0 || self->getBaseStats()->health <= 0)
	{
		return false;
	}

	// 2. quick unit checks (without RNG)
	int totalWounds = self->getFatalWounds();
	int percentHealthLeft = Clamp((self->getHealth() - self->getStunlevel()) * 100 / self->getBaseStats()->health, 0, 100);
	int percentEnergyLeft = Clamp(self->getEnergy() * 100 / self->getBaseStats()->stamina, 0, 100);

	if (healOrStim == BMT_HEAL)
	{
		if (totalWounds <= 0)
			return false;
	}
	else if (healOrStim == BMT_STIMULANT)
	{
		if (self->getStunlevel() <= 0 && percentEnergyLeft >= 40)
			return false;
	}
	else
	{
		// unsupported medikit type
		return false;
	}

	// 3. quick item checks
	std::vector<BattleItem*> usableMedikits;

	for (auto* item : *self->getInventory())
	{
		const RuleItem* itemRule = item->getRules();
		if (itemRule->getBattleType() == BT_MEDIKIT &&
			(itemRule->getMediKitType() == healOrStim || itemRule->getMediKitType() == BMT_NORMAL) &&
			itemRule->getAllowTargetSelf())
		{
			if (_save->getTurn() < itemRule->getAIUseDelay(_save->getMod()))
			{
				// can't use it yet, too soon
				continue;
			}
			usableMedikits.push_back(item);
		}
	}
	if (usableMedikits.empty())
	{
		// no compatible medikits available
		return false;
	}

	// 4. detailed unit checks (with RNG)
	bool wantsToHeal = false;
	bool wantsToStimStun = false;
	bool wantsToStimEnergy = false;

	if (healOrStim == BMT_HEAL)
	{
		if (totalWounds > 0)
		{
			if (self->getStunlevel() + totalWounds >= self->getHealth())
			{
				// going to die or pass out unless we do something, so do something!
				wantsToHeal = true;
			}
			else
			{
				//  0% health left = 120% chance to heal
				// 15% health left =  60% chance to heal
				// 30% health left =   0% chance to heal (actually 5% chance because of random heal wish)
				int chanceToHeal = 120 - (percentHealthLeft * 4);
				if (chanceToHeal <= 0)
				{
					// 5% for random heal wish (it's not urgent, but you know damage accumulates over time)
					chanceToHeal = 5;
				}
				wantsToHeal = RNG::percent(chanceToHeal);
			}
		}
		if (!wantsToHeal)
		{
			return false;
		}
	}
	else if (healOrStim == BMT_STIMULANT)
	{
		// 1. do we want to decrease stun level?
		if (self->getStunlevel() > 0)
		{
			if (self->getStunlevel() + totalWounds >= self->getHealth())
			{
				// going to die or pass out unless we do something, so do something!
				wantsToStimStun = true;
			}
			else
			{
				//  0% health left = 140% chance to stim
				// 10% health left =  70% chance to stim
				// 20% health left =   0% chance to stim
				int chanceToStim1 = 140 - (percentHealthLeft * 7);
				wantsToStimStun = chanceToStim1 > 0 ? RNG::percent(chanceToStim1) : false;
			}
		}
		// 2. do we want to increase energy?
		if (percentEnergyLeft < 40)
		{
			//  0% energy left = 120% chance to stim
			// 20% energy left =  60% chance to stim
			// 40% energy left =   0% chance to stim
			int chanceToStim2 = 120 - (percentEnergyLeft * 3);
			wantsToStimEnergy = RNG::percent(chanceToStim2);
		}
		if (!wantsToStimStun && !wantsToStimEnergy)
		{
			return false;
		}
	}

	// 5. let's do it
	bool used = false;

	for (auto* medikit : usableMedikits)
	{
		const RuleItem* medikitRule = medikit->getRules();
		{
			if ((wantsToHeal && medikit->getHealQuantity() > 0) ||
				(wantsToStimStun && medikit->getStimulantQuantity() > 0 && medikitRule->getStunRecovery() > 0) ||
				(wantsToStimEnergy && medikit->getStimulantQuantity() > 0 && medikitRule->getEnergyRecovery() > 0))
			{
				BattleAction medikitAction;
				{
					medikitAction.weapon = medikit;
					medikitAction.type = BA_USE;
					medikitAction.actor = self;

					medikitAction.updateTU();

					// yes, hardcoded 4 TUs
					// AI throwing grenades does that for decades and nobody cares, so calm down
					// also, AI pays this cost each time, even if using the same medikit multiple times in a row
					medikitAction.Time += 4; // 4TUs for picking up the medikit

					// sigh, modders...
					//medikitAction.Health = 0;
					//medikitAction.Stun = 0;
				}
				if (!medikitAction.spendTU())
				{
					// not enough TUs, try next item
					continue;
				}
				else
				{
					switch (healOrStim)
					{
					case BMT_HEAL:
						if (_traceAI)
						{
							Log(LOG_INFO) << "  Using medikit (heal). TU*/HP/Stun/Wounds: " <<
								self->getTimeUnits() << "/" << self->getHealth() << "/" << self->getStunlevel() << "/" << totalWounds;
						}
						for (int i = 0; i < BODYPART_MAX; ++i)
						{
							if (self->getFatalWound((UnitBodyPart)i))
							{
								_save->getTileEngine()->medikitUse(&medikitAction, self, BMA_HEAL, (UnitBodyPart)i);
								_save->getTileEngine()->medikitRemoveIfEmpty(&medikitAction);
								used = true;
								break;
							}
						}
						break;
					case BMT_STIMULANT:
						if (_traceAI)
						{
							if (wantsToStimStun)
							{
								Log(LOG_INFO) << "  Using medikit (-stun). TU*/HP/Stun/Wounds: " <<
									self->getTimeUnits() << "/" << self->getHealth() << "/" << self->getStunlevel() << "/" << totalWounds;
							}
							else
							{
								Log(LOG_INFO) << "  Using medikit (+energy). TU*/Energy: " << self->getTimeUnits() << "/" << self->getEnergy();
							}
						}
						_save->getTileEngine()->medikitUse(&medikitAction, self, BMA_STIMULANT, BODYPART_TORSO);
						_save->getTileEngine()->medikitRemoveIfEmpty(&medikitAction);
						used = true;
						break;
					case BMT_PAINKILLER:
					case BMT_NORMAL:
						// not supported
						break;
					}
				}
			}
		}
		if (used)
		{
			// only one use per attempt
			break;
		}
	}

	// 6. if we used something, let's try again
	return used;
}

PlayerFactionAI::PlayerAIRole PlayerFactionAI::getPlayerAIRole(BattleItem *weapon) const
{
	if (!weapon)
	{
		return ROLE_SUPPORT;
	}

	const RuleItem *rule = weapon->getRules();
	const UnitStats *stats = _unit->getBaseStats();
	if (rule->getBattleType() == BT_MELEE)
	{
		return ROLE_MELEE;
	}

	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(BA_SNAPSHOT, _unit, weapon);
	BattleItem *ammo = weapon->getAmmoForAction(BA_SNAPSHOT);
	const bool explosive = ammo && ammo->getRules()->getExplosionRadius(attack) > 0;
	const bool heavy = weapon->getTotalWeight() > stats->strength / 2 || explosive || weapon->getCurrentWaypoints() != 0;
	if (heavy && stats->strength >= weapon->getTotalWeight())
	{
		return ROLE_HEAVY;
	}

	const bool accurateWeapon = rule->getAccuracyAimed() >= 100 || (rule->getAccuracySnap() >= 70 && rule->getAccuracyAuto() == 0);
	if (stats->firing >= 65 && accurateWeapon)
	{
		return ROLE_MARKSMAN;
	}

	if (stats->tu >= 58 || stats->reactions >= 55 || rule->getAccuracyAuto() > 0)
	{
		return ROLE_ASSAULT;
	}

	return ROLE_SUPPORT;
}

int PlayerFactionAI::getPreferredEngagementRange(BattleItem *weapon) const
{
	const PlayerAIRole role = getPlayerAIRole(weapon);
	const UnitStats *stats = _unit->getBaseStats();
	int preferred = 9;
	switch (role)
	{
	case ROLE_MARKSMAN:
		preferred = 13;
		break;
	case ROLE_HEAVY:
		preferred = 12;
		break;
	case ROLE_ASSAULT:
		preferred = 8;
		break;
	case ROLE_MELEE:
		preferred = 3;
		break;
	case ROLE_SUPPORT:
	default:
		preferred = 10;
		break;
	}

	if (weapon)
	{
		const RuleItem *rule = weapon->getRules();
		if (rule->getAccuracyAimed() >= 100 && stats->firing >= 60)
		{
			preferred += 1;
		}
		if (rule->getAccuracyAuto() > 0 && rule->getAccuracyAimed() < 90)
		{
			preferred -= 2;
		}
		if (weapon->getTotalWeight() > stats->strength)
		{
			preferred += 2;
		}
	}

	if (stats->firing < 45)
	{
		preferred -= 2;
	}
	else if (stats->firing >= 70)
	{
		preferred += 1;
	}
	return Clamp(preferred, 3, 15);
}

int PlayerFactionAI::scoreWeaponForUnit(BattleItem *weapon) const
{
	if (!weapon || !_save->canUseWeapon(weapon, _unit, false, BA_NONE))
	{
		return -100000;
	}

	const RuleItem *rule = weapon->getRules();
	const UnitStats *stats = _unit->getBaseStats();
	if (rule->getBattleType() != BT_FIREARM && rule->getBattleType() != BT_MELEE)
	{
		return -100000;
	}

	int score = rule->getPower() * 3;
	score += rule->getAccuracySnap() * std::max(30, (int)stats->firing) / 60;
	score += rule->getAccuracyAimed() * std::max(30, (int)stats->firing) / 90;
	score += rule->getAccuracyAuto() * std::max(30, (int)stats->reactions) / 80;
	if (rule->getBattleType() == BT_MELEE)
	{
		score += rule->getAccuracyMelee() * std::max(30, (int)stats->melee) / 70;
		score -= 180;
	}
	if (weapon->getCurrentWaypoints() != 0)
	{
		score += 80;
	}
	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(BA_SNAPSHOT, _unit, weapon);
	BattleItem *ammo = weapon->getAmmoForAction(BA_SNAPSHOT);
	if (ammo && ammo->getRules()->getExplosionRadius(attack) > 0)
	{
		score += ammo->getRules()->getExplosionRadius(attack) * 18;
	}
	const int weight = weapon->getTotalWeight();
	if (weight > stats->strength)
	{
		score -= (weight - stats->strength) * 25;
	}
	else
	{
		score -= weight * 2;
	}
	if (!weapon->haveAnyAmmo() && weapon->isWeaponWithAmmo())
	{
		score -= 250;
	}
	return score;
}

BattleItem *PlayerFactionAI::selectBestCarriedWeapon() const
{
	BattleItem *best = _unit->getMainHandWeapon(false);
	int bestScore = scoreWeaponForUnit(best);
	BattleItem *rightHand = _unit->getItem(_save->getMod()->getInventoryRightHand());
	BattleItem *leftHand = _unit->getItem(_save->getMod()->getInventoryLeftHand());
	BattleItem *hands[] = { rightHand, leftHand };
	for (auto *item : hands)
	{
		int score = scoreWeaponForUnit(item);
		if (score > bestScore)
		{
			bestScore = score;
			best = item;
		}
	}
	return best;
}

bool PlayerFactionAI::tryEquipGroundWeapon(BattleItem *item)
{
	if (!item || !item->getTile() || item->getTile()->getPosition() != _unit->getPosition())
	{
		return false;
	}
	const int previousScore = scoreWeaponForUnit(selectBestCarriedWeapon());
	const int itemScore = scoreWeaponForUnit(item);
	const int requiredGain = _knownEnemies ? 160 : 40;
	if (previousScore > -50000 && itemScore <= previousScore + requiredGain)
	{
		return false;
	}

	const RuleInventory *slot = 0;
	BattleItem *replaced = 0;
	if (!_unit->getItem(_save->getMod()->getInventoryRightHand()))
	{
		slot = _save->getMod()->getInventoryRightHand();
	}
	else if (!_unit->getItem(_save->getMod()->getInventoryLeftHand()))
	{
		slot = _save->getMod()->getInventoryLeftHand();
	}
	else
	{
		BattleItem *rightHand = _unit->getItem(_save->getMod()->getInventoryRightHand());
		BattleItem *leftHand = _unit->getItem(_save->getMod()->getInventoryLeftHand());
		const int rightScore = scoreWeaponForUnit(rightHand);
		const int leftScore = scoreWeaponForUnit(leftHand);
		if (rightScore <= leftScore)
		{
			slot = _save->getMod()->getInventoryRightHand();
			replaced = rightHand;
		}
		else
		{
			slot = _save->getMod()->getInventoryLeftHand();
			replaced = leftHand;
		}
	}

	const int tuCost = item->getMoveToCost(slot);
	if (_unit->getTimeUnits() < tuCost)
	{
		return false;
	}
	Tile *itemTile = item->getTile();
	if (replaced)
	{
		replaced->moveToOwner(0);
		itemTile->addItem(replaced, _save->getMod()->getInventoryGround());
	}
	if (!_unit->fitItemToInventory(slot, item))
	{
		if (replaced)
		{
			itemTile->removeItem(replaced);
			_unit->fitItemToInventory(slot, replaced);
		}
		return false;
	}
	_unit->spendTimeUnits(tuCost);
	_weaponPickedUp = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction role weapon pickup: unit=" << _unit->getId()
			<< ", item=" << item->getRules()->getType()
			<< ", itemScore=" << itemScore
			<< ", previousScore=" << previousScore
			<< ", replaced=" << (replaced ? replaced->getRules()->getType() : "none")
			<< ", role=" << (int)getPlayerAIRole(item)
			<< ", tuCost=" << tuCost
			<< ", position=" << _unit->getPosition()
			<< ", reason=better_role_weapon_on_current_tile";
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::tryEquipGroundExplosive(BattleItem *item)
{
	if (!item || !item->getTile() || item->getTile()->getPosition() != _unit->getPosition() || !item->getRules()->isGrenadeOrProxy())
	{
		return false;
	}
	BattleAction action;
	action.actor = _unit;
	action.weapon = item;
	action.type = BA_THROW;
	const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
	const int itemScore = std::max(0, item->getRules()->getPower()) * 2 + radius * 45 + (item->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? 80 : 0);
	if (radius <= 0 || itemScore < 180)
	{
		return false;
	}
	const RuleInventory *slot = 0;
	BattleItem *replaced = 0;
	if (!_unit->getItem(_save->getMod()->getInventoryRightHand()))
	{
		slot = _save->getMod()->getInventoryRightHand();
	}
	else if (!_unit->getItem(_save->getMod()->getInventoryLeftHand()))
	{
		slot = _save->getMod()->getInventoryLeftHand();
	}
	else
	{
		BattleItem *rightHand = _unit->getItem(_save->getMod()->getInventoryRightHand());
		BattleItem *leftHand = _unit->getItem(_save->getMod()->getInventoryLeftHand());
		const int rightScore = scoreWeaponForUnit(rightHand);
		const int leftScore = scoreWeaponForUnit(leftHand);
		if (rightScore <= leftScore)
		{
			slot = _save->getMod()->getInventoryRightHand();
			replaced = rightHand;
		}
		else
		{
			slot = _save->getMod()->getInventoryLeftHand();
			replaced = leftHand;
		}
	}
	const int tuCost = item->getMoveToCost(slot);
	if (_unit->getTimeUnits() < tuCost)
	{
		return false;
	}
	Tile *itemTile = item->getTile();
	if (replaced)
	{
		replaced->moveToOwner(0);
		itemTile->addItem(replaced, _save->getMod()->getInventoryGround());
	}
	if (!_unit->fitItemToInventory(slot, item))
	{
		if (replaced)
		{
			itemTile->removeItem(replaced);
			_unit->fitItemToInventory(slot, replaced);
		}
		return false;
	}
	_unit->spendTimeUnits(tuCost);
	_weaponPickedUp = true;
	_grenade = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction explosive pickup: unit=" << _unit->getId()
			<< ", item=" << item->getRules()->getType()
			<< ", itemScore=" << itemScore
			<< ", radius=" << radius
			<< ", power=" << item->getRules()->getPower()
			<< ", replaced=" << (replaced ? replaced->getRules()->getType() : "none")
			<< ", tuCost=" << tuCost
			<< ", position=" << _unit->getPosition();
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupRoleWeaponPickup(BattleAction *action)
{
	if (_unit->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	const bool underContact = _visibleEnemies || _spottingEnemies;
	BattleItem *currentWeapon = selectBestCarriedWeapon();
	const int currentScore = scoreWeaponForUnit(currentWeapon);
	if (_knownEnemies && currentScore > -50000 && !underContact)
	{
		return false;
	}
	BattleItem *bestItem = 0;
	Position bestPos;
	const int requiredGain = underContact ? 120 : (_knownEnemies ? 160 : 70);
	int bestScore = currentScore + requiredGain;
	int bestMoveDist = 100000;
	bool hasCarriedExplosive = false;
	for (auto *carried : *_unit->getInventory())
	{
		if (carried && carried->getRules()->isGrenadeOrProxy() && _save->getTurn() >= carried->getRules()->getAIUseDelay(_save->getMod()))
		{
			hasCarriedExplosive = true;
			break;
		}
	}

	auto considerTile = [&](Tile *tile)
	{
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit) || getSpottingUnits(tile->getPosition()) > 0)
		{
			return;
		}
		if (underContact && tile->getPosition() != _unit->getPosition())
		{
			return;
		}
		for (auto *item : *tile->getInventory())
		{
			if (item && item->getRules()->isGrenadeOrProxy())
			{
				BattleAction grenadeAction;
				grenadeAction.actor = _unit;
				grenadeAction.weapon = item;
				grenadeAction.type = BA_THROW;
				const int radius = item->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(grenadeAction));
				const int explosiveScore = std::max(0, item->getRules()->getPower()) * 2 + radius * 45 + (item->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? 80 : 0);
				if ((!hasCarriedExplosive && explosiveScore >= 180) || explosiveScore > bestScore)
				{
					const int moveDist = Position::distance2d(_unit->getPosition(), tile->getPosition());
					if (moveDist <= (underContact ? 0 : 6))
					{
						bestItem = item;
						bestPos = tile->getPosition();
						bestScore = explosiveScore;
						bestMoveDist = moveDist;
					}
				}
				continue;
			}
			const int score = scoreWeaponForUnit(item);
			if (score <= bestScore)
			{
				continue;
			}
			const int moveDist = Position::distance2d(_unit->getPosition(), tile->getPosition());
			if (moveDist > 8)
			{
				continue;
			}
			if (_knownEnemies && currentScore > -50000 && moveDist > 2)
			{
				continue;
			}
			bestItem = item;
			bestPos = tile->getPosition();
			bestScore = score;
			bestMoveDist = moveDist;
		}
	};

	considerTile(_save->getTile(_unit->getPosition()));
	if (bestItem && bestPos == _unit->getPosition())
	{
		const bool explosiveItem = bestItem->getRules()->isGrenadeOrProxy();
		if ((explosiveItem && tryEquipGroundExplosive(bestItem)) || tryEquipGroundWeapon(bestItem))
		{
			action->weapon = selectBestCarriedWeapon();
			_attackAction.weapon = action->weapon;
			if (explosiveItem)
			{
				_attackAction.type = BA_RETHINK;
				grenadeAction();
				if (_attackAction.type == BA_RETHINK)
				{
					_attackAction.actor = _unit;
					_attackAction.weapon = action->weapon;
					_attackAction.target = _unit->getPosition();
					_attackAction.type = BA_NONE;
					_AIMode = AI_COMBAT;
				}
			}
			return true;
		}
	}

	for (auto tileIndex : _reachable)
	{
		considerTile(_save->getTile(tileIndex));
	}

	if (!bestItem || bestPos == _unit->getPosition())
	{
		return false;
	}

	_patrolAction.actor = _unit;
	_patrolAction.weapon = action->weapon;
	_patrolAction.target = bestPos;
	_patrolAction.type = BA_WALK;
	if (_attackAction.type == BA_RETHINK)
	{
		_attackAction = _patrolAction;
	}
	_AIMode = AI_COMBAT;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction role weapon target: unit=" << _unit->getId()
			<< ", item=" << bestItem->getRules()->getType()
			<< ", itemScore=" << bestScore
			<< ", currentWeapon=" << (currentWeapon ? currentWeapon->getRules()->getType() : "none")
			<< ", currentScore=" << currentScore
			<< ", role=" << (int)getPlayerAIRole(bestItem)
			<< ", target=" << bestPos
			<< ", moveDistance=" << bestMoveDist
			<< ", reason=better_role_weapon_reachable";
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Runs any code the state needs to keep updating every AI cycle.
 * @param action (possible) AI action to execute after thinking is done.
 */
void PlayerFactionAI::think(BattleAction *action)
{
	action->type = BA_RETHINK;
	action->actor = _unit;
	action->weapon = _unit->getFaction() == FACTION_PLAYER ? selectBestCarriedWeapon() : _unit->getMainHandWeapon(false);
	_cleanShotMoveAction = false;
	_fallbackCoverAction = false;
	_attackAction.diff = _save->getBattleState()->getGame()->getSavedGame()->getDifficultyCoefficient();
	_attackAction.actor = _unit;
	_attackAction.run = false;
	_attackAction.weapon = action->weapon;
	_attackAction.number = action->number;
	_escapeAction.number = action->number;
	_knownEnemies = countKnownTargets();
	_visibleEnemies = selectNearestTarget();
	_spottingEnemies = getSpottingUnits(_unit->getPosition());
	_melee = (_unit->getUtilityWeapon(BT_MELEE) != 0);
	_rifle = false;
	_blaster = false;
	_reachable = _save->getPathfinding()->findReachable(_unit, BattleActionCost());
	_wasHitBy.clear();
	_foundBaseModuleToDestroy = false;
	_stalkAmbushAction = false;

	if (_unit->getCharging() && _unit->getCharging()->isOut())
	{
		_unit->setCharging(0);
	}

	if (_traceAI)
	{
		if (_unit->getFaction() == FACTION_HOSTILE)
		{
			Log(LOG_INFO) << "Unit has " << _visibleEnemies << "/" << _knownEnemies << " known enemies visible, " << _spottingEnemies << " of whom are spotting him. ";
		}
		else
		{
			Log(LOG_INFO) << "Civilian Unit has " << _visibleEnemies << " enemies visible, " << _spottingEnemies << " of whom are spotting him. ";
		}
		std::string AIMode;
		switch (_AIMode)
		{
		case AI_PATROL:
			AIMode = "Patrol";
			break;
		case AI_AMBUSH:
			AIMode = "Ambush";
			break;
		case AI_COMBAT:
			AIMode = "Combat";
			break;
		case AI_ESCAPE:
			AIMode = "Escape";
			break;
		}
		Log(LOG_INFO) << "Currently using " << AIMode << " behaviour";
	}

	if (_unit->isLeeroyJenkins())
	{
		dont_think(action);
		return;
	}

	if (action->weapon)
	{
		const RuleItem *rule = action->weapon->getRules();
		if (_save->canUseWeapon(action->weapon, _unit, false, BA_NONE)) // Note: ammo is not checked here
		{
			if (rule->getBattleType() == BT_FIREARM)
			{
				if (action->weapon->getCurrentWaypoints() != 0)
				{
					_blaster = true;
					_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_AIMEDSHOT, _unit, action->weapon));
				}
				else
				{
					_rifle = true;
					_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_SNAPSHOT, _unit, action->weapon));
				}
			}
			else if (rule->getBattleType() == BT_MELEE)
			{
				_melee = true;
				_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_HIT, _unit, action->weapon));
			}
		}
		else
		{
			action->weapon = 0;
		}
	}

	BattleItem *grenadeItem = _unit->getGrenadeFromBelt(_save);
	_grenade = grenadeItem != 0;
	if (!_grenade)
	{
		for (auto *item : *_unit->getInventory())
		{
			if (item && item->getRules()->isGrenadeOrProxy() && _save->getTurn() >= item->getRules()->getAIUseDelay(_save->getMod()))
			{
				_grenade = true;
				break;
			}
		}
	}
	const int preferredRange = _unit->getFaction() == FACTION_PLAYER ? getPreferredEngagementRange(action->weapon) : 10;
	const PlayerAIRole playerRole = _unit->getFaction() == FACTION_PLAYER ? getPlayerAIRole(action->weapon) : ROLE_ASSAULT;
	if (_unit->getFaction() == FACTION_PLAYER && Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction role: unit=" << _unit->getId()
			<< ", role=" << (int)playerRole
			<< ", weapon=" << (action->weapon ? action->weapon->getRules()->getType() : "none")
			<< ", weaponScore=" << scoreWeaponForUnit(action->weapon)
			<< ", preferredRange=" << preferredRange
			<< ", firing=" << _unit->getBaseStats()->firing
			<< ", reactions=" << _unit->getBaseStats()->reactions
			<< ", strength=" << _unit->getBaseStats()->strength
			<< ", tu=" << _unit->getBaseStats()->tu;
		_save->appendToAutoBattleLog(log.str());
	}

	if (_spottingEnemies && !_escapeTUs)
	{
		setupEscape();
	}

	if (_knownEnemies && !_melee && !_ambushTUs)
	{
		setupAmbush();
	}

	setupAttack();
	setupPatrol();
	if (_unit->getFaction() == FACTION_PLAYER && _attackAction.type == BA_RETHINK)
	{
		setupRoleWeaponPickup(action);
	}
	if (_unit->getFaction() == FACTION_PLAYER && _attackAction.type == BA_RETHINK)
	{
		setupFallbackCoverMove();
	}

	if (_unit->getFaction() == FACTION_PLAYER && _factionAI && (_knownEnemies || _factionAI->getEnemyContactCount() > 0))
	{
		Position contactPos;
		const BattleRoomInfo *contactRoom = 0;
		int enemiesInRoom = 1;
		bool visibleFactionContact = false;
		if (_factionAI->getBestEnemyContactPosition(&contactPos, &contactRoom, &enemiesInRoom, &visibleFactionContact))
		{
			const int roomSize = contactRoom ? contactRoom->tileCount : 1;
			const int unitRoomId = _factionAI->getRoomIdAt(_unit->getPosition());
			const bool smallDangerRoom = contactRoom && !contactRoom->isOutside && !contactRoom->isHall;
			const bool riskyRoom = smallDangerRoom && (enemiesInRoom > 1 || contactRoom->tileCount > 16 || contactRoom->doorCount + contactRoom->windowCount <= 2);
			const bool insideDangerRoom = riskyRoom && unitRoomId == contactRoom->id;
			const bool openAreaFight = !contactRoom || contactRoom->isOutside || contactRoom->isHall;
			const bool huntingHiddenContact = !visibleFactionContact && !_visibleEnemies;
			int activeAllies = 0;
			for (auto *other : *_save->getUnits())
			{
				if (other && !other->isOut() && other->getFaction() == _unit->getFaction())
				{
					++activeAllies;
				}
			}
			const bool endgameHiddenHunt = huntingHiddenContact && activeAllies <= 3;
			if (action->number <= 1 && (!_spottingEnemies || insideDangerRoom) && (insideDangerRoom || (!_visibleEnemies && _attackAction.type == BA_RETHINK)) && setupFactionStalkAmbush(contactPos, contactRoom, enemiesInRoom, action->weapon))
			{
				_AIMode = AI_COMBAT;
			}
			if (_visibleEnemies && !insideDangerRoom)
			{
				goto factionRoomTacticsDone;
			}
			Position bestPos = _unit->getPosition();
			int currentDist = Position::distance2d(bestPos, contactPos);
			int currentSpotters = getSpottingUnits(bestPos);
			int currentExposure = getEnemyFireExposure(bestPos);
			int bestDist = currentDist;
			int bestSpotters = currentSpotters;
			auto supportCoverScore = [&](const Position &pos) -> int
			{
				Tile *coverTile = _save->getTile(pos);
				if (!coverTile)
				{
					return 0;
				}
				int cover = 0;
				if (coverTile->getMapData(O_OBJECT))
				{
					cover += 8;
				}
				if (coverTile->getMapData(O_NORTHWALL))
				{
					cover += 6;
				}
				if (coverTile->getMapData(O_WESTWALL))
				{
					cover += 6;
				}
				return cover;
			};
			auto allyCrowdingPenalty = [&](const Position &pos) -> int
			{
				int penalty = 0;
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					const int dist = Position::distance2d(pos, other->getPosition());
					if (dist <= 1)
					{
						penalty += 90;
					}
					else if (dist <= 2)
					{
						penalty += 35;
					}
				}
				return penalty;
			};
			auto tooCloseToAlly = [&](const Position &pos) -> bool
			{
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					if (other->getPosition().z == pos.z && Position::distance2d(pos, other->getPosition()) <= 1)
					{
						return true;
					}
				}
				return false;
			};
			auto allyFireLanePenalty = [&](const Position &pos) -> int
			{
				int penalty = 0;
				const double px = (double)pos.x;
				const double py = (double)pos.y;
				for (auto *other : *_save->getUnits())
				{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
					{
						continue;
					}
					BattleItem *otherWeapon = other->getMainHandWeapon(false);
					if (!otherWeapon || otherWeapon->getRules()->getBattleType() != BT_FIREARM)
					{
						continue;
					}
					const Position otherPos = other->getPosition();
					const double vx = (double)(contactPos.x - otherPos.x);
					const double vy = (double)(contactPos.y - otherPos.y);
					const double lenSq = vx * vx + vy * vy;
					if (lenSq < 0.1)
					{
						continue;
					}
					const double ax = px - otherPos.x;
					const double ay = py - otherPos.y;
					const double t = (ax * vx + ay * vy) / lenSq;
					if (t <= 0.0 || t >= 1.0)
					{
						continue;
					}
					const double dx = ax - vx * t;
					const double dy = ay - vy * t;
					if (dx * dx + dy * dy <= 1.0)
					{
						penalty += 120;
					}
				}
				return penalty;
			};
			const bool hasRangedWeapon = _rifle || _blaster || _grenade || !_melee;
			const int desiredOpenDist = huntingHiddenContact ? (endgameHiddenHunt ? std::max(7, preferredRange - 1) : std::max(5, preferredRange - 4)) : (hasRangedWeapon ? preferredRange : 4);
			const int maxSupportMoveDistance = endgameHiddenHunt ? 2 : (huntingHiddenContact ? 6 : (openAreaFight ? 8 : 6));
			const int reserveMoveTU = endgameHiddenHunt ? 22 : (huntingHiddenContact ? 8 : (hasRangedWeapon ? 20 : 12));
			const int minOpenDistance = huntingHiddenContact ? (endgameHiddenHunt ? 5 : 2) : std::max(3, desiredOpenDist - (playerRole == ROLE_ASSAULT ? 4 : 5));
			const int maxOpenDistance = huntingHiddenContact ? desiredOpenDist + (endgameHiddenHunt ? 4 : 8) : desiredOpenDist + (playerRole == ROLE_MARKSMAN || playerRole == ROLE_HEAVY ? 5 : 4);
			int bestSupportScore = openAreaFight
				? abs(bestDist - desiredOpenDist) * 6 + bestSpotters * 40 - supportCoverScore(bestPos) * 5 + allyCrowdingPenalty(bestPos) + allyFireLanePenalty(bestPos)
				: bestDist * 4 + bestSpotters * 20 - supportCoverScore(bestPos) * 3 + allyCrowdingPenalty(bestPos) + allyFireLanePenalty(bestPos);
			if (huntingHiddenContact && currentDist <= desiredOpenDist + 2 && currentSpotters == 0)
			{
				bestSupportScore -= 120;
			}
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				Position pos = tile->getPosition();
				const int moveDist = Position::distance2d(pos, _unit->getPosition());
				if (pos.z != _unit->getPosition().z || moveDist > maxSupportMoveDistance)
				{
					continue;
				}
				if ((_knownEnemies || huntingHiddenContact) && moveDist * 6 > std::max(0, _unit->getTimeUnits() - reserveMoveTU))
				{
					continue;
				}
				if (openAreaFight && tooCloseToAlly(pos))
				{
					continue;
				}
				if (riskyRoom && contactRoom && _factionAI->getRoomIdAt(pos) == contactRoom->id)
				{
					continue;
				}
				int spotters = getSpottingUnits(pos);
				int exposure = getEnemyFireExposure(pos);
				if (!huntingHiddenContact && spotters > bestSpotters)
				{
					continue;
				}
				if (exposure > currentExposure + 60 && exposure > 90)
				{
					continue;
				}
				int dist = Position::distance2d(pos, contactPos);
				if (openAreaFight && hasRangedWeapon && dist < minOpenDistance)
				{
					continue;
				}
				if (openAreaFight && hasRangedWeapon && dist > maxOpenDistance)
				{
					continue;
				}
				if (endgameHiddenHunt && supportCoverScore(pos) < supportCoverScore(bestPos))
				{
					continue;
				}
				if (endgameHiddenHunt && dist < currentDist - 2)
				{
					continue;
				}
				if (!huntingHiddenContact && dist > currentDist + (spotters < currentSpotters ? 8 : 4))
				{
					continue;
				}
				int supportScore = openAreaFight
					? abs(dist - desiredOpenDist) * 6 + spotters * 40 + moveDist * 3 - supportCoverScore(pos) * 5 + allyCrowdingPenalty(pos) + allyFireLanePenalty(pos)
					: dist * 4 + spotters * 20 + moveDist * 2 - supportCoverScore(pos) * 3 + allyCrowdingPenalty(pos) + allyFireLanePenalty(pos);
				supportScore += exposure;
				if (huntingHiddenContact)
				{
					supportScore -= std::max(0, currentDist - dist) * 12;
					supportScore += spotters * 20;
				}
				if (supportScore < bestSupportScore)
				{
					bestSpotters = spotters;
					bestDist = dist;
					bestSupportScore = supportScore;
					bestPos = pos;
				}
			}
			if (bestPos != _unit->getPosition())
			{
				_patrolAction.actor = _unit;
				_patrolAction.weapon = action->weapon;
				_patrolAction.target = bestPos;
				_patrolAction.type = BA_WALK;
				if (_attackAction.type == BA_RETHINK)
				{
					_attackAction = _patrolAction;
				}
				_AIMode = AI_COMBAT;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction support move: unit=" << _unit->getId()
						<< ", contact=" << contactPos
						<< ", contactRoom=" << (contactRoom ? contactRoom->id : -1)
						<< ", roomEnemies=" << enemiesInRoom
						<< ", roomSize=" << roomSize
						<< ", roomDoors=" << (contactRoom ? contactRoom->doorCount : 0)
						<< ", roomWindows=" << (contactRoom ? contactRoom->windowCount : 0)
						<< ", roomEntries=" << (contactRoom ? contactRoom->entryPositions.size() : 0)
						<< ", roomOutside=" << (contactRoom ? contactRoom->isOutside : false)
						<< ", roomHall=" << (contactRoom ? contactRoom->isHall : false)
						<< ", avoidedRoom=" << riskyRoom
						<< ", insideDangerRoom=" << insideDangerRoom
						<< ", openAreaFight=" << openAreaFight
						<< ", visibleFactionContact=" << visibleFactionContact
						<< ", huntingHiddenContact=" << huntingHiddenContact
						<< ", endgameHiddenHunt=" << endgameHiddenHunt
						<< ", activeAllies=" << activeAllies
						<< ", role=" << (int)playerRole
						<< ", desiredOpenDistance=" << desiredOpenDist
						<< ", minOpenDistance=" << minOpenDistance
						<< ", maxOpenDistance=" << maxOpenDistance
						<< ", target=" << bestPos
						<< ", distance=" << bestDist
						<< ", moveDistance=" << Position::distance2d(bestPos, _unit->getPosition())
						<< ", maxMoveDistance=" << maxSupportMoveDistance
						<< ", spotters=" << bestSpotters;
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}
factionRoomTacticsDone:

	if (_psiAction.type != BA_NONE && !_didPsi && _save->getTurn() >= _psiAction.weapon->getRules()->getAIUseDelay(_save->getMod()))
	{
		_didPsi = true;
		action->type = _psiAction.type;
		action->target = _psiAction.target;
		action->number -= 1;
		action->weapon = _psiAction.weapon;
		action->updateTU();
		return;
	}
	else
	{
		_didPsi = false;
	}

	bool evaluate = false;

	switch (_AIMode)
		{
		case AI_PATROL:
			evaluate = (bool)(_spottingEnemies || _visibleEnemies || _knownEnemies || RNG::percent(10));
			break;
		case AI_AMBUSH:
			evaluate = (!_rifle || !_ambushTUs || _visibleEnemies);
			break;
		case AI_COMBAT:
			evaluate = (_attackAction.type == BA_RETHINK);
			break;
		case AI_ESCAPE:
			evaluate = (!_spottingEnemies || !_knownEnemies);
			break;
			}

	if (_weaponPickedUp)
	{
		evaluate = true;
		_weaponPickedUp = false;
	}
	else if (_spottingEnemies > 2
		|| _unit->getHealth() < 2 * _unit->getBaseStats()->health / 3)
	{
		evaluate = true;
	}


	if (_save->isCheating() && _AIMode != AI_COMBAT)
	{
		evaluate = true;
	}

	if (evaluate)
	{
		evaluateAIMode();
		if (_traceAI)
		{
			std::string AIMode;
			switch (_AIMode)
			{
			case AI_PATROL:
				AIMode = "Patrol";
				break;
			case AI_AMBUSH:
				AIMode = "Ambush";
				break;
			case AI_COMBAT:
				AIMode = "Combat";
				break;
			case AI_ESCAPE:
				AIMode = "Escape";
				break;
			}
			Log(LOG_INFO) << "Re-Evaluated, now using " << AIMode << " behaviour";
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && _visibleEnemies && _attackAction.type != BA_RETHINK && _attackAction.type != BA_WALK)
	{
		if (_AIMode == AI_ESCAPE && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction override: unit=" << _unit->getId()
				<< " switches Escape to Combat because a visible enemy can be attacked.";
			_save->appendToAutoBattleLog(log.str());
		}
		_AIMode = AI_COMBAT;
	}

	_reserve = BA_NONE;

	switch (_AIMode)
	{
	case AI_ESCAPE:
		_unit->setCharging(0);
		action->type = _escapeAction.type;
		action->target = _escapeAction.target;
		// end this unit's turn.
		action->finalAction = true;
		// ignore new targets.
		action->desperate = true;
		// if armor allow runing then run way from there.
		action->run = _escapeAction.run;
		// spin 180 at the end of your route.
		_unit->setHiding(true);
		break;
	case AI_PATROL:
		_unit->setCharging(0);
		if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			if (_unit->getFaction() == FACTION_PLAYER)
			{
				_reserve = BA_SNAPSHOT;
			}
			else switch (_unit->getAggression())
			{
			case 0:
				_reserve = BA_AIMEDSHOT;
				break;
			case 1:
				_reserve = BA_AUTOSHOT;
				break;
			case 2:
				_reserve = BA_SNAPSHOT;
				break;
			default:
				break;
			}
		}
		action->type = _patrolAction.type;
		action->target = _patrolAction.target;
		if (_unit->getFaction() == FACTION_PLAYER && _knownEnemies == 0 && action->type == BA_WALK)
		{
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		break;
	case AI_COMBAT:
		action->type = _attackAction.type;
		action->target = _attackAction.target;
		// this may have changed to a grenade.
		action->weapon = _attackAction.weapon;
		if (action->weapon && action->type == BA_THROW && action->weapon->getRules()->isGrenadeOrProxy())
		{
			_unit->spendCost(_unit->getActionTUs(BA_PRIME, action->weapon));
			_unit->spendTimeUnits(4);
		}
		// if this is a firepoint action, set our facing.
		action->finalFacing = _attackAction.finalFacing;
		action->updateTU();
		// if this is a "find fire point" action, don't increment the AI counter.
		if (action->type == BA_WALK && !_stalkAmbushAction && _rifle && _unit->getArmor()->allowsMoving()
			// so long as we can take a shot afterwards.
			&& BattleActionCost(BA_SNAPSHOT, _unit, action->weapon).haveTU())
		{
			action->number -= 1;
		}
		else if (action->type == BA_WALK && (_stalkAmbushAction || _cleanShotMoveAction || _fallbackCoverAction))
		{
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		else if (action->type == BA_LAUNCH)
		{
			action->waypoints = _attackAction.waypoints;
		}
		else if (action->type == BA_AIMEDSHOT || action->type == BA_AUTOSHOT || action->type == BA_SNAPSHOT)
		{
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		break;
	case AI_AMBUSH:
		_unit->setCharging(0);
		action->type = _ambushAction.type;
		action->target = _ambushAction.target;
		// face where we think our target will appear.
		action->finalFacing = _ambushAction.finalFacing;
		// end this unit's turn.
		action->finalAction = true;
		action->kneel = _unit->getArmor()->allowsKneeling(false);
		break;
	default:
		break;
	}

	if (_unit->getFaction() == FACTION_PLAYER && action->type == BA_WALK && (_visibleEnemies || _spottingEnemies))
	{
		const bool firePointMove = _AIMode == AI_COMBAT
			&& _attackAction.type == BA_WALK
			&& action->weapon
			&& BattleActionCost(BA_SNAPSHOT, _unit, action->weapon).haveTU();
		auto defensiveCoverScore = [&](const Position &pos) -> int
		{
			Tile *tile = _save->getTile(pos);
			if (!tile)
			{
				return 0;
			}
			int cover = 0;
			if (tile->getMapData(O_OBJECT))
			{
				cover += 8;
			}
			if (tile->getMapData(O_NORTHWALL))
			{
				cover += 6;
			}
			if (tile->getMapData(O_WESTWALL))
			{
				cover += 6;
			}
			return cover;
		};
		const int targetSpotters = getSpottingUnits(action->target);
		const int currentCover = defensiveCoverScore(_unit->getPosition());
		const int targetCover = defensiveCoverScore(action->target);
		const int defensiveMoveDistance = Position::distance2d(action->target, _unit->getPosition());
		int adjacentAllies = 0;
		for (auto *other : *_save->getUnits())
		{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
			{
				continue;
			}
			if (other->getPosition().z == action->target.z && Position::distance2d(action->target, other->getPosition()) <= 1)
			{
				++adjacentAllies;
			}
		}
		const bool saferSpottingMove = targetSpotters < _spottingEnemies && (targetCover >= currentCover || targetSpotters + 1 < _spottingEnemies || defensiveMoveDistance <= 2);
		const bool betterCoverMove = targetSpotters <= _spottingEnemies && targetCover >= currentCover + 6;
		const bool defensiveMove = defensiveMoveDistance <= 4 && adjacentAllies == 0 && (saferSpottingMove || betterCoverMove);
		if (!firePointMove && !defensiveMove && !_fallbackCoverAction)
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction caution: unit=" << _unit->getId()
					<< " holds position instead of walking under contact"
					<< ", visible=" << _visibleEnemies
					<< ", spotting=" << _spottingEnemies
					<< ", targetSpotters=" << targetSpotters
					<< ", currentCover=" << currentCover
					<< ", targetCover=" << targetCover
					<< ", moveDistance=" << defensiveMoveDistance
					<< ", adjacentAllies=" << adjacentAllies
					<< ", mode=" << _AIMode;
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		else if ((defensiveMove || _fallbackCoverAction) && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction defensive move allowed: unit=" << _unit->getId()
				<< ", target=" << action->target
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies
				<< ", targetSpotters=" << targetSpotters
				<< ", currentCover=" << currentCover
				<< ", targetCover=" << targetCover
				<< ", moveDistance=" << defensiveMoveDistance
				<< ", adjacentAllies=" << adjacentAllies
				<< ", mode=" << _AIMode;
			_save->appendToAutoBattleLog(log.str());
		}
	}

	if (_unit->getFaction() == FACTION_PLAYER && action->type == BA_WALK && _knownEnemies && (_visibleEnemies || _spottingEnemies))
	{
		const int moveDistance = Position::distance2d(action->target, _unit->getPosition());
		int reserveTU = 12;
		if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			reserveTU = (_visibleEnemies || _spottingEnemies) ? 20 : 12;
			BattleActionCost snapCost(BA_SNAPSHOT, _unit, action->weapon);
			if (snapCost.Time > 0)
			{
				reserveTU = std::max(reserveTU, (_visibleEnemies || _spottingEnemies) ? (int)snapCost.Time : (int)snapCost.Time / 2);
			}
		}
		const int estimatedMoveTU = moveDistance * 5;
		if (!_fallbackCoverAction && estimatedMoveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
		{
			if (Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction reserve hold: unit=" << _unit->getId()
					<< " holds position to keep TU reserve"
					<< ", target=" << action->target
					<< ", moveDistance=" << moveDistance
					<< ", estimatedMoveTU=" << estimatedMoveTU
					<< ", reserveTU=" << reserveTU
					<< ", currentTU=" << _unit->getTimeUnits()
					<< ", known=" << _knownEnemies
					<< ", visible=" << _visibleEnemies
					<< ", spotting=" << _spottingEnemies
					<< ", mode=" << _AIMode;
				_save->appendToAutoBattleLog(log.str());
			}
			action->type = BA_NONE;
			action->target = _unit->getPosition();
			action->finalAction = true;
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
	}

	if (action->type == BA_WALK)
	{
		// if we're moving, we'll have to re-evaluate our escape/ambush position.
		if (action->target != _unit->getPosition())
		{
			_escapeTUs = 0;
			_ambushTUs = 0;
		}
		else
		{
			action->type = BA_NONE;
		}
	}
}


/*
 * sets the "was hit" flag to true.
 */
void PlayerFactionAI::setWasHitBy(BattleUnit *attacker)
{
	if (attacker->getFaction() != _unit->getFaction() && !getWasHitBy(attacker->getId()))
		_wasHitBy.push_back(attacker->getId());
}

/*
 * Sets the "unit picked up a weapon" flag.
 */
void PlayerFactionAI::setWeaponPickedUp()
{
	_weaponPickedUp = true;
}

/*
 * Gets whether the unit was hit.
 * @return if it was hit.
 */
bool PlayerFactionAI::getWasHitBy(int attacker) const
{
	return std::find(_wasHitBy.begin(), _wasHitBy.end(), attacker) != _wasHitBy.end();
}
/*
 * Sets up a patrol action.
 * this is mainly going from node to node, moving about the map.
 * handles node selection, and fills out the _patrolAction with useful data.
 */
void PlayerFactionAI::setupPatrol()
{
	_patrolAction.clearTU();
	if (_toNode != 0 && _unit->getPosition() == _toNode->getPosition())
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Patrol destination reached!";
		}
		// destination reached
		// head off to next patrol node
		_fromNode = _toNode;
		freePatrolTarget();
		_toNode = 0;
		// take a peek through window before walking to the next node
		int dir = _save->getTileEngine()->faceWindow(_unit->getPosition());
		if (dir != -1 && dir != _unit->getDirection())
		{
			_unit->lookAt(dir);
			while (_unit->getStatus() == STATUS_TURNING)
			{
				_unit->turn();
			}
		}
	}

	if (_fromNode == 0)
	{
		// assume closest node as "from node"
		// on same level to avoid strange things, and the node has to match unit size or it will freeze
		int closest = 1000000;
		for (auto* node : *_save->getNodes())
		{
			if (node->isDummy())
			{
				continue;
			}
			int d = Position::distanceSq(_unit->getPosition(), node->getPosition());
			if (_unit->getPosition().z == node->getPosition().z
				&& d < closest
				&& (!(node->getType() & Node::TYPE_SMALL) || _unit->getArmor()->getSize() == 1))
			{
				_fromNode = node;
				closest = d;
			}
		}
	}
	int triesLeft = 5;

	while (_toNode == 0 && triesLeft)
	{
		triesLeft--;
		// look for a new node to walk towards
		bool scout = true;
		if (_save->getMissionType() != "STR_BASE_DEFENSE")
		{
			// after turn 20 or if the morale is low, everyone moves out the UFO and scout
			// also anyone standing in fire should also probably move
			if (_save->isCheating() || !_fromNode || _fromNode->getRank() == 0 ||
				(_save->getTile(_unit->getPosition()) && _save->getTile(_unit->getPosition())->getFire()))
			{
				scout = true;
			}
			else
			{
				scout = false;
			}
		}

		// in base defense missions, the smaller aliens walk towards target nodes - or if there, shoot objects around them
		else if (_unit->getArmor()->getSize() == 1 && _unit->getOriginalFaction() == FACTION_HOSTILE &&
				_attackAction.weapon &&
				_attackAction.weapon->getRules()->getAccuracySnap() &&
				!_attackAction.weapon->getRules()->getArcingShot() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT) &&
				!_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getArcingShot() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getDamageType()->isDirect() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getDamageType()->ToTile > 0.01f)
		{
			// can i shoot an object?
			if (_fromNode->isTarget() &&
				_save->canUseWeapon(_attackAction.weapon, _unit, false, BA_SNAPSHOT) &&
				_save->getModuleMap()[_fromNode->getPosition().x / 10][_fromNode->getPosition().y / 10].second > 0)
			{
				// scan this room for objects to destroy
				int x = (_unit->getPosition().x/10)*10;
				int y = (_unit->getPosition().y/10)*10;
				for (int i = x; i < x+9; i++)
				for (int j = y; j < y+9; j++)
				{
					MapData *md = _save->getTile(Position(i, j, 1))->getMapData(O_OBJECT);
					if (md && md->isBaseModule())
					{
						_patrolAction.actor = _unit;
						_patrolAction.target = Position(i, j, 1);
						_patrolAction.weapon = _attackAction.weapon;
						_patrolAction.type = BA_SNAPSHOT;
						_patrolAction.updateTU();
						_foundBaseModuleToDestroy = _save->getMod()->getAIDestroyBaseFacilities();
						return;
					}
				}
			}
			else
			{
				// find closest high value target which is not already allocated
				int closest = 1000000;
				BattleUnit* nodeunit = nullptr;
				for (auto* node : *_save->getNodes())
				{
					if (node->isDummy())
					{
						continue;
					}

					nodeunit = _save->getTile(node->getPosition())->getUnit();
					if (nodeunit && nodeunit->getFaction() == _unit->getFaction())
					{
						continue;
					}

					if (node->isTarget() && !node->isAllocated() && _save->getModuleMap()[node->getPosition().x / 10][node->getPosition().y / 10].second > 0)
					{
						int d = Position::distanceSq(_unit->getPosition(), node->getPosition());
						if (!_toNode ||  (d < closest && node != _fromNode))
						{
							_toNode = node;
							closest = d;
						}
					}
				}
			}
		}

		if (_toNode == 0)
		{
			_toNode = _save->getPatrolNode(scout, _unit, _fromNode);
			if (_toNode == 0)
			{
				_toNode = _save->getPatrolNode(!scout, _unit, _fromNode);
			}
		}

		if (_toNode != 0)
		{
			_save->getPathfinding()->calculate(_unit, _toNode->getPosition(), BAM_NORMAL);
			if (_save->getPathfinding()->getStartDirection() == -1)
			{
				_toNode = 0;
			}
			_save->getPathfinding()->abortPath();
		}
	}

	if (_toNode != 0)
	{
		_toNode->allocateNode();
		_patrolAction.actor = _unit;
		_patrolAction.type = BA_WALK;
		_patrolAction.target = _toNode->getPosition();
		if (_unit->getFaction() == FACTION_PLAYER && _knownEnemies == 0)
		{
			const Position originalTarget = _patrolAction.target;
			const Position currentPos = _unit->getPosition();
			const int maxScoutDistance = _save->getTurn() <= 2 ? 4 : 5;
			int bestScore = -100000;
			Position bestPos = currentPos;
			auto allyCrowdingPenalty = [&](const Position &pos) -> int
			{
				int penalty = 0;
				for (auto *other : *_save->getUnits())
				{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction())
					{
						continue;
					}
					const int dist = Position::distance2d(pos, other->getPosition());
					if (dist <= 1)
					{
						penalty += 45;
					}
					else if (dist <= 2)
					{
						penalty += 18;
					}
				}
				return penalty;
			};
			for (auto tileIndex : _reachable)
			{
				Tile *tile = _save->getTile(tileIndex);
				if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
				{
					continue;
				}
				Position pos = tile->getPosition();
				if (pos.z != currentPos.z || Position::distance2d(pos, currentPos) > maxScoutDistance)
				{
					continue;
				}
				const int moveDist = Position::distance2d(pos, currentPos);
				if (_knownEnemies && moveDist * 6 > std::max(0, _unit->getTimeUnits() - 20))
				{
					continue;
				}
				if (getSpottingUnits(pos) > 0)
				{
					continue;
				}
				int cover = 0;
				if (tile->getMapData(O_OBJECT))
				{
					cover += 8;
				}
				if (tile->getMapData(O_NORTHWALL))
				{
					cover += 6;
				}
				if (tile->getMapData(O_WESTWALL))
				{
					cover += 6;
				}
				const int score = 200 - Position::distance2d(pos, originalTarget) * 4 - moveDist * 2 + cover * 4 - allyCrowdingPenalty(pos);
				if (score > bestScore)
				{
					bestScore = score;
					bestPos = pos;
				}
			}
			if (bestPos != currentPos)
			{
				_patrolAction.target = bestPos;
				freePatrolTarget();
				_toNode = 0;
				if (Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction cautious patrol: unit=" << _unit->getId()
						<< ", originalTarget=" << originalTarget
						<< ", limitedTarget=" << bestPos
						<< ", maxDistance=" << maxScoutDistance
						<< ", score=" << bestScore;
					_save->appendToAutoBattleLog(log.str());
				}
			}
		}
	}
	else
	{
		_patrolAction.type = BA_RETHINK;
	}
}

/**
 * Try to set up an ambush action
 * The idea is to check within a 11x11 tile square for a tile which is not seen by our aggroTarget,
 * but that can be reached by him. we then intuit where we will see the target first from our covered
 * position, and set that as our final facing.
 * Fills out the _ambushAction with useful data.
 */
void PlayerFactionAI::setupAmbush()
{
	_ambushAction.type = BA_RETHINK;
	int bestScore = 0;
	_ambushTUs = 0;
	std::vector<int> path;

	if (selectClosestKnownEnemy())
	{
		const int BASE_SYSTEMATIC_SUCCESS = 100;
		const int COVER_BONUS = 25;
		const int FAST_PASS_THRESHOLD = 80;
		Position origin = _save->getTileEngine()->getSightOriginVoxel(_aggroTarget);

		// we'll use node positions for this, as it gives map makers a good degree of control over how the units will use the environment.
		for (const auto* node : *_save->getNodes())
		{
			if (node->isDummy())
			{
				continue;
			}
			Position pos = node->getPosition();
			Tile *tile = _save->getTile(pos);
			if (tile == 0 || Position::distance2d(pos, _unit->getPosition()) > 10 || pos.z != _unit->getPosition().z || tile->getDangerous() ||
				std::find(_reachableWithAttack.begin(), _reachableWithAttack.end(), _save->getTileIndex(pos))  == _reachableWithAttack.end())
				continue; // just ignore unreachable tiles

			if (_traceAI)
			{
				// colour all the nodes in range purple.
				tile->setPreview(10);
				tile->setMarkerColor(13);
			}

			// make sure we can't be seen here.
			Position target;
			if (!_save->getTileEngine()->canTargetUnit(&origin, tile, &target, _aggroTarget, false, _unit) && !getSpottingUnits(pos))
			{
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				int ambushTUs = _save->getPathfinding()->getTotalTUCost();
				// make sure we can move here
				if (_save->getPathfinding()->getStartDirection() != -1)
				{
					int score = BASE_SYSTEMATIC_SUCCESS;
					score -= ambushTUs;

					// make sure our enemy can reach here too.
					_save->getPathfinding()->calculate(_aggroTarget, pos, BAM_NORMAL);

					if (_save->getPathfinding()->getStartDirection() != -1)
					{
						// ideally we'd like to be behind some cover, like say a window or a low wall.
						if (_save->getTileEngine()->faceWindow(pos) != -1)
						{
							score += COVER_BONUS;
						}
						if (score > bestScore)
						{
							path = _save->getPathfinding()->copyPath();
							bestScore = score;
							_ambushTUs = (pos == _unit->getPosition()) ? 1 : ambushTUs;
							_ambushAction.target = pos;
							if (bestScore > FAST_PASS_THRESHOLD)
							{
								break;
							}
						}
					}
				}
			}
		}

		if (bestScore > 0)
		{
			_ambushAction.type = BA_WALK;
			// i should really make a function for this
			origin = _ambushAction.target.toVoxel() +
				// 4 because -2 is eyes and 2 below that is the rifle (or at least that's my understanding)
				Position(8,8, _unit->getHeight() + _unit->getFloatHeight() - _save->getTile(_ambushAction.target)->getTerrainLevel() - 4);
			Position currentPos = _aggroTarget->getPosition();
			_save->getPathfinding()->setUnit(_aggroTarget);
			size_t tries = path.size();
			// hypothetically walk the target through the path.
			while (tries > 0)
			{
				currentPos = _save->getPathfinding()->getTUCost(currentPos, path.back(), _aggroTarget, 0, BAM_NORMAL).pos;
				path.pop_back();
				Tile *tile = _save->getTile(currentPos);
				Position target;
				// do a virtual fire calculation
				if (_save->getTileEngine()->canTargetUnit(&origin, tile, &target, _unit, false, _aggroTarget))
				{
					// if we can virtually fire at the hypothetical target, we know which way to face.
					_ambushAction.finalFacing = _save->getTileEngine()->getDirectionTo(_ambushAction.target, currentPos);
					break;
				}
				--tries;
			}
			if (_traceAI)
			{
				Log(LOG_INFO) << "Ambush estimation will move to " << _ambushAction.target;
			}
			return;
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Ambush estimation failed";
	}
}

/**
 * Try to set up a combat action
 * This will either be a psionic, grenade, or weapon attack,
 * or potentially just moving to get a line of sight to a target.
 * Fills out the _attackAction with useful data.
 */
void PlayerFactionAI::setupAttack()
{
	_attackAction.type = BA_RETHINK;
	_psiAction.type = BA_NONE;

	bool sniperAttack = false;

	// if enemies are known to us but not necessarily visible, we can attack them with a blaster launcher or psi or a sniper attack.
	if (_knownEnemies)
	{
		if (psiAction())
		{
			// at this point we can save some time with other calculations - the unit WILL make a psionic attack this turn.
			return;
		}
		if (_blaster)
		{
			wayPointAction();
		}
		else if (_unit->getUnitRules()) // xcom soldiers (under mind control) lack unit rules!
		{
			// don't always act on spotter information unless modder says so
			if (RNG::percent(_unit->getUnitRules()->getSniperPercentage()))
			{
				sniperAttack = sniperAction();
			}
		}
	}

	// if we CAN see someone, that makes them a viable target for "regular" attacks.
	// This is skipped if sniperAction has already chosen an attack action
	if (!sniperAttack && selectNearestTarget())
	{
		// if we have both types of weapon, make a determination on which to use.
		if (_melee && _rifle)
		{
			selectMeleeOrRanged();
		}
		if (_grenade)
		{
			grenadeAction();
		}
		if (_melee)
		{
			meleeAction();
		}
		if (_rifle)
		{
			projectileAction();
			if (_unit->getFaction() == FACTION_PLAYER && _attackAction.type == BA_RETHINK && _aggroTarget)
			{
				setupCleanShotMove(_aggroTarget);
			}
		}
	}
	else if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
	{
		BattleUnit *assignedTarget = _factionAI->getAssignedTarget(_unit);
		if (assignedTarget && !assignedTarget->isOut() && _rifle)
		{
			_aggroTarget = assignedTarget;
			if (!_attackAction.weapon)
			{
				_attackAction.weapon = selectBestCarriedWeapon();
			}
			setupCleanShotMove(assignedTarget);
		}
	}

	if (_attackAction.type != BA_RETHINK)
	{
		if (_traceAI)
		{
			if (_attackAction.type != BA_WALK)
			{
				Log(LOG_INFO) << "Attack estimation desires to shoot at " << _attackAction.target;
			}
			else
			{
				Log(LOG_INFO) << "Attack estimation desires to move to " << _attackAction.target;
			}
		}
		return;
	}
	else if (_spottingEnemies || _unit->getAggression() < RNG::generate(0, 3))
	{
		// if enemies can see us, or if we're feeling lucky, we can try to spot the enemy.
		if (findFirePoint())
		{
			if (_traceAI)
			{
				Log(LOG_INFO) << "Attack estimation desires to move to " << _attackAction.target;
			}
			return;
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Attack estimation failed";
	}
}

bool PlayerFactionAI::setupFactionStalkAmbush(const Position &contactPos, const BattleRoomInfo *contactRoom, int enemiesInRoom, BattleItem *weapon)
{
	if (!_factionAI || _reachable.empty())
	{
		return false;
	}
	const int currentDist = Position::distance2d(_unit->getPosition(), contactPos);
	const int roomSize = contactRoom ? contactRoom->tileCount : 1;
	const bool smallDangerRoom = contactRoom && !contactRoom->isOutside && !contactRoom->isHall;
	const bool riskyRoom = smallDangerRoom && (enemiesInRoom > 1 || contactRoom->tileCount > 16 || contactRoom->doorCount + contactRoom->windowCount <= 2);
	const bool rangedAmbush = _rifle || _blaster || (_grenade && !_melee);
	const int minDoorDist = rangedAmbush ? 2 : 0;
	const int desiredDist = riskyRoom ? (rangedAmbush ? 3 : 1) : (contactRoom && contactRoom->isHall ? 6 : (enemiesInRoom > 1 ? 7 : 5));
	const int roomPressure = enemiesInRoom * 20 + (roomSize > 60 ? 10 : 0);
	if (!riskyRoom && currentDist <= desiredDist + 5)
	{
		return false;
	}
	int bestScore = -100000;
	Position bestPos = _unit->getPosition();
	Position bestFace = contactPos;

	auto nearestEntryTo = [&](const Position &pos, Position *entry) -> int
	{
		int bestDist = Position::distance2d(pos, contactPos);
		Position bestEntry = contactPos;
		if (contactRoom)
		{
			for (const auto &candidate : contactRoom->entryPositions)
			{
				if (candidate.z != pos.z)
				{
					continue;
				}
				const int dist = Position::distance2d(pos, candidate);
				if (dist < bestDist)
				{
					bestDist = dist;
					bestEntry = candidate;
				}
			}
		}
		if (entry)
		{
			*entry = bestEntry;
		}
		return bestDist;
	};

	auto coverScoreAt = [&](const Position &pos) -> int
	{
		int score = 0;
		Tile *tile = _save->getTile(pos);
		if (!tile)
		{
			return 0;
		}
		if (tile->getMapData(O_OBJECT))
		{
			score += 8;
		}
		if (tile->getMapData(O_NORTHWALL))
		{
			score += contactPos.y < pos.y ? 16 : 6;
		}
		if (tile->getMapData(O_WESTWALL))
		{
			score += contactPos.x < pos.x ? 16 : 6;
		}
		const Position adjacent[4] = { Position(0, -1, 0), Position(1, 0, 0), Position(0, 1, 0), Position(-1, 0, 0) };
		for (const auto &offset : adjacent)
		{
			Tile *adjacentTile = _save->getTile(pos + offset);
			if (!adjacentTile)
			{
				continue;
			}
			if (adjacentTile->getMapData(O_OBJECT))
			{
				score += 4;
			}
			if (adjacentTile->getMapData(O_NORTHWALL) || adjacentTile->getMapData(O_WESTWALL))
			{
				score += 3;
			}
		}
		return score;
	};

	for (auto tileIndex : _reachable)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		Position pos = tile->getPosition();
		if (pos.z != _unit->getPosition().z)
		{
			continue;
		}
		if (riskyRoom && contactRoom && _factionAI->getRoomIdAt(pos) == contactRoom->id)
		{
			continue;
		}
		int dist = Position::distance2d(pos, contactPos);
		Position entryPos;
		const int entryDist = nearestEntryTo(pos, &entryPos);
		const int currentEntryDist = nearestEntryTo(_unit->getPosition(), 0);
		if (riskyRoom)
		{
			if (entryDist < minDoorDist || entryDist > 7 || entryDist > currentEntryDist + 2)
			{
				continue;
			}
		}
		else if (dist < 2 || dist >= currentDist || dist > desiredDist + 8)
		{
			continue;
		}
		int spotters = getSpottingUnits(pos);
		int cover = coverScoreAt(pos);
		if (cover < (riskyRoom ? 12 : 20))
		{
			continue;
		}
		int score = 100;
		score -= abs((riskyRoom ? entryDist : dist) - desiredDist) * 8;
		score -= spotters * (35 + roomPressure);
		score += cover;
		if (riskyRoom)
		{
			score += std::max(0, 45 - entryDist * 8);
			score += std::min(35, (currentEntryDist - entryDist) * 4);
			if (contactRoom && contactRoom->entryPositions.size() > 1)
			{
				score += 15;
			}
		}
		else if (dist < currentDist)
		{
			score += std::min(30, (currentDist - dist) * 3);
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestFace = riskyRoom ? (pos == entryPos ? contactPos : entryPos) : contactPos;
		}
	}

	if (bestScore < (riskyRoom ? 115 : 155) || bestPos == _unit->getPosition())
	{
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = weapon;
	_attackAction.target = bestPos;
	_attackAction.type = BA_WALK;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, bestFace);
	_stalkAmbushAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction stalk ambush: unit=" << _unit->getId()
			<< ", contact=" << contactPos
			<< ", target=" << bestPos
			<< ", score=" << bestScore
			<< ", roomEnemies=" << enemiesInRoom
			<< ", roomSize=" << roomSize
			<< ", roomDoors=" << (contactRoom ? contactRoom->doorCount : 0)
			<< ", roomWindows=" << (contactRoom ? contactRoom->windowCount : 0)
			<< ", roomEntries=" << (contactRoom ? contactRoom->entryPositions.size() : 0)
			<< ", roomHall=" << (contactRoom ? contactRoom->isHall : false)
			<< ", minDoorDist=" << minDoorDist
			<< ", riskyRoom=" << riskyRoom
			<< ", facing=" << bestFace;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Attempts to find cover, and move toward it.
 * The idea is to check within a 11x11 tile square for a tile which is not seen by our aggroTarget.
 * If there is no such tile, we run away from the target.
 * Fills out the _escapeAction with useful data.
 */
void PlayerFactionAI::setupEscape()
{
	int unitsSpottingMe = getSpottingUnits(_unit->getPosition());
	int currentTilePreference = 15;
	int tries = -1;
	bool coverFound = false;
	selectNearestTarget();
	_escapeTUs = 0;

	int dist = _aggroTarget ? Position::distance2d(_unit->getPosition(), _aggroTarget->getPosition()) : 0;

	int bestTileScore = -100000;
	int score = -100000;
	Position bestTile(0, 0, 0);
	bool run = false;

	Tile *tile = 0;

	// weights of various factors in choosing a tile to which to withdraw
	const int EXPOSURE_PENALTY = 10;
	const int FIRE_PENALTY = 40;
	const int BASE_SYSTEMATIC_SUCCESS = 100;
	const int BASE_DESPERATE_SUCCESS = 110;
	const int FAST_PASS_THRESHOLD = 100; // a score that's good enough to quit the while loop early; it's subjective, hand-tuned and may need tweaking

	std::vector<Position> randomTileSearch = _save->getTileSearch();
	RNG::shuffle(randomTileSearch);

	while (tries < 150 && !coverFound)
	{
		_escapeAction.target = _unit->getPosition(); // start looking in a direction away from the enemy
		_escapeAction.run = _unit->getArmor()->allowsRunning(false) && (tries & 1); // every odd try, i.e. roughly 50%

		if (!_save->getTile(_escapeAction.target))
		{
			_escapeAction.target = _unit->getPosition(); // cornered at the edge of the map perhaps?
		}

		score = 0;

		if (tries == -1)
		{
			// you know, maybe we should just stay where we are and not risk reaction fire...
			// or maybe continue to wherever we were running to and not risk looking stupid
			if (_save->getTile(_unit->lastCover) != 0)
			{
				_escapeAction.target = _unit->lastCover;
			}
		}
		else if (tries < 121)
		{
			// looking for cover
			_escapeAction.target.x += randomTileSearch[tries].x;
			_escapeAction.target.y += randomTileSearch[tries].y;
			score = BASE_SYSTEMATIC_SUCCESS;
			if (_escapeAction.target == _unit->getPosition())
			{
				if (unitsSpottingMe > 0)
				{
					// maybe don't stay in the same spot? move or something if there's any point to it?
					_escapeAction.target.x += RNG::generate(-20,20);
					_escapeAction.target.y += RNG::generate(-20,20);
				}
				else
				{
					score += currentTilePreference;
				}
			}
		}
		else
		{
			if (tries == 121)
			{
				if (_traceAI)
				{
					Log(LOG_INFO) << "best score after systematic search was: " << bestTileScore;
				}
			}

			score = BASE_DESPERATE_SUCCESS; // ruuuuuuun
			_escapeAction.target = _unit->getPosition();
			_escapeAction.target.x += RNG::generate(-10,10);
			_escapeAction.target.y += RNG::generate(-10,10);
			_escapeAction.target.z = _unit->getPosition().z + RNG::generate(-1,1);
			if (_escapeAction.target.z < 0)
			{
				_escapeAction.target.z = 0;
			}
			else if (_escapeAction.target.z >= _save->getMapSizeZ())
			{
				_escapeAction.target.z = _unit->getPosition().z;
			}
		}

		tries++;

		// THINK, DAMN YOU
		tile = _save->getTile(_escapeAction.target);
		int distanceFromTarget = _aggroTarget ? Position::distance2d(_aggroTarget->getPosition(), _escapeAction.target) : 0;
		if (dist >= distanceFromTarget)
		{
			score -= (distanceFromTarget - dist) * 10;
		}
		else
		{
			score += (distanceFromTarget - dist) * 10;
		}
		int spotters = 0;
		if (!tile)
		{
			score = -100001; // no you can't quit the battlefield by running off the map.
		}
		else
		{
			spotters = getSpottingUnits(_escapeAction.target);
			if (std::find(_reachable.begin(), _reachable.end(), _save->getTileIndex(_escapeAction.target))  == _reachable.end())
				continue; // just ignore unreachable tiles

			if (_spottingEnemies || spotters)
			{
				if (_spottingEnemies <= spotters)
				{
					score -= (1 + spotters - _spottingEnemies) * EXPOSURE_PENALTY; // that's for giving away our position
				}
				else
				{
					score += (_spottingEnemies - spotters) * EXPOSURE_PENALTY;
				}
			}
			if (tile->getFire())
			{
				score -= FIRE_PENALTY;
			}
			if (tile->getDangerous())
			{
				score -= BASE_SYSTEMATIC_SUCCESS;
			}

			if (_traceAI)
			{
				tile->setMarkerColor(score < 0 ? 3 : (score < FAST_PASS_THRESHOLD/2 ? 8 : (score < FAST_PASS_THRESHOLD ? 9 : 5)));
				tile->setPreview(10);
				tile->setTUMarker(score);
			}

		}

		if (tile && score > bestTileScore)
		{
			// calculate TUs to tile; we could be getting this from findReachable() somehow but that would break something for sure...
			_save->getPathfinding()->calculate(_unit, _escapeAction.target, _escapeAction.getMoveType());
			if (_escapeAction.target == _unit->getPosition() || _save->getPathfinding()->getStartDirection() != -1)
			{
				bestTileScore = score;
				bestTile = _escapeAction.target;
				run = _escapeAction.run;
				_escapeTUs = _save->getPathfinding()->getTotalTUCost();
				if (_escapeAction.target == _unit->getPosition())
				{
					_escapeTUs = 1;
				}
				if (_traceAI)
				{
					tile->setMarkerColor(score < 0 ? 7 : (score < FAST_PASS_THRESHOLD/2 ? 10 : (score < FAST_PASS_THRESHOLD ? 4 : 5)));
					tile->setPreview(10);
					tile->setTUMarker(score);
				}
			}
			_save->getPathfinding()->abortPath();
			if (bestTileScore > FAST_PASS_THRESHOLD) coverFound = true; // good enough, gogogo
		}
	}
	_escapeAction.target = bestTile;
	_escapeAction.run = run;
	if (_traceAI)
	{
		_save->getTile(_escapeAction.target)->setMarkerColor(13);
	}

	if (bestTileScore <= -100000)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Escape estimation failed.";
		}
		_escapeAction.type = BA_RETHINK; // do something, just don't look dumbstruck :P
		return;
	}
	else
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Escape estimation completed after " << tries << " tries, " << Position::distance2d(_unit->getPosition(), bestTile) << " squares or so away.";
		}
		_escapeAction.type = BA_WALK;
	}
}

/**
 * Counts how many targets, both xcom and civilian are known to this unit
 * @return how many targets are known to us.
 */
int PlayerFactionAI::countKnownTargets() const
{
	int knownEnemies = 0;

	if (_unit->getFaction() == FACTION_PLAYER && _factionAI)
	{
		return _factionAI->getEnemyContactCount();
	}

	if (_unit->getFaction() == FACTION_HOSTILE)
	{
		for (auto* bu : *_save->getUnits())
		{
			if (validTarget(bu, true, true))
			{
				++knownEnemies;
			}
		}
	}
	return knownEnemies;
}

/*
 * counts how many enemies (xcom only) are spotting any given position.
 * @param pos the Position to check for spotters.
 * @return spotters.
 */
int PlayerFactionAI::getSpottingUnits(const Position& pos) const
{
	// if we don't actually occupy the position being checked, we need to do a virtual LOF check.
	bool checking = pos != _unit->getPosition();
	int tally = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, false, false))
		{
			int dist = Position::distance2d(pos, bu->getPosition());
			if (dist > 20) continue;
			Position originVoxel = _save->getTileEngine()->getSightOriginVoxel(bu);
			originVoxel.z -= 2;
			Position targetVoxel;
			if (checking)
			{
				if (_save->getTileEngine()->canTargetUnit(&originVoxel, _save->getTile(pos), &targetVoxel, bu, false, _unit))
				{
					tally++;
				}
			}
			else
			{
				if (_save->getTileEngine()->canTargetUnit(&originVoxel, _save->getTile(pos), &targetVoxel, bu, false))
				{
					tally++;
				}
			}
		}
	}
	return tally;
}

int PlayerFactionAI::getEnemyFireExposure(const Position& pos) const
{
	Tile *tile = _save->getTile(pos);
	if (!tile)
	{
		return 1000;
	}
	int exposure = 0;
	for (auto *enemy : *_save->getUnits())
	{
		if (!enemy || enemy->isOut() || enemy->getFaction() == _unit->getFaction())
		{
			continue;
		}
		BattleItem *weapon = enemy->getMainHandWeapon(false);
		if (!weapon || weapon->getRules()->getBattleType() != BT_FIREARM || !_save->canUseWeapon(weapon, enemy, false, BA_SNAPSHOT))
		{
			continue;
		}
		const int dist = Position::distance2d(pos, enemy->getPosition());
		if (dist > 24)
		{
			continue;
		}
		BattleAction action;
		action.actor = enemy;
		action.weapon = weapon;
		action.target = pos;
		Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
		Position targetVoxel;
		if (_save->getTileEngine()->canTargetUnit(&origin, tile, &targetVoxel, enemy, false))
		{
			exposure += 30 + std::max(0, weapon->getRules()->getPower()) / 3 + std::max(0, weapon->getRules()->getAccuracySnap()) / 5;
		}
	}
	return exposure;
}

int PlayerFactionAI::scoreTargetPriority(BattleUnit *target, bool assigned, bool visible, int distance) const
{
	if (!target)
	{
		return -100000;
	}
	int score = 60;
	score -= distance * 4;
	if (assigned)
	{
		score += visible ? 120 : 35;
	}
	if (visible)
	{
		score += 45;
	}
	if (target->getHealth() > 0)
	{
		score += std::max(0, 80 - target->getHealth()) * 2;
		if (target->getHealth() <= 35)
		{
			score += 120;
		}
	}
	score += target->getFatalWounds() * 20;
	score += std::max(0, target->getTimeUnits()) / 2;
	BattleItem *enemyWeapon = target->getMainHandWeapon(false);
	if (enemyWeapon && enemyWeapon->getRules())
	{
		const RuleItem *rule = enemyWeapon->getRules();
		score += std::max(0, rule->getPower()) / 2;
		score += std::max(std::max(rule->getAccuracySnap(), rule->getAccuracyAimed()), rule->getAccuracyAuto()) / 3;
		if (rule->getBattleType() == BT_MELEE)
		{
			score += distance <= 5 ? 90 : 25;
		}
	}
	if (_attackAction.weapon)
	{
		const int preferred = getPreferredEngagementRange(_attackAction.weapon);
		score -= abs(distance - preferred) * 3;
	}
	return score;
}

/**
 * Selects the nearest known living target we can see/reach and returns the number of visible enemies.
 * This function includes civilians as viable targets.
 * @return viable targets.
 */
int PlayerFactionAI::selectNearestTarget()
{
	int tally = 0;
	_closestDist= 100;
	_aggroTarget = 0;
	Position target;
	BattleUnit *assignedTarget = _factionAI ? _factionAI->getAssignedTarget(_unit) : 0;
	int bestTargetScore = -100000;
	for (auto* bu : *_save->getUnits())
	{
		const bool assigned = assignedTarget == bu;
		const bool visible = bu && bu->getTile() && _save->getTileEngine()->visible(_unit, bu->getTile());
		if (validTarget(bu, true, true) && (visible || assigned))
		{
			if (visible)
			{
				tally++;
			}
			int dist = Position::distance2d(_unit->getPosition(), bu->getPosition());
			{
				bool valid = false;
				if (_rifle || !_melee)
				{
					BattleAction action;
					action.actor = _unit;
					action.weapon = _attackAction.weapon;
					action.target = bu->getPosition();
					Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
					valid = _save->getTileEngine()->canTargetUnit(&origin, bu->getTile(), &target, _unit, false);
				}
				else
				{
					if (selectPointNearTarget(bu, _unit->getTimeUnits()))
					{
						int dir = _save->getTileEngine()->getDirectionTo(_attackAction.target, bu->getPosition());
						valid = _save->getTileEngine()->validMeleeRange(_attackAction.target, dir, _unit, bu, 0);
					}
				}
				if (valid)
				{
					const int targetScore = scoreTargetPriority(bu, assigned, visible, dist);
					if (targetScore > bestTargetScore || (targetScore == bestTargetScore && dist < _closestDist))
					{
						bestTargetScore = targetScore;
						_closestDist = dist;
						_aggroTarget = bu;
					}
				}
			}
		}
	}
	if (_aggroTarget)
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction target accepted: unit=" << _unit->getId()
				<< ", target=" << _aggroTarget->getId()
				<< ", score=" << bestTargetScore
				<< ", distance=" << _closestDist
				<< ", assigned=" << (assignedTarget == _aggroTarget)
				<< ", reason=" << (_factionAI ? _factionAI->getAssignmentReason(_unit) : "");
			_save->appendToAutoBattleLog(log.str());
		}
		return std::max(tally, 1);
	}

	return 0;
}

/**
 * Selects the nearest known living target we can see/reach and returns the number of visible enemies.
 * This function includes civilians as viable targets.
 * Note: Differs from selectNearestTarget() in calling selectPointNearTargetLeeroy().
 * @return viable targets.
 */
int PlayerFactionAI::selectNearestTargetLeeroy(bool canRun)
{
	int tally = 0;
	_closestDist = 100;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, true) &&
			_save->getTileEngine()->visible(_unit, bu->getTile()))
		{
			tally++;
			int dist = Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist < _closestDist)
			{
				bool valid = false;
				if (selectPointNearTargetLeeroy(bu, canRun))
				{
					int dir = _save->getTileEngine()->getDirectionTo(_attackAction.target, bu->getPosition());
					valid = _save->getTileEngine()->validMeleeRange(_attackAction.target, dir, _unit, bu, 0);
				}
				if (valid)
				{
					_closestDist = dist;
					_aggroTarget = bu;
				}
			}
		}
	}
	if (_aggroTarget)
	{
		return tally;
	}

	return 0;
}

/**
 * Selects the nearest known living Xcom unit.
 * used for ambush calculations
 * @return if we found one.
 */
bool PlayerFactionAI::selectClosestKnownEnemy()
{
	_aggroTarget = 0;
	int minDist = 255;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, false))
		{
			int dist = Position::distance2d(bu->getPosition(), _unit->getPosition());
			if (dist < minDist)
			{
				minDist = dist;
				_aggroTarget = bu;
			}
		}
	}
	return _aggroTarget != 0;
}

/**
 * Selects a random known living Xcom or civilian unit.
 * @return if we found one.
 */
bool PlayerFactionAI::selectRandomTarget()
{
	int farthest = -100;
	_aggroTarget = 0;

	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, true))
		{
			int dist = RNG::generate(0,20) - Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist > farthest)
			{
				farthest = dist;
				_aggroTarget = bu;
			}
		}
	}
	return _aggroTarget != 0;
}

/**
 * Selects a point near enough to our target to perform a melee attack.
 * @param target Pointer to a target.
 * @param maxTUs Maximum time units the path to the target can cost.
 * @return True if a point was found.
 */
bool PlayerFactionAI::selectPointNearTarget(BattleUnit *target, int maxTUs)
{
	int size = _unit->getArmor()->getSize();
	int sizeTarget = target->getArmor()->getSize();
	int dirTarget = target->getDirection();
	float dodgeChanceDiff = target->getArmor()->getMeleeDodge(target) * target->getArmor()->getMeleeDodgeBackPenalty() * _attackAction.diff / 160.0f;
	bool returnValue = false;
	int distance = 1000;
	for (int z = -1; z <= 1; ++z)
	{
		for (int x = -size; x <= sizeTarget; ++x)
		{
			for (int y = -size; y <= sizeTarget; ++y)
			{
				if (x || y) // skip the unit itself
				{
					Position checkPath = target->getPosition() + Position (x, y, z);
					if (_save->getTile(checkPath) == 0 || std::find(_reachable.begin(), _reachable.end(), _save->getTileIndex(checkPath))  == _reachable.end())
						continue;
					int dir = _save->getTileEngine()->getDirectionTo(checkPath, target->getPosition());
					bool valid = _save->getTileEngine()->validMeleeRange(checkPath, dir, _unit, target, 0);
					bool fitHere = _save->setUnitPosition(_unit, checkPath, true);

					if (valid && fitHere && !_save->getTile(checkPath)->getDangerous())
					{
						_save->getPathfinding()->calculate(_unit, checkPath, BAM_NORMAL, 0, maxTUs);

						//for 100% dodge diff and on 4th difficulty it will allow aliens to move 10 squares around to made attack from behind.
						int distanceCurrent = _save->getPathfinding()->getPath().size() - dodgeChanceDiff * _save->getTileEngine()->getArcDirection(dir - 4, dirTarget);
						if (_save->getPathfinding()->getStartDirection() != -1 && distanceCurrent < distance)
						{
							_attackAction.target = checkPath;
							returnValue = true;
							distance = distanceCurrent;
						}
						_save->getPathfinding()->abortPath();
					}
				}
			}
		}
	}
	return returnValue;
}

/**
 * Selects a point near enough to our target to perform a melee attack.
 * Note: Differs from selectPointNearTarget() in that it doesn't consider:
 *  - remaining TUs (charge even if not enough TUs to attack)
 *  - dangerous tiles (grenades? pfff!)
 *  - melee dodge (not intelligent enough to attack from behind)
 * @param target Pointer to a target.
 * @return True if a point was found.
 */
bool PlayerFactionAI::selectPointNearTargetLeeroy(BattleUnit *target, bool canRun)
{
	int size = _unit->getArmor()->getSize();
	int targetsize = target->getArmor()->getSize();
	bool returnValue = false;
	unsigned int distance = 1000;
	for (int z = -1; z <= 1; ++z)
	{
		for (int x = -size; x <= targetsize; ++x)
		{
			for (int y = -size; y <= targetsize; ++y)
			{
				if (x || y) // skip the unit itself
				{
					Position checkPath = target->getPosition() + Position(x, y, z);
					if (_save->getTile(checkPath) == 0)
						continue;
					int dir = _save->getTileEngine()->getDirectionTo(checkPath, target->getPosition());
					bool valid = _save->getTileEngine()->validMeleeRange(checkPath, dir, _unit, target, 0);
					bool fitHere = _save->setUnitPosition(_unit, checkPath, true);

					if (valid && fitHere)
					{
						_save->getPathfinding()->calculate(_unit, checkPath, canRun ? BAM_RUN : BAM_NORMAL, 0, 100000); // disregard unit's TUs.
						if (_save->getPathfinding()->getStartDirection() != -1 && _save->getPathfinding()->getPath().size() < distance)
						{
							_attackAction.target = checkPath;
							returnValue = true;
							distance = _save->getPathfinding()->getPath().size();
						}
						_save->getPathfinding()->abortPath();
					}
				}
			}
		}
	}
	return returnValue;
}

/**
 * Selects a target from a list of units seen by spotter units for out-of-LOS actions and populates _attackAction with the relevant data
 * @return True if we have a target selected
 */
bool PlayerFactionAI::selectSpottedUnitForSniper()
{
	_aggroTarget = 0;

	// Create a list of spotted targets and the type of attack we'd like to use on each
	std::vector<std::pair<BattleUnit*, BattleAction>> spottedTargets;

	// Get the TU costs for each available attack type
	BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, _attackAction.weapon);

	BattleActionCost costThrow;
	// Only want to check throwing if we have a grenade, the default constructor (line above) conveniently returns false from haveTU()
	if (_grenade)
	{
		// We know we have a grenade, now we need to know if we have the TUs to throw it
		costThrow.type = BA_THROW;
		costThrow.actor = _attackAction.actor;
		costThrow.weapon = _unit->getGrenadeFromBelt(_save);
		costThrow.updateTU();
		costThrow.Time += 4; // Vanilla TUs for AI picking up grenade from belt
		costThrow += _attackAction.actor->getActionTUs(BA_PRIME, costThrow.weapon);
	}

	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, true) && bu->getTurnsLeftSpottedForSnipersByFaction(_unit->getFaction()))
		{
			// Determine which firing mode to use based on how many hits we expect per turn and the unit's intelligence/aggression
			_aggroTarget = bu;
			_attackAction.type = BA_RETHINK;
			_attackAction.target = bu->getPosition();
			extendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, true);

			BattleAction chosenAction = _attackAction;
			if (chosenAction.type == BA_THROW)
				chosenAction.weapon = costThrow.weapon;

			if (_attackAction.type != BA_RETHINK)
			{
				std::pair<BattleUnit*, BattleAction> spottedTarget;
				spottedTarget = std::make_pair(bu, chosenAction);
				spottedTargets.push_back(spottedTarget);
			}
		}
	}

	int numberOfTargets = static_cast<int>(spottedTargets.size());

	if (numberOfTargets) // Now that we have a list of valid targets, pick one and return.
	{
		int pick = RNG::generate(0, numberOfTargets - 1);
		_aggroTarget = spottedTargets.at(pick).first;
		_attackAction.target = _aggroTarget->getPosition();
		_attackAction.type = spottedTargets.at(pick).second.type;
		_attackAction.weapon = spottedTargets.at(pick).second.weapon;
	}
	else // We didn't find a suitable target
	{
		// Make sure we reset anything we might have changed while testing for targets
		_aggroTarget = 0;
		_attackAction.type = BA_RETHINK;
		_attackAction.weapon = _unit->getMainHandWeapon(false);
	}

	return _aggroTarget != 0;
}

/**
 * Scores a firing mode for a particular target based on a accuracy / TUs ratio
 * @param action Pointer to the BattleAction determining the firing mode
 * @param target Pointer to the BattleUnit we're trying to target
 * @param checkLOF Set to true if you want to check for a valid line of fire
 * @return The calculated score
 */
int PlayerFactionAI::scoreFiringMode(BattleAction *action, BattleUnit *target, bool checkLOF)
{
	// Sanity check first, if the passed action has no type or weapon, return 0.
	if (!action->type || !action->weapon)
	{
		return 0;
	}
	auto* weapon = action->weapon->getRules();

	// Get base accuracy for the action
	int accuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(*action), _save->getMod());
	int distanceSq = _unit->distance3dToUnitSq(target);
	int distance = (int)std::ceil(sqrt(float(distanceSq)));

	{
		int upperLimit, lowerLimit;
		int dropoff = weapon->calculateLimits(upperLimit, lowerLimit, _save->getDepth(), action->type);

		if (distance > upperLimit)
		{
			accuracy -= (distance - upperLimit) * dropoff;
		}
		else if (distance < lowerLimit)
		{
			accuracy -= (lowerLimit - distance) * dropoff;
		}
	}

	bool outOfRange = action->type == BA_THROW
		? weapon->isOutOfThrowRange(distanceSq, _save->getDepth())
		: weapon->isOutOfRange(distanceSq);

	if (outOfRange)
	{
		accuracy = 0;
	}

	if (accuracy > 0 && directProjectileRiskyForAllies(action, target))
	{
		return 0;
	}
	if (accuracy > 0 && autoShotRiskyForAllies(action, target))
	{
		return 0;
	}

	int roleScoreModifier = 100;
	if (_unit->getFaction() == FACTION_PLAYER)
	{
		const PlayerAIRole role = getPlayerAIRole(action->weapon);
		const int preferred = getPreferredEngagementRange(action->weapon);
		const int rangeDelta = abs(distance - preferred);
		roleScoreModifier -= rangeDelta * (role == ROLE_MARKSMAN || role == ROLE_HEAVY ? 3 : 2);
		if (role == ROLE_MARKSMAN && action->type == BA_AIMEDSHOT)
		{
			roleScoreModifier += 18;
		}
		else if (role == ROLE_ASSAULT && action->type == BA_AUTOSHOT && distance <= preferred)
		{
			roleScoreModifier += 12;
		}
		else if (role == ROLE_SUPPORT && distance > preferred + 3)
		{
			roleScoreModifier -= 18;
		}
		if (action->type == BA_AUTOSHOT && distance <= preferred + 1)
		{
			roleScoreModifier += 22;
		}
		if (action->type == BA_SNAPSHOT && target && target->getHealth() > 0 && target->getHealth() <= 35)
		{
			roleScoreModifier += 18;
		}
		if (action->type == BA_AUTOSHOT && distance > preferred + 2)
		{
			roleScoreModifier -= 25;
		}
		roleScoreModifier = Clamp(roleScoreModifier, 35, 140);
	}

	int numberOfShots = 1;
	if (action->type == BA_AIMEDSHOT)
	{
		numberOfShots = weapon->getConfigAimed()->shots;
	}
	else if (action->type == BA_SNAPSHOT)
	{
		numberOfShots = weapon->getConfigSnap()->shots;
	}
	else if (action->type == BA_AUTOSHOT)
	{
		numberOfShots = weapon->getConfigAuto()->shots;
	}

	int tuCost = _unit->getActionTUs(action->type, action->weapon).Time;
	// Need to include TU cost of getting grenade from belt + priming if we're checking throwing
	if (action->type == BA_THROW && _grenade)
	{
		// FIXME: why not just use action->weapon ?
		auto* grenadeItem = _unit->getGrenadeFromBelt(_save);
		tuCost = _unit->getActionTUs(action->type, grenadeItem).Time;
		tuCost += 4;
		tuCost += _unit->getActionTUs(BA_PRIME, grenadeItem).Time;
	}
	int tuTotal = _unit->getBaseStats()->tu;

	// Return a score of zero if this firing mode doesn't exist for this weapon
	if (!tuCost)
	{
		return 0;
	}

	if (checkLOF)
	{
		Position origin = _save->getTileEngine()->getOriginVoxel((*action), 0);
		Position targetPosition;

		if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
		{
			targetPosition = target->getPosition().toVoxel() + Position (8,8, (2 + -target->getTile()->getTerrainLevel()));
			if (!_save->getTileEngine()->validateThrow((*action), origin, targetPosition, _save->getDepth()))
			{
				return 0;
			}
		}
		else
		{
			if (!_save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetPosition, _unit, false, target))
			{
				return 0;
			}
		}
	}

	return accuracy * numberOfShots * tuTotal * roleScoreModifier / tuCost / 100;
}

/**
 * Selects an AI mode based on a number of factors, some RNG and the results of the rest of the determinations.
 */
void PlayerFactionAI::evaluateAIMode()
{
	if ((_unit->getCharging() && _attackAction.type != BA_RETHINK))
	{
		_AIMode = AI_COMBAT;
		return;
	}
	// don't try to run away as often if we're a melee type, and really don't try to run away if we have a viable melee target, or we still have 50% or more TUs remaining.
	int escapeOdds = 15;
	if (_melee)
	{
		escapeOdds = 12;
	}
	if (_unit->getFaction() == FACTION_HOSTILE && (_unit->getTimeUnits() > _unit->getBaseStats()->tu / 2 || _unit->getCharging()))
	{
		escapeOdds = 5;
	}
	int ambushOdds = 12;
	int combatOdds = 20;
	// we're less likely to patrol if we see enemies.
	int patrolOdds = _visibleEnemies ? 15 : 30;

	// the enemy sees us, we should take retreat into consideration, and forget about patrolling for now.
	if (_spottingEnemies)
	{
		patrolOdds = 0;
		if (_escapeTUs == 0)
		{
			setupEscape();
		}
	}

	// melee/blaster units shouldn't consider ambush
	if (!_rifle || _ambushTUs == 0)
	{
		ambushOdds = 0;
		if (_melee)
		{
			combatOdds *= 1.3;
		}
	}

	// if we KNOW there are enemies around...
	if (_knownEnemies)
	{
		if (_knownEnemies == 1)
		{
			combatOdds *= 1.2;
		}

		if (_escapeTUs == 0)
		{
			if (selectClosestKnownEnemy())
			{
				setupEscape();
			}
			else
			{
				escapeOdds = 0;
			}
		}
	}
	else if (_unit->getFaction() == FACTION_HOSTILE)
	{
		combatOdds = 0;
		escapeOdds = 0;
	}

	// take our current mode into consideration
	switch (_AIMode)
	{
	case AI_PATROL:
		patrolOdds *= 1.1;
		break;
	case AI_AMBUSH:
		ambushOdds *= 1.1;
		break;
	case AI_COMBAT:
		combatOdds *= 1.1;
		break;
	case AI_ESCAPE:
		escapeOdds *= 1.1;
		break;
	}

	// take our overall health into consideration
	if (_unit->getHealth() < _unit->getBaseStats()->health / 3)
	{
		escapeOdds *= 1.7;
		combatOdds *= 0.6;
		ambushOdds *= 0.75;
	}
	else if (_unit->getHealth() < 2 * (_unit->getBaseStats()->health / 3))
	{
		escapeOdds *= 1.4;
		combatOdds *= 0.8;
		ambushOdds *= 0.8;
	}
	else if (_unit->getHealth() < _unit->getBaseStats()->health)
	{
		escapeOdds *= 1.1;
	}

	// take our aggression into consideration
	switch (_unit->getAggression())
	{
	case 0:
		escapeOdds *= 1.4;
		combatOdds *= 0.7;
		break;
	case 1:
		ambushOdds *= 1.1;
		break;
	case 2:
		combatOdds *= 1.4;
		escapeOdds *= 0.7;
		break;
	default:
		combatOdds *= Clamp(1.2 + (_unit->getAggression() / 10.0), 0.1, 2.0);
		escapeOdds *= Clamp(0.9 - (_unit->getAggression() / 10.0), 0.1, 2.0);
		break;
	}

	if (_AIMode == AI_COMBAT)
	{
		ambushOdds *= 1.5;
	}

	// factor in the spotters.
	if (_spottingEnemies)
	{
		escapeOdds = 10 * escapeOdds * (_spottingEnemies + 10) / 100;
		combatOdds = 5 * combatOdds * (_spottingEnemies + 20) / 100;
	}
	else
	{
		escapeOdds /= 2;
	}

	// factor in visible enemies.
	if (_visibleEnemies)
	{
		combatOdds = 10 * combatOdds * (_visibleEnemies + 10) /100;
		if (_closestDist < 5)
		{
			ambushOdds = 0;
		}
	}
	// make sure we have an ambush lined up, or don't even consider it.
	if (_ambushTUs)
	{
		ambushOdds *= 1.7;
	}
	else
	{
		ambushOdds = 0;
	}

	// factor in mission type
	if (_save->getMissionType() == "STR_BASE_DEFENSE")
	{
		escapeOdds *= 0.75;
		ambushOdds *= 0.6;
	}

	// no weapons, not psychic? don't pick combat or ambush
	if (!_melee && !_rifle && !_blaster && !_grenade && _unit->getBaseStats()->psiSkill == 0)
	{
		combatOdds = 0;
		ambushOdds = 0;
	}
	// generate a random number to represent our decision.
	int decision = RNG::generate(1, std::max(1, patrolOdds + ambushOdds + escapeOdds + combatOdds));

	if (decision > escapeOdds)
	{
		if (decision > escapeOdds + ambushOdds)
		{
			if (decision > escapeOdds + ambushOdds + combatOdds)
			{
				_AIMode = AI_PATROL;
			}
			else
			{
				_AIMode = AI_COMBAT;
			}
		}
		else
		{
			_AIMode = AI_AMBUSH;
		}
	}
	else
	{
		_AIMode = AI_ESCAPE;
	}

	// if the aliens are cheating, or the unit is charging, enforce combat as a priority.
	if ((_unit->getFaction() == FACTION_HOSTILE && _save->isCheating()) || _unit->getCharging() != 0)
	{
		_AIMode = AI_COMBAT;
	}


	// enforce the validity of our decision, and try fallback behaviour according to priority.
	if (_AIMode == AI_COMBAT)
	{
		auto* xtile = _save->getTile(_attackAction.target);
		bool throwingGrenadeOrProxy = _attackAction.type == BA_THROW && _attackAction.weapon && _attackAction.weapon->getRules()->isGrenadeOrProxy();
		if (xtile && (xtile->getUnit() || throwingGrenadeOrProxy)) // https://openxcom.org/forum/index.php?topic=12145.0
		{
			if (_attackAction.type != BA_RETHINK)
			{
				return;
			}
			if (findFirePoint())
			{
				return;
			}
		}
		else if (selectRandomTarget() && findFirePoint())
		{
			return;
		}
		_AIMode = AI_PATROL;
	}

	if (_AIMode == AI_PATROL)
	{
		if (_toNode || _foundBaseModuleToDestroy)
		{
			return;
		}
		// base defense mission protocol: patrol action becomes an attack action when base modules are sighted
		if (_patrolAction.type == BA_SNAPSHOT)
		{
			return;
		}
		_AIMode = AI_AMBUSH;
	}

	if (_AIMode == AI_AMBUSH)
	{
		if (_ambushTUs != 0)
		{
			return;
		}
		_AIMode = AI_ESCAPE;
	}
}

/**
 * Find a position where we can see our target, and move there.
 * check the 11x11 grid for a position nearby where we can potentially target him.
 * @return True if a possible position was found.
 */
bool PlayerFactionAI::findFirePoint()
{
	if (!selectClosestKnownEnemy())
		return false;
	std::vector<Position> randomTileSearch = _save->getTileSearch(); // copy!
	RNG::shuffle(randomTileSearch);
	Position target;
	const int BASE_SYSTEMATIC_SUCCESS = 100;
	const int FAST_PASS_THRESHOLD = 125;
	bool waitIfOutsideWeaponRange = _unit->getGeoscapeSoldier() ? false : _unit->getUnitRules()->waitIfOutsideWeaponRange();
	bool extendedFireModeChoiceEnabled = _save->getMod()->getAIExtendedFireModeChoice();
	int bestScore = 0;
	_attackAction.type = BA_RETHINK;
	for (const auto& randomPosition : randomTileSearch)
	{
		Position pos = _unit->getPosition() + randomPosition;
		Tile *tile = _save->getTile(pos);
		if (tile == 0  ||
			std::find(_reachableWithAttack.begin(), _reachableWithAttack.end(), _save->getTileIndex(pos))  == _reachableWithAttack.end())
			continue;
		int score = 0;
		// i should really make a function for this
		Position origin = pos.toVoxel() +
			// 4 because -2 is eyes and 2 below that is the rifle (or at least that's my understanding)
			Position(8,8, _unit->getHeight() + _unit->getFloatHeight() - tile->getTerrainLevel() - 4);

		if (_save->getTileEngine()->canTargetUnit(&origin, _aggroTarget->getTile(), &target, _unit, false))
		{
			_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
			// can move here
			if (_save->getPathfinding()->getStartDirection() != -1)
			{
				int spotters = getSpottingUnits(pos);
				if (_unit->getFaction() == FACTION_PLAYER && spotters > 0)
				{
					continue;
				}
				score = BASE_SYSTEMATIC_SUCCESS - spotters * 10;
				score += _unit->getTimeUnits() - _save->getPathfinding()->getTotalTUCost();
				if (!_aggroTarget->checkViewSector(pos))
				{
					score += 10;
				}

				// Extended behavior: if we have a limited-range weapon, bump up the score for getting closer to the target, down for further
				if (!waitIfOutsideWeaponRange && extendedFireModeChoiceEnabled)
				{
					int distanceToTargetSq = _unit->distance3dToUnitSq(_aggroTarget);
					int distanceToTarget = (int)std::ceil(sqrt(float(distanceToTargetSq)));
					if (_attackAction.weapon && _attackAction.weapon->getRules()->isOutOfRange(distanceToTargetSq)) // make sure we can get the ruleset before checking the range
					{
						int proposedDistance = Position::distance2d(pos, _aggroTarget->getPosition());
						proposedDistance = std::max(proposedDistance, 1);
						score = score * distanceToTarget / proposedDistance;
					}
				}

				if (score > bestScore)
				{
					bestScore = score;
					_attackAction.target = pos;
					_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(pos, _aggroTarget->getPosition());
					if (score > FAST_PASS_THRESHOLD)
					{
						break;
					}
				}
			}
		}
	}

	if (bestScore > 70)
	{
		_attackAction.type = BA_WALK;
		if (_traceAI)
		{
			Log(LOG_INFO) << "Firepoint found at " << _attackAction.target << ", with a score of: " << bestScore;
		}
		return true;
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Firepoint failed, best estimation was: " << _attackAction.target << ", with a score of: " << bestScore;
	}

	return false;
}

bool PlayerFactionAI::setupCleanShotMove(BattleUnit *target)
{
	if (_unit->getFaction() != FACTION_PLAYER || !target || target->isOut() || !_attackAction.weapon || (_reachableWithAttack.empty() && _reachable.empty()))
	{
		if (_unit->getFaction() == FACTION_PLAYER && Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction clean shot move rejected early: unit=" << _unit->getId()
				<< ", target=" << (target ? target->getId() : -1)
				<< ", hasWeapon=" << (_attackAction.weapon != 0)
				<< ", reachable=" << _reachable.size()
				<< ", reachableWithAttack=" << _reachableWithAttack.size();
			_save->appendToAutoBattleLog(log.str());
		}
		return false;
	}

	const PlayerAIRole role = getPlayerAIRole(_attackAction.weapon);
	const int preferredRange = getPreferredEngagementRange(_attackAction.weapon);
	const int currentSpotters = getSpottingUnits(_unit->getPosition());
	const int currentExposure = getEnemyFireExposure(_unit->getPosition());
	const int currentDist = Position::distance2d(_unit->getPosition(), target->getPosition());
	const bool meleeRole = role == ROLE_MELEE || _attackAction.weapon->getRules()->getBattleType() == BT_MELEE;
	int bestScore = -100000;
	Position bestPos = _unit->getPosition();
	int bestSpotters = currentSpotters;
	int bestDist = currentDist;
	const bool targetVisible = target->getTile() && _save->getTileEngine()->visible(_unit, target->getTile());

	const std::vector<int> &reachableTiles = _reachableWithAttack.empty() ? _reachable : _reachableWithAttack;
	for (auto tileIndex : reachableTiles)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		Position pos = tile->getPosition();
		if (pos.z != _unit->getPosition().z)
		{
			continue;
		}

		_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
		const int moveTU = _save->getPathfinding()->getTotalTUCost();
		const bool canMove = _save->getPathfinding()->getStartDirection() != -1;
		_save->getPathfinding()->abortPath();
		if (!canMove)
		{
			continue;
		}

		BattleActionCost snapCost(BA_SNAPSHOT, _unit, _attackAction.weapon);
		const int reserveTU = meleeRole ? 8 : std::max(18, (int)snapCost.Time);
		if (moveTU > std::max(0, _unit->getTimeUnits() - reserveTU))
		{
			continue;
		}

		Position origin = pos.toVoxel() + Position(8, 8, _unit->getHeight() + _unit->getFloatHeight() - tile->getTerrainLevel() - 4);
		Position targetVoxel;
		const bool hasLine = meleeRole || _save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetVoxel, _unit, false, target);
		if (!hasLine && targetVisible)
		{
			continue;
		}
		bool allyInLane = false;
		if (!meleeRole)
		{
			const double vx = (double)(target->getPosition().x - pos.x);
			const double vy = (double)(target->getPosition().y - pos.y);
			const double lenSq = vx * vx + vy * vy;
			if (lenSq > 0.1)
			{
				for (auto *ally : *_save->getUnits())
				{
					if (!ally || ally == _unit || ally == target || ally->isOut() || ally->getFaction() != _unit->getFaction() || ally->getPosition().z != pos.z)
					{
						continue;
					}
					const double ax = (double)(ally->getPosition().x - pos.x);
					const double ay = (double)(ally->getPosition().y - pos.y);
					const double t = (ax * vx + ay * vy) / lenSq;
					if (t <= 0.0 || t >= 1.05)
					{
						continue;
					}
					const double dx = ax - vx * t;
					const double dy = ay - vy * t;
					if (dx * dx + dy * dy <= 0.55)
					{
						allyInLane = true;
						break;
					}
				}
			}
		}
		if (allyInLane)
		{
			continue;
		}

		const int spotters = getSpottingUnits(pos);
		const int exposure = getEnemyFireExposure(pos);
		if (spotters > currentSpotters && spotters > 0)
		{
			continue;
		}
		if (exposure > currentExposure + 60 && exposure > 90)
		{
			continue;
		}
		const int dist = Position::distance2d(pos, target->getPosition());
		if (!meleeRole && dist < std::max(3, preferredRange - 5))
		{
			continue;
		}

		int cover = 0;
		if (tile->getMapData(O_OBJECT))
		{
			cover += 8;
		}
		if (tile->getMapData(O_NORTHWALL))
		{
			cover += 6;
		}
		if (tile->getMapData(O_WESTWALL))
		{
			cover += 6;
		}
		int score = 220 - abs(dist - preferredRange) * (role == ROLE_MARKSMAN || role == ROLE_HEAVY ? 5 : 3);
		score -= moveTU * 2;
		score -= spotters * 45;
		score -= exposure;
		score += cover * 5;
		if (!hasLine)
		{
			score = 120 - dist * 5 - moveTU * 2 + cover * 4 - spotters * 35;
			score -= exposure;
			score += std::max(0, currentDist - dist) * 18;
		}
		if (spotters < currentSpotters)
		{
			score += 45;
		}
		if (dist >= currentDist && role == ROLE_MARKSMAN)
		{
			score += 20;
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestSpotters = spotters;
			bestDist = dist;
		}
	}

	if (bestScore <= 90 || bestPos == _unit->getPosition())
	{
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction clean shot move rejected: unit=" << _unit->getId()
				<< ", targetUnit=" << target->getId()
				<< ", bestScore=" << bestScore
				<< ", bestPos=" << bestPos
				<< ", current=" << _unit->getPosition()
				<< ", reachable=" << _reachable.size()
				<< ", reachableWithAttack=" << _reachableWithAttack.size();
			_save->appendToAutoBattleLog(log.str());
		}
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = _attackAction.weapon;
	_attackAction.type = BA_WALK;
	_attackAction.target = bestPos;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, target->getPosition());
	_AIMode = AI_COMBAT;
	_cleanShotMoveAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction clean shot move: unit=" << _unit->getId()
			<< ", targetUnit=" << target->getId()
			<< ", target=" << target->getPosition()
			<< ", moveTarget=" << bestPos
			<< ", score=" << bestScore
			<< ", dist=" << bestDist
			<< ", spotters=" << bestSpotters
			<< ", role=" << (int)role
			<< ", reason=free_fire_lane_and_keep_shot_reserve";
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

bool PlayerFactionAI::setupFallbackCoverMove()
{
	if (_unit->getFaction() != FACTION_PLAYER || _reachable.empty() || (!_visibleEnemies && !_spottingEnemies))
	{
		return false;
	}
	Position facePos = _unit->getPosition();
	BattleUnit *assigned = _factionAI ? _factionAI->getAssignedTarget(_unit) : 0;
	if (_aggroTarget && !_aggroTarget->isOut())
	{
		facePos = _aggroTarget->getPosition();
	}
	else if (assigned && !assigned->isOut())
	{
		facePos = assigned->getPosition();
	}
	else if (_factionAI)
	{
		_factionAI->getBestEnemyContactPosition(&facePos);
	}

	auto coverScoreAt = [&](const Position &pos) -> int
	{
		Tile *tile = _save->getTile(pos);
		if (!tile)
		{
			return 0;
		}
		int score = 0;
		if (tile->getMapData(O_OBJECT))
		{
			score += 10;
		}
		if (tile->getMapData(O_NORTHWALL))
		{
			score += facePos.y < pos.y ? 16 : 6;
		}
		if (tile->getMapData(O_WESTWALL))
		{
			score += facePos.x < pos.x ? 16 : 6;
		}
		const Position adjacent[4] = { Position(0, -1, 0), Position(1, 0, 0), Position(0, 1, 0), Position(-1, 0, 0) };
		for (const auto &offset : adjacent)
		{
			Tile *adjacentTile = _save->getTile(pos + offset);
			if (!adjacentTile)
			{
				continue;
			}
			if (adjacentTile->getMapData(O_OBJECT))
			{
				score += 3;
			}
			if (adjacentTile->getMapData(O_NORTHWALL) || adjacentTile->getMapData(O_WESTWALL))
			{
				score += 2;
			}
		}
		return score;
	};

	auto allyCrowdingPenalty = [&](const Position &pos) -> int
	{
		int penalty = 0;
		for (auto *other : *_save->getUnits())
		{
					if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
			{
				continue;
			}
			const int dist = Position::distance2d(pos, other->getPosition());
			if (dist <= 1)
			{
				penalty += 120;
			}
			else if (dist <= 2)
			{
				penalty += 35;
			}
		}
		return penalty;
	};

	auto allyLanePenalty = [&](const Position &pos) -> int
	{
		int penalty = 0;
		const double px = (double)pos.x;
		const double py = (double)pos.y;
		for (auto *other : *_save->getUnits())
		{
			if (!other || other == _unit || other->isOut() || other->getFaction() != _unit->getFaction() || other->getPosition().z != pos.z)
			{
				continue;
			}
			BattleItem *otherWeapon = other->getMainHandWeapon(false);
			if (!otherWeapon || otherWeapon->getRules()->getBattleType() != BT_FIREARM)
			{
				continue;
			}
			const Position otherPos = other->getPosition();
			const double vx = (double)(facePos.x - otherPos.x);
			const double vy = (double)(facePos.y - otherPos.y);
			const double lenSq = vx * vx + vy * vy;
			if (lenSq < 0.1)
			{
				continue;
			}
			const double ax = px - otherPos.x;
			const double ay = py - otherPos.y;
			const double t = (ax * vx + ay * vy) / lenSq;
			if (t <= 0.0 || t >= 1.0)
			{
				continue;
			}
			const double dx = ax - vx * t;
			const double dy = ay - vy * t;
			if (dx * dx + dy * dy <= 1.0)
			{
				penalty += 90;
			}
		}
		return penalty;
	};

	const Position current = _unit->getPosition();
	const int currentSpotters = getSpottingUnits(current);
	const int currentExposure = getEnemyFireExposure(current);
	const int currentCover = coverScoreAt(current);
	const int currentDist = Position::distance2d(current, facePos);
	const int preferredRange = getPreferredEngagementRange(_attackAction.weapon);
	const bool ranged = _attackAction.weapon && _attackAction.weapon->getRules()->getBattleType() == BT_FIREARM;
	int bestScore = 0;
	Position bestPos = current;
	int bestSpotters = currentSpotters;
	int bestExposure = currentExposure;
	int bestCover = currentCover;

	for (auto tileIndex : _reachable)
	{
		Tile *tile = _save->getTile(tileIndex);
		if (!tile || tile->getDangerous() || (tile->getUnit() && tile->getUnit() != _unit))
		{
			continue;
		}
		const Position pos = tile->getPosition();
		const int moveDist = Position::distance2d(pos, current);
		if (pos.z != current.z || moveDist == 0 || moveDist > 4)
		{
			continue;
		}
		if (moveDist * 6 > _unit->getTimeUnits())
		{
			continue;
		}
		const int spotters = getSpottingUnits(pos);
		const int exposure = getEnemyFireExposure(pos);
		const int cover = coverScoreAt(pos);
		const int dist = Position::distance2d(pos, facePos);
		if (ranged && dist < 2)
		{
			continue;
		}
		int score = 0;
		score += (currentSpotters - spotters) * 95;
		score += (currentExposure - exposure);
		score += (cover - currentCover) * 5;
		score -= moveDist * 8;
		score -= allyCrowdingPenalty(pos);
		score -= allyLanePenalty(pos);
		if (ranged)
		{
			score -= abs(dist - preferredRange) * 2;
			score += dist >= currentDist ? 12 : -18;
		}
		else
		{
			score += dist <= currentDist ? 10 : -8;
		}
		if (spotters > currentSpotters)
		{
			score -= 140;
		}
		if (exposure > currentExposure && spotters >= currentSpotters)
		{
			score -= 90;
		}
		if (score > bestScore)
		{
			bestScore = score;
			bestPos = pos;
			bestSpotters = spotters;
			bestExposure = exposure;
			bestCover = cover;
		}
	}

	if (bestPos == current || bestScore < 45)
	{
		return false;
	}

	_attackAction.actor = _unit;
	_attackAction.weapon = selectBestCarriedWeapon();
	_attackAction.type = BA_WALK;
	_attackAction.target = bestPos;
	_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(bestPos, facePos);
	_AIMode = AI_COMBAT;
	_fallbackCoverAction = true;
	if (Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction fallback cover move: unit=" << _unit->getId()
			<< ", target=" << bestPos
			<< ", face=" << facePos
			<< ", score=" << bestScore
			<< ", spotters=" << currentSpotters << "->" << bestSpotters
			<< ", exposure=" << currentExposure << "->" << bestExposure
			<< ", cover=" << currentCover << "->" << bestCover;
		_save->appendToAutoBattleLog(log.str());
	}
	return true;
}

/**
 * Decides if it worth our while to create an explosion here.
 * Return value in same range as number affected targets but not equal exactly to that value.
 * @param targetPos The target's position.
 * @param attackingUnit The attacking unit.
 * @param radius How big the explosion will be.
 * @param diff Game difficulty.
 * @param grenade Is the explosion coming from a grenade?
 * @return Value greater than zero if it is worthwhile creating an explosion in the target position. Bigger value better target.
 */
int PlayerFactionAI::explosiveEfficacy(Position targetPos, BattleUnit *attackingUnit, int radius, int diff, bool grenade) const
{
	Tile *targetTile = _save->getTile(targetPos);

	// don't throw grenades at flying enemies.
	if (grenade && targetPos.z > 0 && targetTile->hasNoFloor(_save))
	{
		return false;
	}

	if (diff == -1)
	{
		diff = _save->getBattleState()->getGame()->getSavedGame()->getDifficultyCoefficient();
	}
	int distance = Position::distance2d(attackingUnit->getPosition(), targetPos);
	int injurylevel = attackingUnit->getBaseStats()->health - attackingUnit->getHealth();
	int desperation = (100 - attackingUnit->getMorale()) / 10;
	int enemiesAffected = 0;
	// if we're below 1/3 health, let's assume things are dire, and increase desperation.
	if (injurylevel > (attackingUnit->getBaseStats()->health / 3) * 2)
		desperation += 3;

	int efficacy = AIW_SCALE * desperation;

	// don't go kamikaze unless we're already doomed.
	if (abs(attackingUnit->getPosition().z - targetPos.z) <= Options::battleExplosionHeight && distance <= radius)
	{
		if (attackingUnit->getFaction() == FACTION_PLAYER)
		{
			return 0;
		}
		efficacy -= AIW_SCALE * 4;
	}

	// allow difficulty to have its influence
	efficacy += AIW_SCALE * diff/2;

	// account for the unit we're targetting
	BattleUnit *target = targetTile->getUnit();
	if (target && !targetTile->getDangerous())
	{
		if (attackingUnit->getFaction() == FACTION_PLAYER && target->getFaction() == attackingUnit->getFaction())
		{
			return 0;
		}
		++enemiesAffected;
		efficacy += getTargetAttackWeight(target);
	}

	for (auto* bu : *_save->getUnits())
	{
			// don't grenade dead guys
		if (!bu->isOut() &&
			// don't count ourself twice
			bu != attackingUnit &&
			// don't count the target twice
			bu != target &&
			// don't count units that probably won't be affected cause they're out of range
			abs(bu->getPosition().z - targetPos.z) <= Options::battleExplosionHeight &&
			Position::distance2d(bu->getPosition(), targetPos) <= radius)
		{
			if (bu->getTile()->getDangerous())
			{
				// don't count people who were already grenaded this turn
				continue;
			}

			auto weight = getTargetAttackWeight(bu);

			if (weight == 0)
			{
				// AI do not know anything about this unit
				continue;
			}

			// trace a line from the grenade origin to the unit we're checking against
			Position voxelPosA = Position (targetPos.toVoxel() + TileEngine::voxelTileCenter);
			Position voxelPosB = Position (bu->getPosition().toVoxel() + TileEngine::voxelTileCenter);
			std::vector<Position> traj;
			int collidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosB, false, &traj, target, bu);

			if (collidesWith == V_UNIT && traj.front().toTile() == bu->getPosition())
			{
				if (attackingUnit->getFaction() == FACTION_PLAYER && bu->getFaction() == attackingUnit->getFaction())
				{
					return 0;
				}
				if (bu->getFaction() == _targetFaction)
				{
					++enemiesAffected;
				}

				efficacy += weight;
			}
		}
	}
	// don't throw grenades at single targets, unless morale is in the danger zone
	// or we're halfway towards panicking while bleeding to death.
	if (grenade && desperation < 6 && enemiesAffected < 2)
	{
		return 0;
	}

	if (enemiesAffected >= 10)
	{
		// Ignore loses if we can kill lot of enemies.
		return enemiesAffected;
	}
	else if (efficacy > 0)
	{
		// We kill more enemies than allies. Scale back to number of targets, can round down to zero
		return efficacy / AIW_SCALE;
	}
	else
	{
		return 0;
	}
}

bool PlayerFactionAI::explosiveProjectileRiskyForAllies(BattleAction *action, int radius, const Position *originPosition, bool logRejection) const
{
	if (!action || !action->actor || !action->weapon || radius <= 0 || action->actor->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	Tile *targetTile = _save->getTile(action->target);
	if (!targetTile)
	{
		return true;
	}

	BattleAction testAction = *action;
	Position actorPosition = originPosition ? *originPosition : action->actor->getPosition();
	Position originVoxel = _save->getTileEngine()->getOriginVoxel(testAction, _save->getTile(actorPosition));
	Position targetVoxel = action->target.toVoxel() + TileEngine::voxelTileCenter;

	BattleUnit *targetUnit = targetTile->getUnit();
	if (targetUnit && targetUnit != action->actor)
	{
		_save->getTileEngine()->canTargetUnit(&originVoxel, targetTile, &targetVoxel, action->actor, false, targetUnit);
	}
	else if (targetTile->getMapData(O_OBJECT) != 0)
	{
		_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_OBJECT, &targetVoxel, action->actor, false);
	}
	else if (targetTile->getMapData(O_NORTHWALL) != 0)
	{
		_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_NORTHWALL, &targetVoxel, action->actor, false);
	}
	else if (targetTile->getMapData(O_WESTWALL) != 0)
	{
		_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_WESTWALL, &targetVoxel, action->actor, false);
	}
	else if (targetTile->getMapData(O_FLOOR) != 0)
	{
		_save->getTileEngine()->canTargetTile(&originVoxel, targetTile, O_FLOOR, &targetVoxel, action->actor, false);
	}

	BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(*action);
	double accuracy = BattleUnit::getFiringAccuracy(attack, _save->getMod()) / 100.0;
	int upperLimit = 0;
	int lowerLimit = 0;
	int dropoff = action->weapon->getRules()->calculateLimits(upperLimit, lowerLimit, _save->getDepth(), action->type);
	const int xdiff = originVoxel.x - targetVoxel.x;
	const int ydiff = originVoxel.y - targetVoxel.y;
	const int zdiff = originVoxel.z - targetVoxel.z;
	const double realDistance = sqrt((double)(xdiff * xdiff) + (double)(ydiff * ydiff) + (double)(zdiff * zdiff));
	const double distanceTiles = realDistance / 16.0;
	if (distanceTiles > upperLimit)
	{
		accuracy = std::max(0.0, accuracy - (dropoff * (distanceTiles - upperLimit)) / 100.0);
	}
	else if (distanceTiles < lowerLimit)
	{
		accuracy = std::max(0.0, accuracy - (dropoff * (lowerLimit - distanceTiles)) / 100.0);
	}

	const int xDist = abs(originVoxel.x - targetVoxel.x);
	const int yDist = abs(originVoxel.y - targetVoxel.y);
	const int zDist = abs(originVoxel.z - targetVoxel.z);
	int xyShift;
	if (Options::oxceUniformShootingSpread)
	{
		if (xDist <= yDist)
			xyShift = xDist / 4 + yDist;
		else
			xyShift = xDist + yDist / 4;
		xyShift = (int)(xyShift * 0.839);
	}
	else
	{
		if (xDist / 2 <= yDist)
			xyShift = xDist / 4 + yDist;
		else
			xyShift = (xDist + yDist) / 2;
	}
	const int zShift = (xyShift <= zDist) ? (xyShift / 2 + zDist) : (xyShift + zDist / 2);
	int worstDeviationRoll = 100 - (int)(accuracy * 100);
	if (worstDeviationRoll >= 0)
		worstDeviationRoll += 50;
	else
		worstDeviationRoll += 10;
	const int deviation = std::max(1, zShift * worstDeviationRoll / 200);
	const int quarterDeviation = std::max(1, deviation / 4);

	std::vector<Position> targetSamples;
	targetSamples.push_back(targetVoxel);
	targetSamples.push_back(targetVoxel + Position(quarterDeviation, 0, 0));
	targetSamples.push_back(targetVoxel + Position(-quarterDeviation, 0, 0));
	targetSamples.push_back(targetVoxel + Position(0, quarterDeviation, 0));
	targetSamples.push_back(targetVoxel + Position(0, -quarterDeviation, 0));
	targetSamples.push_back(targetVoxel + Position(0, 0, quarterDeviation));
	targetSamples.push_back(targetVoxel + Position(0, 0, -quarterDeviation));

	auto blastWouldHitAlly = [&](const Position &impactTile) -> bool
	{
		for (auto* unit : *_save->getUnits())
		{
			if (!unit || unit->isOut() || unit->getFaction() != action->actor->getFaction())
			{
				continue;
			}
			Position unitPosition = (unit == action->actor) ? actorPosition : unit->getPosition();
			if (abs(unitPosition.z - impactTile.z) <= Options::battleExplosionHeight &&
				Position::distance2d(unitPosition, impactTile) <= radius)
			{
				return true;
			}
		}
		return false;
	};

	int riskySamples = 0;
	int checkedSamples = 0;
	std::string firstReason;
	Position firstImpactTile;
	int firstImpact = V_EMPTY;
	for (size_t i = 0; i < targetSamples.size(); ++i)
	{
		const auto &sample = targetSamples[i];
		std::vector<Position> trajectory;
		int impact = _save->getTileEngine()->calculateLineVoxel(originVoxel, sample, true, &trajectory, action->actor);
		Position impactTile = sample.toTile();
		if (impact != V_EMPTY && !trajectory.empty())
		{
			impactTile = trajectory.front().toTile();
		}
		bool risky = false;
		for (const auto &voxel : trajectory)
		{
			Tile *tile = _save->getTile(voxel.toTile());
			BattleUnit *unit = tile ? tile->getUnit() : 0;
			if (unit && unit != action->actor && unit->getFaction() == action->actor->getFaction())
			{
				risky = true;
				if (firstReason.empty())
				{
					firstReason = "ally_on_trajectory";
					firstImpactTile = voxel.toTile();
					firstImpact = impact;
				}
				break;
			}
		}
		if (!risky && impact != V_EMPTY && blastWouldHitAlly(impactTile))
		{
			risky = true;
			if (firstReason.empty())
			{
				firstReason = "ally_in_impact_blast";
				firstImpactTile = impactTile;
				firstImpact = impact;
			}
		}
		if (risky)
		{
			if (i == 0)
			{
				bool hardReject = firstReason == "ally_on_trajectory" || firstImpactTile == actorPosition;
				if (hardReject && logRejection && Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction explosive shot rejected: unit=" << action->actor->getId()
						<< ", origin=" << actorPosition
						<< ", target=" << action->target
						<< ", radius=" << radius
						<< ", accuracy=" << (int)(accuracy * 100)
						<< ", reason=" << firstReason
						<< ", impact=" << firstImpact
						<< ", impactTile=" << firstImpactTile
						<< ", central=true";
					_save->appendToAutoBattleLog(log.str());
				}
				if (hardReject)
				{
					return true;
				}
			}
			++riskySamples;
		}
		++checkedSamples;
	}
	bool riskyBySpread = riskySamples * 2 >= std::max(1, checkedSamples - 1);
	if (riskyBySpread && logRejection && Options::autoBattleLog)
	{
		std::ostringstream log;
		log << "Player faction explosive shot rejected: unit=" << action->actor->getId()
			<< ", origin=" << actorPosition
			<< ", target=" << action->target
			<< ", radius=" << radius
			<< ", accuracy=" << (int)(accuracy * 100)
			<< ", reason=" << firstReason
			<< ", impact=" << firstImpact
			<< ", impactTile=" << firstImpactTile
			<< ", riskySamples=" << riskySamples
			<< ", checkedSamples=" << checkedSamples;
		_save->appendToAutoBattleLog(log.str());
	}
	return riskyBySpread;
}

bool PlayerFactionAI::directProjectileRiskyForAllies(BattleAction *action, BattleUnit *target, bool logRejection) const
{
	if (!action || !action->actor || !action->weapon || !target || action->actor->getFaction() != FACTION_PLAYER)
	{
		return false;
	}

	if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
	{
		return false;
	}

	const Position from = action->actor->getPosition();
	const Position to = target->getPosition();
	const double vx = (double)(to.x - from.x);
	const double vy = (double)(to.y - from.y);
	const double lenSq = vx * vx + vy * vy;
	if (lenSq > 0.1)
	{
		for (auto *ally : *_save->getUnits())
		{
			if (!ally || ally == action->actor || ally == target || ally->isOut() || ally->getFaction() != action->actor->getFaction())
			{
				continue;
			}
			if (ally->getPosition().z != from.z)
			{
				continue;
			}
			const double ax = (double)(ally->getPosition().x - from.x);
			const double ay = (double)(ally->getPosition().y - from.y);
			const double t = (ax * vx + ay * vy) / lenSq;
			if (t <= 0.0 || t >= 1.05)
			{
				continue;
			}
			const double dx = ax - vx * t;
			const double dy = ay - vy * t;
			const double lateralSq = dx * dx + dy * dy;
			if (lateralSq <= 0.55)
			{
				if (logRejection && Options::autoBattleLog)
				{
					std::ostringstream log;
					log << "Player faction direct shot rejected: unit=" << action->actor->getId()
						<< ", targetUnit=" << target->getId()
						<< ", target=" << action->target
						<< ", fireMode=" << (int)action->type
						<< ", ally=" << ally->getId()
						<< ", allyPos=" << ally->getPosition()
						<< ", lateralSq=" << (int)(lateralSq * 100)
						<< ", reason=ally_in_direct_fire_lane";
					_save->appendToAutoBattleLog(log.str());
				}
				return true;
			}
		}
	}

	Position originVoxel = _save->getTileEngine()->getOriginVoxel(*action, _save->getTile(action->actor->getPosition()));
	Position targetVoxel;
	if (!_save->getTileEngine()->canTargetUnit(&originVoxel, target->getTile(), &targetVoxel, action->actor, false, target))
	{
		return false;
	}

	std::vector<Position> trajectory;
	int impact = _save->getTileEngine()->calculateLineVoxel(originVoxel, targetVoxel, true, &trajectory, action->actor);
	for (const auto &voxel : trajectory)
	{
		Tile *tile = _save->getTile(voxel.toTile());
		BattleUnit *unit = tile ? tile->getOverlappingUnit(_save) : 0;
		if (!unit || unit == action->actor || unit == target)
		{
			Position tilePos = voxel.toTile();
			for (auto *ally : *_save->getUnits())
			{
				if (!ally || ally == action->actor || ally == target || ally->isOut() || ally->getFaction() != action->actor->getFaction())
				{
					continue;
				}
				if (ally->getPosition().z == tilePos.z
					&& Position::distance2d(ally->getPosition(), tilePos) <= 0)
				{
					if (logRejection && Options::autoBattleLog)
					{
						std::ostringstream log;
						log << "Player faction direct shot rejected: unit=" << action->actor->getId()
							<< ", targetUnit=" << target->getId()
							<< ", target=" << action->target
							<< ", fireMode=" << (int)action->type
							<< ", ally=" << ally->getId()
							<< ", allyPos=" << ally->getPosition()
							<< ", voxel=" << voxel
							<< ", impact=" << impact
							<< ", reason=ally_near_trajectory";
						_save->appendToAutoBattleLog(log.str());
					}
					return true;
				}
			}
			continue;
		}
		if (unit->getFaction() == action->actor->getFaction())
		{
			if (logRejection && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction direct shot rejected: unit=" << action->actor->getId()
					<< ", targetUnit=" << target->getId()
					<< ", target=" << action->target
					<< ", fireMode=" << (int)action->type
					<< ", ally=" << unit->getId()
					<< ", allyPos=" << unit->getPosition()
					<< ", voxel=" << voxel
					<< ", impact=" << impact
					<< ", reason=ally_on_trajectory";
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
	}

	return false;
}

bool PlayerFactionAI::autoShotRiskyForAllies(BattleAction *action, BattleUnit *target, bool logRejection) const
{
	if (!action || !action->actor || !action->weapon || !target || action->type != BA_AUTOSHOT || action->actor->getFaction() != FACTION_PLAYER)
	{
		return false;
	}
	if (action->weapon->getArcingShot(action->type))
	{
		return false;
	}

	const Position from = action->actor->getPosition();
	const Position to = target->getPosition();
	const double vx = (double)(to.x - from.x);
	const double vy = (double)(to.y - from.y);
	const double lenSq = vx * vx + vy * vy;
	if (lenSq < 0.1)
	{
		return false;
	}

	for (auto *ally : *_save->getUnits())
	{
		if (!ally || ally == action->actor || ally == target || ally->isOut() || ally->getFaction() != action->actor->getFaction())
		{
			continue;
		}
		if (ally->getPosition().z != from.z)
		{
			continue;
		}
		const double ax = (double)(ally->getPosition().x - from.x);
		const double ay = (double)(ally->getPosition().y - from.y);
		const double t = (ax * vx + ay * vy) / lenSq;
		if (t <= 0.0 || t >= 1.15)
		{
			continue;
		}
		const double closestX = vx * t;
		const double closestY = vy * t;
		const double dx = ax - closestX;
		const double dy = ay - closestY;
		const double lateralSq = dx * dx + dy * dy;
		const int actorDist = Position::distance2d(from, ally->getPosition());
		if (lateralSq <= 2.25 || (actorDist <= 2 && t > 0.0))
		{
			if (logRejection && Options::autoBattleLog)
			{
				std::ostringstream log;
				log << "Player faction auto shot rejected: unit=" << action->actor->getId()
					<< ", targetUnit=" << target->getId()
					<< ", target=" << action->target
					<< ", ally=" << ally->getId()
					<< ", allyPos=" << ally->getPosition()
					<< ", lateralSq=" << (int)(lateralSq * 100)
					<< ", actorDist=" << actorDist
					<< ", reason=ally_in_auto_fire_cone";
				_save->appendToAutoBattleLog(log.str());
			}
			return true;
		}
	}

	return false;
}

bool PlayerFactionAI::projectileRiskyForAllies(BattleAction *action, BattleUnit *target, bool logRejection) const
{
	return directProjectileRiskyForAllies(action, target, logRejection) || autoShotRiskyForAllies(action, target, logRejection);
}

/**
 * Attempts to take a melee attack/charge an enemy we can see.
 * Melee targetting: we can see an enemy, we can move to it so we're charging blindly toward an enemy.
 */
void PlayerFactionAI::meleeAction()
{
	BattleActionCost attackCost(BA_HIT, _unit, _unit->getUtilityWeapon(BT_MELEE));
	if (!attackCost.haveTU())
	{
		// cannot make a melee attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	if (_aggroTarget != 0 && !_aggroTarget->isOut())
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
			return;
		}
	}
	int chargeReserve = std::min(_unit->getTimeUnits() - attackCost.Time, 2 * (_unit->getEnergy() - attackCost.Energy));
	int distance = (chargeReserve / 4) + 1;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		int newDistance = Position::distance2d(_unit->getPosition(), bu->getPosition());
		if (newDistance > 20 ||
			!validTarget(bu, true, true))
			continue;
		//pick closest living unit that we can move to
		if ((newDistance < distance || newDistance == 1) && !bu->isOut())
		{
			if (newDistance == 1 || selectPointNearTarget(bu, chargeReserve))
			{
				_aggroTarget = bu;
				_attackAction.type = BA_WALK;
				_unit->setCharging(_aggroTarget);
				distance = newDistance;
			}

		}
	}
	if (_aggroTarget != 0)
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
		}
	}
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "PlayerFactionAI::meleeAction:" << " [target]: " << (_aggroTarget->getId()) << " at: "  << _attackAction.target; }
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "CHARGE!"; }
}

/**
 * Attempts to take a melee attack/charge an enemy we can see.
 * Melee targetting: we can see an enemy, we can move to it so we're charging blindly toward an enemy.
 * Note: Differs from meleeAction() in calling selectPointNearTargetLeeroy() and ignoring some more checks.
 */
void PlayerFactionAI::meleeActionLeeroy(bool canRun)
{
	if (_aggroTarget != 0 && !_aggroTarget->isOut())
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
			return;
		}
	}
	int distance = 1000;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		int newDistance = Position::distance2d(_unit->getPosition(), bu->getPosition());
		if (!validTarget(bu, true, true))
			continue;
		//pick closest living unit
		if ((newDistance < distance || newDistance == 1) && !bu->isOut())
		{
			if (newDistance == 1 || selectPointNearTargetLeeroy(bu, canRun))
			{
				_aggroTarget = bu;
				_attackAction.type = BA_WALK;
				_attackAction.run = canRun;
				_unit->setCharging(_aggroTarget);
				distance = newDistance;
			}

		}
	}
	if (_aggroTarget != 0)
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
		}
	}
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "PlayerFactionAI::meleeAction:" << " [target]: " << (_aggroTarget->getId()) << " at: " << _attackAction.target; }
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "CHARGE!"; }
}

/**
 * Attempts to fire a waypoint projectile at an enemy we, or one of our teammates sees.
 *
 * Waypoint targeting: pick from any units currently spotted by our allies.
 */
void PlayerFactionAI::wayPointAction()
{
	BattleActionCost attackCost(BA_LAUNCH, _unit, _attackAction.weapon);
	if (!attackCost.haveTU())
	{
		// cannot make a launcher attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (_aggroTarget != 0) break; // loop finished
		if (!validTarget(bu, true, true))
		{
			continue;
		}
		_save->getPathfinding()->calculate(_unit, bu->getPosition(), BAM_MISSILE, bu, -1);
		BattleItem* ammo = _attackAction.weapon->getAmmoForAction(BA_LAUNCH);
		if (_save->getPathfinding()->getStartDirection() != -1 &&
			explosiveEfficacy(bu->getPosition(), _unit, ammo->getRules()->getExplosionRadius({ BA_LAUNCH, _unit, _attackAction.weapon, ammo }), _attackAction.diff))
		{
			_aggroTarget = bu;
		}
		_save->getPathfinding()->abortPath();
	}

	if (_aggroTarget != 0)
	{
		_attackAction.type = BA_LAUNCH;
		_attackAction.updateTU();
		if (!_attackAction.haveTU())
		{
			_attackAction.type = BA_RETHINK;
			return;
		}
		_attackAction.waypoints.clear();

		int PathDirection;
		int CollidesWith;
		int maxWaypoints = _attackAction.weapon->getCurrentWaypoints();
		if (maxWaypoints == -1)
		{
			maxWaypoints = 6 + (_attackAction.diff * 2);
		}
		Position LastWayPoint = _unit->getPosition();
		Position LastPosition = _unit->getPosition();
		Position CurrentPosition = _unit->getPosition();
		Position DirectionVector;

		_save->getPathfinding()->calculate(_unit, _aggroTarget->getPosition(), BAM_MISSILE, _aggroTarget, -1);
		PathDirection = _save->getPathfinding()->dequeuePath();
		while (PathDirection != -1 && (int)_attackAction.waypoints.size() < maxWaypoints)
		{
			LastPosition = CurrentPosition;
			_save->getPathfinding()->directionToVector(PathDirection, &DirectionVector);
			CurrentPosition = CurrentPosition + DirectionVector;
			Position voxelPosA ((CurrentPosition.x * 16)+8, (CurrentPosition.y * 16)+8, (CurrentPosition.z * 24)+16);
			Position voxelPosb ((LastWayPoint.x * 16)+8, (LastWayPoint.y * 16)+8, (LastWayPoint.z * 24)+16);
			CollidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosb, false, nullptr, _unit);
			if (CollidesWith > V_EMPTY && CollidesWith < V_UNIT)
			{
				_attackAction.waypoints.push_back(LastPosition);
				LastWayPoint = LastPosition;
			}
			else if (CollidesWith == V_UNIT)
			{
				BattleUnit* target = _save->getTile(CurrentPosition)->getOverlappingUnit(_save);
				if (target == _aggroTarget)
				{
					_attackAction.waypoints.push_back(CurrentPosition);
					LastWayPoint = CurrentPosition;
				}
			}
			PathDirection = _save->getPathfinding()->dequeuePath();
		}
		_attackAction.target = _attackAction.waypoints.front();
		if (LastWayPoint != _aggroTarget->getPosition())
		{
			_attackAction.type = BA_RETHINK;
		}
	}
}

/**
 * Attempts to fire at an enemy spotted for us.
 *
 */
bool PlayerFactionAI::sniperAction()
{
	if (_traceAI) { Log(LOG_INFO) << "Attempting sniper action..."; }

	if (selectSpottedUnitForSniper())
	{
		_visibleEnemies = std::max(_visibleEnemies, 1); // Make sure we count at least our target as visible, otherwise we might not shoot!
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
			if (_traceAI) { Log(LOG_INFO) << "Sniper action rejected because friendly unit is too close to the firing line."; }
			return false;
		}

		if (_traceAI) { Log(LOG_INFO) << "Target for sniper found at (" << _attackAction.target.x << "," << _attackAction.target.y << "," << _attackAction.target.z << ")."; }
		return true;
	}

	if (_traceAI) { Log(LOG_INFO) << "No valid target found or not enough TUs for sniper action."; }
	return false;
}

/**
 * Attempts to fire at an enemy we can see.
 *
 * Regular targeting: we can see an enemy, we have a gun, let's try to shoot.
 */
void PlayerFactionAI::projectileAction()
{
	_attackAction.target = _aggroTarget->getPosition();
	int distance = Position::distance2d(_unit->getPosition(), _attackAction.target);
	auto testEffect = [&](BattleActionCost& cost)
	{
		if (cost.haveTU())
		{
			BattleActionAttack attack = BattleActionAttack::GetBeforeShoot(cost);
			if (attack.damage_item == nullptr)
			{
				cost.clearTU();
			}
			else
			{
				int radius = attack.damage_item->getRules()->getExplosionRadius(attack);
				BattleAction riskAction = _attackAction;
				riskAction.type = cost.type;
				riskAction.actor = cost.actor;
				riskAction.weapon = cost.weapon;
				if (radius != 0 && explosiveProjectileRiskyForAllies(&riskAction, radius))
				{
					cost.clearTU();
				}
				else if (radius != 0 && explosiveEfficacy(_attackAction.target, _unit, radius, _attackAction.diff) == 0)
				{
					cost.clearTU();
				}
				else if (radius == 0 && directProjectileRiskyForAllies(&riskAction, _aggroTarget))
				{
					cost.clearTU();
				}
				else if (radius == 0 && autoShotRiskyForAllies(&riskAction, _aggroTarget))
				{
					cost.clearTU();
				}
			}
		}
	};

	_attackAction.type = BA_RETHINK;

	BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, _attackAction.weapon);

	testEffect(costAuto);
	testEffect(costSnap);
	testEffect(costAimed);

	// Is the unit willingly waiting outside of weapon's range (e.g. ninja camouflaged in ambush)?
	bool waitIfOutsideWeaponRange = _unit->getGeoscapeSoldier() ? false : _unit->getUnitRules()->waitIfOutsideWeaponRange();

	// Do we want to use the extended firing mode scoring?
	bool extendedFireModeChoiceEnabled = _save->getMod()->getAIExtendedFireModeChoice();
	if (!waitIfOutsideWeaponRange && extendedFireModeChoiceEnabled)
	{
		// Note: this will also check for the weapon's max range
		BattleActionCost costThrow; // Not actually checked here, just passed to extendedFireModeChoice as a necessary argument
		extendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, false);
		if (_attackAction.type != BA_RETHINK && projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}

	// Do we want to check if the weapon is in range?
	bool aiRespectsMaxRange = _save->getMod()->getAIRespectMaxRange();
	if (!waitIfOutsideWeaponRange && aiRespectsMaxRange)
	{
		// If we want to check and it's not in range, perhaps we should re-think shooting
		int distanceSq = _unit->distance3dToPositionSq(_attackAction.target);
		if (_attackAction.weapon->getRules()->isOutOfRange(distanceSq))
		{
			return;
		}
	}

	// vanilla
	if (distance < 4)
	{
		if (costAuto.haveTU())
		{
			_attackAction.type = BA_AUTOSHOT;
			if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
			{
				_attackAction.type = BA_RETHINK;
			}
			return;
		}
		if (!costSnap.haveTU())
		{
			if (costAimed.haveTU())
			{
				_attackAction.type = BA_AIMEDSHOT;
				if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
				{
					_attackAction.type = BA_RETHINK;
				}
			}
			return;
		}
		_attackAction.type = BA_SNAPSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}


	if (distance > 12)
	{
		if (costAimed.haveTU())
		{
			_attackAction.type = BA_AIMEDSHOT;
			if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
			{
				_attackAction.type = BA_RETHINK;
			}
			return;
		}
		if (distance < 20 && costSnap.haveTU())
		{
			_attackAction.type = BA_SNAPSHOT;
			if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
			{
				_attackAction.type = BA_RETHINK;
			}
			return;
		}
	}

	if (costSnap.haveTU())
	{
		_attackAction.type = BA_SNAPSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}
	if (costAimed.haveTU())
	{
		_attackAction.type = BA_AIMEDSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}
	if (costAuto.haveTU())
	{
		_attackAction.type = BA_AUTOSHOT;
		if (projectileRiskyForAllies(&_attackAction, _aggroTarget))
		{
			_attackAction.type = BA_RETHINK;
		}
	}
}

void PlayerFactionAI::extendedFireModeChoice(BattleActionCost& costAuto, BattleActionCost& costSnap, BattleActionCost& costAimed, BattleActionCost& costThrow, bool checkLOF)
{
	std::vector<BattleActionType> attackOptions = { };
	if (costAimed.haveTU())
	{
		attackOptions.push_back(BA_AIMEDSHOT);
	}
	if (costAuto.haveTU())
	{
		attackOptions.push_back(BA_AUTOSHOT);
	}
	if (costSnap.haveTU())
	{
		attackOptions.push_back(BA_SNAPSHOT);
	}
	if (costThrow.haveTU())
	{
		attackOptions.push_back(BA_THROW);
	}

	BattleActionType chosenAction = BA_RETHINK;
	BattleAction testAction = _attackAction;
	int score = 0;
	for (auto& i : attackOptions)
	{
		testAction.type = i;
		if (i == BA_THROW)
		{
			if (_grenade)
			{
				testAction.weapon = _unit->getGrenadeFromBelt(_save);
			}
			else
			{
				continue;
			}
		}
		else
		{
			testAction.weapon = _attackAction.weapon;
		}
		int newScore = scoreFiringMode(&testAction, _aggroTarget, checkLOF);

		// Add a random factor to the firing mode score based on intelligence
		// An intelligence value of 10 will decrease this random factor to 0
		// Default values for and intelligence value of 0 will make this a 50% to 150% roll
		int intelligenceModifier = _save->getMod()->getAIFireChoiceIntelCoeff() * std::max(10 - _unit->getIntelligence(), 0);
		newScore = newScore * (100 + RNG::generate(-intelligenceModifier, intelligenceModifier)) / 100;

		// More aggressive units get a modifier to the score for autoshots
		// Aggression = 0 lowers the score, aggro = 1 is no modifier, aggro > 1 bumps up the score by 5% (configurable) for each increment over 1
		if (i == BA_AUTOSHOT)
		{
			newScore = newScore * (100 + (_unit->getAggression() - 1) * _save->getMod()->getAIFireChoiceAggroCoeff()) / 100;
		}

		if (newScore > score)
		{
			score = newScore;
			chosenAction = i;
		}

		if (_traceAI)
		{
			Log(LOG_INFO) << "Evaluate option " << (int)i << ", score = " << newScore;
		}
	}

	_attackAction.type = chosenAction;
	if (_attackAction.type != BA_RETHINK && projectileRiskyForAllies(&_attackAction, _aggroTarget))
	{
		_attackAction.type = BA_RETHINK;
	}
}

int PlayerFactionAI::scorePlayerGrenadeTarget(BattleItem *grenade, const Position &targetPos, int radius, bool proximity) const
{
	if (!grenade || radius <= 0 || !_save->getTile(targetPos))
	{
		return -100000;
	}
	BattleAction action;
	action.actor = _unit;
	action.weapon = grenade;
	action.type = BA_THROW;
	action.target = targetPos;
	if (explosiveProjectileRiskyForAllies(&action, radius, 0, false))
	{
		return -100000;
	}

	const int power = std::max(0, grenade->getRules()->getPower());
	int enemiesAffected = 0;
	int score = proximity ? 20 : 0;
	for (auto *bu : *_save->getUnits())
	{
		if (!bu || bu->isOut())
		{
			continue;
		}
		if (abs(bu->getPosition().z - targetPos.z) > Options::battleExplosionHeight)
		{
			continue;
		}
		const int dist = Position::distance2d(bu->getPosition(), targetPos);
		if (dist > radius)
		{
			continue;
		}
		if (bu->getFaction() == _unit->getFaction())
		{
			return -100000;
		}
		if (validTarget(bu, true, true))
		{
			++enemiesAffected;
			const int armor = std::max(std::max(bu->getArmor(SIDE_FRONT), bu->getArmor(SIDE_LEFT)), bu->getArmor(SIDE_RIGHT));
			int unitScore = 90;
			unitScore += std::max(0, power - armor / 2);
			unitScore += std::max(0, 90 - bu->getHealth());
			if (bu->getHealth() > 70 || armor >= power / 2)
			{
				unitScore += 45;
			}
			unitScore -= dist * 8;
			score += unitScore;
		}
	}

	if (!proximity)
	{
		const int oldEfficacy = explosiveEfficacy(targetPos, _unit, radius, _attackAction.diff, true);
		score += oldEfficacy * 120;
		if (enemiesAffected == 0)
		{
			return -100000;
		}
		if (enemiesAffected == 1)
		{
			BattleUnit *target = _save->getTile(targetPos)->getUnit();
			if (!target || target->getFaction() == _unit->getFaction())
			{
				score -= 80;
			}
			else if (target->getHealth() >= 60)
			{
				score += 75;
			}
		}
	}
	return score;
}

/**
 * Evaluates whether to throw a grenade at an enemy (or group of enemies) we can see.
 */
void PlayerFactionAI::grenadeAction()
{
	BattleAction bestAction;
	bestAction.type = BA_RETHINK;
	bestAction.actor = _unit;
	int bestScore = -100000;
	std::string bestReason;

	auto tryGrenadeTarget = [&](BattleItem *grenade, const Position &baseTarget, const std::string &reason)
	{
		if (!grenade || !grenade->getRules()->isGrenadeOrProxy() || _save->getTurn() < grenade->getRules()->getAIUseDelay(_save->getMod()))
		{
			return;
		}
		BattleAction action;
		action.weapon = grenade;
		action.type = BA_THROW;
		action.actor = _unit;
		action.target = baseTarget;
		action.updateTU();
		action.Time += 4;
		action += _unit->getActionTUs(BA_PRIME, grenade);
		if (!action.haveTU() || !_save->getTile(baseTarget))
		{
			return;
		}
		int radius = grenade->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
		if (radius <= 0)
		{
			return;
		}
		const bool proximity = grenade->getRules()->getBattleType() == BT_PROXIMITYGRENADE;
		std::vector<std::pair<Position, int>> shifts;
		if (proximity)
		{
			const Position candidates[9] = { Position(0, 0, 0), Position(1, 0, 0), Position(0, 1, 0), Position(-1, 0, 0), Position(0, -1, 0), Position(1, 1, 0), Position(1, -1, 0), Position(-1, 1, 0), Position(-1, -1, 0) };
			for (const auto &candidate : candidates)
			{
				Position shifted = baseTarget + candidate;
				if (shifted.x >= 0 && shifted.x < _save->getMapSizeX() && shifted.y >= 0 && shifted.y < _save->getMapSizeY() && shifted.z >= 0 && shifted.z < _save->getMapSizeZ())
				{
					shifts.push_back(std::make_pair(candidate, _unit->distance3dToPositionSq(shifted)));
				}
			}
			std::sort(shifts.begin(), shifts.end(), [](auto& left, auto& right) {
				return left.second < right.second;
			});
		}
		else
		{
			for (int dx = -2; dx <= 2; ++dx)
			{
				for (int dy = -2; dy <= 2; ++dy)
				{
					Position candidate(dx, dy, 0);
					Position shifted = baseTarget + candidate;
					if (shifted.x >= 0 && shifted.x < _save->getMapSizeX() && shifted.y >= 0 && shifted.y < _save->getMapSizeY() && shifted.z >= 0 && shifted.z < _save->getMapSizeZ())
					{
						shifts.push_back(std::make_pair(candidate, abs(dx) + abs(dy)));
					}
				}
			}
			std::sort(shifts.begin(), shifts.end(), [](auto& left, auto& right) {
				return left.second < right.second;
			});
		}
		Position originVoxel = _save->getTileEngine()->getOriginVoxel(action, 0);
		BattleUnit *baseTargetUnit = 0;
		Tile *baseTile = _save->getTile(baseTarget);
		if (baseTile)
		{
			baseTargetUnit = baseTile->getUnit();
		}
		bool grenadeSolvesBadDirectFire = false;
		if (!proximity && baseTargetUnit && validTarget(baseTargetUnit, true, true))
		{
			BattleItem *directWeapon = selectBestCarriedWeapon();
			if (directWeapon && directWeapon != grenade && directWeapon->getRules()->getBattleType() == BT_FIREARM)
			{
				BattleAction directAction;
				directAction.actor = _unit;
				directAction.weapon = directWeapon;
				directAction.type = BA_SNAPSHOT;
				directAction.target = baseTargetUnit->getPosition();
				Position shotOrigin = _save->getTileEngine()->getOriginVoxel(directAction, 0);
				Position shotTarget = baseTargetUnit->getPosition().toVoxel() + Position(8, 8, 10);
				Tile *directTile = baseTargetUnit->getTile();
				const bool directLine = directTile && _save->getTileEngine()->canTargetUnit(&shotOrigin, directTile, &shotTarget, _unit, false, baseTargetUnit);
				BattleActionAttack directAttack = BattleActionAttack::GetBeforeShoot(directAction);
				const int directPower = directAttack.damage_item ? std::max(0, directAttack.damage_item->getRules()->getPower()) : 0;
				const int armor = std::max(std::max(baseTargetUnit->getArmor(SIDE_FRONT), baseTargetUnit->getArmor(SIDE_LEFT)), baseTargetUnit->getArmor(SIDE_RIGHT));
				const bool weakDirectHit = directPower > 0 && (directPower + 20 < armor || (baseTargetUnit->getHealth() > 55 && directPower < armor + 35));
				grenadeSolvesBadDirectFire = !directLine || projectileRiskyForAllies(&directAction, baseTargetUnit, false) || weakDirectHit;
			}
		}
		for (auto& shift : shifts)
		{
			Position targetTile = baseTarget + shift.first;
			Tile *tile = _save->getTile(targetTile);
			if (!tile)
			{
				continue;
			}
			Position targetVoxel = targetTile.toVoxel() + Position(8,8, (2 + -tile->getTerrainLevel()));
			if (_save->getTileEngine()->validateThrow(action, originVoxel, targetVoxel, _save->getDepth()))
			{
				int score = scorePlayerGrenadeTarget(grenade, targetTile, radius, proximity);
				if (proximity)
				{
					score += 70;
					score -= Position::distance2d(targetTile, _unit->getPosition()) * 2;
					if (reason.find("room_entry") != std::string::npos)
					{
						score += 110;
					}
					else if (reason.find("hidden_contact") != std::string::npos)
					{
						score += 70;
					}
					if (getSpottingUnits(_unit->getPosition()) > 0 && score < 180)
					{
						score -= 45;
					}
				}
				else
				{
					const int currentSpotters = getSpottingUnits(_unit->getPosition());
					const int currentExposure = getEnemyFireExposure(_unit->getPosition());
					if (grenadeSolvesBadDirectFire)
					{
						score += 110;
					}
					if (currentSpotters > 0 && score < 160)
					{
						score -= currentSpotters * (grenadeSolvesBadDirectFire ? 35 : 70);
					}
					if (currentExposure > 0 && score < 190)
					{
						score -= std::min(grenadeSolvesBadDirectFire ? 45 : 90, currentExposure / (grenadeSolvesBadDirectFire ? 4 : 2));
					}
				}
				if (score > bestScore)
				{
					bestScore = score;
					bestAction = action;
					bestAction.target = targetTile;
					bestReason = reason;
				}
			}
		}
	};

	for (auto *item : *_unit->getInventory())
	{
		if (!item || !item->getRules()->isGrenadeOrProxy())
		{
			continue;
		}
		if (_aggroTarget && !_aggroTarget->isOut())
		{
			tryGrenadeTarget(item, _aggroTarget->getPosition(), "visible_or_assigned_target");
		}
		for (auto *bu : *_save->getUnits())
		{
			if (validTarget(bu, true, true) && bu->getTile() && _save->getTileEngine()->visible(_unit, bu->getTile()))
			{
				tryGrenadeTarget(item, bu->getPosition(), "visible_enemy_cluster");
			}
		}
		if (_factionAI)
		{
			Position contactPos;
			const BattleRoomInfo *room = 0;
			int enemiesInRoom = 0;
			bool visibleContact = false;
			if (_factionAI->getBestEnemyContactPosition(&contactPos, &room, &enemiesInRoom, &visibleContact))
			{
				if (!visibleContact)
				{
					tryGrenadeTarget(item, contactPos, item->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? "hidden_contact_sensor_grenade" : "hidden_contact_explosive");
				}
				if (room && item->getRules()->getBattleType() == BT_PROXIMITYGRENADE)
				{
					for (const auto &entry : room->entryPositions)
					{
						if (entry.z == _unit->getPosition().z && Position::distance2d(entry, _unit->getPosition()) <= 16)
						{
							tryGrenadeTarget(item, entry, "room_entry_sensor_grenade");
						}
					}
				}
			}
		}
	}

	if (bestAction.type != BA_RETHINK && bestScore >= (bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE ? 120 : 70))
	{
		_attackAction.weapon = bestAction.weapon;
		_attackAction.target = bestAction.target;
		_attackAction.type = BA_THROW;
		_rifle = false;
		_melee = false;
		if (Options::autoBattleLog)
		{
			std::ostringstream log;
			log << "Player faction explosive action: unit=" << _unit->getId()
				<< ", item=" << bestAction.weapon->getRules()->getType()
				<< ", target=" << bestAction.target
				<< ", score=" << bestScore
				<< ", reason=" << bestReason
				<< ", proximity=" << (bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE);
			_save->appendToAutoBattleLog(log.str());
		}
	}
	else if (Options::autoBattleLog && bestAction.type != BA_RETHINK)
	{
		std::ostringstream log;
		log << "Player faction explosive candidate rejected: unit=" << _unit->getId()
			<< ", item=" << bestAction.weapon->getRules()->getType()
			<< ", target=" << bestAction.target
			<< ", score=" << bestScore
			<< ", reason=" << bestReason
			<< ", proximity=" << (bestAction.weapon->getRules()->getBattleType() == BT_PROXIMITYGRENADE);
		_save->appendToAutoBattleLog(log.str());
	}
	else if (Options::autoBattleLog)
	{
		int carriedExplosives = 0;
		for (auto *item : *_unit->getInventory())
		{
			if (item && item->getRules()->isGrenadeOrProxy())
			{
				++carriedExplosives;
			}
		}
		if (carriedExplosives > 0)
		{
			std::ostringstream log;
			log << "Player faction explosive no target: unit=" << _unit->getId()
				<< ", carriedExplosives=" << carriedExplosives
				<< ", aggroTarget=" << (_aggroTarget ? _aggroTarget->getId() : -1)
				<< ", known=" << _knownEnemies
				<< ", visible=" << _visibleEnemies
				<< ", spotting=" << _spottingEnemies;
			_save->appendToAutoBattleLog(log.str());
		}
	}
}

/**
 * Attempts a psionic attack on an enemy we "know of".
 *
 * Psionic targetting: pick from any of the "exposed" units.
 * Exposed means they have been previously spotted, and are therefore "known" to the AI,
 * regardless of whether we can see them or not, because we're psychic.
 * @return True if a psionic attack is performed.
 */
bool PlayerFactionAI::psiAction()
{
	BattleItem *item = _unit->getUtilityWeapon(BT_PSIAMP);
	if (!item)
	{
		return false;
	}

	const int costLength = 3;
	BattleActionCost cost[costLength] =
	{
		BattleActionCost(BA_USE, _unit, item),
		BattleActionCost(BA_PANIC, _unit, item),
		BattleActionCost(BA_MINDCONTROL, _unit, item),
	};
	bool have = false;
	for (int j = 0; j < costLength; ++j)
	{
		if (cost[j].Time > 0)
		{
			cost[j].Time += _escapeTUs;
			cost[j].Energy += _escapeTUs / 2;
			have |= cost[j].haveTU();
		}
	}
	bool LOSRequired = item->getRules()->isLOSRequired();

	_aggroTarget = 0;
		// don't let mind controlled soldiers mind control other soldiers.
	if (_unit->getOriginalFaction() == _unit->getFaction()
		// and we have the required 25 TUs and can still make it to cover
		&& have
		// and we didn't already do a psi action this round
		&& !_didPsi)
	{
		int weightToAttack = 0;
		BattleActionType typeToAttack = BA_NONE;

		for (auto* bu : *_save->getUnits())
		{
			// don't target tanks
			if (bu->getArmor()->getSize() == 1 &&
				validTarget(bu, true, false) &&
				// they must be player units
				bu->getOriginalFaction() != _unit->getFaction() &&
				(!LOSRequired ||
				std::find(_unit->getVisibleUnits()->begin(), _unit->getVisibleUnits()->end(), bu) != _unit->getVisibleUnits()->end()))
			{
				BattleUnit *victim = bu;
				if (item->getRules()->isOutOfRange(_unit->distance3dToUnitSq(victim)))
				{
					continue;
				}
				for (int j = 0; j < costLength; ++j)
				{
					// can't use this attack.
					if (!cost[j].haveTU())
					{
						continue;
					}

					int weightToAttackMe = _save->getTileEngine()->psiAttackCalculate({ cost[j].type, _unit, item, item }, victim);

					// low chance we hit this target.
					if (weightToAttackMe < 0)
					{
						continue;
					}

					// different bonus per attack.
					if (cost[j].type == BA_MINDCONTROL)
					{
						// target cannot be mind controlled
						if (victim->getUnitRules() && !victim->getUnitRules()->canBeMindControlled()) continue;

						int controlOdds = 40;
						int morale = victim->getMorale();
						int bravery = victim->reduceByBravery(10);
						if (bravery > 6)
							controlOdds -= 15;
						if (bravery < 4)
							controlOdds += 15;
						if (morale >= 40)
						{
							if (morale - 10 * bravery < 50)
								controlOdds -= 15;
						}
						else
						{
							controlOdds += 15;
						}
						if (!morale)
						{
							controlOdds = 100;
						}
						if (RNG::percent(controlOdds))
						{
							weightToAttackMe += 60;
						}
						else
						{
							continue;
						}
					}
					else if (cost[j].type == BA_USE)
					{
						if (RNG::percent(80 - _attackAction.diff * 10)) // Star gods have mercy on us.
						{
							continue;
						}
						BattleActionAttack attack = BattleActionAttack{ BA_USE, _unit, item, item };
						int radius = item->getRules()->getExplosionRadius(attack);
						if (radius > 0)
						{
							int efficity = explosiveEfficacy(victim->getPosition(), _unit, radius, _attackAction.diff);
							if (efficity)
							{
								weightToAttackMe += 2 * efficity * _intelligence; //bonus for boom boom.
							}
							else
							{
								continue;
							}
						}
						else
						{
							weightToAttackMe += item->getRules()->getPowerBonus(attack);
						}
					}
					else if (cost[j].type == BA_PANIC)
					{
						// target cannot be panicked
						if (victim->getUnitRules() && !victim->getUnitRules()->canPanic()) continue;

						weightToAttackMe += 40;
					}

					if (weightToAttackMe > weightToAttack)
					{
						typeToAttack = cost[j].type;
						weightToAttack = weightToAttackMe;
						_aggroTarget = victim;
					}
				}
			}
		}

		if (!_aggroTarget || !weightToAttack) return false;

		if (_visibleEnemies && _attackAction.weapon)
		{
			BattleActionType actions[] = {
				BA_AIMEDSHOT,
				BA_AUTOSHOT,
				BA_SNAPSHOT,
				BA_HIT,
			};
			for (BattleActionType action : actions)
			{
				auto* ammo = _attackAction.weapon->getAmmoForAction(action);
				if (!ammo)
				{
					continue;
				}

				int weightPower = ammo->getRules()->getPowerBonus({ action, _attackAction.actor, _attackAction.weapon, ammo });
				if (action == BA_HIT)
				{
					// prefer psi over melee
					weightPower /= 2;
				}
				else
				{
					// prefer machine guns
					weightPower *= _attackAction.weapon->getActionConf(action)->shots;
				}
				if (weightPower >= weightToAttack)
				{
					return false;
				}
			}
		}
		else if (RNG::generate(35, 155) >= weightToAttack)
		{
			return false;
		}

		if (_traceAI)
		{
			Log(LOG_INFO) << "making a psionic attack this turn";
		}

		_psiAction.type = typeToAttack;
		_psiAction.target = _aggroTarget->getPosition();
		_psiAction.weapon = item;
		return true;
	}
	return false;
}

/**
 * Performs a melee attack action.
 */
void PlayerFactionAI::meleeAttack()
{
	_unit->lookAt(_aggroTarget->getPosition() + Position(_unit->getArmor()->getSize()-1, _unit->getArmor()->getSize()-1, 0), false);
	while (_unit->getStatus() == STATUS_TURNING)
		_unit->turn();
	if (_traceAI) { Log(LOG_INFO) << "Attack unit: " << _aggroTarget->getId(); }
	_attackAction.target = _aggroTarget->getPosition();
	_attackAction.type = BA_HIT;
	_attackAction.weapon = _unit->getUtilityWeapon(BT_MELEE);
}


/**
 *
 * @param target
 * @return
 */
AIAttackWeight PlayerFactionAI::getTargetAttackWeight(BattleUnit* target) const
{
	AIAttackWeight weight = AIW_IGNORED;

	if (target->getFaction() == _unit->getFaction())
	{
		// friendly target have negative weight, used for AoE attacks.
		weight = target->getAITargetWeightAsFriendly(_save->getMod());
	}
	else if (
		_intelligence < target->getTurnsSinceSpottedByFaction(_unit->getFaction()) &&
		(!_unit->isSniper() || !target->getTurnsLeftSpottedForSnipersByFaction(_unit->getFaction())))
	{
		// ignore units that we don't "know" about...
		// ... unless we are a sniper and the spotters know about them
		weight = AIW_IGNORED;
	}
	else if (target->getFaction() == FACTION_HOSTILE || _unit->getFaction() == FACTION_HOSTILE)
	{
		if (target->getFaction() == _targetFaction)
		{
			// enemy unit, full weight
			weight = target->getAITargetWeightAsHostile(_save->getMod());
		}
		else
		{
			// if its not xcom unit then its civilian, less value that xcom
			weight = target->getAITargetWeightAsHostileCivilians(_save->getMod());
		}
	}
	else if (target->getFaction() == FACTION_NEUTRAL || _unit->getFaction() == FACTION_NEUTRAL)
	{
		// if its not alien then its xcom or civilian, humans do not shoot each other, usually...
		weight = target->getAITargetWeightAsNeutral(_save->getMod());
	}

	weight = (AIAttackWeight)ModScript::scriptFunc2<ModScript::AiCalculateTargetWeight>(
		_unit->getArmor(),
		weight, weight,
		_unit, target, _save
	);

	return weight;
}

/**
 * Validates a target.
 * @param target the target we want to validate.
 * @param assessDanger do we care if this unit was previously targetted with a grenade?
 * @param includeCivs do we include civilians in the threat assessment?
 * @return whether this target is someone we would like to kill.
 */
bool PlayerFactionAI::validTarget(BattleUnit *target, bool assessDanger, bool includeCivs) const
{
	// ignore units that:
	// 1. are dead/unconscious
	// 2. are dangerous (they have been grenaded)
	// 3. are hostile/neutral units marked as ignored by the AI
	if (target->isOut() ||
		(assessDanger && target->getTile()->getDangerous()) ||
		(target->getFaction() != FACTION_PLAYER && target->isIgnoredByAI()))
	{
		return false;
	}

	if (includeCivs)
	{
		return  getTargetAttackWeight(target) > AIW_IGNORED;
	}
	else
	{
		return  getTargetAttackWeight(target) > _save->getMod()->getAITargetWeightThreatThreshold();
	}
}

/**
 * Checks the alien's reservation setting.
 * @return the reserve setting.
 */
BattleActionType PlayerFactionAI::getReserveMode()
{
	return _reserve;
}

/**
 * We have a dichotomy on our hands: we have a ranged weapon and melee capability.
 * let's make a determination on which one we'll be using this round.
 */
void PlayerFactionAI::selectMeleeOrRanged()
{
	BattleItem *range = _attackAction.weapon;
	BattleItem *melee = _unit->getUtilityWeapon(BT_MELEE);

	if (!melee || !melee->haveAnyAmmo())
	{
		// no idea how we got here, but melee is definitely out of the question.
		_melee = false;
		return;
	}
	if (!range || !range->haveAnyAmmo())
	{
		_rifle = false;
		return;
	}

	const RuleItem *meleeRule = melee->getRules();

	int meleeOdds = 10;

	int dmg = _aggroTarget->reduceByResistance(meleeRule->getPowerBonus(BattleActionAttack::GetBeforeShoot(BA_HIT, _unit, melee)), meleeRule->getDamageType()->ResistType);

	if (dmg > 50)
	{
		meleeOdds += (dmg - 50) / 2;
	}
	if ( _visibleEnemies > 1 )
	{
		meleeOdds -= 20 * (_visibleEnemies - 1);
	}

	if (meleeOdds > 0 && _unit->getHealth() >= 2 * _unit->getBaseStats()->health / 3)
	{
		if (_unit->getAggression() == 0)
		{
			meleeOdds -= 20;
		}
		else if (_unit->getAggression() > 1)
		{
			meleeOdds += 10 * _unit->getAggression();
		}

		if (RNG::percent(meleeOdds))
		{
			_rifle = false;
			_attackAction.weapon = melee;
			_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_HIT, _unit, melee));
			return;
		}
	}
	_melee = false;
}

/**
 * Checks nearby nodes to see if they'd make good grenade targets
 * @param action contains our details one weapon and user, and we set the target for it here.
 * @return if we found a viable node or not.
 */
bool PlayerFactionAI::getNodeOfBestEfficacy(BattleAction *action, int radius)
{
	int bestScore = 2;
	Position originVoxel = _save->getTileEngine()->getSightOriginVoxel(_unit);
	Position targetVoxel;
	for (const auto* node : *_save->getNodes())
	{
		if (node->isDummy())
		{
			continue;
		}
		int dist = Position::distance2d(node->getPosition(), _unit->getPosition());
		if (dist <= 20 && dist > radius &&
			_save->getTileEngine()->canTargetTile(&originVoxel, _save->getTile(node->getPosition()), O_FLOOR, &targetVoxel, _unit, false))
		{
			int nodePoints = 0;
			for (auto* bu : *_save->getUnits())
			{
				dist = Position::distance2d(node->getPosition(), bu->getPosition());
				if (!bu->isOut() && dist < radius)
				{
					Position targetOriginVoxel = _save->getTileEngine()->getSightOriginVoxel(bu);
					if (_save->getTileEngine()->canTargetTile(&targetOriginVoxel, _save->getTile(node->getPosition()), O_FLOOR, &targetVoxel, bu, false))
					{
						if ((_unit->getFaction() == FACTION_HOSTILE && bu->getFaction() != FACTION_HOSTILE) ||
							(_unit->getFaction() == FACTION_NEUTRAL && bu->getFaction() == FACTION_HOSTILE))
						{
							if (bu->getTurnsSinceSpottedByFaction(_unit->getFaction()) <= _intelligence)
							{
								nodePoints++;
							}
						}
						else
						{
							nodePoints -= 2;
						}
					}
				}
			}
			if (nodePoints > bestScore)
			{
				bestScore = nodePoints;
				action->target = node->getPosition();
			}
		}
	}
	return bestScore > 2;
}

BattleUnit* PlayerFactionAI::getTarget()
{
	return _aggroTarget;
}

void PlayerFactionAI::freePatrolTarget()
{
	if (_toNode)
	{
		_toNode->freeNode();
	}
}

}
