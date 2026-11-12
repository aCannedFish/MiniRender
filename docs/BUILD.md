# 构建与运行

## 本机环境

A0 使用 Apple M3 / macOS，Apple Clang 21.0.0、CMake 4.1.1、Ninja 1.12.1、Qt 6.11.2、MoltenVK 1.4.2、Vulkan Loader/Headers/Validation Layers 1.4.363、glslang 16.6.0。这里记录实际验证组合，后续升级需要重新运行检查。

通过 Homebrew 安装桌面所需依赖：

```sh
brew install cmake ninja qt glslang molten-vk vulkan-loader vulkan-headers vulkan-validationlayers vulkan-tools
```

GLM 1.0.3、Catch2 3.16.1、nlohmann/json 3.12.0 优先使用本机同版本包。缺失时 CMake 下载固定归档并验证 SHA256，定义见 `cmake/Dependencies.cmake`。tinyobjloader 2.0.0rc13 同样通过固定归档与 SHA256 下载。TinyEXR 1.0.12 与其 miniz 依赖以逐文件 SHA256 固定。CPU-only 配置不发现 Qt、Vulkan、CUDA 或 OptiX。

先执行 `vulkaninfo --summary`，确认真实 Apple GPU 与 `VK_LAYER_KHRONOS_validation`。如果受运行环境限制而无法访问 Metal，必须在允许访问 GPU 的本机进程中测试，不能用软件/模拟 ICD 代替。

## 核心程序

```sh
cmake --preset macos-core-debug
cmake --build --preset macos-core-debug
ctest --preset macos-core-debug
build/macos-core-debug/minirender_cli --capabilities
build/macos-core-debug/minirender_cli --test-pattern /tmp/minirender-pattern.ppm
```

`--test-pattern` 生成线性 RGB 与 sRGB 的诊断图。使用 `minirender_cli --render scenes/cornell-box.json cornell` 渲染场景并写出 EXR 与 PNG；`--help` 列出场景检查和光场命令。

## 桌面程序

```sh
cmake --preset macos-desktop-debug
cmake --build --preset macos-desktop-debug
build/macos-desktop-debug/src/app/desktop/minirender_desktop.app/Contents/MacOS/minirender_desktop --validation
```

Qt 提供工作台和原生窗口，项目的 `VulkanViewport` 管理设备与交换链。默认打开 studio 场景与编辑工作台，使用方法见 VIEWPORT.md。A0 棋盘/深度诊断仍由 --smoke 参数运行。

GPU 自动验证默认在隐藏窗口下运行离屏 Vulkan 渲染，检查尺寸变化与图像读回：

```sh
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app
```

测试不会激活或置顶窗口。Qt 继续运行布局和事件循环，Vulkan 使用同一套渲染管线，将最终结果写入独立图像。日志记录 `offscreen` 与 `hidden, no activation`，并检查视口未暴露、窗口未激活。人工检查窗口呈现时可以显式传入 `--onscreen`，该模式也不主动激活窗口。

离屏测试覆盖渲染、尺寸变化、读回和资源释放；交换链呈现由人工窗口检查覆盖。

结果写入 `outputs/a0/gpu-smoke.png` 和同目录日志。脚本检查退出码、像素检查成功和销毁之后的零 Validation 错误。日志缺失、超时、不能回读或缺少 Validation Layer 都是失败，不是跳过。

着色器在构建时编译并嵌入程序，因此可从其他工作目录运行。场景编辑、相机交互和材质参数面板已在 A1.2 加入；阴影、HDR 后处理与 EXR 输出已分别在 A1.3/A1.4 加入。

## 更新应用

本机正式应用固定为 `/Applications/MiniRender.app`，每次更新替换这一份，不保留按版本命名的旧应用。

完成桌面构建和回归后执行：

```sh
python3 scripts/install_macos.py
```

脚本在临时目录复制 Release 应用，使用 `macdeployqt` 部署 Cocoa、原生样式、Vulkan loader、MoltenVK、验证层和运行库，校验包签名，并执行隐藏窗口下的离屏 GPU 检查和完整朝向回归。检查通过后替换正式应用，再按 bundle ID 清理应用目录及本仓库全部工作树构建目录中的旧 `.app` 包。源码、资产、构建缓存和测试结果保留。

macOS 示例与纹理放在包内 `Contents/Resources/scenes`。内置示例作为副本打开，首次保存时选择用户路径。

旧构建包清理后，下次构建先运行 `cmake --preset macos-desktop-release`，再 build 和测试，重新生成应用包元数据。

更新前应退出 MiniRender 并等待相关构建结束。替换失败时恢复原应用；正式包内的 `Contents/Resources/minirender-build.json` 记录提交和安装时间。部署方式参考 [Qt macOS 部署文档](https://doc.qt.io/qt-6/macos-deployment.html)。

## Windows

CPU CI 使用 Windows 2022 runner 和 MSVC，桌面及 NVIDIA 运行仍按计划留到 B 阶段。在 VS Developer PowerShell 中可以使用 `windows-core-debug` 等 presets。Qt 与 Vulkan SDK 的私有位置通过不入库的 `CMakeUserPresets.json` 配置。

OptiX 开关当前会明确报未实现错误，而非构建空后端；到 B2 才加入 CUDA 语言、SDK 检测与设备实现。

## 提交检查

```sh
clang-format --dry-run --Werror include/minirender/core/*.hpp src/core/*.cpp src/app/*.cpp src/app/desktop/*.hpp src/app/desktop/*.cpp tests/*.cpp
ctest --preset macos-core-debug
git diff --check
```

每完成一个可验证 CKPT，提交相关代码、文档和验收记录并 push 到远端分支。不要把构建目录、SDK、机器路径或本地生成日志提交到普通 Git 历史。

纹理文件使用固定提交的 stb 读取/写出，桌面导入使用 Qt Concurrent。新增依赖均由 CMake 传递给 VS Code；运行纹理验证见 TEXTURES.md。
