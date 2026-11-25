// SPDX-License-Identifier: GPL-3.0-only
#include "HostWallpaper.h"

#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace Aurora::Wallpaper;

namespace {

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

bool saveColor(const QString &path, const QSize &size, QRgb color)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    return image.save(path, "PNG");
}

QString profile(const QTemporaryDir &temp)
{
    return temp.filePath(QStringLiteral("Aurora/Sunshine/Bundled/config"));
}

QByteArray largeBmpHeader()
{
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("BM", 2);
    stream << quint32(54) << quint16(0) << quint16(0) << quint32(54)
           << quint32(40) << qint32(10001) << qint32(10000) << quint16(1) << quint16(24)
           << quint32(0) << quint32(0) << qint32(0) << qint32(0) << quint32(0) << quint32(0);
    return header;
}

} // namespace

class HostWallpaperTest final : public QObject
{
    Q_OBJECT

private slots:
    void writesJpegAndRefreshesChangedSource();
    void indexedSourceRetainsColors();
    void sourceAndPreviewLimitsKeepExistingCache();
    void unsafeDestinationLeavesSourceIntact();
    void updaterChecksGateAndRunsSingleFlight();
};

void HostWallpaperTest::writesJpegAndRefreshesChangedSource()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    HostWallpaperCache cache(profile(temp));
    QCOMPARE(cache.wallpaperPath(), temp.filePath("Aurora/Sunshine/Bundled/config/aurora-wallpaper.jpg"));
    const auto source = temp.filePath(QStringLiteral("wallpaper \u6d4b\u8bd5.png"));
    QVERIFY(saveColor(source, QSize(1921, 1081), qRgb(30, 70, 130)));
    const auto original = readFile(source);
    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::Updated);
    QCOMPARE(readFile(source), original);
    const auto first = readFile(cache.wallpaperPath());
    QVERIFY(!first.isEmpty());
    QVERIFY(first.size() <= HostWallpaperCache::maxPreviewBytes);
    QCOMPARE(QImageReader::imageFormat(cache.wallpaperPath()), QByteArray("jpeg"));
    QCOMPARE(QImage(cache.wallpaperPath()).size(), QSize(960, 540));
    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::Unchanged);
    QCOMPARE(readFile(cache.wallpaperPath()), first);

    QVERIFY(saveColor(source, QSize(9, 7), qRgb(150, 60, 20)));
    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::Updated);
    QCOMPARE(QImage(cache.wallpaperPath()).size(), QSize(9, 7));
    QVERIFY(readFile(cache.wallpaperPath()) != first);
    QVERIFY(!QFile::exists(temp.filePath("Aurora/Wallpapers")));

    QVERIFY(QFile::remove(cache.wallpaperPath()));
    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::Updated);
    const auto entries = QDir(profile(temp)).entryList(QDir::Files | QDir::Hidden);
    QCOMPARE(entries, QStringList{"aurora-wallpaper.jpg"});
}

void HostWallpaperTest::indexedSourceRetainsColors()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    HostWallpaperCache cache(profile(temp));
    const auto source = temp.filePath("palette.png");
    QImage image(16, 10, QImage::Format_Indexed8);
    image.setColorTable({qRgb(0, 0, 0), qRgb(32, 133, 201)});
    image.fill(1);
    QVERIFY(image.save(source, "PNG"));

    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::Updated);
    const QImage preview(cache.wallpaperPath());
    QCOMPARE(preview.size(), image.size());
    const auto pixel = preview.pixel(5, 5);
    QVERIFY(qAbs(qRed(pixel) - 32) < 6);
    QVERIFY(qAbs(qGreen(pixel) - 133) < 6);
    QVERIFY(qAbs(qBlue(pixel) - 201) < 6);
}

