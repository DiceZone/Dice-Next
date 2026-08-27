# ─────────────────────────────────────────────────────────────
#  DiceNext 一键打包脚本
#  把运行所必须的 release 打包成 zip → dice-next\release\
#  文件名：DiceNext-beta-3.0.0-NNN-windows-amd64-YYYY-MM-DD-HHMMSS.zip
#  用法：在 dice-next\ 目录右键「用 PowerShell 运行」，或：
#        powershell -ExecutionPolicy Bypass -File package.ps1
# ─────────────────────────────────────────────────────────────
param(
    [ValidateSet('amd64', 'arm64')]
    [string]$Architecture = 'amd64'
)

$ErrorActionPreference = 'Stop'
$root      = Split-Path -Parent $MyInvocation.MyCommand.Path
$server    = Join-Path $root 'server'
$buildDir  = if ($Architecture -eq 'arm64') { 'build-arm64' } else { 'build' }
$relDir    = Join-Path $server "$buildDir\Release"
$exe       = Join-Path $relDir 'dice-next-server.exe'
$launcher  = Join-Path $relDir 'dice-next.exe'
$utf8      = New-Object System.Text.UTF8Encoding($false)       # UTF-8 no BOM

function Resolve-ProjectInput([string]$envName, [string]$projectName, [string]$legacyPath) {
    $candidate = [Environment]::GetEnvironmentVariable($envName)
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = Join-Path (Split-Path -Parent $root) $projectName
    }
    if (-not (Test-Path $candidate)) { $candidate = Join-Path $root $legacyPath }
    return (Resolve-Path $candidate -ErrorAction SilentlyContinue).Path
}

# CI 通过环境变量传入同级仓库；保留旧路径回退，方便从历史单仓库升级。
$webRoot  = Resolve-ProjectInput 'DICENEXT_WEB_ROOT' 'Dice-Next-WebUI' 'web'
$docsRoot = Resolve-ProjectInput 'DICENEXT_DOC_ROOT' 'Dice-Next-Doc' 'docs'
$webDist  = if ($webRoot) { Join-Path $webRoot 'dist' } else { $null }
$defaultData = Join-Path $server 'resources\default-data'

function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $exe))     { Fail "未找到 $exe，请先编译后端 (cmake --build ...)" }
if (-not (Test-Path $launcher)){ Fail "未找到 $launcher，请先重新编译 Windows 管理器" }
if (-not $webRoot -or -not (Test-Path $webDist)) { Fail "未找到前端 dist，请先在 Dice-Next-WebUI 构建，或设置 DICENEXT_WEB_ROOT" }
if (-not $docsRoot) { Fail "未找到文档项目，请设置 DICENEXT_DOC_ROOT 或放在同级 Dice-Next-Doc" }
foreach ($requiredDir in 'decks','helpdoc','plugins\js') {
    if (-not (Test-Path (Join-Path $defaultData $requiredDir))) {
        Fail "默认发行资源不完整：缺少 server\resources\default-data\$requiredDir"
    }
}
if (-not (Test-Path (Join-Path $server 'resources\update-mirrors.json'))) {
    Fail "默认发行资源不完整：缺少 server\resources\update-mirrors.json"
}
foreach ($demo in 'seal_demo.js','checkin.js','cfg_deck_demo.js') {
    if (-not (Test-Path (Join-Path $defaultData "plugins\js\$demo"))) {
        Fail "默认发行资源不完整：缺少示例插件 $demo"
    }
}

# 版本实现文件必须不晚于 exe；成功链接后，构建号会回写到计数器。
$versionSource = Join-Path $server "$buildDir\generated\version_build.cpp"
if (-not (Test-Path $versionSource)) { Fail "未找到 $versionSource，请先重新编译后端" }
$versionSourceText = Get-Content $versionSource -Raw
if ($versionSourceText -notmatch 'int buildNumber\(\) \{ return ([0-9]+); \}') { Fail "无法读取已编译构建号" }
$compiledBuild = [int]$Matches[1]
if ((Get-Item $exe).LastWriteTimeUtc -lt (Get-Item $versionSource).LastWriteTimeUtc) {
    Fail "exe 比版本源文件旧；请重新编译后端，避免包名与 .bot 版本不一致"
}

