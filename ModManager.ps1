# sloperinaRecomp Mod Manager: stock Switch exefs mods (sloperinaGZ ...) in recomp game folders: Hookshot, Longshot and forks
# with the same tools layout. Nothing from the mod's side: its normal release (subsdk module + .ips), as for an emulator.
#
#   ModManager.bat                                a window: pick the game's exe, install the mod loader, install / update mods
#   ModManager.ps1 -InstallLoader <game>          (what the window runs, in a console of its own) add the mod loader
#   ModManager.ps1 -RemoveLoader <game>           build the game back without it
#   ModManager.ps1 -InstallMod <name> <game>      a known mod's newest release (mods\<name>\source.json), built and installed
#   ModManager.ps1 -InstallMod <name> <game> -From <zip or folder>   a mod from its files
#   ModManager.ps1 -Remove <name> <game>
#
# The mod loader is built into the game once per release of the recomp (a minute or two the first time, then seconds: only
# the runtime is compiled, the game's code comes from the builder's own cache). A mod is turned into a DLL on this PC with
# the builder's translator (under a minute, at low priority), in <game>\exefs_mods\<mod>\: removing it is deleting that.
# Updating the recomp itself replaces the game's exe: the window then offers to add the loader again (mods stay).
param(
    [Parameter(Position = 0)][string]$Game = "",
    [switch]$InstallLoader, [switch]$RemoveLoader,
    [string]$InstallMod = "", [string]$From = "", [string]$Remove = "",
    [string]$Tools = "", [int]$Jobs = 2, [ValidateSet("Idle", "BelowNormal", "Normal")][string]$Priority = "BelowNormal"
)
$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
$Data = Join-Path $env:LOCALAPPDATA "sloperinaRecomp"
$Remember = Join-Path $Data "last_game.txt"
$UserAgent = "sloperinaRecomp-ModManager"

function Fail($text) { Write-Host ""; Write-Host "ModManager: $text" -ForegroundColor Red; exit 1 }

# ---- the game folder ----
# whatever was given (the folder, its launcher exe, bin\<exe>) -> the game folder
function GameFolder($path) {
    if (-not $path) { return $null }
    $p = (Resolve-Path -LiteralPath $path.Trim('"') -ErrorAction SilentlyContinue).Path
    if (-not $p) { return $null }
    if (Test-Path -LiteralPath $p -PathType Leaf) { $p = Split-Path $p }
    if ((Split-Path $p -Leaf) -eq "bin" -and (Test-Path (Join-Path (Split-Path $p) "image.bin"))) { $p = Split-Path $p }
    return $p
}

# the folder's version file (hookshot_version.txt, longshot_version.txt ...): product, release, the file and its text
function GameInfo($game) {
    if (-not $game -or -not (Test-Path $game)) { return $null }
    $vf = Get-ChildItem $game -File -Filter "*_version.txt" -ErrorAction SilentlyContinue |
          Where-Object { (Get-Content -Raw $_.FullName) -match "(?m)^version=" } | Select-Object -First 1
    if (-not $vf -or -not (Test-Path (Join-Path $game "romfs"))) { return $null }
    $text = Get-Content -Raw $vf.FullName
    $product = $vf.Name -replace "_version\.txt$", ""
    $null = $text -match "(?m)^version=(\S+)"
    $exe = @((Join-Path $game "bin\$product.exe"), (Join-Path $game "$product.exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $exe) {
        # (a recomp named otherwise: the game is the biggest exe, in bin\ when there is one)
        foreach ($d in @((Join-Path $game "bin"), $game)) {
            $exe = Get-ChildItem $d -File -Filter "*.exe" -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch "setup|unins|crash" } |
                   Sort-Object Length -Descending | Select-Object -First 1 | ForEach-Object { $_.FullName }
            if ($exe) { break }
        }
    }
    [pscustomobject]@{ Folder = $game; Product = $product; Version = $Matches[1]; VersionFile = $vf.FullName; VersionText = $text; Exe = $exe }
}

# the names an exe exports (its PE export table): the loader's mark is hos_modloader_abi
function PeExports($path) {
    if (-not $path -or -not (Test-Path $path)) { return @() }
    $fs = [IO.File]::OpenRead($path)
    $br = New-Object IO.BinaryReader($fs)
    try {
        $fs.Position = 0x3C; $pe = $br.ReadInt32()
        $fs.Position = $pe + 6; $nsec = $br.ReadUInt16()
        $fs.Position = $pe + 20; $optSize = $br.ReadUInt16()
        $opt = $pe + 24; $fs.Position = $opt; $magic = $br.ReadUInt16()
        $fs.Position = $opt + $(if ($magic -eq 0x20B) { 112 } else { 96 })
        $expRva = $br.ReadUInt32()
        if (-not $expRva) { return @() }
        $secs = @()
        $fs.Position = $opt + $optSize
        for ($i = 0; $i -lt $nsec; $i++) {
            $null = $br.ReadBytes(8); $vsize = $br.ReadUInt32(); $va = $br.ReadUInt32(); $rsize = $br.ReadUInt32(); $raw = $br.ReadUInt32()
            $fs.Position += 16
            $secs += , @($va, [Math]::Max($vsize, $rsize), $raw)
        }
        $off = { param($rva) foreach ($s in $secs) { if ($rva -ge $s[0] -and $rva -lt $s[0] + $s[1]) { return [long]($rva - $s[0] + $s[2]) } }; return -1L }
        $fs.Position = (& $off $expRva) + 24
        $n = $br.ReadUInt32(); $null = $br.ReadUInt32(); $namesRva = $br.ReadUInt32()
        if (-not $n -or $n -gt 100000) { return @() }
        $fs.Position = & $off $namesRva
        $rvas = @(); for ($i = 0; $i -lt $n; $i++) { $rvas += $br.ReadUInt32() }
        $names = @()
        foreach ($r in $rvas) {
            $fs.Position = & $off $r
            $sb = New-Object Text.StringBuilder
            while (($b = $fs.ReadByte()) -gt 0) { [void]$sb.Append([char]$b) }
            $names += $sb.ToString()
        }
        return $names
    } catch { return @() } finally { $br.Close() }
}

