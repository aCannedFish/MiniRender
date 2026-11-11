# 体积散射

A7.1 为 CPU 路径追踪加入有界均匀 RGB 介质。光线可在物体表面之间发生散射，阴影查询同时计算几何遮挡和介质透射率。当前通过场景 JSON、CLI 与独立例程使用；交互参数页由 A7.2 接入。

## 参数

`HomogeneousMedium` 保存吸收系数 $\sigma_a$、散射系数 $\sigma_s$ 和 HG 参数 $g$。系数采用 RGB，单位为 $m^{-1}$，消光系数为 $\sigma_t=\sigma_a+\sigma_s$。三组系数使用 double。

`MediumRegion` 定义介质所在的世界空间轴对齐盒。最多支持 32 个区域，启用区域的内部互不重叠，共用边界可以相接。区域参数和启用状态进入场景历史、文件及只读快照。

区域边界负责划分参与介质所在空间，表面材质负责几何界面上的反射与折射。区域内的光线路段使用该区域的系数。

每条射线先与区域裁剪，得到按进入距离排序的正长度区段。主射线使用相机近、远裁剪距离，阴影射线使用实际光源端点。平行且落在盒边界上的射线按外侧处理，切点贡献的介质长度为零。

## 自由程

均匀介质的透射率为 Beer–Lambert 定律：

$$T_r(d)=\exp(-\sigma_t d).$$

