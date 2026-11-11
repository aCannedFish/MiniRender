# MiniRender

A0 工程基础已在本机通过验收：CPU 图像模块、CLI、Qt/Vulkan 纹理与深度诊断。记录见 [A0 验收](docs/checkpoints/A0.md)。课程选题仍在后续阶段实现。功能范围、实现顺序、测试要求与仓库工作流见 [项目计划](PLAN.md)。

开发分为两个阶段：

1. 在 macOS 上完成 CPU 渲染、几何处理、模拟、光场和 Qt UI，并通过 MoltenVK 实现、测试 Vulkan 预览、SSAO 和 SDF。
2. 本机阶段验收后，在 Windows + NVIDIA GPU 上完成 OptiX、平台回归、性能测量与发布打包。

技术路线为 C++、Qt Widgets、Vulkan、CPU 路径追踪及可选 CUDA/OptiX。核心算法自行实现，模块说明、关键公式注释、最小演示和学习笔记随代码维护。

main 保存基线，后续开发通过功能分支和 PR 管理。构建和测试见 [构建说明](docs/BUILD.md)，代码阅读见 [基础实现](docs/FOUNDATION.md)。每完成一个 CKPT 即提交并 push，计划中的功能不代表已通过验收。

VS Code 工作区配置见 [编辑器说明](docs/VSCODE.md)。请使用 desktop preset 生成包含 Qt/Vulkan 的编译参数。

A1.1 场景数据层已通过本机测试，支持 OBJ、JSON 场景、相机/变换和只读快照。见 [场景说明](docs/SCENE.md) 与 [A1.1 验收](docs/checkpoints/A1.1.md)。A1.2 已接入真实场景预览与编辑界面，见 [视口说明](docs/VIEWPORT.md) 和 [验收记录](docs/checkpoints/A1.2.md)。后续进展按各检查点记录。

A1.3 已加入多方向光深度图阴影和 PCF，见 [阴影实现](docs/SHADOWS.md) 与 [验收记录](docs/checkpoints/A1.3.md)。

A1.4 已加入浮点 HDR、独立后处理、FXAA、深度视图和 EXR 输出，见 [HDR 实现](docs/HDR.md) 与 [验收记录](docs/checkpoints/A1.4.md)。

A1.5 已加入纹理资产、线性 mipmap、三种过滤方式及异步导入，见 [纹理实现](docs/TEXTURES.md) 和 [验收记录](docs/checkpoints/A1.5.md)。

UI 交互修复见 [验证记录](docs/checkpoints/UI1.md)：属性布局、连续拖拽、侧栏恢复与 GPU 等待已调整。

参数卡顿修复见 [性能对照](docs/checkpoints/UI2.md)：普通编辑复用网格与描述符，文件导入在后台执行，状态栏显示任务阶段和预览等待。

A1.6 已加入保留遮挡的线框视图，并完成场景编辑阶段验收，见 [验收记录](docs/checkpoints/A1.6.md)。该检查点还补充了 macOS Metal layer 的事务配置，修复连续运行中捕获到的呈现等待。

A2.1 已加入 CPU 场景求交、SAH BVH 和法线诊断命令，见 [实现说明](docs/INTERSECTION.md) 与 [验收记录](docs/checkpoints/A2.1.md)。后续进展按 A2 检查点记录。

A2.2 已加入随机采样、四种基础 BSDF、面积/方向光和环境采样，见 [随机采样](docs/SAMPLING.md)、[表面散射](docs/BSDF.md)、[光源采样](docs/LIGHTING.md) 及 [验收记录](docs/checkpoints/A2.2.md)。

A2.3 已能通过 CLI 进行 CPU 路径追踪，支持三种采样模式、RR、薄透镜及 EXR/PNG，见 [路径追踪](docs/PATH_TRACING.md) 与 [验收记录](docs/checkpoints/A2.3.md)。后续任务与界面按 A2.4 检查点接入。

A2.4a 已接入多线程 RenderSession、暂停/恢复/取消、实际采样进度和不可变预览，CLI 支持线程/tile/批次设置。见 [任务说明](docs/RENDER_SESSION.md) 与 [验收记录](docs/checkpoints/A2.4a.md)。

