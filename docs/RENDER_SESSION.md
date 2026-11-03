# 渲染任务

## 职责

`RenderSession` 管理异步准备、多线程采样、暂停、恢复、取消和图像快照，不依赖 Qt。控制器线程构建 `PathTracer`，创建并收回工作线程，生成最终图像。工作线程只访问固定场景和自己的 tile，不操作 UI 或 Vulkan。

生命周期操作由同一个控制线程串行调用。`progress`、`preview` 可以并发读取。`cancel` 只发出请求，`wait` 才等待线程收回；UI 应轮询状态，在任务终止或销毁时调用 `wait`。

```cpp
minirender::RenderSession session;
session.start(snapshot, render_settings, integrator_settings, {4, 16, 4});
while (!session.wait_for(std::chrono::milliseconds(200))) {
    const auto progress = session.progress();
    // progress.recorded_samples / progress.total_samples 是实际记录进度。
}
session.wait();
const auto image = session.result();
```

## 状态

| 状态 | 含义 |
| --- | --- |
| `idle` | 尚无任务 |
| `preparing` | 后台构建场景与工作缓冲，可取消 |
| `running` | 工作线程采样，可暂停或取消 |
| `pausing` | 已发出暂停请求，等待所有存活线程停稳 |
| `paused` | 所有存活线程已停在路径边界，记录数保持不变 |
| `finalizing` | 采样完成，后台转换完整图像并释放工作缓冲 |
| `cancelling` | 已请求取消，等待线程和资源收回 |
| `cancelled` | 已取消，可读取部分预览 |
| `completed` | 完整结果就绪 |
| `failed` | 准备或采样异常，错误信息可读取 |

准备和最终整理期间不接受暂停。取消会唤醒已暂停线程；异常也会清除暂停请求，避免失败任务停留在等待状态。终止状态发布前收回工作线程和加速结构资源。

每次启动生成新的 `job_id` 和取消状态。旧图像为独立不可变对象，下一任务不修改它。`job_id` 标识任务，文档是否修改由上层 UI 另行判断。

## 调度与累计

线程通过原子索引领取 tile，一个 tile 的全部样本由同一个线程负责。像素内始终按 `sample_index=0,1,...` 累加 double RGB，因此线程数、tile 大小和提交批次不改变结果。

默认 tile 为 16×16，每批提交 4 个样本。提交携带各像素从样本 0 开始的和及实际计数。批次先完整检查图像数值范围，再一起发布像素与计数，异常不会留下半个已验证批次。

部分图像按每像素自己的 $n_p$ 归一化：$I_p=S_p/n_p$，未计算像素为黑色。进度为

$$progress=\frac{\sum_p n_p}{width\,height\,SPP}.$$

暂停时线程局部缓冲可能有尚未提交的样本，它们不计入 `recorded_samples`。取消时会提交已经算完的局部样本。`PathStatistics::paths` 统计实际开始计算的路径，取消或失败时可以高于已记录数。

自动线程数保留一个 CPU 核给其他工作，最多 32 个；也可显式指定。每个线程使用独立统计对象，按 128 字节对齐，避免 M3 上相邻统计计数争用缓存行。

## 同步

控制锁保护状态、暂停协议和任务所有权。Film 锁保护像素提交和预览，两种锁不同时持有。正常路径只读取暂停/取消原子标记，需要暂停时才进入条件变量等待。

`CancellationToken` 为共享原子标记。BVH 构造、节点/图元遍历、光源准备、环境图处理及路径顶点均检查取消，使用专用异常返回任务控制器。取消与算法失败分别映射为 `cancelled` 和 `failed`。

控制器持有工作线程所引用的数据，所有异常分支均先请求停止并 join。下一任务启动时，上一任务的大缓冲在后台释放，避免在 UI 调用 `start` 时做大块析构。

## 图像发布

`preview` 使用 `try_lock`，缓冲忙时返回空图像，上层保留上一帧。默认最长边为 1024，使用最近像素预览，避免每次扫描整个高分辨率工作缓冲。返回图像独立于 Film，worker 的后续提交不会改写它。

`preview(0)` 请求完整分辨率，宜用于后台导出。最终图像也在后台生成，`result` 只在 `completed` 时提供共享的完整线性图像。曝光和显示转换继续使用 `display_image`，不改变累计数据。

## CLI

```sh
build/macos-core-release/minirender_cli --render scenes/path-room.json outputs/room --width 160 --height 160 --spp 128 --threads 4 --tile 16 --batch 4
```

CLI 现在通过同一个任务核心渲染，定期输出准备/采样状态和记录数。`--threads 0` 使用自动选择。单线程 `PathTracer::render` 保留为参考实现。

## 阅读顺序

1. `cancellation.hpp`：取消状态的所有权与异常。
2. `render_session.hpp`：状态、进度、预览和生命周期约定。
3. `Film`：批次验证、实际计数、归一化及不可变预览。
4. `Impl::checkpoint`：暂停条件与快速恢复/再次暂停的处理。
5. `Impl::run`：准备、tile 调度、局部统计、异常和线程收回。
6. 公共 `start/cancel/progress/preview`：控制线程与后台的数据边界。
7. `session_tests.cpp`：串行一致性、实时预览、暂停、取消、失败与析构检查。
