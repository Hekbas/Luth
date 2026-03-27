#pragma once

#include "luth/core/Math.h"
#include "luth/scene/Entity.h"
#include "luth/core/UUID.h"

#include <entt/entt.hpp>
#include <functional>
#include <string>
#include <vector>

namespace Luth::Component
{
    struct ID {
        UUID m_ID;

        ID() = default;
        ID(const ID&) = default;
    };

    struct Tag {
        std::string m_Tag;

        Tag() = default;
        Tag(const Tag&) = default;
        Tag(const std::string& tag) : m_Tag(tag) {}
    };

    struct Parent {
        Entity m_Parent;

        Parent() = default;
        Parent(const Parent&) = default;
        Parent(const Entity& parent) : m_Parent(parent) {}
    };

    struct Children {
        std::vector<Entity> m_Children;

        Children() = default;
        Children(const Children&) = default;
        Children(const std::vector<Entity>& children) : m_Children(children) {}
    };

    struct Transform {
        glm::vec3 Position = { 0.0f, 0.0f, 0.0f };
        glm::vec3 Rotation = { 0.0f, 0.0f, 0.0f }; // Euler angles (degrees)
        glm::vec3 Scale    = { 1.0f, 1.0f, 1.0f };

        glm::mat4 LocalMatrix = glm::mat4(1.0f);
        bool IsDirty = true;
    };

    struct WorldTransform {
        glm::mat4 Matrix = glm::mat4(1.0f);
    };

    struct Camera {
        enum class ProjectionType { Perspective = 0, Orthographic = 1 };

        ProjectionType Projection = ProjectionType::Perspective;

        // Perspective properties
        float VerticalFOV = 45.0f;
        float NearClip = 0.01f;
        float FarClip = 1000.0f;

        // Orthographic properties
        float OrthographicSize = 10.0f;
        float OrthographicNear = -1.0f;
        float OrthographicFar = 1.0f;

        float AspectRatio = 16.0f / 9.0f;

        glm::mat4 ViewMatrix;
        glm::mat4 ProjectionMatrix;

        bool IsDirty = true;

        Camera() = default;
        Camera(const Camera&) = default;
    };

    struct MeshRenderer {
        UUID ModelUUID;
        uint32_t MeshIndex = 0;
        UUID MaterialUUID;
        bool isSkinned;

        // Tmp state for ImGui
        std::string modelNamePreview;
        std::string materialNamePreview;
    };

    struct Animation {
        Animation() = default;
        Animation(UUID uuid) : ModelUUID(uuid) {}
        UUID ModelUUID;
        i32 AnimationIndex = 0;

        // Playback state
        f32 CurrentTime = 0.0f;   // seconds
        f32 Speed = 1.0f;
        bool Playing = true;
        bool Loop = true;

        // Runtime (not serialized)
        u32 BoneBufferOffset = UINT32_MAX;  // SSBO base index
        bool BufferAllocated = false;
        f32 PreviousTime = 0.0f;            // for event crossing detection
        std::vector<Mat4> GlobalBoneTransforms;  // per-frame, for attachments/AABB
        AABB AnimatedAABB;                       // world-space animated bounds
        std::function<void(entt::entity, const std::string&)> OnAnimEvent;
    };

    struct BoneAttachment {
        Entity TargetEntity;              // Entity with Animation component
        i32 BoneIndex = -1;               // Resolved at runtime from BoneName
        std::string BoneName;             // Serialized; resolved to BoneIndex via Skeleton::FindBone
        Vec3 LocalOffset = Vec3(0.0f);
        Vec3 LocalRotation = Vec3(0.0f);  // Euler degrees offset in bone space
    };

    struct DirectionalLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        bool CastShadows = true;
        float ShadowBias = 0.005f;
        float ShadowOrthoSize = 200.0f;
        float ShadowDistance = 200.0f;
    };
    
    struct PointLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
        float Range = 350.0f;
    };
}
