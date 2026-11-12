# MiniRender 计划

更新日期：2026-11-02

## 目标

MiniRender 是面向图形学学习的小型渲染引擎，集成 GAMES101 大作业选定的 12 个选题。通过 Qt 桌面界面加载场景、处理模型、模拟布料、调整材质与光照、切换渲染算法并比较结果。

先在当前 Apple M3、24 GB 内存的 Mac 上完成可以实际运行与验证的功能，再切换到 Windows + NVIDIA GPU 完成 OptiX、目标平台适配和最终验收。CPU 算法、Qt 界面与 Vulkan 渲染代码保持同一实现，不维护按平台分叉的算法。

代码可读性与功能一起验收。每个选题交付实现代码、最小场景、测试、参考来源及学习笔记。核心算法自行实现，基础设施允许使用第三方库。项目按长期学习项目推进，不预设缺少工作时间依据的截止日期。

唯一远端仓库：<https://github.com/aCannedFish/MiniRender>。本文件记录目标与验收要求，表格中的“计划”不代表已经实现或测试通过。

## 平台划分

| 内容 | 本机阶段 | Windows / NVIDIA 阶段 |
| --- | --- | --- |
| C++ 核心、场景、资源、CLI | 完整实现与测试 | MSVC 编译、文件系统与数值回归 |
| Qt 工作台、属性面板、任务管理 | 完整实现与交互测试 | 高 DPI、路径、窗口生命周期与部署回归 |
| Vulkan 实时预览、SSAO、SDF | 使用 MoltenVK 运行与验证 | 原生 Vulkan 驱动复验、调试与性能测量 |
| CPU 路径追踪、MIS、体积、SPPM、SSS | 完整实现与测试 | 正确性与性能回归 |
| 网格处理、点云重建、布料 | 完整实现与测试 | 场景与算法回归 |
| 光场采集、保存与重聚焦 | CPU 采集，UI 重聚焦，完整验证 | 文件兼容和交互回归 |
| CUDA / OptiX 路径追踪 | 定义数据边界与能力查询，不认定 GPU 实现完成 | 编译、求交、路径积分、图像比较与显存测试 |
| 安装与打包 | 本机开发运行与资源定位验证 | Windows 发布目录及干净环境运行验证 |

11 个选题的功能验收可以在本机完成；第 12 个 OptiX 选题在 NVIDIA 环境完成。Windows 兼容性和 NVIDIA 驱动行为不能通过本机测试代替。GPU 加速不等同于依赖 NVIDIA：SSAO 和 SDF 使用 Vulkan 着色器，在本机通过 MoltenVK 运行。

当前已确认本机安装了 CMake、Ninja、Clang、Qt 6.11.2，Qt 构建启用了 Vulkan。标准安装位置尚未发现 MoltenVK；这不构成完整 SDK 搜索结果，也不代表 Vulkan 视口已经通过测试。A0 必须验证加载器、MoltenVK、着色器编译器、Validation Layers 和 Qt 视口的实际组合。

## 技术选择

| 部分 | 方案 |
| --- | --- |
| 语言 | C++20；CUDA 设备代码采用与所选工具链兼容的标准 |
| 构建 | CMake、Ninja、CMake Presets |
| 本机编译器 | Apple Clang，arm64 |
| Windows 编译器 | Visual Studio 2022 / MSVC，x64 |
| UI | Qt 6 Widgets，嵌入 QWindow 与自有 Vulkan 交换链 |
| 实时图形 | Vulkan 1.2 基础能力，GLSL 编译为 SPIR-V |
| 本机图形运行时 | Vulkan Loader + MoltenVK，经 Metal 执行 |
| Windows 图形运行时 | NVIDIA 原生 Vulkan 驱动 |
| CPU 渲染 | 自研 BVH、多线程采样及积分器 |
| NVIDIA 渲染 | CUDA + OptiX，独立可选构建目标 |
| 文档 | Markdown，公式使用 LaTeX |

Qt 负责窗口及平台 surface，项目管理 Vulkan device、交换链、同步、离屏渲染和管线。A0 实测发现 QVulkanWindow 呈现同步问题后改用薄 QWindow 适配层，依据见 docs/FOUNDATION.md。核心算法不得依赖 Qt 类型。只保留平台初始化、资源定位与 SDK 查找等必要适配。

