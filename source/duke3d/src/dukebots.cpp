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

// Bots for Duke Nukem 3D; see dukebots.h. The design follows NotBlood Chosen's bots (bot.cpp there):
// a bot perceives (sees an enemy, with a reaction time), picks a goal (an enemy, an item it needs,
// where an enemy was last seen, a teammate to stay with, somewhere new), finds a path through the
// sectors and steers along it, and fights with the weapon that suits the distance - with the aim
// error, turning speed and reaction time of its difficulty, so it plays like a person, not a turret.

#include "duke3d.h"
#include "dukebots.h"
#include "cmdline.h"
#include "savegame.h"

#define NORMALKEYMOVE 40  // as in player.cpp: walking speed of a key press

int32_t gBotNetCount;
int32_t gBotCount = 3;
int32_t gBotSkill = 1;
int32_t gBotMode = kBotModeDukeMatch;

namespace {

bool bActive;
bool isBot[MAXPLAYERS];  // the player slots the bots play (a bot match: all after you; a network game: the host's bots)
int nNetBots;            // bots in the network game this machine hosts

// ---- difficulty -------------------------------------------------------------------------------

struct SKILL
{
    const char *pzName;
    int nReaction;   // tics before reacting to a newly seen enemy
    int nAimError;   // build angle units (2048 = a full turn) of aim wobble
    int nTurn;       // most the bot turns per tic
    int nFireCone;   // fires when the enemy is within this many angle units of its aim
    int nStrafe;     // percent: how much it dodges while fighting
};

SKILL const kSkills[kBotSkillMax] =
{
    { "Easy",    22, 70,  45, 60, 40 },
    { "Normal",  13, 40,  80, 48, 70 },
    { "Hard",     8, 20, 130, 40, 90 },
    { "Insane",   4,  9, 200, 32, 100 },
};

const char *const kBotNames[] = { "Rex", "Vixen", "Tank", "Ziggy", "Hound", "Blaze", "Viper", "Mojo", "Duchess", "Spike",
    "Rocco", "Sable", "Butch", "Jinx" };

// ---- state of one bot ---------------------------------------------------------------------------

enum { kMaxPath = 64 };

struct BOT
{
    uint32_t nSeed;
    int nTarget;          // sprite it is fighting (-1: none)
    int nTargetSince;     // tics it has seen the target (reaction time)
    int nLastSeenX, nLastSeenY, nLastSeenZ, nLastSeenSect, nLastSeenTime;
    int nGoalX, nGoalY, nGoalSect;   // where it walks to
    int nGoalKind;        // 0 roam, 1 item, 2 hunt, 3 follow
    int nGoalItem;
    int nGoalTime;        // tic the goal was set
    int16_t path[kMaxPath];
    int nPath, nPathPos;
    int nStuckX, nStuckY, nStuckTime, nStuck;
    int nStrafeDir, nStrafeUntil;
    int nAimOffset, nAimUntil;
    int nWantWeapon;
    int nJumpUntil, nOpenTime, nRespawnTime;
    int nIgnoreItem[8];
    int nIgnoreTime[8];
    // statistics for the debug log
    int nStatFight, nStatSeen, nStatWalk, nStatDead, nStatPathFail, nStatStuck, nStatFire, nStatDamage;
    int nLastTarget, nLastTargetHealth;
    int nAreaSize, nAreaTime;   // how many sectors it can walk to from where it is (checked now and then)
    int nBreak;                 // something it shoots to get out of a closed area (-1: none)
    int nBreakHealth, nBreakSince;
    bool bWasDead;
    int nRespawnTries;
};

BOT bots[MAXPLAYERS];
int nTic;

int BotRand(BOT *pBot, int nRange)
{
    pBot->nSeed = pBot->nSeed * 1103515245u + 12345u;
    return nRange > 0 ? (int)((pBot->nSeed >> 8) % (uint32_t)nRange) : 0;
}

int AngDiff(int a, int b)
{
    return ((a - b + 1024) & 2047) - 1024;
}

int Dist2D(int dx, int dy)
{
    return (int)sqrt((double)dx * dx + (double)dy * dy);
}

DukePlayer_t *PlayerOf(int nPlayer)
{
    return g_player[nPlayer].ps;
}

bool PlayerAlive(int nPlayer)
{
    auto *ps = PlayerOf(nPlayer);
    return ps && ps->i >= 0 && sprite[ps->i].extra > 0 && !ps->dead_flag;
}

bool TeamGame(void)
{
    return (g_gametypeFlags[ud.coop] & GAMETYPE_TDM) != 0;
}

bool CoopGame(void)
{
    return (g_gametypeFlags[ud.coop] & GAMETYPE_COOP) != 0;
}

int PlayerTeam(int nPlayer)
{
    return g_player[nPlayer].pteam;
}

// ---- the level: sectors as a graph ---------------------------------------------------------

int sectCenterX[MAXSECTORS], sectCenterY[MAXSECTORS];
int nLevelId = -1;

bool IsDoorSector(int nSect)
{
    switch (sector[nSect].lotag & 0x7fff)
    {
    case ST_9_SLIDING_ST_DOOR: case ST_20_CEILING_DOOR: case ST_21_FLOOR_DOOR: case ST_22_SPLITTING_DOOR:
    case ST_23_SWINGING_DOOR: case ST_25_SLIDING_DOOR: case ST_26_SPLITTING_ST_DOOR: case ST_29_TEETH_DOOR:
        return true;
    }
    return false;
}

bool IsLiftSector(int nSect)
{
    int const t = sector[nSect].lotag & 0x7fff;
    return t >= ST_15_WARP_ELEVATOR && t <= ST_19_ELEVATOR_UP;
}

void PortalMid(int nWall, int *x, int *y)
{
    auto const &w = wall[nWall], &w2 = wall[w.point2];
    *x = (w.x + w2.x) >> 1;
    *y = (w.y + w2.y) >> 1;
}

// can a player go from sector a into the next sector through this wall?
bool Passable(int nWall)
{
    auto const &w = wall[nWall];
    int const b = w.nextsector;
    if (b < 0 || (w.cstat & 1))
        return false;
    int const a = sectorofwall(nWall);
    int x, y;
    PortalMid(nWall, &x, &y);
    int const fa = getflorzofslope(a, x, y), fb = getflorzofslope(b, x, y), cb = getceilzofslope(b, x, y);
    if ((sector[b].lotag & 0x7fff) == ST_2_UNDERWATER)
        return false;
    if (fa - fb > (40 << 8) && !IsLiftSector(b))
        return false;  // too high to jump up
    if (fb - cb < (24 << 8) && !IsDoorSector(b) && !IsLiftSector(b))
        return false;  // too low to get through (a closed door opens: doors are fine)
    return true;
}

// A* over the sectors, from the centers of the portals' sectors
int16_t sectParent[MAXSECTORS];
int sectCost[MAXSECTORS];
uint32_t sectVisit[MAXSECTORS];
uint32_t nVisitMark;
int16_t openHeap[MAXSECTORS];
int nOpen;

bool FindPath(BOT *pBot, int nFrom, int nTo)
{
    pBot->nPath = pBot->nPathPos = 0;
    if ((unsigned)nFrom >= (unsigned)numsectors || (unsigned)nTo >= (unsigned)numsectors)
        return false;
    if (nFrom == nTo)
    {
        pBot->path[pBot->nPath++] = nTo;
        return true;
    }
    if (++nVisitMark == 0)
    {
        Bmemset(sectVisit, 0, sizeof(sectVisit));
        nVisitMark = 1;
    }
    nOpen = 0;
    sectVisit[nFrom] = nVisitMark;
    sectCost[nFrom] = 0;
    sectParent[nFrom] = -1;
    openHeap[nOpen++] = nFrom;
    bool bFound = false;
    while (nOpen > 0)
    {
        // the open sector closest to the goal by cost + straight distance (small lists: a linear scan is enough)
        int nBest = 0, nBestScore = INT32_MAX;
        for (int i = 0; i < nOpen; i++)
        {
            int const s = openHeap[i];
            int const score = sectCost[s] + Dist2D(sectCenterX[s] - sectCenterX[nTo], sectCenterY[s] - sectCenterY[nTo]);
            if (score < nBestScore)
                nBestScore = score, nBest = i;
        }
        int const s = openHeap[nBest];
        openHeap[nBest] = openHeap[--nOpen];
        if (s == nTo)
        {
            bFound = true;
            break;
        }
        for (int w = sector[s].wallptr, e = w + sector[s].wallnum; w < e; w++)
        {
            int const n = wall[w].nextsector;
            if (n < 0 || !Passable(w))
                continue;
            int const cost = sectCost[s] + Dist2D(sectCenterX[s] - sectCenterX[n], sectCenterY[s] - sectCenterY[n]) + 64;
            if (sectVisit[n] == nVisitMark && cost >= sectCost[n])
                continue;
            bool const bNew = sectVisit[n] != nVisitMark;
            sectVisit[n] = nVisitMark;
            sectCost[n] = cost;
            sectParent[n] = s;
            if (bNew && nOpen < MAXSECTORS)
                openHeap[nOpen++] = n;
        }
    }
    if (!bFound)
        return false;
    int16_t rev[kMaxPath];
    int n = 0;
    for (int s = nTo; s >= 0 && n < kMaxPath; s = sectParent[s])
        rev[n++] = s;
    for (int i = 0; i < n; i++)
        pBot->path[i] = rev[n - 1 - i];
    pBot->nPath = n;
    return true;
}

// the wall between two neighbouring sectors (-1: none)
int PortalWall(int a, int b)
{
    for (int w = sector[a].wallptr, e = w + sector[a].wallnum; w < e; w++)
        if (wall[w].nextsector == b)
            return w;
    return -1;
}

void LearnLevel(void)
{
    for (int s = 0; s < numsectors; s++)
    {
        int64_t sx = 0, sy = 0;
        int const n = max<int>(sector[s].wallnum, 1);
        for (int w = sector[s].wallptr, e = w + sector[s].wallnum; w < e; w++)
            sx += wall[w].x, sy += wall[w].y;
        sectCenterX[s] = (int)(sx / n);
        sectCenterY[s] = (int)(sy / n);
    }
}

// how many sectors can be walked to from this one
int AreaSize(int nFrom)
{
    static int16_t queue[MAXSECTORS];
    static uint8_t seen[MAXSECTORS];
    if ((unsigned)nFrom >= (unsigned)numsectors)
        return 0;
    Bmemset(seen, 0, numsectors);
    int nHead = 0, nTail = 0;
    queue[nTail++] = nFrom;
    seen[nFrom] = 1;
    while (nHead < nTail)
    {
        int const s = queue[nHead++];
        for (int w = sector[s].wallptr, e = w + sector[s].wallnum; w < e; w++)
        {
            int const n = wall[w].nextsector;
            if (n >= 0 && !seen[n] && Passable(w))
                seen[n] = 1, queue[nTail++] = n;
        }
    }
    return nTail;
}

// shut in (a roof, a room behind a vent): something to shoot that may blow a way out - barrels,
// charges, breakable vents (they have health and stand or hang in the level)
bool ItemIgnored(BOT *pBot, int nSprite);

int FindBreakable(BOT *pBot, DukePlayer_t *ps)
{
    int nBest = -1, nBestDist = INT32_MAX;
    for (int nStat : { STAT_STANDABLE, STAT_FALLER })
        for (int i = headspritestat[nStat]; i >= 0; i = nextspritestat[i])
        {
            auto const &s = sprite[i];
            if (s.extra <= 0 || (s.cstat & 32768) || s.xrepeat == 0 || ItemIgnored(pBot, i))
                continue;
            int const d = Dist2D(s.x - ps->pos.x, s.y - ps->pos.y);
            if (d >= nBestDist || d > 8000 || d < 64)
                continue;
            // a shot has to get there (a fence in between: cansee looks through it, a bullet doesn't)
            int const ang = getangle(s.x - ps->pos.x, s.y - ps->pos.y);
            int const zvel = (int)(((int64_t)((s.z - (8 << 8)) - ps->pos.z) << 14) / d);
            hitdata_t hit;
            vec3_t const from = ps->pos;
            hitscan(&from, ps->cursectnum, sintable[(ang + 512) & 2047], sintable[ang & 2047], zvel, &hit, CLIPMASK1);
            if (hit.sprite == i)
                nBestDist = d, nBest = i;
        }
    return nBest;
}

// ---- items --------------------------------------------------------------------------------------

// how much a bot wants this item now (0: not at all)
bool ModeActive(int nWhich);
bool IsEnemy(int nMe, int nOther);

int ItemValue(DukePlayer_t *ps, int nSprite)
{
    auto const &s = sprite[nSprite];
    if (ModeActive(kBotModeGunGame))
    {
        switch (tileGetMapping(s.picnum))
        {
        case COLA__: case SIXPAK__: case FIRSTAID__: case ATOMICHEALTH__: case SHIELD__: break;
        default: return 0;  // weapons and ammo come with the kills
        }
    }
    if (s.statnum == MAXSTATUS || (s.cstat & 32768) || s.xrepeat == 0)
        return 0;
    int const nHealth = sprite[ps->i].extra;
    auto weaponValue = [&](int w, int v) { return (ps->gotweapon & (1 << w)) ? (ps->ammo_amount[w] < ps->max_ammo_amount[w] / 3 ? v / 3 : 0) : v; };
    auto ammoValue = [&](int w, int v) { return (ps->gotweapon & (1 << w)) && ps->ammo_amount[w] < ps->max_ammo_amount[w] / 2 ? v : 0; };
    switch (tileGetMapping(s.picnum))
    {
    case FIRSTGUNSPRITE__: return weaponValue(PISTOL_WEAPON, 30);
    case SHOTGUNSPRITE__: return weaponValue(SHOTGUN_WEAPON, 90);
    case CHAINGUNSPRITE__: return weaponValue(CHAINGUN_WEAPON, 100);
    case RPGSPRITE__: return weaponValue(RPG_WEAPON, 110);
    case DEVISTATORSPRITE__: return weaponValue(DEVISTATOR_WEAPON, 115);
    case FREEZESPRITE__: return weaponValue(FREEZE_WEAPON, 80);
    case SHRINKERSPRITE__: return weaponValue(SHRINKER_WEAPON, 50);
    case AMMO__: return ammoValue(PISTOL_WEAPON, 15);
    case SHOTGUNAMMO__: return ammoValue(SHOTGUN_WEAPON, 40);
    case BATTERYAMMO__: return ammoValue(CHAINGUN_WEAPON, 45);
    case RPGAMMO__: return ammoValue(RPG_WEAPON, 50);
    case DEVISTATORAMMO__: return ammoValue(DEVISTATOR_WEAPON, 50);
    case FREEZEAMMO__: return ammoValue(FREEZE_WEAPON, 35);
    case CRYSTALAMMO__: return ammoValue(SHRINKER_WEAPON, 20);
    case AMMOLOTS__: return ammoValue(PISTOL_WEAPON, 25);
    case COLA__: return nHealth < 100 ? 15 : 0;
    case SIXPAK__: return nHealth < 100 ? 35 : 0;
    case FIRSTAID__: return nHealth < 70 ? 60 : 0;
    case ATOMICHEALTH__: return 70;
    case SHIELD__: return ps->inv_amount[GET_SHIELD] < 50 ? 45 : 0;
    }
    return 0;
}

int itemList[2048];
int nItems, nItemsScanned = -1000;

void ScanItems(void)
{
    nItems = 0;
    for (int i = 0; i < MAXSPRITES && nItems < (int)ARRAY_SIZE(itemList); i++)
    {
        auto const &s = sprite[i];
        if (s.statnum == MAXSTATUS || (unsigned)s.sectnum >= (unsigned)numsectors)
            continue;
        switch (tileGetMapping(s.picnum))
        {
        case FIRSTGUNSPRITE__: case SHOTGUNSPRITE__: case CHAINGUNSPRITE__: case RPGSPRITE__: case DEVISTATORSPRITE__:
        case FREEZESPRITE__: case SHRINKERSPRITE__: case AMMO__: case SHOTGUNAMMO__: case BATTERYAMMO__: case RPGAMMO__:
        case DEVISTATORAMMO__: case FREEZEAMMO__: case CRYSTALAMMO__: case AMMOLOTS__: case COLA__: case SIXPAK__:
        case FIRSTAID__: case ATOMICHEALTH__: case SHIELD__:
            itemList[nItems++] = i;
            break;
        }
    }
    nItemsScanned = nTic;
}

bool ItemIgnored(BOT *pBot, int nSprite)
{
    for (int i = 0; i < 8; i++)
        if (pBot->nIgnoreItem[i] == nSprite && nTic < pBot->nIgnoreTime[i])
            return true;
    return false;
}

void IgnoreItem(BOT *pBot, int nSprite, int nTics)
{
    int nSlot = 0;
    for (int i = 1; i < 8; i++)
        if (pBot->nIgnoreTime[i] < pBot->nIgnoreTime[nSlot])
            nSlot = i;
    pBot->nIgnoreItem[nSlot] = nSprite;
    pBot->nIgnoreTime[nSlot] = nTic + nTics;
}

// ---- weapons ------------------------------------------------------------------------------------

bool HasWeapon(DukePlayer_t *ps, int w)
{
    return (ps->gotweapon & (1 << w)) && (w == KNEE_WEAPON || ps->ammo_amount[w] > 0);
}

int WeaponScore(DukePlayer_t *ps, int w, int nDist)
{
    if (!HasWeapon(ps, w))
        return -1;
    switch (w)
    {
    case KNEE_WEAPON: return nDist < 700 ? 35 : 1;
    case PISTOL_WEAPON: return 40;
    case SHOTGUN_WEAPON: return nDist < 2500 ? 95 : 45;
    case CHAINGUN_WEAPON: return 85;
    case RPG_WEAPON: return nDist > 1400 ? 100 : 5;          // not in your own face
    case DEVISTATOR_WEAPON: return nDist > 1400 ? 105 : 10;
    case FREEZE_WEAPON: return 75;
    case SHRINKER_WEAPON: return nDist > 1200 ? 55 : 5;
    }
    return -1;  // pipe bombs, trip bombs: not by bots
}

int BestWeapon(DukePlayer_t *ps, int nDist)
{
    int nBest = KNEE_WEAPON, nBestScore = 0;
    for (int w = 0; w <= FREEZE_WEAPON; w++)
    {
        int const score = WeaponScore(ps, w, nDist);
        if (score > nBestScore)
            nBestScore = score, nBest = w;
    }
    return nBest;
}

int PreferredDistance(int w)
{
    switch (w)
    {
    case KNEE_WEAPON: return 0;
    case SHOTGUN_WEAPON: return 1400;
    case RPG_WEAPON: case DEVISTATOR_WEAPON: return 4000;
    case SHRINKER_WEAPON: return 3000;
    default: return 2600;
    }
}

// ---- seeing -------------------------------------------------------------------------------------

int TargetZ(int nSprite)
{
    auto const &s = sprite[nSprite];
    if (s.picnum == APLAYER && (unsigned)s.yvel < MAXPLAYERS && g_player[s.yvel].ps)
        return g_player[s.yvel].ps->pos.z + (8 << 8);
    return s.z - ((tilesiz[s.picnum].y * s.yrepeat) << 1);
}

bool CanSee(DukePlayer_t *ps, int nSprite)
{
    auto const &s = sprite[nSprite];
    return cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum, s.x, s.y, TargetZ(nSprite), s.sectnum);
}

