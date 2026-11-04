#include "asset_writer.hpp"
#include "obj_writer.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace minirender::detail {
namespace {

constexpr std::size_t block_size = 65536;

void report(CancellationToken cancellation, const SceneSaveCallback& callback, SceneSaveStage stage,
            std::size_t index, std::size_t count, std::size_t completed, std::size_t total) {
    cancellation.check();
    if (callback) {
        callback({stage, index, count, completed, total});
    }
    cancellation.check();
}

// 分块比较也用于防范内容名的哈希碰撞，不将哈希相同直接当作文件相同。
bool equal_files(const std::filesystem::path& first, const std::filesystem::path& second,
                 std::size_t index, std::size_t count, CancellationToken cancellation,
                 const SceneSaveCallback& progress) {
    if (!std::filesystem::is_regular_file(second) ||
        std::filesystem::file_size(first) != std::filesystem::file_size(second)) {
        return false;
    }
    const auto total = static_cast<std::size_t>(std::filesystem::file_size(first));
    report(cancellation, progress, SceneSaveStage::compare, index, count, 0, total);
    std::ifstream a(first, std::ios::binary), b(second, std::ios::binary);
    if (!a || !b) {
        throw std::runtime_error("Cannot compare exported scene assets");
    }
    std::array<char, block_size> ba{}, bb{};
    std::size_t completed = 0;
    while (completed < total) {
        cancellation.check();
        const auto size = static_cast<std::streamsize>(std::min(block_size, total - completed));
        a.read(ba.data(), size);
        b.read(bb.data(), size);
        if (a.gcount() != size || a.bad()) {
            throw std::runtime_error("Cannot read exported scene assets");
        }
        if (b.gcount() != size || b.bad()) {
            return false;
        }
        if (!std::equal(ba.begin(), ba.begin() + size, bb.begin())) {
            return false;
        }
        completed += static_cast<std::size_t>(size);
        report(cancellation, progress, SceneSaveStage::compare, index, count, completed, total);
    }
    return true;
}

// FNV-1a 仅用于稳定文件名，随后仍比较完整字节；它不承担安全校验职责。
std::string content_name(const std::filesystem::path& file, std::size_t index, std::size_t count,
                         CancellationToken cancellation, const SceneSaveCallback& progress) {
    const auto total = static_cast<std::size_t>(std::filesystem::file_size(file));
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read staged mesh");
    }
    std::uint64_t hash = 14695981039346656037ULL;
    std::array<char, block_size> buffer{};
    std::size_t completed = 0;
    report(cancellation, progress, SceneSaveStage::hash, index, count, 0, total);
    while (completed < total) {
        cancellation.check();
        const auto size = static_cast<std::streamsize>(std::min(block_size, total - completed));
        stream.read(buffer.data(), size);
        if (stream.gcount() != size || stream.bad()) {
            throw std::runtime_error("Cannot hash staged mesh");
        }
        for (std::streamsize i = 0; i < size; ++i) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash *= 1099511628211ULL;
        }
        completed += static_cast<std::size_t>(size);
        report(cancellation, progress, SceneSaveStage::hash, index, count, completed, total);
    }
    std::ostringstream name;
    name.imbue(std::locale::classic());
    name << "generated-" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return name.str();
}

// 链接或复制均拒绝覆盖已有目标，遇到并发占名则换名。
std::filesystem::path publish(const std::filesystem::path& source,
                              const std::filesystem::path& destination, const std::string& prefix,
                              const std::filesystem::path& extension, std::size_t index,
                              std::size_t count, CancellationToken cancellation,
                              const SceneSaveCallback& progress) {
    const auto assets = destination / "assets";
    std::filesystem::create_directories(assets);
    for (std::uint64_t suffix = 0;; ++suffix) {
        cancellation.check();
        auto name =
            std::filesystem::path(prefix + (suffix == 0 ? "" : "-" + std::to_string(suffix)));
        name += extension;
        const auto candidate = assets / name;
        if (std::filesystem::exists(candidate)) {
            if (equal_files(source, candidate, index, count, cancellation, progress)) {
                return std::filesystem::path("assets") / name;
            }
            continue;
        }
        report(cancellation, progress, SceneSaveStage::copy, index, count, 0, 1);
        std::error_code error;
        // 同卷硬链接直接发布完整暂存文件；不支持硬链接时退回不覆盖复制。
        std::filesystem::create_hard_link(source, candidate, error);
        if (error && error != std::errc::file_exists) {
            error.clear();
            std::filesystem::copy_file(source, candidate, std::filesystem::copy_options::none,
                                       error);
        }
        if (error == std::errc::file_exists) {
            continue;
        }
        if (error) {
            throw std::filesystem::filesystem_error("Cannot publish scene asset", source, candidate,
                                                    error);
        }
        report(cancellation, progress, SceneSaveStage::copy, index, count, 1, 1);
        return std::filesystem::path("assets") / name;
    }
}

} // namespace

std::filesystem::path portable_asset_path(const std::filesystem::path& source,
                                          const std::filesystem::path& destination,
                                          std::size_t index, std::size_t count,
                                          CancellationToken cancellation,
                                          const SceneSaveCallback& progress) {
    cancellation.check();
    if (!std::filesystem::is_regular_file(source)) {
        const auto utf8 = source.generic_u8string();
        throw std::runtime_error("Missing scene asset file: " +
                                 std::string(utf8.begin(), utf8.end()));
    }
    const auto relative = std::filesystem::relative(source, destination);
    if (!relative.empty()) {
        return relative;
    }
    return publish(source, destination, "asset", source.extension(), index, count, cancellation,
                   progress);
}

std::filesystem::path write_generated_asset(const MeshData& mesh,
                                            const std::filesystem::path& destination,
                                            const std::filesystem::path& staging, std::size_t index,
                                            std::size_t count, CancellationToken cancellation,
                                            const SceneSaveCallback& progress) {
    const auto temporary = staging / ("mesh-" + std::to_string(index) + ".obj");
    std::ofstream stream;
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.open(temporary, std::ios::binary);
    write_obj_content(mesh, stream, cancellation, [&](auto completed, auto total) {
        report(cancellation, progress, SceneSaveStage::mesh, index, count, completed, total);
    });
    stream.close();
    const auto name = content_name(temporary, index, count, cancellation, progress);
    return publish(temporary, destination, name, ".obj", index, count, cancellation, progress);
}

} // namespace minirender::detail
