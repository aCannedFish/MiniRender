#pragma once

#include <filesystem>
#include <fstream>

namespace minirender::detail {

// 目标目录中的独占临时目录拥有所有中间文件。析构只清理该目录，失败不触碰目标。
// commit 关闭流后在同一文件系统中原子替换目标；对象不可复制或移动。
class StagedFile {
  public:
    explicit StagedFile(const std::filesystem::path& destination);
    ~StagedFile();
    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;
    std::ostream& stream() noexcept;
    const std::filesystem::path& directory() const noexcept;
    void commit();

  private:
    std::filesystem::path destination_, directory_, temporary_;
    std::ofstream stream_;
};

} // namespace minirender::detail
