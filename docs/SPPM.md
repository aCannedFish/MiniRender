# 渐进光子映射

A7.3 在共享场景、BSDF、光源和 BVH 上实现 CPU SPPM，并接入 `RenderSession` 与 CLI。相机每轮重新采样可见点，逐像素保留光子统计。Qt 积分器选择与参数页在 A7.4 接入。

方法依据 [Hachisuka 与 Jensen 2009 原文](https://cseweb.ucsd.edu/~henrik/papers/stochastic_progressive_photon_mapping.pdf) §3–4，以及 [PBRT3 §16.2](https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/Stochastic_Progressive_Photon_Mapping)。

## 一轮计算

1. 每个像素生成新的抖动相机路径。镜面和光滑材质继续采样方向，漫反射点或深度末端的光滑点成为本轮可见点。
2. 将可见点的半径盒登记到只读空间哈希。
3. 从光源发射指定数量的光子路径。在后续表面命中处查询附近可见点，累加功率与 BSDF 的乘积。
4. 归并线程结果，更新半径、通量和有效光子数，发布完整迭代图像。

可见点每轮替换，半径和累积统计按像素保留。相机路径在到达可见点前的反射、折射、纹理和布料外观继续使用原有模型。

## 发射功率

面积光按三角形面积采样位置，再按余弦半球采样方向。光子初始功率为

$$\Phi_0=\frac{L_e|n\cdot\omega|}{p_{light}\,p_A\,p_\omega}.$$

双面发射将选边概率计入方向 PDF。实现中约去余弦，得到有限的功率权重。射线起点使用已有几何误差界偏移。

环境光和方向光在覆盖场景包围球投影的圆盘上采样起点。盘面积计入位置 PDF，环境方向使用 `EnvironmentLight` 的分布。方向光使用固定传播方向，其方向概率为离散质量 1。

`PhotonEmission::flux` 已除以选择、位置和方向概率。最终估计除以全部发射试验数，漏出场景或零贡献的试验同样计入分母。

## 统计更新

原文 Eq8–11 给出共享统计的更新。设上一轮有效计数为 $N$，半径为 $R$，本轮找到 $M$ 个光子，方向加权功率和为 $\Phi$，则

$$N'=N+\alpha M,\qquad
R'=R\sqrt{\frac{N+\alpha M}{N+M}},$$

$$\tau'=(\tau+\beta_{camera}\Phi)\frac{R'^2}{R^2}.$$

本轮的功率和为 $\Phi=\sum_p f_{merge}(\omega_o,\omega_p)\Phi_p$。相机吞吐在更新时乘入。没有新光子时，$N,R,\tau$ 保持不变。

完成 $k$ 轮、累计发射 $N_e$ 条光子试验后，像素估计为

$$L=\frac{L_{direct,sum}}{k}+\frac{\tau}{N_e\pi R^2}.$$

面积密度估计已经包含光子到达表面的投影效应，合并时按 BSDF 加权。几何法线与着色法线不同时，代码另做下面的测度换算。

## 传输方向

相机路径使用 radiance 模式，光子路径使用 importance 模式。玻璃 importance 权重沿用已有实现，保持光子功率的折射传输约定。

设现有 PT 的方向因子为 $f_r(\omega_o,\omega_i)|n_s\cdot\omega_i|$。光子携带几何表面单位面积的功率，因此合并核为

$$f_{merge}=f_r(\omega_o,\omega_i)
\frac{|n_s\cdot\omega_i|}{|n_g\cdot\omega_i|}.$$

光子继续传播时，用下面的因子把着色法线权重换成 adjoint 权重：

$$C=\frac{|n_s\cdot\omega_o|\,|n_g\cdot\omega_i|}
{|n_g\cdot\omega_o|\,|n_s\cdot\omega_i|}.$$

