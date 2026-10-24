# Standalone Windows build: Godot export + the repo data the app reads at run time (see app/README.md -> Export).
#   powershell -ExecutionPolicy Bypass -File app/tools/export.ps1            # release -> dist/FlightBench/
#   powershell -ExecutionPolicy Bypass -File app/tools/export.ps1 -Debug     # debug   -> dist/FlightBench-debug/
#   ... -DataOnly                                                            # only refresh <dist>/data/
#   ... -Presets 'rs7-first-flight'                                         # release profile: these presets + their firmware only (v0.1)
#   ... -Zip                                                                 # also pack <dist> as dist/FlightBench-win64-v<version>.zip
# Godot: -Godot <console exe>, else $env:GODOT, else the default engine path below.
# Every build gets LICENSE (the repo's) and README.txt (tools/dist-README.txt with version / commit / date filled in)
# next to the exe: the zip is a GPL distribution of QEMU and of FlightBench, so the licence text and a pointer to the
# source travel with the binaries. -Zip writes the entries with '/' separators and a top-level FlightBench/ folder
# (Compress-Archive on Windows PowerShell 5.1 writes '\' separators, which non-Windows unzip tools keep as file names).
# data/ is rebuilt from scratch each run and holds only what the app reads from boards/ and models/:
#   boards/<id>/*.config, *.cli, board.json, README.md   (*.cli: a CLI board's script, e.g. speedybee-f405-wing/defaults.cli)
#   models/<id>/aircraft.json, *.cli, README.md, visual/*.tscn, env/*.json, **/*profile*.txt
#                                      (*.cli: the aircraft's firmware CLI part; env/: the plane's scenarios, the core reads them)
#   presets/*.json, README.md          (the bundled benches: Main menu -> Presets)
#   firmware/**                        (the firmware the presets point at + FIRMWARE.md: source / licence)
# (the visual C# classes are compiled into FlightBench.dll; SITL tools are not shipped).
# The core: data/core/ = every file of -CoreDir (qemu-system-arm.exe + its DLLs, self-contained) when that
# folder has qemu-system-arm.exe; default $env:CORE_DIR, else <repo>\dist\core (where core/qemu/tools/package-win.sh puts it).
# Without it the build has no core (the app then starts none: Settings -> Paths -> core, or a remote core).
param(
    [switch]$Debug,
    [switch]$DataOnly,
    [switch]$Zip,
    # release profile: only the presets matching these names (wildcards, e.g. 'rs7-*') and the firmware they point at;
    # default: every preset and every firmware file
    [string[]]$Presets = @('*'),
    [string]$CoreDir = $(if ($env:CORE_DIR) { $env:CORE_DIR } else { Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'dist\core' }),
    # the Godot 4.7 .NET console exe: -Godot, else $env:GODOT, else `godot` on PATH
    [string]$Godot = $(if ($env:GODOT) { $env:GODOT } else { 'godot' })
)
$ErrorActionPreference = 'Stop'
$app  = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $app
if ($Debug) { $preset = 'Windows Desktop (debug)'; $out = Join-Path $repo 'dist\FlightBench-debug'; $mode = '--export-debug' }
else        { $preset = 'Windows Desktop';         $out = Join-Path $repo 'dist\FlightBench';       $mode = '--export-release' }

if (-not $DataOnly) {
    if (-not (Get-Command $Godot -ErrorAction SilentlyContinue)) { throw "Godot console exe not found: $Godot (pass -Godot or set GODOT to the Godot 4.7 .NET console exe)" }
    New-Item -ItemType Directory -Force $out | Out-Null
    $exe = Join-Path $out 'FlightBench.exe'
    & $Godot --headless --path $app $mode $preset $exe
    if ($LASTEXITCODE -ne 0) { throw "export failed ($LASTEXITCODE)" }
    if (-not (Test-Path $exe)) { throw "export produced no $exe" }
}

