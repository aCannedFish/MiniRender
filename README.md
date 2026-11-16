# MiniRender

MiniRender 是用于学习图形学的小型渲染引擎，使用 C++20、Qt Widgets 和 Vulkan，提供交互式场景编辑、实时预览及 CPU 离线渲染。几何处理、布料模拟和光场工具共用场景与资产系统，生成的模型可以继续编辑、保存和渲染。

## 功能

| 模块 | 支持内容 |
| --- | --- |
| 场景编辑 | OBJ 导入、JSON 场景保存、材质与纹理编辑、物体拾取和移动/旋转操纵器、撤销/重做；物体检查器与全局渲染设置独立停靠 |
| 实时预览 | 材质原色与纹理、轮廓和接触遮蔽；方向光与 PCF 阴影、GGX 材质、IBL、SSAO；HDR、曝光、FXAA，以及法线、深度、线框和 AO 视图 |
| 路径追踪 | SAH BVH、多线程采样、MIS、Russian roulette、薄透镜景深；漫反射、金属、镜面和玻璃；面积光、方向光及环境光 |
| 高级光照 | 均匀 RGB 介质与多次体积散射、SPPM 焦散、dipole BSSRDF 次表面散射 |
| 几何处理 | 半边拓扑、均匀与自适应 Loop 细分、高度位移、QEM 简化、各向同性重网格、OBJ 导出 |
| 点云与 SDF | PLY 导入/导出、网格表面采样、PCA 法线与定向、多半径 Ball Pivoting 重建；球、盒、圆环及 CSG 距离场编辑与渲染 |
| 布料 | 质量弹簧模拟、固定子步、风与球/平面碰撞、播放/暂停、状态保存与冻结网格；可编辑的微圆柱经纬材质 |
| 光场 | 二维瞳孔阵列采集、流式光场文件、子视图查看、交互式焦距与孔径调整；重聚焦复用已采集数据 |

离线任务支持实际采样进度、暂停、恢复、取消及 PNG/EXR 导出。工具栏“快速导入”提供 Cornell Box、材质球、雾与光束、玻璃焦散、蜡和大理石、光场棋盘等场景，导入后可继续编辑和另存。

## 目录架构

```text
MiniRender/
├── include/minirender/   # 公开接口、参数类型和数据约定
│   ├── core/            # 图像、纹理、随机采样和取消标记
│   ├── scene/           # 场景、材质、相机、编辑事务和操纵器
│   ├── geometry/        # 网格、点云和距离场算法接口
│   ├── render/          # 求交、光传输、采样和渲染任务接口
│   ├── simulation/      # 布料模拟与场景组件
│   └── assets/          # OBJ、PLY、JSON 和光场文件接口
├── src/                 # 与公开模块对应的实现及模块内部辅助类型
│   ├── core/            # 像素、mipmap、颜色编码与采样实现
│   ├── scene/           # 场景校验、快照、历史和几何变换
│   ├── geometry/        # 细分、位移、简化、重网格、点云重建和 SDF
│   ├── render/          # CPU 积分器、BSDF、光源、介质和成像
│   ├── simulation/      # 时间积分、弹簧、碰撞与模拟网格
│   ├── assets/          # 文件解析、生成资产与事务式写盘
│   ├── app/             # CLI 参数和命令分发
│   │   └── desktop/     # Qt 面板、后台任务协调和 Vulkan 资源与管线
│   └── external/        # 第三方图像库的实现入口
├── shaders/             # Vulkan 顶点、片元与共用 GLSL 数据布局
├── scenes/              # 内置 JSON 场景及小型模型、纹理
├── examples/            # 直接调用核心模块的可运行示例
├── tests/               # 数值、几何、文件及任务协议回归
├── scripts/             # 安装、资产生成和隐藏窗口验证工具
├── cmake/               # 固定依赖与构建辅助
├── .vscode/             # 编辑器、CMake 与语法检查配置
└── docs/                # 算法笔记、操作说明与验证资料
```

公开头文件集中说明参数、返回值和所有权。算法实现放在同名模块的 `src` 目录，内部辅助头文件随实现维护。Qt 控件和 Vulkan 上传代码位于 `src/app/desktop`；CPU 算法可通过 `examples` 直接调用。

### 算法入口

