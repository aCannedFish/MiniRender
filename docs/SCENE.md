# 场景数据

## 阅读顺序

1. `scene.hpp`：共享网格、面角属性、相机、材质、物体和只读快照。
2. `scene.cpp`：参数约束、坐标变换、相机射线与快照构造。
3. `obj_loader.cpp`：文件解析到项目网格的转换。
4. `scene_io.cpp`：JSON 格式与相对资产路径。
5. `scene_tests.cpp`：相机一致性、法线变换、UV 接缝和保存加载测试。

## 网格与实例

MeshData 的 positions 是共享几何位置，三角形的每个面角记录位置索引、法线和 UV。多个面角可以引用同一个位置但保存不同 UV 或法线。这让立方体保留 8 个拓扑顶点，同时仍能表现面与面之间的硬法线。

OBJ 由固定版本 tinyobjloader 解析并三角化。缺少法线时用三角形几何法线，保留平面外观；当前未实现 OBJ smoothing group 插值。文件中的 MTL 不用作项目材质，物体材质由场景 JSON 明确指定。

SceneObject 引用 mesh/material ID 并保存自身变换。一个网格可以被多个物体实例化；示例 studio 中两个方块共享同一 cube 网格。

## 坐标关系

变换矩阵为 T × Rz × Ry × Rx × S，列向量依次受到缩放、X/Y/Z 旋转和位移。位置用 mat4 变换，法线使用 mat3(M) 的逆转置，再归一化。非均匀缩放时直接使用 M 变换法线会破坏它与切线的垂直关系。

相机使用右手坐标系，朝向为 normalize(target − position)。生成射线时，uv 原点在左上角；Vulkan 投影也只进行一次 Y 翻转。测试将射线上的世界点重新投影，检查它回到原来的 uv，避免光栅化和路径追踪图像上下颠倒。

## 表面朝向

材质的可选字段 `double_sided` 控制表面朝向，省略时为 `true`，保留旧文件的双面行为。设为 `false` 后，只显示三角形绕序定义的正面。镜像缩放保留原外侧，法线插值不改变正反面判断。使用方法和管线说明见 [表面朝向](SURFACE_SIDES.md)。

## 保存与快照

布料与次表面散射在通用材质中引用不可变的专用参数。SSS 的 double 光学系数保存于 `subsurface` 字段，编辑、历史和渲染快照分别持有对应版本。CPU 的封闭 SSS 边界保留双面求交，实时视口采用表面近似。操作与数据说明见 [SSS 界面](SUBSURFACE_UI.md)和[次表面散射](SUBSURFACE.md)。

场景版本为整数 1。JSON 保存相机、环境、材料、模型引用、物体和方向光。相机可选的 aperture_radius/focus_distance 保存薄透镜参数；可选的 environment_map 引用环境纹理，旧场景使用针孔和常量环境。加载后检查 ID 唯一性、引用完整性、数值有限性、相机及变换是否合法。

模型路径相对于场景文件目录。另存到不同目录时，重算模型路径以继续指向原资产，不把个人绝对路径写入新场景。Windows 跨盘符时无法生成相对路径，因此将相关资产复制到新场景的 assets 目录；已有同内容文件复用，不覆盖不同内容的文件。JSON 文件名与资产路径通过 filesystem 的 UTF-8 转换支持中文及空格。

SceneDocument 可由 UI 编辑。make_snapshot 创建材质、变换、相机及灯光的值拷贝，并共享已发布的只读网格。隐藏物体不会进入渲染快照。任务开始后的 UI 修改不能改变该任务的输入。

## 当前验证

```sh
build/macos-core-debug/minirender_cli --inspect scenes/studio.json
```

预期为 2 个网格、3 个物体、3 个可见实例、26 个实例三角形。这条命令只加载和验证数据，不代表场景已经接入 Vulkan 渲染或 UI 编辑器。

数据层回归随 core CTest 执行。实时编辑见 [场景视口](VIEWPORT.md)，CPU 求交和积分见 [场景求交](INTERSECTION.md) 与 [路径追踪](PATH_TRACING.md)。