本机保留 Vulkan portability 设备枚举，并按设备报告启用所需的 portability subset。启用功能前检查支持情况；不依赖几何着色器、硬件曲面细分、CUDA 互操作、Vulkan Ray Tracing 或厂商专有扩展。Loop 细分在 CPU 执行，SSAO 和 SDF 使用基础图形管线。线框调试采用三角形边线绘制，不强制要求非实心光栅模式。

基础库采用 GLM、Eigen、JSON 与模型/图像解析库、VMA、Catch2。它们用于数学、文件格式、显存分配及测试。BVH、BSDF、采样、网格算法、模拟、光场和 SDF 等选题内容不调用完整算法库代替。

Qt 使用平台原生安装包；其他可源码构建的依赖由统一清单固定版本或提交及校验值，不跟踪浮动 main 分支。A0 记录实际使用的 Qt、Vulkan SDK、MoltenVK、编译器版本。B0 再记录 Windows 驱动、CUDA、OptiX 的兼容组合。机器私有路径放入不提交的用户 preset。

OptiX 9.1 为初始目标，假设 NVIDIA 设备为 Turing 或更新架构，并满足 R590 或更新驱动要求。实际 GPU 尚未提供，B0 检查后记录结果；不把“有 NVIDIA 显卡”直接视为满足 SDK 要求。

### 构建隔离

规划以下独立配置：

- `macos-core-debug` / `macos-core-release`：CPU 核心、CLI、算法测试，无 Qt、Vulkan、CUDA 依赖。
- `macos-desktop-debug` / `macos-desktop-release`：增加 Qt、Vulkan 与 MoltenVK，OptiX 关闭。
- `windows-core-debug` / `windows-core-release`：MSVC 核心与算法回归，无 GPU 运行要求。
- `windows-desktop-debug` / `windows-desktop-release`：Qt 与原生 Vulkan，OptiX 关闭。
- `windows-optix-debug` / `windows-optix-release`：在桌面配置基础上启用 CUDA 与 OptiX。

CMake 按目标发现依赖；仅在 OptiX 开关开启时启用 CUDA 语言并查找相关 SDK。CPU-only 配置不包含 GPU 头文件，不链接 GPU 库。OptiX 关闭时能力查询返回不可用，UI 说明原因，不提供伪实现或静默后端替换。

## 工程结构

| 模块 | 职责 |
| --- | --- |
| core | 数学约定、随机采样、图像、日志、计时、任务控制 |
| scene | 对象、变换、资源引用、材质、光源、相机和只读快照 |
| assets | OBJ、PLY、纹理、场景文件读写与结果导出 |
| geometry | 半边网格、细分、位移、简化、重新网格化、点云重建 |
| simulation | 布料状态、弹簧、时间推进、碰撞与动态网格 |
| render | 共享参数及数学、CPU 积分器、Vulkan 实时渲染、OptiX 后端 |
| app | Qt UI、CLI、操作历史和任务调度 |
| tests | 单元测试、算法比较、图像回归和平台检查 |

CPU、Vulkan、OptiX 实现按目录和构建目标隔离。共享数据、参数含义和经过测试的数学，不强求所有执行代码共用。

### 接口约定

| 接口 | 用途 |
| --- | --- |
| SceneDocument | 可编辑场景，维护物体、资源引用与参数 |
| SceneSnapshot | 固定版本的只读场景，供后台任务使用 |
| MeshData / HalfedgeMesh | 分别用于渲染/导出和拓扑修改，显式转换 |
| BSDF | eval、sample、pdf，注明方向、测度及离散镜面事件 |
| Light / Camera | 光源采样及概率密度、相机射线生成 |
| Medium / BSSRDF | 参与介质与跨表面位置散射，独立于普通 BSDF |
| RenderSession | 启动、暂停、恢复、取消、进度与阶段图像 |
| GeometryOperation | 输入网格和参数，输出新网格、统计和诊断 |
| SimulationWorld | 固定时间步、单步、重置和当前网格导出 |

