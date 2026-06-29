<!--
Thanks for opening a PR. Fill in each section — sections with HTML comments
are guidance, not prose to leave behind. Delete the comments as you go.

Personal-project framing: keep it informal, but the validation evidence and
risk/rollout sections are not optional — they are what makes remote-agent
work safely mergeable.
-->

## Summary

<!-- One-line description of the change, plus the user request / issue that drove it. -->

Closes #

## Type of change

<!-- Tick all that apply. -->

- [ ] bugfix
- [ ] feature
- [ ] chore / refactor
- [ ] docs
- [ ] infra (Terraform)
- [ ] ci (GitHub Actions / Dependabot)
- [ ] dependencies
- [ ] breaking change

## Required reading consulted

<!--
For Copilot coding agent PRs: tick the ones that applied to this change.
Humans can leave blank if the change is trivial.
-->

- [ ] `AGENTS.md` (repo brief)
- [ ] `.github/copilot-instructions.md` (repo orientation)
- [ ] `.github-copilot/.github/instructions/personal.working-preferences.instructions.md` (always-on rules)
- [ ] Stack-specific instruction files referenced in `AGENTS.md` (`standards.*`, `patterns.*`, `platform.*`, `shared.*`)

## Validation evidence

<!--
Paste command output or attach screenshots/logs. Be specific:
"all tests passed" is not enough — show the command and the summary line.
-->

### Build

```text
<!-- e.g. dotnet build src/X.sln → Build succeeded. 0 Warning(s) 0 Error(s) -->
```

### Tests

```text
<!-- e.g. dotnet test … --filter "FullyQualifiedName!~IntegrationTests" → Passed!  - Failed: 0, Passed: 142 -->
```

### Format check

```text
<!-- e.g. terraform fmt -check -recursive (no output = pass)
     or dotnet format --verify-no-changes (no output = pass) -->
```

### Other (lint, terraform plan summary, screenshots)

<!-- Optional. -->

## Risk and rollout

<!--
- Blast radius (which environments / which services).
- Deploy triggers (does this auto-deploy on merge? deploy-dev label? deploy-prd on main?).
- Manual steps required post-merge (manual-steps.md updates?).
- Rollback plan if it goes wrong.
-->

- **Blast radius:**
- **Auto-deploys on merge?**
- **Manual steps post-merge:**
- **Rollback plan:**

## Consumer impact

<!--
Required when this PR touches a published contract (Abstractions / Api.Client packages, Service Bus DTOs / queue-name constants).
Delete this section entirely if no contract changed.

- List downstream consumers affected (repos / services).
- Is this a breaking change? If yes, apply the `breaking-contract` label and bump the package major version.
- Migration notes for consumers (renamed properties, removed fields, behaviour changes).
-->

- **Contracts touched**:
- **Breaking?**:
- **Downstream consumers**:
- **Migration notes**:

## Reviewer focus areas

<!-- Explicit asks: "double-check the role-assignment scope on line X",
"the OpenAPI transformer is the part I'm least sure about", etc. -->

## Agent attestation

<!--
For agent-authored PRs, complete every box below before requesting review.
Humans can leave these unticked when not applicable.

Every box below must be verifiable from the PR itself (diff, validation evidence, linked issue).
Do not add ceremonial boxes — they get ticked mechanically and add no safety.
-->

- [ ] Ran `code-review` sub-agent; High/Medium findings resolved or justified above in **Reviewer focus areas**
- [ ] PR body cites each acceptance criterion from the linked issue
- [ ] No client secrets, GUIDs, connection strings, or hard-coded subscription IDs introduced (`standards.oidc-and-secrets.instructions.md`)

---

<!--
By opening this PR you confirm:
- No client secrets, connection strings, or hard-coded subscription IDs introduced (`standards.oidc-and-secrets.instructions.md`).
- Naming and tagging follow `standards.azure-naming.instructions.md` and `standards.azure-tagging.instructions.md` for any new resources.
- Branching/PR rules in `standards.branching-and-prs.instructions.md` are respected.
-->
