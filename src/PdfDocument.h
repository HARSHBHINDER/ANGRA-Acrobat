#pragma once

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>

struct PdfBookmark {
    QString title;
    int page = -1;
    QList<PdfBookmark> children;
};

struct PdfLinkHit {
    enum class Type { None, Page, Uri };
    Type type = Type::None;
    int page = -1;
    QString uri;
};

// Thin wrapper around PDFium. Owns the document handle; never leaks it.
// All public rects/points use TOP-LEFT-origin page points; PDF-coordinate
// flipping happens inside.
class PdfDocument {
public:
    enum class Status { Ok, FileError, FormatError, PasswordRequired };

    PdfDocument() = default;
    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    Status load(const QString& path, const QString& password = {});
    bool createEmpty(); // new blank document (for conversion/extract targets)
    void close();
    bool isLoaded() const { return m_doc != nullptr; }
    bool isModified() const { return m_modified; }
    QString filePath() const { return m_path; }

    int pageCount() const;
    QSizeF pageSizePoints(int pageIndex) const;
    // scale = output pixels per PDF point (1.0 == 72 dpi)
    QImage renderPage(int pageIndex, double scale) const;

    // --- text ---
    QString pageText(int pageIndex) const;
    QString textInRect(int pageIndex, const QRectF& rect) const;
    QList<QRectF> searchPage(int pageIndex, const QString& term) const;

    // --- navigation data ---
    QList<PdfBookmark> bookmarks() const;
    PdfLinkHit linkAt(int pageIndex, const QPointF& pagePt) const;
    QString metaText(const char* tag) const;
    unsigned long permissions() const;

    // --- page operations (set the modified flag) ---
    void deletePage(int pageIndex);
    int pageRotation(int pageIndex) const;        // quarter turns 0..3
    void setPageRotation(int pageIndex, int rot); // quarter turns 0..3
    bool importAll(const PdfDocument& src, int insertAt);
    bool importRange(const PdfDocument& src, const QByteArray& range, int insertAt);
    bool addImagePage(const QImage& image);     // appended, sized to image at 96 dpi
    bool insertImagePage(int index, const QImage& image, const QSizeF& sizePts);
    bool addTextPage(const QStringList& lines); // appended, US-letter, 11 pt
    bool flattenAllPages();

    // True redaction: burn black boxes into a raster of the page, then
    // REPLACE the page with that bitmap. Original text/vectors are gone.
    bool redactRasterize(int pageIndex, const QList<QRectF>& rects);

    // --- content stamping / page geometry (set the modified flag) ---
    bool cropPage(int pageIndex, const QRectF& rect); // top-left-origin points
    bool movePage(int from, int to);
    bool addPageNumbers();                    // bottom-center, every page
    bool addTextWatermark(const QString& text); // diagonal gray, every page
    bool addTextAt(int pageIndex, const QPointF& pagePt, const QString& text);
    bool placeImage(int pageIndex, const QImage& image, const QPointF& pagePt,
                    double widthPt); // signature/stamp placement
    int extractImages(int pageIndex, const QString& dirPath,
                      const QString& baseName) const; // saved PNG count

    // --- interactive forms (fill only) ---
    bool hasForms() const;
    void formClick(int pageIndex, const QPointF& pagePt);
    void formChar(int pageIndex, int unicode);
    void formKillFocus();

    // --- annotations (set the modified flag) ---
    bool addHighlight(int pageIndex, const QList<QRectF>& rects, const QColor& color);
    bool addNote(int pageIndex, const QPointF& pagePt, const QString& text);
    bool addInk(int pageIndex, const QList<QPolygonF>& strokes, const QColor& color,
                double width);
    bool addSquare(int pageIndex, const QRectF& rect, const QColor& color);

    // Safe save: serialize to memory, validate by reopening, then atomic
    // replace via QSaveFile. Source file is never left half-written.
    bool saveCopy(const QString& destPath, QString* error = nullptr);

    QByteArray password() const { return m_password; }

    static void initLibrary();
    static void shutdownLibrary();

private:
    void initForms();
    void killForms();

    void* m_doc = nullptr;
    void* m_form = nullptr; // FPDF_FORMHANDLE
    void* m_ffi = nullptr;  // FPDF_FORMFILLINFO, heap-owned so it outlives calls
    QByteArray m_data; // backing buffer; must outlive m_doc
    QString m_path;
    QByteArray m_password; // kept so saveCopy can validate its own output
    bool m_modified = false;
};
