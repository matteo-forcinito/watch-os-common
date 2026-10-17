<#
Dev releases from a `dev` branch, then promotion to stable: see docs/guida/09-release-firmware.md.

  Dev (the devices on the Dev channel get it):
    tools\dev\release_dev.ps1 -Version 1.8.6-dev.1 -Notes C:\path\notes.md
  Stable (dev merged into main everywhere, same code as the last dev):
    tools\dev\release_dev.ps1 -Version 1.8.6 -Notes C:\path\notes.md -Promote

What it does:
  1. watch-os-common: the current commit (dev) or origin/dev (promote) is pushed to `dev`
     (and fast-forwarded onto `main` when promoting).
  2. Every firmware, in a temporary worktree (your folders and their local changes are not
     touched): starts from origin/dev (origin/main merged in) or origin/main, points the
     submodule at that library commit, writes release/notes/<version>.md and PROJECT_VER,
     commits and pushes to `dev` (promote: to `dev` and `main`).
  3. Publishes all of them in parallel with release_firmware.py, a log per firmware in %TEMP%.

  -Only amoled,knob     just some products       -NoPublish   prepare and push, no release
#>
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(-dev\.[0-9]+)?$')][string]$Version,
    [Parameter(Mandatory=$true)][string]$Notes,
    [switch]$Promote,
    [switch]$NoPublish,
    [string[]]$Only,
    [string]$Gh = 'C:\Users\matte\Documents\ESP-IDF\ESP32-S3-Knob-Touch-LCD-1.8\.companion-tools\gh\bin\gh.exe'
)
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\matte\Documents\ESP-IDF'
$common = Join-Path $root 'watch-os-common'
$firmwares = [ordered]@{
    amoled     = 'ESP32-S3-Amoled-Touch-1.8'
    amoled175  = 'ESP32-S3-Touch-AMOLED-1.75'
    amoled175c = 'ESP32-S3-Touch-AMOLED-1.75C'
    knob       = 'ESP32-S3-Knob-Touch-LCD-1.8'
    watcher    = 'SenseCap-Watcher-W1-A'
}
if ($Only) { foreach ($p in @($firmwares.Keys)) { if ($Only -notcontains $p) { $firmwares.Remove($p) } } }
if ($firmwares.Count -eq 0) { throw 'No firmware selected' }
$isDev = $Version -match '-dev\.'
if ($Promote -and $isDev) { throw 'Promote needs a stable version (1.8.6), not a dev one' }
if (-not $Promote -and -not $isDev) { throw 'A stable version is published only with -Promote (from the dev branch)' }
$Notes = (Resolve-Path -LiteralPath $Notes).Path
$first = (Get-Content -LiteralPath $Notes -Encoding UTF8 | Where-Object { $_.Trim() } | Select-Object -First 1)
if (-not $first) { throw 'The notes are empty' }
if ($first.Length -gt 79) { Write-Warning "First line of the notes over 79 characters: the watch cuts it" }
$branch = if ($Promote) { 'main' } else { 'dev' }

# git writes progress to stderr even on success: in Windows PowerShell that must not stop the script.
function Git([string]$dir) {
    $ErrorActionPreference = 'Continue'
    $out = & git.exe -C $dir @args 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') in ${dir}: $out" }
    $out
}
function HasRef([string]$dir, [string]$ref) {
    $ErrorActionPreference = 'Continue'
    & git.exe -C $dir rev-parse -q --verify $ref *> $null
    $LASTEXITCODE -eq 0
}

# 1. The library.
Git $common fetch -q origin | Out-Null
if ($Promote) {
    if (-not (HasRef $common 'origin/dev')) { throw 'watch-os-common has no dev branch to promote' }
    $lib = (Git $common rev-parse origin/dev).Trim()
    Git $common push -q origin "${lib}:refs/heads/main" | Out-Null   # fast-forward only
} else {
    if (Git $common status --porcelain --untracked-files=no) { throw 'watch-os-common has uncommitted changes: commit them first' }
    $lib = (Git $common rev-parse HEAD).Trim()
    Git $common push -q origin "${lib}:refs/heads/dev" | Out-Null    # fails if dev went elsewhere
}
Write-Host "watch-os-common $($lib.Substring(0,7)) -> $(if ($Promote) { 'dev, main' } else { 'dev' })"

