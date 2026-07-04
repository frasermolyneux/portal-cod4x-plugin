[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SharedKeyVaultName,

    [Parameter(Mandatory = $true)]
    [string]$GameServerId,

    [Parameter(Mandatory = $false)]
    [string]$RepositoryApiResource,

    [Parameter(Mandatory = $false)]
    [string]$IngestBaseUrl,

    [Parameter(Mandatory = $false)]
    [string]$IngestApiResource,

    [Parameter(Mandatory = $false)]
    [ValidateSet("CallOfDuty4x")]
    [string]$GameType = "CallOfDuty4x",

    [Parameter(Mandatory = $false)]
    [string]$PortalEnvironmentsPath,

    [Parameter(Mandatory = $false)]
    [ValidateRange(15, 900)]
    [int]$RefreshIntervalSeconds = 120,

    [Parameter(Mandatory = $false)]
    [string]$OutputPath = "./portal-cod4x-plugin.config.json",

    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-RequiredCommand {
    param([string]$Name)

    if (-not (Get-Command -Name $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found on PATH."
    }
}

function Get-KeyVaultSecretValue {
    param(
        [string]$VaultName,
        [string]$SecretName
    )

    $value = az keyvault secret show --vault-name $VaultName --name $SecretName --query value -o tsv
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "Secret '$SecretName' in vault '$VaultName' is empty or missing."
    }

    return $value.Trim()
}

function Get-TerraformOutputValue {
    param(
        [string]$Path,
        [string]$Name,
        [string]$JsonPath
    )

    $outputJson = terraform -chdir=$Path output -json $Name
    if ([string]::IsNullOrWhiteSpace($outputJson)) {
        throw "terraform output for '$Name' was empty."
    }

    $outputObject = $outputJson | ConvertFrom-Json
    $valueObject = $outputObject
    foreach ($segment in $JsonPath.Split('.')) {
        if ($null -eq $valueObject) {
            break
        }

        $valueObject = $valueObject.$segment
    }

    $value = [string]$valueObject

    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "Unable to resolve '$Name.$JsonPath' from Terraform output."
    }

    return $value.Trim()
}

Get-RequiredCommand -Name "az"

$tenantId = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "azuread-app-tenant-id-cod4x-plugin"
$clientId = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "azuread-app-client-id-cod4x-plugin"
$clientSecret = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "azuread-app-password-cod4x-plugin"
$repositoryApiBaseUrl = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "cod4x-plugin-repository-api-endpoint"

if ([string]::IsNullOrWhiteSpace($IngestBaseUrl)) {
    try {
        $IngestBaseUrl = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "cod4x-plugin-ingest-api-endpoint"
    }
    catch {
        if ([string]::IsNullOrWhiteSpace($PortalEnvironmentsPath)) {
            throw "IngestBaseUrl is required when the ingest endpoint secret is missing and PortalEnvironmentsPath is not provided."
        }
    }
}

if ([string]::IsNullOrWhiteSpace($RepositoryApiResource) -or [string]::IsNullOrWhiteSpace($IngestApiResource) -or [string]::IsNullOrWhiteSpace($IngestBaseUrl)) {
    if ([string]::IsNullOrWhiteSpace($PortalEnvironmentsPath)) {
        throw "RepositoryApiResource, IngestApiResource, and IngestBaseUrl require PortalEnvironmentsPath when not supplied directly."
    }

    Get-RequiredCommand -Name "terraform"
}

if ([string]::IsNullOrWhiteSpace($RepositoryApiResource)) {
    $RepositoryApiResource = Get-TerraformOutputValue -Path $PortalEnvironmentsPath -Name "repository_api" -JsonPath "application.primary_identifier_uri"
}

if ([string]::IsNullOrWhiteSpace($IngestApiResource)) {
    $IngestApiResource = Get-TerraformOutputValue -Path $PortalEnvironmentsPath -Name "server_events_api" -JsonPath "application.primary_identifier_uri"
}

if ([string]::IsNullOrWhiteSpace($IngestBaseUrl)) {
    $IngestBaseUrl = Get-TerraformOutputValue -Path $PortalEnvironmentsPath -Name "server_events_api" -JsonPath "api_management.endpoint"
}

$configObject = [ordered]@{
    tenantId               = $tenantId
    clientId               = $clientId
    clientSecret           = $clientSecret
    repositoryApiBaseUrl   = $repositoryApiBaseUrl
    repositoryApiResource  = $RepositoryApiResource
    ingestBaseUrl          = $IngestBaseUrl
    ingestApiResource      = $IngestApiResource
    gameServerId           = $GameServerId
    gameType               = $GameType
    refreshIntervalSeconds = $RefreshIntervalSeconds
}

$outputFilePath = Resolve-Path -Path (Split-Path -Path $OutputPath -Parent) -ErrorAction SilentlyContinue
if (-not $outputFilePath) {
    $outputDirectory = Split-Path -Path $OutputPath -Parent
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
}

if ((Test-Path -Path $OutputPath) -and (-not $Force)) {
    throw "Output file '$OutputPath' already exists. Use -Force to overwrite it."
}

$configJson = $configObject | ConvertTo-Json -Depth 4
Set-Content -Path $OutputPath -Value $configJson -Encoding utf8NoBOM

Write-Host "Wrote plugin config to $OutputPath"
Write-Host "GameServerId: $GameServerId"
Write-Host "GameType: $GameType"
Write-Host "Repository API base URL: $repositoryApiBaseUrl"
Write-Host "Repository API resource: $RepositoryApiResource"
Write-Host "Ingest API base URL: $IngestBaseUrl"
Write-Host "Ingest API resource: $IngestApiResource"
Write-Host "Refresh interval: $RefreshIntervalSeconds seconds"
Write-Host "Client secret was written to disk; secure this file on the host."
