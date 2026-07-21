# Generates resources/test.pdf - minimal 2-page PDF, original content, redistributable.
$ErrorActionPreference = "Stop"
$resDir = Join-Path $PSScriptRoot "..\resources"
New-Item -ItemType Directory -Force $resDir | Out-Null

function StreamObj([int]$num, [string]$text) {
    $s = "BT /F1 24 Tf 72 720 Td ($text) Tj ET"
    "$num 0 obj`n<< /Length $($s.Length) >>`nstream`n$s`nendstream`nendobj`n"
}

$objs = @(
    "1 0 obj`n<< /Type /Catalog /Pages 2 0 R >>`nendobj`n",
    "2 0 obj`n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>`nendobj`n",
    "3 0 obj`n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 5 0 R /Resources << /Font << /F1 6 0 R >> >> >>`nendobj`n",
    "4 0 obj`n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 7 0 R /Resources << /Font << /F1 6 0 R >> >> >>`nendobj`n",
    (StreamObj 5 "ANGRA Acrobat test page 1"),
    "6 0 obj`n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>`nendobj`n",
    (StreamObj 7 "ANGRA Acrobat test page 2")
)

$header = "%PDF-1.4`n"
$body = ""
$offsets = @()
foreach ($o in $objs) { $offsets += ($header.Length + $body.Length); $body += $o }
$xrefPos = $header.Length + $body.Length
$xref = "xref`n0 8`n0000000000 65535 f `n"
foreach ($off in $offsets) { $xref += ("{0:D10} 00000 n `n" -f $off) }
$trailer = "trailer`n<< /Size 8 /Root 1 0 R >>`nstartxref`n$xrefPos`n%%EOF`n"

$bytes = [System.Text.Encoding]::ASCII.GetBytes($header + $body + $xref + $trailer)
$outPath = Join-Path (Resolve-Path $resDir) "test.pdf"
[System.IO.File]::WriteAllBytes($outPath, $bytes)

# Self-check: startxref must point at the literal token 'xref'
$check = [System.Text.Encoding]::ASCII.GetString($bytes, $xrefPos, 4)
if ($check -ne "xref") { throw "xref offset self-check failed: got '$check'" }
Write-Host "Wrote $outPath ($($bytes.Length) bytes)"
