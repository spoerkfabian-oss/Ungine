#pragma once

namespace Engine {

class Scene;

// Ages decals with a lifetime (Decal::lifetime > 0): the opacity fades out over the last
// `fadeOut` seconds, then the entity is destroyed. Call per frame while the game runs.
void UpdateDecals(Scene& scene, float deltaTime);

} // namespace Engine
