# Contributing

Bug reports, test results from other PCs and code are all welcome. Issues and pull requests can be written in
English or Portuguese.

## The rule that never changes: nothing from Nintendo

Never post any of these in the repository, issues or pull requests:

- copies of the game in any format (`.xci`, `.nsp`, `.nca`...), keys (`prod.keys`, `title.keys`)
- extracted files (`extracted/`), generated code (`generated/`), the Switch's system fonts (`.bfttf`)
- any game asset: textures, models, music, text, romfs files, not even "just one file to test"
- saves (`save/`) and mods containing game files (`mods/`)

Screenshots and short videos of the game in bug reports are welcome.

## Reporting problems

Open an issue with:

- your PC: CPU, GPU, driver version, and the window size or resolution you chose
- what you did, what happened, and what you expected
- a screenshot or a short video, if it's visual
- if the game closed or froze: the `lasrecomp.log` file next to `LetsGoRecomp.exe`
- if you were using a translation or mod, which one

## Code

- Build and test as in [docs/COMPILAR-WINDOWS.md](docs/COMPILAR-WINDOWS.md).
- Every fix gets a line in [docs/HISTORICO.md](docs/HISTORICO.md) (symptom, diagnosis, cause, fix), and
  [docs/PROGRESSO.md](docs/PROGRESSO.md) tracks the current state.
- New debugging variables and tools go in [docs/DEPURACAO.md](docs/DEPURACAO.md).
- The runtime came from LAS Recomp, made for Zelda. Before blaming the game, check whether one of the runtime's
  assumptions holds for Let's Go (formats, controller styles, return codes, reported resolution).
- To change the game's behavior without rebuilding the game code library, use the function hooks
  (`runtime/fnhook.c`).

## Commit messages

Commits are written in **English** and follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <summary in the imperative, lowercase, no period>

<optional body: what changed and why, wrapped at ~100 columns>
```

- **type:** `feat` (a new feature or option), `fix` (a bug fix), `docs` (documentation only), `perf`, `refactor`,
  `test`, `build` (Makefile, release packaging), `chore` (versions, housekeeping); research tools that only help
  debugging are `chore(debug)` or `feat(debug)` when they add a new tool.
- **scope** (optional): the area, e.g. `fps60`, `renderer`, `input`, `launcher`, `wild-battle`, `release`.
- Examples: `feat(fps60): run the overworld at 60 fps natively`, `fix(input): add a dead zone to controller
  sticks`, `docs: record the aura flicker as an open issue`.
- No `Co-Authored-By` or tool attribution lines.
- Documentation inside `docs/` stays in Portuguese; commit messages, code comments and the README are in English.
