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

// Online lobbies on a relay server (server/relay.py), FIND LAN GAMES and live stats for the
// server's dashboard. EDuke32's network game is a server (the host) with clients that can join
// while it runs; online the ENet datagrams travel through the relay (netrelay.cpp).

#include <string>

#include "duke3d.h"
#include "chosen.h"
#include "dukebots.h"
#include "dukemods.h"
#include "menus.h"
#include "netrelay.h"
#include "network.h"
#include "timer.h"

char gOnlineServer[64];
char gOnlineKey[48];
char gOnlineLobby[25];
char gOnlineLobbyPassword[25];
int32_t gOnlineReportLan;
int32_t gOnlineJoinPolicy;

static const char *const kJoinPolicies[] = { "open", "ask", "closed" };
static int nPolicySent = -1;
// players asking to join while the game runs (join_request), oldest first
struct JOINREQUEST { int nId; char szName[17]; };
static JOINREQUEST joinRequests[8];
static int nJoinRequests;
static uint32_t nNextJoinQuote;
int gOnlineMode;

static char zStatus[128];
static char zLobbyName[25];
static bool bReportSent;
static uint32_t nNextStats;
static std::string mapSentFor;
static bool bAdvertising;

const char *onlineVersion(void)
{
    return CHOSEN_NAME " " CHOSEN_VERSION;
}

const char *onlineServerAddress(void)
{
#ifdef NOTBLOOD_WEB
    // in a browser an empty SERVER means the server the page came from
    return gOnlineServer;
#else
    return gOnlineServer[0] ? gOnlineServer : CHOSEN_PUBLIC_SERVER;
#endif
}

const char *onlineStatusText(void)
{
    return zStatus;
}

void onlineSetStatus(const char *pzText)
{
    Bstrncpyz(zStatus, pzText, sizeof(zStatus));
    LOG_F(INFO, "Online: %s", pzText);
}

