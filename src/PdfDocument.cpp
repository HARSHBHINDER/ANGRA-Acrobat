#include "PdfDocument.h"

#include <QFile>
#include <QSaveFile>
#include <QVarLengthArray>
#include <algorithm>
#include <climits>

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_flatten.h>
#include <fpdf_ppo.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <fpdf_transformpage.h>
#include <fpdfview.h>

namespace {

FPDF_DOCUMENT doc(void* p) { return static_cast<FPDF_DOCUMENT>(p); }

FPDF_WIDESTRING wide(const QString& s) {
    return reinterpret_cast<FPDF_WIDESTRING>(s.utf16());
}

QString fromUtf16Buffer(const QVarLengthArray<unsigned short, 256>& buf, unsigned long bytes) {
    if (bytes < 2)
        return {};
    // bytes includes the 2-byte terminator
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.constData()),
                              static_cast<int>(bytes / 2 - 1));
}

// RAII page/textpage so early returns never leak handles.
struct Page {
    FPDF_PAGE p = nullptr;
    Page(void* d, int i) { p = FPDF_LoadPage(doc(d), i); }
    ~Page() {
        if (p)
            FPDF_ClosePage(p);
    }
    explicit operator bool() const { return p != nullptr; }
};

struct TextPage {
    FPDF_TEXTPAGE t = nullptr;
    explicit TextPage(FPDF_PAGE p) { t = FPDFText_LoadPage(p); }
    ~TextPage() {
        if (t)
            FPDFText_ClosePage(t);
    }
    explicit operator bool() const { return t != nullptr; }
};

} // namespace

void PdfDocument::initLibrary() { FPDF_InitLibrary(); }
void PdfDocument::shutdownLibrary() { FPDF_DestroyLibrary(); }

PdfDocument::~PdfDocument() { close(); }

void PdfDocument::close() {
    if (m_doc) {
        FPDF_CloseDocument(doc(m_doc));
        m_doc = nullptr;
    }
    m_data.clear();
    m_path.clear();
    m_password.clear();
    m_modified = false;
}

PdfDocument::Status PdfDocument::load(const QString& path, const QString& password) {
    close();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return Status::FileError;
    // ponytail: whole file in RAM; switch to FPDF_FileAccess streaming if huge files matter
    m_data = file.readAll();
    if (m_data.isEmpty() || m_data.size() > INT_MAX) {
        m_data.clear();
        return Status::FormatError;
    }
    const QByteArray pw = password.toUtf8();
    FPDF_DOCUMENT d = FPDF_LoadMemDocument(m_data.constData(), static_cast<int>(m_data.size()),
                                           pw.isEmpty() ? nullptr : pw.constData());
    if (!d) {
        const unsigned long err = FPDF_GetLastError();
        m_data.clear();
        return err == FPDF_ERR_PASSWORD ? Status::PasswordRequired : Status::FormatError;
    }
    m_doc = d;
    m_path = path;
    m_password = pw;
    return Status::Ok;
}

bool PdfDocument::createEmpty() {
    close();
    m_doc = FPDF_CreateNewDocument();
    m_modified = true;
    return m_doc != nullptr;
}

int PdfDocument::pageCount() const { return m_doc ? FPDF_GetPageCount(doc(m_doc)) : 0; }

QSizeF PdfDocument::pageSizePoints(int pageIndex) const {
    if (!m_doc)
        return {};
    double w = 0, h = 0;
    if (!FPDF_GetPageSizeByIndex(doc(m_doc), pageIndex, &w, &h))
        return {};
    return {w, h};
}

QImage PdfDocument::renderPage(int pageIndex, double scale) const {
    if (!m_doc)
        return {};
    Page page(m_doc, pageIndex);
    if (!page)
        return {};
    const int w = std::max(1, static_cast<int>(FPDF_GetPageWidthF(page.p) * scale + 0.5));
    const int h = std::max(1, static_cast<int>(FPDF_GetPageHeightF(page.p) * scale + 0.5));
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bitmap =
        FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, image.bits(), image.bytesPerLine());
    FPDF_RenderPageBitmap(bitmap, page.p, 0, 0, w, h, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
    FPDFBitmap_Destroy(bitmap);
    return image;
}

