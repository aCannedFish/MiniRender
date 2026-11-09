#pragma once

#include <glm/glm.hpp>

namespace minirender::detail {

// 实对称 3×3 矩阵的 Jacobi 分解。特征值升序排列，对应单位特征向量为矩阵列。
struct SymmetricEigen {
    glm::dvec3 values;
    glm::dmat3 vectors;
};
SymmetricEigen symmetric_eigen(glm::dmat3 matrix);

} // namespace minirender::detail