// fields of a tab separated message
static std::string msgField(const char *pzMsg, int nField)
{
    std::string s(pzMsg);
    size_t a = 0;
    while (nField-- > 0)
    {
        a = s.find('\t', a);
        if (a == std::string::npos)
            return "";
        a++;
    }
    size_t b = s.find_first_of("\t\n", a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

static std::string cleanField(const char *pzText)
{
    std::string s;
    for (const char *p = pzText; *p; p++)
        if ((unsigned char)*p >= 32 && *p != '\t')
            s += *p;
    return s;
}

// the lobby password itself never leaves the machine: the server compares a hash (as NotBlood Chosen does)
static std::string passwordHash(void)
{
    std::string pw = cleanField(gOnlineLobbyPassword);
    if (pw.empty())
        return "";
    for (char &ch : pw)
        ch = tolower((unsigned char)ch);
    pw = "notblood-lobby:" + pw;
    return relaySha256Hex(pw.data(), pw.size());
}

static const char *playerName(void)
{
    return szPlayerName[0] ? szPlayerName : "Player";
}

static const char *modName(void)
{
    // a user map or an add-on GRP other players need too
    if (boardfilename[0])
        return boardfilename;
    return "";
}

static void drawWaitScreen(const char *pzTitle, const char *pzText)
{
    videoClearScreen(0);
    rotatesprite_fs(160 << 16, 100 << 16, 65536, 0, BETASCREEN, 0, 0, 2 + 8 + 64 + BGSTRETCH);
    rotatesprite_fs(160 << 16, 104 << 16, 60 << 10, 0, DUKENUKEM, 0, 0, 2 + 8);
    rotatesprite_fs(160 << 16, 129 << 16, 30 << 11, 0, THREEDEE, 0, 0, 2 + 8);
    gametext_center_shade(160, pzTitle, 0);
    gametext_center_shade(172, pzText, 14);
    gametext_center_shade(188, "Escape: cancel", 14);
    videoNextPage();
}

// shows a waiting screen until bDone() is true; false on timeout, Escape or a dead connection
template<typename F> static bool onlineWait(const char *pzText, int nTimeoutMs, F bDone)
{
    uint32_t nStart = timerGetTicks();
    while (!bDone())
    {
        if (relayStatus() == kRelayClosed)
        {
            onlineSetStatus(relayError()[0] ? relayError() : "Connection closed");
            return false;
        }
        if (timerGetTicks() - nStart > (uint32_t)nTimeoutMs)
        {
            onlineSetStatus("The server didn't answer in time");
            return false;
        }
        handleevents();
        if (quitevent || KB_KeyPressed(sc_Escape))
        {
            KB_ClearKeyDown(sc_Escape);
            onlineSetStatus("Cancelled");
            return false;
        }
        drawWaitScreen("Online", pzText);
    }
    return true;
}

static bool onlineConnect(void)
{
#ifndef NOTBLOOD_WEB // in a browser no address means the server the page came from
    if (!onlineServerAddress()[0])
    {
        onlineSetStatus("Enter the server address first");
        return false;
    }
#endif
    if (relayStatus() == kRelayOnline)
        return true;
    relayConnect(onlineServerAddress(), gOnlineKey, onlineVersion());
    char buffer[128];
    Bsnprintf(buffer, sizeof(buffer), "Connecting to %s", onlineServerAddress()[0] ? onlineServerAddress() : "the server");
    if (!onlineWait(buffer, 15000, []() { return relayStatus() == kRelayOnline; }))
    {
        relayClose();
        return false;
    }
    return true;
}

// next reply that starts with one of the given commands
static bool onlineReply(const char *pzText, char *pReply, int nSize, const char *pzWant)
{
    return onlineWait(pzText, 10000, [&]() {
        while (relayReadText(pReply, nSize))
        {
            std::string cmd = msgField(pReply, 0);
            if (cmd == pzWant || cmd == "error" || cmd == "join_wait")
                return true;
        }
        return false;
    });
}

static void lobbyNameFor(char *pzName, int nSize)
{
    std::string s;
    for (const char *p = gOnlineLobby[0] ? gOnlineLobby : playerName(); *p && s.size() < 24; p++)
        if (isalnum((unsigned char)*p) || strchr(" _.-", *p))
            s += *p;
    if (!gOnlineLobby[0] && s.size() < 18)
        s += " game";
    if (s.empty())
        s = "Duke game";
    Bstrncpyz(pzName, s.c_str(), nSize);
}

static bool onlineHostLobby(void)
{
    if (!onlineConnect())
        return false;
    lobbyNameFor(zLobbyName, sizeof(zLobbyName));
    std::string cmd = std::string("host\t") + zLobbyName + "\t" + std::to_string(MAXPLAYERS) + "\t" + cleanField(playerName()) +
        "\t" + cleanField(modName()) + "\t" + onlineVersion() + "\t" + passwordHash() + "\t" +
        kJoinPolicies[clamp(gOnlineJoinPolicy, 0, 2)];
    relaySendText(cmd.c_str());
    char reply[512];
    if (!onlineReply("Creating the lobby", reply, sizeof(reply), "hosted"))
    {
        relayClose();
        return false;
    }
    if (msgField(reply, 0) == "error")
    {
        onlineSetStatus(msgField(reply, 1).c_str());
        relayClose();
        return false;
    }
    relaySetHostGamePort(g_netPort);
    gOnlineMode = kOnlineHost;
    // the level starts right away: who comes later joins a running game (JOIN DURING GAME decides)
    relaySendText("state\tplaying");
    nPolicySent = clamp(gOnlineJoinPolicy, 0, 2);
    nJoinRequests = 0;
    char buffer[128];
    Bsnprintf(buffer, sizeof(buffer), "Lobby \"%s\" is open", zLobbyName);
    onlineSetStatus(buffer);
    return true;
}

int chosenListLobbies(CHOSENLISTENTRY *pList, int nMax)
{
    if (!onlineConnect())
        return -1;
    relaySendText("list");
    char reply[8192];
    if (!onlineReply("Looking for lobbies", reply, sizeof(reply), "lobbies"))
    {
        relayClose();
        return -1;
    }
    int nCount = 0;
    const char *p = strchr(reply, '\n');
    while (p && nCount < nMax)
    {
        p++;
        const char *pEnd = strchr(p, '\n');
        std::string line(p, pEnd ? pEnd - p : strlen(p));
        // name host players max state mod version
        std::string mod = msgField(line.c_str(), 5);
        CHOSENLISTENTRY *pEntry = &pList[nCount];
        Bstrncpyz(pEntry->szName, msgField(line.c_str(), 0).c_str(), sizeof(pEntry->szName));
        // newer servers add: bots private user-map
        bool const bLocked = msgField(line.c_str(), 8) == "1";
        Bsnprintf(pEntry->szInfo, sizeof(pEntry->szInfo), "%s  %s/%s%s%s%s", msgField(line.c_str(), 1).c_str(), msgField(line.c_str(), 2).c_str(),
            msgField(line.c_str(), 3).c_str(), mod.empty() ? "" : "  MAP: ", mod.c_str(), bLocked ? "  LOCKED" : "");
        pEntry->szAddress[0] = 0;
        pEntry->nPort = 0;
        nCount++;
        p = pEnd;
    }
    char buffer[64];
    if (nCount)
        Bsnprintf(buffer, sizeof(buffer), "%d lobb%s on the server", nCount, nCount == 1 ? "y" : "ies");
    else
        Bsnprintf(buffer, sizeof(buffer), "No lobbies right now - host one!");
    onlineSetStatus(buffer);
    relayClose();
    return nCount;
}

// ---- hosting and joining ----------------------------------------------------------------------

static bool startServer(void)
{
    if (g_netServer)
        return true;
    if (g_netClient)
        chosenLeaveGame();
    ENetAddress address = { ENET_HOST_ANY, g_netPort, 0 };
    g_netServer = enet_host_create(&address, MAXPLAYERS, CHAN_MAX, 0, 0);
    if (g_netServer == NULL)
    {
        char buffer[96];
        Bsnprintf(buffer, sizeof(buffer), "Can't use port %d (another game running?)", g_netPort);
        onlineSetStatus(buffer);
        return false;
    }
    g_networkMode = NET_SERVER;
    LOG_F(INFO, "Multiplayer server started on port %d.", g_netPort);
    return true;
}

bool chosenHostGame(bool bOnline)
{
    botEndMatch();
    if (bOnline && !onlineHostLobby())
        return false;
    if (!startServer())
    {
        if (bOnline)
            chosenLeaveGame();
        return false;
    }
    if (!bOnline)
    {
        char address[32] = "";
        netLocalAddress(address, sizeof(address));
        char buffer[96];
        Bsnprintf(buffer, sizeof(buffer), "Hosting on %s", address[0] ? address : "this computer");
        onlineSetStatus(buffer);
        if (gOnlineReportLan && gOnlineServer[0])
        {
            relayConnect(gOnlineServer, gOnlineKey, onlineVersion());
            gOnlineMode = kOnlineReport;
            bReportSent = false;
        }
    }
    lanAdvertiseStart();
    bAdvertising = true;

    Menu_Change(MENU_CLOSE);
    Net_FillNewGame(&pendingnewgame, 1);
    Net_StartNewGame();
    Net_SendNewGame(1, NULL);
    botNetHostStart();  // the bots of the game options (players without a connection, played here)
    return true;
}

static bool connectTo(const char *pzAddress, const char *pzShown)
{
    char buffer[128];
    Bsnprintf(buffer, sizeof(buffer), "Connecting to %s", pzShown);
    drawWaitScreen("Joining", buffer);
    Net_Connect(pzAddress);
    if (!g_netClient || !g_netClientPeer)
    {
        Bsnprintf(buffer, sizeof(buffer), "No game found at %s", pzShown);
        onlineSetStatus(buffer);
        return false;
    }
    Bsnprintf(buffer, sizeof(buffer), "Joined %s", pzShown);
    onlineSetStatus(buffer);
    Menu_Change(MENU_CLOSE);
    return true;
}

bool chosenJoinAddress(const char *pzAddress)
{
    if (!pzAddress || !pzAddress[0])
    {
        onlineSetStatus("Enter the host's address first");
        return false;
    }
    if (g_netServer || g_netClient)
        chosenLeaveGame();
    return connectTo(pzAddress, pzAddress);
}

bool chosenJoinLobby(const char *pzLobby)
{
    if (!pzLobby || !pzLobby[0])
    {
        onlineSetStatus("Pick a lobby first");
        return false;
    }
    if (g_netServer || g_netClient)
        chosenLeaveGame();
    if (!onlineConnect())
        return false;
    std::string cmd = std::string("join\t") + cleanField(pzLobby) + "\t" + cleanField(playerName()) + "\t" +
        cleanField(modName()) + "\t" + onlineVersion() + "\t" + passwordHash();
    relaySendText(cmd.c_str());
    char reply[512];
    if (!onlineReply("Joining the lobby", reply, sizeof(reply), "joined"))
    {
        relayClose();
        return false;
    }
    if (msgField(reply, 0) == "join_wait")
    {
        // the game runs and its host decides who comes in
        if (!onlineWait("Waiting for the host to let you in", 65000, [&]() {
                while (relayReadText(reply, sizeof(reply)))
                {
                    std::string cmd = msgField(reply, 0);
                    if (cmd == "joined" || cmd == "error")
                        return true;
                }
                return false;
            }))
        {
            relayClose();
            return false;
        }
    }
    if (msgField(reply, 0) == "error")
    {
        onlineSetStatus(msgField(reply, 1).c_str());
        relayClose();
        return false;
    }
    if (!onlineWait("Joining the lobby", 5000, []() { return relayClientPort() != 0; }))
    {
        relayClose();
        return false;
    }
    Bstrncpyz(zLobbyName, msgField(reply, 1).c_str(), sizeof(zLobbyName));
    gOnlineMode = kOnlineClient;
    char address[32];
    Bsnprintf(address, sizeof(address), "127.0.0.1:%d", relayClientPort());
    char shown[64];
    Bsnprintf(shown, sizeof(shown), "lobby \"%s\"", zLobbyName);
    if (!connectTo(address, shown))
    {
        chosenLeaveGame();
        return false;
    }
    return true;
}

void chosenLeaveGame(void)
{
    botEndMatch();
    if (g_netServer || g_netClient)
    {
        g_netDisconnect = 1;
        Net_GetPackets();
    }
    g_netDisconnect = 0;
    g_networkMode = NET_CLIENT;
    numplayers = g_mostConcurrentPlayers = ud.multimode = 1;
    myconnectindex = screenpeek = 0;
    connecthead = 0;
    connectpoint2[0] = -1;
    if (bAdvertising)
        lanAdvertiseStop();
    bAdvertising = false;
    if (gOnlineMode != kOnlineNone)
        relayClose();
    gOnlineMode = kOnlineNone;
    bReportSent = false;
    mapSentFor.clear();
}

const char *chosenHostInfo(void)
{
    static char buffer[64];
    if (!g_netServer)
        return NULL;
    if (gOnlineMode == kOnlineHost)
    {
        Bsnprintf(buffer, sizeof(buffer), "Lobby: %s", zLobbyName);
        return buffer;
    }
    static char address[32];
    if (!address[0])
        netLocalAddress(address, sizeof(address));
    Bsnprintf(buffer, sizeof(buffer), "Your address: %s", address);
    return buffer;
}

// ---- FIND LAN GAMES ------------------------------------------------------------------------------

void chosenLanScanStart(void)
{
    lanScanStart();
}

int chosenLanScanPoll(CHOSENLISTENTRY *pList, int nMax)
{
    LANGAME games[kChosenListMax];
    int nCount = lanScanPoll(games, min(nMax, (int)kChosenListMax));
    for (int i = 0; i < nCount; i++)
    {
        Bstrncpyz(pList[i].szName, games[i].szHost, sizeof(pList[i].szName));
        bool bSame = !Bstrcmp(games[i].szVersion, onlineVersion());
        Bsnprintf(pList[i].szInfo, sizeof(pList[i].szInfo), "%s  %d/%d%s%s", games[i].szAddress, games[i].nPlayers, games[i].nMaxPlayers,
            bSame ? "" : "  OTHER VERSION: ", bSame ? "" : games[i].szVersion);
        Bstrncpyz(pList[i].szAddress, games[i].szAddress, sizeof(pList[i].szAddress));
        pList[i].nPort = games[i].nPort;
    }
    return nCount;
}

void chosenLanScanStop(void)
{
    lanScanStop();
}

// ---- stats for the dashboard ----------------------------------------------------------------

static void jsonString(std::string &s, const char *pzText)
{
    s += '"';
    for (const unsigned char *p = (const unsigned char *)pzText; *p; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            s += '\\';
            s += (char)*p;
        }
        else if (*p >= 32 && *p < 127)
            s += (char)*p;
    }
    s += '"';
}

static void jsonInt(std::string &s, const char *pzKey, int nValue)
{
    s += '"';
    s += pzKey;
    s += "\":";
    s += std::to_string(nValue);
    s += ',';
}

static const char *levelTitle(void)
{
    static char buffer[96];
    int const nMap = ud.volume_number * MAXLEVELS + ud.level_number;
    if (boardfilename[0] && ud.volume_number == 0 && ud.level_number == 7)
        Bsnprintf(buffer, sizeof(buffer), "%s", boardfilename);
    else if (nMap >= 0 && nMap < MAXVOLUMES * MAXLEVELS && g_mapInfo[nMap].name)
        Bsnprintf(buffer, sizeof(buffer), "E%dL%d: %s", ud.volume_number + 1, ud.level_number + 1, g_mapInfo[nMap].name);
    else
        Bsnprintf(buffer, sizeof(buffer), "E%dL%d", ud.volume_number + 1, ud.level_number + 1);
    return buffer;
}

static void buildStats(std::string &s)
{
    static const char *pzWeapons[] = { "Mighty Foot", "Pistol", "Shotgun", "Ripper Chaingun", "RPG", "Pipe Bombs", "Shrinker",
        "Devastator", "Trip Mines", "Freezethrower", "Pipe Bomb Detonator", "Expander" };
    static const struct { int nItem; const char *pzName; } items[] = {
        { GET_FIRSTAID, "Portable Medkit" }, { GET_STEROIDS, "Steroids" }, { GET_HOLODUKE, "Holoduke" }, { GET_JETPACK, "Jetpack" },
        { GET_HEATS, "Night Vision" }, { GET_SCUBA, "Scuba Gear" }, { GET_BOOTS, "Protective Boots" },
    };

    s = "{\"v\":1,\"mode\":";
    jsonString(s, (g_netServer || g_netClient) ? (GTFLAGS(GAMETYPE_COOP) ? "Co-op" : "DukeMatch") : "Single player");
    s += ",\"level\":";
    jsonString(s, levelTitle());
    s += ",\"diff\":";
    jsonString(s, ud.player_skill > 0 && ud.player_skill <= MAXSKILLS && g_skillNames[ud.player_skill-1][0] ? g_skillNames[ud.player_skill-1] : "");
    s += ',';
    auto const pMe = g_player[myconnectindex].ps;
    jsonInt(s, "time", pMe->player_par / REALGAMETICSPERSEC);
    s += "\"kills\":[" + std::to_string(pMe->actors_killed) + "," + std::to_string(pMe->max_actors_killed) + "],";
    s += "\"secrets\":[" + std::to_string(pMe->secret_rooms) + "," + std::to_string(pMe->max_secret_rooms) + "],";
    s += "\"players\":[";
    bool bFirst = true;
    for (int p = connecthead; p >= 0; p = connectpoint2[p])
    {
        auto const pPlayer = g_player[p].ps;
        if (!pPlayer || pPlayer->i < 0)
            continue;
        if (!bFirst)
            s += ',';
        bFirst = false;
        s += '{';
        jsonInt(s, "id", p);
        s += "\"name\":";
        jsonString(s, g_player[p].user_name[0] ? g_player[p].user_name : "Player");
        s += ',';
        int nHealth = sprite[pPlayer->i].extra;
        jsonInt(s, "alive", nHealth > 0);
        jsonInt(s, "health", max(nHealth, 0));
        s += "\"armor\":[" + std::to_string(pPlayer->inv_amount[GET_SHIELD]) + ",0,0],";
        int nWeapon = clamp((int)pPlayer->curr_weapon, 0, (int)ARRAY_SIZE(pzWeapons) - 1);
        s += "\"weapon\":";
        jsonString(s, pzWeapons[nWeapon]);
        s += ',';
        jsonInt(s, "wid", nWeapon + 1);  // the dashboard counts from 1
        jsonInt(s, "ammo", nWeapon ? pPlayer->ammo_amount[nWeapon] : -1);
        s += "\"weapons\":[";
        for (int i = 0, n = 0; i < (int)ARRAY_SIZE(pzWeapons); i++)
            if (pPlayer->gotweapon & (1 << i))
                s += (n++ ? "," : "") + std::to_string(i + 1);
        s += "],\"keys\":[";
        for (int i = 0, n = 0; i < 3; i++)
            if (pPlayer->got_access & (1 << i))
                s += (n++ ? "," : "") + std::to_string(i + 1);
        s += "],\"items\":[";
        for (int i = 0, n = 0; i < (int)ARRAY_SIZE(items); i++)
            if (pPlayer->inv_amount[items[i].nItem] > 0)
            {
                s += n++ ? ",[" : "[";
                jsonString(s, items[i].pzName);
                s += "," + std::to_string(pPlayer->inv_amount[items[i].nItem]) + "]";
            }
        s += "],\"powerups\":[],";
        int nDeaths = pPlayer->fraggedself;
        for (int k = connecthead; k >= 0; k = connectpoint2[k])
            if (k != p)
                nDeaths += g_player[k].frags[p];
        jsonInt(s, "frags", pPlayer->frag - pPlayer->fraggedself);
        jsonInt(s, "deaths", nDeaths);
        jsonInt(s, "x", pPlayer->pos.x);
        jsonInt(s, "y", pPlayer->pos.y);
        jsonInt(s, "z", pPlayer->pos.z);
        s += "\"ang\":" + std::to_string(fix16_to_int(pPlayer->q16ang) & 2047) + "}";
    }
    s += "],\"enemies\":[";
    int nEnemies = 0;
    for (int nSprite = headspritestat[STAT_ACTOR]; nSprite >= 0 && nEnemies < 400; nSprite = nextspritestat[nSprite])
    {
        spritetype const *pSprite = &sprite[nSprite];
        if (!A_CheckEnemySprite(pSprite) || pSprite->extra <= 0)
            continue;
        s += nEnemies++ ? ",[" : "[";
        s += std::to_string(pSprite->x) + "," + std::to_string(pSprite->y) + "," + std::to_string(pSprite->ang & 2047) + "]";
    }
    s += "]}";
}

static void buildMap(std::string &s)
{
    s = "[";
    bool bFirst = true;
    for (int nSector = 0; nSector < numsectors; nSector++)
    {
        for (int nWall = sector[nSector].wallptr, nEnd = nWall + sector[nSector].wallnum; nWall < nEnd; nWall++)
        {
            walltype const *pWall = &wall[nWall];
            int nNext = pWall->nextwall;
            int nType = 0;
            if (nNext >= 0)
            {
                if (nNext < nWall)  // each two-sided wall once
                    continue;
                int nNextSector = pWall->nextsector;
                if (nNextSector < 0 || (sector[nNextSector].floorz == sector[nSector].floorz && sector[nNextSector].ceilingz == sector[nSector].ceilingz))
                    continue;  // like the automap: no line where nothing changes
                nType = 1;
            }
            walltype const *pWall2 = &wall[pWall->point2];
            if (!bFirst)
                s += ',';
            bFirst = false;
            s += std::to_string(pWall->x) + "," + std::to_string(pWall->y) + "," + std::to_string(pWall2->x) + "," + std::to_string(pWall2->y) + "," + std::to_string(nType);
        }
    }
    s += "]";
}

void chosenFrame(void)
{
    chosenCommandLineAction();
    if (bAdvertising)
    {
        if (!g_netServer)
        {
            lanAdvertiseStop();
            bAdvertising = false;
        }
        else
        {
            lanAdvertiseUpdate(playerName(), onlineVersion(), modName(), g_netPort, numplayers, MAXPLAYERS);
            lanAdvertisePoll();
        }
    }

    if (gOnlineMode == kOnlineNone)
        return;

    // the game ended (the host quit or the server went away)
    if (!g_netServer && !g_netClient)
    {
        chosenLeaveGame();
        return;
    }

    char msg[512];
    while (relayReadText(msg, sizeof(msg)))
    {
        std::string cmd = msgField(msg, 0);
        if (cmd == "peer_join")
        {
            char buffer[64];
            Bsnprintf(buffer, sizeof(buffer), "%s is joining", msgField(msg, 2).c_str());
            if (apStrings[QUOTE_RESERVED4])
            {
                Bstrncpyz(apStrings[QUOTE_RESERVED4], buffer, MAXQUOTELEN);
                P_DoQuote(QUOTE_RESERVED4, g_player[myconnectindex].ps);
            }
        }
        else if (cmd == "join_request" && gOnlineMode == kOnlineHost)
        {
            if (nJoinRequests < (int)ARRAY_SIZE(joinRequests))
            {
                joinRequests[nJoinRequests].nId = atoi(msgField(msg, 1).c_str());
                Bstrncpyz(joinRequests[nJoinRequests].szName, msgField(msg, 2).c_str(), sizeof(joinRequests[0].szName));
                nJoinRequests++;
                nNextJoinQuote = 0;
            }
            else
                relaySendText(("join_answer\t" + msgField(msg, 1) + "\tno").c_str());
        }
        else if (cmd == "join_cancel")
        {
            int const nId = atoi(msgField(msg, 1).c_str());
            for (int i = 0; i < nJoinRequests; i++)
                if (joinRequests[i].nId == nId)
                {
                    memmove(&joinRequests[i], &joinRequests[i + 1], (nJoinRequests - i - 1) * sizeof(joinRequests[0]));
                    nJoinRequests--;
                    break;
                }
        }
        else if (cmd == "chat")
        {
            // a line from someone not in the game (the dashboard); players' own lines come through the game
            std::string who = msgField(msg, 1), text = msgField(msg, 2);
            if (who != playerName() && !text.empty())
            {
                char buffer[MAXQUOTELEN];
                Bsnprintf(buffer, sizeof(buffer), "%s%s%s", who.c_str(), who.empty() ? "" : ": ", text.c_str());
                G_AddUserQuote(buffer);
            }
        }
        else if (cmd == "hosted" && gOnlineMode == kOnlineReport)
            bReportSent = true;
        else if (cmd == "error" && gOnlineMode == kOnlineReport)
        {
            onlineSetStatus(msgField(msg, 1).c_str());
            relayClose();
            gOnlineMode = kOnlineNone;
            return;
        }
    }

    if (relayStatus() != kRelayOnline)
        return;
    if (gOnlineMode == kOnlineHost)
    {
        // JOIN DURING GAME changed in the menu while hosting
        int const nPolicy = clamp(gOnlineJoinPolicy, 0, 2);
        if (nPolicy != nPolicySent)
        {
            relaySendText((std::string("join_policy\t") + kJoinPolicies[nPolicy]).c_str());
            nPolicySent = nPolicy;
        }
        // somebody asks to join: Y lets them in, N doesn't (not while typing or in a menu)
        if (nJoinRequests)
        {
            auto *const ps = g_player[myconnectindex].ps;
            bool const bFree = !(ps->gm & (MODE_MENU | MODE_TYPE));
            int nAnswer = 0;
            if (bFree && KB_KeyPressed(sc_Y))
            {
                KB_ClearKeyDown(sc_Y);
                nAnswer = 1;
            }
            else if (bFree && KB_KeyPressed(sc_N))
            {
                KB_ClearKeyDown(sc_N);
                nAnswer = -1;
            }
            if (nAnswer)
            {
                relaySendText((std::string("join_answer\t") + std::to_string(joinRequests[0].nId) + (nAnswer > 0 ? "\tyes" : "\tno")).c_str());
                memmove(&joinRequests[0], &joinRequests[1], (nJoinRequests - 1) * sizeof(joinRequests[0]));
                nJoinRequests--;
                nNextJoinQuote = 0;
            }
            else if ((int32_t)(timerGetTicks() - nNextJoinQuote) >= 0 && apStrings[QUOTE_RESERVED4])
            {
                char buffer[MAXQUOTELEN];
                Bsnprintf(buffer, sizeof(buffer), "%s wants to join - Y: let in, N: no", joinRequests[0].szName);
                Bstrncpyz(apStrings[QUOTE_RESERVED4], buffer, MAXQUOTELEN);
                P_DoQuote(QUOTE_RESERVED4, ps);
                nNextJoinQuote = timerGetTicks() + 3000;
            }
        }
    }
    if (gOnlineMode == kOnlineReport && !bReportSent)
    {
        static uint32_t nLastTry;
        if (timerGetTicks() - nLastTry < 2000)
            return;
        nLastTry = timerGetTicks();
        lobbyNameFor(zLobbyName, sizeof(zLobbyName));
        std::string name = std::string(zLobbyName) + " - LAN";
        std::string cmd = "report\t" + name.substr(0, 24) + "\t" + std::to_string(MAXPLAYERS) + "\t" + cleanField(playerName()) +
            "\t" + cleanField(modName()) + "\t" + onlineVersion();
        relaySendText(cmd.c_str());
        bReportSent = true;  // "hosted" or "error" follows
        return;
    }
    static int nBotsSent = -1;
    if ((gOnlineMode == kOnlineHost || (gOnlineMode == kOnlineReport && bReportSent)) && g_netServer && nBotsSent != botNetCount())
    {
        nBotsSent = botNetCount();  // the lobby list counts them apart from the people
        relaySendText(("bots\t" + std::to_string(nBotsSent)).c_str());
    }
    if (gOnlineMode == kOnlineClient || !(g_player[myconnectindex].ps->gm & MODE_GAME))
        return;

    char mapId[64];
    Bsnprintf(mapId, sizeof(mapId), "%d-%d-%d-%d-%d", ud.volume_number, ud.level_number, numsectors, numwalls, g_player[myconnectindex].ps->player_par > 0);
    if (mapSentFor != mapId)
    {
        std::string walls;
        buildMap(walls);
        std::string cmd = std::string("map\t") + mapId + "\t" + walls;
        relaySendText(cmd.c_str());
        mapSentFor = mapId;
    }

    uint32_t nNow = timerGetTicks();
    if ((int32_t)(nNow - nNextStats) < 0)
        return;
    nNextStats = nNow + 200;
    std::string stats;
    buildStats(stats);
    stats.insert(0, "stats\t");
    relaySendText(stats.c_str());
}

void chosenChatSent(const char *pzText)
{
    if (gOnlineMode == kOnlineNone || relayStatus() != kRelayOnline || !pzText)
        return;
    std::string t = cleanField(pzText);
    if (!t.empty())
        relaySendText(("chat\t" + t.substr(0, 120)).c_str());
}

// ---- command line --------------------------------------------------------------------------

static int nCommandLineAction;   // 1: host a lobby, 2: join one
static char zCommandLineLobby[25];

bool chosenCommandLineTakesValue(const char *pzOption)
{
    return Bstrcasecmp(pzOption, "online-host") != 0;  // the rest take a value (-bots N, -bot-skill N, ...)
}

bool chosenCommandLine(const char *pzOption, const char *pzValue)
{
    struct { const char *pzName; char *pBuffer; int nSize; } const opts[] = {
        { "online-server", gOnlineServer, sizeof(gOnlineServer) },
        { "online-key", gOnlineKey, sizeof(gOnlineKey) },
        { "online-password", gOnlineLobbyPassword, sizeof(gOnlineLobbyPassword) },
    };
    for (auto const &o : opts)
        if (!Bstrcasecmp(pzOption, o.pzName))
        {
            if (pzValue)
                Bstrncpyz(o.pBuffer, pzValue, o.nSize);
            return true;
        }
    if (!Bstrcasecmp(pzOption, "bots"))  // a bot match with that many bots (and the game options of the menu)
    {
        if (pzValue)
            gBotCount = clamp(atoi(pzValue), 1, kBotMaxBots);
        nCommandLineAction = 3;
        return true;
    }
    if (!Bstrcasecmp(pzOption, "host-bots"))  // bots in the network game this machine hosts (with -online-host)
    {
        if (pzValue)
            gBotNetCount = clamp(atoi(pzValue), 0, kBotMaxBots);
        return true;
    }
    if (!Bstrcasecmp(pzOption, "bot-mode"))  // 0 DukeMatch, 1 Gun Game, 2 King of the Hill, 3 Last Man Standing
    {
        if (pzValue)
            gBotMode = clamp(atoi(pzValue), 0, kBotModeMax - 1);
        return true;
    }
    if (!Bstrcasecmp(pzOption, "bot-skill"))
    {
        if (pzValue)
            gBotSkill = clamp(atoi(pzValue), 0, kBotSkillMax - 1);
        return true;
    }
    if (!Bstrcasecmp(pzOption, "bot-monsters"))  // 0: none, 1-4: their skill
    {
        if (pzValue)
        {
            int const n = clamp(atoi(pzValue), 0, 4);
            ud.m_monsters_off = n == 0;
            if (n)
                ud.m_player_skill = n;
        }
        return true;
    }
    if (!Bstrcasecmp(pzOption, "bot-level"))  // E<episode>L<level> as two numbers: 12 = E1L2
    {
        if (pzValue && strlen(pzValue) >= 2)
        {
            ud.m_volume_number = clamp(pzValue[0] - '1', 0, MAXVOLUMES - 1);
            ud.m_level_number = clamp(atoi(pzValue + 1) - 1, 0, MAXLEVELS - 1);
        }
        return true;
    }
    if (!Bstrcasecmp(pzOption, "bot-gametype"))  // the index in the game type list (0 DukeMatch, 1 co-op, ...)
    {
        if (pzValue)
            ud.m_coop = clamp(atoi(pzValue), 0, MAXGAMETYPES - 1);
        return true;
    }
    if (!Bstrcasecmp(pzOption, "online-join-policy"))  // anyone | ask | nobody (or 0-2)
    {
        if (pzValue)
            gOnlineJoinPolicy = !Bstrcasecmp(pzValue, "ask") ? 1 : !Bstrcasecmp(pzValue, "nobody") ? 2 : !Bstrcasecmp(pzValue, "anyone") ? 0 : clamp(atoi(pzValue), 0, 2);
        return true;
    }
    if (!Bstrcasecmp(pzOption, "online-host"))
    {
        nCommandLineAction = 1;
        return true;
    }
    if (!Bstrcasecmp(pzOption, "online-join"))
    {
        nCommandLineAction = 2;
        if (pzValue)
            Bstrncpyz(zCommandLineLobby, pzValue, sizeof(zCommandLineLobby));
        return true;
    }
    return false;
}

// once the main menu is up: what the command line asked for
void chosenCommandLineAction(void)
{
    // DUKE3D_START_MENU=<menu id>: opens that menu once the main menu is up (screenshots, tests)
    static bool bStartMenu;
    static uint32_t nStartMenuTime;
    if (!nStartMenuTime)
        nStartMenuTime = timerGetTicks() + 2000;
    if (!bStartMenu && (g_player[myconnectindex].ps->gm & MODE_MENU) && (int32_t)(timerGetTicks() - nStartMenuTime) >= 0)
    {
        bStartMenu = true;
        if (char const *pzMenu = getenv("DUKE3D_START_MENU"))
        {
            Menu_Change(atoi(pzMenu));
            LOG_F(INFO, "Start menu %s: now %d", pzMenu, g_currentMenu);
        }
    }
    if (g_player[myconnectindex].ps->gm & MODE_MENU)
        modStartupFinished();  // the game started fine (with its mod, if any)
    if (!nCommandLineAction || !(g_player[myconnectindex].ps->gm & MODE_MENU))
        return;
    int const nAction = nCommandLineAction;
    nCommandLineAction = 0;
    if (nAction == 3)
    {
        botStartMatch();
        return;
    }
    bool const bOk = nAction == 1 ? chosenHostGame(true) : chosenJoinLobby(zCommandLineLobby);
    LOG_F(INFO, "Online (command line): %s - %s", nAction == 1 ? "host" : "join", bOk ? "ok" : onlineStatusText());
}
