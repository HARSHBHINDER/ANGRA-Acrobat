# Dependencies

| Dependency | Version | License | Purpose | Distribution | Maintenance |
|-----------|---------|---------|---------|--------------|-------------|
| Qt 6 (Core, Gui, Widgets) | 6.7+ | LGPL-3.0 | UI shell, imaging | dynamic link, DLLs shipped | active (Qt Project) |
| PDFium | prebuilt latest | Apache-2.0 | parse/render PDF | pdfium.dll shipped | active (Google) |
| CMake / Ninja / MSVC | current | build-only | build system | not shipped | active |
| Inno Setup 6 | 6+ | Inno license | installer build | not shipped | active |

Planned, not yet added (YAGNI): qpdf (M4), Tesseract+Leptonica (M6),
GoogleTest or Catch2 (when plain-assert tests stop being enough).

Rule: every new dependency gets a row here before it is merged.
