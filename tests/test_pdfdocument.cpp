// Smallest checks that fail if PdfDocument breaks. Run via ctest.
#include "PdfDocument.h"

#include <QDir>
#include <cstdio>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #x, __FILE__, __LINE__);                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char** argv) {
    CHECK(argc > 1);
    PdfDocument::initLibrary();
    const QString testPdf = QString::fromLocal8Bit(argv[1]);
    const QString tmp = QDir::tempPath();
    {
        // load + read
        PdfDocument doc;
        CHECK(doc.load(testPdf) == PdfDocument::Status::Ok);
        CHECK(doc.pageCount() == 2);
        const QSizeF size = doc.pageSizePoints(0);
        CHECK(size.width() > 0 && size.height() > 0);
        const QImage image = doc.renderPage(0, 1.0);
        CHECK(!image.isNull());
        CHECK(image.width() > 500 && image.height() > 700);

        // text + search
        CHECK(doc.pageText(0).contains(QStringLiteral("ANGRA")));
        CHECK(!doc.searchPage(0, QStringLiteral("test")).isEmpty());
        CHECK(doc.searchPage(0, QStringLiteral("zzz-not-there")).isEmpty());
        const QString bounded =
            doc.textInRect(0, QRectF(0, 0, size.width(), size.height()));
        CHECK(bounded.contains(QStringLiteral("ANGRA")));

        // page ops + safe save roundtrip
        doc.deletePage(1);
        CHECK(doc.pageCount() == 1);
        CHECK(doc.isModified());
        const QString out1 = tmp + QStringLiteral("/angra-test-out1.pdf");
        QString err;
        CHECK(doc.saveCopy(out1, &err));
        PdfDocument re;
        CHECK(re.load(out1) == PdfDocument::Status::Ok);
        CHECK(re.pageCount() == 1);

        // annotation + save survives validation
        CHECK(re.addHighlight(0, {QRectF(70, 60, 200, 30)}, QColor(255, 220, 0, 160)));
        CHECK(re.saveCopy(tmp + QStringLiteral("/angra-test-out2.pdf"), &err));

        // rotation persists
        re.setPageRotation(0, 1);
        CHECK(re.pageRotation(0) == 1);

        // missing file
        PdfDocument missing;
        CHECK(missing.load(QStringLiteral("no-such-file.pdf")) ==
              PdfDocument::Status::FileError);
    }
    {
        // text -> PDF -> text roundtrip
        PdfDocument out;
        CHECK(out.createEmpty());
        CHECK(out.addTextPage({QStringLiteral("hello angra roundtrip")}));
        const QString path = tmp + QStringLiteral("/angra-test-text.pdf");
        CHECK(out.saveCopy(path));
        PdfDocument re;
        CHECK(re.load(path) == PdfDocument::Status::Ok);
        CHECK(re.pageCount() == 1);
        CHECK(re.pageText(0).contains(QStringLiteral("hello angra roundtrip")));
    }
    {
        // image -> PDF
        QImage img(100, 80, QImage::Format_ARGB32);
        img.fill(Qt::blue);
        PdfDocument out;
        CHECK(out.createEmpty());
        CHECK(out.addImagePage(img));
        CHECK(out.pageCount() == 1);
        CHECK(!out.renderPage(0, 1.0).isNull());

        // extract via importRange
        PdfDocument src;
        CHECK(src.load(testPdf) == PdfDocument::Status::Ok);
        PdfDocument one;
        CHECK(one.createEmpty());
        CHECK(one.importRange(src, "1", 0));
        CHECK(one.pageCount() == 1);
    }
    PdfDocument::shutdownLibrary();
    std::puts("ok");
    return 0;
}
