# Duke3D Chosen 1.1

**Duke3D Chosen** is Duke Nukem 3D (the EDuke32 port) with online lobbies without port forwarding, a LAN game finder, a live game dashboard and touch controls. It runs on **Windows, Linux, macOS, Android, iPad/iPhone and in web browsers**, and all of them play together.

You need your own `DUKE3D.GRP`: Atomic Edition, 20th Anniversary World Tour or Megaton Edition (GOG, Steam), the CD, or the free shareware version. The game files are not included.

## Downloads

| File | For |
| --- | --- |
| `Duke3D-Chosen-1.1-win64.zip` | Windows 10/11 (64-bit) |
| `Duke3D-Chosen-1.1-linux-x64.zip` | Linux on PCs (x86_64), any distribution from about 2022 on, nothing to install |
| `Duke3D-Chosen-1.1-linux-arm64.zip` | Linux on ARM (Raspberry Pi 4/5 with a 64-bit system, ARM laptops and handhelds) |
| `Duke3D-Chosen-1.1-macos.zip` | macOS 11 or newer, Apple Silicon and Intel |
| `Duke3D-Chosen-1.1-android.apk` | Android 5 or newer (phones, tablets, Chromebooks) |
| `Duke3D-Chosen-1.1-ios.ipa` | iPad/iPhone with iOS 15 or newer (unsigned: sideload it with Signulous, Sideloadly or AltStore) |
| `Duke3D-Chosen-1.1-web.zip` | the web version; every Duke3D Chosen server installs it by itself and serves it at `/play/` |

Everybody in a network game needs **1.1**.


## New in 1.1
- **Public server duke3d.pi-mole.com**: leave SERVER empty in ONLINE and host or join right away; https://duke3d.pi-mole.com shows the games live and has the web version.
- **Private lobbies**: fill in the lobby password before HOST A LOBBY; players type the same one to join (upper and lower case don't matter). The lobby list shows LOCKED.
- **Sound on iPad/iPhone** in the web version, also in silent mode.
- **Network games on computers without IPv6** work now (the game opened no network connection there).
- **NAM and WWII GI**: with more than one game (Duke, its add-ons, NAM, WWII GI) in the folder, Linux and Android ask which one to play.
- **Chat** lines of a network game show up on the server's dashboard.
- Command line for shortcuts: `-online-host`, `-online-join "lobby"`, `-online-server`, `-online-password`.

## Setup

**Windows / Linux:** unzip into a folder of its own, copy `DUKE3D.GRP` (and `DUKE.RTS` if you have it) next to the game, start `dukechosen.exe` (Windows) or `./dukechosen` (Linux).

**macOS:** unzip, copy `DUKE3D.GRP` into the `Duke3D` folder next to `Duke3D.app`, double-click `Duke3D.app` (the first time: right-click > Open).

**Android:** install the APK, start it once and allow it "access to all files". Copy `DUKE3D.GRP` into the `Duke3D` folder of the main storage (next to `Download`) and start the game again.

**iPad / iPhone:** sign and install the IPA (Signulous, Sideloadly or AltStore), start it once, copy `DUKE3D.GRP` to Files > On My iPad > Duke3D Chosen.

**Web:** open your server's address followed by `/play/` and pick your Duke Nukem 3D files once. Add it to the home screen to use it like an app, offline too.

## Multiplayer

**MULTIPLAYER** in the main menu:
- **HOST LAN GAME**: pick game type, episode, level and monsters, then ACCEPT. You play right away; others join while you play.
- **FIND LAN GAMES**: games on your network; pick one to join. **JOIN BY ADDRESS**: type the host's address (port 23513).
- **ONLINE**: your server's address and key and a lobby name, then **HOST A LOBBY** or **FIND LOBBIES**. Running your own server: [SERVER.md](https://github.com/AdrianPol-projects/Duke3D-Chosen/blob/main/SERVER.md) (Docker on a Raspberry Pi, a home PC or a VPS). Leave SERVER empty to use the public server, **duke3d.pi-mole.com**.
- **PLAYER SETUP**: your name, color and team.
- **LEAVE GAME** (or QUIT TO TITLE) ends your network game.

The host's computer runs the game; up to 8 players. A server's live dashboard shows the players, their health, weapons and keys, and a map of the level.

## Touch controls

On Android, iPad and in the browser on phones and tablets: a stick on the left, look with your finger on the right, FIRE, ALT, JUMP, DUCK, USE, a weapon picker (WPNS), inventory and the map. In menus, TOUCH sets the sensitivity, size and see-through of the buttons and gyro aiming, and has a layout editor. The buttons hide while a controller is connected.

## Known issues
- Network games use EDuke32's newer network code: the host keeps the game and players can join later. Saving and loading isn't possible in network games.
- Tested on Linux and in browsers (LAN and online games, touch controls). The Windows, macOS, Android and iPad builds come from the same code but weren't played on those devices before this release; please report what you find.
