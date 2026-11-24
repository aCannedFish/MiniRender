// GGX 方向反照率与 Kulla–Conty 补偿，供材质求值、IBL 表生成和数值对照使用。
#pragma once
#include <glm/vec2.hpp>
#include <minirender/core/image.hpp>

namespace minirender {
// 三种估计使用相同的 Smith GGX。split_sum 将已有的 Fresnel 两项 A 和 B 相加。
enum class EnergyMethod { uniform, visible_normal, split_sum };
// 返回白色导体的单次反射能量。mu 是观察方向与法线的夹角余弦。
double integrate_directional_energy(double mu, double roughness, std::uint32_t samples,
                                    std::uint64_t seed, EnergyMethod method);
// 为当前粗糙度行积分 2∫E(μ)μ dμ，将值写入该行蓝色通道。红绿通道存放 A、B。
void store_average_energy(Image& table, std::size_t row);
// 共享的只读能量表，在首次使用补偿材质时生成。
const Image& conductor_energy_table();
// 双线性查询 E(mu,roughness) 和该粗糙度的余弦加权平均值。
glm::dvec2 sample_energy(const Image& table, double mu, double roughness);
// 返回补偿瓣的 RGB BRDF（立体角的倒数），接受入射和出射方向的单次能量。
glm::dvec3 multiple_scattering(glm::dvec3 f0, double ei, double eo, double average);
} // namespace minirender