$data = Join-Path $out 'data'
if (Test-Path $data) { Remove-Item -Recurse -Force $data }
function Copy-Rel([string]$src) {
    $rel = $src.Substring($repo.Length).TrimStart('\')
    $dst = Join-Path $data $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $src -Destination $dst
}
foreach ($b in Get-ChildItem -Directory (Join-Path $repo 'boards') | Where-Object { -not $_.Name.StartsWith('.') }) {
    Get-ChildItem -File $b.FullName | Where-Object { $_.Extension -in '.config', '.cli' -or $_.Name -in 'board.json', 'README.md' } | ForEach-Object { Copy-Rel $_.FullName }
}
foreach ($m in Get-ChildItem -Directory (Join-Path $repo 'models') | Where-Object { -not $_.Name.StartsWith('.') }) {
    Get-ChildItem -File $m.FullName | Where-Object { $_.Name -in 'aircraft.json', 'README.md' -or $_.Extension -eq '.cli' } | ForEach-Object { Copy-Rel $_.FullName }
    $envDir = Join-Path $m.FullName 'env'
    if (Test-Path $envDir) { Get-ChildItem -File $envDir -Filter '*.json' | ForEach-Object { Copy-Rel $_.FullName } }
    $vis = Join-Path $m.FullName 'visual'
    if (Test-Path $vis) { Get-ChildItem -File $vis -Filter '*.tscn' | ForEach-Object { Copy-Rel $_.FullName } }
    Get-ChildItem -File -Recurse $m.FullName -Filter '*profile*.txt' | ForEach-Object { Copy-Rel $_.FullName }
}
# the presets and the firmware they name (paths in a preset are relative to the data root = this data/).
# -Presets filters the preset files by name (without .json); the firmware folder then keeps FIRMWARE.md and only the
# files a kept preset names (the v0.1 release profile: -Presets 'rs7-first-flight' ships the stock-hex RS7 bench alone).
$keepFw = @{}
$allPresets = $Presets -contains '*'
foreach ($sub in 'presets', 'firmware') {
    $dir = Join-Path $repo $sub
    if (-not (Test-Path $dir)) { Write-Host "${sub}: none ($dir not found)"; continue }
    $sf = Get-ChildItem -File -Recurse $dir | Where-Object { $sub -ne 'presets' -or $_.Extension -eq '.json' -or $_.Name -eq 'README.md' }
    if ($sub -eq 'presets' -and -not $allPresets) {
        $sf = $sf | Where-Object { $f = $_; $f.Extension -ne '.json' -or ($Presets | Where-Object { $f.BaseName -like $_ }).Count -gt 0 }
        $left = (Get-ChildItem -File $dir -Filter '*.json' | Where-Object { $f = $_; ($Presets | Where-Object { $f.BaseName -like $_ }).Count -eq 0 })
        if ($left) { Write-Host "presets: not shipped (-Presets $($Presets -join ',')): $(($left | ForEach-Object { $_.Name }) -join ', ')" }
        if (($sf | Where-Object { $_.Extension -eq '.json' }).Count -eq 0) { throw "presets: -Presets $($Presets -join ',') matches no preset in $dir" }
    }
    if ($sub -eq 'presets') {
        foreach ($pf in ($sf | Where-Object { $_.Extension -eq '.json' })) {
            $pj = Get-Content -Raw -Encoding UTF8 $pf.FullName | ConvertFrom-Json
            if (-not [string]::IsNullOrWhiteSpace($pj.firmwarePath)) { $keepFw[(($pj.firmwarePath -replace '\\', '/') -replace '^\./', '')] = $pf.Name }
        }
    }
    if ($sub -eq 'firmware' -and -not $allPresets) {
        $drop = $sf | Where-Object { $f = $_; $rel = ($f.FullName.Substring($repo.Length).TrimStart('\') -replace '\\', '/'); $f.Name -ne 'FIRMWARE.md' -and -not $keepFw.ContainsKey($rel) }
        if ($drop) { Write-Host "firmware: not shipped (no kept preset names it): $(($drop | ForEach-Object { $_.FullName.Substring($dir.Length + 1) }) -join ', ')" }
        $sf = $sf | Where-Object { $f = $_; $rel = ($f.FullName.Substring($repo.Length).TrimStart('\') -replace '\\', '/'); $f.Name -eq 'FIRMWARE.md' -or $keepFw.ContainsKey($rel) }
    }
    $sf | ForEach-Object { Copy-Rel $_.FullName }
    $skb = [math]::Round((($sf | Measure-Object Length -Sum).Sum) / 1KB, 1)
    Write-Host "${sub}: bundled $($sf.Count) files, $skb KB -> $(Join-Path $data $sub)"
    $sf | ForEach-Object { Write-Host ("  {0,10}  {1}" -f $_.Length, $_.FullName.Substring($dir.Length + 1)) }
}
# every relative path a preset names must be in data/ now
foreach ($pf in Get-ChildItem -File (Join-Path $data 'presets') -Filter '*.json' -ErrorAction SilentlyContinue) {
    $pj = Get-Content -Raw -Encoding UTF8 $pf.FullName | ConvertFrom-Json
    foreach ($k in 'firmwarePath', 'boardConfigFile', 'profileFile', 'visual') {
        $v = $pj.$k
        if ([string]::IsNullOrWhiteSpace($v)) { continue }
        if (-not (Test-Path (Join-Path $data $v))) { throw "preset $($pf.Name): $k '$v' is not in $data" }
    }
    Write-Host "  preset $($pf.Name): '$($pj.name)' - paths present in data/"
}
# the core (the app's CoreLocator looks for <exe dir>/data/core/qemu-system-arm.exe first)
$coreExe = Join-Path $CoreDir 'qemu-system-arm.exe'
if (Test-Path $coreExe) {
    $coreOut = Join-Path $data 'core'
    New-Item -ItemType Directory -Force $coreOut | Out-Null
    Copy-Item -Path (Join-Path $CoreDir '*') -Destination $coreOut -Recurse
    $cf = Get-ChildItem -File -Recurse $coreOut
    $cmb = [math]::Round((($cf | Measure-Object Length -Sum).Sum) / 1MB, 1)
    Write-Host "core: bundled $($cf.Count) files, $cmb MB from $CoreDir -> $coreOut"
    $cf | ForEach-Object { Write-Host ("  {0,10}  {1}" -f $_.Length, $_.FullName.Substring($coreOut.Length + 1)) }
    $manifest = Join-Path $coreOut 'CORE-MANIFEST.txt'
    if (Test-Path $manifest) { Get-Content $manifest | Select-Object -First 4 | ForEach-Object { Write-Host "  | $_" } }
} else {
    Write-Host "core: none bundled ($coreExe not found; pass -CoreDir or set CORE_DIR)"
}
$files = Get-ChildItem -File -Recurse $data
$kb = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1KB, 1)
$mb = [math]::Round((($files | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "data: $($files.Count) files, $kb KB ($mb MB) -> $data"
# licence text and the distribution README next to the exe (see the header)
$version = 'dev'
$pg = Get-Content -Raw (Join-Path $app 'project.godot')
if ($pg -match '(?m)^config/version="([^"]+)"') { $version = 'v' + $Matches[1] }
$commit = (& git -C $repo rev-parse --short HEAD 2>$null); if (-not $commit) { $commit = 'unknown' }
if ((& git -C $repo status --porcelain --untracked-files=no 2>$null)) { $commit = "$commit+dirty" }
$date = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $out 'LICENSE')
$readme = Get-Content -Raw -Encoding UTF8 (Join-Path $PSScriptRoot 'dist-README.txt')
$readme = $readme.Replace('{{VERSION}}', $version).Replace('{{COMMIT}}', $commit).Replace('{{DATE}}', $date)
[System.IO.File]::WriteAllText((Join-Path $out 'README.txt'), $readme, (New-Object System.Text.UTF8Encoding $false))
Write-Host "dist: LICENSE + README.txt ($version, $commit, $date) -> $out"

if (Test-Path (Join-Path $out 'FlightBench.exe')) {
    $all = Get-ChildItem -File -Recurse $out
    $amb = [math]::Round((($all | Measure-Object Length -Sum).Sum) / 1MB, 1)
    Write-Host "build: $($all.Count) files, $amb MB -> $out"
}

if ($Zip) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zipPath = Join-Path (Split-Path -Parent $out) "FlightBench-win64-$version$(if ($Debug) { '-debug' }).zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    $fs = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
    $za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem -File -Recurse $out) {
            $entry = 'FlightBench/' + ($f.FullName.Substring($out.Length + 1) -replace '\\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $za.Dispose(); $fs.Dispose() }
    $zmb = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Write-Host "zip: $zmb MB -> $zipPath"
}
