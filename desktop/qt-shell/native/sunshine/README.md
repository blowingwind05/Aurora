# Native Sunshine configuration

`Aurora::SunshineConfig` is a C++17 static library using Qt Core. Its resource embeds
`desktop/SunshineOptions.json` directly. The catalog order, labels, categories, defaults,
choices, and managed overrides follow Aurora `e05bd75`.

## C++ API

Add this directory with `add_subdirectory` in the consuming project, link
`Aurora::SunshineConfig`, and include `SunshineConfig.h`. The functions are in
`Aurora::Sunshine`.

- `managedOptions()` returns the bundled profile's fixed values. The caller passes the
  selected host's managed status to `configForQml` and `validateChanges`.
- `configForQml(config, managed, privacy)` returns the QML `state.config` object with
  `options`, `values`, and nullable `privacy`. Pass decoded configuration JSON and an
  optional `parsePrivacySettings(privacyReply)` result. `parsePreferences(config)`
  exposes the same name and audio normalization independently.
- `validateChanges(qmlChanges, managed)` accepts the QML `changes` object and returns
  `preferences()`, `options()`, and per-field `errors()`. Accepted values are normalized
  strings. An empty option value clears that key. Choices and ordinary toggles allow
  upstream values beyond the catalog's display choices.
- Process accepted preferences first. Call `resolvePreferences(changes, currentConfig,
  currentPrivacy, error)` to fill omitted fields from the initial snapshot. Call
  `mergePreferences(freshConfig, update, freshPrivacy, error)` with a fresh baseline
  before submitting the payload. Both functions return `std::nullopt` on failure and
  fill `error`. An empty preferences group returns `std::nullopt` with an empty error.
  A display update requires the corresponding privacy snapshots.
- For a nonempty options group, call `mergeOptions(freshConfig, changes)` with the latest
  config after the preference attempt. The merges remove the response metadata
  `status`, `version`, and `platform` and retain untouched JSON values, including unknown
  options. The projection uses the decoded values supplied by Qt. For non-string
  numeric or compound values, their displayed text uses Qt's compact JSON formatting.
- After the caller submits each payload, `verifyPreferences(actualConfig, update,
  actualPrivacy)` and `verifyOptions(actualConfig, changes)` return an empty string on
  matching readback. Report a group error against every key in that group and proceed
  with the other group independently. The caller owns the request envelope, transport,
  scheduling, and publishing of the QML save result.

## Offline equivalence check

From the repository root, using a Qt 5.15 or Qt 6 development kit:

```sh
cmake -S desktop/qt-shell/native/sunshine -B build/sunshine-config -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/sunshine-config --config Release
ctest --test-dir build/sunshine-config -C Release --output-on-failure
```

On Windows, put the selected Qt kit's `bin` directory on `PATH` before running CTest.
The test compares all catalog rows with the source JSON. The independent fixture
`tests/legacy-inputs.json` records results from the baseline C# catalog and configuration
code, with an in-memory Sunshine handler for reads and write acknowledgements. The
native test runs with Qt Core and reads only the bundled catalog and test fixtures.
