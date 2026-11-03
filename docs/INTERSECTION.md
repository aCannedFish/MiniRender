# 场景求交

## 数据流

`SceneBvh` 接收有效的 `SceneSnapshot`，持有该不可变快照的共享引用。构造时把可见实例的三角形位置变换到世界空间，记录实例和原三角形索引，再构建 BVH。法线、UV、材质和纹理仍从快照访问，不按光线重复拷贝资产。

构建所用的图元包围盒和重心保存在临时数组，构建结束后释放。运行时保留世界位置、原场景索引、重排索引及平坦节点数组。多个实例会各自保存世界位置，当前采用单层 BVH。

```cpp
const auto snapshot = minirender::make_snapshot(document);
const minirender::SceneBvh scene(snapshot);
const auto ray = snapshot->camera.generate_ray({0.5F, 0.5F}, 1.0F);
if (const auto hit = scene.intersect(ray)) {
    const auto& instance = scene.scene().instances[hit->instance_index];
    // hit 提供世界位置、法线、UV；instance 提供材质及纹理。
}
```

## 射线约定

射线为 $\mathbf p(t)=\mathbf o+t\mathbf d$，方向必须为单位向量，因此 $t$ 直接表示世界空间距离，单位为米。`RayQuery` 检查输入并预计算方向倒数、主轴及剪切系数。

查询使用闭区间 $[t_{min},t_{max}]$。最小值非负且有限，最大值允许为正无穷。默认最小值为 $10^{-5}$ 米；调用方可以按场景和射线用途覆盖它。数值误差范围内无法与零区分的起点命中会被过滤。求交层不移动射线起点，也不忽略发出射线的整个物体。

`intersect` 查找区间内最近命中，等距时按原场景图元顺序选择。`occluded` 在首次命中后返回，适合指定有限距离的阴影射线；它不插值法线或 UV。二者消费同一套包围盒和三角形查询。

## 基础求交

包围盒使用 slab 法：每个坐标轴给出一个允许的 $t$ 区间，三个区间与调用方区间相交。方向分量为零时，直接检查起点是否在对应 slab 内，避免 `0 * infinity`。边界距离通过 `nextafter` 向外舍入，保留接触盒面的射线。

三角形采用射线坐标系中的边函数形式。先平移顶点，使射线起点位于原点；把射线绝对值最大的方向置为 Z 轴，再执行剪切和缩放：

$$
x'=x-\frac{d_x}{d_z}z,\qquad
y'=y-\frac{d_y}{d_z}z,\qquad
z'=\frac{z}{d_z}.
$$

此时射线沿新的 Z 轴。三个二维边函数为

$$
e_0=x'_1y'_2-y'_1x'_2,\quad
e_1=x'_2y'_0-y'_2x'_0,\quad
e_2=x'_0y'_1-y'_0x'_1.
$$

边函数异号时，射线位于三角形外；三者之和为零时没有有效交点。否则，重心坐标为 $b_i=e_i/\sum_j e_j$，距离为 $t=\sum_i b_i z'_i$。边函数为零时仍允许命中，以覆盖相邻三角形的共享边。

实现使用 double，并以 FMA 补偿乘积相减的舍入。坐标变换与三条边函数参考 [Woop、Benthin、Wald，2013 原文](https://jcgt.org/published/0002/01/05/paper.pdf)；原文的单精度与双精度回退方案未在此采用。代码入口为 `RayQuery::intersect`。

## BVH 构建

每个节点保存子树图元的轴对齐包围盒。若光线不与该盒相交，整棵子树可以跳过。叶子保存重排索引中的连续范围，内部节点保存左右孩子索引。

构建在三个轴上分别把重心分到 12 个桶。候选划分的成本为

$$
C_{split}=C_T+C_I\frac{A_LN_L+A_RN_R}{A_P},\qquad
C_{leaf}=C_I N.
$$

$A_P,A_L,A_R$ 为父节点和两个子节点的盒表面积，$N_L,N_R$ 为子节点图元数。本实现令遍历和图元测试的相对成本 $C_T=C_I=1$。前后缀累积计算各桶分割处的面积与数量，选择低于叶子成本的划分。

含四个或更少图元、所有候选无收益、重心没有可分范围，或者深度达到 48 时直接生成叶子。深度限制保证遍历的局部栈有明确上界，不依赖模型分布来避免栈越界。遍历先访问较近的子盒，新的最近距离会裁剪后续区间。

SAH 及分桶说明参考 [PBRT 4，BVH 章节](https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies)。相关原始工作见 [Wald，2007](https://publications.sci.utah.edu/publications/wald07/fastbuild.pdf)。本实现为串行构建，没有接入并行构建或增量 refit。

## 命中数据

`SurfaceHit` 保存距离、世界位置、几何法线、着色法线、UV、重心坐标、实例/三角形索引及 `front_face`。

几何法线来自三角形位置。变换行列式为负时修正其方向，保留原网格外侧的含义。着色法线先按各面角使用逆转置变换并归一化，再按重心权重插值、归一化并对齐几何法线所在半球。若插值抵消到零，使用几何法线。

法线不自动转向射线。`front_face` 由 $\mathbf d\cdot\mathbf n_g<0$ 决定，后续介质折射可以据此判断进入或离开。UV 按原面角插值，保留纹理接缝。法线逆转置的中间运算使用 double，避免合法大缩放使 float 行列式溢出。

网格及展开的世界三角形位置仍采用 float。A2.3 将 CPU 射线、重心坐标和命中位置改为 double，并返回位置误差供后续射线偏移。若实例变换产生非有限位置，或使不同顶点在 float 精度下重合、三角形塌缩，构造会报错。

## 诊断入口

```sh
cmake --build --preset macos-core-release
build/macos-core-release/minirender_cli --trace-normals scenes/studio.json outputs/studio-normals.png
```

输出固定 256×256 的世界空间着色法线图，颜色为 $\mathbf n_s/2+1/2$，背景为黑色。PNG 按现有图像模块执行 sRGB 编码。相机近远裁剪面从视空间深度换算为对应射线的距离区间。命令同时报告 BVH 节点数、深度、构建/查询耗时和测试次数。

这是主射线几何诊断。BSDF、光源采样和路径积分在后续 A2 检查点接入。当前 UI 拾取继续使用 A1 路径，后台任务管理接入后再复用 BVH。

## 阅读顺序

1. `intersection.hpp`：射线区间、包围盒和三角形命中约定。
2. `intersection.cpp`：方向预计算、slab 与置换/剪切求交。
3. `bvh.hpp`：命中数据、只读场景与节点所有权。
4. `SceneBvh` 构造和 `build`：图元展开、临时数据、SAH 与叶子条件。
5. `trace` 与 `surface_hit`：遍历、最近/遮挡查询和属性插值。
6. `intersection_tests.cpp`：解析边界、独立参考和并发检查。
7. `cli.cpp` 的 `--trace-normals`：从场景加载到真实射线图像的完整入口。
