[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SharedKeyVaultName,

    [Parameter(Mandatory = $true)]
    [string]$GameServerId,

    [Parameter(Mandatory = $false)]
    [string]$IngestBaseUrl,

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

$ingestSubscriptionKey = Get-KeyVaultSecretValue -VaultName $SharedKeyVaultName -SecretName "cod4x-plugin-ingest-subscription-key"

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

if ([string]::IsNullOrWhiteSpace($IngestBaseUrl)) {
    Get-RequiredCommand -Name "terraform"
    $IngestBaseUrl = Get-TerraformOutputValue -Path $PortalEnvironmentsPath -Name "server_events_api" -JsonPath "api_management.endpoint"
}

$configObject = [ordered]@{
    ingestBaseUrl          = $IngestBaseUrl
    ingestSubscriptionKey  = $ingestSubscriptionKey
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
Write-Host "Ingest API base URL: $IngestBaseUrl"
Write-Host "Refresh interval: $RefreshIntervalSeconds seconds"
Write-Host "Ingest subscription key was written to disk; secure this file on the host."
