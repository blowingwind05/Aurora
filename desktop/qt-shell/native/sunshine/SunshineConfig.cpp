// SPDX-License-Identifier: GPL-3.0-only
#include "SunshineConfig.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QResource>
#include <QSet>

#include <algorithm>
#include <cmath>

static void initSunshineOptionsResource()
{
    Q_INIT_RESOURCE(aurora_sunshine_options);
}

namespace Aurora::Sunshine {
namespace {

const QString nameKey = QStringLiteral("sunshine_name");
const QString audioKey = QStringLiteral("stream_audio");
const QString sourceKey = QStringLiteral("displaySource");
const QString exclusiveKey = QStringLiteral("exclusiveDisplay");
const QString legacyKey = QStringLiteral("legacyScreensaver");
const QString enabled = QStringLiteral("enabled");
const QString disabled = QStringLiteral("disabled");

const QString privacyDisplayKey = QStringLiteral("aurora_privacy_display_id");
const QString virtualDisplayKey = QStringLiteral("aurora_virtual_display_id");
const QString screensaverKey = QStringLiteral("aurora_privacy_screensaver");

struct Option
{
    QString key;
    QString label;
    QString defaultValue;
    QString kind;
    QString group;
    QJsonArray choices;

    QString category() const
    {
        if (group == QStringLiteral("输入") || group == QStringLiteral("网络"))
            return group;
        return QStringLiteral("显示与编码");
    }
};

const QList<Option> &catalog()
{
    static const auto options = [] {
        initSunshineOptionsResource();
        QFile file(QStringLiteral(":/aurora/sunshine/SunshineOptions.json"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot open the embedded Sunshine options catalog");
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isArray() || document.array().isEmpty())
            qFatal("Invalid embedded Sunshine options catalog");

        QList<Option> result;
        QSet<QString> keys;
        for (const auto &entry : document.array()) {
            const auto item = entry.toObject();
            const auto key = item.value(QStringLiteral("key")).toString();
            if (key.isEmpty() || keys.contains(key))
                qFatal("Empty or duplicate key in the embedded Sunshine options catalog");
            keys.insert(key);
            result.append(Option{
                key, item.value(QStringLiteral("label")).toString(), item.value(QStringLiteral("default")).toString(),
                item.value(QStringLiteral("kind")).toString(), item.value(QStringLiteral("group")).toString(),
                item.value(QStringLiteral("choices")).toArray()
            });
        }
        return result;
    }();
    return options;
}

const QString &switchValue(bool checked)
{
    return checked ? enabled : disabled;
}

bool sameDisplay(const DisplaySelection &a, const DisplaySelection &b)
{
    return a.id == b.id && a.exclusive == b.exclusive && a.screensaver == b.screensaver;
}

bool containsDisplay(const QList<Display> &displays, const QString &id)
{
    return std::any_of(displays.cbegin(), displays.cend(), [&](const Display &display) { return display.id == id; });
}

bool trimSpace(QChar ch)
{
    // The whitespace set used by System.String.Trim, including NEL.
    const auto code = ch.unicode();
    return (code >= 0x09 && code <= 0x0d) || code == 0x85 || ch.category() == QChar::Separator_Space ||
        ch.category() == QChar::Separator_Line || ch.category() == QChar::Separator_Paragraph;
}

QString trimmed(const QString &text)
{
    auto start = text.cbegin();
    auto end = text.cend();
    while (start != end && trimSpace(*start))
        ++start;
    while (end != start && trimSpace(*(end - 1)))
        --end;
    return text.mid(start - text.cbegin(), end - start);
}

bool hasForbiddenCharacter(const QString &text)
{
    return std::any_of(text.cbegin(), text.cend(), [](QChar ch) {
        return ch == QLatin1Char('#') || ch.category() == QChar::Other_Control;
    });
}

bool validName(const QString &text)
{
    return !hasForbiddenCharacter(text) && text.toUtf8().size() <= 128;
}

QString nameError()
{
    return QStringLiteral("名称最多 128 字节，不能含 # 或控制字符。");
}

bool isDisplayId(const QString &text)
{
    const auto id = trimmed(text);
    if (id.size() != 38 || id.front() != QLatin1Char('{') || id.back() != QLatin1Char('}') ||
        id[9] != QLatin1Char('-') || id[14] != QLatin1Char('-') ||
        id[19] != QLatin1Char('-') || id[24] != QLatin1Char('-'))
        return false;

    // Guid.TryParseExact("B") also accepts these prefixes inside fixed-width segments.
    static const QRegularExpression hex(QStringLiteral(R"(\A\+?(?:0[xX])?[0-9a-fA-F]+\z)"));
    return hex.match(id.mid(1, 8)).hasMatch() && hex.match(id.mid(10, 4)).hasMatch() &&
        hex.match(id.mid(15, 4)).hasMatch() && hex.match(id.mid(20, 4)).hasMatch() &&
        hex.match(id.mid(25, 12)).hasMatch();
}

bool validConfigJson(const QString &text, bool expectArray)
{
    // Validate JSON tokens while counting the legacy 64-level limit. Qt can then check
    // structure using in-range stand-ins for numeric literals of any magnitude.
    static const QRegularExpression tokens(QStringLiteral(
        R"json("(?:[^"\\]++|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*+"|[{}\[\]]|[^{}\[\],:" ]++|")json"));
    static const QRegularExpression number(QStringLiteral(
        R"(\A-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\z)"));
    auto normalized = text;
    auto matches = tokens.globalMatch(text);
    int depth = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto token = match.captured();
        const auto first = token.front();
        if (first == QLatin1Char('{') || first == QLatin1Char('[')) {
            if (++depth > 64)
                return false;
        } else if (first == QLatin1Char('}') || first == QLatin1Char(']')) {
            --depth;
        } else if (first == QLatin1Char('"')) {
            if (token.size() == 1)
                return false;
        } else if (token != QStringLiteral("true") && token != QStringLiteral("false") &&
                   token != QStringLiteral("null")) {
            if (!number.match(token).hasMatch())
                return false;
            normalized.replace(match.capturedStart(), token.size(), QStringLiteral("0").leftJustified(token.size()));
        }
    }
    const auto document = QJsonDocument::fromJson(normalized.toUtf8());
    return expectArray ? document.isArray() : document.isObject();
}

QString validateOption(const Option &option, const QString &raw)
{
    if (hasForbiddenCharacter(raw))
        return option.label + QStringLiteral("不能包含换行、控制字符或 #。");
    if (raw.isEmpty())
        return {};
    if (option.kind == QStringLiteral("number")) {
        static const QRegularExpression number(QStringLiteral(
            R"(\A *[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)? *\z)"));
        // The grammar excludes conversion errors other than floating-point overflow or underflow.
        if (!number.match(raw).hasMatch() || !std::isfinite(raw.toDouble()))
            return option.label + QStringLiteral("请输入数字，小数用点。");
    }
    if (option.key == QStringLiteral("port")) {
        bool ok = false;
        const auto port = raw.toInt(&ok);
        if (!ok || port < 1029 || port > 65514)
            return QStringLiteral("端口须为 1029–65514。");
    }
    if (option.kind == QStringLiteral("json") && option.key != QStringLiteral("keybindings") &&
        !validConfigJson(raw, option.key == QStringLiteral("global_prep_cmd")))
        return option.label + QStringLiteral("不是有效的 JSON。");
    return {};
}

QString configText(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isDouble()) {
        const auto wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
    }
    return {};
}

QJsonObject merge(const QJsonObject &current, const Values &changes)
{
    auto payload = current;
    payload.remove(QStringLiteral("status"));
    payload.remove(QStringLiteral("version"));
    payload.remove(QStringLiteral("platform"));
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        if (it.value().isEmpty())
            payload.remove(it.key());
        else
            payload.insert(it.key(), it.value());
    }
    return payload;
}

} // namespace

const Values &managedOptions()
{
    // BundledSunshine.ManagedOptions at e05bd75.
    static const Values settings{
        {QStringLiteral("port"), QStringLiteral("48989")},
        {QStringLiteral("origin_web_ui_allowed"), QStringLiteral("pc")},
        {QStringLiteral("upnp"), disabled},
        {QStringLiteral("system_tray"), disabled},
        {QStringLiteral("cert"), QStringLiteral("credentials/cacert.pem")},
        {QStringLiteral("pkey"), QStringLiteral("credentials/cakey.pem")},
        {QStringLiteral("file_state"), QStringLiteral("sunshine_state.json")},
        {QStringLiteral("credentials_file"), QStringLiteral("account.json")},
        {QStringLiteral("file_apps"), QStringLiteral("apps.json")},
        {QStringLiteral("log_path"), QStringLiteral("sunshine.log")},
    };
    return settings;
}

Preferences parsePreferences(const QJsonObject &current)
{
    const auto audio = current.value(audioKey);
    const auto value = configText(audio).toLower();
    const auto on = audio.isUndefined() || value.contains(QLatin1Char('1')) || value == QStringLiteral("on") ||
        value == QStringLiteral("yes") || value == QStringLiteral("true") || value == QStringLiteral("enable") ||
        value == enabled;
    return {current.value(nameKey).toString(), on};
}

PrivacySettings parsePrivacySettings(const QJsonObject &current)
{
    PrivacySettings result;
    const auto displays = current.value(QStringLiteral("displays"));
    const auto virtualDisplay = current.value(QStringLiteral("supports_virtual_display"));
    result.supportsVirtualDisplay = virtualDisplay.isUndefined() ? displays.isArray() : virtualDisplay.toBool();
    result.supportsExtended = result.supportsVirtualDisplay && current.contains(QStringLiteral("saved_exclusive"));
    result.supportsScreensaver = current.value(QStringLiteral("supports_screensaver")).toBool();
    if (result.supportsVirtualDisplay) {
        for (const auto &entry : displays.toArray()) {
            const auto item = entry.toObject();
            const auto id = item.value(QStringLiteral("id")).toString();
            if (isDisplayId(id))
                result.displays.append(Display{id, item.value(QStringLiteral("name")).toString()});
        }
    }
    const auto selection = [&](const QString &id, const QString &exclusive, const QString &screensaver) {
        const auto text = current.value(id).toString();
        return DisplaySelection{
            text, !text.isEmpty() && (!result.supportsExtended || current.value(exclusive).toBool()),
            current.value(screensaver).toBool()
        };
    };
    result.saved = selection(QStringLiteral("saved"), QStringLiteral("saved_exclusive"), QStringLiteral("saved_screensaver"));
    result.active = selection(QStringLiteral("active"), QStringLiteral("active_exclusive"), QStringLiteral("active_screensaver"));
    return result;
}

QJsonObject configForQml(const QJsonObject &current, bool managed, const std::optional<PrivacySettings> &privacy)
{
    QJsonArray options;
    QJsonObject values;
    for (const auto &option : catalog()) {
        const auto fixed = managedOptions().constFind(option.key);
        const bool isManaged = managed && fixed != managedOptions().cend();
        options.append(QJsonObject{
            {QStringLiteral("key"), option.key}, {QStringLiteral("label"), option.label},
            {QStringLiteral("default"), option.defaultValue}, {QStringLiteral("kind"), option.kind},
            {QStringLiteral("group"), option.group}, {QStringLiteral("category"), option.category()},
            {QStringLiteral("choices"), option.choices}, {QStringLiteral("managed"), isManaged}
        });
        const auto value = current.value(option.key);
        values.insert(option.key, isManaged ? fixed.value() : value.isUndefined() ? option.defaultValue : configText(value));
    }

    const auto preferences = parsePreferences(current);
    values.insert(nameKey, preferences.name);
    values.insert(audioKey, switchValue(preferences.streamAudio));

    QJsonValue privacyView;
    if (managed && privacy) {
        const auto &saved = privacy->saved;
        const auto &active = privacy->active;
        values.insert(sourceKey, saved.id);
        values.insert(exclusiveKey, switchValue(saved.exclusive));
        values.insert(legacyKey, switchValue(saved.screensaver));
        const bool showSource = privacy->supportsVirtualDisplay &&
            (!saved.id.isEmpty() || !active.id.isEmpty() || !privacy->supportsScreensaver);
        QJsonArray displays{QJsonObject{{QStringLiteral("id"), QStringLiteral("")},
                                       {QStringLiteral("name"), QStringLiteral("按原有显示设置")}}};
        for (const auto &display : privacy->displays)
            displays.append(QJsonObject{{QStringLiteral("id"), display.id}, {QStringLiteral("name"), display.name}});
        if (!saved.id.isEmpty() && !containsDisplay(privacy->displays, saved.id))
            displays.append(QJsonObject{{QStringLiteral("id"), saved.id},
                                       {QStringLiteral("name"), QStringLiteral("已选虚拟显示器不可用")}});
        privacyView = QJsonObject{
            {QStringLiteral("showSource"), showSource},
            {QStringLiteral("showLocalDisplays"), showSource && privacy->supportsExtended},
            {QStringLiteral("showLegacyScreensaver"), saved.screensaver || active.screensaver},
            {QStringLiteral("displays"), displays},
            {QStringLiteral("restartNeeded"), !sameDisplay(saved, active)}
        };
    }
    return {{QStringLiteral("options"), options}, {QStringLiteral("values"), values}, {QStringLiteral("privacy"), privacyView}};
}

ValidatedChanges validateChanges(const QJsonObject &changes, bool managed)
{
    ValidatedChanges result;
    result.managed_ = managed;
    const auto &options = catalog();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        const auto key = it.key();
        const auto input = it.value();
        const auto raw = input.toString();
        if (!input.isString() || raw.size() > 65536) {
            result.errors_.insert(key, QStringLiteral("设置值缺失或过长。"));
            continue;
        }
        const auto value = trimmed(raw);
        QString error;
        auto *accepted = &result.preferences_;
        if (key == nameKey) {
            if (!validName(raw))
                error = nameError();
        } else if (key == audioKey || key == exclusiveKey || key == legacyKey) {
            if (value != enabled && value != disabled)
                error = QStringLiteral("无效的开关值。");
        } else if (key == sourceKey) {
            if (!value.isEmpty() && !isDisplayId(value))
                error = QStringLiteral("无效的虚拟显示器。");
        } else {
            const auto option = std::find_if(options.cbegin(), options.cend(),
                [&](const Option &entry) { return entry.key == key; });
            if (option == options.cend())
                error = QStringLiteral("不支持的 Sunshine 设置。");
            else if (managed && managedOptions().contains(key))
                error = QStringLiteral("此配置项由 Aurora 统一管理。");
            else
                error = validateOption(*option, raw);
            accepted = &result.options_;
        }
        if (error.isEmpty())
            accepted->insert(key, value);
        else
            result.errors_.insert(key, error);
    }
    return result;
}

QJsonObject mergeOptions(const QJsonObject &current, const ValidatedChanges &changes)
{
    return merge(current, changes.options());
}

std::optional<PreferenceUpdate> resolvePreferences(const ValidatedChanges &changes, const QJsonObject &current,
    const std::optional<PrivacySettings> &privacy, QString &error)
{
    error.clear();
    const auto &values = changes.preferences();
    if (values.isEmpty())
        return std::nullopt;

    const auto previous = parsePreferences(current);
    PreferenceUpdate update;
    update.preferences_ = {values.value(nameKey, previous.name),
                           values.value(audioKey, switchValue(previous.streamAudio)) == enabled};
    if (values.contains(sourceKey) || values.contains(exclusiveKey) || values.contains(legacyKey)) {
        if (!changes.managed_ || !privacy) {
            error = QStringLiteral("当前被控组件不支持远控显示设置。");
            return std::nullopt;
        }
        if ((values.contains(sourceKey) && !privacy->supportsVirtualDisplay) ||
            (values.contains(exclusiveKey) && !privacy->supportsExtended)) {
            error = QStringLiteral("当前被控组件不支持所选显示设置。");
            return std::nullopt;
        }

        const auto id = values.value(sourceKey, privacy->saved.id);
        const bool legacy = values.value(legacyKey, switchValue(privacy->saved.screensaver)) == enabled;
        if (legacy && !privacy->saved.screensaver) {
            error = QStringLiteral("旧版自动隐私屏保关闭后不能再次启用。");
            return std::nullopt;
        }
        const bool exclusive = !id.isEmpty() && !legacy && (!privacy->supportsExtended ||
            values.value(exclusiveKey, switchValue(privacy->saved.exclusive)) == enabled);
        update.display_ = DisplaySelection{id, exclusive, legacy};
    }
    if (!validName(update.preferences_.name)) {
        error = nameError();
        return std::nullopt;
    }
    return update;
}

std::optional<QJsonObject> mergePreferences(const QJsonObject &fresh, const PreferenceUpdate &update,
    const std::optional<PrivacySettings> &freshPrivacy, QString &error)
{
    error.clear();
    auto payload = merge(fresh, {{nameKey, update.preferences().name}, {audioKey, switchValue(update.preferences().streamAudio)}});
    if (!update.display())
        return payload;
    if (!freshPrivacy) {
        error = QStringLiteral("请先更新内置被控组件。");
        return std::nullopt;
    }

    const auto &display = *update.display();
    const auto &privacy = *freshPrivacy;
    if (display.id != privacy.saved.id && !display.id.isEmpty() && !containsDisplay(privacy.displays, display.id)) {
        error = QStringLiteral("所选虚拟显示器已不可用，请重新打开设置。");
        return std::nullopt;
    }
    if (!display.id.isEmpty() && !display.exclusive && !privacy.supportsExtended) {
        error = QStringLiteral("请更新被控组件以保留本地显示器。");
        return std::nullopt;
    }
    if (display.screensaver && (display.exclusive || (!privacy.supportsScreensaver && !privacy.saved.screensaver))) {
        error = QStringLiteral("当前被控组件或显示设置无法启用隐私屏保。");
        return std::nullopt;
    }

    if (privacy.supportsVirtualDisplay) {
        payload.remove(privacyDisplayKey);
        payload.remove(virtualDisplayKey);
        if (!display.id.isEmpty())
            payload.insert(display.exclusive ? privacyDisplayKey : virtualDisplayKey, display.id);
    }
    if (privacy.supportsScreensaver || privacy.saved.screensaver) {
        payload.remove(screensaverKey);
        if (display.screensaver)
            payload.insert(screensaverKey, enabled);
    }
    return payload;
}

QString verifyOptions(const QJsonObject &actual, const ValidatedChanges &changes)
{
    const auto &options = changes.options();
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        const auto value = actual.value(it.key());
        if (it.value().isEmpty() ? !value.isUndefined()
                                : value.isUndefined() || trimmed(configText(value)) != it.value())
            return QStringLiteral("回读结果与提交值不同，请检查当前 Sunshine 设置。");
    }
    return {};
}

QString verifyPreferences(const QJsonObject &actual, const PreferenceUpdate &update,
                          const std::optional<PrivacySettings> &privacy)
{
    const auto saved = parsePreferences(actual);
    if (saved.name != update.preferences().name || saved.streamAudio != update.preferences().streamAudio)
        return QStringLiteral("Sunshine 没有返回刚保存的值，请重新读取设置。");
    if (update.display() && (!privacy || !sameDisplay(privacy->saved, *update.display())))
        return QStringLiteral("无法确认已保存的显示设置，请重新读取设置。");
    return {};
}

} // namespace Aurora::Sunshine
