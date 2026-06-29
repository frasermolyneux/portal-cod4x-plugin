# Development Workflows

## Prerequisites (Windows)

Local builds require a C++ toolchain and CMake. Install both with winget:

```pwsh
# CMake (provides cmake + ctest on PATH)
winget install --id Kitware.CMake --source winget --accept-source-agreements --accept-package-agreements --silent

# Visual Studio Build Tools 2022 with the C++ workload (provides the MSVC compiler)
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget --accept-source-agreements --accept-package-agreements --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

| Dependency                                    | winget id                                | Provides                              |
| --------------------------------------------- | ---------------------------------------- | ------------------------------------- |
| CMake                                         | `Kitware.CMake`                          | `cmake`, `ctest`                      |
| Visual Studio Build Tools 2022 (C++ workload) | `Microsoft.VisualStudio.2022.BuildTools` | MSVC C++ compiler (`cl`), Windows SDK |

Notes:

- After installing, open a new terminal so the updated `PATH` is picked up. CMake's `project()` step needs a working C++ compiler — without the Build Tools, configure fails with `CMAKE_CXX_COMPILER not set`.
- Do not create an empty `cmake/` directory in the repo root. A folder named `cmake` shadows the `cmake` command in `process`-type VS Code tasks, producing `Path to shell executable ...\cmake does not exist`.

## Local validation

```pwsh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure --build-config Release
```

## CI workflows

- `build-and-test.yml` runs for `feature/**`, `bugfix/**`, and `hotfix/**` pushes.
- `pr-verify.yml` runs for pull requests and enforces draft-aware validation.
- `codequality.yml` runs scheduled Monday scans, push-to-main scans, and PR dependency review.
- `dependabot-automerge.yml` applies org policy and auto-merges safe Dependabot updates.
- `copilot-setup-steps.yml` prepares cloud coding-agent sessions.
