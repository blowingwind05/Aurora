// SPDX-License-Identifier: GPL-3.0-only
#include "StreamSurface.h"

#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>

#include <windows.h>

using Aurora::Session::StreamSurface;

namespace {

LPCWSTR wide(const QString &value)
{
    return reinterpret_cast<LPCWSTR>(value.utf16());
}

HWND hostHandle(const QString &invitation)
{
    return reinterpret_cast<HWND>(static_cast<quintptr>(invitation.section(QLatin1Char(':'), 0, 0).toLongLong()));
}

QString tagFor(const QString &invitation)
{
    return QStringLiteral("Aurora.Embedded.") + invitation.section(QLatin1Char(':'), 2, 2);
}

QString eventName(const QString &invitation, const QString &suffix)
{
    return QStringLiteral("Local\\") + tagFor(invitation) + suffix;
}

void launch(QProcess &child, const QString &invitation)
{
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("AURORA_EMBEDDED_SESSION"), invitation);
    child.setProcessEnvironment(env);
    child.start(QStringLiteral(SESSION_FAKE_CHILD), QStringList{});
}

void readLines(QProcess &child, StreamSurface &surface, QByteArray &lines)
{
    QObject::connect(&child, &QProcess::readyReadStandardOutput, &surface, [&] {
        while (child.canReadLine()) {
            const auto line = child.readLine();
            surface.handleWorkerLine(&child, line);
            lines += line;
        }
    });
}

QByteArray reportSize(const QSize &size)
{
    return "fixture-size " + QByteArray::number(size.width()) + ' ' + QByteArray::number(size.height());
}

QSize windowSize(HWND child)
{
    RECT bounds{};
    return GetClientRect(child, &bounds) ? QSize(bounds.right, bounds.bottom) : QSize();
}

} // namespace

class SessionFixtures final : public QObject
{
    Q_OBJECT

private slots:
    void attachUnderChrome();
    void rejectInvalidOwnerAndWorker();
    void stopWhenOwnerIsDestroyed();
};

void SessionFixtures::attachUnderChrome()
{
    QByteArray lines;
    StreamSurface surface;
    QQuickView owner;
    owner.setTitle(QStringLiteral("Aurora synthetic attachment fixture"));
    owner.setResizeMode(QQuickView::SizeRootObjectToView);
    owner.rootContext()->setContextProperty(QStringLiteral("streamSurface"), &surface);
    owner.setSource(QUrl::fromLocalFile(QStringLiteral(SESSION_PREVIEW_QML)));
    QCOMPARE(owner.status(), QQuickView::Ready);
    owner.resize(780, 480);
    owner.show();

    const auto invitation = surface.prepare(&owner, QRect(0, 44, 780, 436));
    QVERIFY(!invitation.isEmpty());
    const auto fields = invitation.split(QLatin1Char(':'));
    QCOMPARE(fields.size(), 3);
    QCOMPARE(fields[1].toULongLong(), static_cast<qulonglong>(GetCurrentProcessId()));
    QCOMPARE(fields[2].size(), 32);
    const auto host = hostHandle(invitation);
    QVERIFY(host);
    QCOMPARE(GetParent(host), reinterpret_cast<HWND>(owner.winId()));
    QVERIFY(GetWindowLongPtrW(host, GWL_STYLE) & WS_CHILD);
    QCOMPARE(GetPropW(host, wide(tagFor(invitation))), reinterpret_cast<HANDLE>(1));
    POINT origin{};
    QVERIFY(ClientToScreen(host, &origin));
    QVERIFY(ScreenToClient(reinterpret_cast<HWND>(owner.winId()), &origin));
    QCOMPARE(origin.y, qRound(44 * owner.devicePixelRatio()));
    QVERIFY(!surface.surfaceVisible());
    QVERIFY(surface.prepare(&owner, owner.geometry()).isEmpty());

    QProcess child;
    QSignalSpy fullscreen(&surface, &StreamSurface::fullscreenRequested);
    QSignalSpy pixelSizes(&surface, &StreamSurface::clientPixelSizeChanged);
    QSignalSpy ended(&surface, &StreamSurface::workerFinished);
    auto *toolbar = owner.rootObject()->findChild<QQuickItem *>(QStringLiteral("streamTitleBar"));
    QVERIFY(toolbar);
    auto *chrome = toolbar->parentItem();
    QVERIFY(chrome);
    QVERIFY(surface.bindWorker(&child));
    QProcess unrelated;
    QVERIFY(!surface.bindWorker(&unrelated));
    QVERIFY(!surface.handleWorkerLine(&unrelated, "aurora-connection-stage/1,qt"));
    readLines(child, surface, lines);
    launch(child, invitation);
    QTRY_COMPARE(surface.state(), StreamSurface::State::ConnectionStage);
    QVERIFY(surface.surfaceVisible());
    QVERIFY(!surface.videoReady());
    QTRY_VERIFY(lines.contains(reportSize(surface.clientPixelSize())));

    // A ready event and an identically named window owned by the UI PID are insufficient.
    WNDCLASSW impostorClass{};
    impostorClass.lpfnWndProc = DefWindowProcW;
    impostorClass.hInstance = GetModuleHandleW(nullptr);
    impostorClass.lpszClassName = L"AuroraEmbeddedStreamSurface";
    QVERIFY(RegisterClassW(&impostorClass));
    const auto impostor = CreateWindowExW(0, impostorClass.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                                         0, 0, 1, 1, host, nullptr, impostorClass.hInstance, nullptr);
    QVERIFY(impostor);
    const auto ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, wide(eventName(invitation, QStringLiteral(".ready"))));
    QVERIFY(ready);
    QVERIFY(SetEvent(ready));
    QTest::qWait(180);
    QVERIFY(!surface.videoReady());
    CloseHandle(ready);
    QVERIFY(DestroyWindow(impostor));

    QCOMPARE(child.write("ready\n"), qint64(6));
    QTRY_COMPARE(surface.state(), StreamSurface::State::Attached);
    QVERIFY(surface.videoReady());
    QVERIFY(chrome->property("connected").toBool());
    const auto video = FindWindowExW(host, nullptr, L"AuroraEmbeddedStreamSurface", nullptr);
    QVERIFY(video);
    DWORD pid = 0;
    QVERIFY(GetWindowThreadProcessId(video, &pid));
    QCOMPARE(static_cast<qint64>(pid), child.processId());
    QCOMPARE(windowSize(video), surface.clientPixelSize());

    const auto previous = surface.clientPixelSize();
    owner.resize(840, 530);
    QTRY_VERIFY(surface.clientPixelSize().width() > previous.width());
    QVERIFY(!pixelSizes.isEmpty());
    QTRY_VERIFY(lines.contains(reportSize(surface.clientPixelSize())));
    QTRY_COMPARE(windowSize(video), surface.clientPixelSize());

    QCOMPARE(child.write("fullscreen\n"), qint64(11));
    QTRY_COMPARE(fullscreen.size(), 1);
    QVERIFY(chrome->property("fullScreen").toBool());
    QTest::qWait(120);
    QCOMPARE(fullscreen.size(), 1);
    owner.showFullScreen();
    QTRY_COMPARE(owner.visibility(), QWindow::FullScreen);
    QTRY_COMPARE(windowSize(video), surface.clientPixelSize());

    const auto fullSize = surface.clientPixelSize();
    auto *floating = chrome->findChild<QWindow *>(QStringLiteral("streamFloatingOverlay"));
    auto *popup = chrome->findChild<QWindow *>(QStringLiteral("streamPanelOverlay"));
    QVERIFY(floating);
    QVERIFY(popup);
    QTRY_VERIFY(floating->isVisible());
    QCOMPARE(floating->transientParent(), &owner);
    QVERIFY(chrome->setProperty("panelKind", QStringLiteral("controls")));
    QTRY_VERIFY(popup->isVisible());
    QVERIFY(popup->winId() != owner.winId());
    QCOMPARE(popup->transientParent(), &owner);
    QVERIFY(surface.surfaceVisible());
    QVERIFY(IsWindowVisible(video));
    QCOMPARE(surface.clientPixelSize(), fullSize);
    QVERIFY(chrome->setProperty("panelKind", QString()));
    QTRY_VERIFY(!popup->isVisible());
    QVERIFY(surface.surfaceVisible());
    QCOMPARE(hostHandle(invitation), host);
    owner.showMinimized();
    QTRY_VERIFY(!surface.surfaceVisible());
    owner.showNormal();
    QTRY_VERIFY(surface.surfaceVisible());

    surface.requestStop();
    QTRY_COMPARE(ended.size(), 1);
    QCOMPARE(ended.constFirst().at(0).toInt(), 0);
    QVERIFY(lines.contains("fixture-stop-event"));
    QCOMPARE(surface.state(), StreamSurface::State::Finished);
    QVERIFY(!IsWindow(host));
    QVERIFY(!OpenEventW(SYNCHRONIZE, FALSE, wide(eventName(invitation, QStringLiteral(".ready")))));

    const auto next = surface.prepare(&owner, QRect(0, 44, 500, 350));
    QVERIFY(!next.isEmpty() && tagFor(next) != tagFor(invitation));
    surface.requestStop();
    QCOMPARE(surface.state(), StreamSurface::State::Finished);
}

