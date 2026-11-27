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

#define NORMALKEYMOVE 40  // as in player.cpp: walking speed of a key press

int32_t gBotCount = 3;
int32_t gBotSkill = 1;
int32_t gBotMode = kBotModeDukeMatch;

namespace {

bool bActive;

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
            if (p == nBot || !PlayerAlive(p))
                continue;
            if (TeamGame() && PlayerTeam(p) == PlayerTeam(nBot))
                continue;
            consider(PlayerOf(p)->i);
        }
    }
    return nBest;
}

// ---- match types --------------------------------------------------------------------------------

const char *const kModeNames[kBotModeMax] = { "DukeMatch", "Gun Game", "King of the Hill", "Last Man Standing" };
int const kGunGameOrder[] = { PISTOL_WEAPON, SHOTGUN_WEAPON, CHAINGUN_WEAPON, FREEZE_WEAPON, RPG_WEAPON, DEVISTATOR_WEAPON, SHRINKER_WEAPON, KNEE_WEAPON };
int const kGunGameSteps = ARRAY_SIZE(kGunGameOrder);
int const kLives = 3;
int const kHillWin = 60;

int nMode;                       // the match type of the running match (co-op: always DukeMatch rules)
int gunLevel[MAXPLAYERS];
int lives[MAXPLAYERS];
int hillPoints[MAXPLAYERS];
int nHillSect = -1;
uint8_t inHill[MAXSECTORS];
int nHillTic;

bool ModeActive(int nWhich)
{
    return nMode == nWhich && !CoopGame();
}

void Announce(const char *pzText)
{
    G_AddUserQuote(pzText);
    LOG_F(INFO, "Bot match: %s", pzText);
}

bool PlayerOut(int nPlayer)
{
    return ModeActive(kBotModeLastMan) && lives[nPlayer] <= 0;
}

// a hill: a big sector most of the level can walk to, and the sectors around it
void PickHill(void)
{
    Bmemset(inHill, 0, sizeof(inHill));
    nHillSect = -1;
    int nBest = -1;
    int64_t nBestScore = -1;
    for (int nTry = 0; nTry < 60; nTry++)
    {
        int const s = (int)((nTic * 7919u + nTry * 104729u) % (unsigned)max<int>(numsectors, 1));
        if ((sector[s].lotag & 0x7fff) == ST_2_UNDERWATER || sector[s].floorz - sector[s].ceilingz < (40 << 8))
            continue;
        if (AreaSize(s) < numsectors / 3)
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
    nHillSect = nBest;
    inHill[nBest] = 1;
    for (int w = sector[nBest].wallptr, e = w + sector[nBest].wallnum; w < e; w++)
        if (wall[w].nextsector >= 0 && Passable(w))
            inHill[wall[w].nextsector] = 1;
    LOG_F(INFO, "Bot match: the hill is sector %d", nBest);
}

void ResetMode(void)
{
    for (int i = 0; i < MAXPLAYERS; i++)
        gunLevel[i] = 0, lives[i] = kLives, hillPoints[i] = 0;
    if (ModeActive(kBotModeHill))
        PickHill();
}

void ModeTick(void)
{
    if (CoopGame())
        return;
    if (nMode == kBotModeGunGame)
    {
        // everybody has the weapon of their step (and the boot), nothing else
        for (int TRAVERSE_CONNECT(p))
        {
            auto *ps = PlayerOf(p);
            if (!PlayerAlive(p))
                continue;
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
    }
    else if (nMode == kBotModeHill && nHillSect >= 0 && ++nHillTic >= 30)
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
            else if (nHolder == 0 && nPoints % 10 == 0)
            {
                char buffer[64];
                Bsnprintf(buffer, sizeof(buffer), "You hold the hill: %d of %d", nPoints, kHillWin);
                G_AddUserQuote(buffer);
            }
        }
        // now and then: who leads
        if (nTic % (30 * 15) < 30)
        {
            int nLead = 0;
            for (int TRAVERSE_CONNECT(p))
                if (hillPoints[p] > hillPoints[nLead])
                    nLead = p;
            char buffer[96];
            Bsnprintf(buffer, sizeof(buffer), "Hill: %s leads with %d of %d", g_player[nLead].user_name, hillPoints[nLead], kHillWin);
            G_AddUserQuote(buffer);
        }
    }
    else if (nMode == kBotModeLastMan)
    {
        // who is out stays down (no USE to come back)
        int nIn = 0, nLast = -1;
        for (int TRAVERSE_CONNECT(p))
        {
            if (lives[p] > 0)
                nIn++, nLast = p;
            else
                inputfifo[0][p].bits &= ~(1u << SK_OPEN);
        }
        if (nIn <= 1)
        {
            char buffer[96];
            Bsnprintf(buffer, sizeof(buffer), "%s is the last man standing!", nLast >= 0 ? g_player[nLast].user_name : "Nobody");
            Announce(buffer);
            ResetMode();  // everybody gets their lives back and comes back
        }
    }
}

