# Ungine – Feature-Roadmap

Ziel: die im Projektgedächtnis dokumentierten Engine-Lücken als überprüfbare, einzeln lieferbare Phasen schließen. Jede Phase bekommt einen eigenen Commit und aktualisiert `README.md` sowie `CLAUDE.md`. Reihenfolge nach Abhängigkeiten; kein Phase gilt als fertig, solange Implementierung, Nutzerpfad und Fehlerverhalten fehlen.

## Reihenfolge

### Phase 21 – Skeletal Animation
- glTF-Skins, Joint-Hierarchien, inverse Bind-Matrizen und Animation-Clips laden.
- Laufzeit-Animator mit Clip-Auswahl, Loop/Once, Zeitsteuerung, Blend zwischen Clips und Root-Transform-Verhalten.
- GPU-Skinning für sichtbare Meshes; Bounds und Schatten-Culling berücksichtigen animierte Pose.
- Blueprint-Knoten zum Abspielen/Stoppen/Blenden und Abfragen des Animationszustands.
- Editor-Inspektion und eine mitgelieferte animierte Beispielszene.
- Fortschritt: Model-Asset-Strukturen, glTF-Skin-/TRS-Import, CPU-Pose-Sampler, Laufzeit-Wiedergabe, GPU-Skinning in Mesh-, Tiefen- und Schatten-Pässen, pro Frame aus Skin-Gewichten berechnete Bounds für Frustum-/Occlusion-Culling, Clip-Blending und optionales Root Motion sind umgesetzt. Animator-Inspector, Blueprint-Steuerung und ein animiertes Banner im Basic-Projekt sind ergänzt; Tests decken Import, Bounds, Root Motion, Blending, Szenen-Verknüpfung und Blueprint-Knoten ab. Build-, Shader- und GPU-Tests konnten mangels lokaler Build-Werkzeuge noch nicht ausgeführt werden.

### Phase 22 – In-Game-UI
- Laufzeit-UI mit Canvas, Widget-Baum, Layout/Anchors, Skalierung und Z-Reihenfolge.
- Text, Bild, Panel, Button, Checkbox, Slider und Progressbar; Fokus, Maus/Tastatur/Gamepad-Navigation.
- Blueprint-bindbare Werte/Ereignisse, HUD- und Menü-Vorlagen sowie Pause-/Optionsmenü im Player.
- Fortschritt: Canvas-/Widget-Komponenten mit Anchors, Designauflösung, Hierarchie und stabiler Sortierung; Screen-space Rendering für Text, Panels, Buttons, Checkboxen, Slider, Progressbars und Texturen; Maus-, Tastatur- und Gamepad-Steuerung; Blueprint-Events; rückwärtskompatible Szenen-Serialisierung; Editor-Inspector/Add-Component sowie Pause-/Optionsmenü mit Resume, Quit, Music-Lautstärke und Fullscreen im Player. CPU-Tests für Layout, Eingabekanten, Interaktion, Serialisierung und Blueprint-Events ergänzt. Build, Shader-Compile und Laufzeit-GPU-Test sind lokal noch nicht verifiziert (Build-Werkzeuge fehlen).


### Phase 23 – Content-Import und Vorschauen
- Importdialog mit Quellpfad, Ziel, Importoptionen, Fortschritt und verständlichen Fehlern.
- Thumbnails für Modelle, Texturen, Materialien und Audio; asynchron und gecacht.
- Referenzen bei Verschieben/Umbenennen aktualisieren; sichere Bestätigung bei nicht rückgängigem Löschen.
- Fortschritt: Asynchroner Import mit Datei-/Zielwahl, Fortschritt und Fehlerstatus; GLB, glTF samt lokalen URI-Abhängigkeiten, PNG/JPEG/KTX2 und WAV/OGG/MP3/FLAC. AssetManager-Cache liefert asynchrone Textur-, Modell/Material- und Audio-Wellenform-Vorschau (Wireframe max. 600 Dreiecke; gestreamtes Audio ohne PCM-Wellenform). Umbenennen und Ordnerdialog-Verschieben aktualisieren erkannte gespeicherte Referenzen sowie aktive Szenen-/Projektpfade und die aktuelle Szene; Löschen bestätigt vor der nicht rückgängig machbaren Entfernung. Noch nicht lokal kompiliert oder zur Laufzeit geprüft (C++/CMake/Vulkan-Werkzeuge fehlen); offene Referenz- und Formatgrenzen in MEMORY.md §20.

### Phase 24 – Level- und Bereichs-Streaming
- Asynchrones Laden/Entladen von Szenen und Teilbereichen ohne Blockieren des Hauptthreads.
- Explizite Streaming-Volumes/Anforderungen, Ladezustand und Übergänge/Ladebildschirm.
- Gültige Entity-/Asset-Referenzen über Szenengrenzen und definierte Fehler beim Entladen.
- Fortschritt (2026-10-06): umgesetzt und lokal verifiziert – Szenen werden auf dem ThreadPool geparst, Modelle vorab geladen und Entities in einem Schritt erzeugt; additive Sub-Level per Streaming-Volumen (Hysterese, StreamingSource) oder Blueprint (Load/Unload Stream Level, Is Level Loaded, Level-Events); Querverweise per UUID (null solange entladen); Open Level im Player asynchron mit anpassbarem Ladebildschirm; Editor-Levels-Fenster mit schreibgeschützter Vorschau. Grenzen in CLAUDE.md, Offene Punkte 18.

