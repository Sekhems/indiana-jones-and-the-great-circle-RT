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
    [int]$MaxLines = 0,
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'
$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$log  = if ($Log) { $Log } else { Join-Path $root 'VulkanProxy_diagnostic.log' }
if (-not (Test-Path $log)) {
    # The log is written next to vulkan-1.dll, which is not always this folder.
    $probe = @(Get-ChildItem -LiteralPath (Split-Path -Parent $root) -Filter 'VulkanProxy_diagnostic.log' -Recurse -ErrorAction SilentlyContinue |
               Sort-Object LastWriteTime -Descending | Select-Object -First 2)
    if ($probe.Count -ge 2 -and $probe[0].LastWriteTime -ne $probe[1].LastWriteTime) {
        $log = $probe[0].FullName
        Write-Host ("NOTE: no log here; using the newest one found under {0}:" -f (Split-Path -Parent $root)) -ForegroundColor Yellow
        Write-Host ("      {0}  ({1:yyyy-MM-dd HH:mm})" -f $log, $probe[0].LastWriteTime) -ForegroundColor Yellow
    }
}
$ini  = Join-Path $root 'VulkanProxy.ini'
$repl = Join-Path $root 'spv_replace'

function Section($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }

Section 'Files'
if (Test-Path $log) {
    $age = (Get-Date) - (Get-Item $log).LastWriteTime
    $hdr = Select-String -LiteralPath $log -Pattern '=== VulkanProxy' | Select-Object -First 1
    Write-Host ("log: {0}" -f $log)
    Write-Host ("     last written {0:yyyy-MM-dd HH:mm} ({1:N1} h ago)" -f (Get-Item $log).LastWriteTime, $age.TotalHours)
    if ($hdr) { Write-Host ('     ' + $hdr.Line.Trim()) }
    if ($age.TotalHours -gt 24) {
        Write-Host '     WARNING: this log is older than a day. If you rebuilt the DLL since, this is NOT your current build.' -ForegroundColor Red
    }
}
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
            'FollowHardwareCaps','StructureMatchFallback','FuzzyMatch','SelfTest','UseReplacements','RewriteRayQuery','TryVariants'
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
$built = Select-String -LiteralPath $log -Pattern 'replacement index: \d+ file' | Select-Object -Last 1
$found = Select-String -LiteralPath $log -Pattern 'replacement shader file\(s\) found' | Select-Object -Last 1
foreach ($m in @($found, $built)) { if ($m) { Write-Host ('  ' + $m.Line.Trim()) } }

Section 'Replacement results'
$accepted = @(Select-String -LiteralPath $log -Pattern 'shader replacement ACCEPTED')
$struct   = @(Select-String -LiteralPath $log -Pattern 'shader replacement matched \(')
$rejected = @(Select-String -LiteralPath $log -Pattern 'shader replacement REJECTED')
$misses   = @(Select-String -LiteralPath $log -Pattern 'no replacement for hash=')
$noMatchAll = ($accepted.Count -eq 0 -and $misses.Count -gt 0)
# A stale log (old DLL, old run) makes every conclusion below wrong. Say so loudly and
# stop treating its numbers as a verdict on the current build.
$logAge = if (Test-Path $log) { (Get-Date) - (Get-Item $log).LastWriteTime } else { [TimeSpan]::MaxValue }
$logStale = ($logAge.TotalHours -gt 24)
Write-Host ("  accepted  : {0}" -f $accepted.Count)
Write-Host ("  by similarity: {0}" -f $struct.Count)
Write-Host ("  rejected  : {0}" -f $rejected.Count) -ForegroundColor ($(if ($rejected.Count) {'Yellow'} else {'Gray'}))
Write-Host ("  no match  : {0}" -f $misses.Count)
foreach ($m in ($accepted | Select-Object -First 8)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Green }
foreach ($m in ($struct   | Select-Object -First 8)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Green }
foreach ($m in ($rejected | Select-Object -First 8)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Yellow }
if ($noMatchAll -and $logStale) {
    Write-Host '  All lookups in THIS LOG missed, but the log is over a day old (see the Files section).' -ForegroundColor Yellow
    Write-Host '  Rebuild the DLL, run the game once, then run this script again - the numbers above are not from the current build.' -ForegroundColor Yellow
} elseif ($noMatchAll) {
    Write-Host '  All lookups missed: no file in spv_replace is similar to the shaders this GPU compiles.' -ForegroundColor Red
    Write-Host '    - check that spv_replace sits next to the DLL and holds spv_<16 hex>.spv files' -ForegroundColor Yellow
    Write-Host '    - check the "replacement index" line above: candidates=0 means none of the files could be read' -ForegroundColor Yellow
    Write-Host '    - FuzzyMatch=0 in VulkanProxy.ini disables similarity matching; this build needs it at 1' -ForegroundColor Yellow
}