自由程采样采用均匀 RGB 通道混合，见 [PBRT3 §15.2.1](https://pbr-book.org/3ed-2018/Light_Transport_II_Volume_Rendering/Sampling_Volume_Scattering)。先选择通道 $k$，再以该通道的 $\sigma_{t,k}$ 抽指数距离。消光为零的通道总是逃逸。

若距离 $d$ 在当前区段长度 $\ell$ 内，采样密度及路径权重为

$$p(d)=\frac13\sum_{c\in RGB}\sigma_{t,c}T_{r,c}(d),\qquad
w(d)=\frac{T_r(d)\sigma_s}{p(d)}.$$

若未在区段内产生事件，离散逃逸概率与权重为

$$P_{escape}=\frac13\sum_{c\in RGB}T_{r,c}(\ell),\qquad
w_{escape}=\frac{T_r(\ell)}{P_{escape}}.$$

`sample_medium_distance` 返回距离、事件标记和完整 RGB 权重。连续距离密度的单位为 $m^{-1}$，逃逸概率为无量纲。选中的通道决定采样位置，输出权重始终包含全部 RGB 通道。

代码在 log 空间计算混合密度和权重比。某个通道的指数透射下溢时，其余通道仍能保留有效贡献。`optical_depth` 单独处理零消光，令其与无限距离的乘积对应零光学厚度。

`MediumField::sample` 将各区段的逃逸权重相乘，并返回第一个体积事件。纯吸收区段直接乘解析透射率。真空区段不消耗随机数，因此零密度场景保留原有路径图像和随机序列。

## 相函数

Henyey–Greenstein 模型来自 [Henyey 与 Greenstein 1941，式 (2)–(3)](https://articles.adsabs.harvard.edu/pdf/1941ApJ....93...70H)。本项目将散射概率和吸收分配到自由程权重，相函数本身归一化为 1。

与 BSDF 保持一致，$\omega_o$、$\omega_i$ 都离开散射点。这一约定下，HG 的立体角密度为

$$p(\omega_o,\omega_i)=
\frac{1-g^2}{4\pi\left(1+g^2+2g\,\omega_o\cdot\omega_i\right)^{3/2}}.$$

正 $g$ 的前向方向沿原射线传播方向 $-\omega_o$。方向约定与采样形式参见 [PBRT4 §11.3](https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions)。

实现以 $-\omega_o$ 为局部 Z 轴，采样前向夹角余弦 $c$。对 $g\ge0$，逆 CDF 经过代数化简为

$$c=1-\frac{2(1-g)^2(1-u)(1+gu)}{(1-g+2gu)^2}.$$

该式在 $g=0$ 时连续退化为 $c=2u-1$。负 $g$ 使用对应的对称表达式。代码由此避免逆 CDF 中 $1/g$ 在接近零时造成的消去误差。前向散射很强时，PDF 通过保留分母中的 $(1-|g|)^2$ 来维持精度。

`PhaseSample::probability` 是完整的 $sr^{-1}$ 密度，与相函数值相同。相函数采样后的延续权重为 1。独立积分与采样测试分别检查归一化、$E[-\omega_o\cdot\omega_i]=g$、二阶矩和任意坐标系下的方向均值。

## 路径积分

`PathTracer::trace` 首先求最近表面，再以该距离和当前射线区间为上限采样介质：

1. 乘上本段介质权重。出现体积事件时，在该位置计算直接光并采样 HG 方向。
2. 体积顶点的 NEE 方向因子为相函数值，光源选择和立体角 PDF 继续由 `LightSampler` 提供。
3. 没有体积事件时，进入现有表面、发射或环境处理。
4. 表面和体积事件共同计入最大散射深度及俄罗斯轮盘赌，区域边界保留当前深度。

光源采样与方向采样继续使用 power heuristic。体积顶点将先前位置、方向 PDF 和连续事件标记保留到后续发射或环境命中。CLI 的 `bsdf` 模式在体积顶点使用相函数采样，`light` 与 `mis` 保持对应的光源与混合估计方式。方向光作为离散事件由直接光查询处理。

`PathStatistics::medium_scattering_events` 单独记录体积散射，`scattering_events` 记录全部实际延续事件。`RenderSession` 按工作线程归并它们，并保留原有进度、暂停、取消与固定种子语义。

## 阴影

`shadow_transmittance` 接收已经准备的射线。表面起点沿几何误差界偏移，体积起点使用采样位置；有限面积光的终点沿光源几何误差界偏移。

几何遮挡返回 RGB 零值。可见时，将阴影线段在每个介质区域内的长度转成解析 $T_r$ 并相乘。面积光的距离上限同时作用于 BVH 和介质裁剪，环境光与方向光使用无限射线，但只累积有界区域内的长度。

## 文件

在现有场景根节点加入 `media` 数组。旧文件缺省为空数组。系数和边界采用 double 往返，示例字段如下：

```json
{
  "media": [
    {
      "id": "fog",
      "name": "有界雾",
      "minimum": [-2, 0.01, -2],
      "maximum": [2, 2.8, 2],
      "absorption": [0.06, 0.12, 0.18],
      "scattering": [0.3, 0.3, 0.3],
      "anisotropy": 0.6,
      "enabled": true
    }
  ]
}
```

## 运行

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 4
build/macos-core-release/minirender_tests '[medium],[volume]'
build/macos-core-release/minirender_volume_example
```

例程在 `outputs/a7.1` 生成无介质、有界雾、Cornell Box 雾和透光孔光束场景，包含 JSON、PNG 与 EXR。每张图都保存重开并比较像素。`convergence.csv` 记录独立参考图下三个种子的采样误差。

已有 CLI 可直接渲染这些场景：

```sh
build/macos-core-release/minirender_cli --inspect outputs/a7.1/cornell-fog.json
build/macos-core-release/minirender_cli --render outputs/a7.1/cornell-fog.json \
  outputs/a7.1/cornell-cli --width 160 --height 160 --spp 128 --depth 12 \
  --seed 101 --threads 4
```

Windows 核心使用对应的 `windows-core-release` preset 与 `.exe`。Qt/Vulkan 界面接入和设备验收按后续检查点执行。

## 阅读顺序

| 文件 | 内容 |
| --- | --- |
| `include/minirender/scene/medium.hpp`、`src/scene/medium.cpp` | 参数、区域和不重叠检查 |
| `include/minirender/render/medium.hpp`、`src/render/medium.cpp` | HG、自由程、裁剪与 RGB 权重 |
| `src/render/path_tracer.cpp` | 体积与表面顶点、NEE、MIS 和阴影 |
| `src/assets/medium_io.cpp` | JSON 编解码 |
| `tests/medium_tests.cpp` | 解析采样、极值、区域、文件与快照 |
| `tests/volume_path_tests.cpp` | 独立照明积分、多种子能量及后台任务 |
| `examples/volume.cpp` | 场景、保存重开、图像与收敛记录 |
