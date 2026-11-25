// 体积介质参数控件、状态刷新与用户操作回调。

#pragma once

#include <minirender/scene/medium.hpp>

#include <QWidget>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;

// 区域编辑使用独立草稿，完整候选通过校验后再提交一次场景历史。
// 刷新按 media 值与文档代数判断；无关编辑保留草稿及未显示的 double 有效位。
class VolumePanel final : public QWidget {
  public:
    // 建立介质区域列表、草稿编辑区和离线渲染入口。
    explicit VolumePanel(QWidget* parent = nullptr);
    // 同步区域列表。文档代数与区域内容均未变化时保留当前草稿。
    void set_document(std::span<const minirender::MediumRegion> regions, std::uint64_t generation);
    // 资产操作期间更新区域编辑和示例导入按钮。
    void set_asset_busy(bool busy);
    // 工作台接收已校验的区域表，处理示例导入和离线渲染请求。
    std::function<void(std::vector<minirender::MediumRegion>)> on_changed;
    std::function<void(const QString&)>                        on_demo;
    std::function<void()>                                      on_render;
    std::function<bool()>                                      render_active;

  private:
    using VectorAccess = std::function<glm::dvec3&(minirender::MediumRegion&)>;
    // 将三分量控件绑定到草稿字段，并按给定上下限约束输入。
    QWidget* vector_field(const char* name, std::array<QDoubleSpinBox*, 3>& controls,
                          VectorAccess access, double low, double high, bool rgb);
    // 依据列表选择建立对应区域的草稿。
    void select();
    // 把草稿数值同步到控件，抑制信号触发新的编辑。
    void refresh_fields();
    // 根据选择、草稿修改和任务状态决定哪些操作可用。
    void update_buttons();
    // 修改未提交的草稿并标记需要应用。
    void edit(const std::function<void(minirender::MediumRegion&)>& operation);
    // 执行界面操作，在提示区报告异常。
    void invoke(const std::function<void()>& operation);
    // 将当前草稿替换到区域列表后提交。
    void apply();
    // 生成具有唯一标识的新介质区域并选中它。
    void add();
    // 从区域列表中移除选中项并提交结果。
    void remove();
    // 校验完整区域列表，通知工作台将其写入场景。
    void publish(std::vector<minirender::MediumRegion> regions, std::string selected);
    // source_ 对应已发布区域，draft_ 保存选中区域尚未提交的修改。
    std::vector<minirender::MediumRegion>   source_;
    std::optional<minirender::MediumRegion> draft_;
    std::string                             selected_;
    std::uint64_t                           generation_  = 0;
    bool                                    initialized_ = false;
    bool                                    refreshing_  = false;
    bool                                    dirty_       = false;
    bool                                    busy_        = false;
    QComboBox *                             regions_ = nullptr, *demo_ = nullptr;
    QLineEdit*                              name_    = nullptr;
    QCheckBox*                              enabled_ = nullptr;
    std::array<QDoubleSpinBox*, 3>          minimum_{}, maximum_{}, absorption_{}, scattering_{};
    QDoubleSpinBox*                         g_          = nullptr;
    QWidget*                                parameters_ = nullptr;
    QLabel *                                notice_ = nullptr, *summary_ = nullptr;
    QPushButton *                           add_ = nullptr, *remove_ = nullptr, *apply_ = nullptr;
    QPushButton *                           load_ = nullptr, *render_ = nullptr;
};