采用右手坐标系、Y 轴向上、米制、线性 RGB。集中说明相机方向、纹理原点、矩阵顺序和 Vulkan 投影转换。几何拓扑与面角 UV/法线分离，避免把纹理接缝当作几何开口。

场景保存为带格式版本的 JSON，资产路径相对场景目录。先支持 OBJ、PLY、PNG/JPEG/HDR；输出 PNG 和线性 HDR EXR。几何处理生成新资产，不覆盖原始输入。跨平台文件访问使用文件系统库与 UTF-8 转换，覆盖中文和空格路径测试。

## 交互设计

- 中央视口：Vulkan 实时预览、离线渐进图像、线框、法线、深度和 AO 等调试视图。
- 左侧：场景树与资产列表；右侧：变换、材质、光源、相机和算法参数。
- 底部：进度、日志、采样数、耗时与资源使用信息。
- 工具栏：模式切换、渲染控制、保存、加载与导出。
- 专项面板：几何操作、布料播放、光场重聚焦、独立 SDF 参数与结果比较。

支持轨道/自由相机、物体选中、参数编辑撤销重做。耗时几何操作、模拟任务和离线渲染在后台运行；Vulkan 窗口与相关调用按 Qt 线程和资源生命周期约定执行。结果通过安全队列或事件提交，不由后台线程直接操作控件。

渲染固定 SceneSnapshot。改变几何、材质、光源或相机后将旧结果标记为过期，重新开始时清空累积。曝光与色调映射作用于已存线性结果，无须重算光照。布料运动使用实时预览，离线渲染冻结当前帧。

CPU 与 OptiX 首版均经主机图像缓冲传给 Vulkan 显示。按批次更新并限制回传频率，不把 CUDA/Vulkan 共享显存作为项目依赖。

## 选题实现

| 选题 | 算法与边界 | 本机验收 |
| --- | --- | --- |
| Loop 与位移 | 半边 Loop、边界规则、真实高度位移；整体细分后增加基于位移/投影误差的层级自适应三角化和邻接过渡 | 轮廓及阴影改变，局部细分无 T 接点裂缝，显示层级和面数 |
| 简化与重网格 | 拓扑合法的 QEM 边折叠；分裂、折叠、翻边、切向平滑和投影回原表面的各向同性重网格 | 两操作独立运行，统计面数、边长与表面误差 |
| 布料 | 结构/剪切/弯曲弹簧，半隐式 Euler、阻尼、固定子步；固定点、重力、风、球和平面碰撞；表面微圆柱材质 | 播放、暂停、单步和重置；经纬方向影响高光；冻结帧可离线渲染 |
| SSAO | Vulkan 深度/法线、半球采样、随机旋转、双边滤波；作用于环境漫反射 | 查看原始 AO、滤波和合成结果，检查相机与窗口变化 |
| MIS | 光源/BSDF 采样、power heuristic、面积光、环境光、镜面事件与俄罗斯轮盘赌 | 三种采样模式比较，多种子检查均值和方差 |
| 体积散射 | CPU 均匀 RGB 介质、自由程采样、吸收、HG 相函数、多次散射与带介质透射率的阴影射线 | 有界雾与光束；零密度退化、Beer–Lambert 及边界测试 |
| 光场相机 | 二维相机阵列采集四维光场，保存线性子孔径图像与标定，平移叠加重聚焦 | 重开文件后调整焦平面/孔径，射线计数不增加 |
| OptiX | GPU 求交与加速结构；自研表面路径积分、基础 BSDF、MIS、环境光、薄透镜 | 本机仅验收接口隔离；全部设备算法与运行验收在 B 阶段 |
| 渐进光子映射 | CPU SPPM：相机可见点、光子发射、空间哈希、半径和通量渐进更新 | 玻璃焦散收敛，与 PT 对比；不重复计入直接光 |
| 次表面散射 | CPU 均匀材质 dipole 扩散 BSSRDF、表面位置采样、边界 Fresnel | 蜡/大理石的跨位置扩散，检查采样分布与收敛 |
| 点云重建 | 多半径 Ball Pivoting；PCA 法线及邻域方向传播 | 重建与原模型比较，显示孔洞/法线，导出后继续处理 |
| SDF | Vulkan 两个全屏三角形与 sphere tracing；球、盒、圆环、刚性变换、均匀缩放及布尔组合 | 独立视图交互修改，法线/阴影/步数热图，对照解析球求交 |