// the enemy this bot should fight now (-1: none in sight)
int FindTarget(int nBot, BOT *pBot)
{
    auto *ps = PlayerOf(nBot);
    int const nMyAng = fix16_to_int(ps->q16ang) & 2047;
    int nBest = -1, nBestScore = INT32_MAX;
    auto consider = [&](int nSprite)
    {
        auto const &s = sprite[nSprite];
        int const dx = s.x - ps->pos.x, dy = s.y - ps->pos.y;
        int const nDist = Dist2D(dx, dy);
        if (nDist > 24000)
            return;
        int const nAngOff = klabs(AngDiff(getangle(dx, dy), nMyAng));
        // what is in front, or close enough to be heard; the current target is kept while seen
        if (nAngOff > 400 && nDist > 1200 && nSprite != pBot->nTarget)
            return;
        if (!CanSee(ps, nSprite))
            return;
        int const score = nDist + nAngOff * 4 - (nSprite == pBot->nTarget ? 2000 : 0);
        if (score < nBestScore)
            nBestScore = score, nBest = nSprite;
    };
    // monsters: the enemies in co-op; in DukeMatch the ones close enough to be a danger
    if (!ud.monsters_off)
    {
        int const nPenalty = CoopGame() ? 0 : 3000;
        for (int i = headspritestat[STAT_ACTOR]; i >= 0; i = nextspritestat[i])
            if (A_CheckEnemySprite(&sprite[i]) && sprite[i].extra > 0 && !(sprite[i].cstat & 32768))
            {
                if (!CoopGame() && Dist2D(sprite[i].x - ps->pos.x, sprite[i].y - ps->pos.y) > 5000)
                    continue;
                int const nBefore = nBest;
                int const nBeforeScore = nBestScore;
                consider(i);
                if (nBest == i && nBest != nBefore)
                {
                    nBestScore += nPenalty;  // players come first
                    if (nBestScore > nBeforeScore && nBefore >= 0)
                        nBest = nBefore, nBestScore = nBeforeScore;
                }
            }
    }
    if (!CoopGame())
    {
        for (int TRAVERSE_CONNECT(p))
        {
            if (p == nBot || !PlayerAlive(p) || !IsEnemy(nBot, p))
                continue;
            consider(PlayerOf(p)->i);
        }
    }
    return nBest;
}

// ---- match types --------------------------------------------------------------------------------
// The machine that runs the match (a bot match, or the host of a network game) decides everything:
// scores, gun levels, lives, who the juggernaut is, the flags, who hides, the waves. It sends that
// to the other machines (Net_SendModeState), and every machine applies the rules to the players it
// plays itself (ModeLocal: the weapons of the gun game, no shooting for hiders...).

const char *const kModeNames[kBotModeMax] = { "DukeMatch", "Gun Game", "King of the Hill", "Last Man Standing", "Juggernaut",
    "Capture the Flag", "Hide and Seek", "Survival" };
int const kGunGameOrder[] = { PISTOL_WEAPON, SHOTGUN_WEAPON, CHAINGUN_WEAPON, FREEZE_WEAPON, RPG_WEAPON, DEVISTATOR_WEAPON, SHRINKER_WEAPON, KNEE_WEAPON };
int const kGunGameSteps = ARRAY_SIZE(kGunGameOrder);
int const kLives = 3;
int const kHillWin = 60;
int const kJuggernautWin = 10;
int const kJuggernautHealth = 300;
int const kFlagWin = 3;
int const kFlagReturnTics = 30 * 30;
int const kHideFreezeTics = 30 * 15;
int const kHideRoundTics = 30 * 150;
int const kSurvivalLives = 3;

int nMode;                       // the match type of the running match
bool bFromHost;                  // a network game's client: the host's match (Net_ReceiveModeState)
int gunLevel[MAXPLAYERS];
int lives[MAXPLAYERS];
int hillPoints[MAXPLAYERS];      // the score of the modes that count points (the hill, the juggernaut, hide and seek)
int nHillSect = -1;
uint8_t inHill[MAXSECTORS];
int nHillTic;
int nJuggernaut = -1;
// capture the flag: per team (0 blue, 1 red)
struct FLAG { int nSprite, nHomeX, nHomeY, nHomeZ, nHomeSect, nCarrier, nDropTic; bool bHome; };
FLAG flags[2];
int teamScore[2];
// hide and seek
uint16_t nSeekers;               // a bit per player
int nHideTic;                    // tics into the round
uint16_t nDecorPic[MAXPLAYERS];  // what each hider looks like
// survival
int nWave, nWaveTic, nWaveMonsters;
bool bWaveRunning;

bool ModeActive(int nWhich)
{
    if (nMode != nWhich)
        return false;
    return nWhich == kBotModeSurvival ? CoopGame() : !CoopGame();
}

// this machine runs the match (not a network game's client)
bool ModeAuthority(void)
{
    return !g_netClient;
}

void Announce(const char *pzText)
{
    G_AddUserQuote(pzText);
    Net_BroadcastQuote(pzText);  // a network game: everybody sees it
    LOG_F(INFO, "Bot match: %s", pzText);
}

bool IsSeeker(int p) { return (nSeekers >> p) & 1; }
bool IsHider(int p) { return ModeActive(kBotModeHideSeek) && !IsSeeker(p); }

bool PlayerOut(int nPlayer)
{
    return (ModeActive(kBotModeLastMan) || ModeActive(kBotModeSurvival)) && lives[nPlayer] <= 0;
}

int FlagOfTeam(int t) { return clamp(t, 0, 1); }

// who a player fights (bots)
bool IsEnemy(int nMe, int nOther)
{
    if (nMe == nOther)
        return false;
    if (ModeActive(kBotModeHideSeek))
        return IsSeeker(nMe) && !IsSeeker(nOther) && nHideTic >= kHideFreezeTics;
    if (TeamGame() || ModeActive(kBotModeCaptureFlag))
        return PlayerTeam(nMe) != PlayerTeam(nOther);
    return true;
}

