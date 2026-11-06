# Audio

Audio uses miniaudio 0.11.22 (only `AudioEngine.cpp`, `Sound.cpp` and `SoundFile.h` include it).

## Sounds

- `Sound.h`: `SoundData` (interleaved float PCM or a `streamFile`), `LoadSoundFile(path,
  SoundLoadMode Auto/Decode/Stream)` for WAV/MP3/FLAC/OGG (Auto streams sounds longer than
  `kSoundStreamThresholdSeconds` = 10 s; Windows uses the wide-char APIs), `DecodeSound(bytes)`,
  `IsSoundFile`, `WriteWav` (16-bit PCM), `MakeTone` / `MakeNoise`.
- `AssetManager::LoadSound(path, mode)` → `SoundHandle` (cached per path + mode, worker job, Ready
  without GPU work), `Get` → `shared_ptr<const SoundData>`, hot reload by mtime; voices that are
  already playing keep the old data. Sounds are read through the VFS (also streamed from a pak).

## AudioEngine

- Main-thread API, mixing on the device thread. Without a device (`AudioEngineDesc::device = false`)
  `Render(span)` mixes offline; otherwise miniaudio may use its null device.
- **Buses** Master → endpoint; Music, UI, Ambient → Master; World → Freeverb reverb node → Master.
  Each bus is a `BusNode`: user volume × snapshot mix (`SetBusMix(bus, gain, fade)`), per-block RMS
  level (`BusLevel`), ducking, continuous processing (runs without input). The engine starts only
  after the graph is built.
- **Voices:** data source (`ma_audio_buffer_ref` on shared PCM kept alive by `shared_ptr`, or a
  `ma_decoder` for streaming with a synchronous start-offset seek) → `ma_sound` (pitch, loop, 3D
  attenuation model, min/max distance, rolloff, Doppler, position/velocity) → `VoiceNode` (volume
  ramps for fades and `SetVolume(fade)`, occlusion = one-pole low-pass 20 kHz → 800 Hz plus up to
  −12 dB, 30 ms smoothing) → bus. `Stop(fade)`; `IsPlaying` is false right after Stop; `Update()`
  reaps finished voices.
- Gains are atomics with 10 ms ramps (miniaudio's smoothed `ma_sound` volume is not thread-safe);
  the listener's world-up is fixed to +Y (`ma_engine_listener_set_world_up` is not synchronized).
- **Priority and limits:** `VoiceDesc::priority / group / groupLimit / groupLimitMode`. `Admit` checks
  the group (cue instances: steal the least important or reject), the bus (`SetBusVoiceLimit`; steals
  only voices with priority ≤ the new one) and the engine (`maxVoices`: only lower priority is
  displaced; equal priority is rejected). "Least important" = lowest priority, then quietest at the
  listener (volume × distance model × occlusion), then oldest. Stolen voices fade out in 15 ms.
  `AudioStats::stolen / dropped`.
- **Ducking:** `DuckingRule{source, target, depth, threshold, attack, release}` (`SetDucking`); one
  envelope per source bus per target, the strongest reduction wins (`BusDuckGain`). One block of
  latency when the target mixes before the source.

## Components

- `AudioSource`: sound or cue path, bus, volume, pitch, loop, play on start, stream, fade-in, spatial,
  attenuation, min/max, rolloff, Doppler, occlusion, priority (default 128; a cue's priority wins).
- `AudioListener`; `ReverbZone` (half extents, blend distance, `ReverbParams`).
- Paths are absolute in memory and relative to the scene in files.

## AudioSystem

`AudioSystem(engine, assets?, physics?)`:

- `Begin / Update / End` (play mode) and `SetPaused`. Listener = first `AudioListener` > primary camera
  > the view passed in.
- Sources: new/changed/removed; sound, stream, bus or spatial changes restart with a 30 ms fade,
  everything else updates live (volume over 50 ms, pitch, loop, range). Doppler velocity from position
  differences (> 150 m/s discarded).
- **Occlusion:** `occlusionRaysPerSource` (default 3) rays — the direct path plus offset start points on
  a circle (`occlusionSpread` 0.5 m) across the view direction; up to 6 walls per ray, each multiplying
  transmission by 1 − `absorption` of its physics material (no material = fully blocking);
  occlusion = (1 − mean) × strength. The budget `occlusionRays` counts raycasts per frame (at least one
  source per frame). The source's own body is ignored.
- **Reverb zones:** weight 1 inside, linear across `blendDistance` outside; parameters are weighted
  averages, wet / max(1, Σw).
- One-shots `PlayAt` / `Play2D` (wait up to 1 s for loading), `Preview` (editor, UI bus),
  `Play / Stop / IsPlaying(entity)`.
- **Snapshots:** `AudioSettings::snapshots` (`AudioSnapshot{name, volume[bus], fadeSeconds}`);
  `ActivateSnapshot / DeactivateSnapshot(name, fade)` (fade < 0 = own), `DeactivateAllSnapshots`,
  `SnapshotWeight`, `ActiveSnapshots`. Active snapshots multiply (`1 + (v − 1)·w` per bus). `End`
  resets the mix.
- Without an AssetManager sounds load synchronously (tests/tools). The system holds one reference per
  sound until its destructor (destroy it before the AssetManager).

## Sound cues

`.ucue` (`SoundCue.h`): variants `{sound (relative to the cue), weight, volume, pitch}`, volume and
pitch ranges, `noRepeat`, priority 0–255, `maxInstances`, `limitMode` steal/reject.
`PickCueVariant` is weighted (empty or zero-weight variants never play; no repeat with ≥ 2 variants).
A cue works anywhere a sound path works (AudioSource, PlayAt/Play2D, Blueprint nodes, preview): every
play picks a new variant with random volume/pitch; an AudioSource keeps its pick until the next play.
Cues are cached per path, their variants preloaded, hot reloaded by mtime (once per second; preview
checks immediately); a broken file keeps the last good state.

## Settings

`AudioSettings` in the project file (`"audio"`): bus volumes/mute, occlusion on/off and strength,
`occlusionRays`, `occlusionRaysPerSource`, `occlusionSpread`, per-bus voice limits and mode,
snapshots, ducking rules. `ApplicationDesc::audio` (`AudioEngineDesc`), `Application::GetAudio()`.

## Blueprint nodes

Play Sound 2D (bus parameter), Play Sound at Location (location, volume, pitch, bus), Play/Stop
Audio Source (fade out), Is Audio Playing, Set Audio Volume/Pitch, Set Bus Volume,
Activate/Deactivate Audio Snapshot (fade), Get Audio Snapshot Weight. Sound paths are relative to the
working directory, which is the project root in the editor and player.

## Editor

Inspector sections (browse, preview, all fields, play/stop and occlusion state while playing),
"Audio" viewport overlay (speaker icons, min/max spheres, reverb boxes), content types SND and CUE
(double-click previews or opens the Sound Cue window, drag into the viewport creates a source),
Sounds tab in the assets panel, renderer panel "Audio" (mixer, occlusion, voice limits, snapshots,
ducking with live levels; saved with the project), stats section.
