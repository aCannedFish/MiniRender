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

A3.1 已加入 SSAO 半球采样、双边滤波、独立的环境漫反射合成，以及原始/滤波 AO 视图。右侧“实时”页可调整参数，实时 EXR 保留合成后的线性结果。见 [SSAO 说明](docs/SSAO.md) 与 [验收记录](docs/checkpoints/A3.1.md)。A3.2 的 IBL 仍待实现。
