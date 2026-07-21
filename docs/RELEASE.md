# Release

1. Update version in src/Theme.h, CMakeLists.txt, installer/angra-setup.iss.
2. Update docs (STATUS, FEATURE_MATRIX, KNOWN_LIMITATIONS).
3. Tag: git tag v0.x.y && git push --tags.
4. CI builds, tests, packages installer + portable ZIP + SHA256SUMS.txt +
   SBOM.spdx.json, uploads artifacts, creates a DRAFT GitHub release.
5. Review the draft, verify checksums, publish manually.

Signing status: binaries and installer are NOT code-signed. State this in
every release note. Users verify via SHA256SUMS.txt.
