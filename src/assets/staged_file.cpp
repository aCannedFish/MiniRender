#include "staged_file.hpp"

#include <locale>
#include <random>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace minirender::detail {

StagedFile::StagedFile(const std::filesystem::path& destination)
    : destination_(std::filesystem::absolute(destination).lexically_normal()) {
    const auto parent = destination_.parent_path();
    std::filesystem::create_directories(parent);
    std::random_device random;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const auto candidate = parent / (".minirender-tmp-" + std::to_string(random()) + "-" +
                                         std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            directory_ = candidate;
            break;
        }
        if (error && error != std::errc::file_exists) {
            throw std::filesystem::filesystem_error("Cannot create export staging directory",
                                                    candidate, error);
        }
    }
    if (directory_.empty()) {
        throw std::runtime_error("Cannot reserve an export staging directory");
    }
    temporary_ = directory_ / "pending";
    try {
        stream_.exceptions(std::ios::badbit | std::ios::failbit);
        stream_.open(temporary_, std::ios::binary);
        stream_.imbue(std::locale::classic());
    } catch (...) {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        throw;
    }
}

StagedFile::~StagedFile() {
    stream_.exceptions(std::ios::goodbit);
    stream_.close();
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
}
std::ostream& StagedFile::stream() noexcept {
    return stream_;
}
const std::filesystem::path& StagedFile::directory() const noexcept {
    return directory_;
}
void StagedFile::commit() {
    stream_.flush();
    stream_.close();
#ifdef _WIN32
    // filesystem::rename 在 Windows 不替换已有目标；MoveFileExW 提供同卷替换语义。
    if (!MoveFileExW(temporary_.c_str(), destination_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::filesystem::filesystem_error(
            "Cannot commit exported file", temporary_, destination_,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
#else
    std::filesystem::rename(temporary_, destination_);
#endif
}

} // namespace minirender::detail
