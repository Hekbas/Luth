#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <assimp/matrix3x3.h>
#include <assimp/matrix4x4.h>
#include <assimp/quaternion.h>
#include <array>

namespace Luth
{
    // Assimp to GLM conversions
    inline glm::mat3 AiMat3ToGLM(const aiMatrix3x3& from) {
        return {
            {from.a1, from.b1, from.c1},
            {from.a2, from.b2, from.c2},
            {from.a3, from.b3, from.c3}
        };
    }

    inline glm::mat4 AiMat4ToGLM(const aiMatrix4x4& from) {
        return {
            {from.a1, from.b1, from.c1, from.d1},
            {from.a2, from.b2, from.c2, from.d2},
            {from.a3, from.b3, from.c3, from.d3},
            {from.a4, from.b4, from.c4, from.d4}
        };
    }

    inline glm::vec3 AiVec3ToGLM(const aiVector3D& v) {
        return { v.x, v.y, v.z };
    }

    inline glm::quat AiQuatToGLM(const aiQuaternion& q) {
        return { q.w, q.x, q.y, q.z };
    }

    // GLM to Assimp conversions
    inline aiMatrix3x3 GLMMat3ToAi(const glm::mat3& m) {
        return {
            m[0][0], m[1][0], m[2][0],
            m[0][1], m[1][1], m[2][1],
            m[0][2], m[1][2], m[2][2]
        };
    }

    inline aiMatrix4x4 GLMMat4ToAi(const glm::mat4& m) {
        return {
            m[0][0], m[1][0], m[2][0], m[3][0],
            m[0][1], m[1][1], m[2][1], m[3][1],
            m[0][2], m[1][2], m[2][2], m[3][2],
            m[0][3], m[1][3], m[2][3], m[3][3]
        };
    }

    inline aiVector3D GLMVec3ToAi(const glm::vec3& v) {
        return { v.x, v.y, v.z };
    }

    inline aiQuaternion GLMQuatToAi(const glm::quat& q) {
        return { q.w, q.x, q.y, q.z };
    }

    // Matrix operations
    inline glm::mat3 ConvertToNormalMatrix(const glm::mat4& modelMatrix) {
        return glm::transpose(glm::inverse(glm::mat3(modelMatrix)));
    }

    inline glm::mat3 Mat4ToMat3(const glm::mat4& m) {
        return glm::mat3(m);
    }

    // Transformation matrix generators
    inline glm::mat4 ComposeTransform(
        const glm::vec3& translation,
        const glm::quat& rotation,
        const glm::vec3& scale)
    {
        glm::mat4 t = glm::translate(glm::mat4(1.0f), translation);
        glm::mat4 r = glm::mat4_cast(rotation);
        glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
        return t * r * s;
    }

    inline void DecomposeTransform(
        const glm::mat4& transform,
        glm::vec3& translation,
        glm::quat& rotation,
        glm::vec3& scale)
    {
        glm::vec3 skew;
        glm::vec4 perspective;
        glm::decompose(transform, scale, rotation, translation, skew, perspective);
        rotation = glm::conjugate(rotation);
    }

    // Frustum culling
    struct Frustum {
        std::array<glm::vec4, 6> planes;
    };

    inline Frustum CreateFrustumFromCamera(const glm::mat4& viewProj, bool normalize = true) {
        Frustum frustum;
        const glm::mat4 matrix = glm::transpose(viewProj);

        frustum.planes[0] = matrix[3] + matrix[0]; // Left
        frustum.planes[1] = matrix[3] - matrix[0]; // Right
        frustum.planes[2] = matrix[3] + matrix[1]; // Bottom
        frustum.planes[3] = matrix[3] - matrix[1]; // Top
        frustum.planes[4] = matrix[3] + matrix[2]; // Near
        frustum.planes[5] = matrix[3] - matrix[2]; // Far

        if (normalize) {
            for (auto& plane : frustum.planes) {
                const float length = glm::length(glm::vec3(plane));
                plane /= length;
            }
        }

        return frustum;
    }

    inline bool IsInFrustum(const Frustum& frustum, const glm::vec3& point, float radius = 0.0f) {
        for (const auto& plane : frustum.planes) {
            const float distance =
                plane.x * point.x +
                plane.y * point.y +
                plane.z * point.z +
                plane.w;

            if (distance < -radius)
                return false;
        }
        return true;
    }
}
