# sign.ps1: Authenticode-sign the program's own executable files with the personal Azure Trusted
# Signing profile, locally.
#
#   powershell -ExecutionPolicy Bypass -File tools\sign.ps1 -Files a.exe, b.dll [-VerifyOnly]
#       [-Account JonB-Dev] [-Profile md-studio-signing] [-Endpoint https://eus.codesigning.azure.net/]
#
# What it needs and where it gets it:
# - signtool, from the newest Windows Kit on the machine.
# - The Trusted Signing client (Azure.CodeSigning.Dlib.dll), fetched once from NuGet into the
#   ignored build output (build-cmake\signing-client\); it needs a .NET 8 runtime.
# - The service principal (AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET): taken from the
#   environment when a vault run injected them (pvault run --bundle <name> -- ...), else read from
#   the personal signing store (~\.config\signing-personal\credentials.json, the personal side of
#   the bare-name-is-work convention) into THIS process's environment for the child signtool.
#   Values are never printed; only the names of the keys that were found.
#
# Signing sends the file's digest to the signing service and receives the signature; the file
# itself does not leave the machine. Prints SIGN_OK <file> per file, or SIGN_FAIL <why>.
#
# Every parameter is named, never positional: a second file passed loose once bound itself to
# -Account, and the service refused a request for an account called SDL2.dll.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)][string[]]$Files,
    [switch]$VerifyOnly,
    [string]$Account = "JonB-Dev",
    [string]$Profile = "md-studio-signing",
    [string]$Endpoint = "https://eus.codesigning.azure.net/",
    [string]$ClientVersion = "1.0.95"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$clientDir = Join-Path $root "build-cmake\signing-client"

function Find-Signtool {
    $kits = "C:\Program Files (x86)\Windows Kits\10\bin"
    $candidates = Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($c in $candidates) {
        $p = Join-Path $c.FullName "x64\signtool.exe"
        if (Test-Path $p) { return $p }
    }
    throw "signtool.exe not found under $kits"
}

function Get-Client {
    $dlib = Get-ChildItem $clientDir -Recurse -Filter "Azure.CodeSigning.Dlib.dll" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*x64*" } | Select-Object -First 1
    if ($dlib) { return $dlib.FullName }
    New-Item -ItemType Directory -Force $clientDir | Out-Null
    $pkg = Join-Path $clientDir "Microsoft.Trusted.Signing.Client.$ClientVersion.zip"
    Write-Output "fetching the Trusted Signing client $ClientVersion from NuGet"
    Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/Microsoft.Trusted.Signing.Client/$ClientVersion" -OutFile $pkg
    Expand-Archive -Path $pkg -DestinationPath (Join-Path $clientDir $ClientVersion) -Force
    $dlib = Get-ChildItem $clientDir -Recurse -Filter "Azure.CodeSigning.Dlib.dll" |
        Where-Object { $_.FullName -like "*x64*" } | Select-Object -First 1
    if (-not $dlib) { throw "the client package has no x64 Azure.CodeSigning.Dlib.dll" }
    return $dlib.FullName
}

function Set-Credentials {
    $names = @("AZURE_TENANT_ID", "AZURE_CLIENT_ID", "AZURE_CLIENT_SECRET")
    $present = $names | Where-Object { -not [string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($_)) }
    if ($present.Count -eq 3) {
        Write-Output "credentials: from the environment (a vault run)"
        return
    }
    $store = Join-Path $env:USERPROFILE ".config\signing-personal\credentials.json"
    if (-not (Test-Path $store)) { throw "no credentials in the environment and no personal signing store at $store" }
    $json = Get-Content $store -Raw | ConvertFrom-Json
    $found = @()
    foreach ($n in $names) {
        $v = $json.$n
        if ([string]::IsNullOrEmpty($v)) { throw "the personal signing store has no $n" }
        [Environment]::SetEnvironmentVariable($n, $v, "Process")
        $found += $n
    }
    Write-Output ("credentials: from the personal signing store (" + ($found -join ", ") + ")")
}

$signtool = Find-Signtool
Write-Output "signtool: $signtool"
$failures = @()

if (-not $VerifyOnly) {
    $dlib = Get-Client
    Write-Output "client: $dlib"
    $metadata = Join-Path $clientDir "metadata.json"
    @{ Endpoint = $Endpoint; CodeSigningAccountName = $Account; CertificateProfileName = $Profile } |
        ConvertTo-Json | Set-Content -Path $metadata -Encoding ascii
    Set-Credentials
    foreach ($f in $Files) {
        if (-not (Test-Path $f)) { $failures += "missing $f"; continue }
        Write-Output "signing $f"
        & $signtool sign /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib $dlib /dmdf $metadata $f
        if ($LASTEXITCODE -ne 0) { $failures += "signtool sign exited $LASTEXITCODE for $f" }
    }
}

foreach ($f in $Files) {
    if (-not (Test-Path $f)) { continue }
    & $signtool verify /pa /q $f
    if ($LASTEXITCODE -ne 0) { $failures += "signtool verify exited $LASTEXITCODE for $f"; continue }
    $sig = Get-AuthenticodeSignature $f
    Write-Output ("SIGN_OK {0}: {1}, signed by {2}" -f $f, $sig.Status, $sig.SignerCertificate.Subject)
}

if ($failures.Count -gt 0) {
    foreach ($x in $failures) { Write-Output "SIGN_FAIL $x" }
    exit 1
}
exit 0
