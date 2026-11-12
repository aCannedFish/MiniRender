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
    static std::shared_ptr<const LightFieldFile> open(const std::filesystem::path& path,
                                                      CancellationToken cancellation = {});
    const LightFieldCalibration& calibration() const noexcept;
    std::uint64_t traced_paths() const noexcept;
    std::uint64_t file_bytes() const noexcept;
    const std::filesystem::path& path() const noexcept;
    // 原子复制完整容器，失败或取消保留已有目标；没有图像重建或射线生成。
    void save(const std::filesystem::path& path, CancellationToken cancellation = {},
              LightFieldCallback progress = {}) const;

  private:
    LightFieldFile() = default;
    LightFieldFile(const LightFieldFile&) = default;
    friend class LightFieldReader;
    friend struct detail::LightFieldAccess;
    LightFieldCalibration calibration_;
    std::filesystem::path path_;
    std::uint64_t payload_offset_ = 0, file_bytes_ = 0, paths_ = 0;
    std::shared_ptr<const void> lifetime_;
};

// 分子视图流式写入，最多保留一个行缓冲和视图完成标记。
// finish 只在所有视图成功后原子发布完整文件。析构清理自己的 staging。
class LightFieldWriter final {
  public:
    LightFieldWriter(const std::filesystem::path& destination, LightFieldCalibration calibration);
    ~LightFieldWriter();
    LightFieldWriter(const LightFieldWriter&) = delete;
    LightFieldWriter& operator=(const LightFieldWriter&) = delete;
    void write_view(std::size_t index, const Image& image, CancellationToken cancellation = {},
                    LightFieldCallback progress = {});
    std::shared_ptr<const LightFieldFile> finish(std::uint64_t traced_paths);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct LightFieldCacheStatistics {
    std::uint64_t decoded_views = 0, cache_hits = 0;
    std::uint64_t cached_bytes = 0, peak_cached_bytes = 0;
};

// 单个重建线程拥有的 LRU。预算为0时逐视图读取；返回图像不可修改。
// 调用方额外持有的历史图像不属于缓存预算，重建内核每次仅持有一个视图。
class LightFieldReader final {
  public:
    explicit LightFieldReader(std::shared_ptr<const LightFieldFile> file,
                              std::uint64_t cache_budget_bytes = 128ULL * 1024 * 1024);
    ~LightFieldReader();
    const LightFieldFile& file() const noexcept;
    std::uint64_t cache_budget_bytes() const noexcept;
    LightFieldCacheStatistics statistics() const noexcept;
    std::shared_ptr<const Image> view(std::size_t index, CancellationToken cancellation = {});

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace minirender
