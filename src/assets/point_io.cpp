#include "staged_file.hpp"
#include <minirender/assets/point_io.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace minirender {
namespace {

enum class Scalar { i8, u8, i16, u16, i32, u32, f32, f64 };
struct Property {
    std::string name;
    Scalar value;
    std::optional<Scalar> count;
};
struct Element {
    std::string name;
    std::size_t count;
    std::vector<Property> properties;
};

Scalar scalar_type(const std::string& name) {
    for (const auto& [first, alias, type] : {std::tuple{"char", "int8", Scalar::i8},
                                             {"uchar", "uint8", Scalar::u8},
                                             {"short", "int16", Scalar::i16},
                                             {"ushort", "uint16", Scalar::u16},
                                             {"int", "int32", Scalar::i32},
                                             {"uint", "uint32", Scalar::u32},
                                             {"float", "float32", Scalar::f32},
                                             {"double", "float64", Scalar::f64}}) {
        if (name == first || name == alias) {
            return type;
        }
    }
    throw std::invalid_argument("Unsupported PLY scalar type: " + name);
}
bool floating(Scalar type) {
    return type == Scalar::f32 || type == Scalar::f64;
}
bool space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}
std::size_t count_value(const std::string& text) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("PLY count must be an unsigned integer");
    }
    return result;
}

// Header 描述每种记录的实际布局。Reader 对认识和跳过的数据使用同一标量读取器。
// 记录/标量/文件预算分别限制数组、list 扫描和总输入；流由当前工作线程独占。
class Reader final {
  public:
    Reader(const std::filesystem::path& path, PlyReadOptions options,
           CancellationToken cancellation, PointProgress progress)
        : stream_(path, std::ios::binary), options_(options), cancellation_(cancellation),
          progress_(std::move(progress)) {
        cancellation_.check();
        if (!stream_) {
            throw std::runtime_error("Cannot open PLY point cloud");
        }
        if (!options_.maximum_points || !options_.maximum_records || !options_.maximum_values ||
            !options_.maximum_header_bytes ||
            std::filesystem::file_size(path) > options_.maximum_file_bytes) {
            throw std::length_error("PLY input exceeds its read budget");
        }
    }

    PointCloud read() {
        read_header();
        report(0);
        PointCloud result;
        for (const auto& element : elements_) {
            const bool vertex = element.name == "vertex";
            if (vertex) {
                result.positions.reserve(element.count);
                if (has_normals_) {
                    result.normals.reserve(element.count);
                }
            }
            for (std::size_t row = 0; row < element.count; ++row) {
                std::array<float, 6> fields{};
                for (const auto& property : element.properties) {
                    std::size_t count = 1;
                    if (property.count) {
                        const double value = number(*property.count);
                        if (value < 0 ||
                            value > static_cast<double>(options_.maximum_values - values_)) {
                            throw std::length_error("PLY list exceeds its value budget");
                        }
                        count = static_cast<std::size_t>(value);
                    }
                    for (std::size_t i = 0; i < count; ++i) {
                        const double value = number(property.value);
                        if (vertex && !property.count) {
                            const auto field = semantic(property.name);
                            if (field >= 0) {
                                if (std::abs(value) > std::numeric_limits<float>::max()) {
                                    throw std::invalid_argument(
                                        "PLY coordinate exceeds float storage");
                                }
                                fields[static_cast<std::size_t>(field)] = static_cast<float>(value);
                            }
                        }
                    }
                }
                if (vertex) {
                    result.positions.emplace_back(fields[0], fields[1], fields[2]);
                    if (has_normals_) {
                        result.normals.emplace_back(fields[3], fields[4], fields[5]);
                    }
                }
                if (++records_ % 1024 == 0) {
                    report(records_);
                }
            }
        }
        result.validate(cancellation_);
        report(records_);
        return result;
    }

