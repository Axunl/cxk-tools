<#
    sign-windows.ps1 - Authenticode signing for the Windows artifacts of cxk-tools.

    WHY THIS FILE IS PURE ASCII (no Chinese comments, unlike the rest of the repo):
    powershell.exe 5.1 - the default on Windows 10/11 - parses a BOM-less script with
    the local ANSI code page, so UTF-8 Chinese comments turn into mojibake and can
    break parsing further down (this project already hit exactly that with the OCR
    helper, see AGENTS.md). .bat files are ASCII for the same reason. Keep it ASCII.

    WHAT IT SIGNS:
    only our own binaries - cxk-tools.exe, plugins/*.dll and the produced setup.exe.
    Qt / MSVC / onnxruntime DLLs keep whatever signature their vendor shipped: signing
    them with our certificate would replace the vendor's signature with ours, which is
    a false claim about who produced those bytes.

    ENVIRONMENT CONTRACT (all inputs are env vars, so no secret ever shows up on a
    command line that other users of the machine can read through the process list):

      XTOOLS_SIGN_PFX            path to a .pfx holding the code signing key
      XTOOLS_SIGN_PFX_BASE64     same file base64 encoded (CI secret friendly)
      XTOOLS_SIGN_PFX_PASSWORD   password of the .pfx (may be empty)
      XTOOLS_SIGN_THUMBPRINT     use a certificate already in CurrentUser\My
                                 (hardware token / EV certificate case)
      XTOOLS_SIGNTOOL            explicit signtool.exe (default: newest Windows SDK)
      XTOOLS_TIMESTAMP_URL       RFC3161 timestamp server, default DigiCert
      XTOOLS_REQUIRE_SIGNING     1 = "no certificate" is a hard failure, not a skip

    EXIT CODES (package.bat depends on them):
      0  signed and verified (or verified only)
      2  no certificate configured - caller may continue with an unsigned build
      1  anything else: bad PFX, missing signtool, signtool failure, verify failure
#>

[CmdletBinding()]
param(
    # Directory produced by `cmake --install` (+ windeployqt + prune): signs
    # <StageDir>\*.exe and <StageDir>\plugins\*.dll.
    [string]   $StageDir = '',
    # Additional single files, e.g. the installer built after this stage.
    [string[]] $ExtraFile = @(),
    # Only verify the given files (used for setup.exe, which Inno Setup writes).
    [switch]   $VerifyOnly,
    # Local self-signed experiments only: the chain cannot be trusted, so
    # `signtool verify /pa` is doomed to fail and the caller knows it.
    [switch]   $SkipVerify
)

$ErrorActionPreference = 'Stop'

$Script:SigntoolPath = $null
$Script:Thumbprint = $null
$Script:ImportedThumbprint = $null
$Script:TempPfx = $null

function Write-Info { param([string] $Message) Write-Host "[sign] $Message" }
function Write-Warn { param([string] $Message) Write-Host "[sign][warn] $Message" -ForegroundColor Yellow }
function Write-Fail { param([string] $Message) Write-Host "[sign][error] $Message" -ForegroundColor Red }

<#
    Locate signtool.exe. Preference order: explicit env var -> PATH -> newest
    Windows SDK (10.x). Newest-first matters: old SDK builds do not understand
    `/td sha256` and would fail on an otherwise valid certificate.
#>
function Find-SignTool {
    if ($env:XTOOLS_SIGNTOOL) {
        if (Test-Path $env:XTOOLS_SIGNTOOL) { return $env:XTOOLS_SIGNTOOL }
        Write-Warn "XTOOLS_SIGNTOOL points at a missing file: $env:XTOOLS_SIGNTOOL"
    }

    $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $roots = @()
    if ($env:WindowsSdkDir) { $roots += (Join-Path $env:WindowsSdkDir 'bin') }
    $roots += (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin')

    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $versions = Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
            Sort-Object { [version] $_.Name } -Descending
        foreach ($version in $versions) {
            $candidate = Join-Path $version.FullName 'x64\signtool.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    return $null
}

<#
    Resolve the signing certificate to its thumbprint. A PFX is imported into
    CurrentUser\My so that signtool can select it by `/sha1` - that keeps the
    password out of the signtool command line (and therefore out of process
    listings and CI logs). The temporary certificate is removed in the finally
    block at the end of the script.
#>
function Resolve-Signer {
    if ($env:XTOOLS_SIGN_THUMBPRINT) { return $env:XTOOLS_SIGN_THUMBPRINT }

    $pfxPath = $env:XTOOLS_SIGN_PFX
    if (-not $pfxPath -and $env:XTOOLS_SIGN_PFX_BASE64) {
        $Script:TempPfx = Join-Path ([IO.Path]::GetTempPath()) ('cxk-sign-' + [guid]::NewGuid().ToString('N') + '.pfx')
        [IO.File]::WriteAllBytes($Script:TempPfx, [Convert]::FromBase64String($env:XTOOLS_SIGN_PFX_BASE64))
        $pfxPath = $Script:TempPfx
    }
    if (-not $pfxPath) { return $null }
    if (-not (Test-Path $pfxPath)) { throw "PFX file not found: $pfxPath" }

    # ConvertTo-SecureString throws on an empty string, so build the SecureString
    # character by character; an empty password is legitimate for exported PFX.
    $secure = New-Object System.Security.SecureString
    if ($env:XTOOLS_SIGN_PFX_PASSWORD) {
        foreach ($ch in $env:XTOOLS_SIGN_PFX_PASSWORD.ToCharArray()) { $secure.AppendChar($ch) }
    }

    $imported = Import-PfxCertificate -FilePath $pfxPath -CertStoreLocation 'Cert:\CurrentUser\My' -Password $secure
    # A PFX usually carries the whole chain; only the leaf has the private key.
    $leaf = $imported | Where-Object { $_.HasPrivateKey } | Select-Object -First 1
    if (-not $leaf) { throw "PFX contains no certificate with a private key: $pfxPath" }

    $Script:ImportedThumbprint = $leaf.Thumbprint
    Write-Info "imported certificate $($leaf.Subject) (thumbprint $($leaf.Thumbprint))"
    return $leaf.Thumbprint
}

<#
    Sign one PE file. The timestamp is what keeps the signature valid after the
    certificate expires - without it every installer ever published would become
    "untrusted" on the expiry date.

    Retries exist because the public timestamp authorities (DigiCert especially)
    throttle bursts: signing 15 files back to back occasionally yields
    "The specified timestamp server ... returned an invalid response" on a single
    file. Failing the whole release on that would be silly, but shipping a
    signature without a timestamp is worse - so: retry, then fail.
#>
function Invoke-SignOne {
    param([string] $Path, [int] $Attempts = 3)

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        & $Script:SigntoolPath sign /sha1 $Script:Thumbprint /fd sha256 /tr $env:XTOOLS_TIMESTAMP_URL /td sha256 $Path
        if ($LASTEXITCODE -eq 0) { return }
        if ($attempt -lt $Attempts) {
            Write-Warn "sign attempt $attempt failed (exit $LASTEXITCODE): $Path - retrying in 2s"
            Start-Sleep -Seconds 2
        }
    }
    throw "signtool sign failed after $Attempts attempts: $Path"
}

<#
    Verify the embedded signature with the "driver/PE" policy, which is what
    SmartScreen and Windows' own trust UI use. /q keeps the happy path quiet.
#>
function Test-Signature {
    param([string] $Path)

    # $null = ... on purpose: `signtool verify` prints a File/Index/Timestamp table
    # to stdout even with /q, and any non-empty output would make this function
    # return an array that is truthy regardless of the exit code.
    $null = & $Script:SigntoolPath verify /pa /q $Path
    return ($LASTEXITCODE -eq 0)
}

function Invoke-Verify {
    param([string[]] $Paths)

    $failed = @()
    foreach ($path in $Paths) {
        if (Test-Signature -Path $path) {
            Write-Info "verified $path"
        } else {
            Write-Fail "signature check failed: $path"
            $failed += $path
        }
    }
    return $failed
}

try {
    if (-not $env:XTOOLS_TIMESTAMP_URL) { $env:XTOOLS_TIMESTAMP_URL = 'http://timestamp.digicert.com' }

    $Script:SigntoolPath = Find-SignTool
    if (-not $Script:SigntoolPath) {
        Write-Fail 'signtool.exe not found: install the Windows SDK signing tools, or set XTOOLS_SIGNTOOL'
        exit 1
    }

    if ($VerifyOnly) {
        if (-not $ExtraFile -or $ExtraFile.Count -eq 0) {
            Write-Fail 'VerifyOnly needs at least one -ExtraFile'
            exit 1
        }
        $missing = @($ExtraFile | Where-Object { -not (Test-Path $_) })
        if ($missing.Count -gt 0) {
            Write-Fail ("file to verify does not exist: " + ($missing -join ', '))
            exit 1
        }
        $failed = @(Invoke-Verify -Paths $ExtraFile)
        if ($failed.Count -gt 0) { exit 1 }
        exit 0
    }

    $Script:Thumbprint = Resolve-Signer
    if (-not $Script:Thumbprint) {
        if ($env:XTOOLS_REQUIRE_SIGNING -eq '1') {
            Write-Fail 'XTOOLS_REQUIRE_SIGNING=1 but no signing certificate is configured'
            Write-Fail 'set XTOOLS_SIGN_PFX_BASE64 + XTOOLS_SIGN_PFX_PASSWORD (CI) or XTOOLS_SIGN_PFX / XTOOLS_SIGN_THUMBPRINT'
            exit 1
        }
        Write-Warn 'no signing certificate configured (XTOOLS_SIGN_PFX / XTOOLS_SIGN_PFX_BASE64 / XTOOLS_SIGN_THUMBPRINT)'
        Write-Warn 'the artifacts will be unsigned: Windows SmartScreen will warn on first run'
        exit 2
    }

    $targets = New-Object System.Collections.Generic.List[string]
    if ($StageDir) {
        if (-not (Test-Path $StageDir)) {
            Write-Fail "stage directory not found: $StageDir"
            exit 1
        }
        Get-ChildItem -Path $StageDir -Filter '*.exe' -File | ForEach-Object { $targets.Add($_.FullName) }
        $pluginDir = Join-Path $StageDir 'plugins'
        if (Test-Path $pluginDir) {
            Get-ChildItem -Path $pluginDir -Filter '*.dll' -File | ForEach-Object { $targets.Add($_.FullName) }
        }
    }
    foreach ($file in $ExtraFile) {
        if (-not (Test-Path $file)) {
            Write-Fail "file to sign does not exist: $file"
            exit 1
        }
        $targets.Add((Resolve-Path $file).Path)
    }

    if ($targets.Count -eq 0) {
        Write-Warn 'nothing to sign (empty stage directory?)'
        exit 1
    }

    Write-Info "signing $($targets.Count) file(s) with $($Script:SigntoolPath)"
    foreach ($target in $targets) { Invoke-SignOne -Path $target }

    if (-not $SkipVerify) {
        $failed = @(Invoke-Verify -Paths $targets.ToArray())
        if ($failed.Count -gt 0) { exit 1 }
    } else {
        Write-Warn 'signature verification skipped on purpose (-SkipVerify)'
    }

    Write-Info "done: $($targets.Count) file(s) signed"
    exit 0
}
catch {
    Write-Fail $_.Exception.Message
    exit 1
}
finally {
    # Leave no private key behind, and no PFX in %TEMP% - CI runners are shared
    # enough, and a developer machine even more so.
    if ($Script:ImportedThumbprint) {
        Remove-Item -Path "Cert:\CurrentUser\My\$($Script:ImportedThumbprint)" -Force -ErrorAction SilentlyContinue
        Write-Info 'temporary certificate removed from the user store'
    }
    if ($Script:TempPfx -and (Test-Path $Script:TempPfx)) {
        Remove-Item -Path $Script:TempPfx -Force -ErrorAction SilentlyContinue
    }
}