// ---- goals ----------------------------------------------------------------------------------------

void SetGoal(BOT *pBot, int x, int y, int nSect, int nKind, int nItem = -1)
{
    pBot->nGoalX = x, pBot->nGoalY = y, pBot->nGoalSect = nSect;
    pBot->nGoalKind = nKind, pBot->nGoalItem = nItem;
    pBot->nGoalTime = nTic;
    pBot->nPath = pBot->nPathPos = 0;
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
            if (p == nBot || !PlayerAlive(p) || (TeamGame() && PlayerTeam(p) == PlayerTeam(nBot)))
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

} // namespace

// ---- the match ------------------------------------------------------------------------------------

bool botMatchActive(void)
{
    return bActive;
}

bool botIsBot(int nPlayer)
{
    return bActive && nPlayer > 0 && nPlayer < ud.multimode;
}

const char *botModeName(int nMode)
{
    return kModeNames[clamp(nMode, 0, kBotModeMax - 1)];
}

void botOnDeath(int nPlayer, int nKiller)
{
    if (!bActive || CoopGame())
        return;
    if (nMode == kBotModeLastMan && lives[nPlayer] > 0)
    {
        lives[nPlayer]--;
        char buffer[96];
        if (lives[nPlayer] == 0)
            Bsnprintf(buffer, sizeof(buffer), "%s is out", g_player[nPlayer].user_name);
        else
            Bsnprintf(buffer, sizeof(buffer), "%s has %d %s left", g_player[nPlayer].user_name, lives[nPlayer], lives[nPlayer] == 1 ? "life" : "lives");
        G_AddUserQuote(buffer);
    }
    if (nMode == kBotModeGunGame && nKiller >= 0 && nKiller != nPlayer)
    {
        if (gunLevel[nKiller] >= kGunGameSteps - 1)
        {
            char buffer[96];
            Bsnprintf(buffer, sizeof(buffer), "%s wins the Gun Game!", g_player[nKiller].user_name);
            Announce(buffer);
            ResetMode();
        }
        else
        {
            gunLevel[nKiller]++;
            if (nKiller == 0)
            {
                char buffer[64];
                Bsnprintf(buffer, sizeof(buffer), "Gun Game: weapon %d of %d", gunLevel[0] + 1, kGunGameSteps);
                G_AddUserQuote(buffer);
            }
        }
    }
}

const char *botSkillName(int nSkill)
{
    return kSkills[clamp(nSkill, 0, kBotSkillMax - 1)].pzName;
}

void botLevelStart(void)
{
    if (!bActive)
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

void botStartMatch(void)
{
    if (g_netServer || g_netClient)
        return;
    int const nBots = clamp(gBotCount, 1, kBotMaxBots);
    bActive = true;
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
    if (!(g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP))
    {
        ud.m_respawn_items = ud.m_respawn_inventory = 1;
        ud.m_marker = 1;
    }
    nMode = clamp(gBotMode, 0, kBotModeMax - 1);
    LOG_F(INFO, "Bot match: %d bots (%s), %s, %s, E%dL%d", nBots, botSkillName(gBotSkill), botModeName(nMode), g_gametypeNames[ud.m_coop],
          ud.m_volume_number + 1, ud.m_level_number + 1);
    Menu_Change(MENU_CLOSE);
    G_NewGame_EnterLevel();
}

void botEndMatch(void)
{
    if (!bActive)
        return;
    bActive = false;
    ud.multimode = 1;
    g_fakeMultiMode = 0;
    g_mostConcurrentPlayers = 1;
    connectpoint2[0] = -1;
}

void botThinkAll(void)
{
    if (!bActive || numsectors <= 0)
        return;
    nTic++;
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
    for (int i = 1; i < ud.multimode; i++)
        BotThink(i);
    ModeTick();

    // DUKE3D_BOT_DEBUG=1: what every bot does, every 5 seconds
    static int const bDebug = getenv("DUKE3D_BOT_DEBUG") ? 1 : 0;
    if (bDebug && nTic % 150 == 0)
        for (int i = 1; i < ud.multimode; i++)
        {
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
