// Smallest check that fails if PdfDocument breaks. Run via ctest.
#include "PdfDocument.h"

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
    {
        PdfDocument doc;
        CHECK(doc.load(QString::fromLocal8Bit(argv[1])) == PdfDocument::Status::Ok);
        CHECK(doc.pageCount() == 2);
        const QSizeF size = doc.pageSizePoints(0);
        CHECK(size.width() > 0 && size.height() > 0);
        const QImage image = doc.renderPage(0, 1.0);
        CHECK(!image.isNull());
        CHECK(image.width() > 500 && image.height() > 700); // 612x792pt letter at 1px/pt

        PdfDocument missing;
        CHECK(missing.load(QStringLiteral("no-such-file.pdf")) ==
              PdfDocument::Status::FileError);
    }
    PdfDocument::shutdownLibrary();
    std::puts("ok");
    return 0;
}
