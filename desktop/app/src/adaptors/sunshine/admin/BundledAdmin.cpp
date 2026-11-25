// SPDX-License-Identifier: GPL-3.0-only
#include "BundledAdmin.h"
#include "../../../../../protocol/identity/ClientProof.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
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

bool isPairingId(const QString &id)
{
    static const QRegularExpression hex(QStringLiteral(R"(\A[0-9a-fA-F]{32}\z)"));
    return hex.match(id).hasMatch();
}

QString validateApproval(const QString &id, const QString &pin, const QString &name)
{
    if (!isPairingId(id))
        return QStringLiteral("请选择一条待确认的配对请求。");
    if (pin.size() != 4 || !std::all_of(pin.cbegin(), pin.cend(), [](QChar digit) {
            return digit >= QLatin1Char('0') && digit <= QLatin1Char('9');
        }))
        return QStringLiteral("请输入主控端显示的 4 位数字 PIN。");
    if (name.trimmed().isEmpty() || name.toUtf8().size() > 128 ||
        std::any_of(name.cbegin(), name.cend(), [](QChar ch) {
            return ch == QLatin1Char('#') || ch.category() == QChar::Other_Control;
        }))
        return QStringLiteral("名称最多 128 字节，不能含 # 或控制字符。");
    return {};
}

std::optional<quint32> dottedIpv4(const QString &text)
{
    const auto octets = text.split(QLatin1Char('.'));
    if (octets.size() != 4)
        return std::nullopt;

    quint32 address = 0;
    for (const auto &octet : octets) {
        if (octet.isEmpty() || octet.size() > 3 || (octet.size() > 1 && octet.front() == QLatin1Char('0')))
            return std::nullopt;
        quint32 value = 0;
        for (const auto digit : octet) {
            if (digit < QLatin1Char('0') || digit > QLatin1Char('9'))
                return std::nullopt;
            value = 10 * value + quint32(digit.unicode() - '0');
        }
        if (value > 255)
            return std::nullopt;
        address = (address << 8) | value;
    }
    return address;
}

std::optional<QHostAddress> pairingIpv4(const QString &text)
{
    if (!text.contains(QLatin1Char(':'))) {
        const auto address = dottedIpv4(text);
        return address ? std::optional<QHostAddress>(QHostAddress(*address)) : std::nullopt;
    }

    // Legacy pairing correlation also understood IPv4-mapped IPv6.
    static const QRegularExpression ipv6Text(QStringLiteral(R"(\A[0-9a-fA-F:.]{1,45}\z)"));
    if (!ipv6Text.match(text).hasMatch() ||
        (text.contains(QLatin1Char('.')) && !dottedIpv4(text.mid(text.lastIndexOf(QLatin1Char(':')) + 1))))
        return std::nullopt;

    QHostAddress mapped;
    if (!mapped.setAddress(text) || mapped.protocol() != QAbstractSocket::IPv6Protocol)
        return std::nullopt;
    const auto address = mapped.toIPv6Address();
    if (!std::all_of(address.c, address.c + 10, [](quint8 byte) { return byte == 0; }) ||
        address[10] != 0xff || address[11] != 0xff)
        return std::nullopt;
    return QHostAddress((quint32(address[12]) << 24) | (quint32(address[13]) << 16) |
                        (quint32(address[14]) << 8) | quint32(address[15]));
}

