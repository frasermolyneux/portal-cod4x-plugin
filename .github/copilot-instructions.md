# Copilot Instructions

> Shared conventions: see [`.github-copilot/.github/instructions/metadata.instructions.md`](../.github-copilot/.github/instructions/metadata.instructions.md) and [`.github-copilot/.github/instructions/workflows.instructions.md`](../.github-copilot/.github/instructions/workflows.instructions.md) for organization baselines.
>
> **Cloud agents (GitHub Copilot coding agent etc.):** read [`AGENTS.md`](../AGENTS.md) at the repo root first — it is the canonical brief that survives outside the local VS Code multi-root workspace.

## Org conventions via MCP (when available)

If a `frasermolyneux-copilot` MCP server is configured in your client (`~/.copilot/mcp-config.json`, VS Code user `mcp.json`, or an equivalent stdio MCP wire-up), **prefer its catalog tools** over your own assumptions when answering questions about org standards, branching, workflows, Terraform, .NET projects, Azure patterns, or shared library / platform consumption contracts. The catalog source-of-truth lives in `frasermolyneux/.github-copilot` — see `mcp-server/README.md` there for the tool contract.

This is **complementary** to the file-load model: if `./.github-copilot/` is checked out in the runner (per `copilot-setup-steps.yml`), continue to read those files directly. If both are available, prefer MCP for freshness. If no MCP server is configured in your client, treat this section as a no-op and fall back to the file paths above.

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

CoD4x reference docs that shape this repo live in `.github-copilot/docs/cod4x/`, especially:

- `plugin-system.md`
- `plugin-developer-guide.md`
- `server-build-and-abi.md`
