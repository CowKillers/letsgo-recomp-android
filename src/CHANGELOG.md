# Changelog

## 0.3.2 (2026-10-18)

- **Fixes the launcher forgetting your settings** (0.3.1): it saved `larecomp.ini` with some lines run together, so
  the next start went back to the defaults for 60 fps, the resolution, anti-aliasing, the translation, movie audio,
  the fps counter, the d-pad and wild battles. Fixed, and a file saved by 0.3.1 is read correctly: tested with one,
  every setting came back.
- **Game folders with accents or other non-Latin letters work** (for example a Windows user folder named with an
  accent). Tested with a folder named in Cyrillic and Portuguese letters: 0.3.1 could not open the game there, 0.3.2
  runs it.
- **Less memory reserved**: the game's threads no longer commit 256 MB of stack each up front. Pewter City, Eevee:
  committed memory 7.2-7.4 GB -> 4.7 GB; the memory actually in use is unchanged. Helps on PCs with little RAM or a
  small page file.
- **Less GPU time**: fragment and compute shaders unpack half-precision values natively. RTX 3080, Pewter City at 2x
  resolution: 4.0 -> 3.75 ms of GPU per frame (the frame rate at the game's 30 fps does not change on this machine).
  Frames look the same.
- Fixes from LAS Recomp, the project this port is built on: render passes no longer hand empty color slots to the
  driver (they crash Intel UHD 630 drivers there; not tested here on Intel), a GPU fence race, duplicate pipelines,
  and shader instruction details. A rare crash when two game threads touch new memory at the same moment is fixed
  too (found in another port of the same runtime; not seen in Let's Go).
- After a crash, no extra "Visual C++ Runtime Library" box. A controller Windows does not recognise can be added with
  your own SDL mapping line in `gamecontrollerdb.txt` next to the exe.
- Tested with Eevee and Pikachu 1.0.0 on the regression routes (world, menus, area change); saving from the menu checked with Eevee.
- Upgrading: unzip over your install (no new setup).
- Tested with the zip built for this release, unzipped into a new folder named with accented letters, the game files of 0.3.x installs of both games, started without MSYS2 on the PATH (no recompiling needed): Eevee and Pikachu load a save and reach the world; the start screen rewrote a settings file saved by 0.3.1 with every setting kept.

## 0.3.1 (2026-10-18)

- **Fixes the world breaking after a capture** (found playing 0.3.0 with 60 fps on): after entering and leaving a
  capture, fences, signs and ferns could show flat or dark until you changed area. The cause was older than 0.3.0 and
  only luck kept it hidden: when the game rebuilt a texture in place (it streams them), the texture slot the game had
  registered kept pointing at the freed copy, so what it showed depended on what the memory allocator put there next.
  The render thread and the video-memory models of 0.3.0 changed the allocation pattern, which made it show. Slots
  now follow the texture when the game rebuilds it.
- Tested here with a new unattended loop that walks in the grass of Viridian Forest and runs from each encounter
  (Eevee and Pikachu 1.0.0, both 0.3.0 options on): the world looked the same before and after every encounter. The
  0.3.0 performance options stay **on by default**.
- Upgrading: unzip over your install (no new setup).

## 0.3.0 (2026-10-18)

A performance release. Everything below was measured on one machine (Core i5-14600KF, RTX 3080, Windows 11), with
runs taken in turn (A, B, A, B...) because the same frame varies 10-18 ms of GPU time between runs. **Not measured on
other graphics cards** (AMD, Intel, integrated): feedback is welcome in an issue.

- **The graphics work runs on its own thread (on by default).** The game's calls into the Vulkan backend used to run
  on the game's own thread, in the chain that also runs its logic. Unlimited frame rate on the Pewter City route:
  Eevee 126.9 -> 185.5 fps (frame time ratio 1.46, 95% interval 1.40-1.53), Pikachu 132.1 -> 191.4 fps (1.45, 1.31-1.54);
  the 99th percentile frame time drops from 13.3 to 9.9 ms. At the game's own 30 fps cap you will not see more frames:
  the gain is CPU headroom. The paced 60 fps mode is
  unchanged (16.63 ms per frame with and without). Rendered frames match the old path within the run-to-run noise;
  the Vulkan validation layers report no errors on the regression routes of both games. `render_thread = 0` in
  `larecomp.ini` (or `LARECOMP_RENDER_THREAD=0`) turns it off.
- **Models are read from video memory on dedicated GPUs.** The vertex and index data was read by the GPU in place from
  the game's memory (system memory, over PCIe) for every vertex invocation. It is copied to VRAM now: GPU time of the
  Pewter City frame 10.2 -> 1.2 ms. Integrated GPUs keep the old path (not tested). `vertex_read = direct` in `larecomp.ini` (or
  `LARECOMP_VERTEX_READ=direct`) goes back to it.
- **Mutex waits no longer stall the game threads.** A contended lock spun briefly before sleeping on a condition
  variable (~1000 contended locks per frame per thread, ~12 us each on Windows). The paced 60 fps mode now holds 60.0
  fps (it was 58.8-59.3); unlimited frame rate went from about 60 to 65-74 fps at the time.
- **`larecomp.ini` edited by hand on Windows works**: lines ending in CR+LF kept the CR in the value, so
  `resolution = dynamic`, `upscaler = bilinear` and the percentage modes fell back silently.
- **Dynamic resolution** (`resolution = dynamic`, ini only) judged a steady 30 fps as too slow and dropped to 0.60
  scale; it budgets by the scene's pace now.
- For developers: a much faster regression battery (6 routes of both games in ~100 s, was ~30 min), an interleaved
  A/B performance test with a confidence interval, per-draw GPU timing and a frame time CSV (docs/DEPURACAO.md,
  docs/PESQUISA-GPU.md).
- Upgrading from 0.2.x: unzip over your install; no new setup (the game library is not rebuilt).
- Tested with the zip built for this release, unzipped over 0.2.x installs of both games and started without MSYS2 on the PATH (no recompiling needed): Eevee loads a save and reaches the world (Route 2); Pikachu boots with the render thread on and plays the opening movie.

## 0.2.2 (2026-10-17)

From the second patch in [issue #1](https://github.com/raulzrrs/letsgo-recomp/issues/1), thanks again to
@porstobotgovna:

- **No more crashes on the communication screens.** The network services (`nifm`, `ssl`, `bsd`, `ldn`) didn't exist:
  the game got an empty session and closed when opening local wireless, Mystery Gift or internet co-op. They now
  answer like a console that is offline. Tested on both games (Eevee and Pikachu 1.0.0): **local trade** searches for
  a player and B goes back; **Mystery Gift** stays on "Connecting..." (that screen has no way out offline: the game
  waits with no timeout and ignores B, as on a console without Nintendo's servers); the **Pokémon GO settings**
  search and B goes back to the menu.
- The other Bluetooth (`btm:u`) commands answer with no data, like a console with nothing paired.
- Tested with the zip downloaded from this release, unzipped over 0.2.1 installs of both games (no recompiling
  needed): each one boots a save and reaches the local trade search.

## 0.2.1 (2026-10-17)

From [issue #1](https://github.com/raulzrrs/letsgo-recomp/issues/1), with thanks to @porstobotgovna, who played
*Let's Go, Pikachu!* 1.0.2 on an AMD RX 7800 XT with Windows 10 and sent a patch:

- **Supports *Let's Go, Pikachu!*** (and updated games): the launcher accepts either game by its program id, shows
  which one is set up, keeps Pikachu's saves in `save-pikachu\` and installs translations for the set-up game.
  Tested here with Pikachu 1.0.0: setup (13 to 17 minutes), a new game through the title movie, Professor Oak and the
  name screen, and played (from a copy of an Eevee save) with 60 fps, wild battles and the PT-BR translation. Each
  folder holds one set-up game: to have both, unzip the release twice. **60 fps and wild battles work on Pikachu 1.0.0 too:** their hooks and the generator's instruction patches
  are addresses in the game's executable, now in a table per build (the same functions in Pikachu, found by comparing
  the code instruction by instruction, `tools/addrmap.py`). Other builds (the updates) get neither (greyed out on the
  start screen). The game is told its real version (1.0.0 / 1.0.2) instead of always 1.0.0.
- **Fixes a crash at boot (`c0000005` accessing `0x38`) on some Windows 10 machines**, whose Yu Gothic Regular font
  breaks the game's font engine: Yu Gothic Medium and MS Gothic come first now. `LARECOMP_FONT=<file>` overrides it.
- **Pokémon GO Park settings:** the game waited on a Bluetooth pairing event that was never created and aborted; the
  event exists now, and a wait on an empty handle slot waits on the others.
- **Fixes wild battles with 60 fps on (0.2.0, both games):** the battle counted as a menu and got in-between frames;
  it runs at 30 fps like the game's own battles now.
- Flat dumps (the code files next to the RomFS's `bin/`) are set up too.
- The overlay (close the game?, name entry) speaks English, Portuguese or Russian, from the system language
  (`LARECOMP_LANG=en|pt|ru`).
- Crash reports list the last calls to the host, and the previous run's log is kept as `lasrecomp-prev.log`.
- Tested with the zip downloaded from this release, unzipped twice without development tools on PATH: Eevee 1.0.0
  and Pikachu 1.0.0 set up from `exefs` + `romfs.bin` / `romfs` (13 minutes each), an Eevee save in Viridian City with
  the PT-BR translation and 60 fps in the world, and a new Pikachu game with the translation installed from its `.rar`
  (Professor Oak's lines in Portuguese).

## 0.2.0 (2026-10-17)

Two experimental options on the start screen, both off by default:

- **60 fps (experimental).** The overworld runs at 60 fps natively: the game's own time step is halved there, so
  walking, the camera and the world's animations are smooth at the right speed. Menus and the Pokédex keep the game's
  30 updates per second and are shown at 60 with an in-between frame. Battles and captures stay at 30 fps, as the game
  made them (their timelines count frames; an in-between frame there was tried and left out). Tested by playing:
  world, menus and Pokédex at 60 without speed-ups; battles and captures at normal speed.
- **Wild battles (experimental).** Touching a wild Pokémon starts a classic battle against it (its species, level,
  gender, IVs and level-up moves) and then the capture, like the game does with Snorlax and the legendary birds.
  Played on Route 1. Not done yet: split experience, nature and shiny of the touched Pokémon, regional forms, Pokémon
  with an aura; other areas not tested.

Also:

- **Controller stick dead zone**, adjustable on the start screen ("Keyboard and mouse"): a stick at rest no longer
  moves the Poké Ball aim.
- Known issue, also in 0.1.2: the colored aura of special wild Pokémon sometimes disappears for a single frame
  ([docs/PROGRESSO.md](docs/PROGRESSO.md), item 6).
- Tested with the zip downloaded from this release, in a new folder without development tools on PATH: setup from a
  game folder with `exefs` and `romfs` (13 minutes to recompile on a 20-thread CPU), a new game through the controller
  screen, the title movie, Professor Oak and the character and name screens, and the Brazilian Portuguese fan
  translation installed from its downloaded `.rar` (Professor Oak's lines in Portuguese). With both options on (another
  clean folder): the world at 60 fps and a classic battle started by touching a wild Pidgey.

## 0.1.2 (2026-10-16)

- **Fixes a crash at the title screen on a new game** (no save yet) in 0.1.1: the movie player looked for the
  game's movies in an extracted `romfs` folder, which the release's setup doesn't create (it builds the `romfs.bin`
  image). Movies are now read from inside the image.
- Tested from a clean folder with the downloaded zip: setup, a new game through the controller and language screens
  into the introduction, and the Brazilian Portuguese translation installed from the `.rar`.

## 0.1.1 (2026-10-16)

- **Fixes the game closing right after the controller screen** ("svcBreak") in the 0.1.0 release: its FFmpeg was
  built without the Opus decoder the game's music and sound effects use. The release build now checks every decoder
  the game needs, and a missing one is reported by name instead of crashing.
- The one-time setup's progress bar no longer reaches 100% long before the end (it counted the four game modules as
  two).
- The README states exactly which game files and version are supported, with the sources.

## 0.1.0 (2026-10-15)

First public release.

- Native Windows port of *Pokémon: Let's Go, Eevee!* v1.0.0 by static recompilation (built on
  [LAS Recomp](https://github.com/peiikai/LASRecomp)): the game's four executable modules are translated from ARM64
  to C and compiled to x86-64; no ARM64 interpreter or JIT at run time.
- Release zip with `LetsGoRecomp.exe`: pick your extracted game folder in the launcher and the game code is
  recompiled on your PC (bundled generator and Zig compiler; nothing else to install).
- Stable 30 fps, 720p to 4K internal resolution (with supersampling), SMAA, 16x anisotropic filtering, any window
  size, fullscreen; parallel shader translation with disk caches.
- Keyboard and mouse (rebindable; the mouse aims Poké Balls and is the touch screen), any SDL 3 controller, virtual
  finger with the right stick for Play with Eevee.
- Atmosphere-style LayeredFS mods; one-click install of fan translations from the downloaded .zip / .rar / .7z.
- Launcher in English and Brazilian Portuguese.
