# Заливка файлов на Switch по MTP (DBI / sphaira в режиме MTP).
#
# Запуск из корня репозитория:
#   powershell -ExecutionPolicy Bypass -File tools\switch-deploy.ps1            # app + overlay + sysmodule
#   powershell -ExecutionPolicy Bypass -File tools\switch-deploy.ps1 ovl sys    # только выбранное
#   powershell -ExecutionPolicy Bypass -File tools\switch-deploy.ps1 -Pull atmosphere/crash_reports
#   powershell -ExecutionPolicy Bypass -File tools\switch-deploy.ps1 -CollectBak   # унести старые .bak с консоли в backups/
#   powershell -ExecutionPolicy Bypass -File tools\switch-deploy.ps1 -Backup       # всё YaMu-nx с карты в switch-pull/sd-<время>/
#
# Особенности MTP в Windows, ради которых и нужен скрипт:
#  - устройство не получает букву диска, ходим через Shell.Application (COM);
#  - Проводник показывает имена без расширения ("exefs" вместо "exefs.nsp");
#  - CopyHere НЕ перезаписывает существующий файл и молчит, поэтому старую версию сначала
#    переносим с консоли в репозиторий: backups/<цель>/<время>/ (на SD копий не остаётся);
#  - CopyHere асинхронный, поэтому ждём и сверяем MD5, скачав файл обратно.

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Targets = @('app', 'ovl', 'sys'),
    [string]$Pull,          # путь на SD: скачать файл или папку вместо заливки
    [string]$Out,           # куда скачивать (по умолчанию .\switch-pull)
    [switch]$NoVerify,
    [switch]$Backup,        # скачать всё YaMu-nx с карты в switch-pull/sd-<время>/ (пути как на SD)
    [switch]$CollectBak     # забрать с консоли в backups/ старые .bak, оставшиеся от прежних заливок
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# что -> откуда (относительно репозитория) -> куда на SD -> имя файла на SD
$map = @{
    app = @('yamu-nx.nro',                'switch/yamu-nx',                    'yamu-nx.nro')
    ovl = @('overlay/yamu-nx.ovl',        'switch/.overlays',                  'yamu-nx.ovl')
    sys = @('sysmodule/yamu-sys.nsp',     'atmosphere/contents/4200000000594D53', 'exefs.nsp')
}

$shell = New-Object -ComObject Shell.Application

function Get-SdRoot {
    $dev = $shell.Namespace(17).Items() | Where-Object { $_.Name -eq 'Switch' } | Select-Object -First 1
    if (-not $dev) { throw 'Switch не найден по MTP. Откройте DBI -> "Run MTP responder" и подключите кабель.' }
    $sd = $dev.GetFolder.Items() | Where-Object { $_.Name -like '*SD Card' -and $_.Name -notlike '*install*' } | Select-Object -First 1
    if (-not $sd) { throw 'На устройстве нет хранилища "SD Card".' }
    return $sd.GetFolder
}

# Элемент папки по имени: MTP может показывать его с расширением или без.
function Find-Item($folder, [string]$name) {
    $base = [IO.Path]::GetFileNameWithoutExtension($name)
    $items = @($folder.Items())
    $hit = $items | Where-Object { $_.Name -eq $name } | Select-Object -First 1
    if ($hit) { return $hit }
    $ext = [IO.Path]::GetExtension($name)
    # Без расширения: только файлы (папка "yamu-nx" рядом с файлом "yamu-nx" не подходит).
    # Сначала тот, чей тип называет расширение ("Файл OVL"), иначе единственный с таким именем.
    $files = @($items | Where-Object { $_.Name -eq $base -and -not $_.IsFolder })
    $typed = $files | Where-Object { $ext -and $_.Type -match [regex]::Escape($ext.TrimStart('.')) } | Select-Object -First 1
    if ($typed) { return $typed }
    if ($files.Count -eq 1) { return $files[0] }
    return $null
}

function Get-Folder($root, [string]$path, [switch]$Create) {
    $f = $root
    foreach ($seg in ($path -split '[\\/]' | Where-Object { $_ })) {
        $next = @($f.Items()) | Where-Object { $_.Name -eq $seg -and $_.IsFolder } | Select-Object -First 1
        if (-not $next) {
            if (-not $Create) { throw "На SD нет папки $path" }
            $f.NewFolder($seg)
            Start-Sleep -Milliseconds 500
            $next = @($f.Items()) | Where-Object { $_.Name -eq $seg } | Select-Object -First 1
            if (-not $next) { throw "Не удалось создать $seg в $path" }
        }
        $f = $next.GetFolder
    }
    return $f
}

