#include "PdfDocument.h"
#include "Theme.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPrintDialog>
#include <QPrinter>
#include <QRubberBand>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <algorithm>
#include <functional>

// One open document: canvas + thumbnails + selection/search/ink state.
class DocumentTab : public QWidget {
public:
    enum class Tool { Select, Ink };
    enum class FitMode { Custom, Page, Width };

    std::function<void()> onChanged; // set by MainWindow: refresh actions/panels

    DocumentTab() {
        auto* splitter = new QSplitter(this);

        m_thumbs = new QListWidget;
        m_thumbs->setIconSize(QSize(120, 160));
        m_thumbs->setFixedWidth(160);
        m_thumbs->setUniformItemSizes(true);
        QObject::connect(m_thumbs, &QListWidget::currentRowChanged, m_thumbs,
                         [this](int row) {
                             if (!m_syncingThumbs && row >= 0)
                                 gotoPage(row);
                         });

        m_canvas = new QLabel;
        m_canvas->setAlignment(Qt::AlignCenter);
        m_canvas->installEventFilter(this);
        m_canvas->setMouseTracking(false);

        m_scroll = new QScrollArea;
        m_scroll->setWidget(m_canvas);
        m_scroll->setAlignment(Qt::AlignCenter);
        m_scroll->viewport()->setAutoFillBackground(true);
        QPalette pal = m_scroll->viewport()->palette();
        pal.setColor(QPalette::Window, QColor(theme::kCanvasBackground));
        m_scroll->viewport()->setPalette(pal);

        splitter->addWidget(m_thumbs);
        splitter->addWidget(m_scroll);
        splitter->setStretchFactor(1, 1);

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addWidget(splitter);

        m_rubber = new QRubberBand(QRubberBand::Rectangle, m_canvas);
    }

    PdfDocument& doc() { return m_doc; }
    int page() const { return m_page; }
    QString selectionText() const { return m_selText; }
    QList<QRectF> selectionRects() const {
        return m_selRect.isNull() ? QList<QRectF>{} : QList<QRectF>{m_selRect};
    }
    QPointF lastClickPagePt() const { return m_lastPagePt; }
    void setTool(Tool t) { m_tool = t; }

    bool open(const QString& path) {
        QString password;
        for (;;) {
            const PdfDocument::Status st = m_doc.load(path, password);
            if (st == PdfDocument::Status::Ok)
                break;
            if (st == PdfDocument::Status::PasswordRequired) {
                bool ok = false;
                password = QInputDialog::getText(
                    this, tr("Password required"),
                    tr("%1 is protected. Enter the open password:")
                        .arg(QFileInfo(path).fileName()),
                    QLineEdit::Password, {}, &ok);
                if (ok && !password.isEmpty())
                    continue;
                return false;
            }
            QMessageBox::warning(this, theme::kAppName,
                                 st == PdfDocument::Status::FileError
                                     ? tr("Cannot read the file.")
                                     : tr("The file is not a valid PDF or is damaged."));
            return false;
        }
        m_page = 0;
        m_fit = FitMode::Page;
        clearSearch();
        clearSelection();
        applyFit();
        refreshThumbs();
        return true;
    }

    void gotoPage(int page) {
        if (!m_doc.isLoaded() || page < 0 || page >= m_doc.pageCount() || page == m_page) {
            if (page == m_page)
                syncThumb();
            return;
        }
        m_page = page;
        clearSelection();
        if (m_fit != FitMode::Custom)
            applyFit();
        else
            render();
        syncThumb();
    }

    void setZoom(double zoom) {
        if (!m_doc.isLoaded())
            return;
        m_fit = FitMode::Custom;
        m_zoom = std::clamp(zoom, theme::kZoomMin, theme::kZoomMax);
        render();
    }
    double zoom() const { return m_zoom; }

    void setFit(FitMode fit) {
        m_fit = fit;
        if (m_doc.isLoaded())
            applyFit();
    }

