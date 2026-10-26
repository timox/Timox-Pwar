$ErrorActionPreference = 'Stop'
$manifest = Get-Content (Join-Path $PSScriptRoot 'import-manifest.json') -Raw | ConvertFrom-Json
foreach ($file in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $file.target))
    if (-not $path.StartsWith($PSScriptRoot + [IO.Path]::DirectorySeparatorChar)) {
        throw "Manifest path outside checkout: $($file.target)"
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) {
        throw "Source integrity mismatch: $($file.target)"
    }
}
Write-Output "Verified $($manifest.files.Count) source files."
