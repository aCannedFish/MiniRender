# 点云重建

点云核心将无连接的位置与法线转换成三角网格。流程包括 PLY 读取、重复点合并、PCA 法线、方向传播、多半径 Ball Pivoting，以及普通网格资产输出。A5.1 提供核心 API 和完整命令行示例，交互面板由 A5.2 接入。

## 数据

`PointCloud` 保存局部位置与可选法线。坐标使用 float，距离、协方差、球心和旋转角使用 double。法线可以省略；已有法线须为有限非零方向，处理时归一化。

`load_point_cloud` 读取 PLY 1.0 的 ASCII、binary little-endian 和 binary big-endian。文件头决定元素顺序、属性顺序、标量类型和 list 布局。位置使用 x/y/z，法线使用完整的 nx/ny/nz；颜色、置信度、已有面连接及 range_grid 等其他记录会完整读取后跳过。读取器同时限制点数、记录数、标量数、文件和 header 大小，长 list 也参与预算和取消检查。

默认文件预算为 2 GiB、200 万点、800 万条记录、6400 万个标量和 1 MiB header。`save_point_cloud` 写位置与可选法线，默认使用 binary little-endian，也可指定另两种编码。写出采用同目录临时文件，提交前失败或取消会保留原目标。

精确重复的位置合并为首条记录，保留其法线及原始索引，+0 和 -0 视为相同坐标。法线结果的 `source_indices` 映射到输入点云；重建结果的同名字段将输出网格位置映射到本次重建输入。

## 法线

`PointIndex` 使用固定点集的平衡 k-d 树，提供最近 k 点与半径查询。叶子保存原索引，查询结果按“距离、索引”排序，保持可复现性。树构建、查询和排序均检查取消。

PCA 邻域包含中心点，默认取 16 点，可设为 3–128 点。先相对中心平移坐标，再计算邻域均值与协方差。对称 3×3 Jacobi 分解得到升序特征值，最小特征值对应的向量作为局部法线。近共线或缺少稳定最小特征方向的邻域会报告点索引，便于调整邻域或检查输入。

方向传播在邻域图上使用代价 $1-|n_i\cdot n_j|$。Kruskal 算法得到最小生成森林，每个分量选择最高 Z 点，将其法线朝向 +Z，再沿森林统一相邻方向。最高点法线的 Z 分量为零时，依次使用 Y、X 分量确定符号。设置中的距离上限同时限制 PCA 与传播图，分离的数据可形成多个分量。

`estimate=false` 使用已有法线；`orient=false` 保留独立方向；`flip=true` 在最后整体翻转。报告包含重复点数、定向分量、翻转数，以及最近邻间距的 P10、中位数和 P90。默认邻域工作预算为 1600 万条链接。

## 滚球

Ball Pivoting 从一个合法种子三角形开始。三个点定义外接圆，给定半径可以求出圆平面两侧的接触球。种子球位于三角形正面，并且内部没有其他点，三角形方向须与输入法线一致。

每条前沿边保存有向端点、原面对顶点、球心和活动状态。球沿共享边转动时，球心沿垂直于边的圆运动。候选点来自边中点的 $2r$ 邻域。接触事件按有向旋转角排序，选取首个可用接触；进入和离开球体的事件分别判断，近共球点允许合法的零角度扩展。

加入新三角形时检查重复面、共享边方向、边面数与顶点 link 的入/出度。反向前沿配对后成为内部边，相当于 join/glue。前沿可以绕孔汇合，也可以在同一点暂时出现多条开放链。完成后按共享边连通的面扇整理输出顶点；仍独立的面扇各自获得位置副本，`split_vertices` 报告额外顶点数。

半径必须为递增正数。每轮结束后，用新半径重新检查全部边界边与其原面对顶点，只有仍能形成合法空球的边才重新进入活动队列。后续轮次复用已有三角形，并继续搜索未使用点中的种子。

默认预算为 200 万个三角形、单次查询 4096 个邻点、2000 万次候选检查。候选预算包含种子组合、接触球与空球检查。预算失败或取消会结束任务，输入和已发布资产保持原值。

输出使用 `MeshData`，经过完整半边检查后返回。没有合法种子时 `mesh` 为空；有结果时报告面数、实际使用点数、未使用点、额外顶点、分量、边界环和每轮新增面数。孔洞通过这些统计直接显示。

