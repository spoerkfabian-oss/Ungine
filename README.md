# Ungine

C++20/Vulkan-1.3-Engine mit Editor (UE-/Unity-artig), Blueprint-Visual-Scripting, Jolt-Physik
und einem Player für fertige Spiele.

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
   Blueprints und Szenen an.
4. **Blueprint**-Tab: Knoten per Rechtsklick suchen, Pins ziehen zum Verbinden, Variablen links,
   Details/Parameter darunter. Script einer Entity zuweisen: Inspector → *Add component* →
   *Script (Blueprint)*.
5. **Play** (Strg+P) simuliert Physik und Blueprints im Editor; *Stop* stellt die Szene wieder her.
6. **Build → Build & Run** (Strg+B) speichert alles und startet das Spiel im **UnginePlayer**;
   **Build → Package project…** erzeugt einen eigenständigen Spielordner (`<Name>.exe`, Shader,
   Projekt, Content).
7. Der Player rendert durch die primäre **Camera**-Komponente der Startszene (*File → Project
   settings…*). Im Editor zeigt *Game cam* diese Sicht.

## Entwicklung

`Sandbox` ist die Test-/Demo-Anwendung der Engine (siehe `CLAUDE.md` für Architektur, Optionen
und den Stand der Entwicklung). Tests: `EngineTests` (CPU), `EngineGpuTests` und Smoke-Tests per
CTest mit `-DENGINE_GPU_TESTS=ON`.
