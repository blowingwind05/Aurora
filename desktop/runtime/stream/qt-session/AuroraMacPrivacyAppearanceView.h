// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QJSValue>
#include <QQuickItem>
#include <QQuickView>
#include <QStringList>
#include <QVariantMap>

#include <functional>

class AuroraMacPrivacyAppearanceView final : public QQuickView {
    Q_OBJECT

public:
    explicit AuroraMacPrivacyAppearanceView(const QUrl& source)
    {
        setFlags(Qt::SubWindow);
        setResizeMode(SizeRootObjectToView);
        setColor(Qt::transparent);
        setSource(source);
        if (auto* editor = rootObject()) {
            connect(editor, SIGNAL(importRequested(int)), this, SLOT(importImage(int)));
            connect(editor, SIGNAL(saveRequested(int,QVariant)), this, SLOT(saveStyle(int,QVariant)));
            connect(editor, SIGNAL(closeRequested()), this, SLOT(closeEditor()));
        }
    }

    static QVariantMap defaultStyle()
    {
        return {{QStringLiteral("id"), QStringLiteral("default")},
                {QStringLiteral("kind"), QStringLiteral("default")}};
    }

    static QVariantMap customStyle(const QString& id, const QString& name, const QString& file)
    {
        return {{QStringLiteral("id"), id}, {QStringLiteral("kind"), QStringLiteral("custom")},
                {QStringLiteral("name"), name},
                {QStringLiteral("previewUrl"), QUrl::fromLocalFile(file).toString()}};
    }

    QString presetFile(const QString& id) const
    {
        static const QStringList presets{QStringLiteral("mountain-valley"), QStringLiteral("forest-path"),
            QStringLiteral("blossom-grove"), QStringLiteral("ginkgo-lakeside"), QStringLiteral("baikal-piano"),
            QStringLiteral("baikal-accordion"), QStringLiteral("sunflower-field"), QStringLiteral("sunset-sky")};
        return presets.contains(id) ? source().resolved(QUrl(QStringLiteral("../../../Assets/Wallpapers/") +
            id + QStringLiteral(".webp"))).toLocalFile() : QString();
    }

    void capability(bool available, const QString& reason)
    {
        rootObject()->setProperty("available", available);
        rootObject()->setProperty("disabledReason", reason);
    }

    void begin(const QString& device, const QVariantMap& saved)
    {
        QMetaObject::invokeMethod(rootObject(), "beginEdit", Q_ARG(QVariant, QVariant(device)),
            Q_ARG(QVariant, QVariant(saved)), Q_ARG(QVariant, QVariant(defaultStyle())), Q_ARG(QVariant, QVariant()));
        QMetaObject::invokeMethod(rootObject(), "focusEditor");
    }

    void end() { QMetaObject::invokeMethod(rootObject(), "endEdit"); }

    void imported(int request, const QVariantMap& style)
    {
        QMetaObject::invokeMethod(rootObject(), "provideImport", Q_ARG(QVariant, QVariant(request)),
                                  Q_ARG(QVariant, QVariant(style)));
    }

    void finishImport(int request, const QString& message)
    {
        QMetaObject::invokeMethod(rootObject(), "finishImport", Q_ARG(QVariant, QVariant(request)),
                                  Q_ARG(QVariant, QVariant(message)));
    }

    void finishSave(int request, const QString& result, const QString& message)
    {
        QMetaObject::invokeMethod(rootObject(), "finishMacSave", Q_ARG(QVariant, QVariant(request)),
            Q_ARG(QVariant, QVariant(result)), Q_ARG(QVariant, QVariant(message)));
    }

    std::function<void(int)> onImport;
    std::function<void(int, const QVariantMap&)> onSave;
    std::function<void()> onClose;

private slots:
    void importImage(int request) { if (onImport) onImport(request); }
    void saveStyle(int request, const QVariant& style)
    {
        if (onSave) {
            const auto values = style.userType() == qMetaTypeId<QJSValue>()
                ? style.value<QJSValue>().toVariant().toMap() : style.toMap();
            onSave(request, values);
        }
    }
    void closeEditor() { if (onClose) onClose(); }
};
