# Agent guide

Read before working: README.md, docs/PRODUCT_SPEC.md, docs/ARCHITECTURE.md,
docs/FEATURE_MATRIX.md, docs/STATUS.md, docs/ROADMAP.md,
docs/KNOWN_LIMITATIONS.md, docs/SECURITY_MODEL.md.

Rules:
- One narrow vertical slice per run. Build, test, document, keep buildable.
- YAGNI: add dependencies and abstractions only when the active milestone needs them.
- Offline-first: no network code in core document paths, ever.
- Safe save: never overwrite the only valid copy of a document.
- After each run update FEATURE_MATRIX.md, STATUS.md, KNOWN_LIMITATIONS.md and
  report using the structure in docs/STATUS.md.