QString PdfDocument::pageText(int pageIndex) const {
    if (!m_doc)
        return {};
    Page page(m_doc, pageIndex);
    if (!page)
        return {};
    TextPage tp(page.p);
    if (!tp)
        return {};
    const int chars = FPDFText_CountChars(tp.t);
    if (chars <= 0)
        return {};
    QVarLengthArray<unsigned short, 256> buf(chars + 1);
    const int written = FPDFText_GetText(tp.t, 0, chars, buf.data());
    if (written <= 1)
        return {};
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.constData()), written - 1);
}

QString PdfDocument::textInRect(int pageIndex, const QRectF& rect) const {
    if (!m_doc)
        return {};
    Page page(m_doc, pageIndex);
    if (!page)
        return {};
    TextPage tp(page.p);
    if (!tp)
        return {};
    const double h = FPDF_GetPageHeightF(page.p);
    // convert top-left origin to PDF coords: pdfTop = h - rect.top()
    const double left = rect.left(), right = rect.right();
    const double top = h - rect.top(), bottom = h - rect.bottom();
    const int chars = FPDFText_GetBoundedText(tp.t, left, top, right, bottom, nullptr, 0);
    if (chars <= 0)
        return {};
    QVarLengthArray<unsigned short, 256> buf(chars + 1);
    FPDFText_GetBoundedText(tp.t, left, top, right, bottom, buf.data(), chars);
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(buf.constData()), chars);
}

QList<QRectF> PdfDocument::searchPage(int pageIndex, const QString& term) const {
    QList<QRectF> out;
    if (!m_doc || term.isEmpty())
        return out;
    Page page(m_doc, pageIndex);
    if (!page)
        return out;
    TextPage tp(page.p);
    if (!tp)
        return out;
    const double h = FPDF_GetPageHeightF(page.p);
    FPDF_SCHHANDLE find = FPDFText_FindStart(tp.t, wide(term), 0, 0);
    while (FPDFText_FindNext(find)) {
        const int start = FPDFText_GetSchResultIndex(find);
        const int count = FPDFText_GetSchCount(find);
        const int rects = FPDFText_CountRects(tp.t, start, count);
        for (int i = 0; i < rects; ++i) {
            double l, t, r, b;
            if (FPDFText_GetRect(tp.t, i, &l, &t, &r, &b))
                out.append(QRectF(l, h - t, r - l, t - b));
        }
    }
    FPDFText_FindClose(find);
    return out;
}

static void walkBookmarks(FPDF_DOCUMENT d, FPDF_BOOKMARK bm, QList<PdfBookmark>& out) {
    while (bm) {
        PdfBookmark b;
        const unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
        if (bytes >= 2) {
            QVarLengthArray<unsigned short, 256> buf(bytes / 2);
            FPDFBookmark_GetTitle(bm, buf.data(), bytes);
            b.title = fromUtf16Buffer(buf, bytes);
        }
        if (FPDF_DEST dest = FPDFBookmark_GetDest(d, bm))
            b.page = FPDFDest_GetDestPageIndex(d, dest);
        walkBookmarks(d, FPDFBookmark_GetFirstChild(d, bm), b.children);
        out.append(b);
        bm = FPDFBookmark_GetNextSibling(d, bm);
    }
}

QList<PdfBookmark> PdfDocument::bookmarks() const {
    QList<PdfBookmark> out;
    if (m_doc)
        walkBookmarks(doc(m_doc), FPDFBookmark_GetFirstChild(doc(m_doc), nullptr), out);
    return out;
}

