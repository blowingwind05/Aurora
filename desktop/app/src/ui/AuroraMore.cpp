// SPDX-License-Identifier: GPL-3.0-only
#include "AuroraMore.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <array>
#include <optional>

namespace {

constexpr int responseLimit = 1024 * 1024;
constexpr int timeoutMs = 8000;
const auto networkError = QStringLiteral("无法连接 GitHub，请检查网络后重试。");
const auto feedbackEmail = QStringLiteral("blowingwind05@mail.ustc.edu.cn");
const auto repository = QStringLiteral("https://github.com/blowingwind05/Aurora");

const QHash<QString, QUrl> &projectLinks()
{
    static const QHash<QString, QUrl> links{
        {QStringLiteral("source"), QUrl(repository)},
        {QStringLiteral("help"), QUrl(repository + QStringLiteral("#从极光窗口开始"))},
        {QStringLiteral("mail"), QUrl(QStringLiteral("mailto:") + feedbackEmail)},
        {QStringLiteral("issues"), QUrl(repository + QStringLiteral("/issues/new/choose"))},
        {QStringLiteral("author"), QUrl(QStringLiteral("https://github.com/blowingwind05"))},
        {QStringLiteral("license"), QUrl(repository + QStringLiteral("/blob/main/LICENSE"))},
        {QStringLiteral("moonlight"), QUrl(QStringLiteral("https://github.com/moonlight-stream/moonlight-qt"))},
        {QStringLiteral("sunshine"), QUrl(QStringLiteral("https://github.com/SunshineStream/Sunshine"))},
        {QStringLiteral("tailscale"), QUrl(QStringLiteral("https://github.com/tailscale/tailscale"))}
    };
    return links;
}

using Version = std::array<int, 4>;

std::optional<Version> parseVersion(const QString &text)
{
    const auto parts = text.split(QLatin1Char('.'));
    if (parts.size() < 2 || parts.size() > 4)
        return std::nullopt;

    // System.Version accepts signed Int32 components with ASCII whitespace.
    static const QRegularExpression component(QStringLiteral(
        "\\A[\\x09-\\x0d ]*([+-]?[0-9]+)[\\x09-\\x0d ]*\\x00*\\z"));
    Version version{};
    for (int i = 0; i < parts.size(); ++i) {
        const auto match = component.match(parts.at(i));
        bool ok = false;
        const int value = match.captured(1).toInt(&ok);
        if (!ok || value < 0)
            return std::nullopt;
        version[i] = value;
    }
    return version;
}

QString displayVersion(const Version &version)
{
    auto text = QStringLiteral("%1.%2.%3").arg(version[0]).arg(version[1]).arg(version[2]);
    if (version[3] > 0)
        text += QStringLiteral(".%1").arg(version[3]);
    return text;
}

QString releaseMessage(const QByteArray &body, const Version &current)
{
    const auto document = QJsonDocument::fromJson(body);
    if (document.isNull()) {
        // QJsonDocument only accepts objects and arrays at the root.
        const auto wrapped = QJsonDocument::fromJson('[' + body + ']');
        if (!wrapped.isArray() || wrapped.array().size() != 1)
            return QStringLiteral("无法读取 GitHub 的版本信息。");
    }

    const auto info = document.object();
    if (info.value(QStringLiteral("draft")) != QJsonValue(false) ||
            info.value(QStringLiteral("prerelease")) != QJsonValue(false) ||
            !info.value(QStringLiteral("tag_name")).isString()) {
        return QStringLiteral("GitHub 未返回可用的正式版本。");
    }

    auto tag = info.value(QStringLiteral("tag_name")).toString().trimmed();
    if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        tag.remove(0, 1);
    const auto latest = parseVersion(tag);
    if (!latest)
        return QStringLiteral("无法识别 GitHub 的正式版本号。");
    if (*latest > current)
        return QStringLiteral("发现新版本 %1。").arg(displayVersion(*latest));
    if (*latest == current)
        return QStringLiteral("当前已是最新版本");
    return QStringLiteral("当前版本高于已发布的正式版本 %1。").arg(displayVersion(*latest));
}

} // namespace

AuroraMore::AuroraMore(QNetworkAccessManager &network, OpenUrl openUrl)
    : network(network), openUrl(openUrl)
{
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    connect(&deadline, &QTimer::timeout, this, [this] {
        finish(QStringLiteral("检查更新超时，请重试。"));
    });
}

void AuroraMore::setApplicationVersion(const QString &version)
{
    if (hasSnapshot && currentVersion == version)
        return;
    currentVersion = version;
    hasSnapshot = true;
    emit versionLabelChanged();
}

QString AuroraMore::versionLabel() const
{
    if (!hasSnapshot)
        return {};
    const auto version = parseVersion(currentVersion);
    return version ? QStringLiteral("版本 %1").arg(displayVersion(*version)) : QStringLiteral("版本未知");
}

QString AuroraMore::feedbackAddress() const
{
    return feedbackEmail;
}

void AuroraMore::request(const QString &action)
{
    if (action == QStringLiteral("checkUpdates")) {
        checkUpdates();
        return;
    }

    const auto &links = projectLinks();
    const auto target = links.constFind(action);
    if (target != links.cend() && !openUrl(target.value())) {
        emit notice(action == QStringLiteral("mail")
            ? QStringLiteral("无法打开系统邮件应用。") : QStringLiteral("无法打开链接。"));
    }
}

void AuroraMore::checkUpdates()
{
    if (checking() || !hasSnapshot)
        return;
    updateInProgress = true;
    emit checkingChanged();
    const auto version = parseVersion(currentVersion);
    if (!version)
        return finish(QStringLiteral("无法读取当前版本。"));

    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/blowingwind05/Aurora/releases/latest")));
    const auto userAgent = QStringLiteral("Aurora/%1.%2.%3.%4")
        .arg((*version)[0]).arg((*version)[1]).arg((*version)[2]).arg((*version)[3]);
    request.setRawHeader("User-Agent", userAgent.toLatin1());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);

    activeReply = network.get(request);
    activeReply->setReadBufferSize(responseLimit + 1);
    connect(activeReply, &QNetworkReply::metaDataChanged, this, [this] {
        if (activeReply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > responseLimit)
            finish(networkError);
    });
    connect(activeReply, &QIODevice::readyRead, this, [this] {
        if (!readResponse())
            finish(networkError);
    });
    connect(activeReply, &QNetworkReply::finished, this, &AuroraMore::replyFinished);
    deadline.start(timeoutMs);
}

bool AuroraMore::readResponse()
{
    response += activeReply->read(responseLimit + 1 - response.size());
    return response.size() <= responseLimit;
}

void AuroraMore::replyFinished()
{
    const int status = activeReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (!readResponse() || status == 0)
        return finish(networkError);
    if (status == 404)
        return finish(QStringLiteral("GitHub 未返回可用的正式版本（HTTP 404）。"));
    if (status < 200 || status >= 300)
        return finish(QStringLiteral("无法检查更新。GitHub 返回 HTTP %1。").arg(status));
    if (activeReply->error() != QNetworkReply::NoError)
        return finish(networkError);

    const auto version = parseVersion(currentVersion);
    finish(version ? releaseMessage(response, *version) : QStringLiteral("无法读取当前版本。"));
}

void AuroraMore::finish(const QString &message)
{
    deadline.stop();
    if (activeReply) {
        auto reply = activeReply;
        activeReply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        if (reply->isRunning())
            reply->abort();
        reply->deleteLater();
    }
    response.clear();
    updateInProgress = false;
    emit checkingChanged();
    emit notice(message);
}