共享边的位移保持一致，不连续高度接缝给出诊断。几何操作支持定向流形三角网格，检查退化、翻面、边界及特征约束；无法达到目标面数时返回实际结果与原因。

布料不包含自碰撞、撕裂和通用网格碰撞。模拟保持固定拓扑，显示细分独立执行。介质限互不重叠的均匀区域；不加入异质、嵌套介质或体积光子映射。SPPM 首版采用针孔相机与表面散射。SSS 面向闭合、均匀、较厚且高散射的物体，不声称覆盖薄片或多层皮肤。

SDF 作为独立模式，不要求与三角网格共享阴影和离线求交。点云采样不足处允许保留孔洞，不能把不完整重建当作封闭模型交付。

### 后端能力

- Vulkan：基础 PBR、直接光照、方向光阴影、IBL、SSAO、布料表面光照、SDF 独立视图、HDR 显示与 FXAA。所有这些功能先在 MoltenVK 上实现。
- CPU：基础材质、MIS、薄透镜、布料材质、体积、SPPM、SSS 与光场采集。
- OptiX：基础材质、MIS、环境光、薄透镜和三角网格，包括布料冻结后的几何；不承诺本期迁移专门布料材质、体积、SPPM、SSS 或光场采集。
- 基础材质包括 Lambert、GGX Cook–Torrance 导体、理想镜面和理想介质反射/折射。参数与采样约定在 CPU/GPU 间保持一致。
- 实时近似在 UI 中标明；离线后端遇到不支持的算法或材质给出原因，不静默替换。完整 PT 不额外叠加 SSAO。

## 本机阶段

| 阶段 | 内容 | 退出条件 | 状态 |
| --- | --- | --- | --- |
| A0 基础 | CMake 目标隔离、CPU CLI、测试入口、Qt、Vulkan/MoltenVK 冒烟测试；固定依赖 | 无 CUDA 环境可构建；Vulkan 三角形、纹理显示、深度附件、窗口缩放运行；初始化无 Validation 错误 | 本机通过；见 docs/checkpoints/A0.md |
| A1 场景 | 场景/资产读写、相机、选中、参数编辑、保存恢复；实时深度、基础 PBR、阴影、HDR 与 FXAA | 可从文件加载和编辑场景，保存重开一致 | 本机通过；A1.1–A1.6、UI1–UI4 记录见 docs/checkpoints/ |
| A2 路径追踪 | BVH、多线程、基础材质、面积光、环境光、MIS、薄透镜、任务取消 | UI/CLI 结果一致，采样测试通过，保存线性参考图 | 本机通过；A2.1–A2.4b 记录见 docs/checkpoints/ |
| A3 实时效果 | 深度/法线缓冲、SSAO、滤波、IBL 与调试视图 | AO 各阶段可检查，窗口/相机变化无错误；通过本机 GPU 回归 | 本机通过；A3.1–A3.2b 记录见 docs/checkpoints/ |
| A4 几何 | 半边结构、Loop/位移、QEM、重网格、自适应细分 | 拓扑、接缝、误差检查通过，结果用于实时与 CPU 渲染 | 本机通过；A4.1–A4.5 记录见 docs/checkpoints/ |
| A5 重建与 SDF | 点云法线、Ball Pivoting、导出；SDF 实时管线 | 点云到网格链路完整，SDF GPU 图像及解析对照通过 | 本机通过；见 A5.1、A5.2、A5.3a、A5.3b |
| A6 布料 | 弹簧、子步、碰撞、风、表面材质及动态网格 | 固定初态可复现，冻结帧进入 CPU 渲染 | 本机通过；见 A6.1–A6.3 验收 |
| A7 高级光照 | 均匀体积、SPPM、dipole BSSRDF | 每项有独立场景、数值检查、参考图与收敛结果 | 本机通过；见 A7.1–A7.6 验收记录 |
| A8 光场 | 相机阵列、采集、文件、交互重聚焦 | 重聚焦不重新追踪，重开文件结果可复现 | 本机通过；见 A8.1 和 A8.2 验收记录 |
| A9 本机验收 | 综合展示、11 项功能清单、学习文档、回归报告和迁移说明 | 满足设备切换条件，建立 macos-baseline 标签并推送 | 本机通过；见 [A9 验收](docs/checkpoints/A9.md)与[迁移说明](docs/WINDOWS_HANDOFF.md) |

