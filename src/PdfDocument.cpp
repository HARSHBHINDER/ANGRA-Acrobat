#include "PdfDocument.h"

#include <QFile>
#include <QSaveFile>
#include <QVarLengthArray>
#include <algorithm>
#include <climits>
#include <cstdint>

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>

#include <fpdf_annot.h>
#include <fpdf_doc.h>
#include <fpdf_edit.h>
#include <fpdf_flatten.h>
#include <fpdf_formfill.h>
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
    killForms();
    if (m_doc) {
        FPDF_CloseDocument(doc(m_doc));
        m_doc = nullptr;
    }
    m_data.clear();
    m_path.clear();
    m_password.clear();
    m_modified = false;
}

namespace {
void ffiInvalidate(FPDF_FORMFILLINFO*, FPDF_PAGE, double, double, double, double) {}
int ffiSetTimer(FPDF_FORMFILLINFO*, int, TimerCallback) { return 0; }
void ffiKillTimer(FPDF_FORMFILLINFO*, int) {}
FPDF_SYSTEMTIME ffiLocalTime(FPDF_FORMFILLINFO*) { return {}; }
} // namespace

void PdfDocument::initForms() {
    if (!m_doc || m_form)
        return;
    auto* ffi = new FPDF_FORMFILLINFO();
    ffi->version = 1;
    ffi->FFI_Invalidate = &ffiInvalidate;
    ffi->FFI_SetTimer = &ffiSetTimer;
    ffi->FFI_KillTimer = &ffiKillTimer;
    ffi->FFI_GetLocalTime = &ffiLocalTime;
    m_ffi = ffi;
    m_form = FPDFDOC_InitFormFillEnvironment(doc(m_doc), ffi);
}

void PdfDocument::killForms() {
    if (m_form) {
        FPDFDOC_ExitFormFillEnvironment(static_cast<FPDF_FORMHANDLE>(m_form));
        m_form = nullptr;
    }
    delete static_cast<FPDF_FORMFILLINFO*>(m_ffi);
    m_ffi = nullptr;
}

bool PdfDocument::hasForms() const {
    return m_doc && FPDF_GetFormType(doc(m_doc)) != FORMTYPE_NONE;
}

void PdfDocument::formClick(int pageIndex, const QPointF& pagePt) {
    if (!m_doc)
        return;
    initForms();
    if (!m_form)
        return;
    Page page(m_doc, pageIndex);
    if (!page)
        return;
    auto form = static_cast<FPDF_FORMHANDLE>(m_form);
    FORM_OnAfterLoadPage(page.p, form);
    const double y = FPDF_GetPageHeightF(page.p) - pagePt.y();
    FORM_OnLButtonDown(form, page.p, 0, pagePt.x(), y);
    FORM_OnLButtonUp(form, page.p, 0, pagePt.x(), y);
    FORM_OnBeforeClosePage(page.p, form);
    m_modified = true; // field focus/value may have changed
}

void PdfDocument::formChar(int pageIndex, int unicode) {
    if (!m_doc || !m_form)
        return;
    Page page(m_doc, pageIndex);
    if (!page)
        return;
    auto form = static_cast<FPDF_FORMHANDLE>(m_form);
    FORM_OnAfterLoadPage(page.p, form);
    FORM_OnChar(form, page.p, unicode, 0);
    FORM_OnBeforeClosePage(page.p, form);
    m_modified = true;
}

