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
