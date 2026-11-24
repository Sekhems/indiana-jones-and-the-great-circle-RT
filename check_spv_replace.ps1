<#
  Diagnostics for spv_replace on machines where it "did not work".

  Run this next to vulkan-1.dll after starting the game at least once.
  It only READS files: VulkanProxy_diagnostic.log, VulkanProxy.ini, spv_replace\*.

  Typical usage (64-bit PowerShell):
      .\check_spv_replace.ps1
      .\check_spv_replace.ps1 -Exe 'D:\Games\Indiana Jones and the Great Circle\TheGreatCircle.exe'
#>
param(
    [string]$Exe = '',
    [int]$MaxLines = 0
)

$ErrorActionPreference = 'Stop'
$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$log  = Join-Path $root 'VulkanProxy_diagnostic.log'
$ini  = Join-Path $root 'VulkanProxy.ini'
$repl = Join-Path $root 'spv_replace'

function Section($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }

Section 'Files'
foreach ($p in @($log, $ini, $repl)) {
    if (Test-Path $p) { Write-Host ("FOUND    {0}" -f $p) -ForegroundColor Green }
    else              { Write-Host ("MISSING  {0}" -f $p) -ForegroundColor Yellow }
}
$replCount = 0
if (Test-Path $repl) {
    $replCount = @(Get-ChildItem -LiteralPath $repl -Filter 'spv_*.spv' -File -ErrorAction SilentlyContinue).Count
}
Write-Host ("spv_replace files: {0}" -f $replCount)

if (Test-Path $ini) {
    Section 'Effective switches (VulkanProxy.ini)'
    $keys = 'Mode','ForceVirtualRt','DenyFloat16','DenyInt16','DenyDotProduct','DenyMesh',
            'FollowHardwareCaps','StructureMatchFallback','UseReplacements','RewriteRayQuery','TryVariants'
    $text = Get-Content -LiteralPath $ini
    foreach ($k in $keys) {
        $line = $text | Where-Object { $_ -match "^\s*$([regex]::Escape($k))\s*=" } | Select-Object -First 1
        if ($line) { Write-Host ("  {0}" -f $line.Trim()) }
        else       { Write-Host ("  {0}=<absent, default applies>" -f $k) -ForegroundColor DarkGray }
    }
}

if (-not (Test-Path $log)) {
    Write-Host 'No diagnostic log yet: start the game once with this proxy, then re-run.' -ForegroundColor Yellow
    return
}

Section 'This GPU (what the driver really reports)'
$gpu     = Select-String -LiteralPath $log -Pattern 'GPU: name=' | Select-Object -First 1
$profile = Select-String -LiteralPath $log -Pattern 'GPU SHADER PROFILE:' | Select-Object -First 1
$rt      = Select-String -LiteralPath $log -Pattern 'GPU RT PROFILE:' | Select-Object -First 1
foreach ($m in @($gpu, $profile, $rt)) { if ($m) { Write-Host ('  ' + $m.Line.Trim()) } }
if (-not $profile) {
    Write-Host '  (GPU SHADER PROFILE missing: this build did not probe the driver, or the log is from an older DLL)' -ForegroundColor Yellow
}

Section 'Replacement index'
$built = Select-String -LiteralPath $log -Pattern 'structural replacement index built' | Select-Object -Last 1
$found = Select-String -LiteralPath $log -Pattern 'replacement shader file\(s\) found' | Select-Object -Last 1
foreach ($m in @($found, $built)) { if ($m) { Write-Host ('  ' + $m.Line.Trim()) } }

Section 'Replacement results'
$accepted = @(Select-String -LiteralPath $log -Pattern 'shader replacement ACCEPTED')
$struct   = @(Select-String -LiteralPath $log -Pattern 'matched by STRUCTURE')
$rejected = @(Select-String -LiteralPath $log -Pattern 'shader replacement REJECTED')
$misses   = @(Select-String -LiteralPath $log -Pattern 'no replacement for hash=')
Write-Host ("  accepted  : {0}" -f $accepted.Count)
Write-Host ("  by structure: {0}" -f $struct.Count)
Write-Host ("  rejected  : {0}" -f $rejected.Count) -ForegroundColor ($(if ($rejected.Count) {'Yellow'} else {'Gray'}))
Write-Host ("  no match  : {0}" -f $misses.Count)
foreach ($m in ($accepted | Select-Object -First 8)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Green }
foreach ($m in ($struct   | Select-Object -First 8)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Green }
foreach ($m in ($rejected | Select-Object -First 8)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Yellow }

Section 'Shaders the proxy replaced with a stub (fake) module'
$fake = @(Select-String -LiteralPath $log -Pattern '-> FAKE module \(reason=')
Write-Host ("  fake modules: {0}" -f $fake.Count)
foreach ($m in ($fake | Select-Object -First 10)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Yellow }

Section 'First shaders seen (source column shows whether spv_replace was used)'
$shaders = @(Select-String -LiteralPath $log -Pattern 'SHADER #\d+ spv=')
Write-Host ("  SHADER lines: {0}" -f $shaders.Count)
$bySource = $shaders | ForEach-Object {
    if ($_.Line -match 'source=([A-Za-z_]+)') { $Matches[1] } else { 'unknown' }
} | Group-Object | Sort-Object Count -Descending
foreach ($g in $bySource) { Write-Host ("    source={0,-14} {1}" -f $g.Name, $g.Count) }

if ($Exe) {
    Section 'Game executable'
    if (Test-Path $Exe) {
        $dll = Join-Path (Split-Path -Parent $Exe) 'vulkan-1.dll'
        Write-Host ("  exe: {0}" -f $Exe)
        if (Test-Path $dll) {
            $vi = (Get-Item $dll).VersionInfo
            Write-Host ("  vulkan-1.dll next to exe: FileVersion={0} ProductName={1}" -f $vi.FileVersion, $vi.ProductName) -ForegroundColor Green
        } else {
            Write-Host '  vulkan-1.dll NOT found next to the exe: copy the proxy (and spv_replace + ini) there.' -ForegroundColor Yellow
        }
    } else {
        Write-Host '  exe path does not exist.' -ForegroundColor Yellow
    }
}

Section 'What to send in a bug report'
Write-Host @'
  * the "GPU:" + "GPU SHADER PROFILE:" lines above
  * the "SPIRV: structural replacement index built" line
  * the accepted/structure/no-match counters
  * the first "-> FAKE module (reason=...)" lines, if any
  * the game log lines (optional) and whether spv_replace was present next to the DLL
'@