A0 先证明 Qt + MoltenVK 路线可用，不在完成大量 UI 后才验证图形运行时。如果 SDK 配置有问题，先修复；期间可推进独立 CPU 模块，但 A0 GPU 验收保持未通过，不把它悄悄移到 Windows 阶段。

本机 CPU 参考渲染采用低分辨率自动回归、离线高采样参考图两级预算。默认自动回归不超过 256×256，具体样本数和种子写入各场景清单。高采样结果分批生成并缓存，不要求 CPU 离线模式实时运行。光场采集前估算内存，分子视图写盘；不把完整光场无条件常驻内存。

### A2 检查点

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A2.1 求交 | AABB、三角形、SAH BVH、命中属性、遮挡与法线诊断 | 本机通过；见 docs/checkpoints/A2.1.md |
| A2.2 采样 | 随机采样、基础 BSDF、面积光及环境采样 | 本机通过；见 docs/checkpoints/A2.2.md |
| A2.3 积分 | 路径追踪、MIS 三模式、俄罗斯轮盘赌、薄透镜与 CLI 图像 | 本机通过；见 docs/checkpoints/A2.3.md |
| A2.4a 任务核心 | 多线程 RenderSession、暂停/恢复/取消、实际采样进度、快照与 CLI | 本机通过；见 docs/checkpoints/A2.4a.md |
| A2.4b 离线界面 | Qt 控件、Vulkan 显示、过期状态、导出及 UI/CLI 一致性 | 本机通过；见 docs/checkpoints/A2.4b.md |

### A3 检查点

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A3.1 SSAO | 半球采样、旋转核、双边滤波、环境漫反射合成、调试视图与同步验证 | 本机通过；见 docs/checkpoints/A3.1.md |
| A3.2a 预处理 | 漫反射卷积、GGX 粗糙度层、BRDF LUT、取消/进度及独立数值对照 | 本机通过；见 docs/checkpoints/A3.2a.md |
| A3.2b 实时 IBL | Vulkan 上传/采样、Qt 后台任务与缓存、环境图编辑及 CPU/GPU 对照 | 本机通过；见 docs/checkpoints/A3.2b.md |

内置场景 SCENES1 提供可编辑的 Cornell 与材质示例，通过工具栏快速覆盖导入，记录见 docs/checkpoints/SCENES1.md。

### A4 检查点

交互补充 UI4 修复地面共面闪烁并加入材质单/双面设置，记录见 docs/checkpoints/UI4.md。自动回归默认离屏运行。

交互补充 UI3 已通过：视口移动轴、旋转环、单次手势历史及相机协调，记录见 docs/checkpoints/UI3.md。重网格由 A4.4 接入，自适应细分由 A4.5 接入。

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A4.1 拓扑与 Loop | 半边/边界/流形检查、均匀细分、UV 接缝、进度/取消与 CPU 示例 | 本机通过；见 docs/checkpoints/A4.1.md |
| A4.2a 位移与导出 | 真实高度位移、接缝诊断、可取消 OBJ 导出、生成资产保存及 CPU 示例 | 本机通过；见 docs/checkpoints/A4.2a.md |
| A4.2b 几何界面 | Qt 后台几何工具、进度/取消、撤销、导出/保存与实时/CPU 资产替换验证 | 本机通过；见 docs/checkpoints/A4.2b.md |
| A4.3 简化 | QEM、合法边折叠、边界/属性保护、实际目标面数与误差报告 | 本机通过；见 docs/checkpoints/A4.3.md |
| A4.4 重网格 | split/collapse/flip、切向平滑、投影回原表面、边长分布与误差报告 | 本机通过；见 docs/checkpoints/A4.4.md |
| A4.5 自适应 | 位移/投影误差驱动的层级三角化、邻接过渡、无 T 接点裂缝及完整实时/CPU 验收 | 本机通过；见 docs/checkpoints/A4.5.md |

