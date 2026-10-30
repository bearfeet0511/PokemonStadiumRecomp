# Provisions lib/ (gitignored) at the refs this fork builds against, and pins
# the N64Recomp engine checkouts. Run after setup.bat, from anywhere:
#   powershell -ExecutionPolicy Bypass -File tools\setup_libs.ps1
#
# Refs come from the v0.4.5-beta release notes ("GPL corresponding source at
# those refs"); mstan's forks were not pushed after that, so these are also
# the newest published commits. The ares submodule inside N64Recomp points at
# an unpublished commit, so it is skipped: it is only needed for the dev-only
# ares oracle (WITH_ARES_BRIDGE=OFF in build_ssanne.bat).

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$lib = Join-Path $root 'lib'
$engineSha = '2b949c5c3f1c41dd76023e7626a6302a4a262a8a'

function Invoke-Git { & git @args; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }

function Get-Repo($name, $url, $ref) {
    $dir = Join-Path $lib $name
    if (-not (Test-Path (Join-Path $dir '.git'))) { Invoke-Git clone -q $url $dir }
    if ($ref) { Invoke-Git -C $dir checkout -q $ref }
    return $dir
}

New-Item -ItemType Directory -Force $lib | Out-Null

# Engine used to generate C from game.toml (n64recomp\, cloned by setup.bat).
$n64recomp = Join-Path $root 'n64recomp'
if (Test-Path (Join-Path $n64recomp '.git')) {
    Invoke-Git -C $n64recomp fetch -q --no-recurse-submodules origin $engineSha
    Invoke-Git -C $n64recomp checkout -q $engineSha
    Invoke-Git -C $n64recomp submodule update --init lib/ELFIO lib/fmt lib/rabbitizer lib/sljit lib/tomlplusplus
}

$runtime = Get-Repo 'N64ModernRuntime' 'https://github.com/mstan/N64ModernRuntime' 'd2aed7e'
Invoke-Git -C $runtime submodule update --init thirdparty/miniz thirdparty/o1heap thirdparty/xxHash N64Recomp
# The runtime's own engine copy must match the generator, since the generated
# code compiles against its headers.
$runtimeEngine = Join-Path $runtime 'N64Recomp'
Invoke-Git -C $runtimeEngine fetch -q --no-recurse-submodules https://github.com/N64Recomp/N64Recomp $engineSha
Invoke-Git -C $runtimeEngine checkout -q $engineSha
Invoke-Git -C $runtimeEngine submodule update --init lib/ELFIO lib/fmt lib/rabbitizer lib/sljit lib/tomlplusplus

$rt64 = Get-Repo 'rt64' 'https://github.com/mstan/rt64' 'b58c2f7'
Invoke-Git -C $rt64 submodule update --init --recursive

Get-Repo 'concurrentqueue' 'https://github.com/cameron314/concurrentqueue' $null | Out-Null
Get-Repo 'SlotMap' 'https://github.com/SergeyMakeev/SlotMap' $null | Out-Null

# pret's Makefile runs under Linux (tools/docker_disasm.sh), so the disasm
# checkout must keep LF endings even when core.autocrlf=true.
$disasm = Join-Path $root 'disasm'
foreach ($repo in $disasm, (Join-Path $disasm 'tools\n64splat')) {
    Invoke-Git -C $repo config core.autocrlf false
    Invoke-Git -C $repo config core.eol lf
}
Write-Host 'lib/ ready. If disasm was checked out with CRLF, re-checkout it:'
Write-Host '  git -C disasm rm -q --cached -r . ; git -C disasm reset -q --hard'