Section 'Verdict on THIS machine (SPIRV SUMMARY / replacement ACCEPTED)'
$summaries = @(Select-String -LiteralPath $log -Pattern 'SPIRV SUMMARY:')
$liveHits  = @(Select-String -LiteralPath $log -Pattern 'SPIRV: replacement ACCEPTED \(')
$structural = @(Select-String -LiteralPath $log -Pattern 'STRUCTURAL matching is in use')
$hashHits   = @(Select-String -LiteralPath $log -Pattern 'ACCEPTED \(exact hash, not structure\)')
Write-Host ("  accepted via byte hash : {0}" -f $hashHits.Count)
Write-Host ("  accepted via structure  : {0}" -f $structural.Count) -ForegroundColor ($(if ($structural.Count) {'Green'} else {'Gray'}))
if ($summaries.Count) {
    foreach ($m in ($summaries | Select-Object -Last 3)) { Write-Host ('  ' + $m.Line.Trim()) }
} else {
    Write-Host '  no SPIRV SUMMARY line: the game was closed before it created 50 shader modules, or it was closed hard.' -ForegroundColor Yellow
}
Write-Host ("  per-lookup accept lines: {0}" -f $liveHits.Count) -ForegroundColor ($(if ($liveHits.Count) {'Green'} else {'Yellow'}))
foreach ($m in ($liveHits | Select-Object -First 5)) { Write-Host ('    ' + $m.Line.Trim()) -ForegroundColor Green }
$portable = @($summaries | Where-Object { $_.Line -match 'PORTABLE PATH USED' })
$hashOnly = @($summaries | Where-Object { $_.Line -match 'accepted by exact hash only' })
$nothing  = @($summaries | Where-Object { $_.Line -match 'NOTHING ACCEPTED' })
if ($structural.Count -or $portable.Count) {
    Write-Host '  => RESULT: the STRUCTURAL path was used on this card - the same path another GPU takes.' -ForegroundColor Green
} elseif ($hashHits.Count) {
    Write-Host '  => RESULT: replacements are ACCEPTED, but by byte hash only - on this card the game' -ForegroundColor Yellow
    Write-Host '     builds these modules exactly like the reference machine, so the structural path was' -ForegroundColor Yellow
    Write-Host '     never needed. The folder is still portable (see the self-test above).' -ForegroundColor Yellow
} elseif ($hashOnly.Count) {
    Write-Host '  => RESULT: only exact hashes matched, structural matching never fired on this card. Send the log.' -ForegroundColor Yellow
} elseif ($nothing.Count) {
    Write-Host '  => RESULT: shaders were processed but no replacement was accepted. Send the log with the no-match lines.' -ForegroundColor Red
}

Section 'Per-stage result (what the folder served, what is left)'
$table = @(Select-String -LiteralPath $log -Pattern 'SPIRV SUMMARY:   ')
if ($table.Count) {
    foreach ($m in ($table | Select-Object -Last 10)) { Write-Host ('  ' + ($m.Line -replace '^\[\d+\] SPIRV SUMMARY:\s+','')) }
    $action = @($table | Where-Object { $_.Line -match 'ACTION' })
    if ($action.Count) {
        Write-Host '  => There ARE modules that almost matched and were rejected by the thresholds.' -ForegroundColor Yellow
        Write-Host '     Set DumpLookupInput=8 in VulkanProxy.ini, run the game once, and send spv_input_*.spv.' -ForegroundColor Yellow
    } else {
        Write-Host '  => Nothing was rejected: the folder simply has no module for the missing stages.' -ForegroundColor Green
    }
} else {
    Write-Host '  no per-stage table yet: it is printed after 200 lookups and needs the newest DLL.' -ForegroundColor DarkGray
}

Section 'Universal matching self-test (SelfTest=1 in the ini)'
$selfLines = @(Select-String -LiteralPath $log -Pattern 'SELFTEST')
if ($selfLines.Count -eq 0) {
    Write-Host '  not run (SelfTest=0). Set SelfTest=1 to verify on THIS card that the folder is portable.' -ForegroundColor DarkGray
} else {
    foreach ($m in ($selfLines | Select-Object -First 30)) { Write-Host ('  ' + $m.Line.Trim()) }
    $passed = @($selfLines | Where-Object { $_.Line -match 'SELFTEST PASSED' })
    $failed = @($selfLines | Where-Object { $_.Line -match 'SELFTEST FAILED' })
    if ($passed.Count) { Write-Host '  Result: PASSED - replacements are found even when the module is rebuilt like another driver would.' -ForegroundColor Green }
    elseif ($failed.Count) { Write-Host '  Result: FAILED - attach this log, the folder is not portable with the current build.' -ForegroundColor Red }
}

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
