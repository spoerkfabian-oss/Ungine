# Ungine – Feature-Roadmap

Ziel: die im Projektgedächtnis dokumentierten Engine-Lücken als überprüfbare, einzeln lieferbare Phasen schließen. Jede Phase bekommt einen eigenen Commit und aktualisiert `README.md` sowie `CLAUDE.md`. Reihenfolge nach Abhängigkeiten; kein Phase gilt als fertig, solange Implementierung, Nutzerpfad und Fehlerverhalten fehlen.

## Reihenfolge

### Phase 21 – Skeletal Animation
- glTF-Skins, Joint-Hierarchien, inverse Bind-Matrizen und Animation-Clips laden.
- Laufzeit-Animator mit Clip-Auswahl, Loop/Once, Zeitsteuerung, Blend zwischen Clips und Root-Transform-Verhalten.
- GPU-Skinning für sichtbare Meshes; Bounds und Schatten-Culling berücksichtigen animierte Pose.
- Blueprint-Knoten zum Abspielen/Stoppen/Blenden und Abfragen des Animationszustands.
- Editor-Inspektion und eine mitgelieferte animierte Beispielszene.
- Fortschritt: Model-Asset-Strukturen, Import von Skin-Joints/inversen Bind-Matrizen, glTF-TRS-Tracks (STEP/LINEAR/CUBICSPLINE), JOINTS_0/WEIGHTS_0 und ein wiederverwendbarer CPU-Pose-Sampler sind lokal umgesetzt. Noch offen: Laufzeit-Einbindung, GPU-Skinning, Bounds, Blueprint und Demo.

### Phase 22 – In-Game-UI
- Laufzeit-UI mit Canvas, Widget-Baum, Layout/Anchors, Skalierung und Z-Reihenfolge.
- Text, Bild, Panel, Button, Checkbox, Slider und Progressbar; Fokus, Maus/Tastatur/Gamepad-Navigation.
- Blueprint-bindbare Werte/Ereignisse, HUD- und Menü-Vorlagen sowie Pause-/Optionsmenü im Player.

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
