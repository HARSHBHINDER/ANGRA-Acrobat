// Smallest checks that fail if PdfDocument breaks. Run via ctest.
#include "PdfDocument.h"
#ifdef ANGRA_HAVE_QPDF
#include "PdfProtect.h"
#endif

#include <QDir>
#include <QFile>
#include <cstdio>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #x, __FILE__, __LINE__);                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

// Breadcrumb: last line printed before a crash localizes the fault.
#define MARK(tag) do { std::fprintf(stderr, "== %s\n", tag); std::fflush(stderr); } while (0)

int main(int argc, char** argv) {
    CHECK(argc > 1);
    PdfDocument::initLibrary();
    const QString testPdf = QString::fromLocal8Bit(argv[1]);
    const QString tmp = QDir::tempPath();
    {
        MARK("load+read");
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
        MARK("text->pdf");
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
        MARK("image->pdf");
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
        CHECK(one.importPages(src, 0, "1"));
        CHECK(one.pageCount() == 1);
    }
    {
        MARK("redaction");
        // redaction destroys text: build a page with text, redact everything
        PdfDocument out;
        CHECK(out.createEmpty());
        CHECK(out.addTextPage({QStringLiteral("secret content")}));
        CHECK(out.pageText(0).contains(QStringLiteral("secret")));
        const QSizeF pts = out.pageSizePoints(0);
        CHECK(out.redactRasterize(0, {QRectF(0, 0, pts.width(), pts.height())}));
        CHECK(out.pageCount() == 1);
        CHECK(!out.pageText(0).contains(QStringLiteral("secret"))); // text is gone
        const QString path = tmp + QStringLiteral("/angra-test-redact.pdf");
        CHECK(out.saveCopy(path));
        PdfDocument re;
        CHECK(re.load(path) == PdfDocument::Status::Ok);
        CHECK(!re.pageText(0).contains(QStringLiteral("secret")));
    }
#ifdef ANGRA_HAVE_QPDF
    {
        // encrypt roundtrip: no password fails, right password works
        QByteArray bytes;
        QString err;
        CHECK(pdfprotect::encrypt(testPdf, {}, QStringLiteral("pw123"), {}, &bytes, &err));
        const QString path = tmp + QStringLiteral("/angra-test-enc.pdf");
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size());
        f.close();
        PdfDocument locked;
        CHECK(locked.load(path) == PdfDocument::Status::PasswordRequired);
        CHECK(locked.load(path, QStringLiteral("pw123")) == PdfDocument::Status::Ok);
        CHECK(locked.pageCount() == 2);
    }
