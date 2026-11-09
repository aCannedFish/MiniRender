#include <minirender/scene/gizmo.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <numbers>
#include <stdexcept>

using namespace minirender;
using Catch::Approx;

namespace {

GizmoView front_view() {
    GizmoView view;
    view.camera.position = {0, 0, 5};
    view.camera.target = {0, 0, 0};
    view.camera.vertical_fov_degrees = 60;
    view.size = {800, 600};
    return view;
}

glm::dvec2 projected(const GizmoView& view, glm::dvec3 point) {
    const auto p = view.project(point);
    REQUIRE(p.has_value());
    return *p;
}

void matrix_close(const glm::mat4& a, const glm::dmat4& b, double tolerance = 2e-5) {
    for (int column = 0; column < 4; ++column) {
        REQUIRE(glm::length(glm::dvec4(a[column]) - b[column]) < tolerance);
    }
}

} // namespace

TEST_CASE("Gizmo projection size and hits use the camera and logical pixels", "[gizmo]") {
    auto view = front_view();
    const Transform transform;
    const auto layout = make_gizmo_layout(view, transform, GizmoMode::translate);
    REQUIRE(layout.visible);
    REQUIRE(glm::length(layout.center - glm::dvec2(400, 300)) < 1e-8);
    REQUIRE(layout.hit(layout.center).handle == GizmoHandle::screen);
    REQUIRE(layout.hit({450, 300}).handle == GizmoHandle::x);
    REQUIRE(layout.hit({400, 250}).handle == GizmoHandle::y);
    REQUIRE(layout.hit({600, 500}).handle == GizmoHandle::none);
    const auto endpoint = projected(view, {layout.world_radius, 0, 0});
    REQUIRE(endpoint.x - layout.center.x == Approx(view.radius_pixels).margin(1e-5));
    view.camera.position.z = 10;
    const auto farther = make_gizmo_layout(view, transform, GizmoMode::translate);
    REQUIRE(farther.world_radius == Approx(layout.world_radius * 2));
    REQUIRE(projected(view, {farther.world_radius, 0, 0}).x == Approx(endpoint.x).margin(1e-5));
    auto hidden = transform;
    hidden.translation.z = 11;
    REQUIRE_FALSE(make_gizmo_layout(view, hidden, GizmoMode::rotate).visible);
    REQUIRE_FALSE(make_gizmo_layout(view, transform, GizmoMode::select).visible);
    view.size.x = 0;
    REQUIRE_THROWS_AS(make_gizmo_layout(view, transform, GizmoMode::translate),
                      std::invalid_argument);
}

TEST_CASE("Gizmo translation constrains world axes and provides screen-plane movement", "[gizmo]") {
    const auto view = front_view();
    Transform original;
    original.rotation_degrees = {20, -37, 61};
    original.scale = {-2, 0.5F, 3};
    const auto layout = make_gizmo_layout(view, original, GizmoMode::translate);
    const glm::dvec2 start(450, 300);
    auto drag = GizmoDrag::begin(view, original, layout, layout.hit(start), start);
    REQUIRE(drag.has_value());
    const auto moved = drag->update(start + glm::dvec2(80, 30));
    const double pixel_scale = 10 * std::tan(std::numbers::pi / 6) / 600;
    REQUIRE(moved.translation.x == Approx(80 * pixel_scale).margin(2e-6));
    REQUIRE(moved.translation.y == 0);
    REQUIRE(moved.translation.z == 0);
    REQUIRE(moved.rotation_degrees == original.rotation_degrees);
    REQUIRE(moved.scale == original.scale);
    const auto restored = drag->update(start);
    REQUIRE(glm::length(restored.translation) < 1e-6F);
    auto free = GizmoDrag::begin(view, original, layout, layout.hit(layout.center), layout.center);
    REQUIRE(free.has_value());
    const auto shifted = free->update(layout.center + glm::dvec2(80, -30));
    REQUIRE(shifted.translation.x == Approx(80 * pixel_scale).margin(2e-6));
    REQUIRE(shifted.translation.y == Approx(30 * pixel_scale).margin(2e-6));
    REQUIRE(shifted.translation.z == 0);
}

TEST_CASE("All three translation handles remain world-aligned on a rotated scaled object",
          "[gizmo]") {
    auto view = front_view();
    view.camera.position = {3, 2, 5};
    Transform original;
    original.rotation_degrees = {31, -22, 64};
    original.scale = {-2, 0.5F, 3};
    const auto layout = make_gizmo_layout(view, original, GizmoMode::translate);
    for (const auto handle : {GizmoHandle::x, GizmoHandle::y, GizmoHandle::z}) {
        const auto direction = handle == GizmoHandle::x   ? glm::dvec3(1, 0, 0)
                               : handle == GizmoHandle::y ? glm::dvec3(0, 1, 0)
                                                          : glm::dvec3(0, 0, 1);
        const auto start_world = direction * (layout.world_radius * 0.7);
        const auto start = projected(view, start_world);
        REQUIRE(layout.hit(start).handle == handle);
        auto drag = GizmoDrag::begin(view, original, layout, layout.hit(start), start);
        REQUIRE(drag.has_value());
        const auto moved = drag->update(projected(view, start_world + direction * 0.35));
        REQUIRE(glm::length(glm::dvec3(moved.translation) - direction * 0.35) < 3e-6);
        REQUIRE(moved.rotation_degrees == original.rotation_degrees);
        REQUIRE(moved.scale == original.scale);
    }
}