function Wait-Item($folder, [string]$name, [int]$sec = 30) {
    for ($i = 0; $i -lt $sec * 4; $i++) {
        $it = Find-Item $folder $name
        if ($it) { return $it }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Wait-LocalFile([string]$path, [int]$sec = 60) {
    $last = -1
    for ($i = 0; $i -lt $sec * 4; $i++) {
        if (Test-Path $path) {
            $len = (Get-Item $path).Length
            if ($len -gt 0 -and $len -eq $last) { return $true }
            $last = $len
        }
        Start-Sleep -Milliseconds 250
    }
    return (Test-Path $path)
}

$backups = Join-Path $repo 'backups'
$stamp = Get-Date -Format yyyyMMdd-HHmmss

# Перенести файл с консоли в локальную папку: MoveHere копирует и удаляет на SD.
# Ждём, пока он появится здесь целиком и пропадёт там.
function Move-ToRepo($folder, $item, [string]$name, [string]$dest) {
    New-Item -ItemType Directory -Force $dest | Out-Null
    $shell.Namespace((Resolve-Path $dest).Path).MoveHere($item, 0x14)
    for ($i = 0; $i -lt 240; $i++) {
        Start-Sleep -Milliseconds 250
        $here = Get-ChildItem $dest -File | Where-Object { $_.Name -like "$([IO.Path]::GetFileNameWithoutExtension($name))*" } | Select-Object -First 1
        if ($here -and (Wait-LocalFile $here.FullName 5) -and -not (Find-Item $folder $name)) {
            # Проводник дописывает расширение ещё раз ("yamu-nx.ovl.ovl", "x.bak.nro") — возвращаем исходное имя.
            if ($here.Name -ne $name -and -not (Test-Path (Join-Path $dest $name))) { Rename-Item $here.FullName $name }
            return $true
        }
    }
    return $false
}

# Копирование с консоли идёт в фоне: ждём, пока число файлов и размер папки не перестанут
# меняться 20 секунд подряд (большие треки по MTP идут долго, а скрипт не должен выйти раньше).
function Wait-Stable([string]$dir) {
    $last = ''
    $since = Get-Date
    while ($true) {
        Start-Sleep 2
        $files = @(Get-ChildItem $dir -Recurse -File -ErrorAction SilentlyContinue)
        $now = "$($files.Count):$(($files | Measure-Object Length -Sum).Sum)"
        if ($now -ne $last) { $last = $now; $since = Get-Date; continue }
        if (((Get-Date) - $since).TotalSeconds -ge 20) { return }
    }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ('switch-deploy-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
$sd = Get-SdRoot

try {
    if ($CollectBak) {
        $moved = 0
        foreach ($t in $map.Keys) {
            $dir = $map[$t][1]
            $folder = try { Get-Folder $sd $dir } catch { $null }
            if (-not $folder) { continue }
            foreach ($it in @(@($folder.Items()) | Where-Object { -not $_.IsFolder -and $_.Name -like '*.bak' })) {
                $n = $it.Name
                if (Move-ToRepo $folder $it $n (Join-Path $backups "$t\from-sd")) { Write-Host "OK  $dir/$n -> backups\$t\from-sd"; $moved++ }
                else { Write-Warning "$dir/$n`: не удалось перенести" }
            }
        }
        Write-Host "Перенесено с консоли: $moved"
        return
    }

    if ($Pull) {
        if (-not $Out) { $Out = Join-Path (Get-Location) 'switch-pull' }
        New-Item -ItemType Directory -Force $Out | Out-Null
        $parent = Split-Path $Pull -Parent
        $leaf = Split-Path $Pull -Leaf
        $pf = if ($parent) { Get-Folder $sd $parent } else { $sd }
        $it = @($pf.Items()) | Where-Object { $_.Name -eq $leaf } | Select-Object -First 1
        if (-not $it) { $it = Find-Item $pf $leaf }
        if (-not $it) { throw "На SD нет $Pull" }
        $shell.Namespace((Resolve-Path $Out).Path).CopyHere($it, 0x14)
        Wait-Stable $Out
        Write-Host "Скачано в $Out"
        return
    }

    if ($Backup) {
        # Всё YaMu-nx с карты — в switch-pull/sd-<время>/ с теми же путями, что на SD,
        # чтобы потом скопировать содержимое этой папки в корень новой карты.
        $root = Join-Path $repo "switch-pull\sd-$stamp"
        $sets = @(
            @('switch', 'yamu-nx'),                          # приложение, токен, настройки, скачанное, тексты
            @('switch/.overlays', 'yamu-nx.ovl'),            # оверлей
            @('atmosphere/contents', '4200000000594D53')      # фоновый модуль, toolbox.json, флаг автозапуска
        )
        foreach ($s in $sets) {
            $pf = Get-Folder $sd $s[0]
            $it = @($pf.Items()) | Where-Object { $_.Name -eq $s[1] } | Select-Object -First 1
            if (-not $it) { $it = Find-Item $pf $s[1] }
            if (-not $it) { Write-Warning "На SD нет $($s[0])/$($s[1])"; continue }
            $dest = Join-Path $root ($s[0] -replace '/', '\')
            New-Item -ItemType Directory -Force $dest | Out-Null
            Write-Host "Копирую $($s[0])/$($s[1]) …"
            $shell.Namespace((Resolve-Path $dest).Path).CopyHere($it, 0x14)
            Wait-Stable $dest
        }
        # Проводник дописывает расширение ещё раз: "yamu-nx.ovl.ovl" → "yamu-nx.ovl"
        Get-ChildItem $root -Recurse -File | Where-Object { $_.Name -match '\.(nro|ovl|nsp)\.\1$' } |
            ForEach-Object { Rename-Item $_.FullName ($_.Name -replace '\.(nro|ovl|nsp)$', '') }
        $files = Get-ChildItem $root -Recurse -File
        $mb = [math]::Round(($files | Measure-Object Length -Sum).Sum / 1MB, 1)
        Write-Host "Готово: $($files.Count) файлов, $mb МБ в $root"
        return
    }

    $failed = 0
    foreach ($t in $Targets) {
        if (-not $map.ContainsKey($t)) { Write-Warning "Неизвестная цель '$t' (есть: $($map.Keys -join ', '))"; $failed++; continue }
        $src, $dir, $name = $map[$t]
        $local = Join-Path $repo $src
        if (-not (Test-Path $local)) { Write-Warning "$t`: нет файла $local — сначала соберите"; $failed++; continue }

        # копия с нужным именем: CopyHere берёт имя исходного файла
        $stage = Join-Path $tmp $t
        New-Item -ItemType Directory -Force $stage | Out-Null
        Copy-Item $local (Join-Path $stage $name)
        $hash = (Get-FileHash (Join-Path $stage $name) -Algorithm MD5).Hash

        $folder = Get-Folder $sd $dir -Create
        $old = Find-Item $folder $name
        if ($old) {
            # Перезаписать MTP не даёт — старую версию уносим с консоли в репозиторий (backups/<цель>/<время>/).
            $dest = Join-Path $backups "$t\$stamp"
            if (-not (Move-ToRepo $folder $old $name $dest)) { throw "$t`: не удалось забрать старый $dir/$name с консоли" }
            Write-Host "    старая версия -> $(Resolve-Path -Relative $dest)"
        }

        $folder.CopyHere($shell.Namespace($stage).ParseName($name), 0x14)
        $new = Wait-Item $folder $name
        if (-not $new) { Write-Warning "$t`: файл не появился в $dir"; $failed++; continue }

        if ($NoVerify) { Write-Host "OK  $t -> $dir/$name"; continue }
        $back = Join-Path $tmp "$t-back"
        New-Item -ItemType Directory -Force $back | Out-Null
        Start-Sleep 1
        $shell.Namespace($back).CopyHere($new, 0x14)
        $got = Get-ChildItem $back -File | Select-Object -First 1
        if (-not $got -or -not (Wait-LocalFile $got.FullName)) { Write-Warning "$t`: не удалось скачать обратно для проверки"; $failed++; continue }
        $got = Get-ChildItem $back -File | Select-Object -First 1
        $h2 = (Get-FileHash $got.FullName -Algorithm MD5).Hash
        if ($h2 -eq $hash) { Write-Host "OK  $t -> $dir/$name  ($hash)" }
        else { Write-Warning "$t`: MD5 не совпал ($h2 вместо $hash)"; $failed++ }
    }
    if ($Targets -contains 'sys') { Write-Host 'Модуль обновится после перезапуска (ovlSysmodules или перезагрузка консоли).' }
    if ($failed) { exit 1 }
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
