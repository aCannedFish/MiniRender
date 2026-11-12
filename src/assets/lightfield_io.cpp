#include "staged_file.hpp"
#include <minirender/assets/lightfield_io.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <list>
#include <map>
#include <stdexcept>

namespace minirender {
namespace {
using Json = nlohmann::json;
constexpr std::array<char, 8> magic{'M', 'R', 'L', 'F', '\r', '\n', '\0', '\0'};
constexpr std::uint64_t header_bytes = 40;
constexpr std::uint64_t maximum_metadata = 1024 * 1024;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

void put(std::ostream& stream, std::uint64_t value, std::size_t bytes) {
    std::array<char, 8> encoded{};
    for (std::size_t b = 0; b < bytes; ++b) {
        encoded[b] = static_cast<char>((value >> (8 * b)) & 255U);
    }
    stream.write(encoded.data(), static_cast<std::streamsize>(bytes));
}
std::uint64_t get(std::istream& stream, std::size_t bytes) {
    std::array<unsigned char, 8> encoded{};
    stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(bytes));
    std::uint64_t result = 0;
    for (std::size_t b = 0; b < bytes; ++b) {
        result |= std::uint64_t(encoded[b]) << (8 * b);
    }
    return result;
}
std::uint64_t integer(const Json& value, std::uint64_t maximum) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            throw std::invalid_argument("Negative light field integer");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        throw std::invalid_argument("Light field metadata requires an integer");
    }
    if (result > maximum) {
        throw std::invalid_argument("Invalid integer in light field metadata");
    }
    return result;
}
Json vector(glm::vec3 value) {
    return {value.x, value.y, value.z};
}
glm::vec3 vector(const Json& value) {
    if (!value.is_array() || value.size() != 3) {
        throw std::invalid_argument("Light field camera requires three vector components");
    }
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}
Json encode(const LightFieldCalibration& c) {
    const auto& camera = c.camera;
    Json pupils = Json::array();
    for (const auto p : c.pupils) {
        pupils.push_back({p.x, p.y});
    }
    return {{"camera",
             {{"position", vector(camera.position)},
              {"target", vector(camera.target)},
              {"up", vector(camera.up)},
              {"fov", camera.vertical_fov_degrees},
              {"near", camera.near_plane},
              {"far", camera.far_plane},
              {"aperture", camera.aperture_radius},
              {"focus", camera.focus_distance}}},
            {"width", c.render.width},
            {"height", c.render.height},
            {"spp", c.render.samples_per_pixel},
            {"seed", c.render.seed},
            {"columns", c.columns},
            {"rows", c.rows},
            {"radius", c.pupil_radius},
            {"reference_focus", c.reference_focus},
            {"pupils", pupils},
            {"integrator",
             {{"mode", sampling_mode_name(c.integrator.mode)},
              {"depth", c.integrator.maximum_depth},
              {"rr", c.integrator.russian_roulette},
              {"rr_depth", c.integrator.roulette_depth}}}};
}
LightFieldCalibration decode(const Json& json) {
    LightFieldCalibration c;
    const auto& camera = json.at("camera");
    c.camera.position = vector(camera.at("position"));
    c.camera.target = vector(camera.at("target"));
    c.camera.up = vector(camera.at("up"));
    c.camera.vertical_fov_degrees = camera.at("fov").get<float>();
    c.camera.near_plane = camera.at("near").get<float>();
    c.camera.far_plane = camera.at("far").get<float>();
    c.camera.aperture_radius = camera.at("aperture").get<float>();
    c.camera.focus_distance = camera.at("focus").get<float>();
    c.render.width = std::size_t(integer(json.at("width"), 16384));
    c.render.height = std::size_t(integer(json.at("height"), 16384));
    c.render.samples_per_pixel =
        std::uint32_t(integer(json.at("spp"), std::numeric_limits<std::uint32_t>::max()));
    c.render.seed = integer(json.at("seed"), std::numeric_limits<std::uint64_t>::max());
    c.columns = std::uint32_t(integer(json.at("columns"), 15));
    c.rows = std::uint32_t(integer(json.at("rows"), 15));
    c.pupil_radius = json.at("radius").get<double>();
    c.reference_focus = json.at("reference_focus").get<double>();
    const auto& pupils = json.at("pupils");
    if (!pupils.is_array() || pupils.size() != c.view_count()) {
        throw std::invalid_argument("Invalid light field pupil array");
    }
    for (const auto& p : pupils) {
        if (!p.is_array() || p.size() != 2) {
            throw std::invalid_argument("Invalid light field pupil coordinate");
        }
        c.pupils.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
    }
    const auto& integrator = json.at("integrator");
    c.integrator.mode = parse_sampling_mode(integrator.at("mode").get<std::string>());
    c.integrator.maximum_depth = std::uint32_t(integer(integrator.at("depth"), 128));
    c.integrator.russian_roulette = integrator.at("rr").get<bool>();
    c.integrator.roulette_depth = std::uint32_t(integer(integrator.at("rr_depth"), 128));
    c.validate();
    return c;
}
void check_pixel(glm::vec3 p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        throw std::invalid_argument("Light field contains a nonfinite pixel");
    }
}
} // namespace

