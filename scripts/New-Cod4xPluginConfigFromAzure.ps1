[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SharedKeyVaultName,

    [Parameter(Mandatory = $true)]
    [string]$GameServerId,

    [Parameter(Mandatory = $false)]
    [string]$RepositoryApiResource,

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

Get-RequiredCommand -Name "az"

$tenantId = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "azuread-app-tenant-id-cod4x-plugin"
$clientId = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "azuread-app-client-id-cod4x-plugin"
$clientSecret = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "azuread-app-password-cod4x-plugin"
$repositoryApiBaseUrl = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "cod4x-plugin-repository-api-endpoint"

if ([string]::IsNullOrWhiteSpace($RepositoryApiResource)) {
    if ([string]::IsNullOrWhiteSpace($PortalEnvironmentsPath)) {
        throw "RepositoryApiResource is required when PortalEnvironmentsPath is not provided."
    }

    Get-RequiredCommand -Name "terraform"

    $repositoryOutputJson = terraform -chdir=$PortalEnvironmentsPath output -json repository_api
    if ([string]::IsNullOrWhiteSpace($repositoryOutputJson)) {
        throw "terraform output for repository_api was empty."
    }

    $repositoryOutput = $repositoryOutputJson | ConvertFrom-Json
    $RepositoryApiResource = $repositoryOutput.application.primary_identifier_uri

    if ([string]::IsNullOrWhiteSpace($RepositoryApiResource)) {
        throw "Unable to resolve repository_api.application.primary_identifier_uri from Terraform output."
    }
}

$configObject = [ordered]@{
    tenantId               = $tenantId
    clientId               = $clientId
    clientSecret           = $clientSecret
    repositoryApiBaseUrl   = $repositoryApiBaseUrl
    repositoryApiResource  = $RepositoryApiResource
    gameServerId           = $GameServerId
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
Write-Host "Repository API base URL: $repositoryApiBaseUrl"
Write-Host "Repository API resource: $RepositoryApiResource"
Write-Host "Refresh interval: $RefreshIntervalSeconds seconds"
Write-Host "Client secret was written to disk; secure this file on the host."
