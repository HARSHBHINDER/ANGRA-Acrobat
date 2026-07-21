# Feature matrix

## Done (all local, all offline)

| Feature | Notes |
|---------|-------|
| Open (dialog/Ctrl+O/arg/recent), password prompt, tabs | duplicate-session guard |
| Render, zoom, fit, thumbnails, page indicator | hidpi-aware |
| Search (highlighted, F3), selection + copy, bookmarks, links | external URLs confirmed first |
| Print, document properties | |
| Annotations: highlight, note, ink, rectangle; flatten | |
| Form filling | click + keyboard via PDFium formfill; values persist on save |
| Page ops: delete, rotate, extract, insert, merge, split | |
| Safe save | serialize -> validate -> atomic replace |
| Convert: images/text -> PDF; PDF -> PNG/JPG/text | UI + CLI |
| Crop page, rearrange (move page), extract embedded images | |
| Add page numbers, text watermark, free text, place signature image | |
| Edit existing text (replace/delete a text run) + add text | PDFium text objects |
| CLI batch | --to-text, --to-images, --merge |
| Redaction | rasterize-and-replace page; text provably destroyed (tested) |
| Encryption (AES-256), decrypt, sanitize, optimize, repair | via qpdf; build-optional (QPDF_DIR), CI enables it |
| Compare | page-level text diff between two tabs |
| Share (local) | copy file/path to clipboard, show in Explorer |
| Text comparison | line-set diff; see ponytail note in source |

## Deliberately not built (and why - nothing is faked)

| Feature | Reason |
|---------|--------|
| OCR + scanning | needs Tesseract/Leptonica + WIA binaries; not available to this build |
| Digital signatures | CNG + byte-range signing must be done correctly or not at all |
| Form creation | fill works; authoring is a separate editor slice |
| Continuous scroll / async render | UI rework; next slice |
| Sharing connectors / sync | network features; offline-first spec keeps them optional |
| Fuzzing harness | M16; needs a build machine first |