// the main part of the level: what most spawn points can walk to (keys and closed roofs make smaller ones).
// A sector is in it when as much can be reached from there (two thirds of it).
int BigArea(void)
{
    static int nFor = -1, nSize = 1;
    int const nId = numsectors * 7919 + numwalls;
    if (nFor != nId)
    {
        nFor = nId;
        int nBest = 1;
        for (int i = 0; i < g_playerSpawnCnt; i++)
            if ((unsigned)g_playerSpawnPoints[i].sect < (unsigned)numsectors)
                nBest = max(nBest, AreaSize(g_playerSpawnPoints[i].sect));
        nSize = max(1, nBest * 2 / 3);
        LOG_F(INFO, "Bot match: the main part of the level is %d of %d sectors", nBest, numsectors);
    }
    return nSize;
}

// a hill: a big sector most of the level can walk to, and the sectors around it
void MarkHill(int nSect)
{
    Bmemset(inHill, 0, sizeof(inHill));
    nHillSect = nSect;
    if ((unsigned)nSect >= (unsigned)numsectors)
        return;
    inHill[nSect] = 1;
    for (int w = sector[nSect].wallptr, e = w + sector[nSect].wallnum; w < e; w++)
        if (wall[w].nextsector >= 0 && Passable(w))
            inHill[wall[w].nextsector] = 1;
}

void PickHill(void)
{
    int nBest = -1;
    int64_t nBestScore = -1;
    for (int nTry = 0; nTry < 60; nTry++)
    {
        int const s = (int)((nTic * 7919u + nTry * 104729u) % (unsigned)max<int>(numsectors, 1));
        if ((sector[s].lotag & 0x7fff) == ST_2_UNDERWATER || sector[s].floorz - sector[s].ceilingz < (40 << 8))
            continue;
        if (AreaSize(s) < BigArea())
            continue;  // in the big connected part of the level (not a closed roof)
        int minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
        for (int w = sector[s].wallptr, e = w + sector[s].wallnum; w < e; w++)
            minx = min<int>(minx, wall[w].x), maxx = max<int>(maxx, wall[w].x), miny = min<int>(miny, wall[w].y), maxy = max<int>(maxy, wall[w].y);
        int64_t const area = (int64_t)(maxx - minx) * (maxy - miny);
        if (area > nBestScore && area < (int64_t)8192 * 8192)
            nBestScore = area, nBest = s;
    }
    if (nBest < 0)
        nBest = PlayerOf(0)->cursectnum;
    MarkHill(nBest);
    LOG_F(INFO, "Bot match: the hill is sector %d", nBest);
}

// ---- capture the flag

void PlaceFlagSprite(FLAG &f, int x, int y, int z, int nSect)
{
    if (f.nSprite < 0 || (unsigned)nSect >= (unsigned)numsectors)
        return;
    vec3_t const v = { x, y, z };
    setsprite(f.nSprite, &v);
}

void FlagHome(int t)
{
    FLAG &f = flags[t];
    f.bHome = true;
    f.nCarrier = -1;
    PlaceFlagSprite(f, f.nHomeX, f.nHomeY, f.nHomeZ, f.nHomeSect);
}

// the flags: at the two spawn points furthest apart (both in the big part of the level)
void SetupFlags(void)
{
    for (int t = 0; t < 2; t++)
    {
        if (flags[t].nSprite >= 0 && flags[t].nSprite < MAXSPRITES && sprite[flags[t].nSprite].statnum != MAXSTATUS && sprite[flags[t].nSprite].picnum == ACCESSCARD)
            deletesprite(flags[t].nSprite);
        flags[t].nSprite = -1;
    }
    // homes: open places in the big part of the level, away from where players spawn, as far apart as can be
    int cand[64], nCand = 0;
    for (int k = 0; k < numsectors && nCand < 64; k++)
    {
        int const sct = (int)((k * 2654435761u) % (unsigned)numsectors);
        if ((sector[sct].lotag & 0x7fff) == ST_2_UNDERWATER || sector[sct].floorz - sector[sct].ceilingz < (48 << 8) ||
            inside(sectCenterX[sct], sectCenterY[sct], sct) != 1 || AreaSize(sct) < BigArea())
            continue;
        // an ordinary room (a huge sector is a backdrop or an outside area you can't really get into)
        int minx = INT32_MAX, maxx = INT32_MIN, miny = INT32_MAX, maxy = INT32_MIN;
        for (int w = sector[sct].wallptr, e = w + sector[sct].wallnum; w < e; w++)
            minx = min<int>(minx, wall[w].x), maxx = max<int>(maxx, wall[w].x), miny = min<int>(miny, wall[w].y), maxy = max<int>(maxy, wall[w].y);
        if (maxx - minx > 4096 || maxy - miny > 4096 || maxx - minx < 512 || maxy - miny < 512)
            continue;
        // not on a spawn point, but near one: the parts of the level players get to (not behind a key door)
        int nNearest = INT32_MAX;
        for (int i = 0; i < g_playerSpawnCnt; i++)
            nNearest = min(nNearest, Dist2D(g_playerSpawnPoints[i].x - sectCenterX[sct], g_playerSpawnPoints[i].y - sectCenterY[sct]));
        if (nNearest >= 1200 && nNearest <= 4000)
            cand[nCand++] = sct;
    }
    int nHomeA = -1, nHomeB = -1;
    int64_t nBestHome = -1;
    for (int i = 0; i < nCand; i++)
        for (int j = i + 1; j < nCand; j++)
        {
            int64_t const dx = sectCenterX[cand[i]] - sectCenterX[cand[j]], dy = sectCenterY[cand[i]] - sectCenterY[cand[j]];
            if (dx * dx + dy * dy > nBestHome && FindPath(&bots[MAXPLAYERS - 1], cand[i], cand[j]))
                nBestHome = dx * dx + dy * dy, nHomeA = cand[i], nHomeB = cand[j];
        }
    int nA = -1, nB = -1;
    int64_t nBest = -1;
    for (int i = 0; nHomeA < 0 && i < g_playerSpawnCnt; i++)
        for (int j = i + 1; j < g_playerSpawnCnt; j++)
        {
            auto const &a = g_playerSpawnPoints[i], &b = g_playerSpawnPoints[j];
            if ((unsigned)a.sect >= (unsigned)numsectors || (unsigned)b.sect >= (unsigned)numsectors)
                continue;
            int64_t const d = (int64_t)(a.x - b.x) * (a.x - b.x) + (int64_t)(a.y - b.y) * (a.y - b.y);
            if (d > nBest && AreaSize(a.sect) >= BigArea() && FindPath(&bots[MAXPLAYERS - 1], a.sect, b.sect))
                nBest = d, nA = i, nB = j;
        }
    if (nHomeA < 0 && nA < 0)
    {
        nA = 0, nB = g_playerSpawnCnt > 1 ? 1 : 0;
        LOG_F(WARNING, "Capture the Flag: no two places far apart, using spawn points %d and %d", nA, nB);
    }
    int const pick[2] = { nA, nB };
    int const homes[2] = { nHomeA, nHomeB };
    for (int t = 0; t < 2; t++)
    {
        playerspawn_t sp;
        if (nHomeA >= 0)
            sp.x = sectCenterX[homes[t]], sp.y = sectCenterY[homes[t]], sp.sect = homes[t];
        else
            sp = g_playerSpawnPoints[pick[t]];
        FLAG &f = flags[t];
        f.nHomeX = sp.x, f.nHomeY = sp.y, f.nHomeSect = sp.sect;
        f.nHomeZ = sector[sp.sect].floorz;
        int const s = insertsprite(sp.sect, 0);
        if (s >= 0)
        {
            spritetype &spr = sprite[s];
            spr.x = sp.x, spr.y = sp.y, spr.z = f.nHomeZ;
            spr.picnum = ACCESSCARD;
            spr.pal = t == 0 ? 0 : 21;  // a blue and a red key card
            spr.xrepeat = spr.yrepeat = 48;
            spr.cstat = 0;
            spr.shade = -16;
            spr.owner = -1;
            spr.extra = 0;
            spr.ang = 0;
            spr.xvel = spr.yvel = spr.zvel = 0;
            spr.lotag = spr.hitag = 0;
        }
        f.nSprite = s;
        FlagHome(t);
    }
    teamScore[0] = teamScore[1] = 0;
    LOG_F(INFO, "Capture the Flag: blue flag in sector %d, red flag in sector %d (%d places looked at)", flags[0].nHomeSect, flags[1].nHomeSect, nCand);
}

int Near(DukePlayer_t *ps, int x, int y, int z, int nDist)
{
    return Dist2D(ps->pos.x - x, ps->pos.y - y) < nDist && klabs(ps->pos.z - z) < (64 << 8);
}

void FlagTick(void)
{
    for (int t = 0; t < 2; t++)
    {
        FLAG &f = flags[t];
        if (f.nSprite < 0)
            continue;
        if (f.nCarrier >= 0)
        {
            auto *pc = PlayerOf(f.nCarrier);
            if (!g_player[f.nCarrier].playerquitflag || !PlayerAlive(f.nCarrier))
            {
                // dropped where the carrier died
                f.nCarrier = -1;
                f.nDropTic = nTic;
                if (pc && (unsigned)pc->cursectnum < (unsigned)numsectors)
                    PlaceFlagSprite(f, pc->pos.x, pc->pos.y, sector[pc->cursectnum].floorz, pc->cursectnum);
                char buffer[64];
                Bsnprintf(buffer, sizeof(buffer), "The %s flag was dropped", t ? "red" : "blue");
                Announce(buffer);
                continue;
            }
            PlaceFlagSprite(f, pc->pos.x, pc->pos.y, pc->pos.z + (20 << 8), pc->cursectnum);
            // home with it while their own flag is home: a capture
            int const nOwn = FlagOfTeam(PlayerTeam(f.nCarrier));
            if (flags[nOwn].bHome && Near(pc, flags[nOwn].nHomeX, flags[nOwn].nHomeY, flags[nOwn].nHomeZ - (20 << 8), 700))
            {
                int const nScore = ++teamScore[nOwn];
                int const nBy = f.nCarrier;
                hillPoints[nBy]++;
                FlagHome(t);  // (no carrier from here on)
                char buffer[96];
                if (nScore >= kFlagWin)
                {
                    Bsnprintf(buffer, sizeof(buffer), "%s captures! The %s team wins %d-%d", g_player[nBy].user_name, nOwn ? "red" : "blue",
                              teamScore[nOwn], teamScore[nOwn ^ 1]);
                    Announce(buffer);
                    teamScore[0] = teamScore[1] = 0;
                    FlagHome(0), FlagHome(1);
                }
                else
                {
                    Bsnprintf(buffer, sizeof(buffer), "%s captures the %s flag! Blue %d - Red %d", g_player[nBy].user_name, t ? "red" : "blue",
                              teamScore[0], teamScore[1]);
                    Announce(buffer);
                }
            }
            continue;
        }
        spritetype const &spr = sprite[f.nSprite];
        if (!f.bHome && nTic - f.nDropTic > kFlagReturnTics)
        {
            FlagHome(t);
            char buffer[64];
            Bsnprintf(buffer, sizeof(buffer), "The %s flag went home", t ? "red" : "blue");
            Announce(buffer);
            continue;
        }
        for (int TRAVERSE_CONNECT(p))
        {
            if (!PlayerAlive(p) || !Near(PlayerOf(p), spr.x, spr.y, spr.z - (20 << 8), 600))
                continue;
            char buffer[96];
            if (FlagOfTeam(PlayerTeam(p)) == t)
            {
                if (!f.bHome)
                {
                    FlagHome(t);
                    Bsnprintf(buffer, sizeof(buffer), "%s brought the %s flag home", g_player[p].user_name, t ? "red" : "blue");
                    Announce(buffer);
                }
                continue;
            }
            f.nCarrier = p;
            f.bHome = false;
            Bsnprintf(buffer, sizeof(buffer), "%s has the %s flag!", g_player[p].user_name, t ? "red" : "blue");
            Announce(buffer);
            break;
        }
    }
}

// ---- hide and seek

int PickDecorPic(int nPlayer)
{
    // a decoration of the level (something with no game code: plants, chairs, lamps, cans...)
    int nFound = 0, nPic = 0;
    for (int i = headspritestat[STAT_DEFAULT]; i >= 0; i = nextspritestat[i])
    {
        auto const &s = sprite[i];
        if (s.picnum < 10 || (s.cstat & (16 | 32 | 32768)) || s.xrepeat < 16 || s.yrepeat < 16 || tilesiz[s.picnum].y < 16 || tilesiz[s.picnum].y > 128)
            continue;
        if (A_CheckEnemySprite(&s) || s.picnum == ACCESSCARD)
            continue;
        if ((int)((nTic * 2654435761u + i * 40503u + nPlayer * 7919u) >> 7) % ++nFound == 0)
            nPic = s.picnum;
    }
    return nPic ? nPic : FIRE;  // nothing found: a fire
}

