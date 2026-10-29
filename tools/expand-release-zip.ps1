# Extract a locally built Windows release without tar's locale-dependent names.
param(
    [Parameter(Mandatory)][string]$ArchivePath,
    [Parameter(Mandatory)][string]$DestinationPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archiveFile = [System.IO.Path]::GetFullPath($ArchivePath)
$destination = [System.IO.Path]::GetFullPath($DestinationPath)
$directory = Get-Item -LiteralPath $destination
if (-not $directory.PSIsContainer -or
    ($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
    @(Get-ChildItem -LiteralPath $destination -Force).Count -ne 0) {
    throw 'Release extraction requires an empty, non-link destination directory.'
}
$prefix = $destination.TrimEnd([char[]]'\/') + [System.IO.Path]::DirectorySeparatorChar
$targets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$zip = [System.IO.Compression.ZipFile]::OpenRead($archiveFile)
try {
    # Validate EVERY name before writing anything. ZIP names use '/' regardless
    # of the host OS; reject Windows drive/ADS paths and ambiguous aliases too.
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName
        $parts = $name.TrimEnd('/').Split('/')
        if ([string]::IsNullOrEmpty($name) -or $name.StartsWith('/') -or
            $name -match '[\\:\x00-\x1f<>"|?*]' -or
            @($parts | Where-Object {
                $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' -or $_ -match '[. ]$' -or
                $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)'
            }).Count -ne 0) {
            throw "Unsafe archive member: $name"
        }
        $target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($destination, $name))
        if (-not $target.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            -not $targets.Add($target.TrimEnd([char[]]'\/'))) {
            throw "Unsafe or duplicate archive member: $name"
        }
        $unixType = ($entry.ExternalAttributes -shr 16) -band 0xf000
        if ($unixType -eq 0xa000 -or ($entry.ExternalAttributes -band 0x400)) {
            throw "Archive links are not supported in Windows release packages: $name"
        }
    }
    [System.IO.Compression.ZipFileExtensions]::ExtractToDirectory($zip, $destination)
} finally {
    $zip.Dispose()
}
