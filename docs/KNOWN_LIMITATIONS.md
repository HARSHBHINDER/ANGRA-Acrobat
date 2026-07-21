# Known limitations

- Rendering, thumbnails, search, and conversions are synchronous on the UI
  thread; big documents cause visible pauses. Async + cancellation is the
  next slice.
- Whole file lives in RAM (FPDF_LoadMemDocument).
- Thumbnails disabled above 200 pages (sync generation cost).
- Search is page-granular: highlights all matches on a page; F3 jumps pages,
  not individual matches.
- Annotations render via PDFium's built-in appearance handling; some viewers
  may not show them until they generate appearances themselves.
- Annotation editing/deletion not implemented; ink strokes commit one at a
  time with fixed colors.
- Saving always writes a full validated copy (never incremental); signatures
  in source files will not survive editing (none can be created yet either).
- No settings persistence except recent-file list.
- Binaries are not code-signed; SmartScreen will warn.
- Blocked features and why: see FEATURE_MATRIX.md - nothing there is faked.
- Not yet compiled locally (no toolchain on this machine); CI builds it.
