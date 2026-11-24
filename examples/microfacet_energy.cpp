// 生成三种 GGX 积分方法的方向能量表，供学习和数值比较。横轴 μ，纵轴粗糙度。
#include <array>
#include <filesystem>
#include <iostream>
#include <minirender/render/microfacet_energy.hpp>

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 4) {
            throw std::invalid_argument(
                "Usage: minirender_energy_example DIRECTORY [resolution=32] [samples=2048]");
        }
        const auto resolution = argc > 2 ? std::stoul(argv[2]) : 32UL;
        const auto samples    = argc > 3 ? std::stoul(argv[3]) : 2048UL;
        if (resolution < 2 || resolution > 256 || samples < 1 || samples > 65536) {
            throw std::invalid_argument("Resolution must be 2..256 and samples 1..65536");
        }
        const auto directory = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(directory);
        using namespace minirender;
        for (const auto& [method, name] :
             std::array{std::pair{EnergyMethod::uniform, "uniform"},
                        std::pair{EnergyMethod::visible_normal, "vndf"},
                        std::pair{EnergyMethod::split_sum, "split_sum"}}) {
            Image table(resolution, resolution);
            for (std::size_t y = 0; y < resolution; ++y) {
                for (std::size_t x = 0; x < resolution; ++x) {
                    const auto energy = integrate_directional_energy(
                        double(x) / double(resolution - 1), double(y) / double(resolution - 1),
                        static_cast<std::uint32_t>(samples), 101 + y * resolution + x, method);
                    table.at(x, y) = {static_cast<float>(energy), 0, 0};
                }
                store_average_energy(table, y);
            }
            table.write_exr(directory / (std::string(name) + ".exr"));
            std::cout << name << " · E(normal, roughness=1) "
                      << table.at(resolution - 1, resolution - 1).r << " · Eavg "
                      << table.at(0, resolution - 1).b << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