void StartHideRound(void)
{
    nHideTic = 0;
    nSeekers = 0;
    int nPlayers = 0, list[MAXPLAYERS];
    for (int TRAVERSE_CONNECT(p))
        list[nPlayers++] = p;
    // a seeker for every 4 players, at least one
    int const nWant = max(1, nPlayers / 4);
    for (int k = 0; k < nWant && nPlayers > 0; k++)
    {
        int const n = (int)((nTic * 2654435761u + k * 97u) >> 8) % nPlayers;
        nSeekers |= 1 << list[n];
        list[n] = list[--nPlayers];
    }
    for (int p = 0; p < MAXPLAYERS; p++)
        nDecorPic[p] = PickDecorPic(p);
    Announce("Hide and Seek: hide! The seekers come in 15 seconds");
}

void HideTick(void)
{
    nHideTic++;
    if (nHideTic == kHideFreezeTics)
        Announce("Ready or not, here they come!");
    int nHiders = 0, nLast = -1;
    for (int TRAVERSE_CONNECT(p))
        if (!IsSeeker(p))
            nHiders++, nLast = p;
    char buffer[96];
    if (nHiders == 0)
    {
        Announce("The seekers found everybody!");
        StartHideRound();
    }
    else if (nHideTic >= kHideRoundTics)
    {
        for (int TRAVERSE_CONNECT(p))
            if (!IsSeeker(p))
                hillPoints[p] += 3;
        if (nHiders == 1)
            Bsnprintf(buffer, sizeof(buffer), "%s stayed hidden and wins the round!", g_player[nLast].user_name);
        else
            Bsnprintf(buffer, sizeof(buffer), "%d hiders stayed hidden and win the round!", nHiders);
        Announce(buffer);
        StartHideRound();
    }
}

// ---- survival: waves of monsters in co-op

int const kWaveMonsters[] = { LIZTROOP, PIGCOP, OCTABRAIN, LIZMAN, DRONE, COMMANDER };

int CountMonsters(void)
{
    int n = 0;
    for (int nStat : { STAT_ACTOR, STAT_ZOMBIEACTOR })  // awake, and asleep (not seen by anybody yet)
        for (int i = headspritestat[nStat]; i >= 0; i = nextspritestat[i])
            if (A_CheckEnemySprite(&sprite[i]) && sprite[i].extra > 0)
                n++;
    return n;
}

void ClearMonsters(void)
{
    for (int nStat : { STAT_ACTOR, STAT_ZOMBIEACTOR })
        for (int i = headspritestat[nStat], next; i >= 0; i = next)
        {
            next = nextspritestat[i];
            if (A_CheckEnemySprite(&sprite[i]))
                A_DeleteSprite(i);
        }
}

void SpawnWave(void)
{
    nWave++;
    int const nCount = 4 + nWave * 3;
    int const nTypes = min<int>(ARRAY_SIZE(kWaveMonsters), 2 + nWave / 2);
    int nSpawned = 0;
    for (int nTry = 0; nTry < nCount * 40 && nSpawned < nCount; nTry++)
    {
        int const s = (int)((nTic * 7919u + nTry * 104729u + nWave * 31u) % (unsigned)max<int>(numsectors, 1));
        if ((sector[s].lotag & 0x7fff) == ST_2_UNDERWATER || sector[s].floorz - sector[s].ceilingz < (48 << 8) || AreaSize(s) < BigArea())
            continue;
        // not right next to a player, and not in sight
        bool bOk = true;
        for (int TRAVERSE_CONNECT(p))
        {
            auto *ps = PlayerOf(p);
            int const d = Dist2D(ps->pos.x - sectCenterX[s], ps->pos.y - sectCenterY[s]);
            if (d < 2500 || (d < 8000 && cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum, sectCenterX[s], sectCenterY[s], sector[s].floorz - (32 << 8), s)))
                bOk = false;
        }
        if (!bOk || inside(sectCenterX[s], sectCenterY[s], s) != 1)
            continue;
        int const i = insertsprite(s, 0);
        if (i < 0)
            break;
        spritetype &spr = sprite[i];
        spr.x = sectCenterX[s], spr.y = sectCenterY[s], spr.z = sector[s].floorz;
        spr.picnum = kWaveMonsters[(nTry + nSpawned) % nTypes];
        spr.xrepeat = spr.yrepeat = 40;
        spr.cstat = 257;
        spr.pal = 0;
        spr.ang = (nTry * 337) & 2047;
        spr.owner = i;
        spr.lotag = spr.hitag = 0;
        spr.extra = -1;
        A_Spawn(-1, i);
        if (sprite[i].statnum == STAT_ZOMBIEACTOR)
            changespritestat(i, STAT_ACTOR);  // awake: they come for you
        if (getenv("DUKE3D_BOT_DEBUG"))
            LOG_F(INFO, "survival spawn %d: pic %d stat %d extra %d rep %d enemy %d", i, sprite[i].picnum, sprite[i].statnum, sprite[i].extra,
                  sprite[i].xrepeat, A_CheckEnemySprite(&sprite[i]));
        nSpawned++;
    }
    nWaveMonsters = nSpawned;
    bWaveRunning = true;
    // everybody is back, with their lives
    for (int TRAVERSE_CONNECT(p))
        if (lives[p] <= 0)
            lives[p] = 1;
    char buffer[64];
    Bsnprintf(buffer, sizeof(buffer), "Wave %d: %d monsters", nWave, nSpawned);
    Announce(buffer);
}

void SurvivalTick(void)
{
    if (!bWaveRunning)
    {
        if (++nWaveTic >= 30 * 6)
            SpawnWave();
        return;
    }
    if (nTic % 15)
        return;
    nWaveMonsters = CountMonsters();
    if (nWaveMonsters == 0)
    {
        bWaveRunning = false;
        nWaveTic = 0;
        char buffer[64];
        Bsnprintf(buffer, sizeof(buffer), "Wave %d cleared! The next one in 6 seconds", nWave);
        Announce(buffer);
        return;
    }
    // everybody out: the monsters win, from the start
    int nIn = 0;
    for (int TRAVERSE_CONNECT(p))
        if (lives[p] > 0 || PlayerAlive(p))
            nIn++;
    if (nIn == 0)
    {
        char buffer[64];
        Bsnprintf(buffer, sizeof(buffer), "Everybody is down! You made it to wave %d", nWave);
        Announce(buffer);
        ClearMonsters();
        nWave = 0;
        nWaveTic = 0;
        bWaveRunning = false;
        for (int p = 0; p < MAXPLAYERS; p++)
            lives[p] = kSurvivalLives;
    }
}

void ResetMode(void)
{
    for (int i = 0; i < MAXPLAYERS; i++)
        gunLevel[i] = 0, lives[i] = nMode == kBotModeSurvival ? kSurvivalLives : kLives, hillPoints[i] = 0;
    nJuggernaut = -1;
    nHillSect = -1;
    nSeekers = 0;
    teamScore[0] = teamScore[1] = 0;
    if (!ModeAuthority())
        return;
    if (ModeActive(kBotModeHill))
        PickHill();
    if (ModeActive(kBotModeCaptureFlag))
        SetupFlags();
    if (ModeActive(kBotModeHideSeek))
        StartHideRound();
    if (ModeActive(kBotModeSurvival))
    {
        ClearMonsters();
        nWave = 0, nWaveTic = 0, bWaveRunning = false;
        Announce("Survival: the first wave comes in 6 seconds");
    }
    if (ModeActive(kBotModeJuggernaut))
        Announce("Juggernaut: kill the juggernaut to become it");
}

void MakeJuggernaut(int p)
{
    nJuggernaut = p;
    if (p < 0)
        return;
    sprite[PlayerOf(p)->i].extra = kJuggernautHealth;
    PlayerOf(p)->inv_amount[GET_SHIELD] = 100;
    char buffer[64];
    Bsnprintf(buffer, sizeof(buffer), "%s is the Juggernaut!", g_player[p].user_name);
    Announce(buffer);
}

// the rules for a player this machine plays (its own player; the host: everybody it simulates)
void ModeLocal(int p)
{
    auto *ps = PlayerOf(p);
    if (!ps || ps->i < 0)
        return;
    input_t &in = inputfifo[(g_netServer && myconnectindex == p)][p];
    if (PlayerOut(p))
        in.bits &= ~(1u << SK_OPEN);  // who is out stays down
    if (!PlayerAlive(p))
        return;
    if (ModeActive(kBotModeGunGame))
    {
        // the weapon of their step (and the boot), nothing else
        int const w = kGunGameOrder[clamp(gunLevel[p], 0, kGunGameSteps - 1)];
        ps->gotweapon &= (1 << KNEE_WEAPON) | (1 << w);
        if (w != KNEE_WEAPON)
        {
            if (!(ps->gotweapon & (1 << w)))
                P_AddWeapon(ps, w, 1);
            ps->ammo_amount[w] = max<int>(ps->ammo_amount[w], ps->max_ammo_amount[w] / 2);
        }
        if (!(ps->gotweapon & (1 << ps->curr_weapon)) && !ps->reloading)
            P_AddWeapon(ps, w, 1);
    }
    if (ModeActive(kBotModeHideSeek))
    {
        if (!IsSeeker(p))
            in.bits &= ~((1u << SK_FIRE) | (15u << SK_WEAPON_BITS));  // hiders don't shoot
        else if (nHideTic < kHideFreezeTics)
        {
            // seekers wait (and see nothing: they look at the floor)
            in.fvel = in.svel = 0;
            in.q16avel = 0;
            in.bits &= ~((1u << SK_FIRE) | (1u << SK_JUMP));
            ps->q16horiz = F16(199);
        }
    }
}

void SendState(void);

void ModeTick(void)
{
    // every machine: the rules for its own players
    for (int TRAVERSE_CONNECT(p))
        if (p == myconnectindex || (ModeAuthority() && (isBot[p] || g_netServer)))
            ModeLocal(p);
    if (!ModeAuthority())
        return;

    if (ModeActive(kBotModeHill) && nHillSect >= 0 && ++nHillTic >= 30)
    {
        nHillTic = 0;
        int nHolder = -1, nCount = 0;
        for (int TRAVERSE_CONNECT(p))
            if (PlayerAlive(p) && (unsigned)PlayerOf(p)->cursectnum < (unsigned)numsectors && inHill[PlayerOf(p)->cursectnum])
            {
                if (nCount == 0 || !(TeamGame() && PlayerTeam(p) == PlayerTeam(nHolder)))
                    nCount++;
                nHolder = p;
            }
        if (nCount == 1)
        {
            int const nPoints = ++hillPoints[nHolder];
            if (nPoints >= kHillWin)
            {
                char buffer[96];
                Bsnprintf(buffer, sizeof(buffer), "%s is the King of the Hill!", g_player[nHolder].user_name);
                Announce(buffer);
                ResetMode();  // a new hill, everybody from zero
            }
        }
    }
    else if (ModeActive(kBotModeLastMan))
    {
        int nIn = 0, nLast = -1, nTeamsIn = 0;
        bool teamIn[2] = {};
        for (int TRAVERSE_CONNECT(p))
            if (lives[p] > 0)
            {
                nIn++, nLast = p;
                if (!teamIn[PlayerTeam(p) & 1])
                    teamIn[PlayerTeam(p) & 1] = true, nTeamsIn++;
            }
        bool const bTeams = TeamGame();
        if (nIn <= 1 || (bTeams && nTeamsIn <= 1))
        {
            char buffer[96];
            if (bTeams)
                Bsnprintf(buffer, sizeof(buffer), "The %s team is the last team standing!", nLast >= 0 && (PlayerTeam(nLast) & 1) ? "red" : "blue");
            else
                Bsnprintf(buffer, sizeof(buffer), "%s is the last man standing!", nLast >= 0 ? g_player[nLast].user_name : "Nobody");
            Announce(buffer);
            ResetMode();  // everybody gets their lives back and comes back
        }
    }
    else if (ModeActive(kBotModeJuggernaut))
    {
        if (nJuggernaut < 0 || !g_player[nJuggernaut].playerquitflag)
        {
            // somebody alive becomes it (by chance)
            for (int k = 0, p = (int)((nTic * 2654435761u) >> 12) % MAXPLAYERS; k < MAXPLAYERS; k++, p = (p + 1) % MAXPLAYERS)
                if (g_player[p].playerquitflag && g_player[p].ps && PlayerAlive(p))
                {
                    MakeJuggernaut(p);
                    break;
                }
        }
    }
    else if (ModeActive(kBotModeCaptureFlag))
        FlagTick();
    else if (ModeActive(kBotModeHideSeek))
        HideTick();
    else if (ModeActive(kBotModeSurvival))
        SurvivalTick();

    if (g_netServer && nTic % 15 == 0)
        SendState();
}

// ---- goals ----------------------------------------------------------------------------------------

