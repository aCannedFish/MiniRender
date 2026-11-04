# 网格处理

当前完成 A4.1 的半边拓扑和均匀 Loop 核心。两者位于 `geometry`，只依赖 CPU 数据、GLM 和取消标记。Qt 工具、位移、OBJ 导出、QEM、重网格及自适应细分按 [计划](../PLAN.md) 的后续检查点实现。

## 数据约定

`MeshData.positions` 存放物体局部坐标中的共享位置，单位为米。每个三角形的三个 `MeshCorner` 分别保存位置索引、UV 和法线。同一位置可有多个 UV 或硬法线。几何连接只由位置索引决定，不能从 GPU 展开的顶点重新猜测拓扑，也不能仅因坐标相同就焊接位置。

`HalfedgeMesh` 拥有只读连接数组，不持有输入引用。顶点和面索引沿用输入，面角 `c` 的内部半边索引为 `3*face+c`。边按首次出现的面角次序编号。算法不遍历哈希表来决定顺序，因此输出顺序不依赖哈希桶布局。所有索引从 0 开始，`invalid_mesh_index` 表示缺失引用。

每条边有两个方向相反的半边。半边记录起点 `origin`、反向 `twin`、同一面中的 `next/previous`、所属 `face/edge`。边界也有显式半边，其 `face` 为 invalid，`next/previous` 沿外侧边界环连接。顶点记录一条出边和相邻位置数；边界顶点优先记录边界出边，孤立位置没有出边。

从半边 `h` 绕起点移动到下一条出边，使用 `next(twin(h))`。一次旋转必须遍历该顶点的全部出边。这个检查能发现两个闭合壳体仅共享一个位置的非流形情况，仅检查每条边的面数无法发现它。

构造时拒绝空网格、非法索引、非有限数据、零法线、退化面、重复面、同向共享边、超过两个面的边，以及不连续的顶点面片扇。坐标重合但索引独立的面片仍视为独立分量。未使用的位置保留并单独计数，不参与表面 Euler 数。

## Loop 规则

每层从旧网格读取全部位置，再构造新网格，避免原地更新改变后续权重。每条边只新增一个共享位置，每个父三角形生成四个子三角形。原有位置先保留索引，新位置按边编号追加。

内部旧顶点 $p$ 有 $n$ 个邻点 $q_i$，使用原始 Loop 权重：

$$
\lambda_n=\frac38+\frac14\cos\frac{2\pi}{n},\qquad
\beta_n=\frac{\frac58-\lambda_n^2}{n},\qquad
p'=(1-n\beta_n)p+\beta_n\sum_i q_i.
$$

内部边端点为 $a,b$，两侧相对顶点为 $c,d$，新位置为

$$e'=\frac38(a+b)+\frac18(c+d).$$

边界旧顶点只使用两个边界邻点 $q_0,q_1$，边界新点取中点：

$$p'=\frac34p+\frac18(q_0+q_1),\qquad e'=\frac12(a+b).$$

实现以 double 计算相对位移和加权和，最后存回 float。内部度数为 4 时，中心系数为 $33/64$，度数为 6 时为 $5/8$。本实现没有采用 $3/(8n)$ 的简化邻点权重。

父面顺序为 $(a,b,c)$，边上新点为 $ab,bc,ca$，四个子面依次为 $(a,ab,ca)$、$(b,bc,ab)$、$(c,ca,bc)$、$(ab,bc,ca)$。这保持了组合拓扑中的面朝向。几何检查还要求每个子面面积向量在父面法线上的投影为正，拒绝局部折回的结果。该条件会拒绝法线转动达到或超过 90 度的子面，不等价于全局自交检测。

面积检查沿用资产层门槛：叉积的平方长度必须大于 $10^{-20}$，对应米制坐标中的面积平方量纲。过小的模型或过多层细分可能触及此门槛，报错包含层数和子面编号。

## 属性处理

UV 采用逐父面的线性规则：旧面角 UV 保留，边中点 UV 取该父面两个端点的平均值。接缝两侧可得到不同 UV，同时仍引用同一个新几何位置。该规则保留纹理图集，不对 UV 执行平滑 Loop，也不检查或修复 UV 三角形的重叠。

