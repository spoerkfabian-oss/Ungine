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
   Projekt, `Content.upak` = gekochter Inhalt, `BuildReport.txt`); das Fenster *Package report*
   zeigt Warnungen (fehlende Referenzen) und Fehler. Ohne Editor: `UngineCook <projekt> <ziel>`.
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
    keyboard, mouse and mapped gamepad navigation.
14. **Level-Streaming**: Hierarchy → *+ Add* → *Streaming Volume*, im Inspector die Level-Datei
    wählen (*Browse…*). Im Spiel wird das Sub-Level additiv geladen, sobald die Kamera (bzw. eine
    Entity mit *Streaming Source*) in der Box ist, und nach Verlassen der Box plus *Unload margin*
    wieder entladen. Blueprints: *Load/Unload Stream Level*, *Is Level Loaded*, Events *Level
    Loaded/Unloaded*; Verweise auf Entities eines entladenen Levels sind leer und werden beim Laden
    wieder gültig. Im Editor zeigt das Fenster **Levels** Vorschauen (nur ansehen, nicht gespeichert;
    bearbeitet wird ein Level durch Öffnen seiner Datei). *Open Level* lädt im Player im Hintergrund
    hinter einem Ladebildschirm: eigene Gestaltung über *Project Settings → Loading screen* (Szene mit
    UI-Canvas; ein Fortschrittsbalken mit dem Tag `LoadingProgress` zeigt den Fortschritt).
15. **Optionen & Spielstände**: Das Pause-Menü des Players (Escape) hat *Options* mit Grafik
    (Vollbild, VSync, Schattenqualität, AO, Bloom), Audio (Bus-Lautstärken) und Steuerung (Tasten
    neu belegen inkl. Konflikt-Hinweis, Zurücksetzen); gespeichert pro Benutzer (`Settings.json`).
    Blueprints: Kategorie *Options* und *Input → Remap Action*. *Project Settings → Save version*
    versioniert Spielstände: ältere Slots lösen beim Laden das Event *Migrate Save Game* aus,
    Slots einer neueren Spielversion werden abgelehnt (nicht gelesen, nicht überschrieben).
16. **Physik-Werkzeuge**: *Content → + New → Physics material* legt ein `.uphysmat` an (Reibung,
    Abprallen, Oberflächen-Name); im Collider-Inspector zuweisen, bei Mesh-Collidern auch pro
    glTF-Material. Raycasts und Kollisions-Events melden Oberfläche, Kontaktpunkt, Normale und
    Impuls (*On Hit* mit Mindest-Impuls, *On Collision Stay* jeden Step). *Add component → Joint*
    verbindet zwei Körper (Fixed, Hinge, Slider, Ball, Distance, Cone, Swing-Twist, 6DOF) mit
    Limits, Motor und Bruchkraft; den Joint-Anker verschiebt die Toolbar-Schaltfläche *Anchor* per
    Gizmo. *Add component → Ragdoll (from skeleton)* auf einem animierten Modell erzeugt Knochen mit
    Kapseln und Gelenken; *Simulate* (oder Blueprint *Set Ragdoll Simulating*) lässt es fallen.
    Character Controller: Drehmodus (Bewegung/Kamera/Script), Ducken, bewegte Plattformen,
    Rutschen an steilen Hängen.
17. **Audio-Mixer & Sound-Cues**: *Content → + New → Sound cue* legt ein `.ucue` an: Varianten (Sounds
    per „…“ oder aus dem Content-Browser hineinziehen) mit Gewicht, Zufalls-Lautstärke/-Tonhöhe,
    ohne direkte Wiederholung, Priorität und Instanz-Limit. Ein Cue funktioniert überall wie ein
    Sound (Audio Source, *Play Sound*-Knoten). Im Renderer-Panel unter *Audio*: Voice-Limits pro
    Bus, **Snapshots** (benannte Bus-Mixe, per Blueprint *Activate/Deactivate Audio Snapshot*
    überblendet) und **Ducking** (z. B. UI senkt Musik). Occlusion nutzt mehrere Strahlen pro
    Quelle; Wände dämpfen nach der *Sound absorption* ihres Physik-Materials.
18. **Partikel**: *Content → + New → Particle effect* legt ein `.uparticle` an und öffnet den
    Partikel-Editor: Emitter mit Modul-Stacks (Spawn, Initialize, Update, Events – z. B. Spawn Rate,
    Shape, Velocity, Gravity, Curl Noise, Color/Size over Life als Gradient/Kurve, Collision,
    Sub Emitter) und Render-Einstellungen (Sprite mit Sprite-Sheet und weichen Kanten, Mesh, Ribbon
    für Spuren; Alpha oder Additiv). Die Vorschau rechts zeigt jede Änderung sofort; die Szene
    übernimmt sie ebenfalls, gespeichert wird mit *Save*. Platziert wird ein Effekt per Doppelklick
    bzw. Drag & Drop in den Viewport (Komponente *Particle System*); Blueprint-Knoten *Spawn Emitter
    at Location*, *Play/Stop Particles*, *Set Particle Scale/Tint*. Beispiele: Lagerfeuer (Basic),
    Funkenbrunnen mit Kollision und Spuren (Physics).