## 半径选择

示例根据最近邻间距 P90 生成首轮建议，使用其 2、3、4 倍作为递增半径。也可显式传入半径列表。较小半径能够保留更细的局部采样，较大半径能够跨过更稀疏的区域。观察新增面数、未使用点和边界，再调整列表。

自建 5000 点环面使用建议半径后输出 10000 面，边界边为 0，Euler 数为 0。若从更小的 0.0372 米开始，三轮后仍保留 16 个小边界环。初始三角形会保留在后续轮次中，因此首轮半径也会影响最终连接。

## 运行示例

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release
ctest --preset macos-core-release --output-on-failure
build/macos-core-release/minirender_point_cloud_example
```

默认示例在自建环面上按面积采样 5000 个位置，清除法线与面连接，再执行 PLY 往返、PCA/MST 和多半径重建。结果写入 `outputs/a5.1`：

- `input.ply`：原始位置。
- `prepared.ply`：估计并统一方向的法线。
- `reconstruction.obj`：可继续进入几何处理的网格。
- `scene.json` 与配套资产：可在工作台打开的场景。
- `reconstruction.png/.exr`：128×128、32 spp、seed=101 的实际 CPU 渲染。

加载独立点云时可指定输出目录和半径：

```sh
build/macos-core-release/minirender_point_cloud_example \
  input.ply outputs/my-cloud 0.002,0.003,0.004
```

示例保留输入单位，展示场景通过物体变换将模型放至约两米范围。文件输出检查源路径，避免覆盖输入点云。保存重开后再次渲染，并核对线性 RGB 图像。

## 数据验收

实际 PLY 验收使用 [Stanford 3D Scanning Repository](https://graphics.stanford.edu/data/3Dscanrep/) 的 Bunny 数据，来源为 Stanford University Computer Graphics Laboratory。原始单次扫描采用 `bunny/data/bun000.ply`；`bun_zipper.ply` 的顶点用于另一组已有模型演示。两组数据分别记录。

下载入口为 [bunny.tar.gz](https://graphics.stanford.edu/pub/3Dscanrep/bunny.tar.gz)，本次归档 SHA-256 为 `a5720bd96d158df403d153381b8411a727a1d73cff2f33dc9b212d6f75455b84`。扫描文件和输出保留在本地 `work/data` 与 `outputs`，仓库提供实现、测试与复验命令。

原始扫描包含 40256 点及 range_grid，位置读取后重新估计法线，重建生成 77696 面。结果保留扫描边界，统计为 762 条边界边、19 个边界环、1058 个未使用点。详细指标见 [A5.1 验收](checkpoints/A5.1.md)。

## 阅读顺序

1. `geometry/point_cloud.hpp` 与 `assets/point_io.hpp`：数据、文件预算及入口。
2. `src/assets/point_io.cpp`：header、标量、端序和 list 解析。
3. `src/geometry/point_cloud.cpp`：验证、面积采样和精确重复点合并。
4. `src/geometry/point_index.*`：固定点集的空间查询。
5. `src/geometry/symmetric_eigen.*`、`point_normals.cpp`：PCA 与森林定向。
6. `src/geometry/ball_geometry.*`：接触球与旋转角。
7. `src/geometry/ball_pivot.cpp`：前沿、候选、拓扑、半径轮次与输出。
8. `tests/point_cloud_tests.cpp`、`tests/ball_pivot_tests.cpp`：独立几何检查与资产链路。
9. `examples/point_cloud.cpp`：文件、重建、场景和离线图像的完整调用。

## 参考资料

- Bernardini et al., 1999, *The Ball-Pivoting Algorithm for Surface Reconstruction*：[论文原文](https://www.cs.jhu.edu/~misha/Fall05/Papers/bernardini99.pdf)、[IBM 作者记录](https://research.ibm.com/publications/the-ball-pivoting-algorithm-for-surface-reconstruction)。
- Hoppe et al., 1992, *Surface Reconstruction from Unorganized Points*：[作者提供的原文](https://hhoppe.com/recon.pdf)。PCA 与邻域方向传播参考其中的讨论。
- Greg Turk, 1994, *The PLY Polygon File Format*：[原始规范](https://gamma-web.iacs.umd.edu/POWERPLANT/papers/ply.pdf)。
