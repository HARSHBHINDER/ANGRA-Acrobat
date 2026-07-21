#include "PdfDocument.h"
#include "Theme.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <algorithm>

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(theme::kAppName);
        resize(1000, 800);

        m_canvas = new QLabel(tr("Open a PDF to begin (Ctrl+O)"));
        m_canvas->setAlignment(Qt::AlignCenter);
        m_canvas->setForegroundRole(QPalette::BrightText);

        m_scroll = new QScrollArea;
        m_scroll->setWidget(m_canvas);
        m_scroll->setAlignment(Qt::AlignCenter);
        m_scroll->viewport()->setAutoFillBackground(true);
        QPalette pal = m_scroll->viewport()->palette();
        pal.setColor(QPalette::Window, QColor(theme::kCanvasBackground));
        m_scroll->viewport()->setPalette(pal);
        setCentralWidget(m_scroll);

        QToolBar* tb = addToolBar(tr("Main"));
        tb->setMovable(false);

        QAction* openAct = tb->addAction(tr("Open..."), [this] { openFile(); });
        openAct->setShortcut(QKeySequence::Open);
        openAct->setToolTip(tr("Open a PDF document (Ctrl+O)"));

        m_prevAct = tb->addAction(tr("Previous"), [this] { gotoPage(m_page - 1); });
        m_prevAct->setShortcut(QKeySequence(Qt::Key_PageUp));
        m_nextAct = tb->addAction(tr("Next"), [this] { gotoPage(m_page + 1); });
        m_nextAct->setShortcut(QKeySequence(Qt::Key_PageDown));

        m_zoomOutAct =
            tb->addAction(tr("Zoom Out"), [this] { setZoom(m_zoom / theme::kZoomStep); });
        m_zoomOutAct->setShortcut(QKeySequence::ZoomOut);
        m_zoomInAct = tb->addAction(tr("Zoom In"), [this] { setZoom(m_zoom * theme::kZoomStep); });
        m_zoomInAct->setShortcut(QKeySequence::ZoomIn);

        m_fitPageAct = tb->addAction(tr("Fit Page"), [this] {
            m_fit = FitMode::Page;
            applyFit();
        });
        m_fitWidthAct = tb->addAction(tr("Fit Width"), [this] {
            m_fit = FitMode::Width;
            applyFit();
        });

        tb->addSeparator();
        m_pageLabel = new QLabel(tr("No document"));
        tb->addWidget(m_pageLabel);

        QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
        fileMenu->addAction(openAct);
        fileMenu->addSeparator();
        fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

        QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
        helpMenu->addAction(tr("&About %1").arg(theme::kAppName), [this] {
            QMessageBox::about(
                this, tr("About %1").arg(theme::kAppName),
                tr("<b>%1</b> %2<br>An offline-first PDF workstation for Windows.<br>"
                   "Licensed under Apache-2.0. Uses Qt and PDFium; see "
                   "THIRD_PARTY_NOTICES.md.")
                    .arg(theme::kAppName, theme::kAppVersion));
        });

        statusBar()->showMessage(tr("Ready"));
        updateUi();
    }

    void openFile(QString path = {}) {
        if (path.isEmpty()) {
            path = QFileDialog::getOpenFileName(this, tr("Open PDF"), {},
                                                tr("PDF files (*.pdf)"));
            if (path.isEmpty())
                return;
        }
        const QString name = QFileInfo(path).fileName();
        statusBar()->showMessage(tr("Loading %1...").arg(name));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const PdfDocument::Status status = m_doc.load(path);
        QApplication::restoreOverrideCursor();

        if (status != PdfDocument::Status::Ok) {
            QString msg;
            switch (status) {
            case PdfDocument::Status::FileError:
                msg = tr("Cannot read the file. Check that it exists and is accessible.");
                break;
            case PdfDocument::Status::PasswordRequired:
                msg = tr("This PDF is password-protected. Password support arrives in a "
                         "later release.");
                break;
            default:
                msg = tr("The file is not a valid PDF or is damaged.");
                break;
            }
            m_canvas->setPixmap(QPixmap());
            m_canvas->setText(msg);
            m_canvas->adjustSize();
            setWindowTitle(theme::kAppName);
            statusBar()->showMessage(tr("Failed to open %1").arg(name));
            QMessageBox::warning(this, theme::kAppName, msg);
            updateUi();
            return;
        }

        m_page = 0;
        m_fit = FitMode::Page;
        setWindowTitle(QStringLiteral("%1 - %2").arg(theme::kAppName, name));
        applyFit();
        statusBar()->showMessage(tr("Opened %1 (%2 pages)").arg(name).arg(m_doc.pageCount()));
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        if (m_doc.isLoaded() && m_fit != FitMode::Custom)
            applyFit();
    }

