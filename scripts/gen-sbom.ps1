# Writes dist/windows/SBOM.spdx.json (SPDX 2.3, minimal but valid).
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force "dist\windows" | Out-Null
$sbom = [ordered]@{
    spdxVersion = "SPDX-2.3"
    dataLicense = "CC0-1.0"
    SPDXID = "SPDXRef-DOCUMENT"
    name = "angra-acrobat-0.1.0"
    documentNamespace = "https://github.com/angra/angra-acrobat/sbom/0.1.0"
    creationInfo = @{
        created = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        creators = @("Tool: gen-sbom.ps1")
    }
    packages = @(
        [ordered]@{ SPDXID = "SPDXRef-Package-angra-acrobat"; name = "angra-acrobat";
            versionInfo = "0.1.0"; licenseDeclared = "Apache-2.0"; downloadLocation = "NOASSERTION" },
        [ordered]@{ SPDXID = "SPDXRef-Package-qt6"; name = "Qt6"; versionInfo = "6.7";
            licenseDeclared = "LGPL-3.0-only"; downloadLocation = "https://www.qt.io" },
        [ordered]@{ SPDXID = "SPDXRef-Package-pdfium"; name = "PDFium"; versionInfo = "prebuilt";
            licenseDeclared = "Apache-2.0"; downloadLocation = "https://pdfium.googlesource.com/pdfium/" },
        [ordered]@{ SPDXID = "SPDXRef-Package-qpdf"; name = "qpdf"; versionInfo = "11.x";
            licenseDeclared = "Apache-2.0"; downloadLocation = "https://github.com/qpdf/qpdf" }
    )
}
$sbom | ConvertTo-Json -Depth 6 | Set-Content "dist\windows\SBOM.spdx.json"
Write-Host "Wrote dist\windows\SBOM.spdx.json"
