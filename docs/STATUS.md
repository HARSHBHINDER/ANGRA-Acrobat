# Status

Active milestone: compressed M2+M3+M4+M7 slice complete in source form -
production viewer (tabs, thumbs, search, selection, bookmarks, links,
password, print), annotations, page organization with safe save, and local
conversions. Awaiting first CI build for compile/test verification.

## Exact commands

    $env:PDFIUM_DIR = "C:/pdfium"
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="<Qt>/msvc2022_64"
    cmake --build build
    pwsh scripts/make-test-pdf.ps1
    ctest --test-dir build --output-on-failure

## Next vertical slice

Background rendering with cancellation (unblocks large-document UX and is
prerequisite for continuous scroll).
