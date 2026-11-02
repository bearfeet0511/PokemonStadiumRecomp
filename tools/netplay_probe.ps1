# Netplay determinism experiment: does Stadium compute the same state from the
# same inputs? See src/main/netplay_probe.h for the probe itself.
#
#   powershell -ExecutionPolicy Bypass -File tools\netplay_probe.ps1 setup
#   ... record     # play normally; your input is recorded, close when done
#   ... replay     # two instances (a, b) replay that input side by side
#                  # (-DumpAt <poll> also writes each one's RDRAM at that poll)
#   ... compare    # first poll where the runs' state hashes differ
#   ... netplay    # a hosts and b joins over localhost (b plays on keyboard);
#                  # -LatencyMs <n> delays each side's incoming input like a real network
#   ... launch     # open a and b with no netplay override (set it up in the launcher)
#
# Each instance runs from its own folder under third_party\work\np\ (config,
# Stadium save and logs live next to the exe), all starting from the same
# Stadium save snapshot, with the Transfer Pak off and the launcher skipped.
param([Parameter(Mandatory)][ValidateSet('setup', 'record', 'replay', 'compare', 'netplay', 'launch')][string]$Step,
      [int]$DumpAt = -1,
      [int]$LatencyMs = 0)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$np = Join-Path $root 'third_party\work\np'
$instances = 'rec', 'a', 'b'
$saveName = 'pokestadium.us.1.0.bin'

function Start-Instance($name, $port, $extraEnv) {
    Update-Binaries $name
    $dir = Join-Path $np $name
    $env:RECOMP_DETERMINISTIC_CLOCK = '1'
    $env:PSR_STATE_HASH = Join-Path $dir 'hash.txt'
    $env:PSR_DEBUG_PORT = "$port"
    foreach ($k in $extraEnv.Keys) { Set-Item "env:$k" $extraEnv[$k] }
    $p = Start-Process -FilePath (Join-Path $dir 'PokemonStadiumRecomp.exe') -WorkingDirectory $dir `
        -RedirectStandardError (Join-Path $dir 'stderr.log') -PassThru
    foreach ($k in @('RECOMP_DETERMINISTIC_CLOCK', 'PSR_STATE_HASH', 'PSR_DEBUG_PORT') + @($extraEnv.Keys)) {
        Remove-Item "env:$k" -ErrorAction SilentlyContinue
    }
    return $p
}

# Hardlinks to the build's exe and DLLs. The linker writes a new file on every
# build, which breaks the links, so they're refreshed before every launch.
function Update-Binaries($name) {
    $dir = Join-Path $np $name
    foreach ($f in Get-ChildItem $build -File -Include *.exe, *.dll -Name -Path "$build\*") {
        $link = Join-Path $dir $f
        Remove-Item $link -ErrorAction SilentlyContinue
        New-Item -ItemType HardLink -Path $link -Target (Join-Path $build $f) | Out-Null
    }
}

# Every run starts from the same Stadium save, so its contents can't differ.
function Reset-Save($name) {
    $saves = Join-Path (Join-Path $np $name) 'saves'
    New-Item -ItemType Directory -Force $saves | Out-Null
    Copy-Item (Join-Path $np $saveName) (Join-Path $saves $saveName) -Force
    Remove-Item (Join-Path $saves "$saveName.bak") -ErrorAction SilentlyContinue
}

switch ($Step) {
    'setup' {
        New-Item -ItemType Directory -Force $np | Out-Null
        Copy-Item (Join-Path $build "saves\$saveName") (Join-Path $np $saveName) -Force
        $config = Get-Content (Join-Path $build 'launcher.cfg') | Where-Object { $_ -notmatch '^p\d_(rom|save)' }
        $config = $config -replace '^autoplay=.*', 'autoplay=on'
        foreach ($name in $instances) {
            $dir = Join-Path $np $name
            Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
            New-Item -ItemType Directory -Force $dir | Out-Null
            Update-Binaries $name
            Copy-Item (Join-Path $build 'rom.cfg') $dir
            Copy-Item -Recurse (Join-Path $build 'assets') $dir
            Set-Content (Join-Path $dir 'launcher.cfg') $config -Encoding ascii
            Reset-Save $name
        }
        Write-Host "set up $($instances -join ', ') in $np"
    }
    'record' {
        Reset-Save 'rec'
        $p = Start-Instance 'rec' 4381 @{ PSR_INPUT_RECORD = (Join-Path $np 'rec\input.bin') }
        Write-Host "recording (pid $($p.Id)); play, then close the window"
    }
    'replay' {
        $input = Join-Path $np 'rec\input.bin'
        foreach ($name in 'a', 'b') { Reset-Save $name }
        $replayEnv = @{ PSR_INPUT_REPLAY = $input }
        if ($DumpAt -ge 0) { $replayEnv['PSR_STATE_DUMP_AT'] = "$DumpAt" }
        $a = Start-Instance 'a' 4382 $replayEnv
        $b = Start-Instance 'b' 4383 $replayEnv
        Write-Host "replaying in a (pid $($a.Id)) and b (pid $($b.Id))"
    }
    'netplay' {
        foreach ($name in 'a', 'b') { Reset-Save $name }
        # One person can drive both: the host on the gamepad, the guest on the keyboard.
        $bConfig = Join-Path $np 'b\launcher.cfg'
        (Get-Content $bConfig) -replace '^p1_device=.*', 'p1_device=1' | Set-Content $bConfig -Encoding ascii
        # The host records both players' lockstep input, so a session can be
        # replayed offline (e.g. to reproduce a crash on another build).
        $a = Start-Instance 'a' 4382 @{ PSR_NETPLAY = 'host:7311'; PSR_INPUT_RECORD = (Join-Path $np 'a\netplay-input.bin');
                                         PSR_NETPLAY_LATENCY_MS = "$LatencyMs" }
        $b = Start-Instance 'b' 4383 @{ PSR_NETPLAY = 'join:127.0.0.1:7311'; PSR_NETPLAY_LATENCY_MS = "$LatencyMs" }
        Write-Host "host a (pid $($a.Id)), guest b (pid $($b.Id))"
    }
    'launch' {
        foreach ($name in 'a', 'b') { Reset-Save $name }
        $bConfig = Join-Path $np 'b\launcher.cfg'
        (Get-Content $bConfig) -replace '^p1_device=.*', 'p1_device=1' | Set-Content $bConfig -Encoding ascii
        $a = Start-Instance 'a' 4382 @{}
        $b = Start-Instance 'b' 4383 @{}
        Write-Host "a (pid $($a.Id)), b (pid $($b.Id))"
    }
    'compare' {
        $py = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
        & $py -I (Join-Path $PSScriptRoot 'netplay_compare.py') (Join-Path $np 'rec\hash.txt') `
            (Join-Path $np 'a\hash.txt') (Join-Path $np 'b\hash.txt')
    }
}
