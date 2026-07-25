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
        // The app edits whatever objectsAt selected, so the test resolves the
        // run the same way instead of through a second point-based lookup.
        int idx = -1;
        QString found;
        for (const PdfDocument::TextRun& r : d.textRuns(0))
            if (r.text.contains(QStringLiteral("editme"))) {
                idx = r.index;
                found = r.text;
            }
        CHECK(idx >= 0);
        CHECK(found.contains(QStringLiteral("editme")));
        // clicking the run selects it; the blank margin selects nothing
        const QList<PdfDocument::PageObject> objs0 = d.pageObjects(0);
        CHECK(!PdfDocument::objectsAt(objs0, d.textRuns(0).first().rect.center()).isEmpty());
        CHECK(PdfDocument::objectsAt(objs0, QPointF(5, 5)).isEmpty());
        CHECK(d.setTextObject(0, idx, QStringLiteral("changed")));
        const QString path = tmp + QStringLiteral("/angra-test-edit.pdf");
        CHECK(d.saveCopy(path));
        PdfDocument re;
        CHECK(re.load(path) == PdfDocument::Status::Ok);
        CHECK(re.pageText(0).contains(QStringLiteral("changed")));
        CHECK(!re.pageText(0).contains(QStringLiteral("editme")));
        (void)pts;

        MARK("quad-hit-test");
        // objectsAt is a pure function over an enumerated list, so the rotated
        // cases are built by hand rather than needing a rotated fixture.
        {
            auto rect = [](double x, double y, double w, double h) {
                return QPolygonF({QPointF(x, y), QPointF(x + w, y),
                                  QPointF(x + w, y + h), QPointF(x, y + h)});
            };
            PdfDocument::PageObject flat;
            flat.index = 0;
            flat.kind = PdfDocument::ObjectKind::Text;
            flat.quad = rect(100, 100, 80, 20);
            flat.bounds = flat.quad.boundingRect();

            // a diamond: its bounding-box corners sit well outside the shape
            PdfDocument::PageObject turned;
            turned.index = 1;
            turned.kind = PdfDocument::ObjectKind::Path;
            turned.quad = QPolygonF({QPointF(300, 200), QPointF(340, 240),
                                     QPointF(300, 280), QPointF(260, 240)});
            turned.bounds = turned.quad.boundingRect();

            // Regression: PDFium hands back /QuadPoints Z-order (UL, UR, LL,
            // LR). Used as a traversal order that is a bowtie whose interior
            // excludes the object's middle, so every click missed. pageObjects
            // must reorder it into a convex ring before storing it.
            const QList<PdfDocument::PageObject> real = d.pageObjects(0);
            CHECK(!real.isEmpty());
            for (const PdfDocument::PageObject& o : real) {
                CHECK(o.quad.size() == 4);
                // A bowtie's odd-even interior excludes the middle, so this is
                // the assertion that fails when the point order is wrong.
                const QPointF centre = o.bounds.center();
                CHECK(o.quad.containsPoint(centre, Qt::OddEvenFill));
                CHECK(!PdfDocument::objectsAt(real, centre).isEmpty());
            }

            const QList<PdfDocument::PageObject> objs{flat, turned};
            // 1. unrotated hit
            CHECK(PdfDocument::objectsAt(objs, QPointF(140, 110)).contains(0));
            // 2. unrotated miss
            CHECK(PdfDocument::objectsAt(objs, QPointF(140, 400)).isEmpty());
            // 3. rotated hit, inside the real quad
            CHECK(PdfDocument::objectsAt(objs, QPointF(300, 240)).contains(1));
            // 4. inside the rotated AABB but outside the quad: must miss
            CHECK(turned.bounds.contains(QPointF(263, 203))); // a box corner
            CHECK(!PdfDocument::objectsAt(objs, QPointF(263, 203)).contains(1));
            // 6. click tolerance still applies to axis-aligned objects, which
            //    is what keeps thin text selectable
            CHECK(PdfDocument::objectsAt(objs, QPointF(140, 99)).contains(0));
            // topmost first: later list position paints on top
            PdfDocument::PageObject over = flat;
            over.index = 2;
            const QList<int> stacked =
                PdfDocument::objectsAt({flat, over}, QPointF(140, 110));
            CHECK(stacked.size() == 2);
            CHECK(stacked.first() == 1);
        }

        MARK("display-list-move-undo");
        // Render -> detect -> select -> move -> undo -> save, and confirm the
        // objects that were not touched did not shift.
        {
            PdfDocument d2;
            CHECK(d2.load(testPdf) == PdfDocument::Status::Ok);
            const QList<PdfDocument::PageObject> before = d2.pageObjects(0);
            CHECK(!before.isEmpty());
            // every enumerated object carries a real quad whose bounding box
            // is the bounds field the rest of the code uses
            for (const PdfDocument::PageObject& o : before) {
                CHECK(o.quad.size() == 4);
                CHECK(qAbs(o.quad.boundingRect().left() - o.bounds.left()) < 0.01);
                CHECK(qAbs(o.quad.boundingRect().top() - o.bounds.top()) < 0.01);
            }
            int textIdx = -1;
            for (int i = 0; i < before.size(); ++i)
                if (before.at(i).kind == PdfDocument::ObjectKind::Text) {
                    textIdx = i;
                    break;
                }
            CHECK(textIdx >= 0);
            // z-order is the page-object index, so it must be strictly ascending
            for (int i = 1; i < before.size(); ++i)
                CHECK(before.at(i).index > before.at(i - 1).index);

            const QRectF origin = before.at(textIdx).bounds;
            constexpr double kDx = 12.0, kDy = 7.0;
            CHECK(d2.moveObject(0, before.at(textIdx).index, kDx, kDy));
            CHECK(d2.isModified());
            const QList<PdfDocument::PageObject> moved = d2.pageObjects(0);
            CHECK(moved.size() == before.size()); // moving creates nothing
            const QRectF shifted = moved.at(textIdx).bounds;
            CHECK(qAbs(shifted.left() - (origin.left() + kDx)) < 0.6);
            CHECK(qAbs(shifted.top() - (origin.top() + kDy)) < 0.6); // dy is downward
            CHECK(qAbs(shifted.width() - origin.width()) < 0.6);     // translate only

            // every other object stayed exactly where it was
            for (int i = 0; i < before.size(); ++i) {
                if (i == textIdx)
                    continue;
                CHECK(qAbs(moved.at(i).bounds.left() - before.at(i).bounds.left()) < 0.01);
                CHECK(qAbs(moved.at(i).bounds.top() - before.at(i).bounds.top()) < 0.01);
            }

            // undo is the negated delta
            CHECK(d2.moveObject(0, before.at(textIdx).index, -kDx, -kDy));
            const QRectF back = d2.pageObjects(0).at(textIdx).bounds;
            CHECK(qAbs(back.left() - origin.left()) < 0.6);
            CHECK(qAbs(back.top() - origin.top()) < 0.6);

            // move again, then verify it survives save + reopen
            CHECK(d2.moveObject(0, before.at(textIdx).index, kDx, kDy));
            const QString movePath = tmp + QStringLiteral("/angra-test-move.pdf");
            CHECK(d2.saveCopy(movePath));
            PdfDocument mr;
            CHECK(mr.load(movePath) == PdfDocument::Status::Ok);
            const QList<PdfDocument::PageObject> reopened = mr.pageObjects(0);
            CHECK(reopened.size() == before.size());
            CHECK(qAbs(reopened.at(textIdx).bounds.left() - (origin.left() + kDx)) < 0.6);
        }

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
            CHECK(!s.takeFontSubstituted()); // nothing written yet
            CHECK(s.setTextObject(0, runs.at(last).index, QStringLiteral("omega")));
            // addTextPage uses a base-14 font, so this must encode directly
            // and report no substitution; the flag also clears on read.
            CHECK(!s.takeFontSubstituted());
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
        for (const PdfDocument::TextRun& r : re.textRuns(0))
            if (r.text.contains(QStringLiteral("changed")))
                idx2 = r.index;
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
    {
        MARK("attachments");
        // Embedded files live in the document, not the pages, so the payload
        // must come back byte-identical after a save and reopen.
        PdfDocument a;
        CHECK(a.load(testPdf) == PdfDocument::Status::Ok);
        CHECK(a.attachments().isEmpty());
        const QByteArray payload("angra-attachment-payload\x00\x01\xFE binary", 38);
        CHECK(a.addAttachment(QStringLiteral("notes.bin"), payload));
        CHECK(a.isModified());
        CHECK(a.attachments().size() == 1);
        CHECK(a.attachments().first().name == QStringLiteral("notes.bin"));
        CHECK(a.attachments().first().size == payload.size());
        CHECK(a.attachmentData(0) == payload);

        const QString path = tmp + QStringLiteral("/angra-test-attach.pdf");
        CHECK(a.saveCopy(path));
        PdfDocument re;
        CHECK(re.load(path) == PdfDocument::Status::Ok);
        CHECK(re.attachments().size() == 1);
        CHECK(re.attachmentData(0) == payload); // survives the round trip
        CHECK(re.pageCount() == 2);             // pages untouched

        CHECK(re.removeAttachment(0));
        CHECK(re.attachments().isEmpty());
        CHECK(re.attachmentData(0).isEmpty()); // gone, and no crash on reread
    }

    PdfDocument::shutdownLibrary();
    std::puts("ok");
    return 0;
}