正层数细分后，按几何位置累积面积加权法线并归一化，跨 UV 接缝共享平滑法线。原硬法线随平滑操作丢弃。若合成法线长度不超过相邻面积长度和的 $10^{-12}$，算法报告无法定义稳定法线；不会使用任意方向填充。孤立位置不需要法线。

`levels=0` 仍检查输入拓扑，然后返回原来的 const shared_ptr，保留全部原属性。当前输出是有限层细分三角网格，没有再投影到数学上的极限面，也没有折痕或角点标签。

## 任务接口

`subdivide_loop` 同步执行并返回 `LoopResult`。其中 `mesh` 是不可变结果，`levels` 保存输入和每层的拓扑统计。函数从不修改输入，取消和异常均不向调用方返回部分网格。

`LoopSettings` 默认一层、最多 200 万个最终三角形；层数范围为 0–8，调用方可将预算设为 1–1600 万。算法先按 $4^k$ 检查最终面数，再分配输出，并检查 32 位位置/半边索引容量。面数预算不等于内存预算，大网格的输入、输出、旧/新拓扑和临时法线会同时占用内存，Qt 接入时仍需展示估算。

进度回调在调用线程触发。顶点、边、面、法线阶段每处理 1024 个单位报告一次，结束时报告实际总数。法线阶段包含面累积、逐位置归一化和面角写回。拓扑阶段以一次完整校验为单位，仅报告开始和结束；内部循环继续检查取消。回调可以请求取消，函数在回调返回后立即检查标记。默认无回调仍执行取消检查。

取消抛出 `OperationCancelled`，非法几何或设置抛出 `invalid_argument`，预算/索引超限抛出 `length_error`。回调抛出的异常直接传给调用方。UI 必须在 worker 调用该函数，再通过一次编辑事务发布结果。

## 阅读顺序

1. `include/minirender/scene/scene.hpp`：共享位置与面角属性。
2. `include/minirender/geometry/halfedge.hpp`、`src/geometry/halfedge.cpp`：连接、边界和流形检查。
3. `include/minirender/geometry/loop.hpp`、`src/geometry/loop.cpp`：权重、重建、属性及取消。
4. `tests/geometry_tests.cpp`：解析坐标、接缝、错误输入、取消与渲染集成。
5. `examples/loop.cpp`：在现有 studio 场景中替换资产并渲染。

## 运行示例

在仓库根目录运行：

```sh
cmake --build --preset macos-core-release
build/macos-core-release/minirender_tests '[geometry]'
build/macos-core-release/minirender_loop_example
```

示例保持相同相机、材质和光照，分别对共享 cube 资产执行 0、1、3 层细分。输出目录为当前工作目录下的 `outputs/a4.1`，包含各层的线性 EXR 和 Reinhard/sRGB PNG。固定参数为 128×128、32 spp、seed=101，积分器使用默认 MIS、最大深度 8 和 RR。

两个 cube 实例会共同使用新的不可变网格；地面不细分。平滑会收缩模型，位置和缩放保持原值，因此轮廓、物体与地面的间隙和投影都会变化。示例不改写源 OBJ 或场景文件。生成资产的保存与导出随 A4.2 接入。

Windows CPU CI 编译同一示例并运行算法测试；实际桌面与 NVIDIA 后端仍按 B 阶段验证。

## 参考资料

- Charles Loop, 1987, [Smooth Subdivision Surfaces Based on Triangles](https://www.microsoft.com/en-us/research/publication/smooth-subdivision-surfaces-based-on-triangles/)，University of Utah 硕士论文。[原文 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/thesis-10.pdf) 第 35 页的式 4.2–4.3 和第 47 页的式 4.31 给出中心/边点规则及原始权重。这里的 $\beta_n=(1-\alpha_n)/n$ 是对原文中心系数的改写。
- Pharr、Jakob、Humphreys, [PBRT 第三版 §3.8](https://www.pbr-book.org/3ed-2018/Shapes/Subdivision_Surfaces)，核对边界旧点的 1/8 权重与边中点规则。PBRT 的内部权重和最终极限面投影与本项目不同，阅读时需区分。
