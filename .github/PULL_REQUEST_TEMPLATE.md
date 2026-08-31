<!--
Fill in each section — sections with HTML comments are guidance, not prose to leave behind.
Delete the comments as you go. Validation evidence and risk/rollout are not optional.
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

## Validation evidence

<!--
Paste command output. Be specific: "all tests passed" is not enough —
show the command and the summary line.
-->

### Build

```text
<!-- cmake --build build --config Release -->
```

### Tests

```text
<!-- ctest --test-dir build --output-on-failure --build-config Release -->
```

### Format check

```text
<!-- clang-format --dry-run --Werror ... (no output = pass), or
     terraform fmt -check -recursive if terraform/ was touched -->
```

### Other (lint, terraform plan summary, screenshots)

<!-- Optional. -->

## Risk and rollout

- **Blast radius:**
- **Auto-deploys on merge?**
- **Manual steps post-merge:**
- **Rollback plan:**

## Consumer impact

<!--
Required only when this PR changes a published contract (Terraform output, blob artifact path/layout).
Delete this section entirely if no contract changed.
-->

- **Contracts touched**:
- **Breaking?**:
- **Downstream consumers**:
- **Migration notes**:

## Reviewer focus areas

<!-- Explicit asks, e.g. "double-check the role-assignment scope on line X". -->

---

<!--
By opening this PR you confirm no client secrets, connection strings, or
hard-coded subscription IDs were introduced.
-->