std::shared_ptr<const LightFieldFile> LightFieldFile::open(const std::filesystem::path& path,
                                                           CancellationToken cancellation) {
    cancellation.check();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open light field file");
    }
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    std::array<char, 8> signature{};
    stream.read(signature.data(), std::streamsize(signature.size()));
    if (signature != magic || get(stream, 4) != 1) {
        throw std::invalid_argument("Unsupported light field signature/version");
    }
    const auto metadata_bytes = get(stream, 4), payload_bytes = get(stream, 8),
               paths = get(stream, 8), complete = get(stream, 8);
    if (complete != 1 || metadata_bytes == 0 || metadata_bytes > maximum_metadata) {
        throw std::invalid_argument("Incomplete light field or invalid metadata length");
    }
    auto file = std::shared_ptr<LightFieldFile>(new LightFieldFile);
    file->path_ = std::filesystem::absolute(path).lexically_normal();
    std::string text(std::size_t(metadata_bytes), '\0');
    stream.read(text.data(), std::streamsize(text.size()));
    file->calibration_ = decode(Json::parse(text));
    file->payload_offset_ = header_bytes + metadata_bytes;
    const auto expected = file->calibration_.view_bytes() * file->calibration_.view_count();
    if (payload_bytes != expected || paths > file->calibration_.total_paths() ||
        std::filesystem::file_size(file->path_) != file->payload_offset_ + expected) {
        throw std::invalid_argument(
            "Light field payload length or path count differs from its calibration");
    }
    file->paths_ = paths;
    file->file_bytes_ = file->payload_offset_ + expected;
    cancellation.check();
    return file;
}
const LightFieldCalibration& LightFieldFile::calibration() const noexcept {
    return calibration_;
}
std::uint64_t LightFieldFile::traced_paths() const noexcept {
    return paths_;
}
std::uint64_t LightFieldFile::file_bytes() const noexcept {
    return file_bytes_;
}
const std::filesystem::path& LightFieldFile::path() const noexcept {
    return path_;
}
void LightFieldFile::save(const std::filesystem::path& destination, CancellationToken cancellation,
                          LightFieldCallback progress) const {
    cancellation.check();
    detail::StagedFile output(destination);
    std::ifstream source(path_, std::ios::binary);
    if (!source) {
        throw std::runtime_error("Cannot open light field source for copying");
    }
    source.exceptions(std::ios::failbit | std::ios::badbit);
    std::array<char, 65536> buffer{};
    std::uint64_t copied = 0;
    while (copied < file_bytes_) {
        cancellation.check();
        const auto bytes = std::min<std::uint64_t>(buffer.size(), file_bytes_ - copied);
        source.read(buffer.data(), std::streamsize(bytes));
        output.stream().write(buffer.data(), std::streamsize(bytes));
        copied += bytes;
        if (progress) {
            progress({0, copied, file_bytes_});
        }
    }
    source.close(); // Windows 替换同名文件前释放源句柄。
    cancellation.check();
    output.commit();
}

struct LightFieldWriter::Impl {
    detail::StagedFile output;
    std::filesystem::path destination;
    LightFieldCalibration calibration;
    std::vector<bool> written;
    std::uint64_t offset;
    bool finished = false;
    Impl(const std::filesystem::path& path, LightFieldCalibration c)
        : output(path), destination(path), calibration(std::move(c)) {
        calibration.validate();
        written.assign(calibration.view_count(), false);
        const auto json = encode(calibration).dump();
        if (json.size() > maximum_metadata) {
            throw std::length_error("Light field metadata is too large");
        }
        auto& stream = output.stream();
        stream.write(magic.data(), std::streamsize(magic.size()));
        put(stream, 1, 4);
        put(stream, json.size(), 4);
        put(stream, calibration.view_bytes() * calibration.view_count(), 8);
        put(stream, 0, 8);
        put(stream, 0, 8);
        stream.write(json.data(), std::streamsize(json.size()));
        offset = header_bytes + json.size();
    }
};
LightFieldWriter::LightFieldWriter(const std::filesystem::path& path,
                                   LightFieldCalibration calibration)
    : impl_(std::make_unique<Impl>(path, std::move(calibration))) {}
