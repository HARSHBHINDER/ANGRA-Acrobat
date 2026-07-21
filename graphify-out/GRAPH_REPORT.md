# Graph Report - angra-acrobat  (2026-07-21)

## Corpus Check
- 26 files · ~10,334 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 312 nodes · 660 edges · 22 communities (16 shown, 6 thin omitted)
- Extraction: 88% EXTRACTED · 12% INFERRED · 0% AMBIGUOUS · INFERRED: 82 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `4d9e7520`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- PdfDocument
- README.md
- MainWindow
- DocumentTab
- .render
- PdfDocument.h
- FPDF_FORMFILLINFO
- main.cpp
- run
- .afterStructureChange
- .openPath
- BufferWriter
- writeValidatedCopy
- .tabAt
- AGENTS.md
- CONTRIBUTING.md
- SECURITY.md

## God Nodes (most connected - your core abstractions)
1. `DocumentTab` - 81 edges
2. `MainWindow` - 73 edges
3. `PdfDocument` - 54 edges
4. `main()` - 22 edges
5. `doc()` - 19 edges
6. `load` - 11 edges
7. `pageCount` - 11 edges
8. `saveCopy` - 11 edges
9. `redactRasterize` - 10 edges
10. `runCli()` - 10 edges

## Surprising Connections (you probably didn't know these)
- `main()` --calls--> `load`  [INFERRED]
  tests/test_pdfdocument.cpp → src/PdfDocument.h
- `main()` --calls--> `createEmpty`  [INFERRED]
  tests/test_pdfdocument.cpp → src/PdfDocument.h
- `main()` --calls--> `pageCount`  [INFERRED]
  tests/test_pdfdocument.cpp → src/PdfDocument.h
- `main()` --calls--> `pageSizePoints`  [INFERRED]
  tests/test_pdfdocument.cpp → src/PdfDocument.h
- `main()` --calls--> `renderPage`  [INFERRED]
  tests/test_pdfdocument.cpp → src/PdfDocument.h

## Import Cycles
- None detected.

## Communities (22 total, 6 thin omitted)

### Community 0 - "PdfDocument"
Cohesion: 0.08
Nodes (65): FPDF_BOOKMARK, FPDF_DOCUMENT, FPDF_WIDESTRING, QImage, QSizeF, QVarLengthArray, QStringList, main() (+57 more)

### Community 1 - "README.md"
Cohesion: 0.05
Nodes (30): Architecture, Current (M1), Dependency policy, Target layering (grow into, do not pre-build), Dependencies, Deliberately not built (and why - nothing is faked), Done (all local, all offline), Feature matrix (+22 more)

### Community 2 - "MainWindow"
Cohesion: 0.05
Nodes (41): QAction, QMenu, MainWindow, m_bookmarkDock, m_bookmarkTree, m_closeTabAct, m_compareAct, m_copyAct (+33 more)

### Community 3 - "DocumentTab"
Cohesion: 0.08
Nodes (26): QPoint, QWidget, function, QList, QPointF, QPolygonF, DocumentTab, m_canvas (+18 more)

### Community 5 - ".render"
Cohesion: 0.21
Nodes (5): QColor, QEvent, QObject, QRectF, onChanged

### Community 6 - "PdfDocument.h"
Cohesion: 0.16
Nodes (12): QByteArray, QList, QString, PdfBookmark, children, page, title, PdfLinkHit (+4 more)

### Community 7 - "FPDF_FORMFILLINFO"
Cohesion: 0.13
Nodes (13): FPDF_FORMFILLINFO, FPDF_PAGE, FPDF_SYSTEMTIME, FPDF_TEXTPAGE, ffiInvalidate(), ffiKillTimer(), ffiLocalTime(), ffiSetTimer() (+5 more)

### Community 8 - "main.cpp"
Cohesion: 0.18
Nodes (9): QDockWidget, QLabel, QLineEdit, QListWidget, QMainWindow, QRubberBand, QScrollArea, QTabWidget (+1 more)

### Community 9 - "run"
Cohesion: 0.53
Nodes (9): function, QByteArray, QString, decrypt(), encrypt(), optimize(), repair(), run() (+1 more)

### Community 12 - "BufferWriter"
Cohesion: 0.50
Nodes (4): FPDF_FILEWRITE, BufferWriter, buf, QByteArray

### Community 13 - "writeValidatedCopy"
Cohesion: 0.40
Nodes (5): Op, QByteArray, encryptCopy(), qpdfCopy(), writeValidatedCopy()

## Knowledge Gaps
- **100 isolated node(s):** `p`, `t`, `buf`, `title`, `page` (+95 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **6 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `DocumentTab` connect `DocumentTab` to `PdfDocument`, `.buildToolbarAndMenus`, `.render`, `main.cpp`, `.afterStructureChange`, `.openPath`, `writeValidatedCopy`, `.tabAt`?**
  _High betweenness centrality (0.271) - this node is a cross-community bridge._
- **Why does `PdfDocument` connect `PdfDocument` to `DocumentTab`, `writeValidatedCopy`, `PdfDocument.h`?**
  _High betweenness centrality (0.222) - this node is a cross-community bridge._
- **Why does `MainWindow` connect `MainWindow` to `PdfDocument`, `.buildToolbarAndMenus`, `main.cpp`, `.openPath`, `.tabAt`?**
  _High betweenness centrality (0.219) - this node is a cross-community bridge._
- **Are the 18 inferred relationships involving `DocumentTab` (e.g. with `encryptCopy()` and `.buildToolbarAndMenus()`) actually correct?**
  _`DocumentTab` has 18 INFERRED edges - model-reasoned connections that need verification._
- **Are the 2 inferred relationships involving `PdfDocument` (e.g. with `.textToPdf()` and `writeValidatedCopy()`) actually correct?**
  _`PdfDocument` has 2 INFERRED edges - model-reasoned connections that need verification._
- **Are the 19 inferred relationships involving `main()` (e.g. with `QColor` and `addHighlight`) actually correct?**
  _`main()` has 19 INFERRED edges - model-reasoned connections that need verification._
- **What connects `p`, `t`, `buf` to the rest of the system?**
  _100 weakly-connected nodes found - possible documentation gaps or missing edges._