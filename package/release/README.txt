DUKE3D CHOSEN
=============

Version 1.0. Duke Nukem 3D (the EDuke32 port) with online lobbies without port forwarding,
FIND LAN GAMES, a live dashboard for your own server and touch controls. Everybody in a network
game needs the same version - PC, Mac, iPad, Android and browser versions play together.

https://github.com/AdrianPol-projects/Duke3D-Chosen


GAME FILES

  Copy DUKE3D.GRP (and DUKE.RTS if you have it) into this folder. Any Duke Nukem 3D works:
  Atomic Edition, 20th Anniversary World Tour or Megaton Edition (GOG, Steam), the CD, or the
  free shareware version. The game files are not included.

  Add-ons, user maps and mods: put .grp, .zip or .map files in this folder or in autoload/.
  CD music: save the tracks as track02.ogg, track03.ogg, ... in this folder.


STARTING

  Windows: dukechosen.exe       Linux: ./dukechosen (nothing else to install)
  macOS: Duke3D.app (the first time: right-click > Open)
  The game also finds DUKE3D.GRP in Steam and GOG installs by itself.
  Settings (dukechosen.cfg), saves and dukechosen.log: next to the game on Windows,
  in ~/.config/dukechosen on Linux.


MULTIPLAYER (main menu)

  HOST LAN GAME     pick game type, episode, level, monsters, then ACCEPT. You play right away;
                    others can join at any time.
  FIND LAN GAMES    the games on your network; pick one to join.
  JOIN BY ADDRESS   type the host's address (port 23513).
  ONLINE            SERVER and KEY of your relay server, LOBBY NAME, then HOST A LOBBY or
                    FIND LOBBIES. No port forwarding needed. Running a server: SERVER.md on GitHub.
                    LAN ON DASHBOARD shows the LAN games you host on the server's dashboard too.
  PLAYER SETUP      name, color, team.
  LEAVE GAME        ends your network game (QUIT TO TITLE does too).

  On a LAN the host's computer needs UDP port 23513 open (Windows asks the first time) and
  UDP 23516 for FIND LAN GAMES.


CONSOLE (~ key)

  net_server, net_key, net_lobby, net_reportlan   the ONLINE settings
  connect <address>, disconnect                     EDuke32's own commands
