#pragma once

#include <QByteArray>
#include <QImage>
#include <QSizeF>
#include <QString>

// Thin wrapper around PDFium. Owns the document handle; never leaks it.
class PdfDocument {
public:
    enum class Status { Ok, FileError, FormatError, PasswordRequired };

    PdfDocument() = default;
    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    Status load(const QString& path);
    void close();
    bool isLoaded() const { return m_doc != nullptr; }
    int pageCount() const;
    QSizeF pageSizePoints(int pageIndex) const;
    // scale = output pixels per PDF point (1.0 == 72 dpi)
    QImage renderPage(int pageIndex, double scale) const;

    static void initLibrary();
    static void shutdownLibrary();

private:
    void* m_doc = nullptr;
    QByteArray m_data; // backing buffer; must outlive m_doc
};
