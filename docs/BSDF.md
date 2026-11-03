# 表面散射

## 接口约定

`Bsdf` 工作在局部坐标系，`wo` 和 `wi` 均离开表面，局部 +Z 面向 `wo`，要求 `wo.z>0`。`ShadingFrame` 提供右手正交坐标变换。积分器先根据命中法线建立框架；玻璃的入/出射状态另外通过 `front_face` 传入。

`evaluate` 返回连续 BSDF $f$，单位为 $\mathrm{sr}^{-1}$；`pdf` 返回立体角密度。`sample` 接收一个事件选择数及两个方向采样数，返回方向、概率、测度、事件类型、IOR 比和吞吐乘子。

连续事件的吞吐乘子为

$$w=\frac{f(\omega_o,\omega_i)|\cos\theta_i|}{p_\omega(\omega_i)}.$$

理想镜面和理想玻璃是离散事件，`evaluate/pdf` 返回零，`sample` 返回概率质量及已经消去 delta 项的吞吐乘子。调用者根据 `SampleMeasure` 选择 MIS 处理方式。

材质参数沿用现有 `Material`。底色纹理先经现有线性纹理系统求值，再传给构造函数；反射/透射颜色限制在 $[0,1]$。类不保存控件、可变文档或随机生成器。

## Lambert

漫反射为 $f=\rho/\pi$，采用余弦半球采样 $p_\omega=\cos\theta_i/\pi$，因此每个有效样本的吞吐乘子为 $\rho$。下半球的值与密度均为零。

## GGX

导体使用 Cook–Torrance 模型：

$$
f=\frac{F(\omega_o\cdot\mathbf m)D(\mathbf m)G_1(\omega_o)G_1(\omega_i)}
{4\cos\theta_o\cos\theta_i},\qquad
\mathbf m=\frac{\omega_o+\omega_i}{\|\omega_o+\omega_i\|}.
$$

$\alpha=roughness^2$。GGX 分布写为

$$D(\mathbf m)=\frac{\alpha^2}{\pi(m_x^2+m_y^2+\alpha^2m_z^2)^2}.$$

这个写法在低粗糙度时直接使用 X/Y 分量计算正弦平方。Smith 项采用分离的 $G_1$ 乘积，计算时先约去余弦，减少掠射角下的极小数相除。金属 Fresnel 为以底色作 $F_0$ 的 Schlick 近似，未新增复 IOR 参数。

采样采用 [Heitz 2018 的 GGX 可见法线算法](https://jcgt.org/published/0007/04/01/paper.pdf)。可见法线密度及反射 Jacobian 给出

$$
p_m=\frac{D(\mathbf m)G_1(\omega_o)|\omega_o\cdot\mathbf m|}{\cos\theta_o},\qquad
p_\omega=\frac{p_m}{4|\omega_o\cdot\mathbf m|}.
$$

映射可能产生下半球方向，此时返回零贡献空事件。概率不重新归一化，也不重试。对法向视线，有效概率为 $1/(1+\alpha^2)$；其余概率属于空事件。测试同时检查这两部分。

$\alpha\le10^{-6}$ 时使用理想反射极限，避免把无法稳定分辨的极窄波瓣继续当作连续密度。普通镜面使用固定反射颜色；光滑导体使用 Schlick 角度反射率。

## 理想玻璃

玻璃使用精确介质 Fresnel 和 Snell 定律，反射概率为 $F$，透射概率为 $1-F$，全反射时 $F=1$。法线朝向与介质侧分别保存，进入时 $\eta_i=1,\eta_t=IOR$，离开时反过来。

Radiance 模式下，透射吞吐乘子包含 $(\eta_i/\eta_t)^2$；Importance 模式不乘这一项，供后续光子路径使用。透射样本另存 $\eta_t/\eta_i$，便于积分器处理俄罗斯轮盘赌。反射的吞吐乘子为白色，底色作用于每次透射界面，不表示体内的 Beer–Lambert 吸收。

`roughness` 不改变当前理想玻璃模型。粗糙透射与色散不在基础材质范围内。相关传输因子参考 [PBRT 4 介质 BSDF](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF)。

## 验证

测试覆盖 `sample/evaluate/pdf` 一致性、互易性、低粗糙度和掠射角。GGX 法向视线的分桶频次与解析概率对照，倾斜视线的二维频次与独立立体角积分对照。$F_0=1,\alpha=1,\omega_o=N$ 的半球反照率与解析值 $1-\ln2$ 对照。

玻璃检查法向 4% 反射率、Snell 方向、全反射、折射率匹配、两种传输模式及进出界面的 IOR 因子抵消。`sampling_tests.cpp` 保留全部固定种子和统计预算。

阅读顺序为 `bsdf.hpp` 的方向/测度约定、`ShadingFrame`、Fresnel、GGX 函数、`sample`，再读测试。后续积分器负责世界坐标转换、几何法线半球检查、射线偏移和路径累计。
