#include "lepch.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"

// Canonical drawer registration order — matches the inspector layout today.
// Do NOT alphabetize; per-component order is user-visible.

namespace Luth::ComponentDrawers
{
    void RegisterComponentDrawers()
    {
    #if defined(LUTH_BUILD_DEBUG)
        RegisterID();
        RegisterParent();
        RegisterChildren();
        RegisterWorldTransform();
    #endif

        RegisterTransform();
        RegisterCamera();
        RegisterMeshRenderer();
        RegisterAnimation();
        RegisterBoneAttachment();
        RegisterAnimationController();
        RegisterDirectionalLight();
        RegisterPointLight();
        RegisterCollider();
        RegisterRigidBody();
        RegisterCharacterController();
    }
}
