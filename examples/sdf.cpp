#include <minirender/assets/sdf_io.hpp>
#include <minirender/render/sdf_render.hpp>

#include <cmath>
#include <iostream>

using namespace minirender;

// 生成四种距离场参照图，再对照解析球求交。输出供后续 GPU 与编辑器复验。
int main() {
    try {
        const auto output = std::filesystem::current_path() / "outputs/a5.3/core";
        std::filesystem::create_directories(output);
        const auto document = make_sdf_example();
        const auto path = output / std::filesystem::path(u8"距离场 示例.json");
        save_sdf(document, path);
        const auto reopened = load_sdf(path);
        for (const auto& [name, view] : {std::pair{"surface", SdfView::surface},
                                         {"normals", SdfView::normals},
                                         {"depth", SdfView::depth},
                                         {"steps", SdfView::steps}}) {
            const auto result = render_sdf(reopened, {256, 256, view});
            result.image.write_png(output / (std::string(name) + ".png"));
            if (view == SdfView::surface) {
                result.image.write_exr(output / "surface.exr");
            }
            std::cout << name << " hits=" << result.hits << " limited=" << result.limited
                      << " steps=" << result.steps
                      << " normal_fallbacks=" << result.normal_fallbacks << '\n';
            if (result.limited || result.normal_fallbacks) {
                return 1;
            }
        }

        SdfDocument sphere;
        sphere.nodes.push_back({"sphere", "Sphere", SdfKind::sphere, {1, 0, 0}});
        sphere.camera.position = {0, 0, 4};
        sphere.camera.target = {0, 0, 0};
        const SdfField field(sphere);
        double maximum_error = 0;
        std::size_t hits = 0;
        std::size_t misses = 0;
        for (std::size_t y = 0; y < 128; ++y) {
            for (std::size_t x = 0; x < 128; ++x) {
                const auto ray = sphere.camera.generate_ray(
                    {(static_cast<double>(x) + .5) / 128, (static_cast<double>(y) + .5) / 128}, 1);
                const double b = glm::dot(ray.origin, ray.direction);
                const double discriminant = b * b - glm::dot(ray.origin, ray.origin) + 1;
                if (std::abs(discriminant) < .05) {
                    continue; // 轮廓附近另由图像检查覆盖，此处统计稳定解析样本。
                }
                const auto result = trace_sdf(field, ray, sphere.trace);
                if (discriminant < 0) {
                    if (result.state != SdfTraceState::miss) {
                        return 1;
                    }
                    ++misses;
                } else {
                    if (result.state != SdfTraceState::hit) {
                        return 1;
                    }
                    maximum_error = std::max(
                        maximum_error, std::abs(result.distance + b + std::sqrt(discriminant)));
                    ++hits;
                }
            }
        }
        std::cout << "analytic_hits=" << hits << " misses=" << misses
                  << " maximum_distance_error_m=" << maximum_error << '\n';
        return maximum_error < .001 && hits > 1000 && misses > 1000 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