#endif
    {
        MARK("stamping+geometry");
        // stamping + geometry: crop, move, page numbers, watermark, place image
        PdfDocument d;
        CHECK(d.load(testPdf) == PdfDocument::Status::Ok);
        CHECK(d.pageCount() == 2);
        CHECK(d.movePage(0, 1)); // swap; still 2 pages, still renderable
        CHECK(d.pageCount() == 2);
        CHECK(!d.renderPage(0, 1.0).isNull());
        CHECK(d.addPageNumbers());
        CHECK(d.addTextWatermark(QStringLiteral("DRAFT")));
        CHECK(d.addTextAt(0, QPointF(72, 72), QStringLiteral("stamped")));
        QImage sig(40, 20, QImage::Format_ARGB32);
        sig.fill(Qt::red);
        CHECK(d.placeImage(0, sig, QPointF(100, 100), 72.0));
        const QSizeF before = d.pageSizePoints(0);
        CHECK(d.cropPage(0, QRectF(10, 10, 200, 300)));
        const QSizeF after = d.pageSizePoints(0);
        CHECK(after.width() < before.width()); // media box shrank
        const QString path = tmp + QStringLiteral("/angra-test-stamp.pdf");
        CHECK(d.saveCopy(path));
        PdfDocument re;
        CHECK(re.load(path) == PdfDocument::Status::Ok);
        CHECK(re.pageText(0).contains(QStringLiteral("stamped")));
        CHECK(re.extractImages(0, tmp, QStringLiteral("angra-test-extract")) >= 1);
    }
    {
        MARK("edit-text");
        // edit existing text: create a page with known text, find it, replace it
        PdfDocument d;
        CHECK(d.createEmpty());
        CHECK(d.addTextPage({QStringLiteral("editme")}));
        const QSizeF pts = d.pageSizePoints(0);
        // text sits near top-left margin (54pt, ~ top); probe a band of y values
        int idx = -1;
        QString found;
        for (int y = 40; y < 120 && idx < 0; y += 4)
            found = d.textObjectAt(0, QPointF(70, y), &idx);
        CHECK(idx >= 0);
        CHECK(found.contains(QStringLiteral("editme")));
        // A click in the blank margin hits nothing and must report -1 rather
        // than aborting the object scan (regression: empty runs returned early).
        int idxBlank = 0;
        CHECK(d.textObjectAt(0, QPointF(5, 5), &idxBlank).isEmpty());
        CHECK(idxBlank == -1);
        CHECK(d.setTextObject(0, idx, QStringLiteral("changed")));
        const QString path = tmp + QStringLiteral("/angra-test-edit.pdf");
        CHECK(d.saveCopy(path));
        PdfDocument re;
        CHECK(re.load(path) == PdfDocument::Status::Ok);
        CHECK(re.pageText(0).contains(QStringLiteral("changed")));
        CHECK(!re.pageText(0).contains(QStringLiteral("editme")));
        (void)pts;

        MARK("scan-text-runs");
        // Scanner: enumerate runs, then apply highest index first. The order
        // matters - an emptied run is deleted, which shifts every later
        // page-object index down.
        {
            PdfDocument s;
            CHECK(s.createEmpty());
            CHECK(s.addTextPage({QStringLiteral("alpha one"),
                                 QStringLiteral("beta two"),
                                 QStringLiteral("gamma three")}));
            const QList<PdfDocument::TextRun> runs = s.textRuns(0);
            CHECK(runs.size() >= 3);
            for (const PdfDocument::TextRun& r : runs) {
                CHECK(r.index >= 0);
                CHECK(!r.text.trimmed().isEmpty()); // whitespace runs are skipped
                CHECK(r.rect.width() > 0 && r.rect.height() > 0);
            }
            // rewrite the last run, delete the first: descending index order
            const int last = runs.size() - 1;
            CHECK(s.setTextObject(0, runs.at(last).index, QStringLiteral("omega")));
            CHECK(s.setTextObject(0, runs.at(0).index, {})); // empty == delete
            const QString scanPath = tmp + QStringLiteral("/angra-test-scan.pdf");
            CHECK(s.saveCopy(scanPath));
            PdfDocument sr;
            CHECK(sr.load(scanPath) == PdfDocument::Status::Ok);
            const QString after = sr.pageText(0);
            CHECK(after.contains(QStringLiteral("omega")));
            CHECK(!after.contains(QStringLiteral("alpha"))); // first run deleted
            CHECK(sr.textRuns(0).size() == runs.size() - 1);
        }

        MARK("styled-replace");
        // styled replace: bold Times, underlined, survives save/reload
        int idx2 = -1;
        QString found2;
        for (int y = 40; y < 120 && idx2 < 0; y += 4)
            found2 = re.textObjectAt(0, QPointF(70, y), &idx2);
        CHECK(idx2 >= 0);
        PdfDocument::TextStyle st;
        st.family = QStringLiteral("Times");
        st.size = 18;
        st.bold = true;
        st.underline = true;
        st.color = QColor(200, 0, 0);
        CHECK(re.styleTextObject(0, idx2, QStringLiteral("styled"), st));
        const QString sp = tmp + QStringLiteral("/angra-test-styled.pdf");
        CHECK(re.saveCopy(sp));
        PdfDocument re2;
        CHECK(re2.load(sp) == PdfDocument::Status::Ok);
        CHECK(re2.pageText(0).contains(QStringLiteral("styled")));

        MARK("reflow");
        // reflow region: wrap a long paragraph into a narrow box, verify words present
        PdfDocument::TextStyle rs;
        rs.family = QStringLiteral("Helvetica");
        rs.size = 11;
        const QString para =
            QStringLiteral("the quick brown fox jumps over the lazy dog again and again");
        CHECK(re2.reflowRegion(0, QRectF(72, 72, 150, 400), para, rs));
        const QString rp = tmp + QStringLiteral("/angra-test-reflow.pdf");
        CHECK(re2.saveCopy(rp));
        PdfDocument re3;
        CHECK(re3.load(rp) == PdfDocument::Status::Ok);
        const QString out = re3.pageText(0);
        CHECK(out.contains(QStringLiteral("quick")));
        CHECK(out.contains(QStringLiteral("lazy")));
    }
    PdfDocument::shutdownLibrary();
    std::puts("ok");
    return 0;
}