均匀/自适应 Loop、位移、QEM 与重网格已通过几何页进入实时预览、CPU 离线渲染和资产保存。

### A5 检查点

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A5.1 点云核心 | PLY、k-d 树、PCA/MST、多半径 Ball Pivoting、拓扑/预算/取消及文件与 CPU 示例 | 本机通过；见 docs/checkpoints/A5.1.md |
| A5.2 点云界面 | 导入、采样、点云/法线/边界显示、后台重建、历史与资产保存 | 本机通过；见 docs/checkpoints/A5.2.md |
| A5.3a 距离场核心 | 图元/CSG、变换、步进、独立文件、CPU 参照及解析对照 | 本机通过；见 docs/checkpoints/A5.3a.md |
| A5.3b 距离场界面 | Vulkan 两个全屏三角形、交互参数、法线/阴影/步数热图及 CPU/GPU 对照 | 本机通过；见 docs/checkpoints/A5.3b.md |

A5.3 分为核心和界面两个提交检查点，验收范围保持为完整 SDF 独立模式。

### A6 检查点

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A6.1 模拟核心 | 质量弹簧、半隐式 Euler、固定子步、风、球/平面连续碰撞、冻结与 CPU 场景示例 | 本机通过；见 docs/checkpoints/A6.1.md |
| A6.2 表面材质 | 微圆柱经纬模型、材质与文件参数、CPU/Vulkan 着色及数值/图像对照 | 本机通过；见 docs/checkpoints/A6.2.md |
| A6.3 播放界面 | 后台模拟、动态 GPU 网格、播放/暂停/单步/重置、参数与冻结历史 | 本机通过；见 docs/checkpoints/A6.3.md |

### A7 检查点

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A7.1 介质核心 | 均匀 RGB 介质、自由程、HG、多次散射、阴影透射率、文件与 CPU 示例 | 本机通过；见 docs/checkpoints/A7.1.md |
| A7.2 介质界面 | 区域与系数编辑、预置场景、离线任务、历史与文件回归 | 本机通过；见 docs/checkpoints/A7.2.md |
| A7.3 SPPM 核心 | 光子发射、可见点、空间哈希、半径/通量渐进更新、后台任务与收敛 | 本机通过；见 docs/checkpoints/A7.3.md |
| A7.4 SPPM 界面 | 积分器选择、参数、迭代进度、焦散场景与 UI 回归 | 本机通过；见 docs/checkpoints/A7.4.md |
| A7.5 SSS 核心 | dipole BSSRDF、表面位置采样、边界 Fresnel、路径接入与数值检查 | 本机通过；见 docs/checkpoints/A7.5.md |
| A7.6 SSS 界面 | 材质参数、预置、展示场景、历史与成像回归 | 本机通过；见 docs/checkpoints/A7.6.md |

三项算法分别完成核心与界面，范围按上方选题实现表验收。

### A8 检查点

| 检查点 | 内容 | 状态 |
| --- | --- | --- |
| A8.1 光场核心 | 固定瞳孔阵列、流式文件、有界缓存、后台采集、重聚焦与 CLI | 本机通过；见 docs/checkpoints/A8.1.md |
| A8.2 光场界面 | 采集、文件读写、子视图、焦距/孔径、显示归属与交互回归 | 本机通过；见 docs/checkpoints/A8.2.md |

### 切换条件

1. A0–A8 全部达到各自验收要求，12 项中除 OptiX 外的 11 项有真实运行证据。
2. CPU-only 构建不依赖 Qt、Vulkan 或 CUDA；桌面构建在 OptiX 关闭时通过。
3. 每项算法有最小场景、必要测试、实现笔记与已知限制。
4. 提交固定种子、场景参数、误差指标与参考图清单。较大的图像通过发布资产或外部资源清单保存，不塞入普通 Git 历史。
5. 记录 Apple Silicon/Clang 与 MoltenVK 测试结果；Windows/NVIDIA 未验证项明确保留。
6. 所有工作提交并推送，工作区干净；推送标记为 macos-baseline 的带说明标签，并在迁移说明中记录其 commit SHA。