  private:
    static int semantic(const std::string& name) {
        const std::array<std::string_view, 6> names{"x", "y", "z", "nx", "ny", "nz"};
        const auto found = std::find(names.begin(), names.end(), name);
        return found == names.end() ? -1 : static_cast<int>(found - names.begin());
    }
    void report(std::size_t done) {
        cancellation_.check();
        if (progress_) {
            progress_(done, total_records_);
        }
        cancellation_.check();
    }
    std::vector<std::string> line() {
        std::string text;
        char ch = 0;
        for (;;) {
            if (!stream_.get(ch)) {
                throw std::invalid_argument("Truncated PLY header");
            }
            if (++header_bytes_ > options_.maximum_header_bytes) {
                throw std::length_error("PLY header exceeds its byte budget");
            }
            if (header_bytes_ % 1024 == 0) {
                cancellation_.check();
            }
            if (ch == '\n' || ch == '\r') {
                if (ch == '\r' && stream_.peek() == '\n') {
                    stream_.get();
                    if (++header_bytes_ > options_.maximum_header_bytes) {
                        throw std::length_error("PLY header exceeds its byte budget");
                    }
                }
                break;
            }
            text.push_back(ch);
        }
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        std::vector<std::string> words;
        std::string word;
        while (input >> word) {
            words.push_back(word);
        }
        return words;
    }
    void read_header() {
        if (line() != std::vector<std::string>{"ply"}) {
            throw std::invalid_argument("PLY magic number is missing");
        }
        const auto format = line();
        if (format.size() != 3 || format[0] != "format" || format[2] != "1.0") {
            throw std::invalid_argument("PLY format 1.0 is required");
        }
        if (format[1] == "ascii") {
            format_ = PlyFormat::ascii;
        } else if (format[1] == "binary_little_endian") {
            format_ = PlyFormat::little_endian;
        } else if (format[1] == "binary_big_endian") {
            format_ = PlyFormat::big_endian;
        } else {
            throw std::invalid_argument("Unknown PLY encoding");
        }
        std::set<std::string> element_names;
        for (;;) {
            const auto words = line();
            if (words.empty() || words[0] == "comment" || words[0] == "obj_info") {
                continue;
            }
            if (words == std::vector<std::string>{"end_header"}) {
                break;
            }
            if (words[0] == "element" && words.size() == 3) {
                const auto count = count_value(words[2]);
                if (!element_names.insert(words[1]).second) {
                    throw std::invalid_argument("Duplicate PLY element name");
                }
                if (count > options_.maximum_records - total_records_) {
                    throw std::length_error("PLY element count exceeds its record budget");
                }
                total_records_ += count;
                elements_.push_back({words[1], count, {}});
            } else if (words[0] == "property" && !elements_.empty() &&
                       (words.size() == 3 || (words.size() == 5 && words[1] == "list"))) {
                Property property{words.back(), scalar_type(words[words.size() - 2]), {}};
                if (words.size() == 5) {
                    property.count = scalar_type(words[2]);
                    if (floating(*property.count)) {
                        throw std::invalid_argument("PLY list length requires an integer type");
                    }
                }
                const auto& previous = elements_.back().properties;
                if (std::any_of(previous.begin(), previous.end(),
                                [&](const auto& p) { return p.name == property.name; })) {
                    throw std::invalid_argument("Duplicate PLY property name");
                }
                elements_.back().properties.push_back(std::move(property));
            } else {
                throw std::invalid_argument("Invalid PLY header declaration");
            }
        }
        bool found_vertex = false;
        for (const auto& element : elements_) {
            if (element.properties.empty() && element.count) {
                throw std::invalid_argument("PLY element has no properties");
            }
            if (element.name != "vertex") {
                continue;
            }
            found_vertex = true;
            if (!element.count || element.count > options_.maximum_points ||
                element.count >= std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("PLY vertex count exceeds its point budget");
            }
            std::array<bool, 6> present{};
            for (const auto& property : element.properties) {
                const int field = semantic(property.name);
                if (field >= 0) {
                    if (property.count) {
                        throw std::invalid_argument("PLY coordinates must be scalar properties");
                    }
                    present[static_cast<std::size_t>(field)] = true;
                }
            }
            if (!present[0] || !present[1] || !present[2] ||
                ((present[3] || present[4] || present[5]) &&
                 !(present[3] && present[4] && present[5]))) {
                throw std::invalid_argument("PLY requires x/y/z and complete optional nx/ny/nz");
            }
            has_normals_ = present[3];
        }
        if (!found_vertex) {
            throw std::invalid_argument("PLY vertex element is missing");
        }
    }
    template <class T> double binary() {
        std::array<char, sizeof(T)> bytes{};
        if (!stream_.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            throw std::invalid_argument("Truncated binary PLY record");
        }
        if ((std::endian::native == std::endian::little) != (format_ == PlyFormat::little_endian)) {
            std::reverse(bytes.begin(), bytes.end());
        }
        return static_cast<double>(std::bit_cast<T>(bytes));
    }
    std::string token() {
        std::string text;
        char ch = 0;
        while (stream_.get(ch) && space(ch)) {
        }
        if (!stream_) {
            throw std::invalid_argument("Truncated ASCII PLY record");
        }
        do {
            text.push_back(ch);
            if (text.size() > 128) {
                throw std::invalid_argument("PLY numeric token is too long");
            }
        } while (stream_.get(ch) && !space(ch));
        return text;
    }
    double ascii(Scalar type) {
        const auto text = token();
        const auto* first = text.data() + (text.front() == '+' ? 1 : 0);
        const auto* last = text.data() + text.size();
        if (floating(type)) {
            double value = 0;
            // 与现有 CLI 保持一致，兼容尚无浮点 from_chars 的 libc++。
            std::istringstream input(text);
            input.imbue(std::locale::classic());
            input >> std::noskipws >> value;
            if (!input || input.peek() != std::char_traits<char>::eof()) {
                throw std::invalid_argument("Invalid PLY floating point token");
            }
            return value;
        }
        std::int64_t value = 0;
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            throw std::invalid_argument("Invalid PLY integer token");
        }
        const std::array<std::pair<std::int64_t, std::int64_t>, 6> ranges{
            {{-128, 127},
             {0, 255},
             {-32768, 32767},
             {0, 65535},
             {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()},
             {0, std::numeric_limits<std::uint32_t>::max()}}};
        const auto range = ranges[static_cast<std::size_t>(type)];
        if (value < range.first || value > range.second) {
            throw std::invalid_argument("PLY integer token is outside its declared type");
        }
        return static_cast<double>(value);
    }
    double number(Scalar type) {
        if (++values_ > options_.maximum_values) {
            throw std::length_error("PLY data exceeds its value budget");
        }
        if (values_ % 1024 == 0) {
            cancellation_.check();
        }
        double value = 0;
        if (format_ == PlyFormat::ascii) {
            value = ascii(type);
        } else {
            switch (type) {
            case Scalar::i8:
                value = binary<std::int8_t>();
                break;
            case Scalar::u8:
                value = binary<std::uint8_t>();
                break;
            case Scalar::i16:
                value = binary<std::int16_t>();
                break;
            case Scalar::u16:
                value = binary<std::uint16_t>();
                break;
            case Scalar::i32:
                value = binary<std::int32_t>();
                break;
            case Scalar::u32:
                value = binary<std::uint32_t>();
                break;
            case Scalar::f32:
                value = binary<float>();
                break;
            case Scalar::f64:
                value = binary<double>();
                break;
            }
        }
        if (!std::isfinite(value) ||
            (type == Scalar::f32 && std::abs(value) > std::numeric_limits<float>::max())) {
            throw std::invalid_argument("PLY scalar is outside its finite declared type");
        }
        return value;
    }
    std::ifstream stream_;
    PlyReadOptions options_;
    CancellationToken cancellation_;
    PointProgress progress_;
    std::vector<Element> elements_;
    PlyFormat format_ = PlyFormat::ascii;
    std::size_t total_records_ = 0;
    std::size_t records_ = 0;
    std::size_t values_ = 0;
    std::size_t header_bytes_ = 0;
    bool has_normals_ = false;
};

} // namespace