# ── 版本号 + 构建号 + 时间戳 ─────────────────────────────────
$ver = '3.0.0'
$cmake = Get-Content (Join-Path $server 'CMakeLists.txt') -Raw
if ($cmake -match 'project\(dice-next-server\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { $ver = $Matches[1] }
$build = '000'
$counter = Join-Path $server 'build_counter.txt'
if (Test-Path $counter) {
    $c = (Get-Content $counter -Raw).Trim()       # 形如 "3.0:64"
    if ($c -match ':\s*([0-9]+)') {
        $counterBuild = [int]$Matches[1]
        if ($counterBuild -ne $compiledBuild) { Fail "构建计数器($counterBuild) 与 exe 内构建号($compiledBuild) 不一致" }
        $build = '{0:D3}' -f $compiledBuild
    }
}
$ts   = Get-Date -Format 'yyyy-MM-dd-HHmmss'
$name = "DiceNext-beta-$ver-$build-windows-$Architecture-$ts"

$out   = [Environment]::GetEnvironmentVariable('DICENEXT_RELEASE_ROOT')
if ([string]::IsNullOrWhiteSpace($out)) { $out = Join-Path $root 'release' }
$stageRoot = Join-Path ([IO.Path]::GetTempPath()) ("dicenext-package-" + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $stageRoot 'DiceNext-beta'  # 内层文件夹固定，避免解压出无数版本文件夹
New-Item -ItemType Directory -Force -Path $out   | Out-Null
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Write-Host "打包 $name ..." -ForegroundColor Cyan

# ── 1. 管理器 / 核心程序 + 包内运行库 ─────────────────────────
# Windows 会在进入 main() 前加载依赖 DLL，不能由主程序在运行后再设置
# DLL 搜索路径。因此顶层管理器先设置 PATH，再拉起 app/ 内的核心程序。
Copy-Item $launcher (Join-Path $stage 'dice-next.exe')
# 在线升级时替换 dice-next.exe 的维护进程不能从该路径运行，因此随包发出一份
# 独立的固定命名副本。运行期不再复制自身：把可执行文件复制成新名字再立刻拉起
# 是投放器（dropper）的典型行为，会被信誉类杀毒引擎判为恶意。
Copy-Item $launcher (Join-Path $stage 'dice-next-maintenance.exe')
$appStage = Join-Path $stage 'app'
New-Item -ItemType Directory -Force -Path $appStage | Out-Null
Copy-Item $exe (Join-Path $appStage 'dice-next-core.exe')
$libStage = Join-Path $stage 'lib'
New-Item -ItemType Directory -Force -Path $libStage | Out-Null
Get-ChildItem (Join-Path $relDir '*.dll') | Copy-Item -Destination $libStage
$dllCount = (Get-ChildItem (Join-Path $libStage '*.dll') | Measure-Object).Count

# MSVC 运行库必须与 core 程序同目录（app\），放 lib\ 是无效的：Windows 解析 exe 的
# 静态导入时先搜 exe 所在目录，再搜 System32，最后才轮到 PATH——而管理器只把 lib\
# 加进 PATH。也就是说 lib\ 里的这三个 DLL 从来没被加载过，实际生效的一直是用户机器
# System32 里的那份。微软要求运行库版本不得早于编译所用的工具集，否则不保证二进制
# 兼容；用户机器上的旧运行库正是崩溃报告里 MSVCP140 读空指针的来源。放进 app\ 后
# exe 目录优先，随包发出的这份稳定胜出。
#
# 顶层 dice-next.exe 与 dice-next-maintenance.exe 是静态链接（/MT），不需要这些。
$runtimeDirs = @()
$redistArch = if ($Architecture -eq 'arm64') { 'arm64' } else { 'x64' }
foreach ($vsRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
    if (-not $vsRoot) { continue }
    foreach ($edition in 'BuildTools','Community','Professional','Enterprise') {
        $redistRoot = Join-Path $vsRoot "Microsoft Visual Studio\2022\$edition\VC\Redist\MSVC"
        if (-not (Test-Path $redistRoot)) { continue }
        # 版本号倒序，优先取最新的重发行包（与工具集同代或更新）。
        foreach ($verDir in (Get-ChildItem $redistRoot -Directory | Sort-Object Name -Descending)) {
            $candidate = Join-Path $verDir.FullName "$redistArch\Microsoft.VC143.CRT"
            if (Test-Path $candidate) { $runtimeDirs += $candidate }
        }
    }
}
# System32 只在架构一致时能兜底；x64 主机打 arm64 包时它是错的架构。
if ($Architecture -ne 'arm64' -or $env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    $runtimeDirs += (Join-Path $env:SystemRoot 'System32')
}
foreach ($d in 'vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll') {
    $copied = $false
    foreach ($runtimeDir in $runtimeDirs) {
        $source = Join-Path $runtimeDir $d
        if (Test-Path $source) { Copy-Item $source $appStage -Force; $copied = $true; break }
    }
    if (-not $copied) { Fail "缺少 MSVC 运行库 $d（$Architecture）；请安装 VS 2022 生成工具的 C++ 重发行包" }
}

# ── 2. i18n / 内建内容资源 / 前端 ────────────────────────────
# 不再打包配置文件 — 首次运行会自动生成，避免升级覆盖用户自定义内容
Copy-Item (Join-Path $server 'i18n') (Join-Path $stage 'i18n') -Recurse   # i18n 仍是程序资源，留根
# 在线升级镜像清单。放在包内数据文件而不是可执行文件里，用户也可以自行增删。
Copy-Item (Join-Path $server 'resources\update-mirrors.json') (Join-Path $stage 'update-mirrors.json')
# 默认发行资源必须来自受 Git 跟踪的 resources/default-data，绝不能从
# server/data 读取；后者是本地运行数据，既会被 .gitignore 排除，也可能
# 把开发机上的用户牌堆或插件误带进发行包。
Copy-Item (Join-Path $defaultData 'decks') (Join-Path $stage 'decks') -Recurse
$dataStage = Join-Path $stage 'data'
New-Item -ItemType Directory -Force -Path $dataStage | Out-Null
Copy-Item (Join-Path $defaultData 'helpdoc') (Join-Path $dataStage 'helpdoc') -Recurse
Copy-Item (Join-Path $defaultData 'plugins') (Join-Path $dataStage 'plugins') -Recurse
# 人物卡模板是随程序更新的内建资源；放在根目录，避免与 data 下的用户模板混淆。
Copy-Item (Join-Path $server 'card-templates') (Join-Path $stage 'card-templates') -Recurse
# 开发计划页 / 指令表页 需要读取 docs 下的文件
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'docs') | Out-Null
foreach ($docFile in 'roadmap.md','commands.json') {
    $src = Join-Path $docsRoot $docFile
    if (Test-Path $src) { Copy-Item $src (Join-Path $stage "docs\$docFile") }
}
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'web') | Out-Null
Copy-Item $webDist (Join-Path $stage 'web\dist') -Recurse

