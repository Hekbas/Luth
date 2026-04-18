#pragma once

#include "luth/core/types/LuthMath.h"

#include <type_traits>

namespace Luth
{
    // Check if a type is a GLM vector
    template<typename T>
    struct IsGLMVector : std::false_type {};

    template<glm::length_t L, typename T, glm::qualifier Q>
    struct IsGLMVector<glm::vec<L, T, Q>> : std::true_type {};

    // Check if a type is a GLM matrix
    template<typename T>
    struct IsGLMMatrix : std::false_type {};

    template<glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
    struct IsGLMMatrix<glm::mat<C, R, T, Q>> : std::true_type {};
}