private:
    enum class FitMode { Custom, Page, Width };

    void gotoPage(int page) {
        if (!m_doc.isLoaded() || page < 0 || page >= m_doc.pageCount())
            return;
        m_page = page;
        if (m_fit != FitMode::Custom)
            applyFit(); // page sizes can differ
        else
            render();
    }

    void setZoom(double zoom) {
        if (!m_doc.isLoaded())
            return;
        m_fit = FitMode::Custom;
        m_zoom = std::clamp(zoom, theme::kZoomMin, theme::kZoomMax);
        render();
    }

    void applyFit() {
        const QSizeF pt = m_doc.pageSizePoints(m_page);
        if (pt.isEmpty())
            return;
        QSize avail = m_scroll->viewport()->size();
        avail -= QSize(2 * theme::kCanvasMargin, 2 * theme::kCanvasMargin);
        double zoom;
        if (m_fit == FitMode::Width) {
            const int sb = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
            zoom = (avail.width() - sb) / pt.width();
        } else {
            zoom = std::min(avail.width() / pt.width(), avail.height() / pt.height());
        }
        m_zoom = std::clamp(zoom, theme::kZoomMin, theme::kZoomMax);
        render();
    }

    void render() {
        const qreal dpr = devicePixelRatioF();
        QImage image = m_doc.renderPage(m_page, m_zoom * dpr);
        if (image.isNull()) {
            m_canvas->setText(tr("Failed to render page %1.").arg(m_page + 1));
            m_canvas->adjustSize();
            updateUi();
            return;
        }
        image.setDevicePixelRatio(dpr);
        m_canvas->setPixmap(QPixmap::fromImage(std::move(image)));
        m_canvas->adjustSize();
        updateUi();
    }

    void updateUi() {
        const bool loaded = m_doc.isLoaded();
        m_prevAct->setEnabled(loaded && m_page > 0);
        m_nextAct->setEnabled(loaded && m_page + 1 < m_doc.pageCount());
        for (QAction* a : {m_zoomInAct, m_zoomOutAct, m_fitPageAct, m_fitWidthAct})
            a->setEnabled(loaded);
        m_pageLabel->setText(loaded ? tr("Page %1 of %2").arg(m_page + 1).arg(m_doc.pageCount())
                                    : tr("No document"));
    }

    PdfDocument m_doc;
    int m_page = 0;
    double m_zoom = 1.0;
    FitMode m_fit = FitMode::Page;
    QLabel* m_canvas = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_pageLabel = nullptr;
    QAction* m_prevAct = nullptr;
    QAction* m_nextAct = nullptr;
    QAction* m_zoomInAct = nullptr;
    QAction* m_zoomOutAct = nullptr;
    QAction* m_fitPageAct = nullptr;
    QAction* m_fitWidthAct = nullptr;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(theme::kAppName);
    QApplication::setApplicationVersion(theme::kAppVersion);
    PdfDocument::initLibrary();
    int rc;
    {
        MainWindow window;
        window.show();
        const QStringList args = QApplication::arguments();
        if (args.size() > 1)
            window.openFile(args.at(1));
        rc = QApplication::exec();
    }
    PdfDocument::shutdownLibrary();
    return rc;
}
