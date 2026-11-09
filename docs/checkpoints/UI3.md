# 物体操纵

日期：2026-11-09。UI3 本机通过。场景中的选中物体可通过移动轴与旋转环直接操作，使用方法和数学推导见 [物体操纵](../MANIPULATION.md)。

## 完成范围

- 工具栏选择/移动/旋转，以及视口中的 V/T/R 快捷键。
- 世界 X/Y/Z 轴拖拽、中心点屏幕平面移动、世界轴圆环旋转。
- 近似固定的屏幕尺寸、共用投影线段的显示与命中、交叉处深度选择和高亮。
- 连续角度累计、ZYX 欧拉表示、侧视圆环与近平行约束处理。
- 变换字段同步、Esc 取消、单次拖拽撤销/重做及普通编辑的事务协调。
- 鼠标捕获、隐藏、相机与窗口尺寸变化、场景替换和退出时的手势处理。
- 交换链覆盖绘制、sRGB 颜色、HDR 隔离及离线显示时隐藏。
- 轻量参数预览、不可变网格/描述符复用，保持相机手势和数值输入。

## 核心检查

新增 10 个用例，覆盖投影、逻辑像素命中、屏幕尺寸、全部三个世界轴和屏幕平面移动、非均匀/负缩放下的旋转组合、连续 2.25 圈、奇异姿态、侧视圆环和近平行射线保留最后有效结果。

历史检查连续发布 150 次预览，提交后一次撤销恢复初态；取消或返回初态的拖拽保留原重做历史。非法预览或普通编辑不改变已验证状态，普通事务先提交当前手势。场景替换清理手势，撤销可取消当前预览，相机导航在取消后保留。

Core Release、desktop Debug 和 desktop ASan/UBSan 的完整 CPU 回归均为 139/139。构建无新增警告，格式、文档与编译数据库检查通过。新源文件沿用 VSCode 的 CMake Tools 配置。

## 桌面检查

实际 Qt 鼠标事件逐帧驱动拖拽，运行真实 Vulkan 视口。GPU 颜色采样确认轴与环存在；关闭覆盖层后，以像素中心射线查询 CPU BVH，对照稳定区域的法线和深度。

回归先走普通启动使用的异步场景导入路径，确认默认选中物体的操纵器可见。工具选中状态通过 `QAction::toggled` 驱动视口模式，检查直接设置选中状态及点击操作均能切换工具。

| 场景状态 | studio | 20 万面场景 |
| --- | --- | --- |
| 初态 | 2703/2703 | 2468/2468 |
| X 轴移动 | 2686/2686 | 2440/2440 |
| 移动撤销 | 2703/2703，像素恢复 | 2468/2468，像素恢复 |
| 移动重做 | 2686/2686，像素恢复 | 2440/2440，像素恢复 |
| 取消移动 | 2703/2703，像素恢复 | 2468/2468，像素恢复 |
| 世界轴旋转 60° | 2692/2692 | 2462/2462 |
| 旋转撤销/重做 | 对应图像逐像素恢复 | 对应图像逐像素恢复 |
| 保存重开 | 2692/2692，像素恢复 | 2462/2462，像素恢复 |

中心点拖拽后检查位移与相机前向正交，按 Esc 恢复原变换。鼠标捕获释放后提交一次历史，窗口隐藏时取消并恢复；显示和缩放窗口后可继续开始新手势。操纵期间相机保持原值。

对同一场景分别在操纵器关闭与显示时读回 HDR，最大 RGB 差为 0。离线检查使用 8×6、4 spp、seed=1 的默认积分器任务；显示结果时隐藏操纵器并停止物体拖动，返回实时预览后恢复。

中文路径场景保存重开保持位置、旋转和缩放。截图已检查，工具栏和属性字段布局正常。回归结束时保留实际改变了物体的拖拽预览，再退出应用，检查回调与编辑器的销毁顺序。

## 响应时间

20 万面场景由 `scripts/prepare_parameter_probe.py` 生成，包含 204800 个地面三角形及两个 cube，合计 204824 面。

| 指标 | 本机结果 |
| --- | --- |
| studio Debug，最长鼠标回调 | 5.27637ms |
| 密集场景 Release，最长鼠标回调 | 1.10342ms |
| 拖拽期间网格上传与材质描述符重建 | 无 |
| 原有参数回归，最长回调 | 0.766292ms |
| 原有参数回归，下一帧最长等待 | 78ms |
| 原有相机回归 | 6 组拖拽、捕获恢复、两个尺寸和面板恢复通过，最大定时器间隔 158ms |

响应测试采用普通模式，同步验证独立运行。Debug 完整操纵同步验证通过，Vulkan 验证错误为 0。ASan/UBSan 完整操纵与运行中退出通过，未报告地址或未定义行为错误，Vulkan 验证错误为 0；使用 `detect_leaks=0`。

原有线框回归通过可见/遮挡边、HDR 隔离、缓存资产和窗口缩放检查，同步验证错误为 0。原有 14 项参数操作保持网格和描述符复用，后台 OBJ/场景导入及进度显示通过。

## 复验

```sh
cmake --build --preset macos-core-release
ctest --preset macos-core-release --output-on-failure
cmake --build --preset macos-desktop-debug
python3 scripts/verify_gpu.py \
  build/macos-desktop-debug/src/app/desktop/minirender_desktop.app \
  --scene scenes/studio.json --gizmo-check --sync-validation \
  --output outputs/ui3/debug-sync --timeout 150
python3 scripts/prepare_parameter_probe.py work/parameter-probe
cmake --build --preset macos-desktop-release
python3 scripts/verify_gpu.py \
  build/macos-desktop-release/src/app/desktop/minirender_desktop.app \
  --scene work/parameter-probe/dense.json --gizmo-check --timing-only \
  --output outputs/ui3/dense-timing --timeout 240
```

Windows/NVIDIA 桌面、原生鼠标和目标设备 DPI 验证按 B 阶段复验。当前交互证据来自真实 Qt 事件和 GPU 帧。
