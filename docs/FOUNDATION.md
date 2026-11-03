# 基础实现

## 阅读顺序

1. `CMakeLists.txt`：核心、CLI、测试、桌面目标的依赖边界。
2. `image.hpp` 与 `image.cpp`：线性像素存储和唯一的显示编码边界。
3. `cli.cpp` 与 `core_tests.cpp`：无 GPU 的可执行入口和标准数值验证。
4. 桌面 `main.cpp`：Qt 控件、诊断命令行、测试时序和错误统计。
5. `vulkan_viewport.cpp`：device、swapchain、同步、图像回读与窗口事件。
6. `diagnostic_renderer.cpp` 和两个 GLSL shader：纹理上传、描述符、管线和 draw。

## 颜色约定

Image 存储线性 RGB，允许大于 1 的 HDR 值。sRGB 转换只在输出执行，分段函数分别处理暗部线性段与幂函数段。PPM 诊断输出不做色调映射，而是编码后截断；后续真实 HDR 输出另行实现曝光和色调映射。

GPU 棋盘采用 SRGB 纹理格式，硬件采样自动解码为线性 RGB。片段 shader 输出线性值，SRGB swapchain 再编码。不要在 shader 中重复 gamma 校正。

## 资源所有权

Qt 的 QVulkanInstance/QWindow 管理平台 surface。VulkanViewport 管理 device、queue、swapchain、command pool、fence 和 swapchain 图像视图；DiagnosticRenderer 管理纹理、采样器、描述符和图形管线。

窗口可以在 C++ 析构前收到 SurfaceAboutToBeDestroyed 事件，因此要在该事件中先等待设备、销毁 swapchain 相关资源，再允许 Qt 销毁 surface。仅在类析构时释放会违反 surface 生命周期要求。

初始化过程中失败的已创建资源同样通过 release 路径回收。ShaderModule 使用 RAII，创建管线失败时也不会泄漏临时 shader。

## 同步约定

首版同一时刻只提交一帧。CPU 在重用 command buffer 和深度附件前等待 frame fence。Acquire 信号量只在上一次提交完成并消费后重用。

呈现信号量按 swapchain image 分配。重新 acquire 同一图像证明此前关联的呈现已推进到可重用状态，再使用对应信号量。不能按 CPU 在途帧索引选择呈现信号量，两个索引的循环顺序没有一致性保证。

纹理上传有两次布局转换：UNDEFINED → TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL。transfer 写入经 barrier 对 fragment shader 读取可见。帧绘制完成后，正常帧转换为 PRESENT_SRC_KHR；回读帧先转换到 TRANSFER_SRC_OPTIMAL 并拷贝到 host coherent buffer，再转为呈现布局。

resize 和关闭时等待 device idle 后回收资源。图形与呈现使用同一 queue family，首版不涉及 queue ownership 转移。

## Qt 适配

初始 QVulkanWindow 原型在 Qt 6.11.2 / MoltenVK 1.4.2 上发现两项问题：其呈现信号量按在途帧复用触发 VUID-vkQueueSubmit-pSignalSemaphores-00067；在 CALayer expose 事务内同步提交时可能持续占据事件处理过程。

因此采用 QWindow 与自有交换链。窗口 expose/resize 只安排下一次渲染，实际提交由 timer 回调执行，使 Cocoa 事务先完成。没有禁用 Validation 错误，也没有把没有渲染的窗口算作成功。

该决定保留 Qt UI 和 Vulkan 路线，将基础资源管理显式放入项目，便于学习和后续离屏 pass 扩展。

## 验证方式

GPU 检查先画近处棋盘，再画远处红色三角形。若深度测试未生效，中心会被红色覆盖。回读检查两种棋盘颜色、红色边缘的面积和中心颜色；同时确认窗口缩放导致交换链重建。

关闭窗口和销毁 instance 后再检查 Validation 计数，避免只验证绘制期间。外层 Python watchdog 独立于 Qt 事件循环，能报告主线程停滞。

## 来源

- [Qt Vulkan 窗口源码](https://github.com/qt/qtbase/blob/v6.11.2/src/gui/vulkan/qvulkanwindow.cpp)：核对生命周期、平台呈现通知和初始原型问题。
- [Khronos Swapchain Semaphore Reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)：交换链图像与呈现信号量的关系。
- [MoltenVK Runtime Guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)：Metal 运行与 portability 设备要求。

后续 UI 修复在 macOS 进程内启用 MoltenVK 异步提交，普通帧改为轮询 fence，并取消对嵌入窗口的手工激活，记录见 checkpoints/UI1.md。
