# Заливка файлов на Switch по FTP (sys-ftpd / ftpd на консоли, анонимный вход).
#
# Запуск из корня репозитория:
#   powershell -ExecutionPolicy Bypass -File tools\switch-ftp.ps1             # app + overlay + sysmodule
#   powershell -ExecutionPolicy Bypass -File tools\switch-ftp.ps1 app sys     # только выбранное
#   powershell -ExecutionPolicy Bypass -File tools\switch-ftp.ps1 -Pull switch/yamu-nx/app.log
#
# Адрес консоли — параметр -Ip (по умолчанию из переменной окружения SWITCH_FTP, например 192.168.1.50:5000).
# Прежняя версия файла с консоли уносится в backups/<цель>/<время>/ (на SD копий не остаётся),
# после заливки файл скачивается обратно и сверяется MD5.

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Targets = @('app', 'ovl', 'sys'),
    [string]$Pull,          # путь на SD: скачать файл вместо заливки
    [string]$Out,           # куда скачивать (по умолчанию .\switch-pull)
    [string]$Ip = $env:SWITCH_FTP
)

$ErrorActionPreference = 'Stop'
if (-not $Ip) { throw 'Укажите адрес консоли: -Ip 192.168.1.50:5000 или переменная окружения SWITCH_FTP' }
$repo = Split-Path -Parent $PSScriptRoot
$base = "ftp://$Ip"

# что -> откуда (относительно репозитория) -> куда на SD -> имя файла на SD
$map = @{
    app = @('yamu-nx.nro',            'switch/yamu-nx',                       'yamu-nx.nro')
    ovl = @('overlay/yamu-nx.ovl',    'switch/.overlays',                     'yamu-nx.ovl')
    sys = @('sysmodule/yamu-sys.nsp', 'atmosphere/contents/4200000000594D53', 'exefs.nsp')
}

function Ftp([string[]]$a) {
    $out = & curl.exe -s -S --max-time 300 @a 2>&1
    return @{ ok = ($LASTEXITCODE -eq 0); text = ($out -join "`n") }
}

function Md5([string]$path) { (Get-FileHash -Algorithm MD5 $path).Hash }

$probe = Ftp @('--max-time', '8', '--list-only', "$base/")
if (-not $probe.ok) { throw "Switch не отвечает по FTP ($Ip). Включён ли sys-ftpd / ftpd? $($probe.text)" }

if ($Pull) {
    $dir = if ($Out) { $Out } else { Join-Path $repo 'switch-pull' }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $dest = Join-Path $dir (Split-Path $Pull -Leaf)
    $r = Ftp @('-o', $dest, "$base/$Pull")
    if (-not $r.ok) { throw "Не скачалось: $($r.text)" }
    Write-Host "Скачано: $dest"
    return
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$failed = 0
foreach ($t in $Targets) {
    if (-not $map.ContainsKey($t)) { Write-Warning "Неизвестная цель: $t (есть: app, ovl, sys)"; $failed++; continue }
    $src = Join-Path $repo $map[$t][0]
    $remote = "$($map[$t][1])/$($map[$t][2])"
    if (-not (Test-Path $src)) { Write-Warning "$t`: нет файла $src"; $failed++; continue }

    # Прежняя версия — в репозиторий.
    $bakDir = Join-Path $repo "backups\$t\$stamp"
    New-Item -ItemType Directory -Force $bakDir | Out-Null
    $old = Join-Path $bakDir $map[$t][2]
    $r = Ftp @('-o', $old, "$base/$remote")
    if ($r.ok -and (Test-Path $old)) { Write-Host "    старая версия -> .\backups\$t\$stamp" }
    else { Remove-Item $bakDir -Recurse -Force -ErrorAction SilentlyContinue }

    $r = Ftp @('--ftp-create-dirs', '-T', $src, "$base/$remote")
    if (-not $r.ok) { Write-Warning "$t`: не залилось: $($r.text)"; $failed++; continue }

    $check = Join-Path $env:TEMP "yamu-ftp-check-$t"
    $r = Ftp @('-o', $check, "$base/$remote")
    $want = Md5 $src
    if (-not $r.ok -or (Md5 $check) -ne $want) { Write-Warning "$t`: MD5 не совпал после заливки"; $failed++ }
    else { Write-Host "OK  $t -> $remote  ($want)" }
    Remove-Item $check -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 }
