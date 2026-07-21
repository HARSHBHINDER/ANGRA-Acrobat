# Feature matrix

## Done (all local, all offline)

| Feature | Notes |
|---------|-------|
| Open PDF (dialog, Ctrl+O, file argument, Open Recent) | duplicate-session guard |
| Password-protected open | prompt loop |
| Tabs | close prompts on unsaved changes |
| Render page (hidpi-aware) | PDFium |
| Prev/Next, zoom, fit page/width, page indicator | |
| Page thumbnails | click to jump; capped at 200 pages |
| Text search | page-granular, all matches highlighted, F3 next |
| Text selection + copy | rubber-band; bounded-text extraction |
| Bookmarks panel | tree, click to jump |
| Links | in-document jump; external URLs need confirmation |
| Print | Qt print dialog, all pages |
| Document properties | metadata, page count, permission bits |
| Annotations: highlight, sticky note, ink (draw tool), rectangle | |
| Flatten annotations | |
| Page ops: delete, rotate, extract, insert-from-file, merge, split | |
| Safe save (Save Copy) | serialize -> validate -> atomic replace; source never harmed |
| Convert: images->PDF, text->PDF, PDF->images, PDF->text | |

## Blocked — needs a dependency or build feedback, deliberately not faked

| Feature | Blocker |
|---------|---------|
| OCR + scanning | Tesseract/Leptonica + WIA; not in repo yet (M6) |
| Encryption writing, permissions | PDFium cannot write encryption; qpdf (M9) |
| True redaction / sanitization | requires guaranteed content removal via qpdf full rewrite (M9); faking it would be a security hazard |
| Digital signatures | Windows CNG + incremental-save signing (M10) |
| Form filling/creation | PDFium formfill environment (M8) |
| Compare, optimize, repair | M12 |
| Continuous scroll, per-match find, annotation editing | M2/M3 polish |
| Sharing, connectors, sync | M13-M15; optional network features |
| CLI, batch, fuzzing | M16 |