### Phase 25 – Gekochte Builds und Savegame-Versionierung
- Reproduzierbarer Cook-Schritt für Assets, gepackte Laufzeitdaten und selektive Einbeziehung von Content.
- Player startet aus gepackten Daten ohne Import-Kochen; Build-Bericht und klare Fehler bei fehlenden Assets.
- Versionierte Savegames mit Migration/Kompatibilitätsprüfung; Optionsspeicherung inklusive Input-Remapping.
- Fortschritt (2026-10-06): umgesetzt und lokal verifiziert – virtuelles Dateisystem mit einer unkomprimierten Pak-Datei (Index + FNV-Hashes, reproduzierbar), Cooker (Modelle als Binär-Mesh nach Optimierung/LODs, Texturen als KTX2 BC7/BC5, Rest roh, Build-Bericht mit fehlenden Referenzen), `UngineCook` + Editor-Paketierung mit Bericht, Player lädt aus `Content.upak`; Savegames mit Projekt-Save-Version (Migrate-Save-Game-Event, neuere Slots abgelehnt); Optionsmenü (Grafik/Audio/Steuerung mit Remapping) pro Benutzer gespeichert + Blueprint-API. Grenzen in CLAUDE.md, Offene Punkte 19.

### Phase 26 – Physik- und Character-Werkzeuge
- Joints/Constraints mit Grenzwerten, Motoren und serialisierbaren Parametern.
- Steuerbare Character-Rotation und robuste Character-Bewegung.
- Kollisionsmaterialien pro Dreieck sowie Collision-Events mit Kontaktpunkt/Impuls.
- Fortschritt (2026-10-07): umgesetzt und lokal verifiziert – Physik-Material-Assets (`.uphysmat`) pro Collider und pro glTF-Material eines Mesh-Colliders (Oberfläche in Raycasts/Kontakten), Kontaktdaten (Punkt/Normale/Impuls/Geschwindigkeit) + Persist-/Hit-Events, Joints (Fixed/Hinge/Slider/Ball/Distance/Cone/Swing-Twist/6DOF mit Limits, Motoren, Bruch), Ragdolls aus Skeletten, Character-Rotationsmodi/Plattformen/Ducken/Rutschen/Push-Kraft, Blueprint-Knoten und Editor-Werkzeuge (Inspector, Joint-Overlay + Anker-Gizmo, Material-Fenster). Grenzen in CLAUDE.md, Offene Punkte 11.

### Phase 27 – Audio-Ausbau
- Sound-Cues mit gewichteten Variationen, Zufall, Random-Pitch und Loop-Regeln.
- Voice-Priorisierung, damit wichtige Sounds bei Voice-Limit nicht verworfen werden.
- Bus-Snapshots/Ducking und räumliche Verbesserungen (mehrere Occlusion-Strahlen/Materialabsorption); HRTF als optionale, messbare Ausbaustufe.
- Fortschritt (2026-10-12): umgesetzt und lokal verifiziert – Sound-Cues (`.ucue`, gewichtete Varianten, Zufall, No-Repeat, Priorität, Instanz-Limit), Voice-Priorität mit Verdrängung und Limits pro Cue/Bus, Snapshots mit Überblendung, Sidechain-Ducking, Multi-Ray-Occlusion mit Absorption aus Physik-Materialien, Blueprint-/Editor-Werkzeuge. HRTF nach Rücksprache weggelassen. Grenzen in CLAUDE.md, Offene Punkte 15.

### Phase 28 – Partikel (C)
- Modulare Emitter (Niagara-artige Stacks), CPU-Simulation mit Jobs, Sprites/Mesh-Partikel/Ribbons, Sub-Emitter, Kollision, Partikel-Editor mit Vorschau.
- Fortschritt (2026-10-14): umgesetzt und lokal verifiziert – `.uparticle` mit 22 Modulen in vier Stufen, Kurven/Gradienten, deterministische CPU-Simulation auf dem ThreadPool, Kollision per Physik-Raycast, Sub-Emitter (Spawn/Death/Collision/Trail), Sprites (Billboard-Modi, Flipbook, Soft Particles), Mesh-Partikel, Ribbons, Komponente + Blueprint-Knoten + Cooker, Partikel-Editor (Undo, Live-Vorschau, Live-Übernahme in die Szene), Template-Effekte. Grenzen in CLAUDE.md, Offene Punkte 20.

