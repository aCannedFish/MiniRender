# MiniRender

A0 工程基础已在本机通过验收：CPU 图像模块、CLI、Qt/Vulkan 纹理与深度诊断。记录见 [A0 验收](docs/checkpoints/A0.md)。课程选题仍在后续阶段实现。功能范围、实现顺序、测试要求与仓库工作流见 [项目计划](PLAN.md)。

开发分为两个阶段：

1. 在 macOS 上完成 CPU 渲染、几何处理、模拟、光场和 Qt UI，并通过 MoltenVK 实现、测试 Vulkan 预览、SSAO 和 SDF。
2. 本机阶段验收后，在 Windows + NVIDIA GPU 上完成 OptiX、平台回归、性能测量与发布打包。

技术路线为 C++、Qt Widgets、Vulkan、CPU 路径追踪及可选 CUDA/OptiX。核心算法自行实现，模块说明、关键公式注释、最小演示和学习笔记随代码维护。

main 保存基线，后续开发通过功能分支和 PR 管理。构建和测试见 [构建说明](docs/BUILD.md)，代码阅读见 [基础实现](docs/FOUNDATION.md)。每完成一个 CKPT 即提交并 push，计划中的功能不代表已通过验收。

VS Code 工作区配置见 [编辑器说明](docs/VSCODE.md)。请使用 desktop preset 生成包含 Qt/Vulkan 的编译参数。

A1.1 场景数据层已通过本机测试，支持 OBJ、JSON 场景、相机/变换和只读快照。见 [场景说明](docs/SCENE.md) 与 [A1.1 验收](docs/checkpoints/A1.1.md)。A1.2 已接入真实场景预览与编辑界面，见 [视口说明](docs/VIEWPORT.md) 和 [验收记录](docs/checkpoints/A1.2.md)。阴影、HDR 后处理等尚待完成。

A1.3 已加入多方向光深度图阴影和 PCF，见 [阴影实现](docs/SHADOWS.md) 与 [验收记录](docs/checkpoints/A1.3.md)。
