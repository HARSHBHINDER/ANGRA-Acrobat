#include "PdfDocument.h"

#include <QFile>
#include <algorithm>
#include <climits>
#include <fpdfview.h>

void PdfDocument::initLibrary() { FPDF_InitLibrary(); }
void PdfDocument::shutdownLibrary() { FPDF_DestroyLibrary(); }

PdfDocument::~PdfDocument() { close(); }

void PdfDocument::close() {
    if (m_doc) {
        FPDF_CloseDocument(static_cast<FPDF_DOCUMENT>(m_doc));
        m_doc = nullptr;
    }
    m_data.clear();
}

PdfDocument::Status PdfDocument::load(const QString& path) {
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
    FPDF_DOCUMENT doc =
        FPDF_LoadMemDocument(m_data.constData(), static_cast<int>(m_data.size()), nullptr);
    if (!doc) {
        const unsigned long err = FPDF_GetLastError();
        m_data.clear();
        return err == FPDF_ERR_PASSWORD ? Status::PasswordRequired : Status::FormatError;
    }
    m_doc = doc;
    return Status::Ok;
}

int PdfDocument::pageCount() const {
    return m_doc ? FPDF_GetPageCount(static_cast<FPDF_DOCUMENT>(m_doc)) : 0;
}

QSizeF PdfDocument::pageSizePoints(int pageIndex) const {
    if (!m_doc)
        return {};
    double w = 0, h = 0;
    if (!FPDF_GetPageSizeByIndex(static_cast<FPDF_DOCUMENT>(m_doc), pageIndex, &w, &h))
        return {};
    return {w, h};
}

QImage PdfDocument::renderPage(int pageIndex, double scale) const {
    if (!m_doc)
        return {};
    FPDF_PAGE page = FPDF_LoadPage(static_cast<FPDF_DOCUMENT>(m_doc), pageIndex);
    if (!page)
        return {};
    const int w = std::max(1, static_cast<int>(FPDF_GetPageWidthF(page) * scale + 0.5));
    const int h = std::max(1, static_cast<int>(FPDF_GetPageHeightF(page) * scale + 0.5));
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::white);
    FPDF_BITMAP bitmap =
        FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, image.bits(), image.bytesPerLine());
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, FPDF_ANNOT | FPDF_LCD_TEXT);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    return image;
}