void PdfDocument::formKillFocus() {
    if (m_form)
        FORM_ForceToKillFocus(static_cast<FPDF_FORMHANDLE>(m_form));
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
    if (FPDF_GetFormType(d) != FORMTYPE_NONE)
        initForms(); // fields render and accept input from the start
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
    if (m_form) {
        auto form = static_cast<FPDF_FORMHANDLE>(m_form);
        FORM_OnAfterLoadPage(page.p, form);
        FPDF_FFLDraw(form, bitmap, page.p, 0, 0, w, h, 0, FPDF_ANNOT);
        FORM_OnBeforeClosePage(page.p, form);
    }
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
    FPDFText_GetBoundedText(tp.t, left, top, right, bottom, buf.data(), chars + 1);
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

bool PdfDocument::importPages(const PdfDocument& src, int insertAt, const QByteArray& range) {
    if (!m_doc || !src.m_doc)
        return false;
    if (!FPDF_ImportPages(doc(m_doc), doc(src.m_doc),
                          range.isEmpty() ? nullptr : range.constData(), insertAt))
        return false;
    m_modified = true;
    return true;
}

bool PdfDocument::addImagePage(const QImage& image) {
    return insertImagePage(pageCount(),
                           image,
                           QSizeF(image.width() * 72.0 / 96.0, image.height() * 72.0 / 96.0));
}

bool PdfDocument::insertImagePage(int index, const QImage& image, const QSizeF& sizePts) {
    if (!m_doc || image.isNull() || sizePts.isEmpty())
        return false;
    const double wPt = sizePts.width();
    const double hPt = sizePts.height();
    FPDF_PAGE page = FPDFPage_New(doc(m_doc), index, wPt, hPt);
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
    for (int start = 0; start < std::max<int>(1, lines.size()); start += perPage) {
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
    }
    m_modified = true;
    return true;
}

bool PdfDocument::redactRasterize(int pageIndex, const QList<QRectF>& rects) {
    if (!m_doc || rects.isEmpty() || pageIndex < 0 || pageIndex >= pageCount())
        return false;
    const QSizeF pts = pageSizePoints(pageIndex);
    if (pts.isEmpty())
        return false;
    const double scale = 2.0; // 144 dpi replacement bitmap
    QImage img = renderPage(pageIndex, scale);
    if (img.isNull())
        return false;
    QPainter p(&img);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (const QRectF& r : rects)
        p.drawRect(QRectF(r.left() * scale, r.top() * scale, r.width() * scale,
                          r.height() * scale));
    p.end();
    // Insert the raster AFTER the original, then delete the original: the
    // document never has fewer pages than expected if a step fails.
    if (!insertImagePage(pageIndex + 1, img, pts))
        return false;
    FPDFPage_Delete(doc(m_doc), pageIndex);
    m_modified = true;
    return true;
}

bool PdfDocument::cropPage(int pageIndex, const QRectF& rect) {
    if (!m_doc || rect.isEmpty())
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    const float l = rect.left(), r = rect.right();
    const float top = h - rect.top(), bottom = h - rect.bottom();
    FPDFPage_SetMediaBox(page.p, l, bottom, r, top);
    FPDFPage_SetCropBox(page.p, l, bottom, r, top);
    m_modified = true;
    return true;
}

bool PdfDocument::movePage(int from, int to) {
    const int n = pageCount();
    if (!m_doc || from == to || from < 0 || to < 0 || from >= n || to >= n)
        return false;
    QList<int> order;
    for (int i = 0; i < n; ++i)
        order << i;
    order.move(from, to);
    QByteArray range;
    for (int i : order) {
        if (!range.isEmpty())
            range += ',';
        range += QByteArray::number(i + 1);
    }
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    if (!FPDF_ImportPages(nd, doc(m_doc), range.constData(), 0)) {
        FPDF_CloseDocument(nd);
        return false;
    }
    killForms();
    FPDF_CloseDocument(doc(m_doc));
    m_doc = nd;
    m_data.clear(); // old backing buffer no longer needed; import deep-copies
    if (FPDF_GetFormType(nd) != FORMTYPE_NONE)
        initForms();
    m_modified = true;
    return true;
}

// Shared stamping core: x/y in PDF coords (bottom-left origin).
static bool stampText(FPDF_DOCUMENT d, FPDF_PAGE page, const QString& text, float size,
                      const QColor& color, double x, double y, bool diagonal) {
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewTextObj(d, "Helvetica", size);
    if (!obj)
        return false;
    FPDFText_SetText(obj, reinterpret_cast<FPDF_WIDESTRING>(text.utf16()));
    FPDFPageObj_SetFillColor(obj, color.red(), color.green(), color.blue(), color.alpha());
    if (diagonal) {
        const double c = 0.7071, s = 0.7071; // 45 degrees
        FPDFPageObj_Transform(obj, c, s, -s, c, x, y);
    } else {
        FPDFPageObj_Transform(obj, 1, 0, 0, 1, x, y);
    }
    FPDFPage_InsertObject(page, obj);
    FPDFPage_GenerateContent(page);
    return true;
}

bool PdfDocument::addPageNumbers() {
    if (!m_doc)
        return false;
    const int n = pageCount();
    for (int i = 0; i < n; ++i) {
        Page page(m_doc, i);
        if (!page)
            return false;
        const double w = FPDF_GetPageWidthF(page.p);
        if (!stampText(doc(m_doc), page.p, QString::number(i + 1), 11, Qt::black,
                       w / 2 - 6, 24, false))
            return false;
    }
    m_modified = true;
    return true;
}

bool PdfDocument::addTextWatermark(const QString& text) {
    if (!m_doc || text.isEmpty())
        return false;
    const int n = pageCount();
    for (int i = 0; i < n; ++i) {
        Page page(m_doc, i);
        if (!page)
            return false;
        const double w = FPDF_GetPageWidthF(page.p);
        const double h = FPDF_GetPageHeightF(page.p);
        if (!stampText(doc(m_doc), page.p, text, 48, QColor(128, 128, 128, 90),
                       w * 0.2, h * 0.3, true))
            return false;
    }
    m_modified = true;
    return true;
}

bool PdfDocument::addTextAt(int pageIndex, const QPointF& pagePt, const QString& text) {
    if (!m_doc || text.isEmpty())
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    if (!stampText(doc(m_doc), page.p, text, 12, Qt::black, pagePt.x(), h - pagePt.y(),
                   false))
        return false;
    m_modified = true;
    return true;
}

QString PdfDocument::textObjectAt(int pageIndex, const QPointF& pagePt, int* objIndex) const {
    if (objIndex)
        *objIndex = -1;
    if (!m_doc)
        return {};
    Page page(m_doc, pageIndex);
    if (!page)
        return {};
    TextPage tp(page.p);
    if (!tp)
        return {};
    const double h = FPDF_GetPageHeightF(page.p);
    const double px = pagePt.x(), py = h - pagePt.y(); // to PDF coords
    const int count = FPDFPage_CountObjects(page.p);
    // topmost = last in draw order; scan back to front
    for (int i = count - 1; i >= 0; --i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.p, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT)
            continue;
        float l, b, r, t;
        if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t))
            continue;
        if (px < l || px > r || py < b || py > t)
            continue;
        const unsigned long bytes = FPDFTextObj_GetText(obj, tp.t, nullptr, 0);
        if (bytes < 2)
            return {};
        QVarLengthArray<unsigned short, 256> buf(bytes / 2);
        FPDFTextObj_GetText(obj, tp.t, buf.data(), bytes);
        if (objIndex)
            *objIndex = i;
        return fromUtf16Buffer(buf, bytes);
    }
    return {};
}

