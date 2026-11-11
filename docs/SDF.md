# 距离场

A5.3a 提供距离场核心、独立文件和 CPU 参照图。A5.3b 将接入 Vulkan 独立视图与交互面板。

## 数据

`SdfDocument` 保存图元、CSG 引用、根节点、相机、方向光和步进参数。单份文档最多 64 个节点，节点以唯一 ID 写入 JSON。加载后解析 ID 引用，检查缺失引用和循环，再按后序编译。编译结果只包含根节点可达部分，同一个子图只编译一次。

支持以下图元和操作：

| 类型 | 参数 | 距离或组合 |
| --- | --- | --- |
| 球 | 半径 $r$ | $\lVert p\rVert-r$ |
| 盒 | 三轴半长 $b$ | $q=|p|-b$，$\lVert\max(q,0)\rVert+\min(\max(q_x,q_y,q_z),0)$ |
| 圆环 | 主半径 $R$、管半径 $r$，绕 Y 轴 | $\lVert(\lVert p_{xz}\rVert-R,p_y)\rVert-r$ |
| 并集 | 左、右子节点 | $\min(d_a,d_b)$ |
| 交集 | 左、右子节点 | $\max(d_a,d_b)$ |
| 差集 | 左、右子节点 | $\max(d_a,-d_b)$ |

图元使用平移、`Rz * Ry * Rx` 旋转和正均匀缩放。求值时将世界位置变换到图元局部，再将局部距离乘以缩放量，保持世界单位。差集切面沿用被减图元的颜色；距离相同时固定选择左侧，避免颜色随求值顺序变化。

图元返回精确的有符号距离，内部为负。min/max 组合保持 1-Lipschitz 距离界，允许 sphere tracing 使用其绝对值步进。CSG 的组合值在部分区域会低估到边界的实际距离。

## 步进

`trace_sdf` 接收单位方向射线。先与根节点的保守 AABB 相交，得到有限搜索区间。AABB 按命中容差扩张，避免剪掉边界附近的有效样本。每次求值后：

1. 若 $|d|\leq\epsilon_{abs}+\epsilon_{rel}t$，记录命中。
2. 以 $t\leftarrow t+s|d|$ 前进，默认安全系数 $s=0.9$。
3. 越过搜索区间时记录未命中，达到步数上限时记录耗尽。

绝对距离允许射线从实体内部出发并找到出口。命中、未命中和耗尽分别报告。耗尽像素显示为品红，便于检查掠射线和过低的步数预算。

法线使用世界空间的六次中心差分。CSG 棱边可能没有唯一梯度；CPU 参照图对零梯度使用朝向观察者的方向，并单独累计次数。默认示例没有触发该回退。

## 参照图

`render_sdf` 使用确定性的针孔射线和像素中心采样。输出为线性 RGB，可直接保存 EXR，PNG 输出执行 sRGB 编码。

| 视图 | 显示 |
| --- | --- |
| Surface | Lambert 直接光、环境常量与方向光硬阴影 |
| Normals | 朝向观察者的法线映射到 0–1 |
| Depth | 观察空间深度 $z$ 映射为 $1/(1+z)$ |
| Steps | 主射线步数占预算的比例，由蓝到红；耗尽为品红 |

阴影从命中点沿法线偏移后朝光源步进，使用独立的阴影步数预算。达到该预算时按遮挡处理。主射线统计与阴影迭代分开。

CPU 渲染每 256 个像素报告进度，每个像素和每次步进检查取消。调用者持有文档快照，后台执行时无需读取界面控件。

## 文件

`load_sdf` 和 `save_sdf` 使用 `minirender-sdf` 格式，当前版本为整数 `1`。文档独立于三角网格场景，文件中包含所有图元、引用、相机和步进参数。渲染当前采用针孔模型；相机的孔径和焦距也保存在文件中。

输入限制为 1 MiB，节点 ID 最多 128 字节，名称最多 512 字节。步数必须为范围内的整数。保存使用暂存文件，通过验证并完成写入后再替换目标文件；取消和无效文档会保留原文件。

## 运行

macOS：

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 3
build/macos-core-release/minirender_tests '[sdf]'
build/macos-core-release/minirender_sdf_example
```

Windows 使用 `windows-core-release` preset，运行对应构建目录中的 `minirender_sdf_example.exe`。示例的文件路径和图像输出使用 `std::filesystem`，支持中文及空格。

示例在 `outputs/a5.3/core` 写出独立 JSON、Surface EXR 和四张 PNG。场景包含球减盒、旋转圆环和地面盒。保存后重新加载文档，再生成图像。

## 阅读顺序

1. `include/minirender/geometry/sdf.hpp`：图元、图、编译记录和追踪结果。
2. `src/geometry/sdf.cpp`：验证、后序编译、距离求值、包围盒和步进。
3. `src/assets/sdf_io.cpp`：ID 引用、格式检查和暂存保存。
4. `src/render/sdf_render.cpp`：针孔参照、差分法线、阴影及诊断视图。
5. `tests/sdf_tests.cpp`：解析距离、独立二次方程求交、成像和文件验证。
6. `examples/sdf.cpp`：文件往返、四种图像及解析统计。

## 参考资料

John C. Hart，1996，*Sphere Tracing: A Geometric Method for the Antialiased Ray Tracing of Implicit Surfaces*，The Visual Computer 12(10)，527–545，DOI `10.1007/s003710050084`。

- [作者机构的发表记录](https://experts.illinois.edu/en/publications/sphere-tracing-a-geometric-method-for-the-antialiased-ray-tracing/)。
- [Stanford 保存的作者预印本](https://graphics.stanford.edu/courses/cs348b-20-spring-content/uploads/hart.pdf)。实现参考其中的 Lipschitz 距离界、sphere tracing 迭代和图元/变换讨论。该文件包含预印本及封面，页码与期刊版本不同。