PdfLinkHit PdfDocument::linkAt(int pageIndex, const QPointF& pagePt) const {
    PdfLinkHit hit;
    if (!m_doc)
        return hit;
    Page page(m_doc, pageIndex);
    if (!page)
        return hit;
    const double h = FPDF_GetPageHeightF(page.p);
    FPDF_LINK link = FPDFLink_GetLinkAtPoint(page.p, pagePt.x(), h - pagePt.y());
    if (!link)
        return hit;
    if (FPDF_DEST dest = FPDFLink_GetDest(doc(m_doc), link)) {
        hit.type = PdfLinkHit::Type::Page;
        hit.page = FPDFDest_GetDestPageIndex(doc(m_doc), dest);
        return hit;
    }
    if (FPDF_ACTION action = FPDFLink_GetAction(link)) {
        const unsigned long type = FPDFAction_GetType(action);
        if (type == PDFACTION_GOTO) {
            if (FPDF_DEST dest = FPDFAction_GetDest(doc(m_doc), action)) {
                hit.type = PdfLinkHit::Type::Page;
                hit.page = FPDFDest_GetDestPageIndex(doc(m_doc), dest);
            }
        } else if (type == PDFACTION_URI) {
            const unsigned long len = FPDFAction_GetURIPath(doc(m_doc), action, nullptr, 0);
            if (len > 1) {
                QByteArray buf(static_cast<int>(len), 0);
                FPDFAction_GetURIPath(doc(m_doc), action, buf.data(), len);
                hit.type = PdfLinkHit::Type::Uri;
                hit.uri = QString::fromUtf8(buf.constData());
            }
        }
    }
    return hit;
}

QString PdfDocument::metaText(const char* tag) const {
    if (!m_doc)
        return {};
    const unsigned long bytes = FPDF_GetMetaText(doc(m_doc), tag, nullptr, 0);
    if (bytes < 2)
        return {};
    QVarLengthArray<unsigned short, 256> buf(bytes / 2);
    FPDF_GetMetaText(doc(m_doc), tag, buf.data(), bytes);
    return fromUtf16Buffer(buf, bytes);
}

unsigned long PdfDocument::permissions() const {
    return m_doc ? FPDF_GetDocPermissions(doc(m_doc)) : 0;
}

void PdfDocument::deletePage(int pageIndex) {
    if (!m_doc || pageIndex < 0 || pageIndex >= pageCount())
        return;
    FPDFPage_Delete(doc(m_doc), pageIndex);
    m_modified = true;
}

int PdfDocument::pageRotation(int pageIndex) const {
    if (!m_doc)
        return 0;
    Page page(m_doc, pageIndex);
    return page ? FPDFPage_GetRotation(page.p) : 0;
}

void PdfDocument::setPageRotation(int pageIndex, int rot) {
    if (!m_doc)
        return;
    Page page(m_doc, pageIndex);
    if (!page)
        return;
    FPDFPage_SetRotation(page.p, ((rot % 4) + 4) % 4);
    m_modified = true;
}

bool PdfDocument::importAll(const PdfDocument& src, int insertAt) {
    if (!m_doc || !src.m_doc)
        return false;
    if (!FPDF_ImportPages(doc(m_doc), doc(src.m_doc), nullptr, insertAt))
        return false;
    m_modified = true;
    return true;
}

bool PdfDocument::importRange(const PdfDocument& src, const QByteArray& range, int insertAt) {
    if (!m_doc || !src.m_doc)
        return false;
    if (!FPDF_ImportPages(doc(m_doc), doc(src.m_doc), range.constData(), insertAt))
        return false;
    m_modified = true;
    return true;
}

