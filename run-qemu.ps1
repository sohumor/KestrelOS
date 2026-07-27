<#
.SYNOPSIS
    Boot the KestrelOS disk image in QEMU on Windows.

.DESCRIPTION
    Locates qemu-system-x86_64 on PATH or in the usual "Program Files\qemu"
    install location, prints the exact command line it is about to run, and
    launches the image.

    Networking is an RTL8139 NIC on QEMU user-mode networking; the kernel
    configures itself statically for that setup (10.0.2.15/24, gateway
    10.0.2.2, DNS 10.0.2.3) -- see kernel/net.c.

    The image itself must be built from Linux/WSL2 ("make"); this script
    only runs an image that already exists.

.PARAMETER Headless
    Do not open a VGA window. The serial console on stdio is the only UI.

.PARAMETER Image
    Disk image to boot. Default: build\os.img relative to this script.

.PARAMETER Memory
    Guest RAM in QEMU syntax. Default: 256M.

.PARAMETER ExtraArgs
    Any remaining arguments are appended to the QEMU command line.

.EXAMPLE
    .\run-qemu.ps1

.EXAMPLE
    .\run-qemu.ps1 -Headless

.EXAMPLE
    .\run-qemu.ps1 -Memory 512M -Image build\os.img
#>

[CmdletBinding()]
param(
    [switch]$Headless,
    [string]$Image,
    [string]$Memory = '256M',
    [string]$Nic = 'rtl8139',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-Qemu {
    $cmd = Get-Command 'qemu-system-x86_64.exe' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $cmd = Get-Command 'qemu-system-x86_64' -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = @(
        'C:\Program Files\qemu\qemu-system-x86_64.exe',
        'C:\Program Files (x86)\qemu\qemu-system-x86_64.exe',
        'C:\qemu\qemu-system-x86_64.exe'
    )
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if ($root) { $candidates += (Join-Path $root 'qemu\qemu-system-x86_64.exe') }
    }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c -PathType Leaf)) { return $c }
    }
    return $null
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Image) {
    $Image = Join-Path $scriptDir 'build\os.img'
}

$qemu = Find-Qemu
if (-not $qemu) {
    Write-Host 'run-qemu: qemu-system-x86_64 was not found.' -ForegroundColor Red
    Write-Host ''
    Write-Host 'Install QEMU for Windows from https://www.qemu.org/download/#windows'
    Write-Host 'or with a package manager:'
    Write-Host '    winget install SoftwareFreedomConservancy.QEMU'
    Write-Host '    choco install qemu'
    Write-Host ''
    Write-Host 'Then add its install directory (usually C:\Program Files\qemu)'
    Write-Host 'to PATH, or re-run this script from that directory.'
    exit 1
}

if (-not (Test-Path -LiteralPath $Image -PathType Leaf)) {
    Write-Host "run-qemu: image not found: $Image" -ForegroundColor Red
    Write-Host ''
    Write-Host 'Build it first from WSL2/Linux at the repo root:'
    Write-Host '    make'
    Write-Host 'From PowerShell that is:'
    Write-Host '    wsl -d Ubuntu -- make'
    exit 1
}

$qemuArgs = @(
    '-drive', "file=$Image,format=raw",
    '-m', $Memory,
    '-no-reboot',
    '-device', "$Nic,netdev=n0",
    '-netdev', 'user,id=n0',
    '-serial', 'stdio'
)

if ($Headless) {
    $qemuArgs += @('-display', 'none')
}

if ($ExtraArgs) {
    $qemuArgs += $ExtraArgs
}

# Quote only the arguments that need it, so the printed line is copy-pastable.
$printable = $qemuArgs | ForEach-Object {
    if ($_ -match '[\s]') { '"' + $_ + '"' } else { $_ }
}
Write-Host ('+ "' + $qemu + '" ' + ($printable -join ' ')) -ForegroundColor Cyan

& $qemu @qemuArgs
exit $LASTEXITCODE
