# Status

All feasible offline features are source-complete: viewer suite, annotations,
forms fill, page organization, conversions, redaction (rasterize), qpdf-backed
protection (encrypt/decrypt/sanitize/optimize/repair), compare, local share,
CLI batch. Compile verification pends CI (no local toolchain).

## Exact commands

    $env:PDFIUM_DIR = "C:/pdfium"
    $env:QPDF_DIR   = "C:/qpdf"    # optional; enables Protect/Tools extras
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt>/msvc2022_64"
    cmake --build build
    pwsh scripts/make-test-pdf.ps1
    ctest --test-dir build --output-on-failure

## Next vertical slice

Push to GitHub, fix whatever the first CI run flags, cut v0.2.0 draft release.
