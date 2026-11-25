// 单帧按对象、法线、切平面距离和颜色计算权重，再用前帧变换做时域累积。
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <minirender/render/bvh.hpp>
#include <minirender/render/denoiser.hpp>
#include <mutex>
#include <thread>

namespace minirender {
namespace {
// 行之间只读输入，互不写同一像素。回调按实际完成行数串行发布。
template <class Work>
void rows(std::size_t height, CancellationToken token, DenoiseProgress progress, Work work) {
    std::atomic<std::size_t> next{0};
    std::size_t              done = 0;
    std::mutex               lock;
    const auto               count =
        std::min<std::size_t>(height, std::clamp(std::thread::hardware_concurrency(), 1U, 8U));
    std::vector<std::future<void>> jobs;
    for (std::size_t i = 0; i < count; ++i) {
        jobs.emplace_back(std::async(std::launch::async, [&] {
            for (;;) {
                token.check();
                const auto y = next.fetch_add(1);
                if (y >= height) {
                    return;
                }
                work(y);
                if (progress) {
                    std::lock_guard guard(lock);
                    progress(++done, height);
                }
            }
        }));
    }
    for (auto& job : jobs) {
        job.get();
    }
}

void check(const Image& input, const DenoiseGuides& guides, const DenoiseSettings& s) {
    if (input.width() != guides.width || input.height() != guides.height ||
        guides.pixels.size() != guides.width * guides.height) {
        throw std::invalid_argument("Denoiser guide dimensions differ from image");
    }
    if (s.radius < 1 || s.radius > 12 || s.iterations < 1 || s.iterations > 5 ||
        !(s.color_sigma > 0) || !(s.position_sigma > 0) || !(s.normal_power >= 0) ||
        !(s.history_weight >= 0 && s.history_weight <= 1) || !(s.clamp_sigma >= 0)) {
        throw std::invalid_argument("Invalid denoising parameters");
    }
}

// 线性 RGB 的亮度参与空间颜色权重，输出颜色仍保留三个通道。
float luma(glm::vec3 c) {
    return glm::dot(c, glm::vec3(.2126F, .7152F, .0722F));
}

// 用当前物体 7×7 邻域的均值与标准差限制历史色，避免跨轮廓的颜色拖影。
glm::vec3 clamp_history(glm::vec3 history, const Image& current, const DenoiseGuides& guides, int x,
                        int y, int object, float sigma) {
    const int w = static_cast<int>(guides.width);
    const int h = static_cast<int>(guides.height);
    glm::vec3 mean(0), squares(0);
    int       count = 0;
    for (int j = std::max(0, y - 3); j <= std::min(h - 1, y + 3); ++j) {
        for (int i = std::max(0, x - 3); i <= std::min(w - 1, x + 3); ++i) {
            if (guides.pixels[std::size_t(j) * guides.width + std::size_t(i)].object != object) {
                continue;
            }
            const auto c = current.at(std::size_t(i), std::size_t(j));
            mean += c;
            squares += c * c;
            ++count;
        }
    }
    mean /= float(count);
    const auto deviation = glm::sqrt(glm::max(squares / float(count) - mean * mean, glm::vec3(0)));

    return glm::clamp(history, mean - sigma * deviation, mean + sigma * deviation);
}

} // namespace

DenoiseGuides make_guides(std::shared_ptr<const SceneSnapshot> scene, std::size_t width,
                          std::size_t height, CancellationToken token, DenoiseProgress progress) {
    if (!scene || !width || !height || width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::invalid_argument("Invalid guide dimensions");
    }
    SceneBvh      bvh(scene, token);
    DenoiseGuides guides;
    guides.width  = width;
    guides.height = height;
    guides.pixels.resize(width * height);
    for (const auto& obj : scene->instances) {
        guides.objects.push_back(obj.object_to_world);
    }
    const auto aspect    = float(width) / float(height);
    guides.world_to_view = scene->camera.view_matrix();
    glm::mat4 pixels(1);
    pixels[0][0]          = float(width) / 2;
    pixels[1][1]          = float(height) / 2;
    pixels[3][0]          = pixels[0][0];
    pixels[3][1]          = pixels[1][1];
    guides.world_to_pixel = pixels * scene->camera.projection_matrix(aspect) * guides.world_to_view;
    rows(height, token, progress, [&](std::size_t y) {
        for (std::size_t x = 0; x < width; ++x) {
            auto ray = scene->camera.generate_ray(
                {(double(x) + .5) / double(width), (double(y) + .5) / double(height)}, aspect);
            if (auto hit = bvh.intersect(ray)) {
                auto& pixel    = guides.pixels[y * width + x];
                pixel.position = glm::vec3(hit->position);
                pixel.normal   = hit->shading_normal;
                pixel.object   = static_cast<int>(hit->instance_index);
                pixel.depth    = std::abs((guides.world_to_view * glm::vec4(pixel.position, 1)).z);
            }
        }
    });
    return guides;
}

Image filter_spatial(const Image& input, const DenoiseGuides& guides, DenoiseSettings settings,
                     CancellationToken token, DenoiseProgress progress) {
    check(input, guides, settings);
    const int  iterations = settings.filter == SpatialFilter::atrous ? settings.iterations : 1;
    const int  radius     = settings.filter == SpatialFilter::atrous ? 2 : settings.radius;
    const auto w = input.width(), h = input.height();
    const std::array<float, 5> kernel{1, 4, 6, 4, 1};
    Image                      previous = input, next(w, h);
    for (int pass = 0; pass < iterations; ++pass) {
        const int step = 1 << pass;
        // 将本轮的行数换算成跨尺度的总进度，全部行完成后才交换输入和输出。
        DenoiseProgress pass_progress;
        if (progress) {
            pass_progress = [&, pass](std::size_t completed, std::size_t) {
                progress(static_cast<std::size_t>(pass) * h + completed,
                         static_cast<std::size_t>(iterations) * h);
            };
        }
        rows(h, token, pass_progress, [&](std::size_t y) {
            for (std::size_t x = 0; x < w; ++x) {
                const auto& center = guides.pixels[y * w + x];
                const auto  color  = previous.at(x, y);
                if (center.object < 0) {
                    next.at(x, y) = color;
                    continue;
                }
                glm::vec3 sum(0);
                float     total = 0;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int px = static_cast<int>(x) + dx * step,
                                  py = static_cast<int>(y) + dy * step;
                        if (px < 0 || py < 0 || px >= static_cast<int>(w) ||
                            py >= static_cast<int>(h)) {
                            continue;
                        }
                        const auto& other = guides.pixels[std::size_t(py) * w + std::size_t(px)];
                        if (center.object != other.object) {
                            continue;
                        }
                        const auto  value = previous.at(std::size_t(px), std::size_t(py));
                        const float normal =
                            std::pow(std::clamp(glm::dot(center.normal, other.normal), 0.F, 1.F),
                                     settings.normal_power);
                        const float plane =
                            std::abs(glm::dot(center.normal, other.position - center.position));
                        const float luminance =
                            std::abs(luma(color) - luma(value)) /
                            (settings.color_sigma * (.2F + std::max(luma(color), luma(value))));
                        const float spatial =
                            settings.filter == SpatialFilter::atrous
                                ? kernel[std::size_t(dx + 2)] * kernel[std::size_t(dy + 2)]
                                : std::exp(-float(dx * dx + dy * dy) / float(radius * radius));
                        const float weight =
                            spatial * normal *
                            std::exp(-luminance - plane / (settings.position_sigma * float(step)));
                        total += weight;
                        sum += weight * value;
                    }
                }
                next.at(x, y) = total > 1e-8F ? sum / total : color;
            }
        });
        std::swap(previous, next);
    }
    return previous;
}

