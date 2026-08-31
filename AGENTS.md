# AGENTS.md — portal-cod4x-plugin

C++ CoD4x plugin for the XtremeIdiots portal estate, plus a Terraform root scoped to this workload's build-artifact storage only.

Execution brief for the GitHub Copilot coding agent (and other [agents.md](https://agents.md)-following agents) running without local VS Code workspace context. See `.github/copilot-instructions.md` for repository structure, CoD4x ABI conventions, and the docs index.

---

## Build, test, format

```pwsh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure --build-config Release
```

Optional: `clang-format --dry-run --Werror src/*.cpp include/portal_cod4x/*.h tests/*.cpp`

## Terraform boundary

`terraform/` (single root) provisions only this workload's build-artifact storage account + container + RBAC. It reads remote state from `platform-workloads` and `portal-environments` (managed identities only) — it never creates, owns, or copies `portal-environments` resources (Key Vault, APIM, managed identities).

```pwsh
terraform -chdir=terraform fmt -check -recursive
```

Never run `terraform init/plan/apply/destroy`; that happens only via `deploy-dev.yml`, `deploy-prd.yml`, `destroy-environment.yml` under OIDC.

## Artifact ownership

CI builds Linux (`.so`) / Windows (`.dll`) binaries and uploads them as GitHub Actions artifacts on every build/PR. On a release tag, `release-publish-plugin.yml` also uploads both to the Terraform-provisioned Azure Blob container (`releases/<tag>/<os>/x86/...`) and creates a GitHub Release. Installing the artifact on a game server host is out of scope for this repo.

## Do NOT

- ❌ Hard-code runtime secrets; config is cvar/env/local-JSON only (`docs/plugin-settings-rollout.md`).
- ❌ Create or modify `portal-environments` resources/config from this repo — read-only remote state consumption only.
- ❌ Run `terraform init/plan/apply/destroy`, or trigger deploy/release/destroy workflow runs.
- ❌ Change workflow schedules, Terraform resource naming, or role-assignment scopes without the task explicitly calling for it.
