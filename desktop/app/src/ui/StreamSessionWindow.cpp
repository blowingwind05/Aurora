// SPDX-License-Identifier: GPL-3.0-only
#include "StreamSessionWindow.h"
#include "session/SessionSupervisor.h"

#ifdef Q_OS_WIN
#include "platform/WindowsWindowFrame.h"
#endif

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDebug>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTimer>
#include <QWindow>

StreamSessionWindow::StreamSessionWindow(QQmlEngine &engine, const QUrl &source)
    : engine_(engine), source_(source)
{
}

StreamSessionWindow::~StreamSessionWindow() = default;

QString StreamSessionWindow::prepare(const QUuid &attempt, const QString &deviceName,
                                     Aurora::Session::SessionSupervisor &session, Aurora::Session::SessionView &view)
{
    QQmlComponent component(&engine_, source_);
    if (!component.isReady()) {
        qWarning().noquote() << component.errorString();
        return QCoreApplication::translate("NativeDesktop", "无法加载串流窗口。");
    }

    QVariantMap properties{
        {QStringLiteral("session"), QVariant::fromValue(static_cast<QObject *>(&session))},
        {QStringLiteral("deviceName"), deviceName}
    };
#ifdef Q_OS_WIN
    const bool nativeFrame = QGuiApplication::platformName() == QStringLiteral("windows");
    properties.insert(QStringLiteral("nativeWindowsFrame"), nativeFrame);
#endif
    std::unique_ptr<QObject> object(component.createWithInitialProperties(properties));
    auto *window = qobject_cast<QWindow *>(object.get());
    if (!window)
        return QCoreApplication::translate("NativeDesktop", "无法创建串流窗口。");

#ifdef Q_OS_WIN
    if (nativeFrame && !installWindowsWindowFrame(*window)) {
        qWarning("Aurora could not install the native Windows stream frame.");
        window->setProperty("nativeWindowsFrame", false);
    }
#endif

    object.release();
    window_.reset(window);
    attempt_ = attempt;
    const int top = window->property("videoTop").toInt();
    view.owner = window;
    view.viewport = QRect(0, top, window->width(), qMax(0, window->height() - top));
    return {};
}

void StreamSessionWindow::show()
{
    if (window_)
        window_->show();
}

void StreamSessionWindow::close(const QUuid &attempt)
{
    if (!window_ || attempt_ != attempt)
        return;

    auto *window = window_.release();
    attempt_ = {};
    window->setProperty("finished", true);
    QTimer::singleShot(0, window, [window] {
        window->close();
        window->deleteLater();
    });
}