19. **Navigation (KI)**: Hierarchy → *+ Add* → *Nav Mesh Bounds* um den begehbaren Bereich legen –
    das Navmesh entsteht aus den statischen Collidern darin und folgt jeder Änderung (Fenster
    *Navigation*: Overlay, *Bake*, Testpfad per *Pick start/end* + Klick im Viewport). Gespeichert
    wird es als `.unav` neben der Szene. *Nav Area* markiert Flächen (Kosten, *Blocked*), *Nav Link*
    verbindet Punkte (Sprünge, Leitern), *Nav Obstacle* schneidet bewegliche Objekte heraus.
    Figuren bekommen die Komponente *Nav Agent* und laufen per Blueprint *AI Move To* (weichen
    einander aus; mit Character Controller über die Physik). Agenten-Profile (Radius/Höhe) und
    Area-Typen: *File → Project settings → Navigation*. Beispiel: der Wanderer im Physics-Template.
20. **Animation**: Content-Browser → *+ New → Animation graph* (`.uanim`) öffnet das Fenster
    *Animation Graph*: Parameter (Float/Int/Bool/Trigger), Layer (mit Bone-Maske, Override oder
    Additiv), States per Rechtsklick auf die Fläche (ein Clip oder ein 1D-/2D-Blend-Space),
    Transitions per Rechtsklick auf einen State (Bedingungen, Überblendzeit, Exit Time), Notifies
    (Sound/Effekt an einem Knochen) und IK-Ziele (Füße auf dem Boden, Two-Bone, Look-At, FABRIK).
    Die Vorschau zeigt das Preview-Modell mit den eingestellten Parameterwerten. Zuweisen per
    *Assign to selection* auf ein platziertes Modell (Komponente *Animation Controller*); gesteuert
    per Blueprint *Set Anim Float/Bool/Trigger*, Events per *On Anim Notify*. Beispiel: der Fuchs
    im Physics-Template (Gehen/Rennen nach Geschwindigkeit, Füße folgen dem Boden).
21. **Bildqualität, Decals, Reflexionen, Nebel**: Im Renderer-Panel sind *Anti-aliasing (TAA)*
    (glättet Kanten über mehrere Frames, mit Nachschärfen) und *Reflections (SSR)* (Spiegelungen
    glatter Flächen aus dem Bild, sonst Himmel) standardmäßig an. *+ Add → Decal* projiziert eine
    Textur (Albedo/Normal/Maske/Emissive, per Drag & Drop aus dem Content-Browser in den
    Inspector) entlang −Z auf die Oberflächen in der Box; *Sort order* bestimmt, was oben liegt,
    *Lifetime* lässt Decals im Spiel ausblenden (Blueprint *Spawn Decal at Location*). Unter *Fog*
    wird Höhennebel eingeschaltet; *Volumetric* fügt Lichtstrahlen von Sonne und Lampen (mit
    Schatten) hinzu, *+ Add → Fog Volume* setzt lokale Nebelboxen (auch leuchtend). Alle
    Einstellungen werden mit der Szene gespeichert; die Toolbar *Volumes* zeigt Decal- und
    Nebelboxen.
22. **Terrain**: *+ Add → Terrain* (oder Content-Browser *+ New → Terrain*, `.uterrain` per
    Doppelklick/Drag & Drop platzieren) legt ein 128-m-Terrain an. Im Fenster *Terrain* (bei
    ausgewähltem Terrain) gibt es Pinsel *Raise/Lower/Smooth/Flatten* (Shift+Klick nimmt die
    Flatten-Höhe), *Paint* (Layer in der Liste wählen), *Holes* und *Foliage*; Strg kehrt um
    (senken/radieren), jeder Strich ist ein Undo-Schritt. Layer haben Albedo/Normal/ORM-Texturen
    (Drag & Drop aus dem Content-Browser), Tint, Kachelung; steile Hänge werden triplanar
    projiziert. Foliage-Layer sind prozedurale Grashalme, Textur-Karten oder Meshes (Dichte,
    Größe, Wind, Sichtweite). Heightmaps (16-Bit-PNG/RAW) per *Import/Export*. Das Terrain
    kollidiert (Physik), dient als Navmesh-Quelle und wird beim Speichern der Szene mitgespeichert;
    Blueprint *Get Terrain Height* liefert Höhe und Normale.
23. **Materialien**: Content-Browser *+ New → Material* legt einen Node-Graph (`.umat`) an,
    *Create instance* bzw. *+ New → Material instance* eine Instanz (`.uminst`), die Parameter
    überschreibt. Im Fenster *Material*: Rechtsklick auf die Fläche öffnet die Knotenauswahl (tippen,
    Enter), Pins per Ziehen verbinden, Alt-Klick trennt, Entf löscht; links Blend-Modus (Opaque/
    Masked/Translucent/Additive), Lit/Unlit, Two-sided, Parameter (Scalar/Vector/Texture) und die
    Werte des gewählten Knotens; rechts die Vorschau (Kugel/Box/Ebene). *World Position Offset*
    bewegt Vertices (für das Culling *Max displacement* setzen). Zuweisen: Material auf ein Mesh im
    Viewport ziehen oder im Inspector (*Mesh Renderer → Materials*) pro Submesh ablegen; dort lassen
    sich Parameter pro Objekt überschreiben. Blueprints: *Set Material*, *Set Material Scalar/Vector
    Parameter*, *Clear Material Parameters*. Die Shader entstehen beim ersten Laden (Cache unter
    `Saved/Cache/Shaders`); das Basic-Template zeigt ein pulsierendes Beispielmaterial.


## Entwicklung

`Sandbox` ist die Test-/Demo-Anwendung der Engine (siehe `CLAUDE.md` für Architektur, Optionen
und den Stand der Entwicklung). Tests: `EngineTests` (CPU), `EngineGpuTests` und Smoke-Tests per
CTest mit `-DENGINE_GPU_TESTS=ON`. Geplante Engine-Erweiterungen und ihre Reihenfolge stehen in
[ROADMAP.md](ROADMAP.md).
