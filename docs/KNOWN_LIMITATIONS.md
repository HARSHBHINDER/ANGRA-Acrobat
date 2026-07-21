# Known limitations (M1)

- Rendering is synchronous on the UI thread; large pages block briefly.
  Async render coordinator arrives with M2 (cancellation requires it).
- Whole file is loaded into RAM (FPDF_LoadMemDocument). Streaming file
  access when multi-hundred-MB documents matter.
- Password-protected PDFs are detected but cannot be opened (M2).
- Single-page view only; no continuous scroll, search, selection, print (M2).
- No settings persistence yet; nothing to preserve across upgrades.
- Binaries are not code-signed; SmartScreen will warn.
- Fit-width reserves scrollbar width even when no scrollbar appears.
- No sandboxing around PDFium parsing (see SECURITY_MODEL.md).
