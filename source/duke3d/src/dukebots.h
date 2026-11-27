//-------------------------------------------------------------------------
/*
This file is part of Duke3D Chosen (EDuke32 with the NotBlood Chosen features).
GPL 2, see package/common/gpl-2.0.txt.
*/
//-------------------------------------------------------------------------

// Computer players ("bots") for Duke Nukem 3D, after NotBlood Chosen's. A bot is a whole player of a
// local multiplayer game (the players after you): every tic it fills in that player's input from
// what it sees - the way you would with keys - and the game does the rest. They find their way
// through the level (sectors, doors, lifts), pick up weapons, ammo and health, fight with the weapon
// that suits the distance, strafe and dodge, and play DukeMatch, Team DukeMatch and co-op (where they
// fight the monsters and stay near you).

#pragma once

#include "compat.h"

enum
{
    kBotMaxBots = 7,
    kBotSkillMax = 4,     // easy, normal, hard, insane
};

// match types on top of DukeMatch (bot matches)
enum
{
    kBotModeDukeMatch = 0,
    kBotModeGunGame,      // every kill: the next weapon; a kill with the last (the boot) wins
    kBotModeHill,         // a point a second for whoever holds the hill alone; 60 win
    kBotModeLastMan,      // 3 lives each; the last one left wins
    kBotModeMax,
};

extern int32_t gBotCount;   // PLAY WITH BOTS: how many bots
extern int32_t gBotMode;    // kBotMode...
extern int32_t gBotSkill;   // 0..kBotSkillMax-1

// a bot match is running (the players after the first are bots)
bool botMatchActive(void);
bool botIsBot(int nPlayer);

// starts a match with the bots and the game options of the menu (game type, episode, level, monsters)
void botStartMatch(void);
// back to playing alone (a new game, a network game, the title screen)
void botEndMatch(void);

// every tic, before the players' input is used (G_DoMoveThings): the bots' input
void botThinkAll(void);
// a level started: the bots learn it (sectors, items)
void botLevelStart(void);

const char *botSkillName(int nSkill);
const char *botModeName(int nMode);

// a player died in a bot match (nKiller: the player who killed them, -1: anything else)
void botOnDeath(int nPlayer, int nKiller);
