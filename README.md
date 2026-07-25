# ANGRA Acrobat

An offline-first Windows PDF workstation.

> **Personal, non-commercial use only.** Source is public to read and build,
> but you may not sell, sublicense, or use it commercially. See [LICENSE](LICENSE).
> Third-party libraries (Qt, PDFium, qpdf) keep their own licenses — see
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Status: M0+M1 — foundation and basic viewer

Open PDF, render pages, previous/next, zoom in/out, fit page, fit width,
page indicator, loading and error states, About. Everything runs locally;
the application contains no network code.

## Build

Prerequisites: Visual Studio 2022 (MSVC), CMake 3.28+, Ninja, Qt 6.7+,
prebuilt PDFium (e.g. https://github.com/bblanchon/pdfium-binaries —
`pdfium-win-x64.tgz`, extracted so it contains `include/`, `lib/`, `bin/`).

```powershell
$env:PDFIUM_DIR = "C:/pdfium"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/msvc2022_64"
cmake --build build
```

Executable: `build/ANGRA.exe` (pdfium.dll is copied beside it automatically).

## Test

```powershell
pwsh scripts/make-test-pdf.ps1   # regenerates resources/test.pdf
ctest --test-dir build --output-on-failure
```

Qt's bin directory must be on PATH (or run windeployqt) so the test
executable can load Qt DLLs.

## Package

```powershell
windeployqt build/ANGRA.exe --dir build/deploy --no-translations
Copy-Item "$env:PDFIUM_DIR/bin/pdfium.dll" build/deploy/
iscc installer/angra-setup.iss          # -> dist/windows/ANGRA-Acrobat-Setup-x64.exe
pwsh scripts/make-portable.ps1          # -> dist/windows/ANGRA-Acrobat-Portable-x64.zip
pwsh scripts/gen-sbom.ps1               # -> dist/windows/SBOM.spdx.json
pwsh scripts/gen-checksums.ps1          # -> dist/windows/SHA256SUMS.txt
```

## Documentation

- [docs/PRODUCT_SPEC.md](docs/PRODUCT_SPEC.md)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md)
- [docs/STATUS.md](docs/STATUS.md)
- [docs/ROADMAP.md](docs/ROADMAP.md)
- [docs/KNOWN_LIMITATIONS.md](docs/KNOWN_LIMITATIONS.md)
- [docs/SECURITY_MODEL.md](docs/SECURITY_MODEL.md)
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md)
- [docs/TESTING.md](docs/TESTING.md)
- [docs/RELEASE.md](docs/RELEASE.md)

## License

Personal, non-commercial use only — see [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