# 2. Every firmware, in a worktree.
$work = Join-Path $root '.release-work'
New-Item -ItemType Directory -Force $work | Out-Null
$trees = @{}
foreach ($product in $firmwares.Keys) {
    $repo = Join-Path $root $firmwares[$product]
    $tree = Join-Path $work $firmwares[$product]
    if (Test-Path $tree) { try { Git $repo worktree remove --force $tree | Out-Null } catch { Remove-Item -Recurse -Force $tree } }
    Git $repo worktree prune | Out-Null
    Git $repo fetch -q origin | Out-Null
    $hasDev = HasRef $repo 'origin/dev'
    $base = if ($hasDev) { 'origin/dev' } else { 'origin/main' }
    Git $repo worktree add -q --detach $tree $base | Out-Null
    if ($hasDev) {
        # Keep dev mergeable: whatever reached main meanwhile comes in first.
        Git $tree merge -q --no-edit origin/main | Out-Null
    }
    Git $tree update-index --cacheinfo "160000,$lib,watch-os-common" | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $tree 'release\notes') | Out-Null
    Copy-Item -LiteralPath $Notes (Join-Path $tree "release\notes\$Version.md") -Force
    $cmake = Join-Path $tree 'CMakeLists.txt'
    $text = [IO.File]::ReadAllText($cmake)
    # Only the numeric line: the "$ENV{FW_VERSION}" one stays as it is.
    $text = [regex]::Replace($text, 'set\(PROJECT_VER "\d[^"]*"\)', "set(PROJECT_VER `"$Version`")")
    [IO.File]::WriteAllText($cmake, $text, (New-Object Text.UTF8Encoding $false))
    Git $tree add watch-os-common "release/notes/$Version.md" CMakeLists.txt | Out-Null
    $msg = "Release ${Version}: $first`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
    Git $tree commit -q -m $msg | Out-Null
    Git $tree push -q origin 'HEAD:refs/heads/dev' | Out-Null
    if ($Promote) { Git $tree push -q origin 'HEAD:refs/heads/main' | Out-Null }
    $trees[$product] = $tree
    Write-Host "$($firmwares[$product]) -> $branch $((Git $tree rev-parse --short HEAD).Trim())"
}
if ($NoPublish) { Write-Host 'Pushed, not published (-NoPublish). Worktrees in' $work; return }

# 3. Publish in parallel (15-25 minutes).
$script = Join-Path $common 'tools\release_firmware.py'
$jobs = foreach ($product in $firmwares.Keys) {
    $name = $firmwares[$product]
    Start-Job -Name $product -ArgumentList $script, $name, $product, $Version, $trees[$product], $branch, $Gh -ScriptBlock {
        param($script, $name, $product, $version, $tree, $branch, $gh)
        $log = Join-Path $env:TEMP "release-$product.log"
        & python $script --repo "matteo-forcinito/$name" --product $product --version $version --project $tree `
            --source-ref $branch --gh $gh --output (Join-Path $env:TEMP 'release-output') *> $log
        "$product exit=$LASTEXITCODE"
    }
}
$jobs | Wait-Job | Receive-Job
$failed = $false
foreach ($product in $firmwares.Keys) {
    $log = Join-Path $env:TEMP "release-$product.log"
    $last = Get-Content $log -Tail 1
    Write-Host "== $product : $last"
    if ($last -notmatch '^PUBLISHED:') { $failed = $true }
    else { Git (Join-Path $root $firmwares[$product]) worktree remove --force $trees[$product] | Out-Null }
}
if ($failed) { throw "Some releases failed: logs in $env:TEMP\release-<product>.log, worktrees kept in $work" }
