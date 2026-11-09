#include "symmetric_eigen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace minirender::detail {

SymmetricEigen symmetric_eigen(glm::dmat3 matrix) {
    double scale = 0;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                throw std::invalid_argument("Symmetric eigen matrix must be finite");
            }
            scale = std::max(scale, std::abs(matrix[column][row]));
        }
    }
    if (scale == 0) {
        return {{0, 0, 0}, glm::dmat3(1)};
    }
    for (int column = 0; column < 3; ++column) {
        for (int row = column + 1; row < 3; ++row) {
            if (std::abs(matrix[column][row] - matrix[row][column]) > scale * 1e-12) {
                throw std::invalid_argument("Eigen matrix must be symmetric");
            }
        }
    }
    matrix /= scale;
    glm::dmat3 vectors(1);
    for (int iteration = 0; iteration < 64; ++iteration) {
        int p = 0;
        int q = 1;
        for (int a = 0; a < 3; ++a) {
            for (int b = a + 1; b < 3; ++b) {
                if (std::abs(matrix[a][b]) > std::abs(matrix[p][q])) {
                    p = a;
                    q = b;
                }
            }
        }
        const double cross = matrix[p][q];
        if (std::abs(cross) <= 1e-15) {
            break;
        }
        const double tau = (matrix[q][q] - matrix[p][p]) / (2 * cross);
        const double t = std::copysign(1.0, tau) / (std::abs(tau) + std::hypot(1.0, tau));
        const double c = 1 / std::sqrt(1 + t * t);
        const double s = t * c;
        const auto vp = vectors[p];
        const auto vq = vectors[q];
        vectors[p] = c * vp - s * vq;
        vectors[q] = s * vp + c * vq;
        matrix[p][p] -= t * cross;
        matrix[q][q] += t * cross;
        matrix[p][q] = matrix[q][p] = 0;
        for (int row = 0; row < 3; ++row) {
            if (row == p || row == q) {
                continue;
            }
            const double a = matrix[p][row];
            const double b = matrix[q][row];
            matrix[p][row] = matrix[row][p] = c * a - s * b;
            matrix[q][row] = matrix[row][q] = s * a + c * b;
        }
    }
    std::array<int, 3> order{0, 1, 2};
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return matrix[a][a] == matrix[b][b] ? a < b : matrix[a][a] < matrix[b][b];
    });
    SymmetricEigen result;
    for (int i = 0; i < 3; ++i) {
        result.values[i] =
            matrix[order[static_cast<std::size_t>(i)]][order[static_cast<std::size_t>(i)]] * scale;
        result.vectors[i] = glm::normalize(vectors[order[static_cast<std::size_t>(i)]]);
    }
    return result;
}

} // namespace minirender::detail