LightFieldWriter::~LightFieldWriter() = default;
void LightFieldWriter::write_view(std::size_t index, const Image& image,
                                  CancellationToken cancellation, LightFieldCallback progress) {
    auto& self = *impl_;
    if (self.finished || index >= self.written.size() || self.written[index] ||
        image.width() != self.calibration.render.width ||
        image.height() != self.calibration.render.height) {
        throw std::invalid_argument("Invalid, duplicate or mismatched light field view");
    }
    auto& stream = self.output.stream();
    stream.seekp(std::streamoff(self.offset + index * self.calibration.view_bytes()));
    constexpr std::size_t block_pixels = 4096;
    std::array<char, block_pixels * 12> block{};
    for (std::size_t y = 0; y < image.height(); ++y) {
        cancellation.check();
        for (std::size_t first = 0; first < image.width(); first += block_pixels) {
            const auto count = std::min(block_pixels, image.width() - first);
            for (std::size_t p = 0; p < count; ++p) {
                const auto pixel = image.at(first + p, y);
                check_pixel(pixel);
                for (int c = 0; c < 3; ++c) {
                    const auto bits = std::bit_cast<std::uint32_t>(pixel[c]);
                    for (std::size_t b = 0; b < 4; ++b) {
                        block[p * 12 + std::size_t(c) * 4 + b] =
                            static_cast<char>((bits >> (b * 8)) & 255U);
                    }
                }
            }
            stream.write(block.data(), std::streamsize(count * 12));
        }
        if (progress) {
            progress({index, y + 1, image.height()});
        }
    }
    self.written[index] = true;
}
std::shared_ptr<const LightFieldFile> LightFieldWriter::finish(std::uint64_t paths) {
    auto& self = *impl_;
    if (self.finished ||
        std::find(self.written.begin(), self.written.end(), false) != self.written.end() ||
        paths > self.calibration.total_paths()) {
        throw std::logic_error("Cannot publish an incomplete light field");
    }
    self.output.stream().seekp(24);
    put(self.output.stream(), paths, 8);
    put(self.output.stream(), 1, 8);
    self.output.commit();
    self.finished = true;
    return LightFieldFile::open(self.destination);
}

struct LightFieldReader::Impl {
    std::shared_ptr<const LightFieldFile> file;
    std::uint64_t budget;
    LightFieldCacheStatistics statistics;
    std::list<std::size_t> recent;
    struct Entry {
        std::shared_ptr<const Image> image;
        std::list<std::size_t>::iterator position;
    };
    std::map<std::size_t, Entry> cache;
    Impl(std::shared_ptr<const LightFieldFile> source, std::uint64_t bytes)
        : file(std::move(source)), budget(bytes) {
        if (!file || bytes > 8ULL * 1024 * 1024 * 1024) {
            throw std::invalid_argument("Invalid light field reader/cache budget");
        }
    }
};
LightFieldReader::LightFieldReader(std::shared_ptr<const LightFieldFile> file, std::uint64_t budget)
    : impl_(std::make_unique<Impl>(std::move(file), budget)) {}
LightFieldReader::~LightFieldReader() = default;
const LightFieldFile& LightFieldReader::file() const noexcept {
    return *impl_->file;
}
std::uint64_t LightFieldReader::cache_budget_bytes() const noexcept {
    return impl_->budget;
}
LightFieldCacheStatistics LightFieldReader::statistics() const noexcept {
    return impl_->statistics;
}
std::shared_ptr<const Image> LightFieldReader::view(std::size_t index,
                                                    CancellationToken cancellation) {
    cancellation.check();
    auto& self = *impl_;
    const auto& file = *self.file;
    const auto& c = file.calibration_;
    if (index >= c.view_count()) {
        throw std::out_of_range("Light field view index is out of range");
    }
    if (const auto found = self.cache.find(index); found != self.cache.end()) {
        self.recent.splice(self.recent.begin(), self.recent, found->second.position);
        ++self.statistics.cache_hits;
        return found->second.image;
    }
    const auto bytes = c.view_bytes();
    while (!self.recent.empty() &&
           (bytes > self.budget || self.statistics.cached_bytes > self.budget - bytes)) {
        self.cache.erase(self.recent.back());
        self.recent.pop_back();
        self.statistics.cached_bytes -= bytes;
    }
    auto image = std::make_shared<Image>(c.render.width, c.render.height);
    std::ifstream stream(file.path_, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot read light field view " + std::to_string(index));
    }
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.seekg(std::streamoff(file.payload_offset_ + index * bytes));
    constexpr std::size_t block_pixels = 4096;
    std::array<unsigned char, block_pixels * 12> block{};
    for (std::size_t y = 0; y < image->height(); ++y) {
        cancellation.check();
        for (std::size_t first = 0; first < image->width(); first += block_pixels) {
            const auto count = std::min(block_pixels, image->width() - first);
            stream.read(reinterpret_cast<char*>(block.data()), std::streamsize(count * 12));
            for (std::size_t p = 0; p < count; ++p) {
                auto& pixel = image->at(first + p, y);
                for (int component = 0; component < 3; ++component) {
                    std::uint32_t bits = 0;
                    for (std::size_t b = 0; b < 4; ++b) {
                        bits |= std::uint32_t(block[p * 12 + std::size_t(component) * 4 + b])
                                << (b * 8);
                    }
                    pixel[component] = std::bit_cast<float>(bits);
                }
                check_pixel(pixel);
            }
        }
    }
    ++self.statistics.decoded_views;
    if (bytes <= self.budget) {
        self.recent.push_front(index);
        self.cache.emplace(index, Impl::Entry{image, self.recent.begin()});
        self.statistics.cached_bytes += bytes;
        self.statistics.peak_cached_bytes =
            std::max(self.statistics.peak_cached_bytes, self.statistics.cached_bytes);
    }
    return image;
}

} // namespace minirender
