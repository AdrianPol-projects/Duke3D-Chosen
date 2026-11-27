//-------------------------------------------------------------------------
/*
This file is part of Duke3D Chosen (EDuke32 with the NotBlood Chosen features).
GPL 2, see package/common/gpl-2.0.txt.
*/
//-------------------------------------------------------------------------

// MODS menu (as NotBlood Chosen's): mods loaded when the game starts.
//
//   mods/<folder>/       a mod as loose files (maps, .con scripts, .def texture packs, voxels, music...)
//   mods/<file>.zip      a mod packed in a .zip, .grp or .pk3
//
// The choice is stored in mods.cfg and applied at the next start, so choosing restarts the game.
// A mod that stops the game from starting is switched off at the next start.

#pragma once

#include "compat.h"

enum
{
    kModNone = 0,  // the plain game
    kModFolder,
    kModGroup,
};

#define kMaxModEntries 12

struct MODENTRY
{
    int nKind;
    char szName[40];
    char szPath[BMAX_PATH];
};

// the loaded mod ("" for none); network games need the same on every machine
const char *modCurrentName(void);
// startup, before the game files are loaded: the chosen mod (unless the command line chose files)
void modApplySelection(bool bCommandLine);
// the game started fine (the main menu is up)
void modStartupFinished(void);

// the menu: find the mods, list them, choose one (then restart)
int modScan(void);
MODENTRY const *modEntry(int nIndex);
bool modIsActive(MODENTRY const *pMod);
bool modSelect(int nIndex);
void modRestart(const char *pzName);
