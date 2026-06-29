# portal-cod4x-plugin
[![Build and Test](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/build-and-test.yml/badge.svg)](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/build-and-test.yml)
[![Code Quality](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/codequality.yml/badge.svg)](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/codequality.yml)
[![Copilot Setup Steps](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/copilot-setup-steps.yml/badge.svg)](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/copilot-setup-steps.yml)
[![Dependabot Auto-Merge](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/dependabot-automerge.yml/badge.svg)](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/dependabot-automerge.yml)
[![PR Verify](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/pr-verify.yml/badge.svg)](https://github.com/frasermolyneux/portal-cod4x-plugin/actions/workflows/pr-verify.yml)

## Documentation
* [Development Workflows](/docs/development-workflows.md) - Branch strategy, CI workflows, and local validation commands
* [Plugin Development](/docs/plugin-development.md) - CoD4x plugin architecture, build modes, and startup broadcast behavior
* [Plugin Runtime](/docs/plugin-runtime.md) - CoD4x ABI exports, startup message format, and version metadata flow

## Overview
portal-cod4x-plugin is an initial C++ CoD4x plugin skeleton for the XtremeIdiots portal estate. On plugin load, it pushes a color-coded [XI-BOT] online broadcast into server chat and includes the plugin version in that message. The repository includes a small unit test suite around runtime messaging behavior and CoD4x ABI export wiring (`OnInfoRequest`, `OnInit`, `OnTerminate`). Version metadata is sourced from `version.json` and injected into the runtime broadcast via CMake compile definitions.

## Contributing
Please read the [contributing](CONTRIBUTING.md) guidance; this is a learning and development project.

## Security
Please read the [security](SECURITY.md) guidance; I am always open to security feedback through email or opening an issue.