## NVIDIA 阶段

| 阶段 | 内容 | 退出条件 | 状态 |
| --- | --- | --- | --- |
| B0 环境 | 新机器 clone；安装 MSVC、Qt、Vulkan、CUDA、OptiX；记录驱动与 GPU | 从本机验收标签构建 CPU-only 与桌面程序；OptiX 样例运行 | 计划 |
| B1 平台回归 | MSVC 兼容、中文路径、DPI、窗口生命周期、原生 Vulkan、Validation 检查 | 本机 11 项在目标平台复验，差异按数值/驱动/缺陷分类记录 | 计划 |
| B2 GPU 路径 | OptiX 数据转换、GAS/IAS、求交、材质、MIS、环境光、景深、分批调度 | GPU 图像与 CPU 参考统计一致，任务取消和显存生命周期正确 | 计划 |
| B3 最终验收 | NVIDIA 调试与性能报告、完整演示、Windows 发布包与说明 | 12 项完成；干净 Windows 环境可运行；保存最终报告和版本标签 | 计划 |

OptiX 首版采用可读性优先的迭代式路径追踪。按小批次提交和返回结果，在批次边界检查取消，控制 Windows 上单次 GPU 工作时长。CUDA/Vulkan 显存互操作、复杂 wavefront 调度及专有优化不作为首版要求。

Windows 阶段不重新组织场景格式和算法模块。确需平台修复时合入同一代码线，并保留本机 CPU/桌面构建能力。

## 代码与测试

### 编写规范

- 模块说明职责、输入输出、依赖和数据流；类说明所有权、生命周期和不变量。
- 公共函数、算法入口和复杂辅助函数注明单位、坐标空间、返回值、失败条件及实现思路。
- 公式旁对照代码变量与公式项，引用论文章节或公式编号。PDF 的面积/立体角测度转换、离散概率、传输方向和归一化必须明确。
- Vulkan 注释说明布局、访问阶段、同步和销毁时机；OptiX 注释说明主机/设备内存、SBT 和 ray payload。
- 中文解释配合标准英文术语，不逐行翻译代码，不写没有信息的重复注释。
- 使用 RAII、标准容器和明确所有权；避免巨型窗口类、全局可变状态和过度模板封装。
- clang-format 固定格式；Clang 和 MSVC 分别启用适当警告，按涉及模块运行检查。
- 每项实现笔记包含原理、公式、数据流、代码入口、阅读顺序、最小例子、验证方式与限制。文档与代码同提交维护。

### 验证项目

| 类别 | 必测内容 |
| --- | --- |
| 求交 | BVH 对照暴力求交、平行射线、边界命中、实例变换与法线 |
| 采样 | PDF 归一化、离散事件、sample/eval/pdf 一致性、Fresnel 极限 |
| 光照 | MIS 多种子均值/方差、能量、环境光、多光源与低粗糙度 |
| 几何 | UV 接缝、开放边界、退化输入、非法折叠、翻面、细分裂缝与误差 |
| 模拟 | 固定点、无外力、时间步收敛、碰撞穿透和固定初态重现 |
| 高级积分 | 介质解析透射、相函数、SPPM 半径/通量、直接光重复计数、SSS 分布 |
| 成像 | 已知深度的重聚焦、无需新增射线、SDF 球体解析对照 |
| 平台 | 中文/空格路径、资源加载、CPU/GPU 统计一致性、Vulkan 生命周期 |
| UI | 连续启动/取消、暂停恢复、场景修改、窗口缩放、模式切换与保存重载 |

随机算法同时使用固定种子回归和多种子统计。CPU/GPU 不要求逐像素或逐位相同；在线性图像中检查偏差和误差分布。每个测试固定场景、分辨率、种子与预算，在算法阶段建立并记录合理阈值，不以任意性能倍数作通过标准。

性能报告记录硬件、驱动、编译模式、面数、分辨率、样本数、耗时和内存。Debug 启用 Validation Layers，完成阶段不能留有已知的同步、越界或资源生命周期错误。

## 仓库管理

