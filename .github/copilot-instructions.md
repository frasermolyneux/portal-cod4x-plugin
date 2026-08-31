# Copilot Instructions

> **Cloud agents (GitHub Copilot coding agent etc.):** read [`AGENTS.md`](../AGENTS.md) at the repo root first — it is the canonical brief that survives outside the local VS Code multi-root workspace.

## Project Overview

portal-cod4x-plugin is a C++ CoD4x plugin skeleton that currently focuses on baseline lifecycle wiring and startup observability. On `OnInit`, it broadcasts an online message with plugin version metadata and a color-coded [XI-BOT] prefix.

## Repository Structure

- `include/portal_cod4x/` — plugin runtime interfaces and version constants
- `src/plugin_runtime.cpp` — runtime message composition and startup logic
- `src/plugin_exports.cpp` — CoD4x ABI exports (`OnInfoRequest`, `OnInit`, `OnTerminate`)
- `tests/plugin_runtime_tests.cpp` — unit tests for startup broadcast and version message behavior
- `version.json` — version source consumed by CMake and runtime metadata

## Build and Test

```pwsh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure --build-config Release
```

## CoD4x conventions used

- Plugin ABI target: Handler `4.000`
- Startup broadcast uses `Plugin_ChatPrintf(-1, ...)` to broadcast to all players
- Prefix format follows CoD color coding style from the portal estate: `^4[^1XI-BOT^4]^7`
- `OnInfoRequest` publishes plugin name, summary, and major/minor version

## Documentation

- `docs/plugin-development.md` — build modes, versioning, CI artifact naming
- `docs/plugin-runtime.md` — ABI wiring, startup broadcast, ban diagnostics
- `docs/plugin-settings-rollout.md` — runtime config contract and Azure source of truth
- `docs/development-workflows.md` — local prerequisites, CI workflow list
