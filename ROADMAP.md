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

### Phase 24 – Level- und Bereichs-Streaming
- Asynchrones Laden/Entladen von Szenen und Teilbereichen ohne Blockieren des Hauptthreads.
- Explizite Streaming-Volumes/Anforderungen, Ladezustand und Übergänge/Ladebildschirm.
- Gültige Entity-/Asset-Referenzen über Szenengrenzen und definierte Fehler beim Entladen.

### Phase 25 – Gekochte Builds und Savegame-Versionierung
- Reproduzierbarer Cook-Schritt für Assets, gepackte Laufzeitdaten und selektive Einbeziehung von Content.
- Player startet aus gepackten Daten ohne Import-Kochen; Build-Bericht und klare Fehler bei fehlenden Assets.
- Versionierte Savegames mit Migration/Kompatibilitätsprüfung; Optionsspeicherung inklusive Input-Remapping.

### Phase 26 – Physik- und Character-Werkzeuge
- Joints/Constraints mit Grenzwerten, Motoren und serialisierbaren Parametern.
- Steuerbare Character-Rotation und robuste Character-Bewegung.
- Kollisionsmaterialien pro Dreieck sowie Collision-Events mit Kontaktpunkt/Impuls.

### Phase 27 – Audio-Ausbau
- Sound-Cues mit gewichteten Variationen, Zufall, Random-Pitch und Loop-Regeln.
- Voice-Priorisierung, damit wichtige Sounds bei Voice-Limit nicht verworfen werden.
- Bus-Snapshots/Ducking und räumliche Verbesserungen (mehrere Occlusion-Strahlen/Materialabsorption); HRTF als optionale, messbare Ausbaustufe.

## Arbeitsregeln
- Pro Phase erst bestehende Schnittstellen prüfen, dann vertikal implementieren (Datenformat → Laufzeit → Editor/Player → Dokumentation).
- Bestehende Projektvorgabe: pro Phase ein Git-Commit; Branch `codex/feature-roadmap-and-phases`; nicht auf andere Branches pushen.
- Vor jeder Phase Abhängigkeiten und Akzeptanzkriterien festhalten. Keine stillen Datenformatbrüche; neue Szenen-/Asset-Felder rückwärtskompatibel einlesen.
- Bei nicht verfügbarer GPU/Windows-Laufzeit die konkret nicht verifizierten Pfade im Abschluss notieren.