### 提交与同步

1. main 保存可复现的基线。空仓库首次提交初始化计划与仓库入口；此后功能使用 codex/<阶段>-<主题> 分支。
2. 开始任务前检查 status、当前分支和 origin，fetch 远端；已有未提交改动先识别归属，不覆盖、不擅自 stash 或 reset。
3. 按独立功能提交，提交消息说明具体行为。只暂存本次任务相关文件，提交前检查 staged diff 与必要测试。
4. 每完成一个 CKPT 即提交并推送远端功能分支，建立 PR，写明功能、测试、平台和未验证项。通过约定检查后合入 main；PR 尚未合并时准确标注状态。
5. 不强制推送共享分支，不改写已发布历史，不把另一台设备的本地副本当作独立项目。
6. 换机前提交、推送、检查远端 SHA；新机 clone/fetch 并检出记录的标签或分支，不复制 build 目录和二进制。
7. 发生分叉先检查双方提交；main 默认使用 pull --ff-only，不能快进时显式处理，不覆盖远端变化。

不要提交构建产物、用户 preset、Qt/Vulkan/CUDA/OptiX SDK、机器绝对路径、临时日志、大型模型或长视频。小型可再分发测试资产可入库；大型资产记录来源、许可、下载方式和校验值。仓库不发布本机 PDF 或个人路径。

macOS 每次桌面更新后运行 `scripts/install_macos.py`，固定替换 `/Applications/MiniRender.app`，清理同 bundle ID 的旧安装包和工作树构建包。Windows 发布阶段沿用只保留最新版的要求。

### 持续集成

从 A0 建立 macOS 与 Windows 的 CPU 构建/CTest 检查，Windows CI 可在换机前发现部分 MSVC 问题，但不算目标设备实测。Qt 构建与 SPIR-V 编译检查在依赖稳定后加入。

普通托管 CI 不假定有可用 NVIDIA GPU，也不把被跳过的 GPU 测试当作通过。MoltenVK 渲染测试在本机执行并记录；NVIDIA 测试到 B 阶段在目标设备执行。自托管 runner 只在确有需要时配置，默认不开放给不受信任的外部 PR。

阶段报告记录测试状态为已通过、失败或未运行，附 commit、平台、命令、结果位置和已知问题。A9 形成设备交接说明，B3 形成最终验收说明。

## 参考资料

- [Qt QVulkanWindow](https://doc.qt.io/qt-6/qvulkanwindow.html)：窗口、设备和交换链生命周期。
- [MoltenVK Runtime Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)：本机 Vulkan 运行和限制。
- [Vulkan portability enumeration](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_enumeration.html)：可移植设备枚举。
- [OptiX SDK](https://developer.nvidia.com/designworks/optix/download)：目标设备、驱动与工具链要求。
- [Veach 与 Guibas，1995](https://www.cs.jhu.edu/~misha/ReadingSeminar/Papers/Veach95.pdf)：多重重要性采样原文。
- [PBRT 第四版](https://pbr-book.org/4ed/contents)：材质、相机、采样、表面及体积路径追踪实现参考。
- [Garland 与 Heckbert，1997](https://www.cs.cmu.edu/~garland/Papers/quadrics.pdf)：QEM 简化原文。
- [Sadeghi 等，2013](https://graphics.ucsd.edu/~henrik/papers/practical_microcylinder_appearance_model_for_cloth_rendering.pdf)：布料表面微圆柱模型原文。
- [Progressive Photon Mapping，2008](https://graphics.ucsd.edu/~henrik/papers/progressive_photon_mapping/) 与 [SPPM，2009](https://graphics.ucsd.edu/~henrik/papers/stochastic_progressive_photon_mapping.pdf)：渐进光子映射及随机扩展。
- [Jensen 等，2001](https://graphics.stanford.edu/~srm/publications/SG01-subsurf-abstract.html)：dipole 次表面散射论文入口。
- [Bernardini 等，1999](https://research.ibm.com/publications/the-ball-pivoting-algorithm-for-surface-reconstruction)：Ball Pivoting 论文入口。

各算法实现前继续核对原文，在对应学习笔记补充所用公式、实现差异及适用条件。
