# ADR-001: PDF engine selection for fixed-layout editing

Status: **accepted, with two blocking constraints on the specification**
Date: 2026-07-25

## Context

The target is a fixed-layout page editor: positioned text runs, vector paths,
images, clipping, transparency, annotations, form widgets, and transforms —
not a reflowing document model. The specification requires stable references
from every editable object back to its indirect object number, generation
number, content-stream identity, and operator byte range.

The project already links **PDFium** (rendering, page objects) and **qpdf**
(encryption, sanitisation). Prefer what is already integrated unless it
demonstrably cannot do the job. This ADR establishes whether it can.

## Options considered

| Engine | Licence | Parse | Render | Edit page objects | Incremental write | Container surgery | Font work | Verdict |
|---|---|---|---|---|---|---|---|---|
| **PDFium** | BSD-3-Clause | Good | Excellent | Good | `FPDF_INCREMENTAL` flag | **None exposed** | Minimal | **Chosen** — content layer |
| **qpdf** | Apache-2.0 | Excellent | **None** | Operator-level via `TokenFilter` | Excellent | Excellent | None | **Chosen** — container layer |
| MuPDF | AGPL-3.0 / commercial | Excellent | Excellent | Good | Good | Good | Good | **Rejected — licence** |
| Poppler | GPL-2.0-or-later | Excellent | Excellent | Limited | Limited | Moderate | Good | **Rejected — licence** |
| iText 7 | AGPL-3.0 / commercial | Excellent | Moderate | Excellent | Excellent | Excellent | Excellent | **Rejected — licence** |
| PDFBox | Apache-2.0 | Excellent | Good | Good | Good | Good | Good | Rejected — JVM, not linkable into a Qt/C++ desktop binary |
| PDF.js | Apache-2.0 | Good | Good | **None** | None | None | Moderate | Rejected — no writer, wrong runtime |

### Why the copyleft engines are rejected

MuPDF, Poppler, and iText are each a better *single-engine* answer on technical
merit. All three are AGPL or GPL. This project ships under a custom
**personal-use, non-commercial** licence, which *forbids* commercial use.
AGPL/GPL *require* that recipients may use the work commercially. Both
conditions cannot hold at once, so linking them is a licence conflict — not
merely an obligation to publish source, which this project already does.

Artifex and Apryse sell commercial licences for MuPDF and iText. Both are
priced for funded products. Revisit only if the project relicenses to
GPL-compatible terms or buys a commercial licence. That is a business
decision, not an engineering one.

### Why PDFium alone is not sufficient

PDFium's public C API (`fpdfview.h`, `fpdf_edit.h`, `fpdf_annot.h`) exposes
pages and page objects. It does **not** expose:

- indirect object numbers or generation numbers;
- the cross-reference table or stream;
- content-stream operator sequences or byte ranges;
- object streams and prior incremental revisions;
- font programs at a level permitting subset expansion.

Every "source reference" the specification asks for therefore lives in qpdf,
not PDFium. This finding drives the decision below.

## Decision

**PDFium for the content layer; qpdf for the container layer.**

| Concern | Engine | Mechanism |
|---|---|---|
| Rasterisation | PDFium | `FPDF_RenderPageBitmap` |
| Page-object enumeration, z-order | PDFium | `FPDFPage_CountObjects` / `GetObject` |
| Glyph geometry, quads, angle | PDFium | `FPDFText_GetLooseCharBox`, `GetCharAngle`, `GetMatrix` |
| Unicode recovery | PDFium | text page + `ToUnicode` |
| Path geometry | PDFium | `FPDFPath_GetPathSegment` and friends |
| Image bytes, metadata | PDFium | `FPDFImageObj_GetImageDataRaw`, `GetImageMetadata` |
| Object movement | PDFium | `FPDFPageObj_Transform` |
| AcroForm fields, widgets | PDFium | `FPDFAnnot_*` plus the form-fill API |
| Tagged structure (read) | PDFium | `FPDF_StructTree_*` |
| **Indirect object identity** | **qpdf** | `QPDFObjectHandle::getObjectID` / `getGeneration` |
| **Operator-level source refs** | **qpdf** | `QPDFPageObjectHelper::filterContents` + `TokenFilter` |
| **Incremental / preservation save** | **qpdf** | `QPDFWriter` append mode, preserve object streams |
| **Garbage collection, linearise** | **qpdf** | `QPDFWriter` |
| **True redaction, sanitisation** | **qpdf** | full-rewrite path |

Both are already dependencies. No new library is added.

## Two blocking constraints on the specification as written

