// The game this build of the Chosen engine is for: names, folders and the file that shows the
// game data is there (used by the Android, iPad and web platform code).

#pragma once

#define CHOSEN_GAME_NAME "Duke3D Chosen"
#define CHOSEN_GAME_FOLDER "Duke3D"            // Android: main storage/Duke3D, iPad: On My iPad > Duke3D Chosen
#define CHOSEN_GAME_LOG "dukechosen.log"
#define CHOSEN_GAME_EXE "dukechosen"
#define CHOSEN_GAME_PK3 "dukechosen.pk3"       // shipped with the app when there is one
#define CHOSEN_GAME_DATAFILE "DUKE3D.GRP"      // the game data is there when this file is
#define CHOSEN_GAME_DATAFILE_LOWER "duke3d.grp"
#define CHOSEN_GAME_DATAFILE_MIXED "Duke3D.grp"
#define CHOSEN_GAME_FILES "Duke Nukem 3D game files"
#define CHOSEN_GAME_README_NAME "PUT DUKE3D FILES HERE.txt"
#define CHOSEN_GAME_SOURCES "your Duke Nukem 3D folder (GOG/Steam Atomic Edition, 20th Anniversary World Tour, Megaton Edition, the CD, or the free shareware version)"
#define CHOSEN_GAME_README_FILES \
    "Required:\n" \
    "  DUKE3D.GRP  (Atomic Edition, 20th Anniversary World Tour, Megaton Edition, the CD,\n" \
    "              or the free shareware version)\n\n" \
    "Optional:\n" \
    "  DUKE.RTS (the voice macros), user maps (*.MAP), add-ons (*.GRP, *.ZIP)\n" \
    "  CD music as trackXX.ogg\n\n"