void HostWallpaperTest::sourceAndPreviewLimitsKeepExistingCache()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    HostWallpaperCache cache(profile(temp));
    const auto good = temp.filePath("good.png");
    QVERIFY(saveColor(good, QSize(20, 16), qRgb(25, 110, 60)));
    QCOMPARE(cache.refreshFromFile(good), HostWallpaperResult::Updated);
    const auto previous = readFile(cache.wallpaperPath());

    QCOMPARE(cache.refreshFromFile(temp.filePath("missing.png")), HostWallpaperResult::NoSource);
    QCOMPARE(cache.refreshFromFile("https://example.invalid/wallpaper.png"), HostWallpaperResult::NoSource);
    QCOMPARE(cache.refreshFromFile("//offline-fixture.invalid/images/image.png"), HostWallpaperResult::NoSource);
    const auto bad = temp.filePath("invalid.png");
    QVERIFY(writeFile(bad, "not an image"));
    QCOMPARE(cache.refreshFromFile(bad), HostWallpaperResult::InvalidImage);

    QFile tooManyBytes(temp.filePath("oversized.png"));
    QVERIFY(tooManyBytes.open(QIODevice::WriteOnly));
    QVERIFY(tooManyBytes.resize(HostWallpaperCache::maxInputBytes + 1));
    tooManyBytes.close();
    QCOMPARE(cache.refreshFromFile(tooManyBytes.fileName()), HostWallpaperResult::InputTooLarge);

    const auto tooManyPixels = temp.filePath("header-only.bmp");
    QVERIFY(writeFile(tooManyPixels, largeBmpHeader()));
    QCOMPARE(QImageReader(tooManyPixels).size(), QSize(10001, 10000));
    QCOMPARE(cache.refreshFromFile(tooManyPixels), HostWallpaperResult::InputTooLarge);

    QImage noise(960, 960, QImage::Format_RGB32);
    quint32 random = 9717;
    for (int y = 0; y < noise.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(noise.scanLine(y));
        for (int x = 0; x < noise.width(); ++x) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            row[x] = 0xff000000u | (random & 0x00ffffffu);
        }
    }
    const auto tooLargePreview = temp.filePath("noise.png");
    QVERIFY(noise.save(tooLargePreview, "PNG"));
    QByteArray fixtureJpeg;
    QBuffer buffer(&fixtureJpeg);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(noise.save(&buffer, "JPEG", HostWallpaperCache::jpegQuality));
    QVERIFY(fixtureJpeg.size() > HostWallpaperCache::maxPreviewBytes);
    QCOMPARE(cache.refreshFromFile(tooLargePreview), HostWallpaperResult::OutputTooLarge);
    QCOMPARE(readFile(cache.wallpaperPath()), previous);
    QCOMPARE(QDir(profile(temp)).entryList(QDir::Files | QDir::Hidden), QStringList{"aurora-wallpaper.jpg"});
}

void HostWallpaperTest::unsafeDestinationLeavesSourceIntact()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    HostWallpaperCache cache(profile(temp));
    const auto source = temp.filePath("source.png");
    QVERIFY(saveColor(source, QSize(32, 21), qRgb(70, 25, 100)));
    const auto original = readFile(source);

    QVERIFY(QDir().mkpath(cache.wallpaperPath()));
    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::WriteFailed);
    QVERIFY(QFileInfo(cache.wallpaperPath()).isDir());
    QVERIFY(QDir(profile(temp)).rmdir("aurora-wallpaper.jpg"));
    QCOMPARE(cache.refreshFromFile(source), HostWallpaperResult::Updated);

    const auto cached = readFile(cache.wallpaperPath());
    QCOMPARE(cache.refreshFromFile(cache.wallpaperPath()), HostWallpaperResult::WriteFailed);
    QCOMPARE(readFile(cache.wallpaperPath()), cached);
    QCOMPARE(readFile(source), original);
}

void HostWallpaperTest::updaterChecksGateAndRunsSingleFlight()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto source = temp.filePath("test input.png");
    QVERIFY(saveColor(source, QSize(500, 180), qRgb(8, 150, 90)));

    int lookups = 0;
    QThread *lookupThread = nullptr;
    HostWallpaperUpdater updater(profile(temp), [&] {
        ++lookups;
        lookupThread = QThread::currentThread();
        return source;
    });
    QSignalSpy finished(&updater, &HostWallpaperUpdater::finished);

    QVERIFY(!updater.refreshIfAllowed(false));
    QCOMPARE(lookups, 0);
    QVERIFY(!QFileInfo::exists(profile(temp)));
    QVERIFY(updater.refreshIfAllowed(true));
    QCOMPARE(lookups, 1);
    QCOMPARE(lookupThread, QThread::currentThread());
    QVERIFY(finished.isEmpty());
    QVERIFY(!updater.refreshIfAllowed(true));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
    QCOMPARE(finished.first().first().value<HostWallpaperResult>(), HostWallpaperResult::Updated);
    QCOMPARE(QImage(updater.wallpaperPath()).size(), QSize(500, 180));
    QVERIFY(!updater.refreshIfAllowed(true));
    QCOMPARE(lookups, 1);
}

QTEST_GUILESS_MAIN(HostWallpaperTest)
#include "HostWallpaperTest.moc"