TEST_CASE("Gizmo world rotation composes with Euler transforms and supports multiple turns",
          "[gizmo]") {
    const auto view = front_view();
    Transform original;
    original.rotation_degrees = {20, 35, -17};
    original.scale = {-2, 0.5F, 3};
    const auto layout = make_gizmo_layout(view, original, GizmoMode::rotate);
    const auto start_angle = std::numbers::pi / 4;
    const auto point = [&](double angle) {
        return glm::dvec3(std::cos(angle), std::sin(angle), 0) * layout.world_radius;
    };
    const auto start = projected(view, point(start_angle));
    REQUIRE(layout.hit(start).handle == GizmoHandle::z);
    auto drag = GizmoDrag::begin(view, original, layout, layout.hit(start), start);
    REQUIRE(drag.has_value());
    Transform rotated;
    // 2.25 圈，每次推进 pi/4，确保正确跨越 atan2 的正负分支。
    for (int step = 1; step <= 18; ++step) {
        rotated = drag->update(projected(view, point(start_angle + step * std::numbers::pi / 4)));
    }
    const auto expected = glm::rotate(glm::dmat4(1), std::numbers::pi / 2, glm::dvec3(0, 0, 1)) *
                          glm::dmat4(original.matrix());
    matrix_close(rotated.matrix(), expected);
    REQUIRE(rotated.scale == original.scale);
    REQUIRE(rotated.translation == original.translation);
}

TEST_CASE("Gizmo rotation reconstructs a gimbal pose and handles edge-on rings", "[gizmo]") {
    auto view = front_view();
    view.camera.position = {0, 5, 0};
    view.camera.up = {0, 0, -1};
    Transform original;
    original.rotation_degrees = {20, 80, 0};
    const auto layout = make_gizmo_layout(view, original, GizmoMode::rotate);
    const auto anchor = glm::dvec3(layout.world_radius, 0, 0);
    const auto start = projected(view, anchor);
    auto drag = GizmoDrag::begin(view, original, layout, {GizmoHandle::y, anchor, 0}, start);
    REQUIRE(drag.has_value());
    const auto angle = glm::radians(10.0);
    const auto target = glm::dvec3(std::cos(angle), 0, -std::sin(angle)) * layout.world_radius;
    const auto rotated = drag->update(projected(view, target));
    const auto expected =
        glm::rotate(glm::dmat4(1), angle, glm::dvec3(0, 1, 0)) * glm::dmat4(original.matrix());
    matrix_close(rotated.matrix(), expected);
    const auto front = front_view();
    const auto edge_layout = make_gizmo_layout(front, Transform{}, GizmoMode::rotate);
    const auto edge_anchor = glm::dvec3(0, edge_layout.world_radius, 0);
    const auto edge_start = projected(front, edge_anchor);
    auto edge_drag = GizmoDrag::begin(front, Transform{}, edge_layout,
                                      {GizmoHandle::x, edge_anchor, 0}, edge_start);
    REQUIRE(edge_drag.has_value());
    const auto edge_result = edge_drag->update(edge_start + glm::dvec2(0, 20));
    REQUIRE(std::isfinite(edge_result.rotation_degrees.x));
    REQUIRE(std::abs(edge_result.rotation_degrees.x) > 1);
    REQUIRE(edge_result.rotation_degrees.y == Approx(0).margin(1e-5));
    REQUIRE(edge_result.rotation_degrees.z == Approx(0).margin(1e-5));
}

TEST_CASE("Gizmo rotation holds its last valid transform at a parallel constraint ray", "[gizmo]") {
    auto view = front_view();
    view.camera.position = {0, 5, 5};
    const Transform original;
    const auto layout = make_gizmo_layout(view, original, GizmoMode::rotate);
    const glm::dvec3 anchor(layout.world_radius, 0, 0);
    auto drag = GizmoDrag::begin(view, original, layout, {GizmoHandle::y, anchor, 0},
                                 projected(view, anchor));
    REQUIRE(drag.has_value());
    const auto angle = glm::radians(20.0);
    const auto moved = drag->update(projected(
        view, {std::cos(angle) * layout.world_radius, 0, -std::sin(angle) * layout.world_radius}));
    REQUIRE(std::abs(moved.rotation_degrees.y) > 10);
    // 此处射线的世界 Y 分量为零，与 Y=0 约束平面平行。
    const auto horizon_y = (1 - 1 / std::tan(std::numbers::pi / 6)) * view.size.y / 2;
    const auto held = drag->update({view.size.x / 2, horizon_y});
    REQUIRE(held.rotation_degrees == moved.rotation_degrees);
    REQUIRE(held.translation == moved.translation);
}
