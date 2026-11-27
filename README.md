# Duke3D Chosen

Duke Nukem 3D with **online lobbies without port forwarding**, a **LAN game finder**, a **live game dashboard** and **touch controls**, for **Windows, Linux (PC and ARM), macOS, Android, iPad/iPhone and web browsers**. It is [EDuke32](https://www.eduke32.com/) (the version in [NBlood](https://github.com/nukeykt/NBlood)) with the multiplayer and platform work from [NotBlood Chosen](https://github.com/AdrianPol-projects/NotBlood-Chosen). Every player in a network game needs the same version; all platforms of one version play together.

**Downloads:** [releases](https://github.com/AdrianPol-projects/Duke3D-Chosen/releases). Web browsers (no install): every Duke3D Chosen server serves the game at `/play/` ([how it works](platform/Web/README.md)).

Unzip, put your `DUKE3D.GRP` next to the game and start it. Any Duke Nukem 3D works: Atomic Edition, 20th Anniversary World Tour or Megaton Edition (GOG, Steam), the CD, or the free shareware version. The game files are not included.

**What Duke3D Chosen adds to EDuke32**
* **MULTIPLAYER** in the main menu:
  * **PLAY WITH BOTS**: up to 7 computer players (4 difficulties) in DukeMatch, Team DukeMatch or co-op, offline.
  * **HOST LAN GAME**: pick the game type (DukeMatch, co-op, ...), episode, level and monsters, and play. Others can join at any time while you play.
  * **FIND LAN GAMES**: lists the games on your network; pick one to join. **JOIN BY ADDRESS** for anything else.
  * **ONLINE**: enter your server's address and key, then **HOST A LOBBY** or **FIND LOBBIES**. No port forwarding: the game traffic goes through a relay server: the public one, duke3d.pi-mole.com (leave SERVER empty), or your own ([SERVER.md](SERVER.md)). Lobbies can have a password.
  * **LEAVE GAME**, and QUIT TO TITLE now works in network games.
* **Live dashboard** in the browser: lobbies, players' health, armor, weapons, keys, a live level map and an event list.
* **Network game fix:** in EDuke32's current network code the clients never got the level (`Waiting for server` forever). Fixed, so LAN and online games work.
* **Touch devices** (Android, iPad, phones and tablets in the browser): on-screen stick, finger look, fire/jump/duck/use buttons, a weapon picker, gyro aiming, a layout editor; the buttons hide while a controller is connected.
* **Web version**: the whole game in Safari, Chrome, Edge or Firefox, installable as an app that works offline; your files stay in the browser.
* **macOS** app for Apple Silicon and Intel, **Android** keeps everything in the `Duke3D` folder of the main storage, **iPad** in Files > On My iPad > Duke3D Chosen.

Building: `.github/workflows/release.yml` (all platforms), `platform/Android`, `platform/iOS`, `platform/Web`; on Linux `make duke3d`. The relay server is NotBlood Chosen's `server/` with `SITE=duke3d` ([SERVER.md](SERVER.md)); the public one is duke3d.pi-mole.com.

## Acknowledgments

EDuke32 by the EDuke32 team (Richard "TerminX" Gobeille, Evan "Hendricks266" Ramos and many others), NBlood by nukeykt and contributors, NotBlood by clipmove, the Build engine by Ken Silverman, Duke Nukem 3D by 3D Realms. See [AUTHORS.md](AUTHORS.md). Duke Nukem 3D is a trademark of its owners; this project contains no game data.

GPL 2 (game code) and the Build license (engine), see `package/common`.
