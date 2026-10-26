#include "Engine/Scene/Decals.h"
#include "Engine/Scene/Components.h"
#include "Engine/Scene/Scene.h"

#include <vector>

namespace Engine {

void UpdateDecals(Scene& scene, float deltaTime)
{
    if (deltaTime <= 0.0f)
        return;
    std::vector<Entity> expired;
    scene.GetRegistry().ViewOf<Decal>().Each([&](Entity e, Decal& decal) {
        if (decal.lifetime <= 0.0f)
            return;
        decal.age += deltaTime;
        if (decal.age >= decal.lifetime)
            expired.push_back(e);
    });
    for (const Entity e : expired)
        scene.DestroyEntity(e);
}

} // namespace Engine
