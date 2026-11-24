// SPDX-License-Identifier: GPL-3.0-only
#include "BundledAdmin.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace Aurora::Sunshine {
namespace {

using Error = BundledAdmin::Error;
using Result = BundledAdmin::Result;

constexpr quint16 bundledAdminPort = 48990;
constexpr int deadlineMs = 5000;
constexpr qint64 bodyLimit = 256 * 1024;
constexpr qint64 certificateLimit = 64 * 1024;

Result failure(Error code, QString message, int status = 0)
{
    return {code, status, std::move(message), {}};
}

Result tlsFailure()
{
    return failure(Error::Tls, QStringLiteral("无法验证内置 Sunshine 的公开证书。"));
}

bool localAbsolutePath(const QString &path)
{
    const auto normalized = QDir::fromNativeSeparators(path);
    return QDir::isAbsolutePath(normalized) && !normalized.startsWith(QStringLiteral("//")) &&
        !normalized.startsWith(QLatin1Char(':'));
}

QByteArray readPin(const QString &profileDirectory)
{
    if (!localAbsolutePath(profileDirectory))
        return {};

    const QFileInfo profile(profileDirectory);
    const auto canonicalProfile = profile.canonicalFilePath();
    if (!profile.isDir() || !localAbsolutePath(canonicalProfile))
        return {};

    const QFileInfo certificate(QDir(canonicalProfile).filePath(QStringLiteral("credentials/cacert.pem")));
    const auto canonicalCertificate = certificate.canonicalFilePath();
    if (!certificate.isFile() || !localAbsolutePath(canonicalCertificate))
        return {};

    const auto relative = QDir(canonicalProfile).relativeFilePath(canonicalCertificate);
    if (QDir::isAbsolutePath(relative) || relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")))
        return {};

    QFile file(canonicalCertificate);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const auto pem = file.read(certificateLimit + 1);
    if (file.error() != QFileDevice::NoError || pem.size() > certificateLimit)
        return {};
    const auto certificates = QSslCertificate::fromData(pem, QSsl::Pem);
    return certificates.isEmpty() ? QByteArray{} : certificates.first().digest(QCryptographicHash::Sha256);
}

bool isLaunchPassword(const QByteArray &password)
{
    return password.size() == 64 && std::all_of(password.cbegin(), password.cend(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    });
}

bool matchesPin(const QByteArray &expected, const QSslCertificate &peer)
{
    if (peer.isNull())
        return false;
    const auto actual = peer.digest(QCryptographicHash::Sha256);
    if (actual.size() != 32 || expected.size() != 32)
        return false;
    unsigned char difference = 0;
    for (int i = 0; i < 32; ++i)
        difference |= static_cast<unsigned char>(expected.at(i) ^ actual.at(i));
    return difference == 0;
}

class ConfigExchange final : public QObject
{
public:
    ConfigExchange(QObject *parent, quint16 port, const QByteArray &pin, const QByteArray &authorization,
                   const std::optional<QByteArray> &body, BundledAdmin::Completion completion)
        : QObject(parent), pin_(pin), completion_(std::move(completion))
    {
        url_.setScheme(QStringLiteral("https"));
        url_.setHost(QStringLiteral("127.0.0.1"));
        url_.setPort(port);
        url_.setPath(QStringLiteral("/api/config"));

        // A private manager per exchange makes every request perform its own pin check.
        network_.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        QNetworkRequest request(url_);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("Authorization", authorization);

        auto tls = QSslConfiguration::defaultConfiguration();
        tls.setProtocol(QSsl::TlsV1_2OrLater);
        tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
        tls.setCaCertificates({});
        tls.setLocalCertificateChain({});
        tls.setPrivateKey(QSslKey{});
        tls.setSessionTicket({});
        tls.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
        tls.setSslOption(QSsl::SslOptionDisableSessionSharing, true);
        tls.setSslOption(QSsl::SslOptionDisableSessionPersistence, true);
        request.setSslConfiguration(tls);

        if (body) {
            request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
            reply_ = network_.post(request, *body);
        } else {
            reply_ = network_.get(request);
        }
        reply_->setReadBufferSize(bodyLimit + 1);

        connect(reply_, &QNetworkReply::sslErrors, this, [this](const QList<QSslError> &errors) {
            // The bundled public certificate is the trust anchor, including for self-signed hosts.
            if (verifiedPeer())
                reply_->ignoreSslErrors(errors);
            else
                finish(tlsFailure());
        });
        connect(reply_, &QNetworkReply::encrypted, this, [this] {
            // Qt emits encrypted before sending HTTP data on this connection.
            pinChecked_ = verifiedPeer();
            if (!pinChecked_)
                finish(tlsFailure());
        });
        connect(reply_, &QNetworkReply::metaDataChanged, this, [this] { checkHeaders(); });
        connect(reply_, &QIODevice::readyRead, this, [this] { readBody(); });
        connect(reply_, &QNetworkReply::finished, this, [this] { handleFinished(); });

        deadline_.setSingleShot(true);
        deadline_.setTimerType(Qt::PreciseTimer);
        connect(&deadline_, &QTimer::timeout, this, [this] {
            finish(failure(Error::Timeout, QStringLiteral("Sunshine 没有及时响应，请检查本机服务后重试。")));
        });
        deadline_.start(deadlineMs);
    }

    ~ConfigExchange() override
    {
        disconnect(reply_, nullptr, this, nullptr);
        if (reply_->isRunning())
            reply_->abort();
    }

private:
    bool verifiedPeer() const
    {
        return reply_->url() == url_ && matchesPin(pin_, reply_->sslConfiguration().peerCertificate());
    }

    int statusCode() const
    {
        return reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }

    bool checkHeaders()
    {
        if (finished_)
            return false;
        if (reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong() > bodyLimit) {
            finish(failure(Error::ResponseTooLarge, QStringLiteral("Sunshine 返回的配置响应过大。"), statusCode()));
            return false;
        }
        const auto encoding = reply_->rawHeader("Content-Encoding").trimmed().toLower();
        if (!encoding.isEmpty() && encoding != "identity") {
            finish(failure(Error::InvalidResponse, QStringLiteral("Sunshine 返回了不支持的响应编码。"), statusCode()));
            return false;
        }
        return true;
    }

    bool readBody()
    {
        if (finished_)
            return false;
        response_ += reply_->read(bodyLimit + 1 - response_.size());
        if (response_.size() > bodyLimit) {
            finish(failure(Error::ResponseTooLarge, QStringLiteral("Sunshine 返回的配置响应过大。"), statusCode()));
            return false;
        }
        return true;
    }

    void handleFinished()
    {
        if (finished_)
            return;

        const int status = statusCode();
        if (reply_->error() == QNetworkReply::SslHandshakeFailedError || (status != 0 && (!pinChecked_ || !verifiedPeer())))
            return finish(tlsFailure());
        if (!checkHeaders() || !readBody())
            return;
        if (status == 401)
            return finish(failure(Error::Unauthorized, QStringLiteral("Sunshine 账号或密码不正确。"), status));
        if (status != 0 && (status < 200 || status >= 300))
            return finish(failure(Error::HttpStatus, QStringLiteral("Sunshine 拒绝请求（HTTP %1）。").arg(status), status));
        if (status == 0 || reply_->error() != QNetworkReply::NoError)
            return finish(failure(Error::Network, QStringLiteral("无法连接内置 Sunshine。"), status));

        const auto document = QJsonDocument::fromJson(response_);
        if (!document.isObject())
            return finish(failure(Error::InvalidResponse, QStringLiteral("Sunshine 返回的配置响应不是有效的 JSON 对象。"), status));
        const auto config = document.object();
        if (config.value(QStringLiteral("status")) != QJsonValue(true))
            return finish(failure(Error::Rejected, QStringLiteral("Sunshine 未接受这项操作。"), status));

        finish({Error::None, status, {}, config});
    }

    void finish(Result result)
    {
        if (finished_)
            return;
        finished_ = true;
        deadline_.stop();
        if (reply_->isRunning())
            reply_->abort();
        // Run callbacks after Qt has left the reply signal, especially during TLS verification.
        QTimer::singleShot(0, this, [this, result = std::move(result)]() mutable {
            auto completion = std::move(completion_);
            // The callback may destroy the client while this event is being delivered.
            setParent(nullptr);
            deleteLater();
            completion(std::move(result));
        });
    }

    QNetworkAccessManager network_;
    QTimer deadline_;
    QUrl url_;
    QByteArray pin_;
    BundledAdmin::Completion completion_;
    QNetworkReply *reply_ = nullptr;
    QByteArray response_;
    bool pinChecked_ = false;
    bool finished_ = false;
};

} // namespace

std::unique_ptr<BundledAdmin> BundledAdmin::open(const QString &profileDirectory,
    const QByteArray &launchPassword, QString &error)
{
    return openAtPort(profileDirectory, launchPassword, bundledAdminPort, error);
}

std::unique_ptr<BundledAdmin> BundledAdmin::openAtPort(const QString &profileDirectory,
    const QByteArray &launchPassword, quint16 port, QString &error)
{
    error.clear();
    if (!isLaunchPassword(launchPassword)) {
        error = QStringLiteral("内置 Sunshine 的本次启动凭据无效。");
        return {};
    }
    if (!QSslSocket::supportsSsl()) {
        error = QStringLiteral("当前 Qt 运行库不支持 HTTPS。");
        return {};
    }
    auto pin = readPin(profileDirectory);
    if (pin.size() != 32) {
        error = QStringLiteral("Aurora 无法读取内置 Sunshine 的公开证书。");
        return {};
    }

    auto credentials = QByteArray("aurora-local:") + launchPassword;
    auto authorization = QByteArray("Basic ") + credentials.toBase64();
    credentials.fill('\0');
    return std::unique_ptr<BundledAdmin>(new BundledAdmin(port, std::move(pin), std::move(authorization)));
}

BundledAdmin::BundledAdmin(quint16 port, QByteArray pin, QByteArray authorization)
    : port_(port), pin_(std::move(pin)), authorization_(std::move(authorization))
{
}

BundledAdmin::~BundledAdmin()
{
    authorization_.fill('\0');
}

void BundledAdmin::sendConfig(std::optional<QByteArray> body, Completion completion)
{
    new ConfigExchange(this, port_, pin_, authorization_, body, std::move(completion));
}

void BundledAdmin::finishOperation(Completion &completion, Result result)
{
    busy_ = false;
    completion(std::move(result));
}

bool BundledAdmin::readConfig(Completion completion)
{
    if (QThread::currentThread() != thread() || busy_ || !completion)
        return false;

    busy_ = true;
    sendConfig(std::nullopt, [this, completion = std::move(completion)](Result result) mutable {
        finishOperation(completion, std::move(result));
    });
    return true;
}

bool BundledAdmin::saveConfig(PreparePayload prepare, VerifyReadback verify, Completion completion)
{
    if (QThread::currentThread() != thread() || busy_ || !prepare || !verify || !completion)
        return false;

    busy_ = true;
    sendConfig(std::nullopt,
        [this, prepare = std::move(prepare), verify = std::move(verify),
         completion = std::move(completion)](Result current) mutable {
        if (!current)
            return finishOperation(completion, std::move(current));

        QString error;
        const QPointer<BundledAdmin> alive(this);
        const auto payload = prepare(current.config, error);
        if (!alive)
            return;
        if (!payload)
            return finishOperation(completion, failure(Error::InvalidPayload, error.isEmpty()
                ? QStringLiteral("无法生成 Sunshine 配置。") : std::move(error)));

        auto body = QJsonDocument(*payload).toJson(QJsonDocument::Compact);
        if (body.size() > bodyLimit)
            return finishOperation(completion, failure(Error::InvalidPayload, QStringLiteral("待提交的 Sunshine 配置过大。")));

        sendConfig(std::move(body), [this, verify = std::move(verify),
                                     completion = std::move(completion)](Result posted) mutable {
            if (!posted)
                return finishOperation(completion, std::move(posted));

            sendConfig(std::nullopt, [this, verify = std::move(verify),
                                      completion = std::move(completion)](Result actual) mutable {
                if (actual) {
                    const QPointer<BundledAdmin> alive(this);
                    auto error = verify(actual.config);
                    if (!alive)
                        return;
                    if (!error.isEmpty())
                        actual = failure(Error::ReadbackMismatch, std::move(error), actual.httpStatus);
                }
                finishOperation(completion, std::move(actual));
            });
        });
    });
    return true;
}

} // namespace Aurora::Sunshine
