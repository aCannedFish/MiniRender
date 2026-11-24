// 校验缓存头、字节数和 SHA-256，按小端浮点数保存面角传输。失败的写入不替换已有缓存。
#include "prt_cache.hpp"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QtEndian>
#include <bit>

namespace {
constexpr quint32 magic = 0x50525431;
using namespace minirender;

std::shared_ptr<PrtBake> read_cache(const QString& path, std::uint64_t key,
                                    const SceneSnapshot& scene, CancellationToken token) {
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
        return {};
    }
    QDataStream stream(&input);
    stream.setByteOrder(QDataStream::LittleEndian);
    quint32 tag;
    quint64 saved, count;
    stream >> tag >> saved >> count;
    auto    bake     = std::make_shared<PrtBake>();
    quint64 expected = 0;
    for (const auto& obj : scene.instances) {
        bake->offsets.push_back(static_cast<std::uint32_t>(expected));
        expected += obj.mesh->triangles.size() * 3;
    }
    if (stream.status() != QDataStream::Ok || magic != tag || saved != key || count != expected ||
        input.size() != static_cast<qint64>(20 + count * 108 + 32)) {
        return {};
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    bake->vertices.resize(count);
    std::array<quint32, 27 * 64> words{};
    for (std::size_t i = 0; i < count;) {
        token.check();
        const auto rows  = std::min<std::size_t>(64, count - i);
        const auto size  = static_cast<qint64>(108 * rows);
        auto*      bytes = reinterpret_cast<char*>(words.data());
        if (input.read(bytes, size) != size) {
            return {};
        }
        digest.addData(QByteArrayView(bytes, size));
        std::size_t offset = 0;
        for (std::size_t j = 0; j < rows; ++j) {
            for (auto& coefficient : bake->vertices[i + j]) {
                for (int c = 0; c < 3; ++c) {
                    coefficient[c] = std::bit_cast<float>(qFromLittleEndian(words[offset++]));
                }
            }
        }
        i += rows;
    }
    return digest.result() == input.read(32) ? bake : nullptr;
}

void write_cache(const QString& path, std::uint64_t key, const PrtBake& bake,
                 CancellationToken token) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << magic << quint64(key) << quint64(bake.vertices.size());
    QCryptographicHash           digest(QCryptographicHash::Sha256);
    std::array<quint32, 27 * 64> words{};
    for (std::size_t i = 0; i < bake.vertices.size();) {
        token.check();
        const auto  rows   = std::min<std::size_t>(64, bake.vertices.size() - i);
        std::size_t offset = 0;
        for (std::size_t j = 0; j < rows; ++j) {
            for (auto& k : bake.vertices[i + j]) {
                for (int c = 0; c < 3; ++c) {
                    words[offset++] = qToLittleEndian(std::bit_cast<quint32>(k[c]));
                }
            }
        }
        const QByteArrayView bytes(reinterpret_cast<const char*>(words.data()),
                                   static_cast<qsizetype>(rows * 108));
        if (file.write(bytes.data(), bytes.size()) != bytes.size()) {
            return;
        }
        digest.addData(bytes);
        i += rows;
    }
    token.check();
    if (file.write(digest.result()) == 32) {
        file.commit();
    }
}
} // namespace

CachedPrt load_or_bake_prt(const QString&                                       directory,
                           std::shared_ptr<const minirender::SceneSnapshot>     scene,
                           minirender::PrtSettings                              settings,
                           minirender::CancellationToken                        cancellation,
                           const std::function<void(std::size_t, std::size_t)>& progress) {
    const auto key  = minirender::prt_cache_key(*scene, settings, cancellation);
    const auto path = QDir(directory).filePath(QString::number(key, 16) + ".mrprt");
    if (auto bake = read_cache(path, key, *scene, cancellation)) {
        return {std::move(bake), true};
    }
    auto bake = std::make_shared<minirender::PrtBake>(
        minirender::bake_prt(scene, settings, cancellation, progress));
    if (QDir().mkpath(directory)) {
        write_cache(path, key, *bake, cancellation);
    }
    return {std::move(bake), false};
}