bool PdfDocument::setTextObject(int pageIndex, int objIndex, const QString& text) {
    if (!m_doc || objIndex < 0)
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.p, objIndex);
    if (!obj || FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT)
        return false;
    if (text.isEmpty()) { // delete
        if (!FPDFPage_RemoveObject(page.p, obj))
            return false;
        FPDFPageObj_Destroy(obj);
    } else if (!FPDFText_SetText(obj, wide(text))) {
        return false;
    }
    FPDFPage_GenerateContent(page.p);
    m_modified = true;
    return true;
}

// Map family + bold/italic to a PDF base-14 font name.
static QByteArray base14Name(const QString& family, bool bold, bool italic) {
    if (family.startsWith(QStringLiteral("Times"), Qt::CaseInsensitive)) {
        if (bold && italic) return "Times-BoldItalic";
        if (bold) return "Times-Bold";
        if (italic) return "Times-Italic";
        return "Times-Roman";
    }
    if (family.startsWith(QStringLiteral("Courier"), Qt::CaseInsensitive)) {
        if (bold && italic) return "Courier-BoldOblique";
        if (bold) return "Courier-Bold";
        if (italic) return "Courier-Oblique";
        return "Courier";
    }
    if (bold && italic) return "Helvetica-BoldOblique";
    if (bold) return "Helvetica-Bold";
    if (italic) return "Helvetica-Oblique";
    return "Helvetica";
}

