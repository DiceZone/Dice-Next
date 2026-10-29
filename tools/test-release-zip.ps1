# No bot/listener is started here. Exercise the exact extractor used by CI.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sandbox = Join-Path ([System.IO.Path]::GetTempPath()) ('dicenext-zip-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
$extractor = Join-Path $PSScriptRoot 'expand-release-zip.ps1'
$caseNumber = 0

function New-FixtureZip([string]$file, [string[]]$names, [bool]$symlink = $false) {
    $zip = [System.IO.Compression.ZipFile]::Open($file, [System.IO.Compression.ZipArchiveMode]::Create, [System.Text.Encoding]::UTF8)
    try {
        foreach ($name in $names) {
            $entry = $zip.CreateEntry($name)
            if ($symlink) { $entry.ExternalAttributes = -1577123840 } # Unix symlink (0xa1ff0000)
            $writer = [System.IO.StreamWriter]::new($entry.Open(), [System.Text.Encoding]::UTF8)
            try { $writer.Write('中文内容 / Unicode / 🎲') } finally { $writer.Dispose() }
        }
    } finally { $zip.Dispose() }
}

function Assert-Rejected([string[]]$names, [bool]$symlink = $false) {
    $script:caseNumber++
    $file = Join-Path $sandbox "invalid-$script:caseNumber.zip"
    $dest = Join-Path $sandbox "invalid-$script:caseNumber"
    New-Item -ItemType Directory -Path $dest | Out-Null
    New-FixtureZip $file (@('package/valid.txt') + $names) $symlink
    $rejected = $false
    try { & $extractor -ArchivePath $file -DestinationPath $dest } catch { $rejected = $true }
    if (-not $rejected) { throw "Extractor accepted unsafe fixture: $names" }
    if (@(Get-ChildItem -LiteralPath $dest -Force).Count -ne 0) { throw "Wrote files before validating: $names" }
}

try {
    # The archive path itself also contains spaces, apostrophes and brackets.
    $file = Join-Path $sandbox "发行包 [UTF-8] ' test.zip"
    $dest = Join-Path $sandbox '解压目录 [验收]'
    New-Item -ItemType Directory -Path $dest | Out-Null
    $names = @('DiceNext-beta/使用说明.txt', 'DiceNext-beta/data/helpdoc/COC/COC技能查询.json',
        'DiceNext-beta/data/deck/溯洄系内置牌堆.json', 'DiceNext-beta/图像 🎲.txt', 'DiceNext-beta/web/dist/v2/index.html')
    New-FixtureZip $file $names
    & $extractor -ArchivePath $file -DestinationPath $dest
    foreach ($name in $names) {
        $content = [System.IO.File]::ReadAllText((Join-Path $dest $name))
        if ($content -ne '中文内容 / Unicode / 🎲') { throw "Filename/content changed: $name" }
    }
    $rejected = $false
    try { & $extractor -ArchivePath $file -DestinationPath $dest } catch { $rejected = $true }
    if (-not $rejected) { throw 'Extractor accepted an occupied destination.' }
    foreach ($name in @('../escape.txt', 'package/../../escape.txt', '/absolute.txt',
        'C:/absolute.txt', 'C:relative.txt', '\\server\share\file', 'package\..\escape.txt',
        'package/data.txt:stream', 'package/../escape.txt', 'package/./file', 'package/NUL.txt',
        'package/name. /file', "package/line`nbreak.txt")) {
        Assert-Rejected @($name)
    }
    Assert-Rejected @('package/Case.txt', 'package/case.txt')
    Assert-Rejected @('package/link') $true
    $badZip = Join-Path $sandbox 'corrupt.zip'
    [System.IO.File]::WriteAllText($badZip, 'not a ZIP archive')
    $badDest = Join-Path $sandbox 'corrupt'
    New-Item -ItemType Directory -Path $badDest | Out-Null
    $rejected = $false
    try { & $extractor -ArchivePath $badZip -DestinationPath $badDest } catch { $rejected = $true }
    if (-not $rejected) { throw 'Extractor accepted a corrupt archive.' }
    Write-Host 'PASS Windows ZIP extraction: Unicode names/content, unsafe paths, links, duplicates, occupied destination and corrupt ZIP'
} finally {
    # Only this test's uniquely named directory, never the temp directory itself.
    if ([System.IO.Path]::GetFileName($sandbox) -notmatch '^dicenext-zip-test-[0-9a-f]{32}$') { throw 'Unsafe cleanup target' }
    Remove-Item -LiteralPath $sandbox -Recurse -Force
}