function LoaderInstalled($info) { return $info -and $info.Exe -and ((PeExports $info.Exe) -contains "hos_modloader_abi") }

function GameRunning($game) {
    return [bool](Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($game, "OrdinalIgnoreCase") })
}

# ---- how long things take: measured here, kept in times.json (an estimate from the measurements made writing this before) ----
$TimesFile = Join-Path $Data "times.json"
function Times { $t = @{}; if (Test-Path $TimesFile) { try { (Get-Content -Raw $TimesFile | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $t[$_.Name] = $_.Value } } catch { } }; return $t }
function NoteTime($key, $seconds) {
    New-Item -ItemType Directory -Force $Data | Out-Null
    $t = Times
    $t[$key] = [int]$seconds
    try { $t | ConvertTo-Json | Set-Content -Encoding UTF8 $TimesFile } catch { }
}
# seconds: kind loader-first (a new release of the recomp: the game is translated once), loader, mod
function Estimate($kind, $jobs, $priority) {
    $key = "$kind-$jobs-$priority"
    $t = Times
    if ($t.ContainsKey($key)) { return [int]$t[$key] }
    $j = [Math]::Max(1, $jobs)
    $sec = switch ($kind) {
        "loader-first" { 110 + 612 / $j }    # (translation ~2 min; ~500 runtime and graphics files: 306 s with 2 jobs at idle)
        "loader" { 10 + 30 / $j }            # (6-20 runtime files: 22 s with 2 jobs)
        default { 15 + 30 }                  # (one big file of the mod's code: ~35 s whatever the jobs, its translation ~5 s)
    }
    if ($priority -eq "Idle") { $sec *= 1.3 }   # (and longer while the PC is busy: everything else goes first)
    return [int]$sec
}
function FirstLoader($info) { return -not ((Times).ContainsKey("built-$($info.Product)-$($info.Version)")) }
function Duration($sec) { if ($sec -lt 90) { return "about $([int]([Math]::Ceiling($sec / 10) * 10)) seconds" }; return "about $([int][Math]::Ceiling($sec / 60)) minutes" }