    void applyFit() {
        if (m_fit == FitMode::Custom) {
            render();
            return;
        }
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
        if (!m_doc.isLoaded())
            return;
        const qreal dpr = devicePixelRatioF();
        QImage image = m_doc.renderPage(m_page, m_zoom * dpr);
        if (image.isNull()) {
            m_canvas->setText(tr("Failed to render page %1.").arg(m_page + 1));
            m_canvas->adjustSize();
            changed();
            return;
        }
        if (m_searchPageIdx == m_page && !m_searchRects.isEmpty()) {
            QPainter p(&image);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 235, 59, 110));
            const double s = m_zoom * dpr;
            for (const QRectF& r : m_searchRects)
                p.drawRect(QRectF(r.left() * s, r.top() * s, r.width() * s, r.height() * s));
        }
        image.setDevicePixelRatio(dpr);
        m_canvas->setPixmap(QPixmap::fromImage(std::move(image)));
        m_canvas->adjustSize();
        changed();
    }

    // --- search: page-granular; highlights every match on the found page ---
    bool search(const QString& term, int fromPage) {
        if (!m_doc.isLoaded() || term.isEmpty())
            return false;
        const int count = m_doc.pageCount();
        for (int i = 0; i < count; ++i) {
            const int p = (fromPage + i) % count;
            const QList<QRectF> rects = m_doc.searchPage(p, term);
            if (!rects.isEmpty()) {
                m_searchTerm = term;
                m_searchPageIdx = p;
                m_searchRects = rects;
                if (p != m_page)
                    gotoPage(p);
                else
                    render();
                return true;
            }
        }
        clearSearch();
        render();
        return false;
    }
    bool findNext() {
        return !m_searchTerm.isEmpty() && search(m_searchTerm, m_searchPageIdx + 1);
    }
    void clearSearch() {
        m_searchTerm.clear();
        m_searchRects.clear();
        m_searchPageIdx = -1;
    }

    void refreshThumbs() {
        m_syncingThumbs = true;
        m_thumbs->clear();
        const int count = m_doc.pageCount();
        // ponytail: sync thumbs capped at 200 pages; background rendering when it hurts
        if (count > 0 && count <= 200) {
            for (int i = 0; i < count; ++i) {
                const QSizeF pt = m_doc.pageSizePoints(i);
                const double s = pt.width() > 0 ? 120.0 / pt.width() : 0.15;
                auto* item = new QListWidgetItem(
                    QIcon(QPixmap::fromImage(m_doc.renderPage(i, s))),
                    QString::number(i + 1));
                m_thumbs->addItem(item);
            }
            m_thumbs->setCurrentRow(m_page);
        }
        m_syncingThumbs = false;
    }

    void afterStructureChange() {
        m_page = std::clamp(m_page, 0, std::max(0, m_doc.pageCount() - 1));
        clearSearch();
        clearSelection();
        applyFit();
        refreshThumbs();
    }

    void print() {
        if (!m_doc.isLoaded())
            return;
        QPrinter printer(QPrinter::HighResolution);
        QPrintDialog dlg(&printer, this);
        dlg.setWindowTitle(tr("Print"));
        if (dlg.exec() != QDialog::Accepted)
            return;
        QPainter painter(&printer);
        const int count = m_doc.pageCount();
        for (int i = 0; i < count; ++i) {
            if (i > 0)
                printer.newPage();
            const QSizeF pt = m_doc.pageSizePoints(i);
            const QRectF target = printer.pageRect(QPrinter::DevicePixel);
            if (pt.isEmpty() || target.isEmpty())
                continue;
            const double s =
                std::min(target.width() / pt.width(), target.height() / pt.height());
            const QImage img = m_doc.renderPage(i, s);
            if (!img.isNull())
                painter.drawImage(QPointF(0, 0), img);
        }
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        if (m_doc.isLoaded() && m_fit != FitMode::Custom)
            applyFit();
    }

    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (obj != m_canvas || !m_doc.isLoaded())
            return QWidget::eventFilter(obj, ev);
        const auto* me = static_cast<QMouseEvent*>(ev);
        switch (ev->type()) {
        case QEvent::MouseButtonPress:
            if (me->button() == Qt::LeftButton) {
                m_dragStart = me->position().toPoint();
                if (m_tool == Tool::Ink) {
                    m_stroke.clear();
                    m_stroke << toPagePt(m_dragStart);
                } else {
                    m_rubber->setGeometry(QRect(m_dragStart, QSize()));
                    m_rubber->show();
                }
                return true;
            }
            break;
        case QEvent::MouseMove:
            if (me->buttons() & Qt::LeftButton) {
                if (m_tool == Tool::Ink)
                    m_stroke << toPagePt(me->position().toPoint());
                else
                    m_rubber->setGeometry(
                        QRect(m_dragStart, me->position().toPoint()).normalized());
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (me->button() == Qt::LeftButton) {
                const QPoint end = me->position().toPoint();
                if (m_tool == Tool::Ink) {
                    if (m_stroke.size() > 1) {
                        m_doc.addInk(m_page, {m_stroke}, QColor(200, 30, 30), 2.0);
                        render();
                    }
                    m_stroke.clear();
                    return true;
                }
                m_rubber->hide();
                if ((end - m_dragStart).manhattanLength() < 4) {
                    handleClick(toPagePt(end));
                } else {
                    const QRectF sel =
                        QRectF(toPagePt(m_dragStart), toPagePt(end)).normalized();
                    m_selRect = sel;
                    m_selText = m_doc.textInRect(m_page, sel);
                    changed();
                }
                return true;
            }
            break;
        default:
            break;
        }
        return QWidget::eventFilter(obj, ev);
    }

