// 工作台内部控件、后台结果与文件路径辅助；供分拆后的工作台实现共享。
#pragma once

#include "scene_id.hpp"
#include "workbench.hpp"

#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QFormLayout;
class QScrollArea;

namespace workbench_detail {

using minirender::desktop::unique_id;

// 工作线程不访问 UI 或 Vulkan。结果和异常均在 watcher 的 UI 线程回调处理。
template <typename T> struct TaskResult {
    // 工作线程成功时返回的独立结果。
    T value{};
    // 异常文本在 GUI 线程统一显示。
    std::string error;
};

// 文件解析和完整网格校验一起完成，GUI 事务复用校验缓存。
struct LoadedMesh {
    std::shared_ptr<const minirender::MeshData>       data;
    std::shared_ptr<const minirender::RasterMeshData> raster;
    minirender::MeshValidationCache                   validated;
};

// 后台场景解析结果，携带可直接复用的校验和顶点准备数据。
struct LoadedScene {
    minirender::SceneDocument       document;
    minirender::MeshValidationCache validated;
    std::vector<std::pair<std::shared_ptr<const minirender::MeshData>,
                          std::shared_ptr<const minirender::RasterMeshData>>>
        raster;
};

// 解析场景、校验网格并准备可见物体的光栅数据，供普通打开和资源目录共用。
LoadedScene prepare_scene(const std::filesystem::path& source);

// work 在工作线程执行，done 只在 owner 存活时由 GUI 线程处理结果或错误。
template <typename Work, typename Done> void run_background(QObject* owner, Work work, Done done) {
    using Result  = TaskResult<decltype(work())>;
    auto* watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [watcher, done] {
        // finished 由 Qt 投递到 owner 所在线程，此时可以安全更新控件。
        const auto result = watcher->result();
        watcher->deleteLater();
        done(result);
    });
    // worker 只捕获固定输入与任务函数，不直接访问窗口。
    watcher->setFuture(QtConcurrent::run([work] {
        Result result;
        try {
            result.value = work();
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    }));
}

// 将 Qt UTF-8 路径转换为 filesystem 原生路径，保留中文和空格。
std::filesystem::path native_path(const QString& path);
// 创建带范围和步长的数值控件，键盘输入在提交时应用。
QDoubleSpinBox* spin(QWidget* parent, const QString& name, double minimum, double maximum,
                     double step);
// 用无横向滚动条的容器承载表单，内容随面板宽度调整。
QScrollArea* scroll_panel(QWidget* panel);
// 设置表单边距、行间距和窄窗口换行策略。
void configure_form(QFormLayout* form);

} // namespace workbench_detail

// 三分量编辑器：数值来自三个独立输入框，程序刷新时抑制编辑回调。
class VectorField final : public QWidget {
  public:
    // 建立 XYZ 或 RGB 分量；范围和前缀由所在面板固定。
    VectorField(QWidget* parent, const QString& prefix, double minimum = -10000,
                double maximum = 10000);
    // 返回当前三个分量，以场景使用的 float 表示。
    glm::vec3 value() const;
    // 程序更新控件值，不调用 on_changed。
    void set_value(glm::vec3 value);
    // 用户提交数值或步进时调用，所有权由工作台管理。
    std::function<void()> on_changed;

  private:
    // 由 QWidget 父子关系管理的三个数值输入框。
    std::array<QDoubleSpinBox*, 3> values_{};
};
