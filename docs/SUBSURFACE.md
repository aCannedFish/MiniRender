# 次表面散射

CPU 路径追踪支持均匀材质的 dipole 扩散 BSSRDF。适用对象是闭合、定向、连通且较厚的高散射网格。光可以从表面的一个位置进入，在另一位置离开，场景中的遮挡、光源、环境和外部介质继续参与传输。

模型采用 [Jensen 等人的 SIGGRAPH 2001 原文](https://graphics.stanford.edu/papers/bssrdf/)，实现其中的 dipole 扩散分量。物体页的参数、预置和实时近似操作见 [SSS 界面](SUBSURFACE_UI.md)。

## 参数

`SubsurfaceParameters` 保存 RGB 吸收系数、约化散射系数和密度。系数单位为 $m^{-1}$，约化散射已包含相函数的 $(1-g)$ 因子：

$$\sigma'_s=\sigma_s(1-g),\qquad
\sigma'_t=\sigma_a+\sigma'_s,\qquad
\alpha'=\frac{\sigma'_s}{\sigma'_t}.$$

密度同时乘到两种系数上。密度增大 $k$ 倍时，扩散距离缩短至 $1/k$，径向面积密度增大 $k^2$ 倍，总反射率保持不变。折射率沿用 `Material::ior`，范围为 1–3。

大理石预置来自原文 Figure 5 的测量表。原表使用 $mm^{-1}$，保存到引擎时乘以 1000：

| 参数 | R | G | B |
| --- | --- | --- | --- |
| 吸收 | 2.1 | 4.1 | 7.1 |
| 约化散射 | 2190 | 2620 | 3000 |

大理石折射率为 1.5。蜡预置使用项目自定的暖色展示参数，折射率为 1.44。SSS 的离线颜色由这些光学系数决定，通用材质中的 `base_color` 保留供其他材质类型使用。均匀 SSS 材质的颜色纹理字段须为空。

## 径向核

扩散系数、有效输运系数和实光源深度为：

$$D=\frac{1}{3\sigma'_t},\qquad
\sigma_{tr}=\sqrt{3\sigma_a\sigma'_t},\qquad
z_r=\frac{1}{\sigma'_t}.$$

内部漫反射边界系数为 $F_{dr}$，令

$$A=\frac{1+F_{dr}}{1-F_{dr}},\qquad z_v=z_r+4AD.$$

两个光源的 fluence 对边界法线求导，得到面积核：

$$R_d(r)=\frac{\alpha'}{4\pi}
\sum_{z\in\{z_r,z_v\}}
\frac{z(1+\sigma_{tr}d_z)e^{-\sigma_{tr}d_z}}{d_z^3},
\qquad d_z=\sqrt{r^2+z^2}.$$

核的单位是 $m^{-2}$。半径 $r$ 内的面积累计和总反射率有闭式表达：

$$C(r)=\frac{\alpha'}{2}
\sum_z\left[e^{-\sigma_{tr}z}-\frac{z}{d_z}e^{-\sigma_{tr}d_z}\right],
\qquad
\rho_d=\frac{\alpha'}2\sum_z e^{-\sigma_{tr}z}.$$

`DipoleProfile` 用该累计做归一和二分反演。小半径下以稳定形式计算 $d_z-z$，并使用 `log1p/expm1` 保留两个相近数相减后的有效位。零吸收时保留扩散核的幂律尾。

## 边界

外侧方向核为 $F_t(\mu)/\pi$。64 点 Gauss–Legendre 积分计算外侧余弦半球透射率：

$$H=2\int_0^1[1-F_r(\mu,1,\eta)]\mu\,d\mu.$$

折射互易关系给出内部逃逸比例 $1-F_{dr}=H/\eta^2$，从而得到上面的 $A$。折射率为 1 时透射率严格为 1。

完整扩散响应使用两个外侧边界因子：

$$S_d(p_o,\omega_o,p_i,\omega_i)=
[1-F_r(\omega_o)]R_d(\|p_i-p_o\|)
\frac{1-F_r(\omega_i)}{\pi}.$$

路径追踪按 $F_r(\omega_o)$ 选择界面镜面反射，按其补集选择扩散，扩散分支的概率抵消对应边界因子。两个端点均位于物体外部，位置跳转中没有显式折射射线段，因此不再叠加玻璃 BSDF 的 $\eta^2$ 因子。

## 表面采样

采样参考 [PBRT 的三轴投影方法](https://pbr-book.org/3ed-2018/Light_Transport_II_Volume_Rendering/Sampling_Subsurface_Reflection_Functions)。在出射点的几何法线、两个切线方向上分配 1/2、1/4、1/4 的轴概率，并混合三个 RGB 通道。

设实例世界包围盒的对角线长度为 $R$。同一实例上的两点间距不超过 $R$，因此以出射点为中心的该球覆盖整个物体。径向提议在 $[0,R]$ 上精确归一，响应仍使用原始 $R_d$。

通道选择概率取 $C_c(R)/\sum_kC_k(R)$。采样圆盘后，沿所选轴追踪球内线段，收集同一实例的全部唯一命中，再等概率选择一点。不同物体共享材质时仍各自采样。

对候选点 $p_i$，每个轴的圆盘半径为 $r_a$，对应线段命中数为 $n_a$。最终表面积 PDF 为：

$$p_A(p_i\mid p_o)=\sum_a q_a
\frac{|n_i\cdot a|}{n_a}
\frac{\sum_c R_{d,c}(r_a)}{\sum_c C_c(R)}.$$

每个轴分别计算自己的命中数，避免把所选轴的数量用于其他轴。三个通道使用同一几何范围，可共享轴查询。被选轴的结果直接复用，其余轴再查询。采样权重为 RGB 的 $R_d(\|p_i-p_o\|)/p_A$。

`SceneBvh::intersect_instance` 查询指定实例的双面几何。求交区间按位置误差向前推进，以合并共享三角边上的重复命中。每条探针最多保留 4096 个命中，超出时返回预算错误。

## 路径接入

`SubsurfaceSampler` 在后台准备阶段检查共享位置拓扑，要求一个闭合连通分量及朝外绕序。UV 和硬法线接缝保留为面角数据。采样在世界空间进行，支持旋转、镜像和非均匀缩放。

扩散分支采样新表面点后，在该点执行光源采样和余弦方向延续。方向核仍包含入射边界透射率，MIS 保持原有直接光与命中发射的互补权重。阴影射线复用几何遮挡和外部介质透射率。

一个边界反射或跨位置扩散算一次外部散射事件，消耗一个最大深度。`PathStatistics` 报告成功扩散次数和位置跳转总距离。非 SSS 材质保留原有随机维度次序。内部侧命中会返回错误，SPPM 对 BSSRDF 材质返回明确的不支持提示。

## 文件

材质 `kind` 为 `subsurface`，光学参数独立保存为 double：

```json
"subsurface": {
  "absorption": [2.1, 4.1, 7.1],
  "reduced_scattering": [2190.0, 2620.0, 3000.0],
  "density": 1.0
}
```

数据通过不可变共享对象进入快照，编辑时创建新的参数对象。撤销、重做和旧任务继续持有各自版本。

## 运行

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 4
build/macos-core-release/minirender_tests '[sss]'
build/macos-core-release/minirender_subsurface_example
build/macos-core-release/minirender_cli --render outputs/a7.5/marble.json \
  outputs/a7.5/marble-cli --width 128 --height 128 --spp 128 \
  --depth 8 --seed 101 --threads 4
```

例程保存大理石、蜡、较高密度蜡及两个方块侧面扩散场景的 PNG/EXR 和可编辑文件，并重开文件核对图像。另以独立面积积分为参考，记录多个种子的收敛 CSV。

## 阅读顺序

| 文件 | 内容 |
| --- | --- |
| `scene/subsurface.*` | 光学参数、单位和预置 |
| `render/dipole.*` | Fresnel 矩、径向核、面积累计和反演 |
| `render/subsurface.*` | 闭合检查、三轴投影、实例探针和混合 PDF |
| `render/subsurface_path.cpp` | 边界分支、新位置照明和方向延续 |
| `assets/subsurface_io.*` | double 参数读写 |
| `dipole_tests.cpp` | 独立 fluence 导数、积分、CDF 和缩放 |
| `subsurface_sampling_tests.cpp` | 非凸多命中、暴力 PDF 与世界面积积分 |
| `subsurface_path_tests.cpp` | 跨位置能量、MIS、文件和后台任务 |
