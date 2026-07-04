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

`build-and-test.yml` and `pr-verify.yml` always publish Linux x86 and Windows x86 plugin binaries. For Windows builds, CI uses `windows-sdk/com_plugin.lib` when present and otherwise generates a temporary fallback import library from expected host exports (`Plugin_Printf`, `Plugin_ChatPrintf`) and the configured host module name (`COD4X_WINDOWS_HOST_MODULE`, default `exec_cod4boom.exe`) before building. CI then verifies the plugin import table contains that host module:

- `portal-cod4x-plugin-v<major.minor>-linux-x86-<run-number>`
- `portal-cod4x-plugin-v<major.minor.run-number>-linux-x86`
- `portal-cod4x-plugin-v<major.minor.run-number>-windows-x86`
