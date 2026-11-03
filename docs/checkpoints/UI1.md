# UI 修复

日期：2026-11-03。用户反馈：属性组件偏移、遮挡；相机拖拽一次后容易卡顿或不能继续拖拽。

## 交互修复

- 移除嵌入式 QWindow 在每次 mouse press 中的手工 requestActivate，焦点交由 QWidget 容器管理。
- 鼠标状态按本视口的 press/release 跟踪；外部窗口带入的按钮状态不会启动相机拖拽。
- 释放、窗口失活、隐藏和丢失捕获时清理拖拽状态。键盘焦点变化不等同于丢失鼠标捕获。
- macOS 进程使用 MoltenVK 官方异步提交模式，避免主线程在 vkQueueSubmit 的 Metal nextDrawable 等待中受阻。
- 普通帧轮询 GPU fence，图像获取采用有界等待；显式截图和 HDR 回读保留有限等待。
- 相机参数始终即时写入文档；可见相机面板的数值刷新合并到 33ms 更新，减少高频控件重排。

线程采样中，原始路径在 vkQueueSubmit → nextDrawable 上消耗主线程时间。仅将提交异步化还不充分，因此同时修正了子窗口激活、鼠标状态和帧调度。原始采样保存在本地 work/drag-hang.txt。

## 布局修复

- 属性页加入可滚动容器，窄窗口下可以访问末尾控件。
- 表单字段统一伸缩；XYZ 分量使用相同列宽约束，长行按需要换行。
- 标签页采用 document mode，提示文字换行，初始窗口尺寸参考可用屏幕。
- 增加“恢复布局”按钮，用于重新显示或停靠侧栏。

## 验证

```sh
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene scenes/studio.json --interaction-check --output outputs/ui-fix --timeout 40
```

程序内端到端测试跨事件循环执行六次鼠标序列，覆盖环绕、转向和平移。每次拖拽都要求相机变化、渲染继续推进，并检查 release 后停止响应、丢失捕获取消拖拽。独立 heartbeat 检查 UI 事件循环。

在 960×560 和 1180×760 两种窗口尺寸下，逐页检查物体、灯光、相机表单的水平边界和字段重叠，并确认可以滚动到最末控件。关闭侧栏后，恢复布局必须重新显示它。

本机结果：六次拖拽期间帧数分别推进 20–25 帧；测试 heartbeat 为 307，最大测试 tick 间隔 168ms（含 resize）；Vulkan Validation 为 0 errors。

既有综合 GPU 回归通过：HDR/EXR、曝光、FXAA、法线/深度、resize、多光源阴影及纹理 CPU/GPU 对照均通过，Validation 为 0 errors。CPU CTest 23/23 通过。截图复核了新布局；外部桌面工具对自定义 Qt 视口的原生 drag 操作返回不支持，因此没有把该路径记录为通过。

## 开发状态

本次修复独立提交，不包含尚未验收的线框扩展。线框工作保存在本地 Git stash，标签为 `WIP wireframe and initial UI fixes before dedicated repair`，原分支为 codex/a1-editor-completion。恢复时应仅提取线框相关改动，保留本次已验证的交互修复，避免整包覆盖旧 UI 实验。

按用户要求，本修复完成并推送后暂停，A1 其余任务及 A2–A9 尚未完成。

MoltenVK 配置依据：[v1.4.2 官方参数文档](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/Docs/MoltenVK_Configuration_Parameters.md#mvk_config_synchronous_queue_submits)。