bool PdfDocument::addImagePage(const QImage& image) {
    if (!m_doc || image.isNull())
        return false;
    const double wPt = image.width() * 72.0 / 96.0;
    const double hPt = image.height() * 72.0 / 96.0;
    FPDF_PAGE page = FPDFPage_New(doc(m_doc), pageCount(), wPt, hPt);
    if (!page)
        return false;
    QImage img = image.convertToFormat(QImage::Format_ARGB32);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.width(), img.height(), FPDFBitmap_BGRA,
                                          img.bits(), img.bytesPerLine());
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc(m_doc));
    FPDFImageObj_SetBitmap(&page, 1, obj, bmp);
    // image object is a unit square; scale to full page
    FPDFPageObj_Transform(obj, wPt, 0, 0, hPt, 0, 0);
    FPDFPage_InsertObject(page, obj); // page takes ownership
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
    FPDFBitmap_Destroy(bmp);
    m_modified = true;
    return true;
}

bool PdfDocument::addTextPage(const QStringList& lines) {
    if (!m_doc)
        return false;
    const double W = 612, H = 792, margin = 54, lead = 14;
    const float fontSize = 11;
    const int perPage = static_cast<int>((H - 2 * margin) / lead);
    for (int start = 0; start < lines.size() || start == 0; start += perPage) {
        FPDF_PAGE page = FPDFPage_New(doc(m_doc), pageCount(), W, H);
        if (!page)
            return false;
        double y = H - margin - fontSize;
        const int end = std::min<int>(lines.size(), start + perPage);
        for (int i = start; i < end; ++i) {
            FPDF_PAGEOBJECT t = FPDFPageObj_NewTextObj(doc(m_doc), "Helvetica", fontSize);
            FPDFText_SetText(t, wide(lines.at(i)));
            FPDFPageObj_Transform(t, 1, 0, 0, 1, margin, y);
            FPDFPage_InsertObject(page, t);
            y -= lead;
        }
        FPDFPage_GenerateContent(page);
        FPDF_ClosePage(page);
        if (lines.isEmpty())
            break;
    }
    m_modified = true;
    return true;
}

bool PdfDocument::flattenAllPages() {
    if (!m_doc)
        return false;
    const int count = pageCount();
    for (int i = 0; i < count; ++i) {
        Page page(m_doc, i);
        if (page)
            FPDFPage_Flatten(page.p, FLAT_NORMALDISPLAY);
    }
    m_modified = true;
    return true;
}

bool PdfDocument::addHighlight(int pageIndex, const QList<QRectF>& rects, const QColor& color) {
    if (!m_doc || rects.isEmpty())
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page.p, FPDF_ANNOT_HIGHLIGHT);
    if (!annot)
        return false;
    FS_RECTF bound{static_cast<float>(rects.first().left()),
                   static_cast<float>(h - rects.first().top()),
                   static_cast<float>(rects.first().right()),
                   static_cast<float>(h - rects.first().bottom())};
    for (const QRectF& r : rects) {
        const float l = static_cast<float>(r.left());
        const float rr = static_cast<float>(r.right());
        const float t = static_cast<float>(h - r.top());
        const float b = static_cast<float>(h - r.bottom());
        FS_QUADPOINTSF q{l, t, rr, t, l, b, rr, b};
        FPDFAnnot_AppendAttachmentPoints(annot, &q);
        bound.left = std::min(bound.left, l);
        bound.right = std::max(bound.right, rr);
        bound.top = std::max(bound.top, t);
        bound.bottom = std::min(bound.bottom, b);
    }
    FPDFAnnot_SetRect(annot, &bound);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, color.red(), color.green(),
                       color.blue(), color.alpha());
    FPDFPage_CloseAnnot(annot);
    m_modified = true;
    return true;
}

bool PdfDocument::addNote(int pageIndex, const QPointF& pagePt, const QString& text) {
    if (!m_doc)
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page.p, FPDF_ANNOT_TEXT);
    if (!annot)
        return false;
    const float x = static_cast<float>(pagePt.x());
    const float y = static_cast<float>(h - pagePt.y());
    FS_RECTF rect{x, y + 20, x + 20, y};
    FPDFAnnot_SetRect(annot, &rect);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, 255, 220, 0, 255);
    FPDFAnnot_SetStringValue(annot, "Contents", wide(text));
    FPDFPage_CloseAnnot(annot);
    m_modified = true;
    return true;
}

