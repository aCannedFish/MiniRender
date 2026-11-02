# MiniRender

A0 工程基础已在本机通过验收：CPU 图像模块、CLI、Qt/Vulkan 纹理与深度诊断。记录见 [A0 验收](docs/checkpoints/A0.md)。其余选题尚未实现。功能范围、实现顺序、测试要求与仓库工作流见 [项目计划](PLAN.md)。

开发分为两个阶段：

1. 在 macOS 上完成 CPU 渲染、几何处理、模拟、光场和 Qt UI，并通过 MoltenVK 实现、测试 Vulkan 预览、SSAO 和 SDF。
2. 本机阶段验收后，在 Windows + NVIDIA GPU 上完成 OptiX、平台回归、性能测量与发布打包。

技术路线为 C++、Qt Widgets、Vulkan、CPU 路径追踪及可选 CUDA/OptiX。核心算法自行实现，模块说明、关键公式注释、最小演示和学习笔记随代码维护。

main 保存基线，后续开发通过功能分支和 PR 管理。构建和测试见 [构建说明](docs/BUILD.md)，代码阅读见 [基础实现](docs/FOUNDATION.md)。每完成一个 CKPT 即提交并 push，计划中的功能不代表已通过验收。