### Phase 29 – Navigation (C)
- Recast/Detour gekachelt mit Laufzeit-Rebake, DetourCrowd, mehrere Agenten-Profile, Areas/Kosten, Off-Mesh-Links, dynamische Hindernisse, gebackene Datei, vollständige Editor-Werkzeuge.
- Fortschritt (2026-10-15): umgesetzt und lokal verifiziert – TileCache-Navmesh pro Profil aus statischen Collidern in NavMeshBounds, Hintergrund-Neubau geänderter Kacheln, `.unav` neben der Szene (beim Laden nur geänderte Kacheln neu), Areas/Blocked, Links, Hindernisse, Crowd-Agenten (direkt oder über den CharacterController), Abfragen, Blueprint-Knoten (AI Move To, Find Path, …), Editor (Navigation-Fenster, Overlay, Testpfad, Inspector, Project Settings), Physics-Template-Wanderer. Grenzen in CLAUDE.md, Offene Punkte 21.

### Phase 30 – Animation-State-Machine + IK (C)
- `.uanim`-Asset mit Graph-Editor, 1D-/2D-Blend-Spaces, Layer + Bone-Masken, Notifies; IK: Two-Bone, Foot-IK auf Gelände, Look-At, FABRIK; Testmodell Khronos-Fuchs.
- Fortschritt (2026-10-22): umgesetzt und lokal verifiziert – Parameter/Layer/States/Transitions (Any State, Exit Time, Trigger), phasensynchrone Blend Spaces (2D per Delaunay), Additiv-Layer, Notifies (Event/Sound/Partikel), Root Motion, vier IK-Löser, AnimationController + AnimationSystem, Blueprint-Knoten, Cooker-Prüfung, Animation-Graph-Fenster mit Live-Vorschau, Fuchs als Physics-Template-Wanderer. Fix: Skinning-Fehler aus Phase 21 (Skin-Einflüsse beim Vertex-Reorder). Grenzen in CLAUDE.md, Offene Punkte 22.

### Phase 31 – TAA, Decals, SSR, Nebel (C)
- TAA mit Motion Vectors, Clustered Decals, Hi-Z-SSR mit temporaler Akkumulation, Höhennebel + volumetrischer Froxel-Nebel mit Nebel-Volumen.
- Fortschritt (2026-10-26): umgesetzt und lokal verifiziert – Halton-Jitter, Velocity-Target (Skinning, Partikel, Transparente), Resolve mit Varianz-Clipping + Schärfen; Decals in den Licht-Clustern (Albedo/Normal/Maske/Emissive, Winkel-/Kanten-Fade, Sortierung, Lebensdauer, Blueprint-Spawn); SSR über eine Closest-Depth-Pyramide (halbe Auflösung, temporal, Rauheits-Blur, IBL-Fallback); analytischer Höhennebel + Froxel-Gitter (Sonne mit Kaskadenschatten, Clusterlichter mit Schatten, Himmel, Nebel-Volumen, temporale Reprojektion); Szenen-Settings, Editor (Renderer-Abschnitte, Inspector, Overlay), Cooker. Grenzen in CLAUDE.md, Offene Punkte 23.

### Phase 32 – Terrain (C)
- Heightmap-Terrain mit CDLOD, Splat-Layern + Painting/Sculpting mit Undo, Foliage/Gras, Heightmap-Import/-Export, Löchern + Triplanar.
- Fortschritt (2026-10-26): umgesetzt und lokal verifiziert – `.uterrain` (Höhen/Splat/Löcher/Foliage-Dichte, `TerrainStore` mit Änderungsrechtecken), CDLOD-Quadtree mit Morphing (Prepass/Hi-Z, Kaskaden, Lighting mit gemeinsamem `ShadeSurface`), bis 8 Layer (Albedo/Normal/ORM, triplanar an Hängen), Löcher (Render/Physik/Navmesh), Jolt-HeightField, Navmesh-Quelle pro Kachel, Foliage (Chunk-Scatter, Halme/Karten/Meshes, Wind mit Motion Vectors, Ausdünnung, Schatten), Editor (Pinsel mit Undo pro Strich, Terrain-Fenster, Inspector, Content-Typ TER, Import/Export), Cooker, Blueprint *Get Terrain Height*. Grenzen in CLAUDE.md, Offene Punkte 24.

### Phasen 31–34 (C, Reihenfolge vom Nutzer bestätigt)
- 31 TAA/Decals/SSR/Nebel, 32 Terrain, 33 Material-Editor, 34 verschachtelte Prefabs + Settings-Undo.

## Arbeitsregeln
- Pro Phase erst bestehende Schnittstellen prüfen, dann vertikal implementieren (Datenformat → Laufzeit → Editor/Player → Dokumentation).
- Bestehende Projektvorgabe: pro Phase ein Git-Commit; Branch `codex/feature-roadmap-and-phases`; nicht auf andere Branches pushen.
- Vor jeder Phase Abhängigkeiten und Akzeptanzkriterien festhalten. Keine stillen Datenformatbrüche; neue Szenen-/Asset-Felder rückwärtskompatibel einlesen.
- Bei nicht verfügbarer GPU/Windows-Laufzeit die konkret nicht verifizierten Pfade im Abschluss notieren.