void SetGoal(BOT *pBot, int x, int y, int nSect, int nKind, int nItem = -1)
{
    pBot->nGoalX = x, pBot->nGoalY = y, pBot->nGoalSect = nSect;
    pBot->nGoalKind = nKind, pBot->nGoalItem = nItem;
    pBot->nGoalTime = nTic;
    pBot->nPath = pBot->nPathPos = 0;
}

// the goals of the match types (true: one was set)
bool ModeGoalTo(BOT *pBot, DukePlayer_t *ps, int x, int y, int nSect, int nKind)
{
    if ((unsigned)nSect >= (unsigned)numsectors || !FindPath(pBot, ps->cursectnum, nSect))
        return false;
    SetGoal(pBot, x, y, nSect, nKind);
    return true;
}

bool ModeGoal(int nBot, BOT *pBot)
{
    auto *ps = PlayerOf(nBot);
    if (ModeActive(kBotModeSurvival))
    {
        // the nearest monster it can get to
        int nBest = -1, nBestDist = INT32_MAX;
        for (int nStat : { STAT_ACTOR, STAT_ZOMBIEACTOR })
            for (int i = headspritestat[nStat]; i >= 0; i = nextspritestat[i])
                if (A_CheckEnemySprite(&sprite[i]) && sprite[i].extra > 0)
                {
                    int const d = Dist2D(sprite[i].x - ps->pos.x, sprite[i].y - ps->pos.y) + BotRand(pBot, 2000);
                    if (d < nBestDist)
                        nBestDist = d, nBest = i;
                }
        return nBest >= 0 && BotRand(pBot, 100) < 80 && ModeGoalTo(pBot, ps, sprite[nBest].x, sprite[nBest].y, sprite[nBest].sectnum, 2);
    }
    if (ModeActive(kBotModeCaptureFlag))
    {
        int const nMine = FlagOfTeam(PlayerTeam(nBot)), nTheirs = nMine ^ 1;
        FLAG const &mine = flags[nMine], &theirs = flags[nTheirs];
        if (theirs.nCarrier == nBot)  // home with it
        {
            if (ModeGoalTo(pBot, ps, mine.nHomeX, mine.nHomeY, mine.nHomeSect, 5))
                return true;
        }
        if (mine.nCarrier >= 0 && BotRand(pBot, 100) < 60)  // after who has our flag
        {
            auto *pc = PlayerOf(mine.nCarrier);
            if (ModeGoalTo(pBot, ps, pc->pos.x, pc->pos.y, pc->cursectnum, 2))
                return true;
        }
        if (!mine.bHome && mine.nCarrier < 0 && mine.nSprite >= 0)  // our flag lies somewhere: take it home
        {
            auto const &f = sprite[mine.nSprite];
            if (ModeGoalTo(pBot, ps, f.x, f.y, f.sectnum, 5))
                return true;
        }
        // half go for their flag, the others stay near home (by player number, so it doesn't flip)
        if (theirs.nCarrier < 0 && theirs.nSprite >= 0 && ((nBot & 1) || BotRand(pBot, 100) < 30))
        {
            auto const &f = sprite[theirs.nSprite];
            if (ModeGoalTo(pBot, ps, f.x, f.y, f.sectnum, 5))
                return true;
        }
        if (Dist2D(ps->pos.x - mine.nHomeX, ps->pos.y - mine.nHomeY) > 3000 && BotRand(pBot, 100) < 50)
        {
            if (ModeGoalTo(pBot, ps, mine.nHomeX + BotRand(pBot, 1024) - 512, mine.nHomeY + BotRand(pBot, 1024) - 512, mine.nHomeSect, 4))
                return true;
        }
        return false;
    }
    if (ModeActive(kBotModeHideSeek))
    {
        if (!IsSeeker(nBot))
        {
            // a hiding place: somewhere far from the seekers, then keep still
            if (pBot->nGoalKind == 6)
                return true;
            int nBest = -1, nBestScore = -1;
            for (int nTry = 0; nTry < 24; nTry++)
            {
                int const sct = BotRand(pBot, numsectors);
                if (AreaSize(sct) < BigArea() || !FindPath(pBot, ps->cursectnum, sct))
                    continue;
                int nScore = INT32_MAX;
                for (int TRAVERSE_CONNECT(p))
                    if (IsSeeker(p))
                        nScore = min(nScore, Dist2D(PlayerOf(p)->pos.x - sectCenterX[sct], PlayerOf(p)->pos.y - sectCenterY[sct]));
                nScore = min(nScore, 20000) + BotRand(pBot, 3000);
                if (nScore > nBestScore)
                    nBestScore = nScore, nBest = sct;
            }
            if (nBest >= 0)
            {
                if (ModeGoalTo(pBot, ps, sectCenterX[nBest], sectCenterY[nBest], nBest, 6))
                    return true;
            }
            return false;
        }
        if (nHideTic < kHideFreezeTics)
            return true;  // waiting
        // a seeker: go where a hider is (they hear them), now and then somewhere else
        int nBest = -1, nBestDist = INT32_MAX;
        for (int TRAVERSE_CONNECT(p))
        {
            if (IsSeeker(p) || !PlayerAlive(p))
                continue;
            int const d = Dist2D(PlayerOf(p)->pos.x - ps->pos.x, PlayerOf(p)->pos.y - ps->pos.y) + BotRand(pBot, 8000);
            if (d < nBestDist)
                nBestDist = d, nBest = p;
        }
        if (nBest >= 0 && BotRand(pBot, 100) < 75)
        {
            auto *po = PlayerOf(nBest);
            if (ModeGoalTo(pBot, ps, po->pos.x + BotRand(pBot, 2048) - 1024, po->pos.y + BotRand(pBot, 2048) - 1024, po->cursectnum, 2))
                return true;
        }
        return false;
    }
    if (ModeActive(kBotModeJuggernaut) && nJuggernaut >= 0 && nJuggernaut != nBot && PlayerAlive(nJuggernaut) && BotRand(pBot, 100) < 70 &&
        FindPath(pBot, ps->cursectnum, PlayerOf(nJuggernaut)->cursectnum))
    {
        auto *pj = PlayerOf(nJuggernaut);
        if (ModeGoalTo(pBot, ps, pj->pos.x, pj->pos.y, pj->cursectnum, 2))
            return true;
    }
    return false;
}

bool PickItemGoal(int nBot, BOT *pBot)
{
    auto *ps = PlayerOf(nBot);
    int nBest = -1, nBestScore = 0;
    for (int i = 0; i < nItems; i++)
    {
        int const nSprite = itemList[i];
        if (ItemIgnored(pBot, nSprite))
            continue;
        int const value = ItemValue(ps, nSprite);
        if (value <= 0)
            continue;
        int const nDist = Dist2D(sprite[nSprite].x - ps->pos.x, sprite[nSprite].y - ps->pos.y);
        int const score = value * 64 - nDist / 8;
        if (score > nBestScore)
            nBestScore = score, nBest = nSprite;
    }
    if (nBest < 0)
        return false;
    SetGoal(pBot, sprite[nBest].x, sprite[nBest].y, sprite[nBest].sectnum, 1, nBest);
    return true;
}

void PickGoal(int nBot, BOT *pBot)
{
    auto *ps = PlayerOf(nBot);
    if (ModeActive(kBotModeSurvival) && ModeGoal(nBot, pBot))
        return;
    // co-op: stay with the first player (you)
    if (CoopGame() && nBot != 0 && PlayerAlive(0))
    {
        auto *pLead = PlayerOf(0);
        if (Dist2D(pLead->pos.x - ps->pos.x, pLead->pos.y - ps->pos.y) > 2500)
        {
            SetGoal(pBot, pLead->pos.x, pLead->pos.y, pLead->cursectnum, 3);
            return;
        }
    }
    // an enemy seen a moment ago: go after it (sometimes; an item it really needs comes first)
    if (pBot->nLastSeenSect >= 0 && nTic - pBot->nLastSeenTime < 30 * 8 && BotRand(pBot, 100) < 60)
    {
        SetGoal(pBot, pBot->nLastSeenX, pBot->nLastSeenY, pBot->nLastSeenSect, 2);
        pBot->nLastSeenSect = -1;
        return;
    }
    if (ModeGoal(nBot, pBot))
        return;
    // King of the Hill: the hill (health first when nearly dead)
    if (ModeActive(kBotModeHill) && nHillSect >= 0 && (sprite[ps->i].extra > 30 || BotRand(pBot, 100) < 50))
    {
        // somewhere on the hill: its middle or a sector around it
        int nSect = nHillSect;
        for (int w = sector[nHillSect].wallptr, e = w + sector[nHillSect].wallnum, n = 0; w < e; w++)
            if (wall[w].nextsector >= 0 && inHill[wall[w].nextsector] && BotRand(pBot, ++n + 1) == 0)
                nSect = wall[w].nextsector;
        SetGoal(pBot, sectCenterX[nSect] + BotRand(pBot, 512) - 256, sectCenterY[nSect] + BotRand(pBot, 512) - 256, nSect, 4);
        return;
    }
    // something it needs badly (a better weapon, health when hurt) comes first
    int const nHealth = sprite[ps->i].extra;
    bool const bArmed = HasWeapon(ps, SHOTGUN_WEAPON) || HasWeapon(ps, CHAINGUN_WEAPON) || HasWeapon(ps, RPG_WEAPON) || HasWeapon(ps, DEVISTATOR_WEAPON);
    if ((!bArmed || nHealth < 50 || BotRand(pBot, 100) < 40) && PickItemGoal(nBot, pBot))
        return;
    // DukeMatch: go where an opponent is (you hear them: shots, steps, the jetpack...)
    if (!CoopGame() && BotRand(pBot, 100) < 70)
    {
        int nBest = -1, nBestDist = INT32_MAX;
        for (int TRAVERSE_CONNECT(p))
        {
            if (p == nBot || !PlayerAlive(p) || !IsEnemy(nBot, p))
                continue;
            auto *po = PlayerOf(p);
            int const d = Dist2D(po->pos.x - ps->pos.x, po->pos.y - ps->pos.y) + BotRand(pBot, 6000);
            if (d < nBestDist && FindPath(pBot, ps->cursectnum, po->cursectnum))
                nBestDist = d, nBest = p;
        }
        if (nBest >= 0)
        {
            auto *po = PlayerOf(nBest);
            SetGoal(pBot, po->pos.x, po->pos.y, po->cursectnum, 2);
            return;
        }
    }
    if (PickItemGoal(nBot, pBot))
        return;
    // somewhere else in the level it can get to
    for (int nTry = 0; nTry < 12; nTry++)
    {
        int const s = BotRand(pBot, numsectors);
        if ((sector[s].lotag & 0x7fff) == ST_2_UNDERWATER || sector[s].floorz - sector[s].ceilingz < (32 << 8))
            continue;
        if (!FindPath(pBot, ps->cursectnum, s))
            continue;
        SetGoal(pBot, sectCenterX[s], sectCenterY[s], s, 0);
        return;
    }
    // nothing found: a few sectors further, any way it can go (and a fresh look in a moment)
    int nSect = ps->cursectnum;
    for (int nStep = 0; nStep < 6 && nSect >= 0; nStep++)
    {
        int nNext = -1, nCount = 0;
        for (int w = sector[nSect].wallptr, e = w + sector[nSect].wallnum; w < e; w++)
            if (wall[w].nextsector >= 0 && Passable(w) && BotRand(pBot, ++nCount) == 0)
                nNext = wall[w].nextsector;
        if (nNext < 0)
            break;
        nSect = nNext;
    }
    if (nSect >= 0)
        SetGoal(pBot, sectCenterX[nSect], sectCenterY[nSect], nSect, 0);
}

// ---- input --------------------------------------------------------------------------------------

void MakeInput(int nBot, int nForward, int nStrafe, uint32_t nBits)
{
    auto *ps = PlayerOf(nBot);
    int const ang = fix16_to_int(ps->q16ang);
    auto &input = inputfifo[0][nBot];
    input = {};
    input.bits = nBits;
    // the same turn into world coordinates as the local player's (dukeFillInputForTic)
    input.fvel = mulscale9(nForward, sintable[(ang + 2560) & 2047]) + mulscale9(nStrafe, sintable[(ang + 2048) & 2047]);
    input.svel = mulscale9(nForward, sintable[(ang + 2048) & 2047]) + mulscale9(nStrafe, sintable[(ang + 1536) & 2047]);
    if (!FURY)
    {
        input.fvel += ps->fric.x;
        input.svel += ps->fric.y;
    }
}

// turns towards an angle at most nMax a tic
void TurnTo(DukePlayer_t *ps, int nAng, int nMax)
{
    int const cur = fix16_to_int(ps->q16ang) & 2047;
    int const d = AngDiff(nAng, cur);
    int const step = clamp(d, -nMax, nMax);
    ps->q16ang = fix16_from_int((cur + step) & 2047);
}

