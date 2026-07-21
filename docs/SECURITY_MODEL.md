# Security model

## Threat: hostile PDF files

All parsing happens in PDFium (memory-safe-ish C++, heavily fuzzed upstream,
but not sandboxed here). Mitigations now: prebuilt current PDFium; document
bytes only, no scripting, no external resource loading. Planned (M16):
fuzzing our wrapper boundary, crash containment.

## Threat: network exfiltration

None possible: the M1 binary contains no network code and links no network
libraries. Offline-first is enforced by construction; future connectors live
in separate modules that core code cannot depend on (see ARCHITECTURE.md).

## Threat: file loss

M1 never writes documents. From M4 on, the safe-save protocol applies
(temp write, validate, atomic replace, source preserved on failure).

## Secrets

None exist in this codebase. Connector credentials (M14+) will use Windows
Credential Manager; never files, logs, or the repository.

## Reporting

See SECURITY.md.
