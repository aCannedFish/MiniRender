# 路径追踪

## 运行

```sh
cmake --build --preset macos-core-release
build/macos-core-release/minirender_cli --render scenes/path-room.json outputs/room --width 160 --height 160 --spp 128 --depth 12 --mode mis --seed 101
```

命令保存 `room.exr` 和 `room.png`。EXR 保留线性 RGB，PNG 应用曝光、Reinhard 和既有 sRGB 编码。`--exposure` 只改变 PNG；路径累计不使用曝光值。输出名可带 `.exr` 或 `.png`，内部统一作为同一组结果的前缀。

可选参数有 `--width`、`--height`、`--spp`、`--depth`、`--seed`、`--mode`、`--exposure`、`--no-rr`、`--rr-depth`。模式为 `bsdf`、`light`、`mis`。Windows 使用 UTF-16 入口再统一转换为 UTF-8，场景和输出支持中文路径。

## 数据流

`PathTracer` 构造时固定 `SceneSnapshot`、`SceneBvh`、`LightSampler` 和积分设置。`trace` 计算一条射线的辐射，`sample_pixel` 按像素/样本编号派生随机流、生成相机射线，`render` 顺序累计图像。

单像素样本返回 double RGB，图像平均后转为项目的 float 图像。积分器检查非有限贡献，不对高亮样本做截断。统计量由调用方持有，包含路径、阴影射线、散射事件、RR 终止及几何测试次数。

`sample_pixel` 没有可变共享状态，后续 RenderSession 将复用它实现分块并行和任务控制。本检查点的 `render` 是单线程参考入口。

## 路径累计

迭代维护辐射和 $L$、路径吞吐 $\beta$、前次 BSDF 密度、前次顶点及 delta 标记。

到达表面时先累计自发光，未命中时累计环境。随后计算直接光照、采样 BSDF，并更新 $\beta\leftarrow\beta w_{BSDF}$。`maximum_depth` 表示允许的散射事件数；达到上限时保留该顶点的发射，停止进一步散射。

光源样本已经包含选择 PMF，连续 NEE 的贡献为

$$\Delta L=\beta\frac{f(\omega_o,\omega_i)\cos\theta_i L_i}{p_L}\,w_L.$$

可见性使用有限或无限阴影射线。几何法线检查阻止反射穿到物体另一侧。理想玻璃使用几何法线建立界面框架，保留 `front_face` 的介质侧信息；普通材质使用着色法线。

理想镜面和理想玻璃仅经 BSDF 采样延长路径。底色纹理使用既有过滤设置；当前离线采样显式使用 LOD 0，尚未加入射线微分。

## MIS

采用单样本 power heuristic，$w_a=p_a^2/(p_a^2+p_b^2)$，先缩放两个密度再平方。只有同一测度的连续样本参与这个比较。

| 模式 | NEE | BSDF 后续命中光源/环境 |
| --- | --- | --- |
| `bsdf` | 方向光等离散方向 | 权重为 1 |
| `light` | 所有可直接采样的光源 | 相机或前次 delta 后保留，其余由 NEE 估计 |
| `mis` | 连续样本用 $w_L$；方向光权重为 1 | 连续样本用 $w_B$，相机或 delta 后权重为 1 |

方向光无法由连续 BSDF 样本命中，因此三种模式都保留其 NEE。三种模式按相同次序分配随机维度，后续 BSDF 和 RR 的随机流不因模式改变。

面积光命中密度从前次顶点和当前命中计算，环境命中密度使用包含环境选择 PMF 的方向密度。原理参考 [Veach 与 Guibas，1995](https://www.cs.jhu.edu/~misha/ReadingSeminar/Papers/Veach95.pdf) 和 [PBRT 4 路径积分](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer)。

## 路径终止

RR 从指定散射深度后启用。存活概率由 $\max(\beta\,etaScale)$ 决定，并限制到 $[0.05,0.95]$；存活后将吞吐除以概率。

玻璃透射的 Radiance 权重含 $(\eta_i/\eta_t)^2$，`etaScale` 累乘样本的 $(\eta_t/\eta_i)^2$，避免仅因进入高 IOR 物体而过早终止。有限最大深度保留为独立预算限制。

测试用两片发光漫反射平面比较 RR 开启前后的均值，并检查散射工作量下降；深度 8、反射率 0.5 的参考和为 $2-1/256$。

## 数值处理

有限阴影线段的测试在旧 float 射线上出现 476/1000 次错误终点遮挡。CPU 射线、重心和命中位置已改为 double，网格/GPU 缓冲保持 float。光源样本保留终点、法线与误差，阴影线段两端均向外偏移，再把终点距离向内舍入一个 double ULP。

命中位置通过重心插值重建，并保存逐轴误差。偏移距离为几何法线绝对值与误差向量的点积。零误差平面额外使用远低于 float 几何间隔的数值下界，避免非正规数乘法下溢。

三角形查询传播剪切、边函数与距离运算误差，排除无法与起点零距离区分的候选。仅移动射线起点不足以消除这一类误差。相关推导见 [PBRT 4 三角形求交](https://pbr-book.org/4ed/Shapes/Triangle_Meshes)。测试覆盖缩放、$10^8$ 量级的场景平移、光源前后遮挡及多次反弹。

## 薄透镜

相机增加 `aperture_radius` 和 `focus_distance`，单位为米。焦距沿 forward 测量。先求针孔射线与焦平面的交点，再从圆盘孔径点射向该交点。孔径为零时严格返回针孔射线。

JSON v1 的两个字段可省略，分别默认为 0 和 4。测试验证不同孔径样本在同一焦平面汇聚，并检查保存重开。实时视口继续使用针孔；状态提示会说明环境图、景深和面光源照明的离线用途。模型参考 [PBRT 投影相机](https://pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models)。

## 阅读顺序

1. `path_tracer.hpp`：设置、统计、单路径与单样本接口。
2. `Camera::generate_lens_ray`：薄透镜几何关系。
3. `offset_origin`、`visible`：起点、终点和线段范围。
4. `PathTracer::trace`：发射、NEE、MIS、BSDF、RR 的迭代顺序。
5. `sample_pixel`、`render`：随机流、像素累计及 HDR。
6. `render_cli.cpp`：参数、Unicode 路径、输出与统计。
7. `path_tests.cpp`：解析参考、精度回归及多种子方差实验。
