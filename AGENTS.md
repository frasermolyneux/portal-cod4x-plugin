# AGENTS.md — portal-cod4x-plugin

C++ CoD4x plugin repository for XtremeIdiots portal integration. Current scope is a minimal loadable plugin skeleton with startup broadcast and unit-tested runtime messaging behavior.

This file is the brief for the **GitHub Copilot coding agent** (and any other agent that follows the [agents.md](https://agents.md) convention) when it runs in a cloud runner without the local VS Code multi-root workspace context.

> If you are a human reading this in VS Code, prefer `.github/copilot-instructions.md` for project orientation. `AGENTS.md` is the agent execution brief.

---

## Required reading (read these BEFORE doing any work)

The `copilot-setup-steps.yml` workflow checks out `frasermolyneux/.github-copilot` at `./.github-copilot/` in the runner, so the paths below resolve.

1. `.github/copilot-instructions.md` — repo-specific orientation, build commands, conventions
2. `.github-copilot/.github/instructions/personal.working-preferences.instructions.md` — Fraser's always-on rules: git hands-off, default to assigned branch, run `code-review` agent before reporting done
3. `.github-copilot/.github/copilot-instructions.md` — org-wide context catalog (use as index for the layered instruction files below)
4. Stack-specific files — see **Stack guardrails** below

---

## Org conventions via MCP (when available)

If a `frasermolyneux-copilot` MCP server is configured in your client (`~/.copilot/mcp-config.json`, VS Code user `mcp.json`, or an equivalent stdio MCP wire-up), **prefer its catalog tools** over your own assumptions when answering questions about org standards, branching, workflows, Terraform, .NET projects, Azure patterns, or shared library / platform consumption contracts. The catalog source-of-truth lives in `frasermolyneux/.github-copilot` — see `mcp-server/README.md` there for the tool contract.

This is **complementary** to the file-load model: if `./.github-copilot/` is checked out in the runner (per `copilot-setup-steps.yml`), continue to read those files directly. If both are available, prefer MCP for freshness. If no MCP server is configured in your client, treat this section as a no-op and fall back to the file paths above.

---

## Stack guardrails

### Tenant facts (always-on)
- `.github-copilot/.github/instructions/tenant.subscriptions.instructions.md`
- `.github-copilot/.github/instructions/tenant.regions.instructions.md`
- `.github-copilot/.github/instructions/tenant.identity.instructions.md`

### Enforceable standards (apply to your changes)
- `.github-copilot/.github/instructions/standards.branching-and-prs.instructions.md`
- `.github-copilot/.github/instructions/workflows.instructions.md`
- `.github-copilot/.github/instructions/workflows.scheduling.instructions.md`

### Patterns (apply where relevant)
- `.github-copilot/.github/instructions/patterns.nbgv-versioning.instructions.md`

---

## Build, test, format

```pwsh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure --build-config Release
```

Optional formatting check (when `clang-format` is installed):

```pwsh
clang-format --dry-run --Werror src/*.cpp include/portal_cod4x/*.h tests/*.cpp
```

---

## Do NOT

- ❌ Do not `git commit`, `git push`, force-push, rebase, `reset --hard`, or create/delete branches. Work on the branch you were assigned to.
- ❌ Do not bypass build/test validation before sign-off.
- ❌ Do not hard-code runtime secrets in source; use cvars/environment-based configuration when extending runtime behavior.
- ❌ Do not change workflow schedules without updating `.github-copilot/docs/ops-clock.md`.
- ❌ Do not pull context from sibling workspace folders. Only what is inside this repo and `./.github-copilot/` is in scope.
- ❌ Do not assume tools/SDKs are installed beyond what `.github/workflows/copilot-setup-steps.yml` provisions. If you need more, add the step and explain why.

---

## Opening the PR

You MUST use `.github/PULL_REQUEST_TEMPLATE.md` as your PR body — do **not** write a freeform body. The org template is inherited from `frasermolyneux/.github` and GitHub pre-populates it when you open the PR. Concretely:

1. Fill `## Summary` (one line) and `Closes #<issue>`.
2. Tick the relevant `## Type of change` box.
3. Paste the **actual command output** from your Build, Tests, and Format check runs into `## Validation evidence`. Show the real summary line, not "tests passed".
4. Fill `## Risk and rollout` — blast radius, auto-deploy?, manual steps post-merge, rollback plan.
5. Tick **every** box in `## Agent attestation`.
6. Delete `## Consumer impact` only if no published contract (Abstractions / Client NuGet / Service Bus DTO / Terraform output) changed.

Complete the `## Agent attestation` section before requesting review; reviewers use it as a readiness checklist.

---

## Pre-PR checks (run before you open the PR)

- [ ] CMake configure/build succeeds
- [ ] Unit tests pass (`ctest --output-on-failure`)
- [ ] If `clang-format` is available, format check passes
- [ ] No new secrets / GUIDs / connection strings introduced
- [ ] Changes align with files in **Stack guardrails**
- [ ] `code-review` sub-agent run; High/Medium findings resolved or justified in the PR body

---

## Escalation

If you hit any of the conditions below, **open the PR as draft** and **apply the `needs-decision` label** instead of pushing forward to ready-for-review. Post a comment on the originating issue summarising what's blocking you and what decision is needed.

This protects against the agent silently expanding scope, bypassing a contract change, or merging a half-resolved review finding.

Stop and escalate when:

- Required reading file is missing or conflicting.
- A required SDK/toolchain for x86 plugin binary output is unavailable in CI runner setup.
- A `code-review` finding is High severity and you cannot resolve it without expanding scope.
- The requested behavior conflicts with CoD4x handler ABI constraints.