A2.4b 已将任务接入 Qt 离线面板，包含真实进度、暂停/恢复/取消、场景过期提示、后台导出和 Vulkan 图像显示。UI/CLI 输出一致，A2 本机验收完成。见 [界面说明](docs/OFFLINE_UI.md) 与 [验收记录](docs/checkpoints/A2.4b.md)。

A3.1 已加入 SSAO 半球采样、双边滤波、独立的环境漫反射合成，以及原始/滤波 AO 视图。右侧“实时”页可调整参数，实时 EXR 保留合成后的线性结果。见 [SSAO 说明](docs/SSAO.md) 与 [验收记录](docs/checkpoints/A3.1.md)。

A3.2a 已完成 IBL 的 CPU 预处理核心：漫反射卷积、GGX 粗糙度层、BRDF LUT、进度和取消。数值对照及默认参数参考图见 [IBL 说明](docs/IBL.md) 与 [验收记录](docs/checkpoints/A3.2a.md)。

A3.2b 已接入 Vulkan 实时 IBL 与环境背景，右侧“环境”页支持导入、强度/旋转、后台进度、取消/重试和缓存。UI 参数可撤销并保存，SSAO 继续只影响环境漫反射。A3 本机验收完成，见 [环境界面](docs/ENVIRONMENT_UI.md) 与 [验收记录](docs/checkpoints/A3.2b.md)。

A4.1 已加入半边拓扑和均匀 Loop 核心，保留面角 UV 接缝，提供进度、取消及 CPU 渲染示例。几何工具 UI、位移、简化、重网格和自适应细分按后续检查点接入。见 [网格处理](docs/GEOMETRY.md) 与 [验收记录](docs/checkpoints/A4.1.md)。

A4.2a 已加入真实高度位移、共享顶点与边内部的接缝检查、OBJ 写出和生成资产的场景保存，支持进度与取消。见 [高度位移](docs/DISPLACEMENT.md) 和 [资产导出](docs/ASSET_EXPORT.md)。Qt 后台几何工具由 A4.2b 接入。

A4.2b 已接入右侧“几何”页，提供后台生成、阶段进度、取消、撤销/重做和 OBJ 导出，工具栏支持后台场景保存。生成结果用于实时与离线渲染，顶点/线框索引准备和导入校验在后台完成。见 [几何工具](docs/GEOMETRY_UI.md) 与 [验收记录](docs/checkpoints/A4.2b.md)。后续继续简化、重网格和自适应细分。

A4.3 已加入 QEM 网格简化，支持合法边折叠、边界/属性保护、实际面数与停止原因，以及独立双向表面距离。几何页选择“QEM 网格简化”即可生成可撤销的 LOD 资产。见 [实现笔记](docs/SIMPLIFICATION.md) 与 [验收记录](docs/checkpoints/A4.3.md)。

UI3 已加入视口物体操纵。选中物体后，使用顶部“移动”“旋转”工具，或在视口按 T/R；拖动彩色轴、白色中心点和旋转环即可操作，松开提交、Esc 取消，整次拖拽支持一次撤销/重做。见 [使用与实现](docs/MANIPULATION.md) 和 [验收记录](docs/checkpoints/UI3.md)。

UI4 已修复示例场景从地面下方观察时的共面闪烁。材质面板提供“双面渲染”，示例地面朝上单面显示，方块保持原有接触位置；实时表面、线框、阴影、拾取和 CPU 渲染遵循相同朝向。自动 GPU 回归默认隐藏窗口、离屏渲染。见 [表面朝向](docs/SURFACE_SIDES.md) 与 [验收记录](docs/checkpoints/UI4.md)。

A4.4 已加入各向同性重网格，支持长边分裂、短边折叠、价数翻边、切向平滑和原表面位置/UV 投影。几何页可设置目标边长、保护条件和预算，结果附带实际质量及双向表面距离，并用于实时、离线渲染和导出。见 [实现笔记](docs/REMESHING.md) 与 [验收记录](docs/checkpoints/A4.4.md)。

工具栏“快速导入”提供经典 Cornell Box、玻璃与金属、材质球阵列和小光源示例。覆盖导入后可继续编辑物体、材质、灯光与相机，再启动离线渲染。见 [内置场景](docs/BUILTIN_SCENES.md) 与 [验收记录](docs/checkpoints/SCENES1.md)。