private:
    QPointF toPagePt(const QPoint& widgetPos) const {
        return QPointF(widgetPos.x() / m_zoom, widgetPos.y() / m_zoom);
    }

    void handleClick(const QPointF& pagePt) {
        m_lastPagePt = pagePt;
        clearSelection();
        const PdfLinkHit hit = m_doc.linkAt(m_page, pagePt);
        if (hit.type == PdfLinkHit::Type::Page && hit.page >= 0) {
            gotoPage(hit.page);
        } else if (hit.type == PdfLinkHit::Type::Uri) {
            // external-link safety: never open silently
            if (QMessageBox::question(
                    this, theme::kAppName,
                    tr("Open this external link in your browser?\n\n%1").arg(hit.uri)) ==
                QMessageBox::Yes)
                QDesktopServices::openUrl(QUrl(hit.uri));
        }
        changed();
    }

    void clearSelection() {
        m_selRect = QRectF();
        m_selText.clear();
    }

    void changed() {
        if (onChanged)
            onChanged();
    }

    void syncThumb() {
        if (m_thumbs->count() > m_page) {
            m_syncingThumbs = true;
            m_thumbs->setCurrentRow(m_page);
            m_syncingThumbs = false;
        }
    }

    PdfDocument m_doc;
    int m_page = 0;
    double m_zoom = 1.0;
    FitMode m_fit = FitMode::Page;
    Tool m_tool = Tool::Select;

    QLabel* m_canvas = nullptr;
    QScrollArea* m_scroll = nullptr;
    QListWidget* m_thumbs = nullptr;
    QRubberBand* m_rubber = nullptr;
    bool m_syncingThumbs = false;

    QPoint m_dragStart;
    QPolygonF m_stroke;
    QRectF m_selRect;
    QString m_selText;
    QPointF m_lastPagePt;

    QString m_searchTerm;
    QList<QRectF> m_searchRects;
    int m_searchPageIdx = -1;
};

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(theme::kAppName);
        resize(1200, 850);

        m_tabs = new QTabWidget;
        m_tabs->setTabsClosable(true);
        m_tabs->setMovable(true);
        m_tabs->setDocumentMode(true);
        setCentralWidget(m_tabs);
        QObject::connect(m_tabs, &QTabWidget::tabCloseRequested, m_tabs,
                         [this](int i) { closeTab(i); });
        QObject::connect(m_tabs, &QTabWidget::currentChanged, m_tabs,
                         [this](int) { refreshPanels(); });

        m_bookmarkTree = new QTreeWidget;
        m_bookmarkTree->setHeaderLabel(tr("Bookmarks"));
        QObject::connect(m_bookmarkTree, &QTreeWidget::itemActivated, m_bookmarkTree,
                         [this](QTreeWidgetItem* item, int) {
                             const int page = item->data(0, Qt::UserRole).toInt();
                             if (DocumentTab* t = tab(); t && page >= 0)
                                 t->gotoPage(page);
                         });
        auto* dock = new QDockWidget(tr("Bookmarks"), this);
        dock->setObjectName("bookmarksDock");
        dock->setWidget(m_bookmarkTree);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        m_bookmarkDock = dock;

        buildToolbarAndMenus();
        statusBar()->showMessage(tr("Ready"));
        updateUi();
    }

    void openPath(const QString& path) {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (tabAt(i)->doc().filePath() == path) { // no duplicate sessions
                m_tabs->setCurrentIndex(i);
                return;
            }
        }
        auto* t = new DocumentTab;
        t->onChanged = [this] { updateUi(); };
        if (!t->open(path)) {
            delete t;
            return;
        }
        const QString name = QFileInfo(path).fileName();
        m_tabs->setCurrentIndex(m_tabs->addTab(t, name));
        addRecent(path);
        refreshPanels();
        statusBar()->showMessage(tr("Opened %1 (%2 pages)").arg(name).arg(t->doc().pageCount()));
    }

