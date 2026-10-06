# Ungine

C++20/Vulkan-1.3-Engine mit Editor (UE-/Unity-artig), Blueprint-Visual-Scripting, Jolt-Physik,
3D-Audio (miniaudio) und einem Player für fertige Spiele.

## Bauen (Windows, Visual Studio 2022)

```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

Startprojekt in Visual Studio ist **UngineEditor**. Abhängigkeiten lädt CMake selbst (FetchContent);
nötig sind nur das Vulkan SDK (für `glslc`) und ein Vulkan-1.3-Treiber.

## Installer (Doppelklick-Editor)

```
cd build
cpack -C Release
```

Ergebnis: `Ungine-<Version>-win64.exe` (NSIS, falls installiert) und ein ZIP. Der Installer legt
Startmenü- und Desktop-Verknüpfung **Ungine Editor** an und verknüpft `.ungineproj`-Dateien mit
dem Editor – ein Doppelklick auf ein Projekt öffnet es.

Linux: `cpack` erzeugt ein `.tar.gz`; nach dem Entpacken registriert
`bin/install-desktop-integration.sh` Menüeintrag, Icon und den Dateityp.

## Arbeiten mit dem Editor

1. **UngineEditor** starten → Projektbrowser: zuletzt geöffnete Projekte, neues Projekt aus einer
   Vorlage (*Blank*, *Basic Scene*, *Physics Playground*) oder *Open project…*.
2. Projektordner: `<Name>.ungineproj`, `Content/` (Szenen, Blueprints, Modelle, Texturen),
   `Saved/` (Caches, Layout – nicht Teil des Spiels).
3. **Content**-Browser: Doppelklick öffnet Szenen/Blueprints bzw. platziert Modelle; Dateien lassen
   sich in den Viewport ziehen (Modelle) oder auf die Auswahl (Blueprints). *+ New* legt Ordner,
   Blueprints und Szenen an. *Import file...* kopiert Modelle (.glb/.gltf samt lokalen Dateien),
   Texturen und Sounds in den aktuellen Content-Ordner. Auswahl zeigt Textur-, Modell-/Material- oder
   Audiovorschau; Rechtsklick bietet *Move to folder...*, *Rename...* und die bestätigungspflichtige
   Löschung. Referenzen in unterstützten Szenen-, Blueprint-, Prefab- und glTF-Dateien werden bei
   Verschieben/Umbenennen angepasst.
4. **Blueprint**-Tab: Knoten per Rechtsklick suchen, Pins ziehen zum Verbinden, Variablen links,
   Details/Parameter darunter. Script einer Entity zuweisen: Inspector → *Add component* →
   *Script (Blueprint)*.
5. **Play** (Strg+P) simuliert Physik und Blueprints im Editor; *Stop* stellt die Szene wieder her.
6. **Build → Build & Run** (Strg+B) speichert alles und startet das Spiel im **UnginePlayer**;
   **Build → Package project…** erzeugt einen eigenständigen Spielordner (`<Name>.exe`, Shader,
   Projekt, Content).
7. Der Player rendert durch die primäre **Camera**-Komponente der Startszene (*File → Project
   settings…*). Im Editor zeigt *Game cam* diese Sicht.
8. **Audio**: Sounds (WAV/OGG/MP3/FLAC) nach `Content/` kopieren. Doppelklick im Content-Browser
   spielt sie an; in den Viewport ziehen legt eine **Audio Source** an (3D, Loop, Bus, Reichweite,
   Occlusion – Inspector). **Reverb Zone** und **Audio Listener** über *Add component*. Mixer
   (Master/World/Music/UI/Ambient) und Occlusion: Renderer-Panel → *Audio* (im Projekt gespeichert).
   Blueprints: Kategorie *Audio* (Play Sound 2D, Play Sound at Location, Play/Stop Audio Source, …;
   Pfade relativ zum Projektordner, z. B. `Content/Sounds/impact.wav`).
9. **Blueprints für Fortgeschrittene**: links *Graphs* → *+ Function* (Ein-/Ausgänge, lokale
   Variablen, *Pure*); Aufruf über die Knotensuche (*Call …*). Variablen können Arrays sein
   (*For Each Loop*, *Add*, *Find*, …); das Häkchen neben einer Variable macht sie
   *instance editable* (Wert pro Entity im Inspector, auch Verweise auf andere Entities).
   **Debugger**: F9 setzt einen Breakpoint (roter Punkt, im Details-Panel optional mit Bedingung
   wie `health < 10` und Hit-Count); im Play-Modus hält das Spiel dort an, F5 läuft weiter, F10
   springt über Funktionsaufrufe, F11 hinein, Shift+F11 heraus; Werte und Call Stack stehen im
   *Debug*-Bereich und im Pin-Tooltip. Komfort: Doppelklick auf eine Verbindung = Reroute-Knoten, Shift+W/A/S/D richtet
   aus, M blendet die Minimap um, C setzt einen Kommentar um die Auswahl (Farben im Details-Panel).
10. **Prefabs** (Blueprint-Klassen): Hierarchy → Rechtsklick → *Create prefab…* speichert den
   Teilbaum als `.uprefab` (z. B. `Content/Prefabs/`). Prefabs per Doppelklick oder Ziehen aus dem
   Content-Browser platzieren; Änderungen an einer Instanz sind *Overrides* (Inspector: einzeln
   zurücksetzen), *Apply to prefab* überträgt sie auf alle Instanzen, *Unlink* löst die Verbindung.
   Blueprints erzeugen Prefabs zur Laufzeit mit *Spawn Prefab*. *Print String* erscheint im Player
   oben links auf dem Bildschirm.
11. **Blueprints 3**: Content → *+ New* → *Enum*, *Struct*, *Interface* (bearbeitet im Fenster
   *Blueprint types*) und *Blueprint library* (Funktionen/Makros für alle Blueprints, Aufruf als
   `Bibliothek.Funktion`). Variablen-Typen: Arrays, Maps (`Map<string, int>`), Enums, Structs. In der
   Blueprint-Seitenleiste: *+ Macro* (mehrere Exec-Pins, Delays erlaubt), *Custom Events* mit
   Parametern, *Event Dispatchers* (Call/Bind), *Interfaces* (fügt die Funktionen an),
   *Timelines* (Kurven-Editor: Doppelklick = Key, Ziehen verschiebt). Knoten: *Switch*, *Select*,
   *Tween* (Move/Rotate/Scale To), *Save Game*, *Open Level*, *Input Actions* (Project Settings →
   *Input*). **Construction Script** (*Construction Script*-Event): läuft im Editor nach jeder
   Änderung und vor BeginPlay; was es erzeugt, wird nicht gespeichert (grau in der Hierarchy).
   Strg+F sucht in allen Blueprints, Rechtsklick → *Collapse to function / macro* fasst Knoten
    zusammen.
12. **Skeletal animation**: The *Basic Scene* template includes a waving glTF banner. Imported
    animated models play their first clip automatically. Select the model instance to change its
    clip, speed, looping, blend target and root-motion settings in the **Animator** inspector.
    Blueprints can play, stop, select and blend clips, change speed/looping/root motion and query
    playback. Supported glTF interpolation modes are STEP, LINEAR and CUBICSPLINE.
13. **In-game UI**: Add a *UI Canvas* and child *UI Widget* components in the Inspector. Widgets
    use parent-relative anchors and offsets, with a design resolution that scales into the game
    viewport. Text, images, panels, buttons, checkboxes, sliders and progress bars are supported.
    Buttons and values can raise Blueprint events (On UI Clicked, On UI Value Changed,
    On UI Checked Changed). The standalone player opens its pause menu with Escape and supports
    keyboard, mouse and mapped gamepad navigation; Options currently control music volume and
    fullscreen for the current session.
14. **Level-Streaming**: Hierarchy → *+ Add* → *Streaming Volume*, im Inspector die Level-Datei
    wählen (*Browse…*). Im Spiel wird das Sub-Level additiv geladen, sobald die Kamera (bzw. eine
    Entity mit *Streaming Source*) in der Box ist, und nach Verlassen der Box plus *Unload margin*
    wieder entladen. Blueprints: *Load/Unload Stream Level*, *Is Level Loaded*, Events *Level
    Loaded/Unloaded*; Verweise auf Entities eines entladenen Levels sind leer und werden beim Laden
    wieder gültig. Im Editor zeigt das Fenster **Levels** Vorschauen (nur ansehen, nicht gespeichert;
    bearbeitet wird ein Level durch Öffnen seiner Datei). *Open Level* lädt im Player im Hintergrund
    hinter einem Ladebildschirm: eigene Gestaltung über *Project Settings → Loading screen* (Szene mit
    UI-Canvas; ein Fortschrittsbalken mit dem Tag `LoadingProgress` zeigt den Fortschritt).


## Entwicklung

`Sandbox` ist die Test-/Demo-Anwendung der Engine (siehe `CLAUDE.md` für Architektur, Optionen
und den Stand der Entwicklung). Tests: `EngineTests` (CPU), `EngineGpuTests` und Smoke-Tests per
CTest mit `-DENGINE_GPU_TESTS=ON`. Geplante Engine-Erweiterungen und ihre Reihenfolge stehen in
[ROADMAP.md](ROADMAP.md).