| 功能 | 公开接口 | 核心实现 |
| --- | --- | --- |
| BVH 求交 | [bvh.hpp](include/minirender/render/bvh.hpp) | [bvh.cpp](src/render/bvh.cpp) |
| 路径追踪 | [path_tracer.hpp](include/minirender/render/path_tracer.hpp) | [path_tracer.cpp](src/render/path_tracer.cpp) |
| 材质与 MIS | [bsdf.hpp](include/minirender/render/bsdf.hpp)、[light.hpp](include/minirender/render/light.hpp) | [bsdf.cpp](src/render/bsdf.cpp)、[light.cpp](src/render/light.cpp) |
| 后台渲染 | [render_session.hpp](include/minirender/render/render_session.hpp) | [render_session.cpp](src/render/render_session.cpp) |
| Loop 细分 | [loop.hpp](include/minirender/geometry/loop.hpp) | [loop.cpp](src/geometry/loop.cpp) |
| 位移与自适应 | [displacement.hpp](include/minirender/geometry/displacement.hpp)、[adaptive.hpp](include/minirender/geometry/adaptive.hpp) | [displacement.cpp](src/geometry/displacement.cpp)、[adaptive.cpp](src/geometry/adaptive.cpp) |
| QEM 简化 | [simplify.hpp](include/minirender/geometry/simplify.hpp) | [simplify.cpp](src/geometry/simplify.cpp) |
| 重网格 | [remesh.hpp](include/minirender/geometry/remesh.hpp) | [remesh.cpp](src/geometry/remesh.cpp)、[remesh_mesh.cpp](src/geometry/remesh_mesh.cpp) |
| 点云重建 | [point_normals.hpp](include/minirender/geometry/point_normals.hpp)、[ball_pivot.hpp](include/minirender/geometry/ball_pivot.hpp) | [point_normals.cpp](src/geometry/point_normals.cpp)、[ball_pivot.cpp](src/geometry/ball_pivot.cpp) |
| 距离场 | [sdf.hpp](include/minirender/geometry/sdf.hpp)、[sdf_render.hpp](include/minirender/render/sdf_render.hpp) | [sdf.cpp](src/geometry/sdf.cpp)、[sdf_render.cpp](src/render/sdf_render.cpp) |
| 布料模拟 | [cloth.hpp](include/minirender/simulation/cloth.hpp) | [cloth.cpp](src/simulation/cloth.cpp)、[cloth_collision.cpp](src/simulation/cloth_collision.cpp) |
| 布料外观 | [cloth_bsdf.hpp](include/minirender/render/cloth_bsdf.hpp) | [cloth_bsdf.cpp](src/render/cloth_bsdf.cpp) |
| 体积散射 | [medium.hpp](include/minirender/render/medium.hpp) | [medium.cpp](src/render/medium.cpp) |
| 光子映射 | [sppm.hpp](include/minirender/render/sppm.hpp) | [sppm.cpp](src/render/sppm.cpp) |
| 次表面散射 | [dipole.hpp](include/minirender/render/dipole.hpp)、[subsurface.hpp](include/minirender/render/subsurface.hpp) | [dipole.cpp](src/render/dipole.cpp)、[subsurface.cpp](src/render/subsurface.cpp) |
| 光场 | [lightfield.hpp](include/minirender/render/lightfield.hpp)、[lightfield_capture.hpp](include/minirender/render/lightfield_capture.hpp) | [lightfield_capture.cpp](src/render/lightfield_capture.cpp)、[lightfield_refocus.cpp](src/render/lightfield_refocus.cpp) |
| IBL | [ibl.hpp](include/minirender/render/ibl.hpp) | [ibl.cpp](src/render/ibl.cpp) |
| SSAO | [ssao.hpp](include/minirender/render/ssao.hpp) | [ssao.cpp](src/render/ssao.cpp)、[ssao.frag](shaders/ssao.frag)、[ao_filter.frag](shaders/ao_filter.frag) |

工作台按职责划分为窗口协调、属性面板、资产操作和几何任务，入口分别为 [workbench.cpp](src/app/desktop/workbench.cpp)、[workbench_properties.cpp](src/app/desktop/workbench_properties.cpp)、[workbench_assets.cpp](src/app/desktop/workbench_assets.cpp) 和 [workbench_geometry.cpp](src/app/desktop/workbench_geometry.cpp)。共用数值控件的接口放在内部头文件，实现位于 [workbench_widgets.cpp](src/app/desktop/workbench_widgets.cpp)。

## 构建要求

- Git、CMake 3.25 或更新版本、Ninja，以及支持 C++20 的编译器。
- 桌面程序需要 Qt 6.8 或更新版本，以及 Vulkan SDK（含 `glslangValidator`）。macOS 通过 MoltenVK 使用 Vulkan。
- 安装脚本和 GPU 验证脚本使用 Python 3。

GLM、Catch2、JSON、OBJ 和图像解析依赖由 CMake 管理，缺失时下载固定版本并核对哈希。CPU-only 构建可独立使用，不需要 Qt、Vulkan 或 CUDA。依赖版本和配置说明见[构建文档](docs/BUILD.md)。

先获取源码，以下命令均在仓库根目录执行：

```sh
git clone https://github.com/aCannedFish/MiniRender.git
cd MiniRender
```

## macOS

安装 Xcode Command Line Tools，并通过 Homebrew 安装桌面依赖：

