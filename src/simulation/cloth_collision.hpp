// 布料粒子与平面、球体的连续碰撞及接触修正。

#pragma once

#include <minirender/simulation/cloth.hpp>

namespace minirender::detail {

// 一个粒子的接触次数，以及步内是否已达到碰撞次数上限。
struct ClothContacts {
    std::size_t count   = 0;
    bool        limited = false;
};

// 在剩余时间中反复选择最早接触，更新速度后继续运动。最多八次事件，达到上限时停止粒子。
// 起点已在实体内部时先投影到外部；投影未收敛时抛错，由 advance 保留整帧旧状态。
ClothContacts move_cloth_particle(glm::dvec3& position, glm::dvec3& velocity, double seconds,
                                  double thickness, const ClothEnvironment& environment,
                                  CancellationToken cancellation);

} // namespace minirender::detail