void SessionFixtures::rejectInvalidOwnerAndWorker()
{
    QWindow owner;
    owner.resize(640, 480);
    QWindow nested(&owner);
    StreamSurface surface;
    QVERIFY(surface.prepare(&nested, QRect(0, 44, 600, 400)).isEmpty());
    QVERIFY(surface.attachmentFailed());

    const auto invite = surface.prepare(&owner, QRect(0, 44, 600, 400));
    QVERIFY(!invite.isEmpty());
    auto fields = invite.split(QLatin1Char(':'));
    fields[1] = QString::number(GetCurrentProcessId() + 1);
    QProcess child;
    QSignalSpy ended(&surface, &StreamSurface::workerFinished);
    QVERIFY(surface.bindWorker(&child));
    launch(child, fields.join(QLatin1Char(':')));
    QTRY_COMPARE(ended.size(), 1);
    QCOMPARE(ended.constFirst().at(0).toInt(), 74);
    QVERIFY(surface.attachmentFailed());
    QVERIFY(!surface.surfaceVisible());
}

void SessionFixtures::stopWhenOwnerIsDestroyed()
{
    QByteArray lines;
    QWindow owner;
    owner.resize(640, 480);
    owner.show();
    StreamSurface surface;
    const auto invite = surface.prepare(&owner, QRect(0, 44, 640, 436));
    QVERIFY(!invite.isEmpty());
    QProcess child;
    QSignalSpy lost(&surface, &StreamSurface::surfaceLost);
    QSignalSpy ended(&surface, &StreamSurface::workerFinished);
    QVERIFY(surface.bindWorker(&child));
    readLines(child, surface, lines);
    launch(child, invite);
    QTRY_COMPARE(surface.state(), StreamSurface::State::ConnectionStage);
    owner.destroy();
    QTRY_COMPARE(ended.size(), 1);
    QCOMPARE(lost.size(), 1);
    QVERIFY(lines.contains("fixture-stop-event"));
    QVERIFY(!surface.surfaceVisible());
    QVERIFY(!IsWindow(hostHandle(invite)));
}

int main(int argc, char **argv)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);
    SessionFixtures tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "SessionFixtures.moc"