QJsonObject pendingObject(const QByteArray &body, QJsonObject object)
{
    // QJsonValue does not retain the numeric spelling. Like TryGetInt32, accept
    // source ports written as JSON integers only. Keep quoted strings intact.
    static const QRegularExpression tokens(QStringLiteral(
        R"json("(?:[^"\\]++|\\.)*+"|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)json"));
    const auto source = QString::fromUtf8(body);
    auto normalized = source;
    auto matches = tokens.globalMatch(source);
    bool changed = false;
    while (matches.hasNext()) {
        const auto token = matches.next();
        const auto value = token.captured();
        if (value.front() == QLatin1Char('"') ||
            (!value.contains(QLatin1Char('.')) && !value.contains(QLatin1Char('e'), Qt::CaseInsensitive)))
            continue;
        const auto start = token.capturedStart();
        normalized[start] = QLatin1Char('0');
        for (auto i = start + 1; i < token.capturedEnd(); ++i)
            normalized[i] = QLatin1Char(' ');
        changed = true;
    }
    return changed ? QJsonDocument::fromJson(normalized.toUtf8()).object() : std::move(object);
}

std::optional<PendingPairing> parsePendingPairing(const QJsonValue &entry)
{
    if (!entry.isObject())
        return std::nullopt;
    const auto object = entry.toObject();
    const auto id = object.value(QStringLiteral("id")).toString();
    if (!isPairingId(id))
        return std::nullopt;

    const auto address = object.value(QStringLiteral("address")).toString();
    const auto port = object.value(QStringLiteral("sourcePort")).toDouble();
    std::optional<quint16> sourcePort;
    if (port >= 1 && port <= 65535 && port == quint16(port))
        sourcePort = quint16(port);
    return PendingPairing{id, object.value(QStringLiteral("name")).toString(), address, pairingIpv4(address), sourcePort};
}

QString pairingCancelledMessage()
{
    return QStringLiteral("Sunshine 配对授权已取消。");
}

} // namespace

class BundledAdmin::Exchange final : public QObject
{
public:
    Exchange(QObject *parent, quint16 port, const QByteArray &pin, const QByteArray &authorization,
             Endpoint endpoint, const std::optional<QByteArray> &body, Completion completion,
             ApprovalStillCurrent stillCurrent)
        : QObject(parent), endpoint_(endpoint), pin_(pin), completion_(std::move(completion)),
          stillCurrent_(std::move(stillCurrent))
    {
        url_.setScheme(QStringLiteral("https"));
        url_.setHost(QStringLiteral("127.0.0.1"));
        url_.setPort(port);
        switch (endpoint) {
        case Endpoint::Config: url_.setPath(QStringLiteral("/api/config")); break;
        case Endpoint::PendingPairings:
        case Endpoint::PairApproval: url_.setPath(QStringLiteral("/api/pin")); break;
        case Endpoint::ClientGrant: url_.setPath(QStringLiteral("/api/aurora/client-grant")); break;
        case Endpoint::ExpireClientGrants: url_.setPath(QStringLiteral("/api/aurora/client-grant/expire")); break;
        }

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
            else if (stillCurrent_ && !stillCurrent_())
                finish(failure(Error::Cancelled, pairingCancelledMessage()));
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

    ~Exchange() override
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
            finish(failure(Error::ResponseTooLarge, QStringLiteral("Sunshine 返回的响应过大。"), statusCode()));
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
            finish(failure(Error::ResponseTooLarge, QStringLiteral("Sunshine 返回的响应过大。"), statusCode()));
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
            return finish(failure(Error::InvalidResponse, QStringLiteral("Sunshine 返回的响应不是有效的 JSON 对象。"), status));
        auto config = document.object();
        if (endpoint_ == Endpoint::PendingPairings)
            config = pendingObject(response_, std::move(config));
        else if (endpoint_ == Endpoint::ClientGrant &&
                 config.value(QStringLiteral("status")) == QJsonValue(true) &&
                 config.value(QStringLiteral("result")) != QJsonValue(QStringLiteral("accepted")))
            return finish(failure(Error::InvalidResponse, QStringLiteral("Sunshine 返回了未知的客户端授权结果。"), status));
        else if (config.value(QStringLiteral("status")) != QJsonValue(true))
            return finish(failure(Error::Rejected, QStringLiteral("Sunshine 未接受这项操作。"), status));

        finish({Error::None, status, {}, std::move(config)});
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
    const Endpoint endpoint_;
    QByteArray pin_;
    Completion completion_;
    ApprovalStillCurrent stillCurrent_;
    QNetworkReply *reply_ = nullptr;
    QByteArray response_;
    bool pinChecked_ = false;
    bool finished_ = false;
};

SupervisorPairingApproval SupervisorPairingApproval::authorized(QString pairingId, QString pin, QString name)
{
    return SupervisorPairingApproval(std::move(pairingId), std::move(pin), std::move(name));
}

SupervisorPairingApproval SupervisorPairingApproval::clientGrant(QString name, QByteArray challenge,
    QByteArray certificate, QByteArray signature)
{
    SupervisorPairingApproval approval({}, {}, std::move(name));
    approval.grant_ = ClientGrant{std::move(challenge), std::move(certificate), std::move(signature)};
    return approval;
}

SupervisorPairingApproval SupervisorPairingApproval::expireClientGrants()
{
    SupervisorPairingApproval approval({}, {}, {});
    approval.expireGrants_ = true;
    return approval;
}

SupervisorPairingApproval::SupervisorPairingApproval(QString pairingId, QString pin, QString name)
    : id_(std::move(pairingId)), pin_(std::move(pin)), name_(std::move(name))
{
}

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

void BundledAdmin::send(Endpoint endpoint, std::optional<QByteArray> body, Completion completion,
                        ApprovalStillCurrent stillCurrent)
{
    new Exchange(this, port_, pin_, authorization_, endpoint, body, std::move(completion), std::move(stillCurrent));
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
    send(Endpoint::Config, std::nullopt, [this, completion = std::move(completion)](Result result) mutable {
        finishOperation(completion, std::move(result));
    });
    return true;
}

bool BundledAdmin::saveConfig(PreparePayload prepare, VerifyReadback verify, Completion completion)
{
    if (QThread::currentThread() != thread() || busy_ || !prepare || !verify || !completion)
        return false;

    busy_ = true;
    send(Endpoint::Config, std::nullopt,
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

        send(Endpoint::Config, std::move(body), [this, verify = std::move(verify),
                                     completion = std::move(completion)](Result posted) mutable {
            if (!posted)
                return finishOperation(completion, std::move(posted));

            send(Endpoint::Config, std::nullopt, [this, verify = std::move(verify),
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

bool BundledAdmin::readPendingPairings(PendingCompletion done)
{
    if (QThread::currentThread() != thread() || busy_ || !done)
        return false;

    busy_ = true;
    send(Endpoint::PendingPairings, std::nullopt, [this, done = std::move(done)](Result reply) {
        PendingResult result{reply.error, reply.httpStatus, std::move(reply.message), {}};
        if (reply) {
            result.supportsClientGrant = reply.config.value(QStringLiteral("aurora_client_grant")) == QJsonValue(true);
            const auto entries = reply.config.value(QStringLiteral("pairings"));
            if (!entries.isArray()) {
                result.error = Error::InvalidResponse;
                result.message = QStringLiteral("当前 Sunshine 不支持在 Aurora 内选择待确认的配对。");
            } else {
                for (const auto &entry : entries.toArray()) {
                    if (auto pending = parsePendingPairing(entry))
                        result.pairings.append(std::move(*pending));
                }
            }
        }

        busy_ = false;
        done(std::move(result));
    });
    return true;
}

bool BundledAdmin::approvePairing(SupervisorPairingApproval approval, ApprovalCompletion done,
                                  ApprovalStillCurrent stillCurrent)
{
    if (QThread::currentThread() != thread() || busy_ || !done)
        return false;

    auto error = validateApproval(approval.id_, approval.pin_, approval.name_);
    QJsonObject payload{{QStringLiteral("name"), approval.name_}};
    const auto *grant = approval.grant_ ? &*approval.grant_ : nullptr;
    auto endpoint = Endpoint::PairApproval;
    if (approval.expireGrants_) {
        endpoint = Endpoint::ExpireClientGrants;
        payload = {};
        error.clear();
    } else if (grant) {
        endpoint = Endpoint::ClientGrant;
        const auto name = approval.name_.toUtf8();
        if (!name.isEmpty() && name.size() <= 128 && !name.contains('\0') &&
            grant->challenge.size() == ClientProof::ChallengeSize &&
            !grant->certificate.isEmpty() && grant->certificate.size() <= ClientProof::MaxCertificateBytes &&
            !grant->signature.isEmpty() && grant->signature.size() <= ClientProof::MaxSignatureBytes) {
            error.clear();
            payload.insert(QStringLiteral("challenge"), QString::fromLatin1(grant->challenge.toHex()));
            payload.insert(QStringLiteral("certificate"), QString::fromLatin1(grant->certificate.toHex()));
            payload.insert(QStringLiteral("signature"), QString::fromLatin1(grant->signature.toHex()));
        } else {
            error = QStringLiteral("无效的客户端授权证明。");
        }
    } else {
        payload.insert(QStringLiteral("pairing_id"), approval.id_);
        payload.insert(QStringLiteral("pin"), approval.pin_);
    }
    busy_ = true;
    if (!error.isEmpty() || (stillCurrent && !stillCurrent())) {
        const auto code = error.isEmpty() ? Error::Cancelled : Error::InvalidPayload;
        if (error.isEmpty())
            error = pairingCancelledMessage();
        QTimer::singleShot(0, this, [this, done = std::move(done), code, error = std::move(error)] {
            busy_ = false;
            done({code, 0, error});
        });
        return true;
    }

    send(endpoint, QJsonDocument(payload).toJson(QJsonDocument::Compact),
        [this, done = std::move(done)](Result reply) {
            busy_ = false;
            done({reply.error, reply.httpStatus, std::move(reply.message)});
        }, std::move(stillCurrent));
    return true;
}

} // namespace Aurora::Sunshine