PointCloud load_point_cloud(const std::filesystem::path& path, PlyReadOptions options,
                            CancellationToken cancellation, PointProgress progress) {
    return Reader(path, options, cancellation, std::move(progress)).read();
}

void save_point_cloud(const PointCloud& cloud, const std::filesystem::path& path, PlyFormat format,
                      CancellationToken cancellation, PointProgress progress) {
    cloud.validate(cancellation);
    if (format != PlyFormat::ascii && format != PlyFormat::little_endian &&
        format != PlyFormat::big_endian) {
        throw std::invalid_argument("Unknown PLY output encoding");
    }
    detail::StagedFile staged(path);
    auto& stream = staged.stream();
    stream.imbue(std::locale::classic());
    const char* encoding = format == PlyFormat::ascii           ? "ascii"
                           : format == PlyFormat::little_endian ? "binary_little_endian"
                                                                : "binary_big_endian";
    stream << "ply\nformat " << encoding << " 1.0\ncomment MiniRender point cloud\n"
           << "element vertex " << cloud.positions.size() << "\n"
           << "property float x\nproperty float y\nproperty float z\n";
    if (!cloud.normals.empty()) {
        stream << "property float nx\nproperty float ny\nproperty float nz\n";
    }
    stream << "end_header\n" << std::setprecision(std::numeric_limits<float>::max_digits10);
    const auto report = [&](std::size_t done) {
        cancellation.check();
        if (progress) {
            progress(done, cloud.positions.size());
        }
        cancellation.check();
    };
    const auto write = [&](glm::vec3 value, bool last) {
        for (int axis = 0; axis < 3; ++axis) {
            if (format == PlyFormat::ascii) {
                stream << value[axis] << (axis == 2 && last ? '\n' : ' ');
            } else {
                auto bytes = std::bit_cast<std::array<char, sizeof(float)>>(value[axis]);
                if ((std::endian::native == std::endian::little) !=
                    (format == PlyFormat::little_endian)) {
                    std::reverse(bytes.begin(), bytes.end());
                }
                stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
        }
    };
    report(0);
    for (std::size_t i = 0; i < cloud.positions.size(); ++i) {
        write(cloud.positions[i], cloud.normals.empty());
        if (!cloud.normals.empty()) {
            write(cloud.normals[i], true);
        }
        if ((i + 1) % 1024 == 0) {
            report(i + 1);
        }
    }
    report(cloud.positions.size());
    staged.commit();
}

} // namespace minirender