bool PdfDocument::styleTextObject(int pageIndex, int objIndex, const QString& text,
                                  const TextStyle& style, const QByteArray& ttf) {
    if (!m_doc || objIndex < 0 || text.isEmpty())
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    FPDF_PAGEOBJECT old = FPDFPage_GetObject(page.p, objIndex);
    if (!old || FPDFPageObj_GetType(old) != FPDF_PAGEOBJ_TEXT)
        return false;
    // Keep the old run's position (translation from its matrix).
    FS_MATRIX m{1, 0, 0, 1, 0, 0};
    FPDFPageObj_GetMatrix(old, &m);
    // ponytail: preserves position + font size only; old rotation/shear dropped.
    // Upgrade to full matrix reuse when rotated text needs restyling.

    FPDF_PAGEOBJECT obj;
    if (!ttf.isEmpty()) {
        FPDF_FONT font =
            FPDFText_LoadFont(doc(m_doc), reinterpret_cast<const uint8_t*>(ttf.constData()),
                              static_cast<uint32_t>(ttf.size()), FPDF_FONT_TRUETYPE, 0);
        if (!font)
            return false;
        obj = FPDFPageObj_CreateTextObj(doc(m_doc), font, style.size);
    } else {
        obj = FPDFPageObj_NewTextObj(
            doc(m_doc), base14Name(style.family, style.bold, style.italic).constData(),
            style.size);
    }
    if (!obj)
        return false;
    FPDFText_SetText(obj, wide(text));
    FPDFPageObj_SetFillColor(obj, style.color.red(), style.color.green(),
                             style.color.blue(), style.color.alpha());
    const FS_MATRIX place{1, 0, 0, 1, m.e, m.f};
    FPDFPageObj_SetMatrix(obj, &place);

    // Remove the old run, insert the new one.
    FPDFPage_RemoveObject(page.p, old);
    FPDFPageObj_Destroy(old);
    FPDFPage_InsertObject(page.p, obj);
    FPDFPage_GenerateContent(page.p); // finalize so bounds are valid

    if (style.underline) {
        float l, b, r, t;
        if (FPDFPageObj_GetBounds(obj, &l, &b, &r, &t)) {
            FPDF_PAGEOBJECT line = FPDFPageObj_CreateNewPath(l, b - 1);
            FPDFPath_LineTo(line, r, b - 1);
            FPDFPath_SetDrawMode(line, 0, 1); // no fill, stroke
            FPDFPageObj_SetStrokeColor(line, style.color.red(), style.color.green(),
                                       style.color.blue(), style.color.alpha());
            FPDFPageObj_SetStrokeWidth(line, std::max(0.5f, style.size / 16.0f));
            FPDFPage_InsertObject(page.p, line);
            FPDFPage_GenerateContent(page.p);
        }
    }
    m_modified = true;
    return true;
}

