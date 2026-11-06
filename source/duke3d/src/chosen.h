//-------------------------------------------------------------------------
/*
This file is part of Duke3D Chosen (EDuke32 with the NotBlood Chosen features).

Duke3D Chosen is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/
//-------------------------------------------------------------------------

// Duke3D Chosen: online lobbies through a relay server, a live dashboard, FIND LAN GAMES and
// a MULTIPLAYER menu (chosen.cpp). Everybody in a network game needs the same version.

#pragma once

#define CHOSEN_NAME "Duke3D Chosen"
#define CHOSEN_VERSION "1.0"

enum
{
    kOnlineNone = 0,
    kOnlineHost,    // hosting a lobby: remote players come in through the relay
    kOnlineClient,  // joined a lobby
    kOnlineReport,  // LAN game whose stats are shown on the server's dashboard
};

extern char gOnlineServer[64];
extern char gOnlineKey[48];
extern char gOnlineLobby[25];
extern int32_t gOnlineReportLan;  // show hosted LAN games on the dashboard
extern int gOnlineMode;

// the version string players have to match ("Duke3D Chosen 1.0")
const char *onlineVersion(void);
const char *onlineStatusText(void);
void onlineSetStatus(const char *pzText);

// ---- the MULTIPLAYER menu ----
// host a game: bOnline opens a lobby on the relay server first. Starts the level picked in the
// game options; players can join while it runs.
bool chosenHostGame(bool bOnline);
// join a game on the LAN (address "host" or "host:port") or a lobby on the relay server
bool chosenJoinAddress(const char *pzAddress);
bool chosenJoinLobby(const char *pzLobby);
// a network game is being left (quit to title): closes the connections
void chosenLeaveGame(void);

// lists for the menus: names and info lines; count, or -1 when it didn't work (see onlineStatusText)
enum { kChosenListMax = 8 };
struct CHOSENLISTENTRY
{
    char szName[25];
    char szInfo[64];
    char szAddress[48];
    int nPort;
};
int chosenListLobbies(CHOSENLISTENTRY *pList, int nMax);
void chosenLanScanStart(void);
int chosenLanScanPoll(CHOSENLISTENTRY *pList, int nMax);  // games found so far
void chosenLanScanStop(void);

// called every frame by the game loop: relay messages, LAN answers, stats for the dashboard
void chosenFrame(void);
// text shown under the host's frags: the address or lobby others need (NULL when none)
const char *chosenHostInfo(void);
