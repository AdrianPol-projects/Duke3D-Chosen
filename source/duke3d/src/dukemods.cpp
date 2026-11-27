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

// The MODS menu's work (see dukemods.h), after NotBlood Chosen's mods.cpp.

#ifdef _WIN32
# include "windows_inc.h"
#elif defined __APPLE__
# include <TargetConditionals.h>
# include <mach-o/dyld.h>
# include <spawn.h>
#elif !defined __ANDROID__ && !defined __EMSCRIPTEN__
# include <spawn.h>
# include <unistd.h>
#endif

#include "duke3d.h"
#include "dukemods.h"

#if !defined _WIN32 && !defined __ANDROID__ && !(defined __APPLE__ && TARGET_OS_IPHONE) && !defined __EMSCRIPTEN__
extern char **environ;
# define MODS_CAN_RESPAWN
#endif

extern char const * const *g_chosenArgv;

namespace
{

const char kSelectFile[] = "mods.cfg";
// mods.cfg is renamed to this while the game starts: still there at the next start = the mod failed
const char kTryingFile[] = "mods.cfg.starting";
bool bFailedLastTime;
char szFailedName[40];

MODENTRY gMods[kMaxModEntries];
int gModCount;
MODENTRY gActive;
bool bActive;

bool isDir(const char *pzPath)
{
    struct Bstat st;
    return !Bstat(pzPath, &st) && (st.st_mode & S_IFMT) == S_IFDIR;
}

bool hasExtension(const char *pzName, const char *pzExt)
{
    size_t const n = strlen(pzName), e = strlen(pzExt);
    return n > e && !Bstrcasecmp(pzName + n - e, pzExt);
}

void addEntry(int nKind, const char *pzName, const char *pzPath)
{
    if (gModCount >= kMaxModEntries)
        return;
    for (int i = 0; i < gModCount; i++)
        if (!Bstrcasecmp(gMods[i].szName, pzName))
            return;  // the same mod in two places: the first one wins
    MODENTRY &m = gMods[gModCount++];
    m = {};
    m.nKind = nKind;
    Bstrncpyz(m.szName, pzName, sizeof(m.szName));
    Bstrncpyz(m.szPath, pzPath, sizeof(m.szPath));
}

void scanDir(const char *pzRoot)
{
    char base[BMAX_PATH];
    Bsnprintf(base, sizeof(base), "%s/mods", pzRoot);
    BDIR *dir = Bopendir(base);
    if (!dir)
        return;
    while (Bdirent *d = Breaddir(dir))
    {
        if (d->name[0] == '.')
            continue;
        char path[BMAX_PATH];
        Bsnprintf(path, sizeof(path), "%s/%s", base, d->name);
        if (isDir(path))
            addEntry(kModFolder, d->name, path);
        else if (hasExtension(d->name, ".zip") || hasExtension(d->name, ".grp") || hasExtension(d->name, ".pk3"))
        {
            char name[64];
            Bstrncpyz(name, d->name, sizeof(name));
            if (char *dot = strrchr(name, '.'))
                *dot = '\0';
            addEntry(kModGroup, name, path);
        }
    }
    Bclosedir(dir);
}

void absolutePath(const char *pzIn, char *pzOut, int nSize)
{
#ifdef _WIN32
    if (!_fullpath(pzOut, pzIn, nSize))
        Bstrncpyz(pzOut, pzIn, nSize);
#else
    char *p = realpath(pzIn, NULL);
    Bstrncpyz(pzOut, p ? p : pzIn, nSize);
    free(p);
#endif
}

// a mods folder with a note, where the game files are (phones and tablets have nowhere else)
void makeModsFolder(void)
{
    if (isDir("mods"))
        return;
    Bmkdir("mods", S_IRWXU);
    if (FILE *fp = fopen("mods/Put mods here.txt", "w"))
    {
        fputs("Mods go here: a folder each (maps, .con scripts, .def texture packs, voxels, music...)\n"
              "or .zip, .grp and .pk3 files. Choose them in the main menu: MODS.\n", fp);
        fclose(fp);
    }
}

} // namespace

const char *modCurrentName(void)
{
    return bActive ? gActive.szName : "";
}

