# 编辑器样式

日期：2026-11-15。工作台增加自定义深色样式，并将单个物体的属性与全局渲染设置分开停靠。使用方法和模块入口见[编辑器界面](../EDITOR_UI.md)。

## 界面变化

- 顶部采用线形图标，悬停显示名称及快捷键。视口上方保留选择、移动和旋转工具。
- 右侧物体检查器包含物体、几何、布料。左侧场景与渲染面板管理全局配置及独立工具。
- 两组页面保留各自的选择和滚动位置。全局面板可以单独收起，三个面板可以恢复默认停靠位置。
- 输入框、按钮、列表、菜单、滑块、滚动条、复选框和进度条采用一致的颜色及状态。
- XYZ、RGB 分量分别标记，常用数值省略末尾小数零。较窄的表单使用整行向量输入。
- 材质、布料和介质的嵌套表单按实际最小宽度布局，修复缩小窗口时的遮挡。

## 验证范围

新增隐藏窗口检查遍历 12 页，在 1280×800、960×560、1100×700 下检查页面导航、分组独立性、控件边界、滚动到底、数值步进、面板开关与布局恢复。Retina 默认 2× 和增加缩放后的 3× 像素密度各完成 36 组布局检查。截图包含实际 Vulkan 视口。

既有相机六次连续拖拽、物体移动/旋转、14 项参数编辑、离线任务、SSS、布料材质、模拟、介质、光场、几何、点云、SDF 和全部 14 个模板均通过。CPU 核心回归 298/298 通过。

面板宽度随用户显式操作调整，主窗口销毁前释放页面路由器，避免退出阶段的停靠事件继续访问布局。ASan/UBSan 下的全页布局、SSS 与介质专项均通过 Vulkan 同步验证，错误数为 0。几何和渲染任务的关闭检查继续使用原有回归。

## 复验入口

```sh
python3 scripts/verify_gpu.py build/macos-desktop-release/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --theme-check --sync-validation --output outputs/editor-theme --timeout 120
QT_SCALE_FACTOR=1.5 python3 scripts/verify_gpu.py build/macos-desktop-release/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --theme-check --sync-validation --output outputs/editor-theme-dpi --timeout 120
```

关联检查的模式为 `--interaction-check`、`--gizmo-check`、`--parameter-check`、`--offline-check`、`--subsurface-check`、`--cloth-check`、`--simulation-check`、`--volume-check`、`--lightfield-check`、`--geometry-check`、`--points-check`、`--sdf-check` 和 `--presets-check`。计时与同步验证分别运行。

Debug 和 Release 编译数据库包含样式、面板路由和界面检查模块。资源随应用打包，使用固定的 MiniRender.app 更新流程。Windows 桌面实测仍在设备切换阶段进行。
