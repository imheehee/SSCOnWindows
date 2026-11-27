# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory)][ValidateSet('WSL2', 'Qiling')][string]$Engine,
    [Parameter(Mandatory)][string]$Blob,
    [string]$Rootfs
)
$ErrorActionPreference = 'Stop'
$blobPath = (Get-Item -LiteralPath $Blob).FullName
if ([IO.Path]::GetFileName($blobPath) -cne 'libScalable_Encoder.so') {
    throw 'Choose libScalable_Encoder.so from your own device image.'
}
$stream = [IO.File]::OpenRead($blobPath)
try {
    $header = New-Object byte[] 20
    if ($stream.Read($header, 0, 20) -ne 20 -or
        [BitConverter]::ToString($header, 0, 6) -ne '7F-45-4C-46-02-01' -or
        $header[16] -ne 3 -or $header[17] -ne 0 -or $header[18] -ne 183 -or $header[19] -ne 0) {
        throw 'The encoder must be a little-endian ARM64 ELF shared library.'
    }
} finally {
    $stream.Dispose()
}
if ((Read-Host 'Install/replace only the SSC encoder and daemon assets for this user? [y/N]') -notmatch '^[yY]$') {
    return
}

if ($Engine -eq 'Qiling') {
    if (!(Get-Command py -ErrorAction SilentlyContinue)) {
        throw 'Install Python 3.14 from python.org with the py launcher and rerun setup.'
    }
    & py -3.14 -c 'import sys'
    if ($LASTEXITCODE -ne 0) { throw 'Python 3.14 is unavailable from the py launcher.' }
    & py -3.14 -c 'import qiling'
    if ($LASTEXITCODE -ne 0) {
        if ((Read-Host 'Install Qiling for Python 3.14 with pip? [y/N]') -notmatch '^[yY]$') { return }
        & py -3.14 -m pip install --user qiling
        if ($LASTEXITCODE -ne 0) { throw 'pip could not install Qiling. See the pip output above.' }
    }
    if (!$Rootfs) {
        $Rootfs = Join-Path $PSScriptRoot 'ssc_daemon\rootfs'
        if (!(Test-Path -LiteralPath (Join-Path $Rootfs 'lib\ld-linux-aarch64.so.1'))) {
            $Rootfs = Read-Host 'Enter the trusted ARM64 rootfs directory (repository tools\ssc_daemon\rootfs)'
        }
    }
    if (!$Rootfs) { throw 'An ARM64 rootfs is required. See docs\ko\setup.md.' }
    foreach ($file in @('helper/ssc_blob_helper','shims/libc.so','shims/libdl.so','shims/libm.so',
                        'shims/liblog.so','lib/ld-linux-aarch64.so.1','lib/aarch64-linux-gnu/libc.so.6',
                        'lib/aarch64-linux-gnu/libgcc_s.so.1')) {
        if (!(Test-Path -LiteralPath (Join-Path $Rootfs $file) -PathType Leaf)) { throw "Missing rootfs file: $file" }
    }
    $native = Join-Path $env:LOCALAPPDATA 'SSCOnWindows\encoder'
    $target = Join-Path $native 'rootfs'
    $blobDir = Join-Path $target 'blob'
    New-Item -ItemType Directory -Force -Path $native, $target, $blobDir | Out-Null
    foreach ($folder in 'helper','shims','lib') {
        Copy-Item -LiteralPath (Join-Path $Rootfs $folder) -Destination $target -Recurse -Force
    }
    foreach ($file in 'sscblobd.py','qiling_patches.py') {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "ssc_daemon\$file") -Destination $native -Force
    }
    Copy-Item -LiteralPath $blobPath -Destination (Join-Path $blobDir 'libScalable_Encoder.so') -Force
    & py -3.14 (Join-Path $native 'sscblobd.py') --check-managed
    if ($LASTEXITCODE -ne 0) { throw 'Qiling could not import the managed daemon.' }
    Write-Host "Qiling installed at $native. Run the SSC self-test in WinUI."
    return
}

if (!(Get-Command wsl.exe -ErrorAction SilentlyContinue)) { throw 'Install Ubuntu with wsl --install -d Ubuntu, then rerun setup.' }
$wsl = @()
if ($env:SSC_WSL_DISTRO) { $wsl += @('--distribution', $env:SSC_WSL_DISTRO) }
$kernel = (& wsl.exe @wsl --exec uname -r | Out-String)
if ($LASTEXITCODE -ne 0 -or $kernel -notmatch 'WSL2') {
    throw 'Start an Ubuntu WSL2 distribution first. Check wsl --list --verbose and SSC_WSL_DISTRO.'
}
& wsl.exe @wsl --exec bash -c 'command -v gcc >/dev/null && command -v qemu-aarch64 >/dev/null && test -r /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1'
if ($LASTEXITCODE -ne 0) {
    if ((Read-Host 'Install gcc, qemu-user and libc6-arm64-cross in Ubuntu using apt? [y/N]') -notmatch '^[yY]$') { return }
    & wsl.exe @wsl --exec sudo apt-get update
    if ($LASTEXITCODE -ne 0) { throw 'Ubuntu apt update failed.' }
    & wsl.exe @wsl --exec sudo apt-get install gcc qemu-user libc6-arm64-cross
    if ($LASTEXITCODE -ne 0) { throw 'Ubuntu dependency installation failed.' }
}
$payload = Join-Path $PSScriptRoot 'ssc_payload\build_blob'
$tmp = [IO.Path]::GetTempFileName()
try {
    $script = @'
set -eu
payload=$(wslpath -a "$(printf '%s' "$1" | base64 -d)")
blob=$(wslpath -a "$(printf '%s' "$2" | base64 -d)")
target="$HOME/ssc/openssc/build_blob"
mkdir -p "$target" "$HOME/ssc/blob"
for file in ssc_blob_helper libc.so libdl.so liblog.so libm.so; do
    install -m 755 "$payload/$file" "$target/$file"
done
gcc -O2 -o "$target/sscblobd" "$payload/sscblobd.c"
"$target/sscblobd" --check-managed
install -m 644 "$blob" "$HOME/ssc/blob/libScalable_Encoder.so"
'@
    [IO.File]::WriteAllText($tmp, ($script -replace "`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
    $linuxScript = (& wsl.exe @wsl --exec wslpath -a $tmp | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or !$linuxScript) { throw 'Could not map the setup temp file into WSL.' }
    $encodedPayload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payload))
    $encodedBlob = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($blobPath))
    & wsl.exe @wsl --exec bash $linuxScript $encodedPayload $encodedBlob
    if ($LASTEXITCODE -ne 0) { throw 'WSL setup failed. See the command output above.' }
} finally {
    Remove-Item -LiteralPath $tmp -Force
}
Write-Host 'Installed in ~/ssc/blob and ~/ssc/openssc/build_blob. Run the SSC self-test in WinUI.'