bool PdfDocument::addInk(int pageIndex, const QList<QPolygonF>& strokes, const QColor& color,
                         double width) {
    if (!m_doc || strokes.isEmpty())
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page.p, FPDF_ANNOT_INK);
    if (!annot)
        return false;
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const QPolygonF& stroke : strokes) {
        QVarLengthArray<FS_POINTF, 64> pts;
        pts.reserve(stroke.size());
        for (const QPointF& p : stroke) {
            const float x = static_cast<float>(p.x());
            const float y = static_cast<float>(h - p.y());
            pts.append(FS_POINTF{x, y});
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
        FPDFAnnot_AddInkStroke(annot, pts.constData(), pts.size());
    }
    const float pad = static_cast<float>(width) + 2;
    FS_RECTF rect{minX - pad, maxY + pad, maxX + pad, minY - pad};
    FPDFAnnot_SetRect(annot, &rect);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, color.red(), color.green(),
                       color.blue(), color.alpha());
    FPDFAnnot_SetBorder(annot, 0, 0, static_cast<float>(width));
    FPDFPage_CloseAnnot(annot);
    m_modified = true;
    return true;
}

bool PdfDocument::addSquare(int pageIndex, const QRectF& rect, const QColor& color) {
    if (!m_doc)
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    FPDF_ANNOTATION annot = FPDFPage_CreateAnnot(page.p, FPDF_ANNOT_SQUARE);
    if (!annot)
        return false;
    FS_RECTF r{static_cast<float>(rect.left()), static_cast<float>(h - rect.top()),
               static_cast<float>(rect.right()), static_cast<float>(h - rect.bottom())};
    FPDFAnnot_SetRect(annot, &r);
    FPDFAnnot_SetColor(annot, FPDFANNOT_COLORTYPE_Color, color.red(), color.green(),
                       color.blue(), color.alpha());
    FPDFAnnot_SetBorder(annot, 0, 0, 2);
    FPDFPage_CloseAnnot(annot);
    m_modified = true;
    return true;
}

namespace {
struct BufferWriter : FPDF_FILEWRITE {
    QByteArray buf;
    static int writeBlock(FPDF_FILEWRITE* fw, const void* data, unsigned long size) {
        static_cast<BufferWriter*>(fw)->buf.append(static_cast<const char*>(data),
                                                   static_cast<qsizetype>(size));
        return 1;
    }
};
} // namespace

bool PdfDocument::saveCopy(const QString& destPath, QString* error) {
    const auto fail = [error](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!m_doc)
        return fail(QStringLiteral("No document is loaded."));

    BufferWriter writer;
    writer.version = 1;
    writer.WriteBlock = &BufferWriter::writeBlock;
    if (!FPDF_SaveAsCopy(doc(m_doc), &writer, FPDF_NO_INCREMENTAL))
        return fail(QStringLiteral("Serializing the document failed."));

    // Validate the bytes before letting them near the destination.
    FPDF_DOCUMENT check =
        FPDF_LoadMemDocument(writer.buf.constData(), static_cast<int>(writer.buf.size()),
                             m_password.isEmpty() ? nullptr : m_password.constData());
    if (!check)
        return fail(QStringLiteral("Validation failed: output is not a readable PDF."));
    const int checkPages = FPDF_GetPageCount(check);
    FPDF_CloseDocument(check);
    if (checkPages != pageCount())
        return fail(QStringLiteral("Validation failed: page count mismatch."));

    QSaveFile out(destPath);
    if (!out.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("Cannot open destination for writing."));
    if (out.write(writer.buf) != writer.buf.size() || !out.commit())
        return fail(QStringLiteral("Writing the destination file failed."));

    m_modified = false;
    if (m_path.isEmpty())
        m_path = destPath;
    return true;
}
