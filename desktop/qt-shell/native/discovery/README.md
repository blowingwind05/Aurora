# Native device discovery catalog

Compile `Discovery.cpp` as C++17, include `Discovery.h`, and link Qt Core and
Qt Network from Qt 5.15 or Qt 6. The code operates on snapshots and address
values. The caller supplies any probe work.

## Inputs and outputs

- Populate `NetworkSnapshot::devices` from the existing account provider's
  normalized, eligible peers. Keep its primary and alternate IP addresses.
  `findTailnetPeer()` matches one authenticated non-self peer by either address
  and accepts IPv4-mapped IPv6 for this lookup. An ambiguous address yields no
  peer. The returned pointer borrows from the snapshot.
- Supply verified `PresentDevice` records and the saved identities for the
  current owner and local node to `resolveKnownIdentities()`. A single live
  GUID takes precedence over the saved GUID. Conflicting live GUIDs hide the
  node even if it had a saved ID, and a saved ID keeps an absent node visible.
  Persist a changed live ID through the account-scoped store.
- `merge(peers, lan, tailnet)` indexes bundled Sunshine routes on port 48989
  and merges only known Tailnet IDs. LAN presence is joined by Aurora GUID.
  If both routes exist, their Sunshine IDs must match with ordinal,
  case-insensitive comparison. Duplicate addresses within one bundled
  route set or live identity set raise `std::invalid_argument`, as they did
  when building the corresponding dictionaries in C#. Missing snapshots may
  be passed as null. The merge retains LAN-only rows and uses a stable
  connectable/online/name sort with the caller's Qt locale.
- Use `visibleDevices(network, lan, tailnet)` for the desktop consumer. It
  checks the authenticated snapshot and keeps only Tailnet-visible rows.
  Before publishing, the shell still needs the current catalog/account epoch
  and readiness checks from `QtDevicesReady`. Project each visible device with
  `qtDeviceRow(device, wallpaperUrl)` for the existing QML roles.
- Call `chooseRoute(device, probe[, RouteRequirement::Power])` to probe LAN
  before Tailnet. The callback receives the original route, including its
  embedded Tailnet flag. A successful result contains both the original route
  and the fresh, matching host response. Power selection requires fresh power
  support. An empty result means the caller should report the existing
  connection failure. Timeouts, encoder wait, and dispatch off the GUI thread
  belong to the probe and runtime integration.

The original `Discovery.cs`, `AuroraPresence.cs`, and
`AuroraWindow.QtShell.cs` define these policies. Name collation and Unicode
case tables come from Qt, so locale-specific ordering and casing of newer
Unicode characters can depend on the Qt build.

## Offline verification

Point `CMAKE_PREFIX_PATH` at the installed Qt SDK. On Windows, add its `bin`
directory to `PATH` for the fixture executable. Run from the repository root.

```sh
cmake -S desktop/qt-shell/native/discovery -B build/discovery -DCMAKE_PREFIX_PATH="/path/to/Qt/SDK"
cmake --build build/discovery --config Release
cmake -E chdir build/discovery ctest -C Release --output-on-failure
```

The native fixtures reuse the normalized `desk` and `lap` peers from
`tests/Discovery.Tests.ps1` and the desktop merge, identity, and route cases
from `tests/DesktopDiscovery.Tests.ps1`. The original CLI fixture remains
runnable offline with `pwsh -NoProfile -File tests/Discovery.Tests.ps1`.
The native fixture inputs are in-memory records and the probes are callbacks
that return fixture values.

The production integration still needs status normalization, discovery and
server-info transports, identity persistence, probe and stream lifecycle, and
shell wiring. The package and application entry point are independent of this
standalone fixture target.
