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
            displayGroup = chrome->findChild<QQuickItem*>(QStringLiteral("streamDisplayGroup"));
            connect(chrome, SIGNAL(nativePanelRequested(QString,bool)), this, SLOT(openPanel(QString,bool)));
            connect(chrome, SIGNAL(displayRequested(QString)), this, SLOT(selectDisplay(QString)));
            connect(chrome, SIGNAL(createDisplayRequested()), this, SLOT(createDisplay()));
            connect(chrome, SIGNAL(removeDisplayRequested(QString)), this, SLOT(removeDisplay(QString)));
        }
    }

    bool ready() const { return status() == QQuickView::Ready && controls && statusButton; }

    bool hasButtonAt(const QPointF& scenePoint) const
    {
        return hit(controls, scenePoint) || hit(statusButton, scenePoint) ||
            (displayGroup && rootObject()->property("displayControls").toBool() && hit(displayGroup, scenePoint));
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
    std::function<void(const QRectF&)> onDisplays;
    std::function<void(const QString&, const QString&)> onDisplayAction;

private slots:
    void openPanel(const QString& kind, bool keyboard)
    {
        if (kind == QStringLiteral("controls") && onControls) onControls(keyboard);
        if (kind == QStringLiteral("connection") && onConnection) onConnection(connectionRect());
        if (kind == QStringLiteral("displays") && onDisplays) onDisplays(itemRect(displayGroup));
    }

    void selectDisplay(const QString& id)
    {
        if (onDisplayAction) onDisplayAction(QStringLiteral("select"), id);
    }

    void createDisplay()
    {
        if (onDisplayAction) onDisplayAction(QStringLiteral("create"), {});
    }

    void removeDisplay(const QString& id)
    {
        if (onDisplayAction) onDisplayAction(QStringLiteral("remove"), id);
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
    QQuickItem* displayGroup = nullptr;
};