bool PdfDocument::reflowRegion(int pageIndex, const QRectF& rect, const QString& text,
                               const TextStyle& style) {
    if (!m_doc || rect.isEmpty())
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    const float rl = rect.left(), rr = rect.right();
    const float rtop = h - rect.top(), rbot = h - rect.bottom(); // PDF coords

    // 1. delete text runs whose center falls inside the region
    QList<FPDF_PAGEOBJECT> doomed;
    const int count = FPDFPage_CountObjects(page.p);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.p, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT)
            continue;
        float l, b, r, t;
        if (!FPDFPageObj_GetBounds(obj, &l, &b, &r, &t))
            continue;
        const float cx = (l + r) / 2, cy = (b + t) / 2;
        if (cx >= rl && cx <= rr && cy >= rbot && cy <= rtop)
            doomed.append(obj);
    }
    for (FPDF_PAGEOBJECT obj : doomed) {
        FPDFPage_RemoveObject(page.p, obj);
        FPDFPageObj_Destroy(obj);
    }

    // 2. greedy word-wrap to rect width using base-14 metrics.
    // ponytail: QFontMetrics of the platform font approximates the PDF base-14
    // width; exact wrap would read the font's /Widths. Close enough to wrap.
    QFont qf(style.family.startsWith(QStringLiteral("Times")) ? QStringLiteral("Times New Roman")
             : style.family.startsWith(QStringLiteral("Courier")) ? QStringLiteral("Courier New")
                                                                   : QStringLiteral("Arial"));
    qf.setPointSizeF(style.size);
    qf.setBold(style.bold);
    qf.setItalic(style.italic);
    const QFontMetricsF fm(qf);
    const double maxW = rect.width();

    QStringList lines;
    for (const QString& para : text.split(QLatin1Char('\n'))) {
        QString line;
        for (const QString& word : para.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const QString probe = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
            if (fm.horizontalAdvance(probe) > maxW && !line.isEmpty()) {
                lines << line;
                line = word;
            } else {
                line = probe;
            }
        }
        lines << line; // last line of paragraph (may be empty for blank line)
    }

    // 3. emit one text object per line from the top of the region downward
    const QByteArray fontName = base14Name(style.family, style.bold, style.italic);
    const double lead = style.size * 1.2;
    double y = rtop - style.size;
    for (const QString& line : lines) {
        if (!line.isEmpty()) {
            FPDF_PAGEOBJECT obj = FPDFPageObj_NewTextObj(doc(m_doc), fontName.constData(),
                                                         style.size);
            if (obj) {
                FPDFText_SetText(obj, wide(line));
                FPDFPageObj_SetFillColor(obj, style.color.red(), style.color.green(),
                                         style.color.blue(), style.color.alpha());
                const FS_MATRIX place{1, 0, 0, 1, rl, static_cast<float>(y)};
                FPDFPageObj_SetMatrix(obj, &place);
                FPDFPage_InsertObject(page.p, obj);
            }
        }
        y -= lead;
    }
    FPDFPage_GenerateContent(page.p);
    m_modified = true;
    return true;
}

bool PdfDocument::placeImage(int pageIndex, const QImage& image, const QPointF& pagePt,
                             double widthPt) {
    if (!m_doc || image.isNull() || widthPt <= 0)
        return false;
    Page page(m_doc, pageIndex);
    if (!page)
        return false;
    const double h = FPDF_GetPageHeightF(page.p);
    const double hPt = widthPt * image.height() / image.width();
    QImage img = image.convertToFormat(QImage::Format_ARGB32);
    FPDF_BITMAP bmp = FPDFBitmap_CreateEx(img.width(), img.height(), FPDFBitmap_BGRA,
                                          img.bits(), img.bytesPerLine());
    FPDF_PAGEOBJECT obj = FPDFPageObj_NewImageObj(doc(m_doc));
    FPDFImageObj_SetBitmap(&page.p, 1, obj, bmp);
    FPDFPageObj_Transform(obj, widthPt, 0, 0, hPt, pagePt.x(), h - pagePt.y() - hPt);
    FPDFPage_InsertObject(page.p, obj);
    FPDFPage_GenerateContent(page.p);
    FPDFBitmap_Destroy(bmp);
    m_modified = true;
    return true;
}

int PdfDocument::extractImages(int pageIndex, const QString& dirPath,
                               const QString& baseName) const {
    if (!m_doc)
        return 0;
    Page page(m_doc, pageIndex);
    if (!page)
        return 0;
    int saved = 0;
    const int count = FPDFPage_CountObjects(page.p);
    for (int i = 0; i < count; ++i) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page.p, i);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_IMAGE)
            continue;
        FPDF_BITMAP bmp = FPDFImageObj_GetRenderedBitmap(doc(m_doc), page.p, obj);
        if (!bmp)
            continue;
        if (FPDFBitmap_GetFormat(bmp) == FPDFBitmap_BGRA) {
            const QImage img(static_cast<const uchar*>(FPDFBitmap_GetBuffer(bmp)),
                             FPDFBitmap_GetWidth(bmp), FPDFBitmap_GetHeight(bmp),
                             FPDFBitmap_GetStride(bmp), QImage::Format_ARGB32);
            const QString dest = QStringLiteral("%1/%2-%3.png")
                                     .arg(dirPath, baseName)
                                     .arg(saved + 1, 3, 10, QLatin1Char('0'));
            if (img.copy().save(dest)) // copy() detaches before bitmap destroy
                ++saved;
        }
        FPDFBitmap_Destroy(bmp);
    }
    return saved;
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
