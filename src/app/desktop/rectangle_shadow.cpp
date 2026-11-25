// 每个分层采样点从六个方向观察场景。几何使用相同的深度管线和网格缓冲。
#include "rectangle_shadow.hpp"
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <numbers>

bool RectangleShadow::configure(VulkanViewport& viewport, const minirender::SceneSnapshot& scene,
                                glm::vec3 low, glm::vec3 high, int grid, std::uint64_t revision) {
    if (revision_ == revision && grid_ == grid) {
        return false;
    }
    if (revision != revision_) {
        std::vector<Caster>                   casters;
        std::vector<std::array<glm::vec3, 4>> corners;
        for (const auto& instance : scene.instances) {
            casters.push_back(
                {instance.mesh, instance.object_to_world, instance.material.double_sided});
        }
        for (const auto& lamp : scene.rectangles) {
            corners.push_back(lamp.corners);
        }
        const bool same = casters == casters_ && corners == corners_;
        if (!same) {
            cached_grid_ = -1;
        }
        casters_ = std::move(casters);
        corners_ = std::move(corners);
        if (same && grid_ == grid && layers_) {
            revision_ = revision;
            return false;
        }
    }
    const auto                 count = std::max<std::size_t>(1, scene.rectangles.size() * 54U);
    VkPhysicalDeviceProperties limits{};
    vkGetPhysicalDeviceProperties(viewport.physicalDevice(), &limits);
    if (count > limits.limits.maxImageArrayLayers) {
        throw std::runtime_error("矩形光深度图层数超出设备上限");
    }
    const bool allocate = count != layers_;
    if (allocate) {
        map_.release();
        layers_ = static_cast<std::uint32_t>(count);
        map_.create(viewport, resolution, layers_);
        matrices_.create(viewport.device(), viewport.physicalDevice(), sizeof(glm::mat4) * count,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    grid_     = grid;
    revision_ = revision;
    if (allocate) {
        cached_grid_ = -1;
    }
    if (!grid || cached_grid_ == grid) {
        return allocate;
    }
    cached_grid_ = grid;
    std::vector<glm::mat4>         projections(count, glm::mat4(1));
    const std::array<glm::vec3, 6> axes{
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    const std::array<glm::vec3, 6> up{
        {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}}};
    std::size_t layer = 0;
    for (const auto& lamp : scene.rectangles) {
        for (int y = 0; y < grid; ++y) {
            for (int x = 0; x < grid; ++x) {
                const auto sample =
                    lamp.corners[0] +
                    (lamp.corners[1] - lamp.corners[0]) * ((float(x) + .5F) / float(grid)) +
                    (lamp.corners[3] - lamp.corners[0]) * ((float(y) + .5F) / float(grid));
                const float far = std::max(
                    .001F,
                    glm::length(glm::max(glm::abs(high - sample), glm::abs(low - sample))) * 1.02F);
                auto projection =
                    glm::perspectiveRH_ZO(std::numbers::pi_v<float> * .5F, 1.F, far * .0002F, far);
                projection[1][1] *= -1.F;
                for (std::size_t face = 0; face < 6; ++face) {
                    projections[layer++] =
                        projection * glm::lookAtRH(sample, sample + axes[face], up[face]);
                }
            }
        }
    }
    active_layers_ = static_cast<std::uint32_t>(layer);
    matrices_.write(projections.data(), projections.size() * sizeof(glm::mat4));
    dirty_ = layer > 0;
    return allocate;
}

void RectangleShadow::render(VkCommandBuffer                                        command,
                             const std::function<void(std::uint32_t, std::size_t)>& draw) {
    map_.prepare(command);
    if (!dirty_ || !grid_) {
        return;
    }
    for (std::uint32_t layer = 0; layer < active_layers_; ++layer) {
        map_.begin(command, layer);
        draw(layer, layer / static_cast<std::uint32_t>(6 * grid_ * grid_));
        vkCmdEndRenderPass(command);
    }
    dirty_ = false;
    ++updates_;
}

void RectangleShadow::release() {
    map_.release();
    matrices_.release();
    casters_.clear();
    corners_.clear();
    cached_grid_   = -1;
    active_layers_ = 0;
    layers_        = 0;
    grid_          = 0;
    dirty_         = false;
    revision_      = ~std::uint64_t(0);
}
