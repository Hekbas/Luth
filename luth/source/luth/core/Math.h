#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <assimp/matrix4x4.h>

namespace Luth
{
    inline Mat4 AiMat4ToGLM(const aiMatrix4x4& m) {
        return {
            {m.a1, m.b1, m.c1, m.d1},
            {m.a2, m.b2, m.c2, m.d2},
            {m.a3, m.b3, m.c3, m.d3},
            {m.a4, m.b4, m.c4, m.d4}
        };
    }

    inline Mat3 ConvertToNormalMatrix(const Mat4& modelMatrix) {
        return glm::transpose(glm::inverse(Mat3(modelMatrix)));
    }
}
