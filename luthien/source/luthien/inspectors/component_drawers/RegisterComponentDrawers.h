#pragma once

namespace Luth::ComponentDrawers
{
    // Per-component register functions — each defined in its own .cpp.
    // DEBUG-only drawers live in DebugDrawers.cpp (stubs in Release).
    void RegisterID();
    void RegisterParent();
    void RegisterChildren();
    void RegisterWorldTransform();

    void RegisterTransform();
    void RegisterCamera();
    void RegisterMeshRenderer();
    void RegisterAnimation();
    void RegisterBoneAttachment();
    void RegisterAnimationController();
    void RegisterDirectionalLight();
    void RegisterPointLight();
    void RegisterCollider();
    void RegisterRigidBody();

    // Umbrella — calls each Register* in canonical order. Called once from
    // Editor::InitPanels.
    void RegisterComponentDrawers();
}