Stated now rather than discovered at milestone 4.

### 1. "Operator byte range" source references are not achievable as specified

qpdf's `TokenFilter` yields an ordered **token/operator sequence**, not stable
byte offsets that survive an edit. Once any operator is rewritten, every
downstream byte offset shifts and each cached range is invalid.

The achievable source reference is:

```
(page index, content-stream identity, operator ordinal,
 Form XObject nesting path, resource name, original CTM and graphics state)
```

plus the qpdf indirect object number and generation of the owning stream. That
is stable within a session and reproducible across a reopen of an unmodified
file. It is **not** a byte range. Acceptance criteria that depend on
byte-range identity must be restated against operator ordinals.

### 2. Several requirements need libraries this project does not have

| Requirement | Needs | Status |
|---|---|---|
| OCR fallback (Pipeline 6) | Tesseract or equivalent | **Absent.** Pipeline unimplementable today. |
| Shaping, bidi, vertical writing for *new* text (Pipeline 10) | HarfBuzz | **Absent.** Existing text keeps its shaping; newly typed text is unshaped. |
| Font subset expansion (Pipeline 10) | FreeType plus font-table surgery | **Absent.** Substitute-and-warn is the only honest behaviour. |
| Deskew, dewarp, denoise (Pipeline 6) | OpenCV or equivalent | **Absent.** |
| Independent golden renderer (Pipeline 17) | A second renderer | Use `pdftoppm` as a CI-only subprocess — never linked, so its GPL does not reach the binary. |

Each is a dependency decision with its own licence, binary size, and CI cost.

## The three guarantees, defined

The word "lossless" is never used unqualified in this project.

1. **Byte preservation** — original bytes and prior object revisions are kept
   by appending an incremental update. Holds only on the incremental save
   path, and only until a full rewrite or true redaction is requested. A full
   rewrite discards it by design.
2. **Visual fidelity** — unmodified regions render identically within a stated
   per-channel anti-aliasing tolerance, measured at multiple DPIs and page
   rotations against an independent renderer. "Identical" means within
   tolerance, never bit-identical.
3. **Semantic preservation** — Unicode mappings, font resources, structure
   tags, reading order, form fields, annotations, metadata, clipping, blend
   modes, optional content, and object relationships survive the round trip.
   Verified by extraction diff, not by eye.

These are independent. An incremental save can hold (1) and (3) while a font
substitution breaks (2). A full rewrite can hold (2) and (3) while discarding
(1) deliberately.

**A raster screenshot cannot be converted back into the source PDF.** The
screenshot has already discarded fonts, vector geometry, object references,
Unicode CMaps, annotations, widgets, metadata, hidden content, exact page
coordinates, and any glyph pixels sitting under an overlay. Reconstruction
from an image is OCR plus synthesis and is labelled as such in the UI.
High-fidelity editing requires the original PDF.

## Consequences

- The source document is opened read-only and never written in place. Edits go
  to a new file; the destination is replaced atomically only after the output
  is reopened and validated through an independent parse.
- Two engines mean two object models and a mapping between them. That mapping
  is the main source of accidental complexity here, and is the first thing to
  review if the codebase starts to sprawl.
- `PdfDocument::textRuns()` is already a partial display list. The typed
  display list extends it rather than replacing it.
- Editing overlays live in the Qt widget layer and are never inserted into a
  content stream.

## Current implementation status

| Milestone | State |
|---|---|
| 1. Secure open, render, immutable source | Partial — opens, renders, saves to a copy. No ingestion limits, no sandbox, no source hash. |
| 2. Native display list | Partial — text runs only, with bounds and z-order. No paths, images, forms, shadings. |
| 3. Selection overlay, hit test, movement, undo | **Not started.** |
| 4. Preservation-first save, golden render | **Not started.** Current save is a full rewrite via PDFium. |
| 5. Native text editing and fonts | Partial — text replace and styled replace; no font-availability check. |
| 6. AcroForm widgets | Partial — fill and click; no appearance-stream regeneration. |
| 7–11 | **Not started.** |

No milestone beyond 2 is claimed until it has a golden-render test.

## Decision required before implementation continues

The pairing above is sound and adds no dependency. Two answers are needed,
because they change what gets built:

1. **Accept operator-ordinal source references** in place of byte ranges, or
   fund a relicence / commercial engine to get nearer byte-level identity.
2. **Confirm the deferral of OCR, shaping, and subset expansion** to a later
   phase.

Until (1) is answered, Pipelines 2, 3, and 13 cannot be specified precisely
enough to implement without rework.
