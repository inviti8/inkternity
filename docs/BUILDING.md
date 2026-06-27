## Building
This program is written in C++, and uses [conan](https://conan.io) to fetch dependencies.

> For the day-to-day **local Windows dev loop** (incremental build/run/verify
> commands, gotchas, and cutting a release), see **[LOCAL_RUNBOOK.md](LOCAL_RUNBOOK.md)**.

## Linux
After cloning the repository, `cd` into the repo, then run:
```
./conan/export_libs.sh
conan install . --build=missing -pr=conan/profiles/linux-x86_64
cd build/Release
source generators/conanbuild.sh
cmake ../.. -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build .
source generators/deactivate_conanbuild.sh
```
After building the project, you should place the `data` folder located in the root of the repository next to the `inkternity` executable before running it. What I usually do is create a symbolic link of the data folder and place it in the build directory.

You can also build in Debug mode by setting the `build_type` in the `conan install` command to `Debug`, and also by setting `CMAKE_BUILD_TYPE=Debug` when running CMake.
## macOS
After cloning the repository, `cd` into the repo, then run:
```
./conan/export_libs.sh
conan install . --build=missing -pr=conan/profiles/macOS-arm64
cd build/Release
source generators/conanbuild.sh
cmake ../.. -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build .
source generators/deactivate_conanbuild.sh
```
This will build an app bundle that contains all the data necessary to run the program. To create a .dmg installer for the app, run:
```
cpack -G DragNDrop
```
## Windows
The windows version of this program is built on Visual Studio 2022's compiler.

After cloning the repository, `cd` into the repo, then update the git submodules to get the `clip` library:
```
git submodule update --init --recursive
```
Then run:
```
.\conan\export_libs.bat
conan install . --build=missing -pr=conan/profiles/win-x86_64
cd build
.\generators\conanbuild.bat
cmake .. -T host=x64 -DCMAKE_TOOLCHAIN_FILE="generators\conan_toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
.\generators\deactivate_conanbuild.bat
```

`-T host=x64` forces the 64-bit MSVC host compiler. Without it some
translation units (notably `src/Toolbar.cpp`) hit C1060 "compiler is out
of heap space" on the default 32-bit `cl.exe`.

### Incremental rebuilds on Windows
`cmake` itself is fetched by Conan and **is not on `PATH`** unless you've
sourced the Conan build env (`generators\conanbuild.bat`) for the
current shell. If a tool/agent reports `cmake: command not found` while
in this repo, that's why. Two ways to invoke it:

- Source the env first: `cd build && .\generators\conanbuild.bat`, then
  `cmake --build . --config Release`.
- Or call cmake by full path, which the cache records:
  ```
  for /f "tokens=2 delims==" %p in ('findstr CMAKE_COMMAND:INTERNAL= build\CMakeCache.txt') do "%p" --build build --config Release --target main -- -m:2
  ```
  In PowerShell, watch for arg-splitting on `-m:2` — wrap the MSBuild
  flags in single quotes via `cmd /c '... -- -m:2'` if PowerShell splits
  them.

After building the project, the repo's `assets/data/` folder must be
reachable as `data/` next to `inkternity.exe` — either copy it or, more
conveniently, create a directory junction:
```
mklink /J build\Release\data assets\data
```

You can create an NSIS installer of the application by running:
```
cpack -G NSIS
```
## Emscripten
You can use Emscripten to build a web version of this program. Keep in mind that this version might be more buggy, and is missing a few features. In addition, I have only tried building it on a Linux machine.

You'll need to setup conan toolchains. You can read about that [here](https://github.com/conan-io/conan-toolchains).

After cloning the repository, `cd` into the repo, then update the git submodules to get `datachannel-wasm`:
```
git submodule update --init --recursive
```
Then run:
```
./conan/export_libs.sh
conan install . --profile:host=conan/profiles/emscripten --profile:build=default --build=missing
cd build/Release
ln -s ../../data data
source generators/conanbuild.sh
cmake ../.. -DCMAKE_TOOLCHAIN_FILE=generators/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build .
source generators/deactivate_conanbuild.sh
```
These commands will generate a javascript file containing the entire program. An example of a website that can run and display this program can be found in `emscripteninstall/index.html`. To try this out, place `inkternity.js`, `emscripteninstall/index.html`, and `emscripteninstall/loading.gif` in a folder, and host a webserver from that folder. The server must set these two HTTP headers:
```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```
