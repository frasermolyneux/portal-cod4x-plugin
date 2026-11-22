# Plugin Development

## Runtime behavior

The plugin runtime now includes:

- CoD4x callback wiring for player/connect/chat/map lifecycle events
- non-blocking HTTP state-machine work on `OnFrame`
- bounded egress buffering with batch posts to APIM ingest endpoints
- periodic settings pull with offline local cache
- startup broadcast on `OnInit`

Startup broadcast details:

- Prefix: `^4[^1XI-BOT^4]^7`
- Message: `Portal Plugin is online (version <version>)`
- Broadcast target slot: `-1` (all players)

Runtime logic is isolated in `plugin_runtime` and `plugin_exports` modules with unit-test coverage for runtime behavior and callback bridging.

Player connect, disconnect, map change and status callbacks publish separate playtime observations to
`/ingest/events/playtime-observation` as well as the existing legacy events. The playtime payload contains
only the player identity fields required by the consumer, not IP addresses or chat. Observation IDs
are the message IDs and remain stable during in-memory retries; producer timestamps use the host's
UTC wall clock. The queue is memory-only: a plugin or server restart loses unsent events, and no spool
replays them. It holds at most 1,000 events; at capacity, new chat is discarded and other events evict a
buffered chat if available, otherwise the oldest event. An in-flight event can be evicted before its result
is known. Events are also discarded after eight failed attempts or 15 minutes. A retry retains its observation
ID while buffered, but sequence numbers are scoped to a producer epoch and do not provide durable replay.
Consequently, a missing join, disconnect or map change can leave playtime or map attribution incomplete;
later partial snapshots cannot prove absence or recover unobserved history. Report this gap in coverage
rather than treating measured time as complete.
Playtime roster observations include `playtimeCoverage` with the producer's UTC start time and cumulative
counts of dedicated playtime observations discarded for capacity, attempts or age during the accompanying producer epoch.
Each discard is counted once; attempt exhaustion takes precedence if both thresholds are exceeded.
The observation's producer epoch scopes these counters. Its `eventPublishedUtc` is stamped on the
first publish attempt and retained on retries; the consumer uses it as the coverage report time.
These counts report loss of local retention, not confirmed downstream loss: an in-flight request may
already have been accepted. Counters reset on restart, and losses can occur before a status is delivered,
so zero counts and a new epoch do not establish complete coverage.
Status snapshots are always marked partial: the current host interface cannot reliably distinguish empty
slots, bots and unresolved players, so they must not be used to infer absence or treated as proof that
individual entries are human. Bot creation can bypass `OnPlayerConnect`; the current adapter does not read
the host's per-client state or bot flag, so player bot classification is unknown. The upstream SDK exposes
client state, but this repository does not pin a matching deployed host or verify its client layout; handler
version 4.000 alone does not establish that compatibility. Callback timestamps reflect when the plugin
observes the event; hot-loaded players are not credited for earlier time.
If a disconnect is missed, a same-player reconnect in the same slot cannot be distinguished safely
from a late connect callback; its join can be suppressed and the resulting coverage remains uncertain.
A zero-ID slot in a partial roster does not prove a disconnect, and the local discard counters do not
count callbacks the plugin never observed or joins suppressed by this ambiguity.
The plugin is the intended CoD4x playtime source; servers without an active plugin have a coverage gap.
Deploy the ingest route and consumer before releasing the plugin. No production host coverage or
callback freshness has been verified here.

Phase 2 settings rollout guidance is documented in `docs/plugin-settings-rollout.md`, including config generation and smoke validation.

Local config generation helper:

```pwsh
./scripts/New-Cod4xPluginConfigFromAzure.ps1 -SharedKeyVaultName <shared-kv-name> -GameServerId <server-guid> -PortalEnvironmentsPath "../portal-environments/terraform"
```

## Versioning

`version.json` is the version source for the repository. CMake extracts `major.minor` and stamps compile-time constants used by:

- `OnInfoRequest` plugin version fields
- Startup chat broadcast version text

You can override the semantic value passed to code at build time with `-DPORTAL_COD4X_PLUGIN_VERSION=<value>`.

In CI plugin-binary jobs, CMake is explicitly passed a full semantic token (`major.minor.runNumber`) via
`-DPORTAL_COD4X_PLUGIN_VERSION=...` so `pluginInfo` short description and runtime health checks are pinned to
the same build version as the produced artifact.

## Build modes

> Local builds require CMake and a C++ toolchain. See [development-workflows.md](development-workflows.md#prerequisites-windows) for the winget install commands.

### Test/build mode (default)

```pwsh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure --build-config Release
```

### Plugin binary mode

Enable module output (x86 default) with:

```pwsh
cmake -S . -B build -DPORTAL_COD4X_BUILD_PLUGIN_BINARY=ON -DPORTAL_COD4X_PLUGIN_ARCH=x86
cmake --build build --config Release
```

### Plugin binary mode (Windows x86 / Win32)

When using a multi-config MSVC generator, plugin architecture is selected at configure time. Use `-A Win32` for CoD4x-compatible x86 output, and provide the CoD4x host import library (`com_plugin.lib`) so host API symbols resolve during linking:

```pwsh
cmake -S . -B build-win32 -A Win32 -DPORTAL_COD4X_BUILD_PLUGIN_BINARY=ON -DPORTAL_COD4X_PLUGIN_ARCH=x86 -DCOD4X_PLUGIN_HOST_IMPORT_LIB=<path-to-com_plugin.lib>
cmake --build build-win32 --config Release
```

### Plugin binary mode (Linux x86)

Linux x86 plugin output requires multilib toolchain packages:

```bash
sudo apt-get update
sudo apt-get install -y gcc-multilib g++-multilib
cmake -S . -B build-linux-x86 -DPORTAL_COD4X_BUILD_PLUGIN_BINARY=ON -DPORTAL_COD4X_PLUGIN_ARCH=x86
cmake --build build-linux-x86 --config Release
```

If building against the real CoD4x SDK headers, also set:

```pwsh
cmake -S . -B build -DPORTAL_COD4X_BUILD_PLUGIN_BINARY=ON -DPORTAL_COD4X_PLUGIN_ARCH=x86 -DPORTAL_COD4X_USE_EXTERNAL_SDK=ON -DCOD4X_PLUGIN_SDK_INCLUDE_DIR=<path-to-CoD4x-plugins-folder>
```

## CI plugin artifacts

`build-and-test.yml`, `pr-verify.yml`, and `release-version-and-tag.yml` publish Linux x86 and Windows x86 plugin binaries. For Windows builds, CI uses `windows-sdk/com_plugin.lib` when present and otherwise generates a temporary fallback import library from the canonical symbol manifest at `windows-sdk/com_plugin.exports.txt` and the configured host module name (`COD4X_WINDOWS_HOST_MODULE`, default `exec_cod4boom.exe`) before building. CI then verifies both:

- the plugin import table contains the expected host module
- the host module import section contains every required symbol from `windows-sdk/com_plugin.exports.txt`

- `portal-cod4x-plugin-v<major.minor>-linux-x86-<run-number>`
- `portal-cod4x-plugin-v<major.minor.run-number>-linux-x86`
- `portal-cod4x-plugin-v<major.minor.run-number>-windows-x86`