protected:
    void closeEvent(QCloseEvent* ev) override {
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (tabAt(i)->doc().isModified()) {
                if (QMessageBox::question(
                        this, theme::kAppName,
                        tr("Documents have unsaved changes. Discard them and exit?")) !=
                    QMessageBox::Yes) {
                    ev->ignore();
                    return;
                }
                break;
            }
        }
        ev->accept();
    }

private:
    // static_cast is safe: the tab widget only ever holds DocumentTabs
    DocumentTab* tab() const { return static_cast<DocumentTab*>(m_tabs->currentWidget()); }
    DocumentTab* tabAt(int i) const { return static_cast<DocumentTab*>(m_tabs->widget(i)); }

    // ---------- UI construction ----------
    void buildToolbarAndMenus() {
        QToolBar* tb = addToolBar(tr("Main"));
        tb->setMovable(false);

        auto act = [](QToolBar* bar, const QString& text, auto fn, QKeySequence key = {}) {
            QAction* a = bar->addAction(text, fn);
            if (!key.isEmpty())
                a->setShortcut(key);
            return a;
        };

        m_openAct = act(tb, tr("Open..."), [this] { openDialog(); }, QKeySequence::Open);
        m_saveAct = act(tb, tr("Save Copy..."), [this] { saveCopyDialog(); },
                        QKeySequence::Save);
        tb->addSeparator();
        m_prevAct = act(tb, tr("Previous"),
                        [this] { if (auto* t = tab()) t->gotoPage(t->page() - 1); },
                        QKeySequence(Qt::Key_PageUp));
        m_nextAct = act(tb, tr("Next"),
                        [this] { if (auto* t = tab()) t->gotoPage(t->page() + 1); },
                        QKeySequence(Qt::Key_PageDown));
        m_zoomOutAct = act(tb, tr("Zoom Out"),
                           [this] { if (auto* t = tab()) t->setZoom(t->zoom() / theme::kZoomStep); },
                           QKeySequence::ZoomOut);
        m_zoomInAct = act(tb, tr("Zoom In"),
                          [this] { if (auto* t = tab()) t->setZoom(t->zoom() * theme::kZoomStep); },
                          QKeySequence::ZoomIn);
        m_fitPageAct = act(tb, tr("Fit Page"),
                           [this] { if (auto* t = tab()) t->setFit(DocumentTab::FitMode::Page); });
        m_fitWidthAct = act(tb, tr("Fit Width"),
                            [this] { if (auto* t = tab()) t->setFit(DocumentTab::FitMode::Width); });
        tb->addSeparator();

        m_searchEdit = new QLineEdit;
        m_searchEdit->setPlaceholderText(tr("Search text (Enter = next)"));
        m_searchEdit->setMaximumWidth(220);
        m_searchEdit->setClearButtonEnabled(true);
        QObject::connect(m_searchEdit, &QLineEdit::returnPressed, m_searchEdit, [this] {
            if (auto* t = tab()) {
                const bool found = t->search(m_searchEdit->text(), t->page());
                statusBar()->showMessage(found ? tr("Found on page %1").arg(t->page() + 1)
                                               : tr("Not found"));
            }
        });
        tb->addWidget(m_searchEdit);
        m_findNextAct = act(tb, tr("Find Next"), [this] {
            if (auto* t = tab())
                statusBar()->showMessage(t->findNext()
                                             ? tr("Found on page %1").arg(t->page() + 1)
                                             : tr("No more matches"));
        }, QKeySequence(Qt::Key_F3));
        tb->addSeparator();
        m_pageLabel = new QLabel(tr("No document"));
        tb->addWidget(m_pageLabel);

        // File
        QMenu* file = menuBar()->addMenu(tr("&File"));
        file->addAction(m_openAct);
        m_recentMenu = file->addMenu(tr("Open &Recent"));
        QObject::connect(m_recentMenu, &QMenu::aboutToShow, m_recentMenu,
                         [this] { rebuildRecentMenu(); });
        file->addSeparator();
        file->addAction(m_saveAct);
        m_propsAct = file->addAction(tr("Document &Properties..."),
                                     [this] { showProperties(); });
        m_printAct = file->addAction(tr("&Print..."), QKeySequence::Print,
                                     [this] { if (auto* t = tab()) t->print(); });
        file->addSeparator();
        m_closeTabAct = file->addAction(tr("&Close Tab"), QKeySequence(Qt::CTRL | Qt::Key_W),
                                        [this] { closeTab(m_tabs->currentIndex()); });
        file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

        // Edit
        QMenu* edit = menuBar()->addMenu(tr("&Edit"));
        m_copyAct = edit->addAction(tr("&Copy Selected Text"), QKeySequence::Copy, [this] {
            if (auto* t = tab(); t && !t->selectionText().isEmpty()) {
                QApplication::clipboard()->setText(t->selectionText());
                statusBar()->showMessage(tr("Copied %1 characters")
                                             .arg(t->selectionText().size()));
            }
        });
        edit->addAction(tr("&Find"), QKeySequence::Find, this,
                        [this] { m_searchEdit->setFocus(); });

        // View
        QMenu* view = menuBar()->addMenu(tr("&View"));
        view->addAction(m_zoomInAct);
        view->addAction(m_zoomOutAct);
        view->addAction(m_fitPageAct);
        view->addAction(m_fitWidthAct);
        view->addSeparator();
        view->addAction(m_bookmarkDock->toggleViewAction());

        // Document
        QMenu* docm = menuBar()->addMenu(tr("&Document"));
        m_rotateAct = docm->addAction(tr("&Rotate Page 90°"), [this] {
            if (auto* t = tab()) {
                t->doc().setPageRotation(t->page(), t->doc().pageRotation(t->page()) + 1);
                t->afterStructureChange();
            }
        });
        m_deletePageAct = docm->addAction(tr("&Delete Page"), [this] { deleteCurrentPage(); });
        m_extractAct = docm->addAction(tr("&Extract Current Page..."),
                                       [this] { extractCurrentPage(); });
        docm->addSeparator();
        m_insertAct = docm->addAction(tr("&Insert Pages From File..."),
                                      [this] { insertFromFile(); });
        m_mergeAct = docm->addAction(tr("&Merge Files Into Document..."),
                                     [this] { mergeFiles(); });
        m_splitAct = docm->addAction(tr("&Split Into Single Pages..."), [this] { splitAll(); });
        docm->addSeparator();
        m_flattenAct = docm->addAction(tr("&Flatten Annotations"), [this] {
            if (auto* t = tab(); t && t->doc().flattenAllPages()) {
                t->render();
                statusBar()->showMessage(tr("Annotations flattened"));
            }
        });

        // Comment
        QMenu* comment = menuBar()->addMenu(tr("&Comment"));
        m_highlightAct = comment->addAction(tr("&Highlight Selection"), [this] {
            if (auto* t = tab(); t && !t->selectionRects().isEmpty()) {
                t->doc().addHighlight(t->page(), t->selectionRects(),
                                      QColor(255, 220, 0, 160));
                t->render();
            }
        });
        m_noteAct = comment->addAction(tr("Add &Note At Last Click..."), [this] {
            if (auto* t = tab()) {
                bool ok = false;
                const QString text = QInputDialog::getMultiLineText(
                    this, tr("Add Note"), tr("Note text:"), {}, &ok);
                if (ok && !text.isEmpty()) {
                    t->doc().addNote(t->page(), t->lastClickPagePt(), text);
                    t->render();
                }
            }
        });
        m_squareAct = comment->addAction(tr("Add &Rectangle From Selection"), [this] {
            if (auto* t = tab(); t && !t->selectionRects().isEmpty()) {
                t->doc().addSquare(t->page(), t->selectionRects().first(),
                                   QColor(200, 30, 30));
                t->render();
            }
        });
        comment->addSeparator();
        m_selectToolAct = comment->addAction(tr("&Select Tool"));
        m_inkToolAct = comment->addAction(tr("&Draw Ink Tool"));
        m_selectToolAct->setCheckable(true);
        m_inkToolAct->setCheckable(true);
        m_selectToolAct->setChecked(true);
        auto* group = new QActionGroup(this);
        group->addAction(m_selectToolAct);
        group->addAction(m_inkToolAct);
        QObject::connect(group, &QActionGroup::triggered, group, [this](QAction* a) {
            if (auto* t = tab())
                t->setTool(a == m_inkToolAct ? DocumentTab::Tool::Ink
                                             : DocumentTab::Tool::Select);
        });

        // Convert
        QMenu* convert = menuBar()->addMenu(tr("Con&vert"));
        convert->addAction(tr("&Images to PDF..."), [this] { imagesToPdf(); });
        convert->addAction(tr("&Text File to PDF..."), [this] { textToPdf(); });
        m_toImagesAct = convert->addAction(tr("PDF to I&mages..."), [this] { pdfToImages(); });
        m_toTextAct = convert->addAction(tr("PDF to Te&xt..."), [this] { pdfToText(); });

        // Help
        QMenu* help = menuBar()->addMenu(tr("&Help"));
        help->addAction(tr("&About %1").arg(theme::kAppName), [this] {
            QMessageBox::about(
                this, tr("About %1").arg(theme::kAppName),
                tr("<b>%1</b> %2<br>An offline-first PDF workstation for Windows.<br>"
                   "Licensed under Apache-2.0. Uses Qt and PDFium; see "
                   "THIRD_PARTY_NOTICES.md.")
                    .arg(theme::kAppName, theme::kAppVersion));
        });
    }

    // ---------- commands ----------
    void openDialog() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open PDF"), {},
                                                          tr("PDF files (*.pdf)"));
        if (!path.isEmpty())
            openPath(path);
    }

    void saveCopyDialog() {
        auto* t = tab();
        if (!t)
            return;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save Copy"), t->doc().filePath(), tr("PDF files (*.pdf)"));
        if (path.isEmpty())
            return;
        QString err;
        if (t->doc().saveCopy(path, &err)) {
            statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()));
            updateUi();
        } else {
            QMessageBox::warning(this, theme::kAppName,
                                 tr("Save failed: %1 The original file was not touched.")
                                     .arg(err));
        }
    }

    void closeTab(int index) {
        DocumentTab* t = tabAt(index);
        if (!t)
            return;
        if (t->doc().isModified()) {
            const auto ret = QMessageBox::question(
                this, theme::kAppName,
                tr("This document has unsaved changes. Save a copy before closing?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
            if (ret == QMessageBox::Cancel)
                return;
            if (ret == QMessageBox::Save) {
                m_tabs->setCurrentIndex(index);
                saveCopyDialog();
                if (t->doc().isModified()) // user cancelled the save dialog
                    return;
            }
        }
        m_tabs->removeTab(index);
        delete t;
        refreshPanels();
    }

    void deleteCurrentPage() {
        auto* t = tab();
        if (!t || t->doc().pageCount() <= 1)
            return;
        if (QMessageBox::question(this, theme::kAppName,
                                  tr("Delete page %1?").arg(t->page() + 1)) !=
            QMessageBox::Yes)
            return;
        t->doc().deletePage(t->page());
        t->afterStructureChange();
    }

    void extractCurrentPage() {
        auto* t = tab();
        if (!t)
            return;
        const QString path = QFileDialog::getSaveFileName(this, tr("Extract Page"), {},
                                                          tr("PDF files (*.pdf)"));
        if (path.isEmpty())
            return;
        PdfDocument out;
        QString err;
        if (out.createEmpty() &&
            out.importRange(t->doc(), QByteArray::number(t->page() + 1), 0) &&
            out.saveCopy(path, &err))
            statusBar()->showMessage(tr("Extracted page %1").arg(t->page() + 1));
        else
            QMessageBox::warning(this, theme::kAppName, tr("Extract failed. %1").arg(err));
    }

    void insertFromFile() {
        auto* t = tab();
        if (!t)
            return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Insert Pages From"), {},
                                                          tr("PDF files (*.pdf)"));
        if (path.isEmpty())
            return;
        PdfDocument src;
        if (src.load(path) != PdfDocument::Status::Ok) {
            QMessageBox::warning(this, theme::kAppName, tr("Cannot open %1").arg(path));
            return;
        }
        if (t->doc().importAll(src, t->page() + 1))
            t->afterStructureChange();
    }

    void mergeFiles() {
        auto* t = tab();
        if (!t)
            return;
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("Merge Files (appended in order)"), {}, tr("PDF files (*.pdf)"));
        for (const QString& p : paths) {
            PdfDocument src;
            if (src.load(p) != PdfDocument::Status::Ok ||
                !t->doc().importAll(src, t->doc().pageCount())) {
                QMessageBox::warning(this, theme::kAppName, tr("Skipping %1").arg(p));
            }
        }
        if (!paths.isEmpty())
            t->afterStructureChange();
    }

    void splitAll() {
        auto* t = tab();
        if (!t)
            return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Split Into Folder"));
        if (dir.isEmpty())
            return;
        const QString base = QFileInfo(t->doc().filePath()).completeBaseName();
        int okCount = 0;
        for (int i = 0; i < t->doc().pageCount(); ++i) {
            PdfDocument out;
            const QString dest =
                QDir(dir).filePath(QStringLiteral("%1-page-%2.pdf")
                                       .arg(base.isEmpty() ? QStringLiteral("document") : base)
                                       .arg(i + 1, 3, 10, QLatin1Char('0')));
            if (out.createEmpty() && out.importRange(t->doc(), QByteArray::number(i + 1), 0) &&
                out.saveCopy(dest))
                ++okCount;
        }
        statusBar()->showMessage(tr("Split: wrote %1 files").arg(okCount));
    }

    void imagesToPdf() {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("Images to PDF"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp)"));
        if (paths.isEmpty())
            return;
        PdfDocument out;
        out.createEmpty();
        int added = 0;
        for (const QString& p : paths) {
            const QImage img(p);
            if (!img.isNull() && out.addImagePage(img))
                ++added;
        }
        if (added == 0) {
            QMessageBox::warning(this, theme::kAppName, tr("No readable images."));
            return;
        }
        const QString dest = QFileDialog::getSaveFileName(this, tr("Save PDF"), {},
                                                          tr("PDF files (*.pdf)"));
        QString err;
        if (!dest.isEmpty() && out.saveCopy(dest, &err)) {
            statusBar()->showMessage(tr("Created PDF with %1 pages").arg(added));
            openPath(dest);
        } else if (!dest.isEmpty()) {
            QMessageBox::warning(this, theme::kAppName, err);
        }
    }

    void textToPdf() {
        const QString src = QFileDialog::getOpenFileName(this, tr("Text File to PDF"), {},
                                                         tr("Text files (*.txt *.md)"));
        if (src.isEmpty())
            return;
        QFile f(src);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QMessageBox::warning(this, theme::kAppName, tr("Cannot read %1").arg(src));
            return;
        }
        const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        PdfDocument out;
        out.createEmpty();
        out.addTextPage(lines);
        const QString dest = QFileDialog::getSaveFileName(this, tr("Save PDF"), {},
                                                          tr("PDF files (*.pdf)"));
        QString err;
        if (!dest.isEmpty() && out.saveCopy(dest, &err))
            openPath(dest);
        else if (!dest.isEmpty())
            QMessageBox::warning(this, theme::kAppName, err);
    }

    void pdfToImages() {
        auto* t = tab();
        if (!t)
            return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Export Images To"));
        if (dir.isEmpty())
            return;
        int okCount = 0;
        for (int i = 0; i < t->doc().pageCount(); ++i) {
            const QImage img = t->doc().renderPage(i, 2.0); // 144 dpi
            const QString dest = QDir(dir).filePath(
                QStringLiteral("page-%1.png").arg(i + 1, 3, 10, QLatin1Char('0')));
            if (!img.isNull() && img.save(dest))
                ++okCount;
        }
        statusBar()->showMessage(tr("Exported %1 images").arg(okCount));
    }

    void pdfToText() {
        auto* t = tab();
        if (!t)
            return;
        const QString dest = QFileDialog::getSaveFileName(this, tr("Export Text"), {},
                                                          tr("Text files (*.txt)"));
        if (dest.isEmpty())
            return;
        QStringList parts;
        for (int i = 0; i < t->doc().pageCount(); ++i)
            parts << t->doc().pageText(i);
        QFile f(dest);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text) &&
            f.write(parts.join(QStringLiteral("\n\n")).toUtf8()) >= 0)
            statusBar()->showMessage(tr("Exported text"));
        else
            QMessageBox::warning(this, theme::kAppName, tr("Cannot write %1").arg(dest));
    }

    void showProperties() {
        auto* t = tab();
        if (!t)
            return;
        auto row = [&](const char* tag, const QString& label) {
            const QString v = t->doc().metaText(tag);
            return v.isEmpty() ? QString()
                               : QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                                     .arg(label, v.toHtmlEscaped());
        };
        QString html = QStringLiteral("<table>");
        html += row("Title", tr("Title"));
        html += row("Author", tr("Author"));
        html += row("Subject", tr("Subject"));
        html += row("Keywords", tr("Keywords"));
        html += row("Creator", tr("Creator"));
        html += row("Producer", tr("Producer"));
        html += row("CreationDate", tr("Created"));
        html += row("ModDate", tr("Modified"));
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
                    .arg(tr("Pages"))
                    .arg(t->doc().pageCount());
        html += QStringLiteral("<tr><td><b>%1</b></td><td>0x%2</td></tr>")
                    .arg(tr("Permissions"))
                    .arg(QString::number(t->doc().permissions(), 16));
        html += QStringLiteral("</table>");
        QMessageBox::information(this, tr("Document Properties"), html);
    }

    // ---------- recent files ----------
    void addRecent(const QString& path) {
        QSettings s;
        QStringList list = s.value("recentFiles").toStringList();
        list.removeAll(path);
        list.prepend(path);
        while (list.size() > 8)
            list.removeLast();
        s.setValue("recentFiles", list);
    }

    void rebuildRecentMenu() {
        m_recentMenu->clear();
        const QStringList list = QSettings().value("recentFiles").toStringList();
        for (const QString& path : list)
            m_recentMenu->addAction(QFileInfo(path).fileName(),
                                    [this, path] { openPath(path); });
        m_recentMenu->setEnabled(!list.isEmpty());
    }

    // ---------- state sync ----------
    void refreshPanels() {
        m_bookmarkTree->clear();
        if (auto* t = tab(); t && t->doc().isLoaded()) {
            std::function<void(QTreeWidgetItem*, const QList<PdfBookmark>&)> fill =
                [&](QTreeWidgetItem* parent, const QList<PdfBookmark>& list) {
                    for (const PdfBookmark& b : list) {
                        auto* item = parent
                                         ? new QTreeWidgetItem(parent)
                                         : new QTreeWidgetItem(m_bookmarkTree);
                        item->setText(0, b.title);
                        item->setData(0, Qt::UserRole, b.page);
                        fill(item, b.children);
                    }
                };
            fill(nullptr, t->doc().bookmarks());
        }
        updateUi();
    }

    void updateUi() {
        DocumentTab* t = tab();
        const bool loaded = t && t->doc().isLoaded();
        for (QAction* a :
             {m_saveAct, m_prevAct, m_nextAct, m_zoomInAct, m_zoomOutAct, m_fitPageAct,
              m_fitWidthAct, m_findNextAct, m_propsAct, m_printAct, m_closeTabAct,
              m_rotateAct, m_deletePageAct, m_extractAct, m_insertAct, m_mergeAct,
              m_splitAct, m_flattenAct, m_highlightAct, m_noteAct, m_squareAct,
              m_toImagesAct, m_toTextAct, m_copyAct})
            a->setEnabled(loaded);
        if (loaded) {
            m_prevAct->setEnabled(t->page() > 0);
            m_nextAct->setEnabled(t->page() + 1 < t->doc().pageCount());
            m_deletePageAct->setEnabled(t->doc().pageCount() > 1);
            m_copyAct->setEnabled(!t->selectionText().isEmpty());
            m_highlightAct->setEnabled(!t->selectionRects().isEmpty());
            m_squareAct->setEnabled(!t->selectionRects().isEmpty());
            m_pageLabel->setText(
                tr("Page %1 of %2").arg(t->page() + 1).arg(t->doc().pageCount()));
            const int idx = m_tabs->currentIndex();
            QString title = QFileInfo(t->doc().filePath()).fileName();
            if (t->doc().isModified())
                title += QStringLiteral(" *");
            m_tabs->setTabText(idx, title);
            setWindowTitle(QStringLiteral("%1 - %2").arg(theme::kAppName, title));
        } else {
            m_pageLabel->setText(tr("No document"));
            setWindowTitle(theme::kAppName);
        }
    }

    QTabWidget* m_tabs = nullptr;
    QTreeWidget* m_bookmarkTree = nullptr;
    QDockWidget* m_bookmarkDock = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QLabel* m_pageLabel = nullptr;
    QMenu* m_recentMenu = nullptr;

    QAction *m_openAct = nullptr, *m_saveAct = nullptr, *m_prevAct = nullptr,
            *m_nextAct = nullptr, *m_zoomInAct = nullptr, *m_zoomOutAct = nullptr,
            *m_fitPageAct = nullptr, *m_fitWidthAct = nullptr, *m_findNextAct = nullptr,
            *m_propsAct = nullptr, *m_printAct = nullptr, *m_closeTabAct = nullptr,
            *m_rotateAct = nullptr, *m_deletePageAct = nullptr, *m_extractAct = nullptr,
            *m_insertAct = nullptr, *m_mergeAct = nullptr, *m_splitAct = nullptr,
            *m_flattenAct = nullptr, *m_highlightAct = nullptr, *m_noteAct = nullptr,
            *m_squareAct = nullptr, *m_selectToolAct = nullptr, *m_inkToolAct = nullptr,
            *m_toImagesAct = nullptr, *m_toTextAct = nullptr, *m_copyAct = nullptr;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ANGRA"));
    QApplication::setApplicationName(theme::kAppName);
    QApplication::setApplicationVersion(theme::kAppVersion);
    PdfDocument::initLibrary();
    int rc;
    {
        MainWindow window;
        window.show();
        const QStringList args = QApplication::arguments();
        if (args.size() > 1)
            window.openPath(args.at(1));
        rc = QApplication::exec();
    }
    PdfDocument::shutdownLibrary();
    return rc;
}
