#pragma once

#include <minirender/core/cancellation.hpp>
#include <minirender/render/intersection.hpp>

#include <span>

namespace minirender {

// 几何/着色法线均指向原网格的外侧，不随入射射线自动翻转。
// 镜像变换通过行列式符号保持原外侧语义；front_face 表示射线是否从外侧到达。
// 材质与纹理通过 instance_index 访问 scene()，不在每次命中时拷贝字符串或资产。
struct SurfaceHit {
    double distance = 0;
    glm::dvec3 position{0};
    glm::vec3 geometric_normal{0};
    glm::vec3 shading_normal{0};
    glm::vec2 uv{0};
    glm::dvec3 barycentric{0};
    std::uint32_t instance_index = 0;
    std::uint32_t triangle_index = 0;
    bool front_face = true;
    glm::dvec3 position_error{0}; // 重心插值运算的逐轴绝对误差界，用于偏移后续射线。
};

struct BvhBuildStatistics {
    std::size_t triangles = 0;
    std::size_t nodes = 0;
    std::size_t leaves = 0;
    std::size_t maximum_depth = 0;
};

// 可选的调用方计数器，跨查询累加。并发查询应各自传入独立计数器。
struct TraceStatistics {
    std::uint64_t bounds_tests = 0;
    std::uint64_t triangle_tests = 0;
};

// CPU 只读场景加速结构。输入必须是 make_snapshot 生成的有效不可变快照。
// 构造时展开世界空间三角形位置；使用 12 桶 SAH、平坦节点及有界深度。
// 构造完成后不修改任何成员，可供多条渲染线程共享；不依赖 Qt 或 GPU SDK。
class SceneBvh {
  public:
    explicit SceneBvh(std::shared_ptr<const SceneSnapshot> scene,
                      CancellationToken cancellation = {});
    const SceneSnapshot& scene() const noexcept;
    const BvhBuildStatistics& statistics() const noexcept;
    // 返回闭区间内最近命中；等距命中按原场景图元次序选择，避免遍历顺序影响结果。
    std::optional<SurfaceHit> intersect(const Ray& ray, RayInterval interval = {},
                                        TraceStatistics* statistics = nullptr,
                                        const CancellationToken& cancellation = {}) const;
    // 阴影查询在首次命中时返回，不计算 UV、法线或其余最近命中数据。
    bool occluded(const Ray& ray, RayInterval interval = {}, TraceStatistics* statistics = nullptr,
                  const CancellationToken& cancellation = {}) const;
    // BSSRDF 探针查询指定实例的全部几何面，包含背面，不受材质单面显示设置影响。
    std::optional<SurfaceHit> intersect_instance(const Ray& ray, std::uint32_t instance,
                                                 RayInterval interval = {},
                                                 TraceStatistics* statistics = nullptr,
                                                 const CancellationToken& cancellation = {}) const;

  private:
    struct Primitive {
        std::array<glm::vec3, 3> vertices;
        std::uint32_t instance;
        std::uint32_t triangle;
    };
    // 包围盒与重心仅供构建使用，构造结束后释放，不加入每条射线的图元工作集。
    struct BuildPrimitive {
        Bounds3 bounds;
        glm::vec3 centroid;
    };
    // 叶子：count>0，first 为重排索引的起点；内部节点：first 为左孩子，right 为右孩子。
    struct Node {
        Bounds3 bounds;
        std::uint32_t first = 0;
        std::uint32_t right = 0;
        std::uint32_t count = 0;
    };
    struct PrimitiveHit {
        std::uint32_t primitive;
        TriangleHit hit;
    };
    std::uint32_t build(std::uint32_t begin, std::uint32_t end, std::size_t depth,
                        std::span<const BuildPrimitive> primitives);
    std::optional<PrimitiveHit> trace(const Ray& ray, RayInterval interval, bool any_hit,
                                      TraceStatistics* statistics,
                                      const CancellationToken& cancellation,
                                      std::optional<std::uint32_t> instance_filter = {}) const;
    SurfaceHit surface_hit(const Ray& ray, const PrimitiveHit& hit) const;
    std::shared_ptr<const SceneSnapshot> scene_;
    std::vector<Primitive> primitives_;
    std::vector<std::uint32_t> order_;
    std::vector<Node> nodes_;
    std::vector<bool> mirrored_;
    BvhBuildStatistics statistics_;
    CancellationToken construction_cancellation_;
};

} // namespace minirender
