# Plugin Settings Rollout

This runbook covers Phase 2 operations for the CoD4x plugin runtime config flow:

- materialize local plugin config from Azure secrets and Terraform outputs
- deploy the config to the game server host
- validate runtime polling and ingest egress behavior
- rotate credentials safely

## Runtime config contract

The plugin reads a local JSON file named `portal-cod4x-plugin.config.json`.

Required fields:

- `ingestBaseUrl`
- `ingestSubscriptionKey`
- `gameServerId`
- `gameType` (`CallOfDuty4x`)
- `refreshIntervalSeconds` (15-900, default 120)

The plugin authenticates to APIM with the short `ingestSubscriptionKey` (sent as the `Ocp-Apim-Subscription-Key` header). APIM's managed identity forwards events to Service Bus and proxies active-ban reads to the Repository API, so the plugin no longer needs an Entra ID app registration or client secret.

An example file is provided at `portal-cod4x-plugin.config.example.json`.

### Optional bot prefix

The existing agent installer/config writer supplies `agentName` after resolving the
effective server-over-global Agent Name. It is a complete rendered prefix, for example
`^4[^1>XI< BOT^4]^7`, not a plain display name. The plugin uses it verbatim and puts
`^5(PM)^7 ` before it for private replies. The header appears only on the first fragment.

Older agents/configs can omit the field; the plugin retains `^4[^1XI-BOT^4]^7`.
Null or blank values also select that default. Configured prefixes must be native-safe
UTF-8 text without quotes/control bytes and no more than 139 UTF-8 bytes.

Upload the config before plugin load for a matching startup broadcast. For live renames,
do not overwrite the credential-bearing config: upload the optional sibling
`portal-cod4x-plugin.agent-name.json` with this non-sensitive content instead:

```json
{
    "gameServerId": "11111111-2222-3333-4444-555555555555",
    "agentName": "^4[^1>XI< BOT^4]^7"
}
```

The plugin reads a valid name file at startup and checks it every 60 seconds without
reloading credentials/server identity. Missing files retain the current name for older
agents; invalid/incomplete refreshes retain the last good name and log a diagnostic.
Explicit null/blank `agentName` resets the legacy default. Retry failed uploads; a bad
name file cannot corrupt auth, and an invalid name file at restart falls back to the
main config's optional value.

Before every agent-managed load/restart/rollback, republish the current effective name
file, regardless of the name cache. Do not load an abandoned old sidecar after a failed
publication; retry through the existing lifecycle.

The additive main field and name file can be deployed by the agent before the plugin
release: older plugins ignore both. Verify the actual loaded plugin version, startup
prefix and a private command reply after the normal installer rollout.

## Source of truth from Azure

Values are sourced from `portal-environments` Terraform outputs and shared Key Vault secrets:

- secret `cod4x-plugin-ingest-api-endpoint` for `ingestBaseUrl`
- secret `cod4x-plugin-ingest-subscription-key` for `ingestSubscriptionKey`
- Terraform output `server_events_api.api_management.endpoint` for ingest endpoint fallback

## Generate config file

Use the helper script:

```pwsh
./scripts/New-Cod4xPluginConfigFromAzure.ps1 `
  -SharedKeyVaultName <shared-kv-name> `
  -GameServerId <game-server-guid> `
  -PortalEnvironmentsPath "../portal-environments/terraform" `
  -OutputPath "./portal-cod4x-plugin.config.json"
```

If Terraform output access is not available, pass `-IngestBaseUrl` directly.

## Deploy config to CoD4x host

1. Copy `portal-cod4x-plugin.config.json` to the plugin runtime working directory used by CoD4x.
2. Restrict read access on the file to the game server process account only.
3. Restart the CoD4x server or reload the plugin.

## Smoke test checklist

1. Startup check:
- Confirm log line similar to `Portal Plugin is online (version ...)`.
- Confirm log line `plugin config loaded for gameServerId ...`.

2. API auth and poll check:
- Confirm no recurring `failed to acquire access token for repository API` errors.
- Confirm no recurring ingest token acquisition failures.
- Confirm periodic runtime activity every `refreshIntervalSeconds`.

3. Settings merge and enforcement check:
- Set global `cod4xCommands.enabled=true`, add a command minPower, wait one poll interval.
- Confirm command reconciliation logs and in-server power change.
- Add a server override for the same command, wait one poll interval.
- Confirm server override wins over global.

4. Disabled gate check:
- Set effective `cod4xCommands.enabled=false`.
- Confirm log `cod4xCommands enforcement disabled...` and no further command updates are applied.

## Rollback

To stop active enforcement quickly:

1. Set server-level `cod4xCommands.enabled=false` (preferred) or global false.
2. Wait one poll interval.
3. Confirm enforcement-disabled log message.

This stops new reconciliations and leaves current in-server command powers unchanged.

## Credential rotation

The plugin app secret rotates in Terraform every 30 days.

Rotation process:

1. Apply `portal-environments` changes.
2. Regenerate `portal-cod4x-plugin.config.json` with the helper script.
3. Deploy updated config file to each CoD4x host.
4. Reload plugin/server and run smoke checks.

If config is not refreshed before expiry, token acquisition will fail and settings refresh will stop.