这一换算可参见 [PBRT 的路径测量方程](https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/The_Path-Space_Measurement_Equation) 和 [Veach 1997 原文](https://graphics.stanford.edu/papers/veach_thesis/)。布料模型采用转置的辐射核计算 importance 权重，采样提议及其 PDF 保留原值。这样可以使用原有材质，而保持两个传输方向的一致性。

## 直接光

相机路径累计直接可见发射、环境和 NEE。继续光滑路径时，光源/方向采样使用互补 MIS 权重；终止为可见点时，直接光由光源采样计算。

光子首次落点属于直接光，密度估计从后续命中开始累加。经过玻璃等 delta 界面后到达表面的光子可以产生焦散。统计还记录光源侧含 delta、两侧都含 delta 的非零合并贡献，便于检查 SDS 场景。

最大深度统一限制整条光路。若可见点之前已有 $e$ 次相机侧散射、光子之前已有 $p$ 次光源侧散射，合并要求 $e+p+1\le d_{max}$。独立能量测试按相同深度预算检查结果。

## 空间哈希

格宽为本轮最大半径的四倍，每个半径盒最多覆盖八个格子。条目保存完整整数格坐标和可见点索引。查询先核对格坐标，再检查真实球距离，哈希碰撞不会重复累加同一可见点。

合并还检查实例和几何法线朝向，区分不同物体与反向表面。有限半径仍有边界模糊及核估计偏差，逐渐减小半径会减弱这些影响。

每个工作线程独立累加 $M$ 和 $\Phi$，归并顺序固定。随机流由 seed、迭代、像素或光子索引决定。线程数变化保留相同的路径集合，浮点归并允许小幅舍入差异。

## 后台任务

`RenderSession` 根据 `IntegratorKind` 选择 PT 或 SPPM。SPPM 每轮生成一份完整估计，Film 直接替换该图像，保留原有预览、导出和 job ID 语义。

暂停请求取消尚未提交的迭代，收回线程后进入 paused。恢复时从同一迭代随机索引重算。取消和异常均保留上一次完整统计与图像。进度同时提供当前相机、建表、光子和更新阶段，以及已完成迭代数。

默认 SPPM 工作内存预算为 512 MiB，分配前估算可见点、哈希、线程累积、持久统计及图像所需空间。场景资产和 BVH 的存储另计。高分辨率或较多工作线程超出预算时，任务返回具体原因。

按项目计划，当前 SPPM 使用针孔相机和表面传输。有活动参与介质或非零光圈时返回原因，可用已有 PT 渲染这些场景。

## 运行

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 4
build/macos-core-release/minirender_tests '[sppm]'
build/macos-core-release/minirender_cli --render scenes/cornell-materials.json \
  outputs/sppm/glass --integrator sppm --width 128 --height 128 --spp 128 \
  --depth 8 --photons 40000 --radius 0.12 --alpha 0.7 --threads 4 --seed 101
build/macos-core-release/minirender_sppm_example
```

SPPM 的 `--spp` 对应相机采样与完整迭代数。`--photons` 指每轮发射试验数，`--radius` 使用世界空间米，`--sppm-memory-mb` 调整工作预算。原 `--mode` 只用于 PT；SPPM 使用上面的固定分解。

例程在 `outputs/a7.3` 生成玻璃焦散、镜面观察焦散、场景文件、EXR/PNG、独立 PT 参考和多种子收敛 CSV。每张展示图都保存重开并核对结果。

## 阅读顺序

| 文件 | 内容 |
| --- | --- |
| `light_emission.cpp` | 光源发射与功率 PDF |
| `sppm_surface.*` | 材质、UV、两种传输方向与测度 |
| `sppm_grid.*` | 有界哈希存储与邻域查询 |
| `sppm.cpp` | 相机、光子、统计更新与原子迭代 |
| `render_session.cpp` 的 `run_sppm` | 预览、暂停回滚、恢复与取消 |
| `sppm_tests.cpp`、`sppm_energy_tests.cpp` | 功率、邻域、统计、任务和独立能量检查 |

渲染实现位于 `src/render`，公开接口位于 `include/minirender/render`，专项测试位于 `tests`。