# ---- the builder's tools, and building the loader in ----
function FindTools($info) {
    if ($Tools) { return (Resolve-Path $Tools).Path }
    $version = $info.Version
    $known = KnownTools
    $key = "$($info.Product)-$version"
    if ($known.ContainsKey($key) -and (IsTools $known[$key] $info)) { return $known[$key] }
    # Any recomp of this family, whatever it's called: a builder's tools are a folder with python\ and nx2\make_game.py, and
    # its version.txt is the game's release. Looked for where the game's version file says it was built from, above the game
    # folder, then where installers put programs (Windows' list of installed programs, AppData, the user folder, Program
    # Files), a few levels down
    $roots = @()
    if ($info.VersionText -match "(?m)^builder=(.+?)\s*$") {
        $b = $Matches[1]; $roots += $(if (Test-Path $b -PathType Container) { $b } else { Split-Path $b })
    }
    for ($d = $info.Folder; $d; $d = Split-Path $d) { $roots += $d }
    foreach ($k in @("HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*", "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
                     "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*")) {
        $roots += @(Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object { $_.InstallLocation } | ForEach-Object { $_.InstallLocation })
    }
    $roots += @($env:LOCALAPPDATA, $env:APPDATA, (Join-Path $env:USERPROFILE "Downloads"), (Join-Path $env:USERPROFILE "Desktop"),
                (Join-Path $env:USERPROFILE "Documents"), $env:USERPROFILE, $env:ProgramFiles, ${env:ProgramFiles(x86)})
    $found = @(SearchTools @($roots | Where-Object { $_ } | Select-Object -Unique) $info)
    $t = $found | Where-Object { $v = Join-Path $_ "version.txt"; (Test-Path $v) -and ((Get-Content -Raw $v).Trim() -eq $version) } | Select-Object -First 1
    if (-not $t) { $t = $found | Where-Object { $_ -match [regex]::Escape("\$version\") } | Select-Object -First 1 }
    if ($t) { RememberTools $info $t }
    return $t
}

# a builder's tools folder (of any release: the caller checks it's the game's)
function IsTools($dir) {
    return (Test-Path -LiteralPath (Join-Path $dir "nx2\make_game.py")) -and (Test-Path -LiteralPath (Join-Path $dir "python\python.exe"))
}

# tools folders under the roots, at most 5 levels down, skipping links and big folders no builder lives in (20 s at most)
function SearchTools($roots, $info) {
    $skip = "^(\.|\$|node_modules$|Windows$|Microsoft|Packages$|Temp$|.*cache$|Caches?$|Google$|Mozilla$|NVIDIA|AMD$|Intel$|steamapps$|" +
            "objcache$|lift-store$|romfs$|python$|llvm-mingw$|emsdk$|cpm$|third_party.*|gpu$|host$|nx2$|sloperinaRecomp$)"
    $deadline = (Get-Date).AddSeconds(20)
    $out = New-Object System.Collections.Generic.List[string]
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $queue = New-Object System.Collections.Generic.Queue[object]
    foreach ($r in $roots) { if ([IO.Directory]::Exists($r)) { $queue.Enqueue([Tuple]::Create([IO.Path]::GetFullPath($r).TrimEnd("\"), 0)) } }
    while ($queue.Count -and (Get-Date) -lt $deadline) {
        $item = $queue.Dequeue(); $dir = $item.Item1; $depth = $item.Item2
        if (-not $seen.Add($dir)) { continue }
        if (IsTools $dir) { $out.Add($dir); continue }
        if ($depth -ge 5) { continue }
        try { $subs = [IO.Directory]::GetDirectories($dir) } catch { continue }
        foreach ($sub in $subs) {
            if ([IO.Path]::GetFileName($sub) -match $skip) { continue }
            try { if ([IO.File]::GetAttributes($sub) -band [IO.FileAttributes]::ReparsePoint) { continue } } catch { continue }
            $queue.Enqueue([Tuple]::Create([string]$sub, [int]($depth + 1)))
        }
    }
    return $out
}

# the tools folder found (or picked) for each game release, so it's looked for once
$ToolsFile = Join-Path $Data "tools.json"
function KnownTools { $t = @{}; if (Test-Path $ToolsFile) { try { (Get-Content -Raw $ToolsFile | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $t[$_.Name] = $_.Value } } catch { } }; return $t }
function RememberTools($info, $dir) {
    New-Item -ItemType Directory -Force $Data | Out-Null
    $t = KnownTools; $t["$($info.Product)-$($info.Version)"] = $dir
    try { $t | ConvertTo-Json | Set-Content -Encoding UTF8 $ToolsFile } catch { }
}

# the builder's work folder (its compiled objects, its kept translations): beside its tools or above the game, else in
# AppData under a folder named after the recomp (Triforce: %LOCALAPPDATA%\Triforce\build)
function FindBuilderWork($tools, $game, $product) {
    $dirs = @()
    foreach ($start in @($tools, $game)) { for ($d = $start; $d; $d = Split-Path $d) { $dirs += $d } }
    foreach ($a in @(@($env:LOCALAPPDATA, $env:APPDATA) | ForEach-Object { Get-ChildItem $_ -Directory -ErrorAction SilentlyContinue } |
                     Where-Object { $product -and $_.Name -match [regex]::Escape($product) })) {
        $dirs += $a.FullName
        $dirs += @(Get-ChildItem $a.FullName -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    }
    foreach ($d in $dirs) {
        foreach ($w in @($d, (Join-Path $d "work"), (Join-Path $d "build"))) {
            if ((Test-Path (Join-Path $w "objcache")) -and -not $w.StartsWith($Data, "OrdinalIgnoreCase") -and $w -notmatch "sloperinaRecomp") { return $w }
        }
    }
    return $null
}

# files from one folder into another as hard links (a copy across drives): the builder's caches, read only by us
function LinkTree($from, $to) {
    $n = 0
    foreach ($f in Get-ChildItem -Recurse -File $from -ErrorAction SilentlyContinue) {
        $t = Join-Path $to $f.FullName.Substring($from.Length).TrimStart("\")
        if (Test-Path $t) { continue }
        New-Item -ItemType Directory -Force (Split-Path $t) | Out-Null
        try { New-Item -ItemType HardLink -Path $t -Target $f.FullName -ErrorAction Stop | Out-Null } catch { Copy-Item $f.FullName $t }
        $n++
    }
    return $n
}

function BuildLoader($info, [bool]$with) {
    $game = $info.Folder
    if (GameRunning $game) { Fail "the game is running: close it first" }
    $tools = FindTools $info
    if (-not $tools) { Fail "the builder for $($info.Product) $($info.Version) wasn't found. Install that release, or give its tools folder: ModManager.ps1 -InstallLoader <game> -Tools <...\tools>" }
    if (-not (Test-Path (Join-Path $tools "python\python.exe"))) { Fail "$tools isn't a builder's tools folder (no python\)" }
    $bwork = FindBuilderWork $tools $game $info.Product
    # (the work folder on the builder's drive, so its caches link in instead of being copied)
    $root = if ($bwork) { Join-Path (Split-Path $bwork) "sloperinaRecomp" } else { $Data }
    $stage = Join-Path $root "$($info.Product)-$($info.Version)"
    $st = Join-Path $stage "tools"
    $work = Join-Path $root "work-$($info.Product)"
    $store = Join-Path $root "lift-store-$($info.Product)"
    Write-Host "$($info.Product) $($info.Version): $game"
    Write-Host "builder: $tools"
    # a copy of the builder's tools (theirs stay as they are), its compiler, Python and libraries linked, not copied
    New-Item -ItemType Directory -Force $st, $work, $store | Out-Null
    Get-ChildItem $tools -Directory | ForEach-Object {
        $dst = Join-Path $st $_.Name
        $big = (Get-Item $_.FullName).Attributes -band [IO.FileAttributes]::ReparsePoint -or
               @("cpm", "emsdk", "llvm-mingw", "python", "wgpu-native", "third_party_objects") -contains $_.Name
        if ($big) {
            if (-not (Test-Path $dst)) { New-Item -ItemType Junction -Path $dst -Target $_.FullName | Out-Null }
        } else {
            robocopy $_.FullName $dst /MIR /XD __pycache__ /NFL /NDL /NJH /NJS /NP | Out-Null
            if ($LASTEXITCODE -ge 8) { Fail "couldn't copy the builder's $($_.Name)" }
        }
    }
    Get-ChildItem $tools -File | Copy-Item -Destination $st -Force
    Get-ChildItem (Split-Path $tools) -File -Filter "*Setup.exe" | Copy-Item -Destination $stage -Force   # (the launcher layout)
    $py = Join-Path $st "python\python.exe"
    if ($with) {
        Write-Host "adding the mod loader:"
        & $py -I (Join-Path $Here "loader\patch_loader.py") $st
        if ($LASTEXITCODE) { Fail "this $($info.Product) release can't take the mod loader (above)" }
    } else {
        Write-Host "building the game without the mod loader"
    }
    if (Test-Path (Join-Path $st "wgpu-native")) { $env:WGPU_NATIVE = Join-Path $st "wgpu-native" }
    # The builder's compiled objects and kept translations, so only the runtime is compiled: first the work folder's note
    # of the compiler's headers as make_game.py takes it for this copy (it drops every object when the note differs), then
    # the builder's objects and translation snapshots it doesn't have, linked
    Push-Location $st
    try {
        & $py -I -c "import sys; sys.argv = ['make_game.py']; sys.path.insert(0, 'nx2'); import make_game; make_game.bundled_mingw(); make_game.drop_stale_objects(r'$work')" | Out-Null
    } catch { } finally { Pop-Location }
    if ($bwork) {
        if (Test-Path (Join-Path $work "system_headers.txt")) {
            $n = LinkTree (Join-Path $bwork "objcache") (Join-Path $work "objcache")
            if ($n) { Write-Host "reusing $n of the builder's compiled objects" }
        }
        if (Test-Path (Join-Path $bwork "lift-store")) {
            $n = LinkTree (Join-Path $bwork "lift-store") $store
            if ($n) { Write-Host "reusing the builder's kept translation" }
        }
    }
    $envName = "LONGSHOT_LIFT_STORE"
    $ls = Join-Path $st "nx2\lift_store.py"
    if ((Test-Path $ls) -and ((Get-Content -Raw $ls) -match 'STORE_ENV = "(\w+)"')) { $envName = $Matches[1] }
    Set-Item "env:$envName" $store
    $log = Join-Path $stage "build_log.txt"
    $mgArgs = @("-I", "nx2/make_game.py", "--update", $game, "--work", $work)
    # (the options this builder has: a recomp of the family may lack some)
    $mgText = Get-Content -Raw (Join-Path $st "nx2\make_game.py")
    if ($mgText -match '"--no-prepare-shaders"') { $mgArgs += "--no-prepare-shaders" }
    if ($mgText -match '"--jobs"') { $mgArgs += @("--jobs", "$Jobs") }
    if ($mgText -match '"--log"') { $mgArgs += @("--log", $log) }
    # (the PC stays usable: below-normal priority, which the builder's Python and every compiler it starts inherit)
    try { (Get-Process -Id $PID).PriorityClass = $Priority } catch { }
    $first = FirstLoader $info
    Write-Host ("building at {0} priority with {1} compile jobs: {2} (the log: $log)" -f $Priority, $Jobs, (Duration (Estimate $(if ($first) { "loader-first" } else { "loader" }) $Jobs $Priority)))
    $t0 = Get-Date
    Push-Location $st
    try {
        & $py @mgArgs | ForEach-Object {
            if ($_ -match "^@@stage \S+ (.*)$") { Write-Host ("  {0,4:n0}s  {1}" -f ((Get-Date) - $t0).TotalSeconds, $Matches[1]) }
            elseif ($_ -match "^@@error (.*)$") { Write-Host "  $($Matches[1])" -ForegroundColor Red }
            elseif ($_ -match "^the build took") { Write-Host "  $_" }
        }
        $code = $LASTEXITCODE
    } finally { Pop-Location }
    # (the builder's own lines, which its launcher reads, back as they were)
    Set-Content -NoNewline -Path $info.VersionFile -Value $info.VersionText
    if ($code) { Fail "the build stopped (see $log). The game in the folder is as it was before." }
    New-Item -ItemType Directory -Force (Join-Path $game "exefs_mods") | Out-Null
    Write-Host ""
    NoteTime "$(if ($first) { 'loader-first' } else { 'loader' })-$Jobs-$Priority" ((Get-Date) - $t0).TotalSeconds
    NoteTime "built-$($info.Product)-$($info.Version)" 1
    Write-Host ("done in {0:n0}s: the mod loader is {1}" -f ((Get-Date) - $t0).TotalSeconds, $(if ($with) { "in" } else { "out" })) -ForegroundColor Green
}

# ---- mods ----
# A mod is a stock Switch exefs mod (a subsdk module and its .ips, as on Atmosphere or an emulator), turned into a DLL on this
# PC (dll\modlift.py and dll\build_dll.ps1, with the builder's own tools: under a minute, at low priority). The known ones
# (mods\<name>\source.json) come from their GitHub releases, as they are; any other can be added from its zip or folder.
function ReadJson($path) { if (Test-Path $path) { try { return Get-Content -Raw $path | ConvertFrom-Json } catch { } }; return $null }

function Catalog {
    $root = Join-Path $Here "mods"
    if (-not (Test-Path $root)) { return @() }
    @(Get-ChildItem $root -Directory | ForEach-Object {
        $j = ReadJson (Join-Path $_.FullName "source.json")
        if ($j) { [pscustomobject]@{ Name = $_.Name; Source = $j } }
    } | Where-Object { $_ })
}

function InstalledMods($game) {
    $root = Join-Path $game "exefs_mods"
    if (-not (Test-Path $root)) { return @() }
    @(Get-ChildItem $root -Directory | Where-Object { Test-Path (Join-Path $_.FullName "mod.dll") } | ForEach-Object {
        $j = ReadJson (Join-Path $_.FullName "mod.json")
        [pscustomobject]@{ Name = $_.Name; Version = $(if ($j) { $j.version } else { "?" }); Path = $_.FullName }
    })
}

function RemoveMod($name, $game) {
    if (GameRunning $game) { throw "the game is running: close it first" }
    $dst = Join-Path (Join-Path $game "exefs_mods") $name
    if (Test-Path (Join-Path $dst "mod.dll")) { Remove-Item -Recurse -Force $dst }
}

# a mod's newest release on GitHub: its version and its zip's address
function LatestRelease($source) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $rel = Invoke-RestMethod -UseBasicParsing -Headers @{ "User-Agent" = $UserAgent } "https://api.github.com/repos/$($source.github)/releases/latest"
    $asset = $rel.assets | Where-Object { $_.name -match $source.asset } | Select-Object -First 1
    if (-not $asset) { throw "its latest release ($($rel.tag_name)) has no $($source.asset) download" }
    [pscustomobject]@{ Version = ($rel.tag_name -replace "^v", ""); Url = $asset.browser_download_url; File = $asset.name }
}

# the module and .ips files of a mod (an unpacked release, an Atmosphere / emulator mod folder): the module is a subsdk1-9
# file (several copies in a release are the same file), the .ips are named by the game build they patch
function ModFiles($dir) {
    $all = Get-ChildItem -Recurse -File $dir
    $module = $all | Where-Object { $_.Name -match "^subsdk[1-9]$" } | Sort-Object { $_.FullName.Length } | Select-Object -First 1
    $ips = @($all | Where-Object { $_.Extension -eq ".ips" } | Group-Object Name | ForEach-Object { $_.Group[0].FullName })
    if (-not $module) { throw "no subsdk1-9 module in it" }
    [pscustomobject]@{ Module = $module.FullName; Ips = $ips }
}

# build a mod DLL from a stock mod (a zip, or an unpacked folder) and put it in the game
function BuildMod($info, $name, $version, $from) {
    $game = $info.Folder
    if (GameRunning $game) { Fail "the game is running: close it first" }
    $tools = FindTools $info
    if (-not $tools) { Fail "the builder for $($info.Product) $($info.Version) wasn't found (its translator turns the mod into a DLL)" }
    $exefs = Get-ChildItem -Force $game -Directory -Filter ".*_exefs" | Select-Object -First 1
    if (-not $exefs) { Fail "$game has no .<name>_exefs (the game's code the builder keeps): the mod's imports can't be bound" }
    $work = Join-Path $Data "mods\$name"
    if (Test-Path $work) { Remove-Item -Recurse -Force $work }
    New-Item -ItemType Directory -Force $work | Out-Null
    $src = $from
    if ((Test-Path $from -PathType Leaf) -and $from -match "\.zip$") {
        $src = Join-Path $work "unpacked"
        Expand-Archive $from $src
    }
    $files = ModFiles $src
    Write-Host "$name $version`: $([IO.Path]::GetFileName($files.Module)) and $($files.Ips.Count) .ips"
    $py = Join-Path $tools "python\python.exe"
    $clang = Join-Path $tools "llvm-mingw\bin\clang.exe"
    try { (Get-Process -Id $PID).PriorityClass = $Priority } catch { }
    $t0 = Get-Date
    Write-Host ("building it at {0} priority with {1} jobs: {2}" -f $Priority, $Jobs, (Duration (Estimate "mod" $Jobs $Priority)))
    Write-Host "translating it (the builder's translator)..."
    & $py -I (Join-Path $Here "dll\modlift.py") $tools $exefs.FullName $files.Module (Join-Path $work "out") @($files.Ips) |
        Where-Object { $_ -match "^(ips|module|wrote|modlift)" } | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE) { Fail "$name couldn't be translated (above)" }
    Write-Host "compiling it..."
    $dll = Join-Path $work "mod.dll"
    & (Join-Path $Here "dll\build_dll.ps1") -Out (Join-Path $work "out") -Dll $dll -Name $name -Version $version -Clang $clang -Jobs $Jobs -Priority $Priority |
        ForEach-Object { Write-Host "  $_" }
    $dst = Join-Path (Join-Path $game "exefs_mods") $name
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item $dll (Join-Path $dst "mod.dll") -Force
    @{ name = $name; version = $version; built = (Get-Date -Format "yyyy-MM-dd HH:mm"); for = "$($info.Product) $($info.Version)" } |
        ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $dst "mod.json")
    Remove-Item -Recurse -Force (Join-Path $work "out\obj") -ErrorAction SilentlyContinue
    Write-Host ""
    NoteTime "mod-$Jobs-$Priority" ((Get-Date) - $t0).TotalSeconds
    Write-Host ("done in {0:n0}s: $name $version is in the game" -f ((Get-Date) - $t0).TotalSeconds) -ForegroundColor Green
    if (-not (LoaderInstalled $info)) { Write-Host "The game needs the mod loader for it to run: click Install loader." -ForegroundColor DarkYellow }
}

# a known mod's newest release, downloaded and built
function InstallKnown($info, $name) {
    $c = Catalog | Where-Object { $_.Name -eq $name } | Select-Object -First 1
    if (-not $c) { Fail "$name isn't one of the manager's known mods" }
    $rel = LatestRelease $c.Source
    $zip = Join-Path $Data "downloads\$($rel.File)"
    New-Item -ItemType Directory -Force (Split-Path $zip) | Out-Null
    if (-not (Test-Path $zip)) {
        Write-Host "downloading $($rel.File)..."
        Invoke-WebRequest -UseBasicParsing -Headers @{ "User-Agent" = $UserAgent } $rel.Url -OutFile "$zip.part"
        Move-Item -Force "$zip.part" $zip
    }
    BuildMod $info $name $rel.Version $zip
}

# ---- the console jobs ----
if ($InstallLoader -or $RemoveLoader -or $InstallMod -or $Remove) {
    $g = GameFolder $Game
    $info = GameInfo $g
    if (-not $info) { Fail "$Game isn't a game folder a recomp builder made (it needs romfs and <name>_version.txt)" }
    if ($InstallLoader) { BuildLoader $info $true }
    elseif ($RemoveLoader) { BuildLoader $info $false }
    elseif ($InstallMod -and $From) { BuildMod $info $InstallMod "local" (Resolve-Path $From).Path }
    elseif ($InstallMod) { InstallKnown $info $InstallMod }
    else { RemoveMod $Remove $info.Folder; Write-Host "removed $Remove" }
    exit 0
}

# ---- the window ----
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
New-Item -ItemType Directory -Force $Data | Out-Null
$script:info = $null

$form = New-Object System.Windows.Forms.Form
$form.Text = "sloperinaRecomp Mod Manager"
$form.Size = New-Object System.Drawing.Size(640, 560)
$form.MinimumSize = $form.Size
$form.StartPosition = "CenterScreen"
$form.AllowDrop = $true
$font = New-Object System.Drawing.Font("Segoe UI", 9.5)
$form.Font = $font

function Label($text, $x, $y, $w, $h) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, $h)
    $form.Controls.Add($l); return $l
}
function Button($text, $x, $y, $w) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, 30)
    $form.Controls.Add($b); return $b
}

$gameLabel = Label "Drop your game's exe (hookshot.exe, longshot.exe ...) here, or Browse." 16 14 480 40
$browse = Button "Browse..." 504 16 104
$loaderLabel = Label "" 16 62 480 40
$loaderButton = Button "Install loader" 504 64 104
$list = New-Object System.Windows.Forms.ListView
$list.View = "Details"; $list.FullRowSelect = $true; $list.MultiSelect = $false; $list.HideSelection = $false
$list.Location = New-Object System.Drawing.Point(16, 114); $list.Size = New-Object System.Drawing.Size(592, 200)
$list.Anchor = "Top, Left, Right"
[void]$list.Columns.Add("Mod", 180); [void]$list.Columns.Add("In the game", 120); [void]$list.Columns.Add("Available", 120); [void]$list.Columns.Add("", 160)
$form.Controls.Add($list)
$installButton = Button "Install / update" 16 322 140
$removeButton = Button "Remove" 164 322 100
$onlineButton = Button "Check for updates" 272 322 150
$addButton = Button "Add mod from file..." 430 322 150
$status = New-Object System.Windows.Forms.TextBox
$status.Multiline = $true; $status.ReadOnly = $true; $status.ScrollBars = "Vertical"
$speedLabel = Label "Speed:" 16 366 50 24
$speed = New-Object System.Windows.Forms.ComboBox
$speed.DropDownStyle = "DropDownList"; $speed.Location = New-Object System.Drawing.Point(68, 362); $speed.Size = New-Object System.Drawing.Size(250, 26)
$cpus = [Environment]::ProcessorCount
$speeds = @(
    @{ Text = "Gentle (low priority, 2 threads)"; Priority = "Idle"; Jobs = 2 },
    @{ Text = "Balanced (below normal, $([Math]::Max(2, [int]($cpus / 2))) threads)"; Priority = "BelowNormal"; Jobs = [Math]::Max(2, [int]($cpus / 2)) },
    @{ Text = "Fast (normal priority, all $cpus threads)"; Priority = "Normal"; Jobs = $cpus })
foreach ($o in $speeds) { [void]$speed.Items.Add($o.Text) }
$form.Controls.Add($speed)
$estimate = Label "" 330 366 290 24
$status.Location = New-Object System.Drawing.Point(16, 400); $status.Size = New-Object System.Drawing.Size(592, 100)
$status.Anchor = "Top, Bottom, Left, Right"
$form.Controls.Add($status)

function Say($text) { $status.AppendText($text + "`r`n") }

$speedFile = Join-Path $Data "speed.txt"
$speed.SelectedIndex = 0
try { if (Test-Path $speedFile) { $speed.SelectedIndex = [Math]::Min(2, [int](Get-Content -Raw $speedFile).Trim()) } } catch { }
function Speed { return $speeds[[Math]::Max(0, $speed.SelectedIndex)] }
function SpeedArgs { $o = Speed; return "-Priority $($o.Priority) -Jobs $($o.Jobs)" }
# the builder's tools for the game: looked for (once per release), else the player shows where they are. $null: not given
function ToolsArg {
    Say "Looking for $($script:info.Product) $($script:info.Version)'s builder..."
    [System.Windows.Forms.Application]::DoEvents()
    $t = FindTools $script:info
    if (-not $t) {
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = "The builder for $($script:info.Product) $($script:info.Version) wasn't found. Choose its tools folder (the one with python and nx2 in it)."
        if ($dlg.ShowDialog() -ne "OK") { Say "The builder's tools are needed for this."; return $null }
        $t = $dlg.SelectedPath
        if (-not (IsTools $t)) { Say "That isn't a builder's tools folder (it needs python\ and nx2\make_game.py): $t"; return $null }
        RememberTools $script:info $t
    }
    return "-Tools '$t'"
}
function ShowEstimate {
    $o = Speed
    $mod = Duration (Estimate "mod" $o.Jobs $o.Priority)
    $loader = if ($script:info -and -not (LoaderInstalled $script:info)) {
        Duration (Estimate $(if (FirstLoader $script:info) { "loader-first" } else { "loader" }) $o.Jobs $o.Priority)
    } else { $null }
    $estimate.Text = if ($loader) { "Estimated: loader $loader, a mod $mod" } else { "Estimated: a mod $mod" }
}
$speed.Add_SelectedIndexChanged({ try { Set-Content -Path $speedFile -Value $speed.SelectedIndex } catch { }; ShowEstimate })

# the newest versions seen online (Check for updates), kept between runs
$latestFile = Join-Path $Data "latest.json"
$script:latest = @{}
$lj = ReadJson $latestFile
if ($lj) { $lj.PSObject.Properties | ForEach-Object { $script:latest[$_.Name] = $_.Value } }
function Latest($name) { if ($script:latest.ContainsKey($name)) { return $script:latest[$name] }; return "?" }

function Refresh {
    ShowEstimate
    $i = $script:info
    $list.Items.Clear()
    $catalog = Catalog
    if (-not $i) {
        $gameLabel.Text = "Drop your game's exe (hookshot.exe, longshot.exe ...) here, or Browse."
        $loaderLabel.Text = "First pick your game: drop its exe on this window, or click Browse (or Install loader)."
        $loaderLabel.ForeColor = [System.Drawing.Color]::DarkOrange
        $loaderButton.Text = "Install loader"
        $loaderButton.Enabled = $true; $installButton.Enabled = $false; $removeButton.Enabled = $false
        foreach ($m in $catalog) { $it = New-Object System.Windows.Forms.ListViewItem($m.Name); [void]$it.SubItems.Add(""); [void]$it.SubItems.Add($(Latest $m.Name)); [void]$it.SubItems.Add(""); [void]$list.Items.Add($it) }
        return
    }
    $gameLabel.Text = "$($i.Product) $($i.Version)`n$($i.Folder)"
    $has = LoaderInstalled $i
    $loaderLabel.Text = if ($has) { "Mod loader: installed. Mods install and update instantly (with the game closed)." }
                        else { "Mod loader: not in this game yet (a new install, or the recomp was updated).`nAdd it once: about a minute, at low priority." }
    $loaderLabel.ForeColor = if ($has) { [System.Drawing.Color]::DarkGreen } else { [System.Drawing.Color]::DarkOrange }
    $loaderButton.Text = if ($has) { "Rebuild loader" } else { "Install loader" }
    $loaderButton.Enabled = $true
    $installButton.Enabled = $true; $removeButton.Enabled = $true
    $installed = @{}
    foreach ($m in InstalledMods $i.Folder) { $installed[$m.Name] = $m }
    $names = @($catalog | ForEach-Object { $_.Name }) + @($installed.Keys) | Select-Object -Unique
    foreach ($n in $names) {
        $c = $catalog | Where-Object { $_.Name -eq $n } | Select-Object -First 1
        $g = $installed[$n]
        $avail = if ($c) { Latest $n } else { "" }
        $state = if (-not $g) { "not installed" } elseif ($avail -and $avail -ne "?" -and $avail -ne $g.Version) { "update available" } else { "installed" }
        if ($g -and -not $has) { $state = "needs the loader" }
        $it = New-Object System.Windows.Forms.ListViewItem($n)
        [void]$it.SubItems.Add($(if ($g) { $g.Version } else { "" }))
        [void]$it.SubItems.Add($avail)
        [void]$it.SubItems.Add($state)
        [void]$list.Items.Add($it)
    }
    if ($list.Items.Count) { $list.Items[0].Selected = $true }
}

function Choose($path) {
    $g = GameFolder $path
    $i = GameInfo $g
    if (-not $i) { Say "That isn't a recomp game folder (it needs romfs and <name>_version.txt): $path"; return }
    $script:info = $i
    Set-Content -Path $Remember -Value $i.Folder
    Refresh
}

function Selected { if ($list.SelectedItems.Count) { return $list.SelectedItems[0].Text }; return $null }

# a console job (the loader's build) in a window of its own; the list is refreshed when it ends
function RunConsole($argsText) {
    $form.Enabled = $false
    try {
        $p = Start-Process powershell -ArgumentList "-NoProfile -ExecutionPolicy Bypass -Command `"& '$PSCommandPath' $argsText; Write-Host ''; Read-Host 'Press Enter to close'`"" -PassThru
        while (-not $p.HasExited) { [System.Windows.Forms.Application]::DoEvents(); Start-Sleep -Milliseconds 200 }
    } finally { $form.Enabled = $true; Refresh }
}

$onDragEnter = { param($s, $e) if ($e.Data.GetDataPresent([System.Windows.Forms.DataFormats]::FileDrop)) { $e.Effect = "Copy" } }
$onDrop = { param($s, $e) Choose (@($e.Data.GetData([System.Windows.Forms.DataFormats]::FileDrop))[0]) }
$form.Add_DragEnter($onDragEnter); $form.Add_DragDrop($onDrop)

$browse.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Choose your game's exe (hookshot.exe, longshot.exe ...)"
    $dlg.Filter = "Game (*.exe)|*.exe|All files|*.*"
    if ($script:info) { $dlg.InitialDirectory = $script:info.Folder }
    if ($dlg.ShowDialog() -eq "OK") { Choose $dlg.FileName }
})

$loaderButton.Add_Click({
    if (-not $script:info) { $browse.PerformClick() }
    if (-not $script:info) { return }
    if (GameRunning $script:info.Folder) { Say "Close the game first."; return }
    Say "Building the mod loader into $($script:info.Product) $($script:info.Version) (a console window shows how it goes)..."
    $ta = ToolsArg; if (-not $ta) { return }
    RunConsole "-InstallLoader '$($script:info.Folder)' $ta $(SpeedArgs)"
    Say $(if (LoaderInstalled $script:info) { "The mod loader is in." } else { "The mod loader isn't in: see the console window's messages." })
})

$installButton.Add_Click({
    $n = Selected
    if (-not $n -or -not $script:info) { return }
    if (GameRunning $script:info.Folder) { Say "Close the game first."; return }
    if (-not (Catalog | Where-Object { $_.Name -eq $n })) { Say "$n was added from its files: use Add mod from file... to update it."; return }
    Say "Getting the newest $n and building it for your game (a console window shows how it goes)..."
    $ta = ToolsArg; if (-not $ta) { return }
    RunConsole "-InstallMod '$n' '$($script:info.Folder)' $ta $(SpeedArgs)"
    $m = InstalledMods $script:info.Folder | Where-Object { $_.Name -eq $n }
    Say $(if ($m) { "$n $($m.Version) is in the game." } else { "$n isn't in the game: see the console window's messages." })
    if ($m -and -not (LoaderInstalled $script:info)) { Say "The game needs the mod loader for it to run: click Install loader." }
})

$removeButton.Add_Click({
    $n = Selected
    if (-not $n -or -not $script:info) { return }
    try { RemoveMod $n $script:info.Folder; Say "Removed $n (its settings and saves in user\sdmc stay)." } catch { Say "Couldn't remove ${n}: $($_.Exception.Message)" }
    Refresh
})

$onlineButton.Add_Click({
    foreach ($m in Catalog) {
        try {
            $rel = LatestRelease $m.Source
            $script:latest[$m.Name] = $rel.Version
            $have = if ($script:info) { InstalledMods $script:info.Folder | Where-Object { $_.Name -eq $m.Name } } else { $null }
            Say $(if (-not $have) { "$($m.Name): $($rel.Version) is the newest (not installed)" }
                  elseif ($have.Version -eq $rel.Version) { "$($m.Name) $($have.Version) is the newest" }
                  else { "$($m.Name): $($rel.Version) is out (you have $($have.Version)): click Install / update" })
        } catch { Say "$($m.Name): couldn't check ($($_.Exception.Message))" }
    }
    try { $script:latest | ConvertTo-Json | Set-Content -Encoding UTF8 $latestFile } catch { }
    Refresh
})

$addButton.Add_Click({
    if (-not $script:info) { return }
    if (GameRunning $script:info.Folder) { Say "Close the game first."; return }
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "A mod's release zip, or its subsdk module (subsdk1-9) in its folder"
    $dlg.Filter = "Mod (*.zip;subsdk*)|*.zip;subsdk*|All files|*.*"
    if ($dlg.ShowDialog() -ne "OK") { return }
    $f = $dlg.FileName
    $src = if ($f -match "\.zip$") { $f } else { Split-Path $f }
    $default = [IO.Path]::GetFileNameWithoutExtension($f) -replace "[-_ ]?v?\d+(\.\d+)+$", ""
    if ($default -match "^subsdk") { $default = Split-Path (Split-Path $f) -Leaf }
    Add-Type -AssemblyName Microsoft.VisualBasic
    $n = [Microsoft.VisualBasic.Interaction]::InputBox("The mod's name (its folder in exefs_mods):", "Add mod", $default)
    if (-not $n) { return }
    Say "Building $n for your game (a console window shows how it goes)..."
    $ta = ToolsArg; if (-not $ta) { return }
    RunConsole "-InstallMod '$n' '$($script:info.Folder)' -From '$src' $ta $(SpeedArgs)"
    Refresh
})

$start = if ($Game) { $Game } elseif (Test-Path $Remember) { (Get-Content -Raw $Remember).Trim() } else { "" }
if ($start) { Choose $start }
Refresh
[void]$form.ShowDialog()
