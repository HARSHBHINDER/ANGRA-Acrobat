# Contributing

- Build per README.md. Format with clang-format (config in repo).
- One focused change per PR; keep the repository buildable.
- New dependencies need an entry in docs/DEPENDENCIES.md (license, version,
  purpose, distribution requirements, maintenance status).
- No proprietary SDKs, trackers, analytics, or undocumented binaries.
- Tests: extend tests/ so a break in your change fails ctest.
