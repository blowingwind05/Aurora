# Bundled Sunshine administration

`Aurora::SunshineAdmin` is a C++17 library using Qt Network. It uses
`https://127.0.0.1:48990/api/config`, the bundled Sunshine endpoint at `e05bd75`.

## Interface

Add this directory with `add_subdirectory`, link `Aurora::SunshineAdmin`, and include
`BundledAdmin.h`. The client is `Aurora::Sunshine::BundledAdmin`.

- `open(profileDirectory, launchPassword, error)` takes the bundled host's existing
  `config` directory and its current 64-byte ASCII hex password. The lifecycle owner
  supplies the same password bytes it sends the child over stdin and releases the
  client when the host exits or restarts. The legacy profile is
  `Aurora/Sunshine/Bundled/config` under the user's Local Application Data directory on
  Windows or Application Data directory on macOS.
- The client reads the SHA-256 leaf pin from `credentials/cacert.pem` inside that
  profile and uses HTTP Basic authentication as `aurora-local`. The client holds the
  authorization in memory. The certificate must resolve to a local file inside the
  supplied profile.
- `readConfig(done)` fetches and returns a decoded JSON object. Call
  `saveConfig(prepare, verify, done)` to fetch the latest config, POST a prepared
  full payload, and verify a fresh readback. `prepare(current, error)` returns a
  `QJsonObject` or `std::nullopt` with an error. `verify(actual)` returns an empty
  string when the changes are confirmed. The catalog owns validation, the managed
  field gate, and the merge. With the catalog module, a caller can capture the
  result of `validateChanges` and use `mergeOptions` and `verifyOptions` in those
  callbacks. For preferences, the caller also owns the required privacy snapshots.
- Both methods return `true` when started and deliver one asynchronous completion
  while the client lives. `Result` contains an error code, HTTP status when available,
  an error message or the successful config. Calls belong on the client's event-loop
  thread. One operation can be in flight per client. Destruction cancels outstanding
  work. An error after POST can occur after the server applied the payload.

Each exchange has a five-second deadline and a 256 KiB limit on the config body.
Replies require pinned HTTPS, a success HTTP status, and a JSON object with boolean
`status: true`. Each exchange owns a fresh Qt network manager with direct
connections, manual redirects, and cookie handling and authentication reuse
disabled. Qt trust exceptions are limited to the exact pinned leaf on the fixed
endpoint.

This slice covers bundled config transport. The lifecycle owner, QML wiring,
privacy transport, restart, pairing, and standalone Sunshine administration are
separate integration work.

## Bundled pairing

- `readPendingPairings(PendingCompletion)` reads `GET /api/pin`. It returns
  a typed list from the response's `pairings` array. Each entry has a 32-digit
  hexadecimal ID and untrusted name and address text. `sourceIpv4` is present
  only for strict dotted-decimal IPv4 or a valid IPv4-mapped IPv6 address.
  `sourcePort` is present only for a JSON integer from 1 through 65535. Legacy
  rows with missing source metadata remain available to callers that support them.
- `approvePairing(SupervisorPairingApproval, ApprovalCompletion, stillCurrent)`
  sends `POST /api/pin` with the exact `pairing_id`, four ASCII digit `pin`, and
  `name`. Construct the approval with `SupervisorPairingApproval::authorized`
  after the coordinator has approved that match. The optional `stillCurrent`
  predicate runs on the admin thread at admission and during TLS before HTTP data
  is sent. Approval requires an authenticated HTTP success with boolean
  `status: true`.

Both calls share the settings transport's loopback endpoint, leaf pin, credentials,
deadline, and body limit. The coordinator owns supervisor authorization, whois,
source-port attribution, and correlation with the selected pending request.

## Offline verification

From the repository root, with a Qt 5.15 or Qt 6 development kit that supports TLS:

```sh
cmake -S desktop/qt-shell/native/sunshine/admin -B build/sunshine-admin -DCMAKE_PREFIX_PATH=/path/to/qt
cmake --build build/sunshine-admin --config Release
ctest --test-dir build/sunshine-admin -C Release --output-on-failure
```

On Windows, include the Qt kit's `bin` directory on `PATH` for CTest. The fixture
binds ephemeral IPv4 loopback ports and creates a temporary profile in its build
directory. All PEM files under `tests/tls` are public test fixtures. Their certificates
and private keys are disposable and only serve the test listener. The test uses an
invented launch credential, including for its wire-level authorization checks.
