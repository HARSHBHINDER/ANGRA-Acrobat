# Status

Active milestone: M1 complete pending first real build on a machine with the
toolchain installed. Next: M2 (production viewer).

## What works

Single-page viewer: open (dialog, Ctrl+O, or file argument), render at any
zoom, prev/next, fit page/width, page indicator, error and loading states,
About. Test: tests/test_pdfdocument.cpp via ctest.

## Exact commands

    $env:PDFIUM_DIR = "C:/pdfium"
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt>/msvc2022_64"
    cmake --build build
    pwsh scripts/make-test-pdf.ps1
    ctest --test-dir build --output-on-failure

## Report template (every run)

CURRENT MILESTONE / SELECTED VERTICAL SLICE / ACCEPTANCE CRITERIA / COMPLETED /
CHANGED / ARCHITECTURE / BUILD / TESTS / PDF VALIDATION / INSTALLER /
SHARING AND CONNECTIVITY / SECURITY / ACCESSIBILITY / YAGNI REVIEW /
KNOWN LIMITATIONS / BLOCKERS / NEXT VERTICAL SLICE (exactly one).
