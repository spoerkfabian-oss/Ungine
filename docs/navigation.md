# Navigation

`NavigationWorld` builds Recast/Detour navmeshes and moves DetourCrowd agents
(`engine/include/Engine/Navigation/`).

## Settings (`NavTypes.h`, project file `"navigation"`)

- `NavSettings`: cell size 0.2, cell height 0.1, tile size 48 cells, edge max length/error, region
  min/merge size, `maxTiles` 4096 tile layers per navmesh, `maxObstacles` 256, `maxAgents` 128 per
  profile.
- `profiles`: `NavAgentProfile{name, radius, height, maxClimb, maxSlope°}`, up to 8 — **one navmesh per
  profile**.
- `areas`: `NavAreaType{name, cost ≥ 1, color}`, up to 16; index 0 "Default" is fixed. Recast area ID =
  index + 1, poly flag = bit of the index. `kNavAreaBlocked` ("Blocked") removes the surface.
- `SanitizeNavSettings` clamps and makes names unique; `BuildHash` covers voxel/tile/profile/area-name
  settings (costs and colors do not need a rebuild).

## Components

- `NavMeshBounds{halfExtents}`: build volume (world AABB; spans outside every box are masked per cell).
- `NavArea{halfExtents, area}`: convex XZ hull of the rotated box + height range →
  `rcMarkConvexPolyArea` (ordered by UUID).
- `NavLink{start, end, radius, bidirectional, area, profiles}`: off-mesh connection (entity space).
- `NavObstacle{Box (yaw) / Cylinder, halfExtents/radius/height, enabled}`: TileCache obstacle that
  follows the entity.
- `NavAgent{profile, maxSpeed, acceleration, stoppingDistance, separationWeight, avoidance
  Off/Low/Medium/High, faceMovement, driveCharacter, areaMask}`.

## Sources

Colliders without trigger on static bodies or without a RigidBody (character and agent entities
excluded): boxes as 12 triangles, spheres/capsules as UV spheres (scaled by the largest axis like the
physics), mesh colliders from the model's collision data (waits for the model; failed = no geometry),
and terrains (triangles clipped per tile straight from the height field, holes left out). Each source
has a signature (world matrix + collider + mesh key) + world triangles + AABB, keyed by UUID. The
mesh key includes a content hash of the model's collision positions and indices (cached per model
revision, stable across sessions so bake files still match), so reloads that move vertices rebuild.

## Tiles and rebuilds

- Fixed origin (0, 0, 0), tile = tile size × cell size. Per profile: `dtTileCache` (uncompressed
  layers), `dtNavMesh`, `dtNavMeshQuery`, `dtCrowd`.
- `BuildTileLayers` (any thread) rasterizes the source triangles in the tile rectangle expanded by
  the agent radius + 3 cells, filters, builds the compact heightfield, erodes, masks by bounds, marks
  areas and builds heightfield layers. Jobs run on the ThreadPool (budget 2 × workers per update); a
  generation per tile drops stale results.
- Inserting a tile removes the old cache/navmesh tiles at (tx, ty), adds the layers, builds the navmesh
  tiles and re-adds obstacles on that tile. Links are passed in `MeshProcess` (stored in the start
  tile); link changes rebuild navmesh tiles from the cache only. Obstacles retry when the request queue
  is full (≤ 64 per update).
- `Update(scene, dt, simulate)` diffs all sources/areas/bounds/links/obstacles by signature every
  frame; changed or removed ones dirty the old + new AABB → tiles of all profiles (`autoRebuild`).
  `Build(scene)` rebuilds everything (blocking), `Flush(scene)` finishes pending work (blocking),
  `Reset()`, `SetSettings` (build-relevant changes rebuild all; costs only update filters).

## Bake files

`.unav` next to the scene (`NavMeshFileFor`: `Main.scene.json` → `Main.unav`): `UNAV` v1, build hash,
cache tiles per profile (sorted), then signature + AABB of every source (sorted, reproducible).
`LoadBaked` (through the VFS, so also from a pak) replaces the navmeshes and adopts the source state,
so only tiles of changed sources rebuild. Other settings or a broken file are refused with an error.
Links are re-added from the scene.

## Queries

All per profile with an area mask: `ProjectPoint` (extent 2/4/2 m), `FindPath` (corridor ≤ 512
polygons, straight path ≤ 256 corners, partial paths end at the nearest reachable point),
`RandomPoint`, `RandomPointInRadius` (up to 32 tries until the point itself is inside the radius),
`Raycast` (2D over the surface, hit + wall normal), `SetRandomSeed`.

## Crowd

- Agents are entities with `NavAgent`, added when they project onto the navmesh. Parameters: radius
  and height from the profile, collision query range 12 r, path optimization range 30 r, anticipate
  turns, visibility/topology optimization, avoidance quality 0–3, separation, one query filter per
  area mask (≤ 16 per profile).
- `MoveTo(entity, target, acceptanceRadius)`, `StopMovement`, `MoveStatus` Idle/Moving/Arrived/Failed
  (Arrived = XZ distance ≤ acceptance radius or stopping distance; Failed = target not on the navmesh,
  target failed, end of a partial path reached, or no agent), `AgentVelocity`, `HasAgent`.
- Movement: with a CharacterController + `driveCharacter` + physics the agent drives
  `SetCharacterInput` and the crowd position follows the character (while crossing an off-mesh link
  the entity is placed directly); otherwise the entity position and yaw are set directly. Entities
  moved externally by more than 5 cm are re-placed. `simulate = false` (editor edit mode) removes all
  agents from the crowd.
- Debug: `ForEachTriangle / ForEachBoundaryEdge / ForEachTile / ForEachAgent`, `Stats()`.

## Blueprint nodes (category AI)

AI Move To (latent, Success/Failed; calling again while waiting changes the target), AI Stop
Movement, Get AI Move Status (Moving, Velocity, Status), Find Path (points, found, complete, length),
Get Random Reachable Point (origin, radius 0 = anywhere), Project Point to Navigation, Navigation
Raycast, Set Agent Speed, Set Nav Obstacle Enabled. The profile is a text parameter (default
"Default").

## Editor and player

- Edit mode runs `Update(…, false)`: tiles follow every change in the background. Play runs
  `Update(…, true)`; pause and breakpoints stop it. Saving a scene flushes and writes the `.unav` (no
  NavMeshBounds → the old file is deleted); New/Open reset and load the bake file.
- **Navigation** window (overlay, tiles, auto rebuild, profile, Bake, status, test path via
  *Pick start/end* + click in the viewport, area legend, stats), viewport overlay "Nav", inspectors for
  all five components, Hierarchy "+ Add", Project Settings "Navigation" (costs/colors apply
  immediately, other changes via "Apply (rebuilds the navmesh)"). `.unav` files are hidden in the
  content browser and move/rename/delete with their scene.
- The player loads `NavMeshFileFor(scene)` per level, updates after scripts and resets on level
  changes. The cooker packs `.unav` raw.
- Example: the wandering fox in the Physics template (`Wander.ugraph`).