A4.5 加入自适应 Loop 与位移，支持局部米制和固定视图像素误差、按需有限层参照、邻接平衡与终端过渡。几何页显示各层面数、实际采样误差和预算情况。生成资产可撤销、保存，并用于实时与 CPU 渲染。实现和阅读顺序见 [自适应细分](docs/ADAPTIVE.md)。

A5.1 加入点云重建核心，支持 PLY 三种编码、PCA 法线、最小生成森林定向与多半径 Ball Pivoting。命令行示例完成点云读写、网格重建、场景保存和离线渲染，并报告孔洞、分量及未使用点。实现见 [点云重建](docs/POINT_CLOUD.md)，数据与测试见 [验收记录](docs/checkpoints/A5.1.md)。

A5.2 接入右侧“点云”页，提供后台导入/采样、法线缓存、重建、取消、独立预览和 PLY 导出。生成结果可撤销、编辑、保存与离线渲染；同一份点云重复重建会更新目标物体。见 [点云工具](docs/POINT_CLOUD_UI.md) 与 [验收记录](docs/checkpoints/A5.2.md)。

A5.3 加入球、盒、圆环和 CSG 距离场，支持刚性变换、均匀缩放、独立 JSON、sphere tracing 与 CPU 参照图。右侧“距离场”页提供 Vulkan 视图、交互参数、布尔组合、独立历史和文件操作。实现见 [距离场](docs/SDF.md)，操作见 [距离场界面](docs/SDF_UI.md)，测试见 [验收记录](docs/checkpoints/A5.3b.md)。

A6.1 加入 CPU 质量弹簧布料、固定子步、风与球/平面连续碰撞。冻结网格可以进入普通场景保存和离线渲染。使用方法与代码阅读顺序见 [布料模拟](docs/CLOTH_SIMULATION.md)。

A6.2 接入微圆柱布料材质，提供经纬颜色、方向、宽度、覆盖率、切向曲线和四种预置。CPU 使用完整混合采样，Vulkan 直接光与 CPU 模型对照通过，材质编辑可撤销、保存和重开。实现与操作见 [布料材质](docs/CLOTH_MATERIAL.md)，测试见 [验收记录](docs/checkpoints/A6.2.md)。

A6.3 加入“布料”页、落球与悬挂示例、后台固定步播放、参数应用和可恢复的状态保存。布料与碰撞体可移动、旋转，冻结后可继续使用几何工具；离线渲染固定当前可见帧。动态帧复用 GPU 缓冲，长计算显示进度并支持取消。操作、调度和代码阅读顺序见 [布料播放](docs/CLOTH_UI.md)，测试见 [验收记录](docs/checkpoints/A6.3.md)。

A7.1 加入均匀 RGB 介质、自由程采样、HG 相函数、多次散射和带介质衰减的阴影查询。场景文件保留有界区域及光学参数，CLI 和后台 CPU 任务可以渲染雾与光束。独立例程生成场景、参考图和多种子收敛记录。实现与代码阅读顺序见 [体积散射](docs/VOLUME.md)，测试见 [验收记录](docs/checkpoints/A7.1.md)。

A7.2 加入“介质”页，支持区域草稿、参数应用、历史和精度保留。快速导入扩展为七个模板，包含有界雾、Cornell Box 雾与透光孔光束；可继续编辑并进入带进度的离线渲染。操作与实现见 [介质界面](docs/VOLUME_UI.md)，测试见 [验收记录](docs/checkpoints/A7.2.md)。

A7.3 加入 SPPM 核心，提供光子发射、随机相机可见点、空间哈希和逐像素渐进统计。A7.4 将积分器选择、光子预算、半径、迭代和阶段进度接入离线面板，并提供两个可编辑的焦散快速导入模板。共享后台任务支持暂停回滚、恢复和图像导出。算法、功率及法线测度说明见 [渐进光子映射](docs/SPPM.md)，操作见[离线界面](docs/OFFLINE_UI.md)，数值与收敛记录见 [核心验收](docs/checkpoints/A7.3.md)。
