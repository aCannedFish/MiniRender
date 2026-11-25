// 光场容器的标定、RGB32f 视图数据及有界读取缓存。

#pragma once

#include <minirender/render/lightfield.hpp>

namespace minirender {
namespace detail {
struct LightFieldAccess;
}

// 完整文件的不可变索引，不加载所有像素。采集临时文件的所有权可随结果共享。
class LightFieldFile final {
  public:
    ~LightFieldFile() = default;
    // 校验容器头与标定，建立文件索引；像素按子视图延迟读取。
    static std::shared_ptr<const LightFieldFile> open(const std::filesystem::path& path,
                                                      CancellationToken cancellation = {});
    // 返回文件保存的完整采集标定。
    const LightFieldCalibration& calibration() const noexcept;
    // 返回原始采集实际执行的相机主路径数。
    std::uint64_t traced_paths() const noexcept;
    // 返回完整容器的字节大小。
    std::uint64_t file_bytes() const noexcept;
    // 返回当前容器位置，临时文件的生命周期由共享所有权维持。
    const std::filesystem::path& path() const noexcept;
    // 原子复制完整容器，失败或取消保留已有目标；没有图像重建或射线生成。
    void save(const std::filesystem::path& path, CancellationToken cancellation = {},
              LightFieldCallback progress = {}) const;

  private:
    LightFieldFile()                      = default;
    LightFieldFile(const LightFieldFile&) = default;
    friend class LightFieldReader;
    friend struct detail::LightFieldAccess;
    // 从文件头解析的完整采集标定。
    LightFieldCalibration calibration_;
    // 容器的文件系统位置。
    std::filesystem::path path_;
    // 子视图像素区在文件中的起始字节偏移。
    std::uint64_t payload_offset_ = 0;
    // 通过长度校验的完整文件大小。
    std::uint64_t file_bytes_ = 0;
    // 文件保存的实际主路径计数。
    std::uint64_t paths_ = 0;
    // 共享临时存储所有权，使结果可在采集会话销毁后继续读取。
    std::shared_ptr<const void> lifetime_;
};

// 分子视图流式写入，最多保留一个行缓冲和视图完成标记。
// finish 只在所有视图成功后原子发布完整文件。析构清理自己的 staging。
class LightFieldWriter final {
  public:
    LightFieldWriter(const std::filesystem::path& destination, LightFieldCalibration calibration);
    ~LightFieldWriter();
    LightFieldWriter(const LightFieldWriter&)            = delete;
    LightFieldWriter& operator=(const LightFieldWriter&) = delete;
    // 将指定索引的 RGB32f 子视图写入暂存容器，逐块检查取消。
    void write_view(std::size_t index, const Image& image, CancellationToken cancellation = {},
                    LightFieldCallback progress = {});
    // 在全部子视图写入后发布完整文件，并记录实际采集路径数。
    std::shared_ptr<const LightFieldFile> finish(std::uint64_t traced_paths);

  private:
    struct Impl;
    // 内部文件句柄、逐视图状态或缓存实现的独占所有权。
    std::unique_ptr<Impl> impl_;
};

// 统计实际解码与命中缓存的次数，便于评估重聚焦 I/O。
struct LightFieldCacheStatistics {
    // 从文件实际解码的子视图次数。
    std::uint64_t decoded_views = 0;
    // 直接复用已缓存视图的次数。
    std::uint64_t cache_hits = 0;
    // 当前缓存保有的像素字节数。
    std::uint64_t cached_bytes = 0;
    // 本 reader 生命周期内缓存像素字节数峰值。
    std::uint64_t peak_cached_bytes = 0;
};

// 单个重建线程拥有的 LRU。预算为0时逐视图读取；返回图像不可修改。
// 调用方额外持有的历史图像不属于缓存预算，重建内核每次仅持有一个视图。
class LightFieldReader final {
  public:
    explicit LightFieldReader(std::shared_ptr<const LightFieldFile> file,
                              std::uint64_t cache_budget_bytes = 128ULL * 1024 * 1024);
    ~LightFieldReader();
    // 返回 reader 保有的只读文件索引。
    const LightFieldFile& file() const noexcept;
    // 返回 reader 的 LRU 像素缓存上限。
    std::uint64_t cache_budget_bytes() const noexcept;
    // 返回已解码视图数、缓存命中及内存统计。
    LightFieldCacheStatistics statistics() const noexcept;
    // 读取并校验指定子视图，按预算复用或更新 LRU 缓存。
    std::shared_ptr<const Image> view(std::size_t index, CancellationToken cancellation = {});

  private:
    struct Impl;
    // 内部文件句柄、逐视图状态或缓存实现的独占所有权。
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