void LookAt(DukePlayer_t *ps, int nDist, int dz)
{
    int horiz = 100;
    if (nDist > 0)
        horiz = 100 - (int)((512LL * dz) / nDist);
    horiz = clamp(horiz, 100 - 80, 100 + 80);
    ps->q16horiz = fix16_from_int(horiz);
}

// ---- thinking -------------------------------------------------------------------------------------

void BotThink(int nBot)
{
    BOT *pBot = &bots[nBot];
    auto *ps = PlayerOf(nBot);
    SKILL const &sk = kSkills[clamp(gBotSkill, 0, kBotSkillMax - 1)];

    if (!ps || ps->i < 0)
        return;

    // dead: press USE to come back (DukeMatch); in co-op the game restarts the level
    if (!PlayerAlive(nBot))
    {
        pBot->nTarget = -1;
        pBot->nPath = 0;
        uint32_t nBits = (nTic - pBot->nRespawnTime) % 16 < 4 && !PlayerOut(nBot) ? (1u << SK_OPEN) : 0;
        if (nTic - pBot->nRespawnTime > 64)
            pBot->nRespawnTime = nTic;
        MakeInput(nBot, 0, 0, nBits);
        pBot->nGoalKind = -1;
        pBot->nStatDead++;
        pBot->bWasDead = true;
        return;
    }

    // just came back: a spawn spot it can't get away from (a closed roof...) - DukeMatch picks another one
    if (pBot->bWasDead)
    {
        pBot->bWasDead = false;
        if (!CoopGame() && AreaSize(ps->cursectnum) < max(24, numsectors / 6) && pBot->nRespawnTries < 6)
        {
            pBot->nRespawnTries++;
            P_ResetMultiPlayer(nBot);
            pBot->bWasDead = true;  // look again next tic
            MakeInput(nBot, 0, 0, 0);
            return;
        }
        pBot->nRespawnTries = 0;
    }

    int const myAng = fix16_to_int(ps->q16ang) & 2047;
    uint32_t nBits = (1u << SK_RUN);
    int nForward = 0, nStrafe = 0;
    int const kMove = NORMALKEYMOVE << 1;  // running

    // ---- an enemy in sight?
    if ((nTic + nBot) % 3 == 0 || pBot->nTarget >= 0)
    {
        int const nNew = FindTarget(nBot, pBot);
        if (nNew != pBot->nTarget)
        {
            pBot->nTarget = nNew;
            pBot->nTargetSince = 0;
        }
    }
    if (pBot->nTarget >= 0)
    {
        auto const &t = sprite[pBot->nTarget];
        if (t.statnum == MAXSTATUS || t.extra <= 0)
            pBot->nTarget = -1;
    }

    // damage done to the target since the last tic (debug statistics)
    if (pBot->nTarget >= 0 && pBot->nTarget == pBot->nLastTarget && sprite[pBot->nTarget].extra < pBot->nLastTargetHealth)
        pBot->nStatDamage += pBot->nLastTargetHealth - sprite[pBot->nTarget].extra;
    pBot->nLastTarget = pBot->nTarget;
    pBot->nLastTargetHealth = pBot->nTarget >= 0 ? sprite[pBot->nTarget].extra : 0;

    bool bFighting = false;
    if (pBot->nTarget >= 0)
    {
        auto const &t = sprite[pBot->nTarget];
        pBot->nTargetSince++;
        pBot->nLastSeenX = t.x, pBot->nLastSeenY = t.y, pBot->nLastSeenZ = t.z, pBot->nLastSeenSect = t.sectnum;
        pBot->nLastSeenTime = nTic;

        int const dx = t.x - ps->pos.x, dy = t.y - ps->pos.y;
        int const nDist = Dist2D(dx, dy);

        // weapon for the distance
        int const w = BestWeapon(ps, nDist);
        if (ps->curr_weapon != w)
            nBits |= (uint32_t)(w + 1) << SK_WEAPON_BITS;

        if (pBot->nTargetSince >= sk.nReaction)
        {
            bFighting = true;
            // aim: at the target, with a wobble that changes now and then (and grows while running sideways)
            if (nTic >= pBot->nAimUntil)
            {
                pBot->nAimOffset = BotRand(pBot, sk.nAimError * 2 + 1) - sk.nAimError;
                pBot->nAimUntil = nTic + 8 + BotRand(pBot, 16);
            }
            int const aimAng = (getangle(dx, dy) + pBot->nAimOffset) & 2047;
            TurnTo(ps, aimAng, sk.nTurn);
            LookAt(ps, nDist, TargetZ(pBot->nTarget) - ps->pos.z);
            if (klabs(AngDiff(getangle(dx, dy), fix16_to_int(ps->q16ang) & 2047)) < sk.nFireCone + 2000 * 64 / max(nDist, 64))
                nBits |= (1u << SK_FIRE);

            // keep the distance that suits the weapon, dodge sideways
            int const pref = PreferredDistance(ps->curr_weapon);
            if (nDist > pref + 600)
                nForward = kMove;
            else if (nDist < pref - 600)
                nForward = -kMove;
            if (nTic >= pBot->nStrafeUntil)
            {
                pBot->nStrafeDir = BotRand(pBot, 100) < sk.nStrafe ? (BotRand(pBot, 2) ? 1 : -1) : 0;
                pBot->nStrafeUntil = nTic + 15 + BotRand(pBot, 35);
            }
            nStrafe = pBot->nStrafeDir * kMove;
            if (BotRand(pBot, 200) == 0)
                pBot->nJumpUntil = nTic + 4;  // a hop now and then
        }
        else
            TurnTo(ps, getangle(dx, dy), sk.nTurn / 2);  // noticed it: turning, not shooting yet
    }

    // ---- shut in a small area: shoot a way out
    if (!bFighting && nTic - pBot->nAreaTime > 90)
    {
        pBot->nAreaTime = nTic;
        pBot->nAreaSize = AreaSize(ps->cursectnum);
        int const nWas = pBot->nBreak;
        pBot->nBreak = !CoopGame() && pBot->nAreaSize < max(24, numsectors / 6) ? FindBreakable(pBot, ps) : -1;
        if (pBot->nBreak >= 0 && pBot->nBreak != nWas)
            pBot->nBreakHealth = sprite[pBot->nBreak].extra, pBot->nBreakSince = nTic;
    }
    if (!bFighting && pBot->nBreak >= 0)
    {
        auto const &b = sprite[pBot->nBreak];
        if (b.extra < pBot->nBreakHealth)
            pBot->nBreakHealth = b.extra, pBot->nBreakSince = nTic;
        if (b.statnum == MAXSTATUS || b.extra <= 0 || (b.cstat & 32768))
            pBot->nBreak = -1, pBot->nAreaTime = 0;
        else if (nTic - pBot->nBreakSince > 30 * 8)
        {
            IgnoreItem(pBot, pBot->nBreak, 30 * 120);  // bullets don't hurt it: not this one
            pBot->nBreak = -1;
        }
        else
        {
            int const dx = b.x - ps->pos.x, dy = b.y - ps->pos.y, nDist = Dist2D(dx, dy);
            int const w = HasWeapon(ps, PISTOL_WEAPON) ? PISTOL_WEAPON : BestWeapon(ps, nDist);
            if (ps->curr_weapon != w)
                nBits |= (uint32_t)(w + 1) << SK_WEAPON_BITS;
            TurnTo(ps, getangle(dx, dy), sk.nTurn);
            LookAt(ps, nDist, (b.z - (8 << 8)) - ps->pos.z);
            if (nDist < 1600)
                nForward = -(NORMALKEYMOVE << 1);  // not too close: it may explode
            else if (klabs(AngDiff(getangle(dx, dy), fix16_to_int(ps->q16ang) & 2047)) < 24)
                nBits |= (1u << SK_FIRE);
            if (nBits & (1u << SK_FIRE))
                pBot->nStatFire++;
            pBot->nStatWalk++;
            MakeInput(nBot, nForward, 0, nBits);
            return;
        }
    }

    // ---- walking somewhere
    if (!bFighting)
    {
        LookAt(ps, 0, 0);
        // a goal that is done or too old: a new one
        bool bNew = pBot->nGoalKind < 0 || nTic - pBot->nGoalTime > 30 * 25 || (pBot->nGoalKind == 2 && nTic - pBot->nGoalTime > 30 * 4);
        if (!bNew && pBot->nGoalKind == 1)
        {
            int const nItem = pBot->nGoalItem;
            if (ItemValue(ps, nItem) <= 0 || Dist2D(sprite[nItem].x - ps->pos.x, sprite[nItem].y - ps->pos.y) < 200)
                bNew = true;
        }
        else if (!bNew && Dist2D(pBot->nGoalX - ps->pos.x, pBot->nGoalY - ps->pos.y) < 400)
            bNew = true;
        if (!bNew && nTic % 60 == nBot && pBot->nGoalKind == 0 && PickItemGoal(nBot, pBot))
            bNew = false;  // something worth having came up
        if (bNew)
            PickGoal(nBot, pBot);

        if (pBot->nPath == 0 && pBot->nGoalSect >= 0)
        {
            if (!FindPath(pBot, ps->cursectnum, pBot->nGoalSect))
            {
                pBot->nStatPathFail++;
                if (pBot->nGoalKind == 1)
                    IgnoreItem(pBot, pBot->nGoalItem, 30 * 30);
                pBot->nGoalKind = -1;  // unreachable: something else next tic
            }
        }

        // where to walk: through the next portal of the path, or straight at the goal in its sector
        int tx = pBot->nGoalX, ty = pBot->nGoalY;
        if (pBot->nPath > 0)
        {
            while (pBot->nPathPos < pBot->nPath - 1 && ps->cursectnum == pBot->path[pBot->nPathPos + 1])
                pBot->nPathPos++;
            // still on the path?
            bool bOnPath = false;
            for (int i = pBot->nPathPos; i < pBot->nPath && !bOnPath; i++)
                if (pBot->path[i] == ps->cursectnum)
                    bOnPath = true, pBot->nPathPos = i;
            if (!bOnPath)
                pBot->nPath = 0;  // fell off: plan again
            else if (pBot->nPathPos < pBot->nPath - 1)
            {
                int const nNext = pBot->path[pBot->nPathPos + 1];
                int const nWall = PortalWall(ps->cursectnum, nNext);
                if (nWall >= 0)
                {
                    PortalMid(nWall, &tx, &ty);
                    // aim a bit into the next sector, so the bot doesn't graze the door frame
                    tx += (sectCenterX[nNext] - tx) / 6;
                    ty += (sectCenterY[nNext] - ty) / 6;
                    // a closed door or a lift on the way: USE when close
                    if ((IsDoorSector(nNext) || IsLiftSector(nNext)) && Dist2D(tx - ps->pos.x, ty - ps->pos.y) < 900 && nTic - pBot->nOpenTime > 20)
                    {
                        nBits |= (1u << SK_OPEN);
                        pBot->nOpenTime = nTic;
                    }
                    // a step up: jump
                    int const fa = getflorzofslope(ps->cursectnum, tx, ty), fb = getflorzofslope(nNext, tx, ty);
                    if (fa - fb > (18 << 8) && Dist2D(tx - ps->pos.x, ty - ps->pos.y) < 700)
                        pBot->nJumpUntil = nTic + 3;
                }
            }
        }
        int const dx = tx - ps->pos.x, dy = ty - ps->pos.y;
        if (dx || dy)
        {
            int const want = getangle(dx, dy);
            TurnTo(ps, want, sk.nTurn);
            int const off = klabs(AngDiff(want, fix16_to_int(ps->q16ang) & 2047));
            nForward = off < 300 ? kMove : off < 600 ? kMove / 2 : 0;
        }
    }

    // ---- stuck?
    if (nTic - pBot->nStuckTime >= 16)
    {
        bool const bTrying = nForward != 0 || nStrafe != 0;
        if (bTrying && Dist2D(ps->pos.x - pBot->nStuckX, ps->pos.y - pBot->nStuckY) < 64)
        {
            pBot->nStuck++;
            pBot->nStatStuck++;
            pBot->nJumpUntil = nTic + 4;
            nBits |= (1u << SK_OPEN);
            pBot->nStrafeDir = BotRand(pBot, 2) ? 1 : -1;
            pBot->nStrafeUntil = nTic + 12;
            if (pBot->nStuck >= 4)
            {
                if (pBot->nGoalKind == 1)
                    IgnoreItem(pBot, pBot->nGoalItem, 30 * 20);
                pBot->nGoalKind = -1;
                pBot->nStuck = 0;
            }
        }
        else
            pBot->nStuck = 0;
        pBot->nStuckX = ps->pos.x, pBot->nStuckY = ps->pos.y, pBot->nStuckTime = nTic;
    }
    if (!bFighting && pBot->nStuck > 0 && nTic < pBot->nStrafeUntil)
        nStrafe = pBot->nStrafeDir * (NORMALKEYMOVE << 1);
    if (nTic < pBot->nJumpUntil)
        nBits |= (1u << SK_JUMP);

    (void)myAng;
    if (bFighting)
        pBot->nStatFight++;
    else if (pBot->nTarget >= 0)
        pBot->nStatSeen++;
    else
        pBot->nStatWalk++;
    if (nBits & (1u << SK_FIRE))
        pBot->nStatFire++;
    MakeInput(nBot, nForward, nStrafe, nBits);
}

