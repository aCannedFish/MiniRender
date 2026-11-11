# 布料模拟

A6.1 实现独立 CPU 模拟器和冻结网格。专用布料材质在 A6.2 接入，播放界面与动态 GPU 缓冲见 [布料播放](CLOTH_UI.md)。核心示例使用漫反射材质检查形状、阴影和资产保存。

## 模型

`ClothGrid` 通过 double 原点、两条不共线的边向量和行列数定义规则网格。全部物理计算使用世界空间米制坐标，状态位置和速度采用 double。索引为 `row * columns + column`，正面朝 `cross(span_u, span_v)`。两条边允许剪切，以支持场景实例变换后的静止网格。

每个单元划分成两个三角形。静止三角形的质量按三个端点均分，因此每个粒子的质量为

$$m_i=\rho_A\sum_{f\ni i}\frac{A_f^0}{3}.$$

$\rho_A$ 为面密度，$A_f^0$ 为静止面积。这样可以与三角形空气压力的分配保持一致。网格总质量等于静止面积乘以面密度。

| 弹簧 | 连接 | 默认刚度 |
| --- | --- | --- |
| 结构 | 相邻经纬点 | 200 N/m |
| 剪切 | 单元的两条对角线 | 100 N/m |
| 弯曲 | 经纬方向相隔两个点 | 5 N/m |

每条弹簧保存一次，力成对累加。设方向 $d=(x_b-x_a)/\lVert x_b-x_a\rVert$，静止长度为 $L$，则 a 端受力为

$$F_a=[k(\lVert x_b-x_a\rVert-L)+c(v_b-v_a)\cdot d]d,\qquad F_b=-F_a.$$

轴向阻尼系数 $c=2\zeta\sqrt{k\mu}$，$\zeta$ 是阻尼比，$\mu$ 是两端有效质量。固定端按逆质量零处理。弹簧长度为零时使用静止方向，避免方向归一化产生非有限值。

## 时间推进

默认模拟帧间隔为 $1/60$ 秒，每帧执行 64 个固定子步。每个子步先计算全部力，再更新速度和位置：

$$v^{n+1}=\left(v^n+hF^n/m\right)e^{-\lambda h},\qquad x^{n+1}=x^n+h v^{n+1}.$$

$\lambda$ 为全局速度阻尼，单位为 $s^{-1}$。碰撞响应在位置更新过程中执行。固定点始终保持静止锚点和零速度。GUI 播放时将按这一固定帧序列调度模拟。

构造时根据每个自由粒子的刚度、阻尼行和计算 $K$ 与 $C$ 的上界，选择子步满足 $Kh^2+2Ch\leq1$。子步不足时返回所需数量，供参数界面显示。默认 25×25 网格的估计下限为 52，使用值为 64。

`advance` 在状态副本上执行请求的帧数，成功后发布完整结果。取消、进度回调异常或计算错误都会保留调用前的状态。进度单位为已完成子步，长循环内部也检查取消。

## 外力

重力按粒子质量计算。风使用三角形的当前面积、单位法线和平均速度。设相对法向速度为 $q=(v_{wind}-\bar v)\cdot n$，三角形总压力为

$$F_{air}=\tfrac12\rho_{air}C_D A q|q|n.$$

该力均分到三个端点。翻转三角形绕序会同时改变 $q$ 和 $n$ 的符号，压力方向保持一致。布料与风同速时，相对空气力为零。

## 碰撞

碰撞体包括静止球和单面平面。平面法线指向允许停留的半空间，构造时归一化。球半径和平面距离均加上布料厚度。

每个粒子先处理已有穿透，再对剩余运动查找最早接触。球使用线段二次方程，能够检测起点和终点均在球外的穿越。平面使用带厚度的线段交点。响应去掉入射法向速度，并按库仑摩擦限制切向速度损失，再继续推进剩余时间。

每个子步最多处理八次接触事件，达到上限时停止该粒子并累计次数。初始穿透投影有独立的收敛检查。固定锚点落入碰撞体时拒绝该配置。

碰撞作用于粒子，模拟采用固定拓扑。自碰撞、撕裂和通用网格碰撞按计划不在当前范围内。

## 冻结

`freeze` 返回只读 `MeshData` 和对应变换。网格位置先减去 `grid.origin`，由实例平移恢复世界位置，保留 float 局部网格细节。UV 保持经纬参数，后续用于布料切向和纹理。

法线采用当前三角形面积加权。折痕处的方向相消时，选取面积最大的邻面方向。退化三角形会报错。冻结成功后，后续模拟不会修改该网格，已开始的离线渲染可继续使用旧快照。

冻结结果可以通过普通场景对象保存、重开、移动和旋转。示例完成了模拟、冻结、OBJ 资产保存、场景 JSON 和固定种子 CPU 渲染的完整流程。

## 运行

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 3
build/macos-core-release/minirender_tests '[cloth]'
build/macos-core-release/minirender_cloth_example
```

Windows 使用 `windows-core-release` preset 及对应目录中的 `.exe`。核心构建保持 Qt、Vulkan 和 CUDA 关闭。

示例输出位于 `outputs/a6.1`：

- `drape/frame-0.png`、`frame-18.png`、`frame-45.png`、`frame-90.png` 及相应 EXR：0、0.3、0.75、1.5 秒的落球状态。
- `drape/布料 冻结.json`：落球冻结场景及配套资产。
- `pinned/wind.png/.exr`：两角固定的迎风布料。
- `pinned/布料 冻结.json`：悬挂冻结场景及配套资产。

在应用中使用顶部“打开”加载这些 JSON，可继续编辑冻结物体。

## 阅读顺序

1. `simulation/cloth.hpp`：网格、参数、状态、统计和模拟器契约。
2. `src/simulation/cloth.cpp`：验证、质量与弹簧构建、外力、子步与状态发布。
3. `src/simulation/cloth_collision.*`：线段求交、最早事件和摩擦响应。
4. `src/simulation/cloth_mesh.cpp`：局部坐标、平滑法线、UV 和冻结对象。
5. `tests/cloth_tests.cpp`：解析振子、收敛、风压、碰撞、取消和冻结检查。
6. `examples/cloth.cpp`：模拟、普通场景资产和离线渲染调用。

## 参考资料

- [UC Berkeley CS184 Cloth Sim](https://cs184.eecs.berkeley.edu/sp21/docs/proj4)：规则网格及结构、剪切、弯曲弹簧的教学说明。
- Baraff 与 Witkin，1998，*Large Steps in Cloth Simulation*：[论文原文](https://www.cs.cmu.edu/~baraff/papers/sig98.pdf)、[作者机构记录](https://publications.ri.cmu.edu/large-steps-in-cloth-simulation)。可继续阅读其中的隐式积分和阻尼处理。
- Sadeghi 等，2013，*A Practical Microcylinder Appearance Model for Cloth Rendering*：[作者原文](https://cseweb.ucsd.edu/~henrik/papers/practical_microcylinder_appearance_model_for_cloth_rendering.pdf)。A6.2 的布料表面模型按此文推进。
