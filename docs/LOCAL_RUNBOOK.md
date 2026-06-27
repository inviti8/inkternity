# Local dev runbook (Windows desktop)

A practical cheat-sheet for the **day-to-day desktop dev loop** on a Windows
machine: build, run, verify, and cut a release. For the full **from-scratch**
build (submodules, Conan profiles, the configure step, Linux/macOS/Emscripten),
see **[BUILDING.md](BUILDING.md)**; for the release pipeline design see
**[design/BUILDS.md](design/BUILDS.md)**.

> ⚠️ **This file is public.** Never paste secrets into it: no `.env` contents, no
> TURN secret, no signing certs / notarization credentials, no API tokens, and no
> absolute paths that contain your Windows username. Use placeholders
> (`<repo>`, `%AppData%`) for anything machine- or user-specific.

---

## 0. Environment assumptions

- **OS / compiler:** Windows 11, **Visual Studio 2022** MSVC (`cl.exe`), 64-bit
  host. CMake generator is "Visual Studio 17 2022".
- **Python:** always via **`uv`** — every Python invocation, including Conan and
  the helper scripts, goes through `uv run` (never raw `pip`/`python`).
- **Dependencies:** [Conan](https://conan.io). The Conan home is on the **D:**
  drive (`CONAN_HOME = D:\.conan2`) to keep it off the system drive.
- **Repo lives on D:** (large build tree). Adjust the `D:\...` paths below if
  yours differs.
- **Build tree:** configured under `build\` (the VS solution + `CMakeCache.txt`);
  Release artifacts land in `build\Release\` (`inkternity.exe` + `data\`).
- **`build\Release\data` is a directory junction** to `assets\data` (see §4) —
  so asset edits are live immediately, no copy/rebuild needed.

One-time env-var setup (Android SDK/NDK/JDK, verifies `CONAN_HOME`) is automated:

```
uv run --no-project python scripts/setup_dev_env.py            # dry-run, shows changes
uv run --no-project python scripts/setup_dev_env.py --apply    # persist via setx
```

---

## 1. First-time configure (once per clone / after a clean)

Follow **BUILDING.md → Windows** for the canonical sequence. In short:

```
git submodule update --init --recursive
.\conan\export_libs.bat
uv run conan install . --build=missing -pr=conan/profiles/win-x86_64
cd build
.\generators\conanbuild.bat
cmake .. -T host=x64 -DCMAKE_TOOLCHAIN_FILE="generators\conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

- `-T host=x64` is **required** — without the 64-bit host compiler, heavy TUs
  (e.g. `src/Toolbar.cpp`) fail with C1060 "compiler is out of heap space".
- Create the asset junction once (so the exe can find `data\`):
  ```
  mklink /J build\Release\data assets\data
  ```

---

## 2. Finding `cmake` (it is NOT on PATH)

CMake is fetched by Conan and lives inside the Conan cache, **not on `PATH`**
unless you've sourced `build\generators\conanbuild.bat` in the current shell. Two
ways to get its path without sourcing anything:

**PowerShell — read it from the configured cache:**
```powershell
$cmake = (Select-String -Path build\CMakeCache.txt -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$').Matches.Groups[1].Value
& $cmake --version
```

**cmd:**
```
for /f "tokens=2 delims==" %p in ('findstr CMAKE_COMMAND:INTERNAL= build\CMakeCache.txt') do set CMAKE=%p
"%CMAKE%" --version
```

The path looks like `D:\.conan2\p\cmake<hash>\p\bin\cmake.exe`. The `<hash>`
changes whenever the pinned CMake version changes — **always re-derive it** from
`CMakeCache.txt` (or `build\Release\generators\CMakePresets.json` →
`cmakeExecutable`) rather than hard-coding it.

> **Caveat — don't use `--preset conan-release` here.** `CMakeUserPresets.json`
> includes several per-arch `generators/` preset files that each define a
> `conan-release` preset, so `cmake --build --preset conan-release` fails with
> `Duplicate preset: "conan-release"`. Build the **binary dir directly** instead
> (below).

---

## 3. The daily build loop

Let `$cmake` be the path from §2.

**Incremental build of the app only** (fast — this is the usual one):
```powershell
& $cmake --build build --config Release --target main
```
Output: `build\Release\inkternity.exe`. A successful link prints
`main.vcxproj -> ...\build\Release\inkternity.exe`.

**Build everything** (drop `--target main`):
```powershell
& $cmake --build build --config Release
```

**Parallelism:** append MSBuild flags after `--`, e.g. `-- -m:4`. In PowerShell,
wrap them so they aren't arg-split: `& $cmake --build build --config Release --% -- -m:4`.

**Clean rebuild** (when CMake lists or generated files get out of sync):
```powershell
& $cmake --build build --config Release --target clean
& $cmake --build build --config Release --target main
```
A *full* clean (deleting `build\`) means re-running §1.

---

## 4. Running + verifying

The exe loads assets relative to its working directory (`data\...`), so **run it
from `build\Release`**:
```powershell
cd build\Release
.\inkternity.exe
```

- `build\Release\data` is a **junction** to `assets\data`, so any edit to a
  model/icon/shader under `assets\data\` is picked up on the next launch with **no
  rebuild** — just relaunch. (Code changes still need a §3 build.)
- The in-app log prints to the console you launched from. Useful signals when
  verifying the 3D armature, for example:
  ```
  GL Version: 3.3.0 ...
  ArmatureModel: loaded 6 primitives, 89 joints, 51 pickable.
  ```
- Per-user app data (settings, palettes, brush + armature presets, saves) lives
  under `%AppData%\HEAVYMETA\Inkternity\`. Deleting it gives a clean-slate run.

---

## 5. Where things live (quick map)

| Area | Path |
|---|---|
| App source | `src/` |
| 3D armature subsystem | `src/Armature/` |
| Canvas objects | `src/CanvasComponents/` |
| Bundled assets (models/icons/fonts) | `assets/data/` |
| Save-format version constants | `src/VersionConstants.{hpp,cpp}` |
| Vendored single-header libs | `deps/` (+ each one's `VENDORING.md`) |
| Third-party license mirrors | `assets/data/third_party_licenses/` |
| Helper scripts | `scripts/` (run via `uv run`) |
| CI release pipeline | `.github/workflows/build-installers.yml` |

**Save-format rule:** if you change what gets serialized into a `.inkternity`
file (a canvas component's `save_file`/`load_file`, etc.), bump
`src/VersionConstants.{hpp,cpp}` (header string + number) and gate the new fields
on the version in `load_file`. See the existing append-and-gate entries for the
pattern.

---

## 6. Cutting a release

Releases are built by CI, not locally. The trigger is a **`v*` semver tag push**;
GitHub Actions then builds the Linux/Windows/macOS installers and assembles a
**draft** GitHub Release. Helper:

```powershell
# Suggest the next tag (prints commands, does nothing):
uv run --no-project python scripts/create_release.py

# Create + push a specific tag -> triggers the CI build:
uv run --no-project python scripts/create_release.py --tag v0.14.0-rc1 --push
```

- The **tag is the version source of truth** for installer metadata (not the
  save-format number in `VersionConstants`). Follow the existing `v0.MINOR.0-rcN`
  convention.
- `create_release.py --push` requires a **clean working tree** (untracked test
  files will block it). If you intentionally have untracked scratch files, tag
  manually instead: `git tag <v>; git push origin <v>`.
- **Signing + macOS notarization are handled entirely by CI** using credentials
  stored as repository secrets — **no local secrets or certs are needed** to cut a
  release, and none should ever be committed or written into this repo.
- Watch the run from the repo's **Actions** tab (or `gh run list`). Builds take
  roughly half an hour; the result is a *draft* you review before publishing.
- Known transient: the macOS DMG packaging step (`hdiutil`) occasionally fails
  with "Resource busy" — that's flaky CI, not a code issue; just re-run that job.

To produce a local installer for testing (no signing):
```
cpack -G NSIS
```

---

## 7. Gotchas cheat-sheet

| Symptom | Cause / fix |
|---|---|
| `cmake: command not found` | Not on PATH — derive its path from `CMakeCache.txt` (§2) or source `conanbuild.bat`. |
| `Duplicate preset: "conan-release"` | Multi-arch preset includes collide — build the binary dir directly, don't use `--preset` (§2). |
| C1060 "compiler is out of heap space" | Configure without 64-bit host — reconfigure with `-T host=x64` (§1). |
| Asset edit not showing | Run from `build\Release` (cwd matters); confirm the `data` junction exists (§1/§4). |
| App can't find `data/...` | Same — wrong working dir or missing junction. |
| Editing a Conan recipe (e.g. Skia) | Invalidates that package's cache; a Skia-from-source rebuild can take **hours**. If you must, rebuild every target you use in the same session to avoid repeating it. |
| Changed canvas serialization | Bump `VersionConstants` + version-gate the new fields (§5). |

---

*Keep this runbook current as the local setup evolves — but remember it's public,
so describe **how** to find machine-specific values rather than pasting them.*