# ── 3. 使用说明（中文，UTF-8 no BOM） ────────────────────────
$port = 18088 # 默认端口，与 config\server.json 保持一致
$readme = @"
DiceNext $ver($build) — Windows $Architecture 公测版使用说明
=======================================

【运行】
1. 解压本压缩包到任意文件夹（路径建议不含特殊字符）。
2. 双击 dice-next.exe 启动服务器。首次启动会自动创建配置文件。
3. 浏览器打开管理后台： http://localhost:$port

【接入 QQ 机器人】
OneBot v11：
方式 A（推荐，网页操作）：
  - 打开后台 → 左侧「适配器」→ 新建连接 →
    选 正向/反向 WS，填你的 OneBot 端点（如 NapCat/LLOneBot）与 Token → 保存并启用。
方式 B（改配置文件）：
  - 编辑 config\adapters.json，填好 endpoint / access_token，
    把 enabled 改为 true，重启即可。

QQ 官方机器人 2.0：
  - 打开后台 → 左侧「适配器」→ 新建连接 → 选「QQ 官方机器人 2.0」。
  - 可填写 AppID 与 AppSecret，或点击「扫码绑定 QQ 官方机器人」并用手机 QQ 扫码。
  - 该适配器使用官方 Gateway WebSocket，不需要配置 OneBot 或 Webhook。

【试用】
  - 后台「测试台」可不连 QQ 直接发指令试回复。
  - QQ 群里发： .help  .r3d6+2  .ra 侦查 60  .coc  .jrrp  .draw 塔罗
  - 支持简体/繁体/英文（后台右上角切换界面语言；群内可设回复语言）。

【说明】
  - 首次运行会自动生成 config\ 目录，各功能区分别保存在 server.json、adapters.json 等文件中。
  - 升级时解压完整覆盖所有文件即可，dice-next.exe / dice-next-maintenance.exe / app\ / lib\ / i18n / web\dist\ / data\ 都需要更新。你的 config\ 目录在解压包之外，不会丢失。
  - 端口 $port，可在 config\server.json 的 port 修改；若该端口被占用会自动顺延到下一个可用端口并写回配置（可右键托盘图标→打开网页面板，自动用正确端口）。
  - 同一套数据（同一安装目录）只能运行一个进程，重复启动会自动退出，避免数据冲突。
  - 仅支持 64 位 Windows 10/11。dice-next.exe 是管理器，负责启动 app\dice-next-core.exe；dice-next-maintenance.exe 与它内容相同，只在应用在线升级时被拉起，用来替换正在运行的管理器本身；第三方依赖收纳在 lib\，MSVC 运行库随 app\ 一同发出，通常无需额外安装。
  - 托管面板可设置系统环境变量 DICENEXT_UPDATE_RESTART=NO：管理器在应用在线升级后只退出，不会自行重启 core，避免与面板的进程守护逻辑冲突。
  - 当前处于公测阶段。建议优先通过 GitHub Issues 反馈问题，也欢迎加入 QQ 群 933145116 交流。

—— DiceZone / Shia
"@
[IO.File]::WriteAllText((Join-Path $stage '使用说明.txt'), $readme, (New-Object System.Text.UTF8Encoding($false)))

# ── 5. 压缩成 zip（用 UTF-8 条目名，避免中文文件名乱码） ─────
$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $zip,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $true,                       # 顶层包含 $name 文件夹
    [System.Text.Encoding]::UTF8)
# Copy-Item 会保留部分 vcpkg DLL 的只读属性；先解除只读再清理，
# 避免 ZIP 已生成却因临时目录清理失败而把整次打包标记为失败。
Get-ChildItem $stageRoot -Recurse -Force | Where-Object { -not $_.PSIsContainer } | ForEach-Object {
    $_.IsReadOnly = $false
}
Remove-Item $stageRoot -Recurse -Force

$sizeMB = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "[OK] 打包完成: release\$name.zip  ($sizeMB MB, $dllCount 个 DLL)" -ForegroundColor Green
