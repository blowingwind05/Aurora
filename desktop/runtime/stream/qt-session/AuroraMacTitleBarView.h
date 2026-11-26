// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QQuickItem>
#include <QQuickView>
#include <QVariant>

#include <functional>

class AuroraMacTitleBarView final : public QQuickView {
    Q_OBJECT

public:
    explicit AuroraMacTitleBarView(const QUrl& source)
    {
        setFlags(Qt::SubWindow);
        setResizeMode(SizeRootObjectToView);
        setColor(QColor(0xed, 0xf1, 0xf8));
        setSource(source);
        if (auto* chrome = rootObject()) {
            controls = chrome->findChild<QQuickItem*>(QStringLiteral("controlCenter"));
            statusButton = chrome->findChild<QQuickItem*>(QStringLiteral("connectionStatus"));
            connect(chrome, SIGNAL(nativePanelRequested(QString,bool)), this, SLOT(openPanel(QString,bool)));
        }
    }

    bool ready() const { return status() == QQuickView::Ready && controls && statusButton; }

    bool hasButtonAt(const QPointF& scenePoint) const
    {
        return hit(controls, scenePoint) || hit(statusButton, scenePoint);
    }

    QRectF controlsRect() const { return itemRect(controls); }
    QRectF connectionRect() const { return itemRect(statusButton); }

    void setSelectedPanel(const QString& kind)
    {
        if (auto* chrome = rootObject()) chrome->setProperty("panelKind", kind);
    }

    void focusControls() { QMetaObject::invokeMethod(rootObject(), "focusNativeControls"); }

    void clearKeyboardFocus()
    {
        if (auto* chrome = rootObject()) chrome->forceActiveFocus(Qt::OtherFocusReason);
    }

    std::function<void(bool)> onControls;
    std::function<void(const QRectF&)> onConnection;

private slots:
    void openPanel(const QString& kind, bool keyboard)
    {
        if (kind == QStringLiteral("controls") && onControls) onControls(keyboard);
        if (kind == QStringLiteral("connection") && onConnection) onConnection(connectionRect());
    }

private:
    static QRectF itemRect(const QQuickItem* item)
    {
        return item ? item->mapRectToScene(QRectF(0, 0, item->width(), item->height())) : QRectF();
    }

    static bool hit(const QQuickItem* item, const QPointF& point)
    {
        return item && item->isVisible() && item->isEnabled() && item->contains(item->mapFromScene(point));
    }

    QQuickItem* controls = nullptr;
    QQuickItem* statusButton = nullptr;
};
