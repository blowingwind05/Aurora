// SPDX-License-Identifier: GPL-3.0-only
#include "SunshineConfig.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

#include <algorithm>
#include <cstdlib>

using namespace Aurora::Sunshine;

namespace {

const auto managedKey = QStringLiteral("managed");

[[noreturn]] void fail(const QString &message)
{
    QTextStream(stderr) << message << '\n';
    std::exit(1);
}

QByteArray json(const QJsonValue &value)
{
    if (value.isUndefined())
        return "<missing>";
    const auto wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

void expect(const QJsonValue &actual, const QJsonValue &expected, const QString &name)
{
    if (actual != expected)
        fail(name + QStringLiteral("\nExpected: ") + QString::fromUtf8(json(expected)) +
            QStringLiteral("\nActual:   ") + QString::fromUtf8(json(actual)));
}

QJsonDocument read(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(file.errorString() + QStringLiteral(" ") + path);
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (document.isNull())
        fail(QStringLiteral("Invalid test input: ") + path);
    return document;
}

QJsonObject object(const Values &values)
{
    QJsonObject result;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

void compareCatalog(const QJsonArray &source, const QJsonObject &fixture)
{
    const auto managed = fixture.value(QStringLiteral("managedOptions")).toObject();
    expect(object(managedOptions()), managed, QStringLiteral("Managed settings"));
    const auto bundled = configForQml({}, true);
    const auto installed = configForQml({}, false);
    const auto bundledOptions = bundled.value(QStringLiteral("options")).toArray();
    const auto bundledValues = bundled.value(QStringLiteral("values")).toObject();
    const auto installedValues = installed.value(QStringLiteral("values")).toObject();
    expect(int(source.size()), 96, QStringLiteral("Baseline catalog size"));
    expect(int(bundledOptions.size()), int(source.size()), QStringLiteral("Embedded catalog size"));

    QJsonArray managedKeys;
    QJsonArray expectedBundled;
    QJsonArray expectedInstalled;
    for (const auto &value : source) {
        auto option = value.toObject();
        const auto key = option.value(QStringLiteral("key")).toString();
        const auto group = option.value(QStringLiteral("group")).toString();
        const bool isManaged = managed.contains(key);
        if (isManaged)
            managedKeys.append(key);
        option.insert(QStringLiteral("category"), group == QStringLiteral("输入") || group == QStringLiteral("网络")
            ? group : QStringLiteral("显示与编码"));
        option.insert(QStringLiteral("choices"), option.value(QStringLiteral("choices")).toArray());
        option.insert(managedKey, isManaged);
        expectedBundled.append(option);
        option.insert(managedKey, false);
        expectedInstalled.append(option);
        expect(bundledValues.value(key), isManaged ? managed.value(key) : option.value(QStringLiteral("default")),
               QStringLiteral("Bundled default ") + key);
        expect(installedValues.value(key), option.value(QStringLiteral("default")), QStringLiteral("Installed default ") + key);
    }
    for (int i = 0; i < source.size(); ++i)
        expect(bundledOptions.at(i), expectedBundled.at(i), QStringLiteral("Bundled option ") + QString::number(i));
    expect(installed.value(QStringLiteral("options")), expectedInstalled, QStringLiteral("Installed options in order"));
    expect(managedKeys, fixture.value(QStringLiteral("managedKeysInOrder")), QStringLiteral("Catalog managed key order"));
}

void compareValidation(const QJsonObject &test)
{
    const auto key = test.value(QStringLiteral("key")).toString();
    const auto count = test.value(QStringLiteral("repeat"));
    const auto raw = count.isDouble() ? QJsonValue(QString(count.toInt(), QLatin1Char('x')))
                                      : test.value(QStringLiteral("raw"));
    const auto changes = validateChanges({{key, raw}}, test.value(managedKey).toBool());
    const auto group = test.value(QStringLiteral("group")).toString();
    const auto name = QStringLiteral("Validation ") + key + QLatin1Char(' ') +
        (count.isDouble() ? QString::number(count.toInt()) : QString::fromUtf8(json(raw)));
    const auto error = test.value(QStringLiteral("error"));
    const auto normalized = count.isDouble() ? raw : test.value(QStringLiteral("normalized"));
    const auto expectedErrors = error.isString() ? QJsonObject{{key, error}} : QJsonObject{};
    const auto expectedValues = error.isString() ? QJsonObject{} : QJsonObject{{key, normalized}};
    expect(object(changes.errors()), expectedErrors, name);
    expect(object(changes.options()), group == QStringLiteral("options") ? expectedValues : QJsonObject{}, name);
    expect(object(changes.preferences()), group == QStringLiteral("preferences") ? expectedValues : QJsonObject{}, name);
}

std::optional<PrivacySettings> currentPrivacy(const QJsonObject &current, QJsonValue capabilities)
{
    if (!capabilities.isObject())
        return std::nullopt;

    auto data = capabilities.toObject();
    const auto exclusive = QStringLiteral("aurora_privacy_display_id");
    const auto virtualDisplay = QStringLiteral("aurora_virtual_display_id");
    const auto savedExclusive = QStringLiteral("saved_exclusive");
    data.insert(QStringLiteral("saved"), current.value(exclusive).toString(current.value(virtualDisplay).toString()));
    if (data.contains(savedExclusive))
        data.insert(savedExclusive, current.contains(exclusive));
    data.insert(QStringLiteral("saved_screensaver"), current.value(QStringLiteral("aurora_privacy_screensaver")) ==
        QJsonValue(QStringLiteral("enabled")));
    return parsePrivacySettings(data);
}

void compareFlow(const QJsonObject &test)
{
    const auto name = test.value(QStringLiteral("name")).toString();
    const auto managed = test.value(managedKey).toBool();
    const auto capabilities = test.value(QStringLiteral("privacy"));
    const auto ignoreWrites = test.value(QStringLiteral("ignoreWrites")).toBool();
    auto current = test.value(QStringLiteral("current")).toObject();
    const auto before = configForQml(current, managed, currentPrivacy(current, capabilities));
    const auto actualValues = before.value(QStringLiteral("values")).toObject();
    const auto expectedValues = test.value(QStringLiteral("expectedValues")).toObject();
    for (auto it = expectedValues.begin(); it != expectedValues.end(); ++it)
        expect(actualValues.value(it.key()), it.value(), name + QStringLiteral(" value ") + it.key());
    expect(before.value(QStringLiteral("privacy")), test.value(QStringLiteral("expectedPrivacy")), name + QStringLiteral(" privacy"));

    const auto changes = validateChanges(test.value(QStringLiteral("changes")).toObject(), managed);
    QJsonArray writes;
    Values errors = changes.errors();
    QStringList saved;
    const auto write = [&](const QJsonObject &payload) {
        writes.append(payload);
        if (!ignoreWrites)
            current = payload;
    };
    const auto report = [&](const Values &group, const QString &error) {
        for (auto it = group.cbegin(); it != group.cend(); ++it) {
            if (error.isEmpty())
                saved.append(it.key());
            else
                errors.insert(it.key(), error);
        }
    };

    if (!changes.preferences().isEmpty()) {
        QString error;
        const auto update = resolvePreferences(changes, current, currentPrivacy(current, capabilities), error);
        if (update) {
            const auto payload = mergePreferences(current, *update, currentPrivacy(current, capabilities), error);
            if (payload) {
                write(*payload);
                error = verifyPreferences(current, *update, currentPrivacy(current, capabilities));
            }
        }
        report(changes.preferences(), error);
    }
    if (!changes.options().isEmpty()) {
        write(mergeOptions(current, changes));
        report(changes.options(), verifyOptions(current, changes));
    }
    std::sort(saved.begin(), saved.end());
    expect(QJsonArray::fromStringList(saved), test.value(QStringLiteral("saved")), name + QStringLiteral(" saved"));
    expect(object(errors), test.value(QStringLiteral("errors")), name + QStringLiteral(" errors"));
    expect(writes, test.value(QStringLiteral("writes")), name + QStringLiteral(" payloads"));
}

} // namespace

int main(int argc, char *argv[])
{
    const QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3)
        fail(QStringLiteral("Expected the source catalog and legacy fixture paths."));
    const auto fixture = read(args.at(2)).object();
    compareCatalog(read(args.at(1)).array(), fixture);
    const auto validation = fixture.value(QStringLiteral("validation")).toArray();
    const auto flows = fixture.value(QStringLiteral("flows")).toArray();
    if (validation.isEmpty() || flows.isEmpty())
        fail(QStringLiteral("Missing legacy fixtures"));
    for (const auto &test : validation)
        compareValidation(test.toObject());
    for (const auto &test : flows)
        compareFlow(test.toObject());
    QTextStream(stdout) << "PASS: 96 catalog rows, " << validation.size() << " legacy validation inputs, "
                        << flows.size() << " offline config flows\n";
}
