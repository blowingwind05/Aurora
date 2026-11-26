// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>

namespace AuroraPrivacyArtwork {
constexpr int MaxEncodedBytes = 8 * 1024 * 1024;
constexpr qint64 MaxImportBytes = 64LL * 1024 * 1024;
constexpr qint64 MaxSourcePixels = 32LL * 1024 * 1024;
constexpr int MaxImageEdge = 2880;

inline QByteArray prepare(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > MaxImportBytes)
        return {};

    QImageReader reader(&file);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const auto format = reader.format();
    if (format != "jpeg" && format != "png" && format != "webp")
        return {};

    const auto size = reader.size();
    if (!size.isValid() || size.isEmpty() || qint64(size.width()) * size.height() > MaxSourcePixels)
        return {};
    if (qMax(size.width(), size.height()) > MaxImageEdge)
        reader.setScaledSize(size.scaled(MaxImageEdge, MaxImageEdge, Qt::KeepAspectRatio));

    auto image = reader.read();
    if (image.isNull() || qint64(image.width()) * image.height() > MaxSourcePixels)
        return {};
    if (qMax(image.width(), image.height()) > MaxImageEdge) {
        image = image.scaled(MaxImageEdge, MaxImageEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QImage opaque(image.size(), QImage::Format_RGB32);
    if (opaque.isNull())
        return {};
    opaque.fill(Qt::black);
    {
        QPainter painter(&opaque);
        painter.drawImage(QPoint(0, 0), image);
    }

    // New bitmap pixels omit local EXIF, text and paths. Every output pixel is opaque.
    for (int attempt = 0; attempt != 4; ++attempt) {
        QByteArray png;
        QBuffer output(&png);
        if (!output.open(QIODevice::WriteOnly) || !QImageWriter(&output, "png").write(opaque))
            return {};
        if (png.size() <= MaxEncodedBytes)
            return png;
        opaque = opaque.scaled(opaque.size() * 0.75, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (opaque.isNull())
            return {};
    }
    return {};
}
}
