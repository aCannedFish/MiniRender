# 参数响应

日期：2026-11-03。用户反馈：切换直接光照、法线和深度，修改灯光或纹理过滤时，界面会明显卡顿。

## 原因

每次编辑先调用 SceneDocument::validate，再在 make_snapshot 中重复校验，二者都逐三角形扫描网格。SceneRenderer 随后把变换、法线和材质重新写入整场景顶点流，重新分配 GPU 缓冲、重建描述符，并为各方向光遍历顶点计算包围盒。

数值控件原来监听 editingFinished。该信号会在失焦时触发，即使数值没有变化。因此，点击显示模式等控件时，也可能间接提交一次没有变化的场景编辑。

独立切换显示模式在本次自动测试中没有出现同样的长停顿，原路径约 20–40ms。不能把所有历史停顿都归为某一个原因；本次修复消除了已复现的重复工作，并增加等待提示和回归预算。

## 修改

1. SceneEditor 使用 MeshValidationCache，按不可变网格的共享所有权复用校验结果。参数、ID、引用和新网格仍校验。弱引用过期后清理，不延长资产生命周期。
2. GPU 按网格缓存局部空间顶点。同一网格的多个实例复用顶点，变换与材质放入独立 std430 SSBO，着色器通过 draw 的实例索引读取。
3. 只有纹理绑定、实例数量或实例缓冲容量变化时才重建描述符。改变过滤方式只更新实例参数。
4. 网格首次上传时保存局部 AABB；场景更新时变换八个角点生成保守世界包围盒，避免逐顶点重新拟合阴影。
5. 数值控件使用 valueChanged，保留 keyboardTracking=false。键盘输入在 Enter/失焦时提交，步进立即生效；没有数值变化时不生成编辑事务。名称输入也检查修改状态。
6. 打开场景、OBJ 导入和纹理解码在 QtConcurrent 工作线程执行。工作线程不操作控件或 Vulkan；主线程通过 watcher 安装结果，并丢弃旧文档的过期结果。

## 等待提示

状态栏增加任务文字和不显示百分比的忙碌条，说明正在读取场景、读取并校验网格、读取纹理并生成 mipmap。预览版本等待超过 150ms 时显示“正在更新预览…”，对应帧的 GPU fence 完成后收起。持续拖拽期间若帧仍不断完成，不会把正常更新累计成一次长等待。

实时预览没有采样总数，当前使用不确定进度。A2 离线渲染再按已完成样本数显示可计量进度。此改动没有用定时递增的百分比模拟完成度。

Vulkan 资源首次创建、上传录制及场景安装仍在主线程按帧边界执行。特别大的新资产首次安装仍可能产生短暂开销；普通参数编辑已经不经过这条网格上传路径。

## 性能对照

同一 Apple M3、Debug 构建、Vulkan Validation 开启，场景包含 204,824 个三角形。测试通过实际 Qt 控件修改灯光、过滤、粗糙度、变换，并往返切换三种显示模式。等待指标包含定时器检查，并要求编辑后至少有两帧推进，测量使用 CPU 墙钟，不记录 GPU timestamp 或精确的屏幕显示延迟。

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 六次数值/过滤编辑的 UI 回调 | 238–301ms | 0.51–0.89ms |
| 编辑后观察到两帧推进 | 771–982ms | 58–65ms |
| 三种显示模式切换 | 20–40ms | 19–40ms |
| 普通参数编辑的网格上传/描述符重建 | 每次全量重建 | 均为 0 次 |

修复前日志：work/parameter-probe/baseline.log。最终回归日志：outputs/parameter-final/gpu-smoke.log。生成的大网格与图像保留在忽略目录，不进入 Git。

## 验证

```sh
python3 scripts/prepare_parameter_probe.py work/parameter-probe
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app --scene work/parameter-probe/dense.json --parameter-check --output outputs/parameter-final --timeout 80
```

参数测试检查 12 次控件操作、无变化的失焦提交、GPU 缓存计数、后台 OBJ/场景导入、忙碌条出现和 GPU 完成后收起。回归预算为控件回调小于 50ms、两帧推进小于 500ms，用于本机 Debug 检查，不作为其他硬件的性能承诺。

CPU-only Debug 与 desktop Debug CTest 均为 24/24 通过，包括新增的缓存校验测试，确认新非法几何、坏引用与非法相机仍被拒绝。

既有 GPU 回归通过：六次连续拖拽、两种窗口尺寸、侧栏恢复；纹理的三种过滤与 CPU 参考一致；多光源阴影 127/127 遮挡样本及 1124/1124 受光样本一致；法线 55/55、深度 54/54；HDR/EXR、曝光、FXAA、resize 均通过。各 GPU 运行的 Validation 均为 0 errors。

## 阅读顺序

1. scene.hpp / scene.cpp：不可变网格与校验缓存。
2. editor.cpp：缓存如何参与编辑事务和快照生成。
3. scene_renderer.cpp 的 upload_scene：网格缓存、实例缓冲和描述符更新条件。
4. scene_instance.glsl、scene.vert、shadow.vert：局部坐标到世界坐标及实例参数读取。
5. workbench.cpp 的 run_background、begin_asset_task：后台操作与 UI 状态。
6. vulkan_viewport.cpp 的 mark_preview_changed 和 render_frame：预览版本与 fence 完成状态。
7. parameter_smoke.cpp：控件、响应时间、资产缓存与任务提示的组合检查。

接口依据：[Qt keyboardTracking](https://doc.qt.io/qt-6/qabstractspinbox.html#keyboardTracking-prop)、[Khronos Shader Memory Layout](https://docs.vulkan.org/guide/latest/shader_memory_layout.html)。

后续 A1.6 连续测试另外捕获到间歇的 QMetalLayer drawable 等待，并补充 Qt 的非事务 Metal layer 配置。详情见 [A1.6](A1.6.md)。