```sh
xcode-select --install
brew install cmake ninja qt glslang molten-vk vulkan-loader vulkan-headers vulkan-validationlayers vulkan-tools
```

构建并运行核心测试：

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release --parallel 4
ctest --preset macos-desktop-release --output-on-failure
```

保存场景并退出正在运行的 MiniRender，然后安装：

```sh
python3 scripts/install_macos.py
open /Applications/MiniRender.app
```

安装器打包 Qt 和 Vulkan 运行库及内置场景，通过隐藏窗口检查后更新 `/Applications/MiniRender.app`，并清理同应用标识的旧安装包和工作树构建包。后续更新重复上述配置、构建和安装命令。

只使用 CPU 命令行时，选择 core preset：

```sh
cmake --preset macos-core-release
cmake --build --preset macos-core-release --parallel 4
ctest --preset macos-core-release --output-on-failure
```

## Windows

安装 Visual Studio 2022 的 C++ 工具链、CMake 和 Ninja，在 x64 Developer PowerShell 中构建 CPU 程序：

```powershell
cmake --preset windows-core-release
cmake --build --preset windows-core-release --parallel 4
ctest --preset windows-core-release --output-on-failure
.\build\windows-core-release\minirender_cli.exe --capabilities
```

构建桌面程序还需安装 Qt 的 MSVC 版本和 Vulkan SDK。将下面的 Qt 路径替换为本机目录：

```powershell
$qtPrefix = "C:/Qt/6.11.2/msvc2022_64"
$env:CMAKE_PREFIX_PATH = $qtPrefix
$env:PATH = "$qtPrefix/bin;$env:PATH"
cmake --preset windows-desktop-release
cmake --build --preset windows-desktop-release --parallel 4
.\build\windows-desktop-release\src\app\desktop\minirender_desktop.exe
```

桌面依赖配置见[构建文档](docs/BUILD.md#windows)，目标设备验证和安装包进度见 [PLAN.md](PLAN.md#nvidia-阶段)。

## 使用

### 桌面操作

1. 打开应用，在“快速导入”中选择场景。
2. 使用默认“原色预览”查看模型。单击选中物体，在视口按 V/T/R 切换选择、移动和旋转；右侧检查器调整材质与几何。
3. 在左侧“场景与渲染”面板调整相机和光源，或进入“离线”页选择 Path Tracing/SPPM，设置采样参数后点击“开始”。

视口上方可切换原色、光照和诊断视图。原色模式便于查看初始模型；光照合成显示实时方向光与环境光效果；面积光、间接照明、体积和次表面散射通过离线渲染查看。

窗口布局和控件见[编辑器界面](docs/EDITOR_UI.md)，模板说明见[内置场景](docs/BUILTIN_SCENES.md)，预览模式见[原色预览](docs/BASE_COLOR_PREVIEW.md)。

### 命令行

以 macOS core 构建为例，渲染 Cornell Box：

```sh
build/macos-core-release/minirender_cli --render scenes/cornell-box.json outputs/cornell --width 128 --height 128 --spp 32 --depth 10 --mode mis --seed 101 --threads 4
```

输出为 `outputs/cornell.exr` 和 `outputs/cornell.png`。Windows 使用 `build/windows-core-release/minirender_cli.exe`，参数相同。桌面构建目录中也包含 CLI。

CLI 还提供场景检查、法线图、光场采集与重聚焦命令，可通过 `--help` 查看入口。详细参数见[路径追踪](docs/PATH_TRACING.md)、[离线任务](docs/RENDER_SESSION.md)和[光场](docs/LIGHT_FIELD.md)。

### 开发调试

VS Code 打开仓库根目录，安装推荐扩展后选择 `macos-desktop-debug` 或 `windows-desktop-debug`，执行 CMake 配置。CMake Tools 与编译数据库为 Qt、Vulkan 和核心依赖提供语法检查参数，详见 [VS Code 配置](docs/VSCODE.md)。

GPU 回归默认使用隐藏窗口，例如验证原色预览轮廓：

```sh
python3 scripts/verify_gpu.py /Applications/MiniRender.app --scene scenes/studio.json --outline-check --sync-validation --output outputs/outline --timeout 240
```

## 文档

- [学习路线](docs/LEARNING.md)：从场景与像素开始阅读各模块实现。
- [场景格式](docs/SCENE.md)：模型、材质、纹理、相机与场景保存。
- [几何工具](docs/GEOMETRY_UI.md)、[点云工具](docs/POINT_CLOUD_UI.md)、[布料工具](docs/CLOTH_UI.md)：编辑、处理与导出资产。
- [离线界面](docs/OFFLINE_UI.md)、[光场界面](docs/LIGHT_FIELD_UI.md)：渲染任务、文件与图像输出。
- [PLAN.md](PLAN.md)：开发计划、阶段状态及验证记录索引。
