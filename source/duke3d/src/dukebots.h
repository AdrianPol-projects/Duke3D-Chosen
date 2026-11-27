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
    kBotModeLastMan,      // 3 lives each; the last one left wins (teams: the last team)
    kBotModeJuggernaut,   // one player is the juggernaut (300 health): who kills them becomes it; 10 points win
    kBotModeCaptureFlag,  // teams: bring the other team's key card home while yours is there; 3 captures win
    kBotModeHideSeek,     // hiders look like the level's decorations; seekers come after 15 s; the found join them
    kBotModeSurvival,     // co-op: waves of monsters, 3 lives each (back every wave)
    kBotModeMax,
};

extern int32_t gBotCount;   // PLAY WITH BOTS: how many bots
extern int32_t gBotMode;    // kBotMode...
extern int32_t gBotSkill;   // 0..kBotSkillMax-1
extern int32_t gBotNetCount;  // bots in a hosted network game (LAN or online)

// a bot match is running (the players after the first are bots)
bool botMatchActive(void);
bool botIsBot(int nPlayer);

// starts a match with the bots and the game options of the menu (game type, episode, level, monsters)
void botStartMatch(void);
// back to playing alone (a new game, a network game, the title screen)
void botEndMatch(void);
// loading a saved bot game (co-op with bots, a bot match): the players it was saved with
bool botPrepareLoad(int nPlayers);

// every tic, before the players' input is used (G_DoMoveThings): the bots' input
void botThinkAll(void);
// a level started: the bots learn it (sectors, items)
void botLevelStart(void);

const char *botSkillName(int nSkill);
const char *botModeName(int nMode);

// a player died in a bot match (nKiller: the player who killed them, -1: anything else)
void botOnDeath(int nPlayer, int nKiller);

// network games (LAN, online lobbies): the host adds gBotNetCount bots as players without a connection
// and plays them; every machine sees them as players (the scores, the website's live view)
void botNetHostStart(void);
// a player joined: they learn the bots' names
void botNetPlayerJoined(void);
// the game is full and a player wants in: a bot leaves (false: no bots)
bool botNetMakeRoom(void);
bool botNetGameHasBots(void);
int botNetCount(void);

// match types in network games: the host sends the match to everybody; every machine shows it
void botReceiveModeState(uint8_t const *buf, int nLength);
int botModeRunning(void);         // the match type (-1: none)
void botDrawPlayers(void);        // after G_DoSpriteAnimations: hiders, the juggernaut
void botDrawHud(void);            // the match's line on the screen
void botFixGameType(int nMatch);  // ud.m_coop for the match type (co-op for survival, teams for the flags)
