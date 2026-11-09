# VS Code

## 打开工程

用 VS Code 打开 MiniRender 仓库根目录，安装工作区推荐的 Microsoft C/C++ 与 CMake Tools 扩展。执行 `CMake: Select Configure Preset`，本机选择 `macos-desktop-debug`，Windows 选择 `windows-desktop-debug`，然后执行 `CMake: Configure` 和构建。

工程依赖必须先安装或由 CMake 下载。桌面配置包含所有核心目标及 Qt/Vulkan 源文件；如果只选 core 配置，CMake Tools 不会提供桌面目标的编译信息。

## 语法检查

使用 Microsoft C/C++ 的实际诊断，未关闭错误提示。CMake Tools 是首选 configuration provider，向 IntelliSense 提供实际 compiler、defines、include paths 和 C++20 标准。

`.vscode/c_cpp_properties.json` 为 Mac/Windows 设置各自 desktop-debug 编译数据库作为备用。Qt Framework、Vulkan、GLM、Catch2 及之后加入的库都从 CMake 目标传播，无需手写 Homebrew 或个人 SDK 绝对路径。

本机同时安装了 clangd 与 C/C++。本工作区通过 `clangd.enable: false` 只停用 clangd 扩展，保留 C/C++ 诊断，避免两套语言服务使用不同参数后产生重复或冲突错误。该设置不改变其他工作区。

若确实希望改用 clangd，应同时停用 C/C++ IntelliSense，并让 clangd 读取 `build/compile_commands.json`。CMake Tools 配置完成后会将当前编译数据库复制到该位置。不能同时保留两套诊断，也不能给所有 C++ 文件统一猜测 include 路径。

## 构建与维护

Ctrl/Cmd+Shift+B 构建 CMake Tools 当前 build directory；测试任务在相同目录运行 CTest。格式化使用仓库 `.clang-format`。

切换 preset、新增源文件或依赖后重新 Configure。若出现 unknown type，先检查 CMake Configure 是否成功、是否选择 desktop preset，以及 compile_commands 中是否存在对应 `.cpp`。头文件由包含它的翻译单元继承配置。

编译数据库由本机生成且包含绝对路径，不能提交。只有共享的 settings、扩展推荐、任务和 C/C++ 配置进入 Git；私人调试路径留在用户设置或 CMakeUserPresets 中。

## 验证记录

A0.1 检查了配置 JSON 和扩展支持的配置键。使用 desktop-debug 的真实 compile_commands 对桌面 main.cpp 执行 clangd 静态检查，结果为 0 errors；其中包含 Qt、Vulkan 和核心头文件。核心与桌面 C++ 编译也已通过。

这证明编译参数可被语言服务解析。VS Code 的初次索引仍需要所选 preset 配置成功；Windows 的本地 IntelliSense 界面将在 B 阶段实测。

配置依据：[Microsoft IntelliSense 文档](https://code.visualstudio.com/docs/cpp/configure-intellisense)、[C/C++ 配置参考](https://code.visualstudio.com/docs/cpp/customize-cpp-settings)。

macOS 桌面回归的 `regression_activity.mm` 使用 Objective-C++ 调用系统 activity API。desktop preset 配置该编译器并为文件记录 `-fobjc-arc`；C++ 头文件通过普通指针隔离原生类型，核心和 Windows 构建不编译该文件。
