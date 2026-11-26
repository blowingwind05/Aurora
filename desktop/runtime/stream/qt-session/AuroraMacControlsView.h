// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDir>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QStringList>

#include <functional>

inline bool auroraMacSessionChromeEnabled()
{
    return qEnvironmentVariableIntValue("AURORA_QT_SESSION") == 1 &&
           QGuiApplication::platformName() == QStringLiteral("cocoa");
}

inline QUrl auroraMacSessionQml(const QString& file)
{
    // Build-Mac-Streamer stages the shared QML, theme and icons together.
    const QDir bin(QCoreApplication::applicationDirPath());
    return QUrl::fromLocalFile(bin.absoluteFilePath(
        QStringLiteral("../Resources/ui/desktop/app/qml/stream/") + file));
}

class AuroraMacControlsView final : public QQuickView {
    Q_OBJECT

public:
    explicit AuroraMacControlsView(const QUrl& source)
    {
        setFlags(Qt::SubWindow);
        setColor(Qt::transparent);
        setSource(source);
        if (auto* menu = rootObject()) {
            // Size the native panel before the first Qt scene graph frame.
            QMetaObject::invokeMethod(menu, "layoutNativeHome");
            connect(menu, SIGNAL(nativeSettingPageRequested(QString)), this, SLOT(openPage(QString)));
            connect(menu, SIGNAL(toggleFullscreenRequested()), this, SLOT(toggleFullscreen()));
            connect(menu, SIGNAL(connectionDetailsRequested()), this, SLOT(showConnectionDetails()));
            connect(menu, SIGNAL(disconnectRequested()), this, SLOT(disconnectStream()));
        }
    }

    void focusInitial(bool keyboard)
    {
        const QVariant reason = int(keyboard ? Qt::TabFocusReason : Qt::MouseFocusReason);
        QMetaObject::invokeMethod(rootObject(), "focusInitial", Q_ARG(QVariant, reason));
    }

    std::function<void(int)> onOpenPage;
    std::function<void()> onToggleFullscreen;
    std::function<void()> onDisconnect;
    std::function<void()> onShowConnectionDetails;

private slots:
    void openPage(const QString& page)
    {
        const QStringList pages{QStringLiteral("quality"), QStringLiteral("audio"),
                                QStringLiteral("input"), QStringLiteral("safety")};
        const int index = pages.indexOf(page);
        if (index >= 0 && onOpenPage) onOpenPage(index + 1);
    }

    void toggleFullscreen() { if (onToggleFullscreen) onToggleFullscreen(); }
    void disconnectStream() { if (onDisconnect) onDisconnect(); }
    void showConnectionDetails() { if (onShowConnectionDetails) onShowConnectionDetails(); }
};