// the match, from the host to everybody (PACKET_CHOSEN_MODE)
void SendState(void)
{
    uint8_t buf[160];
    int l = 0;
    buf[l++] = PACKET_CHOSEN_MODE;
    buf[l++] = nMode;
    buf[l++] = (uint8_t)(int8_t)nJuggernaut;
    buf[l++] = nHillSect & 255, buf[l++] = (nHillSect >> 8) & 255;
    buf[l++] = nSeekers & 255, buf[l++] = nSeekers >> 8;
    buf[l++] = min(nHideTic, 65535) & 255, buf[l++] = min(nHideTic, 65535) >> 8;
    buf[l++] = teamScore[0], buf[l++] = teamScore[1];
    buf[l++] = (uint8_t)(int8_t)flags[0].nCarrier, buf[l++] = (uint8_t)(int8_t)flags[1].nCarrier;
    buf[l++] = flags[0].bHome | (flags[1].bHome << 1);
    buf[l++] = nWave, buf[l++] = nWaveMonsters & 255, buf[l++] = nWaveMonsters >> 8, buf[l++] = bWaveRunning;
    for (int p = 0; p < MAXPLAYERS; p++)
    {
        buf[l++] = gunLevel[p], buf[l++] = clamp(lives[p], 0, 255);
        buf[l++] = hillPoints[p] & 255, buf[l++] = (hillPoints[p] >> 8) & 255;
        buf[l++] = nDecorPic[p] & 255, buf[l++] = nDecorPic[p] >> 8;
        buf[l++] = isBot[p];
    }
    Net_SendModeState(buf, l);
}

} // namespace

static int nLoggedMode = -1;

void botReceiveModeState(uint8_t const *buf, int nLength)
{
    if (nLength < 18 + MAXPLAYERS * 7)
        return;
    int l = 1;
    nMode = clamp<int>(buf[l++], 0, kBotModeMax - 1);
    nJuggernaut = (int8_t)buf[l++];
    int const nHill = (int16_t)(buf[l] | (buf[l + 1] << 8));
    l += 2;
    if (nHill != nHillSect)
        MarkHill(nHill);
    nSeekers = buf[l] | (buf[l + 1] << 8), l += 2;
    nHideTic = buf[l] | (buf[l + 1] << 8), l += 2;
    teamScore[0] = buf[l++], teamScore[1] = buf[l++];
    flags[0].nCarrier = (int8_t)buf[l++], flags[1].nCarrier = (int8_t)buf[l++];
    flags[0].bHome = buf[l] & 1, flags[1].bHome = (buf[l] >> 1) & 1, l++;
    nWave = buf[l++], nWaveMonsters = buf[l] | (buf[l + 1] << 8), l += 2, bWaveRunning = buf[l++];
    for (int p = 0; p < MAXPLAYERS; p++)
    {
        gunLevel[p] = buf[l++], lives[p] = buf[l++];
        hillPoints[p] = buf[l] | (buf[l + 1] << 8), l += 2;
        nDecorPic[p] = buf[l] | (buf[l + 1] << 8), l += 2;
        isBot[p] = buf[l++];  // a client knows which players are bots (the scores, the HUD)
    }
    if (!bFromHost || nMode != nLoggedMode)
        LOG_F(INFO, "Match from the host: %s", botModeName(nMode));
    nLoggedMode = nMode;
    bFromHost = true;
}

int botModeRunning(void)
{
    if (!bActive && !g_netServer && !(g_netClient && bFromHost))
        return -1;
    return nMode;
}

// the look of the players: hiders are decorations, the juggernaut is bigger (after G_DoSpriteAnimations)
void botDrawPlayers(void)
{
    int const nRunning = botModeRunning();
    if (nRunning != kBotModeHideSeek && nRunning != kBotModeJuggernaut)
        return;
    for (int j = 0; j < spritesortcnt; j++)
    {
        auto *t = &tsprite[j];
        if ((unsigned)t->owner >= MAXSPRITES)
            continue;
        auto const &spr = sprite[t->owner];
        if (spr.picnum != APLAYER || (unsigned)spr.yvel >= MAXPLAYERS || !g_player[spr.yvel].ps || g_player[spr.yvel].ps->i != t->owner)
            continue;
        int const p = spr.yvel;
        if (nRunning == kBotModeJuggernaut && p == nJuggernaut)
            t->xrepeat = t->xrepeat * 5 / 4, t->yrepeat = t->yrepeat * 5 / 4;
        if (nRunning == kBotModeHideSeek && !IsSeeker(p) && spr.extra > 0 && nDecorPic[p])
        {
            t->picnum = nDecorPic[p];
            t->xrepeat = t->yrepeat = 32;
            t->pal = 0;
            t->shade = sector[t->sectnum].floorshade;
            t->cstat &= ~(4 | 8 | 16 | 32);
            t->xoffset = t->yoffset = 0;
            t->z = sector[t->sectnum].floorz;
        }
    }
}

// a line at the top of the screen: how the match goes
void botDrawHud(void)
{
    int const nRunning = botModeRunning();
    if (nRunning <= kBotModeDukeMatch)
        return;
    int const me = myconnectindex;
    char text[128] = "";
    switch (nRunning)
    {
    case kBotModeGunGame:
        Bsnprintf(text, sizeof(text), "GUN GAME: WEAPON %d OF %d", gunLevel[me] + 1, kGunGameSteps);
        break;
    case kBotModeHill:
    {
        int nLead = me;
        for (int TRAVERSE_CONNECT(p))
            if (hillPoints[p] > hillPoints[nLead])
                nLead = p;
        Bsnprintf(text, sizeof(text), "HILL: YOU %d  LEAD %s %d  (%d WINS)%s", hillPoints[me], g_player[nLead].user_name, hillPoints[nLead], kHillWin,
                  (unsigned)g_player[me].ps->cursectnum < (unsigned)numsectors && inHill[g_player[me].ps->cursectnum] ? "  ON THE HILL" : "");
        break;
    }
    case kBotModeLastMan:
        Bsnprintf(text, sizeof(text), "LAST MAN STANDING: %d %s LEFT", lives[me], lives[me] == 1 ? "LIFE" : "LIVES");
        break;
    case kBotModeJuggernaut:
        Bsnprintf(text, sizeof(text), "JUGGERNAUT: %s   YOUR POINTS %d OF %d", nJuggernaut == me ? "YOU!" : nJuggernaut >= 0 ? g_player[nJuggernaut].user_name : "-",
                  hillPoints[me], kJuggernautWin);
        break;
    case kBotModeCaptureFlag:
        Bsnprintf(text, sizeof(text), "BLUE %d - RED %d   BLUE FLAG %s  RED FLAG %s", teamScore[0], teamScore[1],
                  flags[0].nCarrier >= 0 ? "TAKEN" : flags[0].bHome ? "HOME" : "DROPPED", flags[1].nCarrier >= 0 ? "TAKEN" : flags[1].bHome ? "HOME" : "DROPPED");
        break;
    case kBotModeHideSeek:
    {
        int nHiders = 0;
        for (int TRAVERSE_CONNECT(p))
            nHiders += !IsSeeker(p);
        int const nLeft = max(0, (nHideTic < kHideFreezeTics ? kHideFreezeTics : kHideRoundTics) - nHideTic) / 30;
        Bsnprintf(text, sizeof(text), "%s  HIDERS LEFT %d  %s %d:%02d", IsSeeker(me) ? "SEEKER" : "HIDER", nHiders,
                  nHideTic < kHideFreezeTics ? "SEEKERS COME IN" : "TIME", nLeft / 60, nLeft % 60);
        break;
    }
    case kBotModeSurvival:
        if (bWaveRunning)
            Bsnprintf(text, sizeof(text), "WAVE %d: %d MONSTERS LEFT   LIVES %d", nWave, nWaveMonsters, lives[me]);
        else
            Bsnprintf(text, sizeof(text), "SURVIVAL: WAVE %d IS COMING   LIVES %d", nWave + 1, lives[me]);
        break;
    }
    if (text[0])
        minitext(160 - (int)strlen(text) * 2, 30, text, 0, 2 + 8 + 16 + ROTATESPRITE_MAX);
}

// the game type a match type needs (survival: co-op, capture the flag: teams; the others: not co-op)
void botFixGameType(int nMatch)
{
    auto findType = [](int nWant, int nNot) {
        for (int i = 0; i < MAXGAMETYPES && g_gametypeNames[i][0]; i++)
            if ((g_gametypeFlags[i] & nWant) == nWant && !(g_gametypeFlags[i] & nNot))
                return i;
        return -1;
    };
    int nType = -1;
    if (nMatch == kBotModeSurvival && !(g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP))
        nType = findType(GAMETYPE_COOP, 0);
    else if (nMatch == kBotModeCaptureFlag && !(g_gametypeFlags[ud.m_coop] & GAMETYPE_TDM))
        nType = findType(GAMETYPE_TDM, GAMETYPE_COOP);
    else if (nMatch != kBotModeSurvival && nMatch != kBotModeDukeMatch && (g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP))
        nType = findType(0, GAMETYPE_COOP);
    if (nType >= 0)
    {
        ud.m_coop = nType;
        LOG_F(INFO, "%s: game type %s", botModeName(nMatch), g_gametypeNames[nType]);
    }
    if (nMatch == kBotModeSurvival)
        ud.m_monsters_off = 0;
}

// ---- the match ------------------------------------------------------------------------------------

bool botMatchActive(void)
{
    return bActive || (g_netServer && nNetBots > 0);
}

bool botIsBot(int nPlayer)
{
    return (unsigned)nPlayer < MAXPLAYERS && isBot[nPlayer] && g_player[nPlayer].ps && (bActive || g_netServer);
}

bool botNetGameHasBots(void)
{
    return g_netServer && nNetBots > 0;
}

int botNetCount(void)
{
    return g_netServer ? nNetBots : 0;
}

const char *botModeName(int nMode)
{
    return kModeNames[clamp(nMode, 0, kBotModeMax - 1)];
}

void botOnDeath(int nPlayer, int nKiller)
{
    if (!ModeAuthority() || (!bActive && !g_netServer) || (unsigned)nPlayer >= MAXPLAYERS)
        return;
    bool const bByPlayer = nKiller >= 0 && nKiller < MAXPLAYERS && nKiller != nPlayer;
    char buffer[96];
    if ((ModeActive(kBotModeLastMan) || ModeActive(kBotModeSurvival)) && lives[nPlayer] > 0)
    {
        lives[nPlayer]--;
        if (lives[nPlayer] == 0)
            Bsnprintf(buffer, sizeof(buffer), ModeActive(kBotModeSurvival) ? "%s is out until the next wave" : "%s is out", g_player[nPlayer].user_name);
        else
            Bsnprintf(buffer, sizeof(buffer), "%s has %d %s left", g_player[nPlayer].user_name, lives[nPlayer], lives[nPlayer] == 1 ? "life" : "lives");
        G_AddUserQuote(buffer), Net_BroadcastQuote(buffer);
    }
    if (ModeActive(kBotModeGunGame) && bByPlayer)
    {
        if (gunLevel[nKiller] >= kGunGameSteps - 1)
        {
            Bsnprintf(buffer, sizeof(buffer), "%s wins the Gun Game!", g_player[nKiller].user_name);
            Announce(buffer);
            ResetMode();
        }
        else
            gunLevel[nKiller]++;
    }
    if (ModeActive(kBotModeJuggernaut))
    {
        if (nPlayer == nJuggernaut)
        {
            if (bByPlayer)
            {
                hillPoints[nKiller]++;
                MakeJuggernaut(nKiller);
            }
            else
                nJuggernaut = -1;  // somebody else (ModeTick)
        }
        else if (bByPlayer && nKiller == nJuggernaut)
            hillPoints[nKiller]++;
        if (bByPlayer && hillPoints[nKiller] >= kJuggernautWin)
        {
            Bsnprintf(buffer, sizeof(buffer), "%s wins the Juggernaut match!", g_player[nKiller].user_name);
            Announce(buffer);
            ResetMode();
        }
    }
    if (ModeActive(kBotModeHideSeek) && !IsSeeker(nPlayer))
    {
        nSeekers |= 1 << nPlayer;  // found: a seeker now
        if (bByPlayer)
            hillPoints[nKiller]++;
        Bsnprintf(buffer, sizeof(buffer), "%s was found!", g_player[nPlayer].user_name);
        Announce(buffer);
    }
}

