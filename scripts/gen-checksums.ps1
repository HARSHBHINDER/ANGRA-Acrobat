# Writes dist/windows/SHA256SUMS.txt covering every artifact in dist/windows.
$ErrorActionPreference = "Stop"
$dist = "dist\windows"
Get-ChildItem $dist -File | Where-Object Name -ne "SHA256SUMS.txt" |
    ForEach-Object { "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } |
    Set-Content "$dist\SHA256SUMS.txt"
Write-Host "Wrote $dist\SHA256SUMS.txt"