TemporalResult filter_temporal(const Image& current, const DenoiseGuides& guides,
                               const Image& previous, const DenoiseGuides& old,
                               DenoiseSettings settings, CancellationToken token,
                               DenoiseProgress progress) {
    check(current, guides, settings);
    check(previous, old, settings);
    Image                    output = current;
    std::atomic<std::size_t> accepted{0};
    if (guides.width != old.width || guides.height != old.height ||
        guides.objects.size() != old.objects.size()) {
        return {std::move(output), 0};
    }
    // 每个对象的变换只求逆一次。向前一帧变换位置与法线，区分遮挡变化与对象运动。
    std::vector<glm::mat4> motion;
    std::vector<glm::mat3> normal_motion;
    for (std::size_t i = 0; i < guides.objects.size(); ++i) {
        auto m = old.objects[i] * glm::inverse(guides.objects[i]);
        motion.push_back(m);
        normal_motion.push_back(glm::transpose(glm::inverse(glm::mat3(m))));
    }
    const int w = static_cast<int>(guides.width), h = static_cast<int>(guides.height);
    rows(guides.height, token, progress, [&](std::size_t y) {
        std::size_t valid = 0;
        for (int x = 0; x < w; ++x) {
            const auto& g = guides.pixels[y * guides.width + std::size_t(x)];
            if (g.object < 0 || std::size_t(g.object) >= motion.size()) {
                continue;
            }
            const auto      index = std::size_t(g.object);
            const glm::vec3 world = motion[index] * glm::vec4(g.position, 1);
            const auto      pixel = old.world_to_pixel * glm::vec4(world, 1);
            if (!(pixel.w > 1e-7F)) {
                continue;
            }
            const auto uv = glm::vec2(pixel) / pixel.w;
            if (!(uv.x >= 0 && uv.x < float(w) && uv.y >= 0 && uv.y < float(h))) {
                continue;
            }
            const auto  px = std::size_t(uv.x), py = std::size_t(uv.y);
            const auto& guide     = old.pixels[py * old.width + px];
            const float expected  = std::abs((old.world_to_view * glm::vec4(world, 1)).z);
            const float tolerance = std::max(.01F, .02F * expected);
            if (guide.object != g.object || std::abs(guide.depth - expected) > tolerance ||
                glm::length(world - guide.position) > 2 * tolerance ||
                glm::dot(glm::normalize(normal_motion[index] * g.normal), guide.normal) < .8F) {
                continue;
            }
            const auto history = clamp_history(previous.at(px, py), current, guides, x,
                                               static_cast<int>(y), g.object, settings.clamp_sigma);
            output.at(std::size_t(x), y) =
                glm::mix(current.at(std::size_t(x), y), history, settings.history_weight);
            ++valid;
        }
        accepted += valid;
    });
    return {std::move(output), accepted.load()};
}
} // namespace minirender