const char *botSkillName(int nSkill)
{
    return kSkills[clamp(nSkill, 0, kBotSkillMax - 1)].pzName;
}

void botLevelStart(void)
{
    if (!bActive && !g_netServer)
        return;
    LearnLevel();
    nItemsScanned = -1000;
    for (int i = 0; i < MAXPLAYERS; i++)
    {
        BOT *pBot = &bots[i];
        uint32_t const nSeed = pBot->nSeed ? pBot->nSeed : 0x9e3779b9u * (i + 1);
        Bmemset(pBot, 0, sizeof(*pBot));
        pBot->nSeed = nSeed;
        pBot->nTarget = -1;
        pBot->nGoalKind = -1;
        pBot->nGoalSect = -1;
        pBot->nLastSeenSect = -1;
        pBot->nBreak = -1;
        pBot->bWasDead = true;  // the start spot gets the same check as a respawn
    }
}

// the players of a bot match: you and nBots bots (also for loading a saved bot game)
static void SetupPlayers(int nBots)
{
    nBots = clamp(nBots, 1, kBotMaxBots);
    bActive = true;
    nNetBots = 0;
    Bmemset(isBot, 0, sizeof(isBot));
    for (int i = 1; i <= nBots; i++)
        isBot[i] = true;
    ud.multimode = 1 + nBots;
    g_fakeMultiMode = ud.multimode;
    g_mostConcurrentPlayers = ud.multimode;
    for (int i = 0; i < ud.multimode - 1; i++)
        connectpoint2[i] = i + 1;
    connectpoint2[ud.multimode - 1] = -1;
    int nName = (int)(timerGetTicks() / 7) % ARRAY_SIZE(kBotNames);
    for (int i = 0; i < ud.multimode; i++)
    {
        G_MaybeAllocPlayer(i);
        g_player[i].playerquitflag = 1;
        if (i > 0)
        {
            Bstrncpyz(g_player[i].user_name, kBotNames[(nName + i) % ARRAY_SIZE(kBotNames)], sizeof(g_player[i].user_name));
            g_player[i].pcolor = 9 + (i % 8);
            Bmemset(&inputfifo[0][i], 0, sizeof(input_t));
        }
        if (i == 0 && !g_player[0].user_name[0])
            Bstrncpyz(g_player[0].user_name, szPlayerName[0] ? szPlayerName : "Duke", sizeof(g_player[0].user_name));
        // auto aim for the bots as for a player with it on (they aim like one: roughly, the game helps)
        if (i > 0)
            g_player[i].ps->auto_aim = 1;
        // teams: you and every other bot against the rest
        g_player[i].pteam = i & 1;
        g_player[i].ps->team = i & 1;
    }
}

bool botPrepareLoad(int nPlayers)
{
    if (g_netServer || g_netClient || nPlayers < 2 || nPlayers > 1 + kBotMaxBots)
        return false;
    SetupPlayers(nPlayers - 1);
    nMode = clamp(gBotMode, 0, kBotModeMax - 1);
    nLevelId = -1;  // learn the level again after loading
    LOG_F(INFO, "Bot match: loading a saved game with %d bots", nPlayers - 1);
    return true;
}

void botStartMatch(void)
{
    if (g_netServer || g_netClient)
        return;
    int const nBots = clamp(gBotCount, 1, kBotMaxBots);
    SetupPlayers(nBots);
    if (!(g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP))
    {
        ud.m_respawn_items = ud.m_respawn_inventory = 1;
        ud.m_marker = 1;
    }
    nMode = clamp(gBotMode, 0, kBotModeMax - 1);
    botFixGameType(nMode);
    LOG_F(INFO, "Bot match: %d bots (%s), %s, %s, E%dL%d", nBots, botSkillName(gBotSkill), botModeName(nMode), g_gametypeNames[ud.m_coop],
          ud.m_volume_number + 1, ud.m_level_number + 1);
    Menu_Change(MENU_CLOSE);
    G_NewGame_EnterLevel();
}

void botEndMatch(void)
{
    nNetBots = 0;
    bFromHost = false;
    nMode = kBotModeDukeMatch;
    if (!bActive)
    {
        Bmemset(isBot, 0, sizeof(isBot));
        return;
    }
    bActive = false;
    Bmemset(isBot, 0, sizeof(isBot));
    ud.multimode = 1;
    g_fakeMultiMode = 0;
    g_mostConcurrentPlayers = 1;
    connectpoint2[0] = -1;
}

void botThinkAll(void)
{
    if ((!bActive && !g_netServer && !g_netClient) || numsectors <= 0 || !(g_player[myconnectindex].ps->gm & MODE_GAME))
        return;
    nTic++;
    if (g_netClient)
    {
        // the host runs the match: here only the rules for this player
        if (bFromHost)
            ModeTick();
        return;
    }
    int const nId = numsectors * 7919 + numwalls;
    if (nId != nLevelId)
    {
        nLevelId = nId;
        botLevelStart();
        ResetMode();
        if (getenv("DUKE3D_BOT_DEBUG"))
            for (int i = 0; i < ud.multimode; i++)
            {
                int nReach = 0, nFrom = PlayerOf(i)->cursectnum;
                for (int s = 0; s < numsectors; s += 4)
                    nReach += FindPath(&bots[MAXPLAYERS - 1], nFrom, s);
                LOG_F(INFO, "bot debug: from sector %d (player %d) %d of %d sampled sectors reachable", nFrom, i, nReach, (numsectors + 3) / 4);

            }
    }
    if (nTic - nItemsScanned > 90)
        ScanItems();
    for (int TRAVERSE_CONNECT(i))
        if (isBot[i])
            BotThink(i);
    ModeTick();

    // DUKE3D_BOT_SAVETEST=1: saves at 10 s, drops the bots, loads the save at 12 s (a test of saved bot games)
    static int const bSaveTest = getenv("DUKE3D_BOT_SAVETEST") ? 1 : 0;
    static savebrief_t testSave("bot save test");
    if (bSaveTest && nTic == 300)
    {
        int const r = G_SavePlayer(testSave, false);
        LOG_F(INFO, "Bot save test: saved (%d) to %s, %d players", r, testSave.path, ud.multimode);
    }
    if (bSaveTest && nTic == 360)
    {
        int const nPlayers = ud.multimode;
        botEndMatch();
        LOG_F(INFO, "Bot save test: back to %d player(s), loading", ud.multimode);
        int const r = G_LoadPlayer(testSave);
        LOG_F(INFO, "Bot save test: loaded (%d): %d players (were %d), bots %s", r, ud.multimode, nPlayers, bActive ? "on" : "off");
    }

    // DUKE3D_BOT_DEBUG=1: what every bot does, every 5 seconds
    static int const bDebug = getenv("DUKE3D_BOT_DEBUG") ? 1 : 0;
    if (bDebug && nTic % 150 == 0 && ModeActive(kBotModeCaptureFlag))
        for (int t = 0; t < 2; t++)
        {
            FLAG const &f = flags[t];
            int const c = f.nCarrier;
            int const nOwn = c >= 0 ? FlagOfTeam(PlayerTeam(c)) : 0;
            LOG_F(INFO, "flag %d: home %d carrier %d%s%s dist to their home %d (z %d vs %d, sect %d)", t, f.bHome, c, c >= 0 ? " team " : "",
                  c >= 0 ? (nOwn ? "red" : "blue") : "", c >= 0 ? Dist2D(PlayerOf(c)->pos.x - flags[nOwn].nHomeX, PlayerOf(c)->pos.y - flags[nOwn].nHomeY) : -1,
                  c >= 0 ? PlayerOf(c)->pos.z >> 8 : 0, flags[nOwn].nHomeZ >> 8, c >= 0 ? PlayerOf(c)->cursectnum : -1);
            if (c >= 0)
            {
                BOT const *pb = &bots[c];
                char line[512];
                int n = Bsnprintf(line, sizeof(line), "carrier path (%d, at %d):", pb->nPath, pb->nPathPos);
                for (int k = 0; k < pb->nPath && n < 480; k++)
                    n += Bsnprintf(line + n, sizeof(line) - n, " %d(lo%d hi%d)", pb->path[k], sector[pb->path[k]].lotag, sector[pb->path[k]].hitag);
                LOG_F(INFO, "%s", line);
            }
        }
    if (bDebug && nTic % 150 == 0)
        for (int TRAVERSE_CONNECT(i))
        {
            if (!isBot[i])
                continue;
            auto *ps = PlayerOf(i);
            BOT *pBot = &bots[i];
            LOG_F(INFO, "bot %d %s: sect %d hp %d weapon %d goal %d | tics: fight %d seen %d walk %d dead %d; path fails %d, stuck %d, firing %d, damage %d",
                  i, g_player[i].user_name, ps->cursectnum, sprite[ps->i].extra, ps->curr_weapon, pBot->nGoalKind,
                  pBot->nStatFight, pBot->nStatSeen, pBot->nStatWalk, pBot->nStatDead, pBot->nStatPathFail, pBot->nStatStuck, pBot->nStatFire, pBot->nStatDamage);
            if (pBot->nBreak >= 0)
                LOG_F(INFO, "bot %d: shut in (%d sectors), breaking sprite %d (pic %d, extra %d, dist %d)", i, pBot->nAreaSize, pBot->nBreak,
                      sprite[pBot->nBreak].picnum, sprite[pBot->nBreak].extra, Dist2D(sprite[pBot->nBreak].x - ps->pos.x, sprite[pBot->nBreak].y - ps->pos.y));
        }

    // a line in the log every 30 seconds: who leads (handy to see how the bots do)
    if (nTic % (30 * 30) == 0)
    {
        char buffer[256];
        int n = Bsnprintf(buffer, sizeof(buffer), "Bot match frags:");
        for (int TRAVERSE_CONNECT(p))
            n += Bsnprintf(buffer + n, sizeof(buffer) - n, " %s %d (%d kills, %d suicides)", g_player[p].user_name[0] ? g_player[p].user_name : "You",
                           g_player[p].ps->frag - g_player[p].ps->fraggedself, g_player[p].ps->frag, g_player[p].ps->fraggedself);
        if (CoopGame())
        {
            int nKilled = 0;
            for (int TRAVERSE_CONNECT(p))
                nKilled += g_player[p].ps->actors_killed;
            Bsnprintf(buffer + n, sizeof(buffer) - n, " | monsters killed %d/%d", nKilled, g_player[0].ps->max_actors_killed);
        }
        LOG_F(INFO, "%s", buffer);
        if (ModeActive(kBotModeHill) || ModeActive(kBotModeLastMan) || ModeActive(kBotModeGunGame))
        {
            int n2 = Bsnprintf(buffer, sizeof(buffer), "Bot match %s:", botModeName(nMode));
            for (int TRAVERSE_CONNECT(p))
                n2 += Bsnprintf(buffer + n2, sizeof(buffer) - n2, " %s %d", g_player[p].user_name,
                                nMode == kBotModeHill ? hillPoints[p] : nMode == kBotModeLastMan ? lives[p] : gunLevel[p] + 1);
            LOG_F(INFO, "%s", buffer);
        }
    }
}

// ---- bots in network games: the host plays them as players without a connection ----

void botNetHostStart(void)
{
    if (!g_netServer)
        return;
    int const nWanted = clamp(gBotNetCount, 0, kBotMaxBots);
    int nName = (int)(timerGetTicks() / 7) % ARRAY_SIZE(kBotNames);
    for (int b = 0; b < nWanted; b++)
    {
        int const nTeam = (numplayers + b) & 1;
        int const nIndex = Net_AddBot(kBotNames[(nName + b) % ARRAY_SIZE(kBotNames)], 9 + ((numplayers + b) % 8), nTeam);
        if (nIndex < 0)
            break;
        isBot[nIndex] = true;
        nNetBots++;
        Bmemset(&inputfifo[0][nIndex], 0, sizeof(input_t));
    }
    nMode = clamp(gBotMode, 0, kBotModeMax - 1);
    nLevelId = -1;
    LOG_F(INFO, "Network game: %d bots (%s), %s", nNetBots, botSkillName(gBotSkill), botModeName(nMode));
}

void botNetPlayerJoined(void)
{
    if (!g_netServer)
        return;
    for (int TRAVERSE_CONNECT(i))
        if (isBot[i])
            Net_SendBotInfo(i);
}

// a bot's slot is wanted by a joining player when the game is full: the bot leaves
bool botNetMakeRoom(void)
{
    if (!g_netServer || nNetBots <= 0)
        return false;
    for (int i = MAXPLAYERS - 1; i > 0; i--)
        if (isBot[i] && g_player[i].playerquitflag)
        {
            isBot[i] = false;
            nNetBots--;
            Net_RemoveBot(i);
            return true;
        }
    return false;
}
