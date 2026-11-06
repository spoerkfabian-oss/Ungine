# Folder `tools/`

Developer-only utilities. Nothing here is built by default (`EXCLUDE_FROM_ALL` in
`tools/CMakeLists.txt`) or installed.

## MakeSounds

Generates the sample sounds in `assets/sounds/` (and the copies in the templates). Everything is
synthesized and deterministic, so the files carry no third-party license (CC0).

```
cmake --build build --target MakeSounds
build/tools/MakeSounds assets/sounds          # Windows: build\tools\<Config>\MakeSounds.exe
```

Output: `impact.wav`, `hum.wav`, `chime.wav`, `music_loop.wav` (16-bit PCM). After regenerating, copy
the changed files into `templates/*/Content/Sounds/` and update `assets/sounds/LICENSE.md` if the
list changes.

## Adding a tool

Add an `EXCLUDE_FROM_ALL` executable to `tools/CMakeLists.txt`, link `Engine`, call
`engine_set_warnings(<target>)` and put it in the `Tools` IDE folder, then document it here.
