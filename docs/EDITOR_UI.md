# 编辑器界面

工作台使用深灰背景、蓝色选中状态和统一的线形图标。输入框、按钮、下拉列表、滚动条、复选框和进度条共用一套样式。鼠标停留在工具栏图标上可以查看功能和快捷键。

## 面板布局

右侧“物体检查器”包含物体、几何和布料三页，用于修改选中物体的变换、材质、网格及模拟参数。位置与旋转用 XYZ 标记，颜色用 RGB 标记；输入框边缘的颜色帮助区分分量。

左侧上方为场景对象与资源列表，下方“场景与渲染”放置灯光、相机、离线、实时、环境、介质和光场设置，以及点云、距离场工具。切换这里的页面时，右侧物体页保持原状态。两个面板均有独立的滚动位置。

工具栏的“渲染设置”可收起或展开全局面板。面板标题可拖动停靠，右上角提供浮动和关闭按钮。“恢复布局”重新打开三个面板并恢复默认位置。窗口较窄时，三分量输入的标签自动换行。

## 常用操作

- 视口上方切换选择、移动和旋转，也可在视口使用 V、T、R。右侧下拉框选择[原色预览](BASE_COLOR_PREVIEW.md)、光照和诊断模式。
- 顶部图标提供打开、保存、另存、撤销、重做、资源导入、复制、删除、截图和 EXR 导出。
- “快速导入”加载内置场景副本，之后可继续修改和另存。
- 键盘焦点显示蓝色边框；禁用控件使用较暗的文字和背景。
- 原色预览的轮廓与接触遮蔽在“实时”页分别开关，轮廓强度可调整。
- 状态栏显示设备、对象数量及当前视图；相机操作提示放在状态栏的悬停说明中。

## 实现位置

| 文件 | 内容 |
| --- | --- |
| `src/app/desktop/editor.qss` | 色板、间距、边框、悬停、焦点、选中及禁用样式 |
| `editor_theme.cpp` | Fusion 样式、调色板、字体、线形图标、导航按钮与面板标题 |
| `workbench_panels.cpp` | 页面注册与分组、独立面板、显示切换和布局恢复 |
| `workbench.cpp` | 工作台操作、表单字段和视口工具栏 |
| `theme_smoke.cpp` | 页面切换、互不干扰、边界、滚动、步进按钮与截图检查 |

图标由 QPainter 生成 1×、2×、3× 图像。下拉箭头和复选标记使用 QtGui 可读取的 XPM 资源，样式表和图形随可执行文件打包。数字显示省略小数末尾的零，范围、精度和步进仍由原数值控件管理。

页面注册顺序在 `Workbench` 中集中定义。全局编号映射到两个 QTabWidget 的局部编号，场景导入和已有回归通过同一接口切换页面。界面不会为了换页重新创建参数面板。

## 复验

```sh
cmake --preset macos-desktop-release
cmake --build --preset macos-desktop-release --parallel 4
python3 scripts/verify_gpu.py build/macos-desktop-release/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --theme-check --sync-validation --output outputs/editor-theme --timeout 120
```

检查默认使用隐藏窗口。另设 `QT_SCALE_FACTOR=1.5` 可检查分数缩放下的布局；在本机 Retina 屏上对应 3× 像素密度。

实现参考 Qt 的 [Style Sheets Examples](https://doc.qt.io/qt-6/stylesheet-examples.html) 和 [QProxyStyle](https://doc.qt.io/qt-6/qproxystyle.html) 文档。
