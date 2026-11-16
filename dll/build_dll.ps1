# Builds a mod DLL from a translated exefs mod (modlift.py's output folder): its C, the translator's runtime and smod_rt.c,
# compiled with the builder's clang at the given priority (idle: the PC stays usable), then linked.
#   build_dll.ps1 -Out <modlift output> -Dll <mod.dll to write> [-Name <mod>] [-Version <text>] [-Clang <clang.exe>] [-Jobs 2]
param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$Dll, [string]$Name = "mod", [string]$Version = "",
      [string]$Clang = "", [int]$Jobs = 2, [string]$Priority = "Idle")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
if (-not $Clang) { throw "build_dll: give -Clang (a builder's llvm-mingw\bin\clang.exe)" }
$gen = Join-Path $Out "gen"; $rt = Join-Path $Out "rt64"; $obj = Join-Path $Out "obj"
$api = @((Join-Path $here "..\loader\files\host"), $here) | Where-Object { Test-Path (Join-Path $_ "smod_api.h") } | Select-Object -First 1
New-Item -ItemType Directory -Force $obj | Out-Null
Get-ChildItem $obj -Filter *.o | Remove-Item -Force
Set-Content -Encoding ASCII (Join-Path $gen "smod_version.h") ("#define SMOD_NAME `"$Name`"`r`n#define SMOD_VERSION_TEXT `"$Version`"")
$common = @("--target=x86_64-w64-mingw32", "-march=x86-64-v2", "-ffp-contract=off", "-I`"$rt`"", "-I`"$gen`"", "-I`"$api`"",
            "-Wno-everything", "-fno-strict-aliasing", "-mcx16", "-g0",
            "-DA64_WRITE_TRACK", "-Da64_dirty=(*smod_dirty)")   # (written pages marked in the runtime's table: smod_rt.c)
$lifted = @("-O1", "-mllvm", "-rotation-max-header-size=0", "-mllvm", "-capture-tracking-max-uses-to-explore=5000")
$files = @(Get-ChildItem $gen -Filter "code_*.c" | ForEach-Object { , @($_.FullName, $lifted) }) +
        @(@((Join-Path $gen "a64_suyu.c"), @("-O2")), @((Join-Path $gen "a64_table.c"), @("-O2")), @((Join-Path $gen "a64_hooks.c"), @("-O2")),
          @((Join-Path $gen "smod_out.c"), @("-O2")), @((Join-Path $gen "smod_gen.c"), @("-O0")), @((Join-Path $rt "a64_rt.c"), @("-O2")),
          @((Join-Path $here "smod_rt.c"), @("-O2"))) | Where-Object { Test-Path $_[0] }
$objs = @(); $running = @()
$t0 = Get-Date
foreach ($j in $files) {
    $o = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($j[0]) + ".o"); $objs += $o
    while (@($running | Where-Object { -not $_.HasExited }).Count -ge $Jobs) { Start-Sleep -Milliseconds 100 }
    $p = Start-Process -FilePath $Clang -ArgumentList ($common + $j[1] + @("-c", "`"$($j[0])`"", "-o", "`"$o`"")) -NoNewWindow -PassThru `
         -RedirectStandardError (Join-Path $obj ([IO.Path]::GetFileName($j[0]) + ".err"))
    $null = $p.Handle
    try { $p.PriorityClass = $Priority } catch { }
    $running += $p
}
$running | ForEach-Object { $_.WaitForExit() }
foreach ($o in $objs) {
    if (-not (Test-Path $o)) {
        $err = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($o) + ".c.err")
        if (Test-Path $err) { Get-Content $err | Select-Object -First 20 }
        throw "build_dll: $([IO.Path]::GetFileName($o)) didn't compile"
    }
}
$p = Start-Process -FilePath $Clang -ArgumentList (@("--target=x86_64-w64-mingw32", "-shared", "-fuse-ld=lld", "-o", "`"$Dll`"") +
     ($objs | ForEach-Object { "`"$_`"" }) + @("-static-libgcc", "-Wl,--exclude-all-symbols")) -NoNewWindow -PassThru -RedirectStandardError (Join-Path $obj "link.err")
$null = $p.Handle
$p.WaitForExit()
if ($p.ExitCode -or -not (Test-Path $Dll)) { Get-Content (Join-Path $obj "link.err") | Select-Object -First 30; throw "build_dll: the link failed" }
"{0}: {1:n0} KB in {2:n0}s" -f $Dll, ((Get-Item $Dll).Length / 1KB), ((Get-Date) - $t0).TotalSeconds
