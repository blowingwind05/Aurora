// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>

class NativeInstance final : public QObject
{
    Q_OBJECT

public:
    enum class Result { Primary, Activated, Error };
    explicit NativeInstance(const QString &dataDirectory);
    Result start();

signals:
    void activationRequested();

private:
    Result listen();
    bool activatePrimary();
    void acceptConnections();

    const QString directory;
    const QString name;
    QLockFile lock;
    QLocalServer server;
};