void modApplySelection(bool bCommandLine)
{
    makeModsFolder();
    FILE *fp = fopen(kTryingFile, "r");
    if (fp)
    {
        char line[BMAX_PATH + 16];
        while (fgets(line, sizeof(line), fp))
            if (!strncmp(line, "name=", 5))
            {
                line[strcspn(line, "\r\n")] = '\0';
                Bstrncpyz(szFailedName, line + 5, sizeof(szFailedName));
            }
        fclose(fp);
        remove(kTryingFile);
        bFailedLastTime = true;
        LOG_F(ERROR, "Mods: \"%s\" stopped the game from starting last time, playing without it", szFailedName);
        return;
    }
    fp = fopen(kSelectFile, "r");
    if (!fp)
        return;
    MODENTRY m = {};
    char line[BMAX_PATH + 16];
    while (fgets(line, sizeof(line), fp))
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *pValue = strchr(line, '=');
        if (!pValue)
            continue;
        *pValue++ = '\0';
        if (!strcmp(line, "name")) Bstrncpyz(m.szName, pValue, sizeof(m.szName));
        else if (!strcmp(line, "path")) Bstrncpyz(m.szPath, pValue, sizeof(m.szPath));
        else if (!strcmp(line, "kind")) m.nKind = atoi(pValue);
    }
    fclose(fp);
    if (m.nKind == kModNone || !m.szName[0])
        return;
    if (bCommandLine)
    {
        LOG_F(INFO, "Mods: \"%s\" is chosen, but the command line chooses the game files", m.szName);
        return;
    }
    if (m.nKind == kModFolder)
    {
        if (!isDir(m.szPath))
        {
            LOG_F(ERROR, "Mods: the folder of \"%s\" is gone (%s)", m.szName, m.szPath);
            return;
        }
        G_AddPath(m.szPath);
    }
    else if (m.nKind == kModGroup)
        G_AddGroup(m.szPath);
    else
        return;
    gActive = m;
    bActive = true;
    rename(kSelectFile, kTryingFile);
    LOG_F(INFO, "Mods: playing \"%s\"", m.szName);
}

void modStartupFinished(void)
{
    static bool bDone;
    if (bDone)
        return;
    bDone = true;
    if (bActive)
        rename(kTryingFile, kSelectFile);
    if (bFailedLastTime)
    {
        char buffer[96];
        Bsnprintf(buffer, sizeof(buffer), "The mod \"%s\" didn't load and was switched off", szFailedName);
        G_AddUserQuote(buffer);
    }
}

int modScan(void)
{
    gModCount = 0;
    addEntry(kModNone, "No mods (the plain game)", "");
    char roots[2][BMAX_PATH];
    int nRoots = 0;
    if (Bgetcwd(roots[nRoots], BMAX_PATH))
        nRoots++;
    if (char *pzApp = Bgetappdir())
    {
        absolutePath(pzApp, roots[nRoots], BMAX_PATH);
        if (!nRoots || Bstrcmp(roots[0], roots[nRoots]))
            nRoots++;
        Xfree(pzApp);
    }
    for (int i = 0; i < nRoots; i++)
        scanDir(roots[i]);
    return gModCount;
}

MODENTRY const *modEntry(int nIndex)
{
    return (nIndex >= 0 && nIndex < gModCount) ? &gMods[nIndex] : NULL;
}

bool modIsActive(MODENTRY const *pMod)
{
    if (!bActive)
        return pMod->nKind == kModNone;
    return pMod->nKind == gActive.nKind && !Bstrcasecmp(pMod->szName, gActive.szName);
}

bool modSelect(int nIndex)
{
    MODENTRY const *pMod = modEntry(nIndex);
    if (!pMod)
        return false;
    FILE *fp = fopen(kSelectFile, "w");
    if (!fp)
    {
        LOG_F(ERROR, "Mods: could not write %s", kSelectFile);
        return false;
    }
    char path[BMAX_PATH] = "";
    if (pMod->szPath[0])
        absolutePath(pMod->szPath, path, sizeof(path));
    fprintf(fp, "; the mod Duke3D Chosen loads at startup (chosen in the MODS menu)\n");
    fprintf(fp, "name=%s\nkind=%d\npath=%s\n", pMod->szName, pMod->nKind, path);
    fclose(fp);
    LOG_F(INFO, "Mods: \"%s\" chosen, restarting", pMod->szName);
    return true;
}

void modRestart(const char *pzName)
{
#ifdef MODS_CAN_RESPAWN
    char exe[BMAX_PATH] = "";
# ifdef __APPLE__
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size))
        exe[0] = '\0';
# else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = '\0';
# endif
    if (exe[0] && g_chosenArgv)
    {
        pid_t pid;
        if (!posix_spawn(&pid, exe, NULL, NULL, (char *const *)g_chosenArgv, environ))
            G_GameExit(" ");
        LOG_F(ERROR, "Mods: could not start %s again", exe);
    }
#elif defined _WIN32
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(NULL, exe, MAX_PATH))
    {
        // the old window is still open for a moment, so skip the "already running" question
        wchar_t cmd[4096];
        _snwprintf(cmd, ARRAY_SIZE(cmd), L"%ls -noinstancechecking", GetCommandLineW());
        cmd[ARRAY_SIZE(cmd)-1] = 0;
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            G_GameExit(" ");
        }
        LOG_F(ERROR, "Mods: could not start the game again");
    }
#endif
    // phones, tablets, browsers: apps can't start themselves again
    char text[96];
    Bsnprintf(text, sizeof(text), "Start Duke3D Chosen again to play %s", pzName);
    G_AddUserQuote(text);
    G_GameExit(text);
}
