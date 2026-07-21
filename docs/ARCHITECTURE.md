# Architecture

## Current (M1)

    main.cpp (MainWindow: UI shell, actions, fit/zoom state)
        |
    PdfDocument (src/PdfDocument.h/.cpp - owns PDFium handle, load/render)
        |
    PDFium (prebuilt shared library)

- src/Theme.h holds design tokens (names, colors, zoom constants).
- pdfcore static library = PdfDocument; linked by app and tests.
- No raw PDFium handles escape PdfDocument (void* internally, never exposed).

## Target layering (grow into, do not pre-build)

UI -> view models -> commands -> document sessions -> domain services ->
backend adapters -> PDF libraries. Connectors (future) sit beside, never
under, core document code. Forbidden: PDF libraries calling UI; UI touching
raw PDF objects; core modules depending on network code.

## Dependency policy

Add a dependency only when the active milestone requires it. qpdf arrives at
M4 (page organization / safe structural saves), Tesseract at M6 (OCR).
