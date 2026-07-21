# Product specification (condensed)

ANGRA Acrobat is an offline-first Windows PDF workstation. All document
parsing, rendering, editing, OCR, conversion, signing, encryption, redaction,
comparison, optimization, and printing happen locally. Network use only on
explicit user action (share, connectors, update check) - none of which exist
yet.

Hard rules:
- Local document features never depend on network availability.
- Never upload document content, text, form values, or metadata automatically.
- Never overwrite the only valid copy of a document (safe-save protocol).
- Clean-room: public PDF spec + documented Windows APIs + open-source
  dependencies only. No proprietary code, icons, fonts, or wording.

Full feature roadmap: docs/ROADMAP.md. Current truth: docs/FEATURE_MATRIX.md.
