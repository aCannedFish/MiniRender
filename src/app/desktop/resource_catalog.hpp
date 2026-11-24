// 资源包索引、包内相对路径和 SHA-256 校验。读取阶段不修改编辑器文档。
#pragma once
#include <QList>
#include <QString>
#include <array>
#include <functional>
#include <optional>

namespace catalog {
// 文件路径相对于包根目录。摘要对应磁盘上的原始字节。
struct File {
    QString    path;
    QByteArray sha256;
};

// 引导文件的顺序是法线、位置、深度、物体 ID 和矩阵。旧清单仍可播放 beauty。
struct Frame {
    File                               beauty;
    std::optional<std::array<File, 5>> guides;
    std::optional<File>                reference;
    QString                            label;
};
enum class Kind { scene, model, sequence };

// 模式由用户选择。推荐效果随覆盖导入应用，序列在独立查看器中打开。
enum class ImportAction { append, replace, recommended, open_sequence };

// 优先使用用户挂载的课程包，其次读取本机安装记录或当前仓库的课程包。
QString course_directory();
// 用户通过文件选择器成功挂载后，保存课程包路径供下次启动使用。
void remember_course_directory(const QString& root);

// 一个可浏览的资源。dependencies 包含场景 JSON 引用的网格和纹理。
struct Entry {
    QString     root, id, name, description, source, rights, thumbnail, recommended;
    Kind        kind = Kind::scene;
    File        file;
    QList<File> dependencies;
    QList<std::pair<QString, File>> videos;
    bool                            builtin = false;
};

// 读取索引及元数据，保留清单顺序。文件内容在用户选择导入时校验。
QList<Entry> read(const QString& root, bool bundled = false);
// 返回已验证位于资源包内的真实文件路径。
QString resolve(const QString& root, const QString& relative);
// 按块读取文件并校验摘要，调用方可在回调中报告进度或取消。
void verify(const QString& root, const File& file,
            const std::function<void(qint64)>& advanced = {});
// 读取帧清单，帧内容在播放或跳转到该帧时加载。
QList<Frame> frames(const Entry& entry);
// 校验场景及其直接引用，返回场景路径；清单缺项时在解析模型前报错。
QString verify_scene(const Entry& entry, const std::function<void(qint64)>& advanced);
} // namespace catalog
