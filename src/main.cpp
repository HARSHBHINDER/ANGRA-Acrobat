#include "PdfDocument.h"
#include "Theme.h"
#ifdef ANGRA_HAVE_QPDF
#include "PdfProtect.h"
#endif

#include <QAction>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QFileDialog>
#include <QFileInfo>
#include <QDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QPushButton>
#include <QRubberBand>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QAbstractButton>
#include <QHeaderView>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdio>
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
        m_canvas->setFocusPolicy(Qt::StrongFocus); // keyboard input for form fields

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
        m_doc.formKillFocus();
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
        m_baseImage = m_doc.renderPage(m_page, m_zoom * dpr);
        if (m_baseImage.isNull()) {
            m_canvas->setText(tr("Failed to render page %1.").arg(m_page + 1));
            m_canvas->adjustSize();
            changed();
            return;
        }
        m_objects = m_doc.pageObjects(m_page); // detect objects
        if (m_selected >= m_objects.size())
            m_selected = -1;
        paintOverlay();
    }

    // Overlay is composited onto a copy of the cached render, so selecting or
    // dragging never re-runs PDFium. Screen pixels only: nothing drawn here is
    // written into a content stream, so overlays cannot print.
    void paintOverlay() {
        if (m_baseImage.isNull())
            return;
        const qreal dpr = devicePixelRatioF();
        const double s = m_zoom * dpr;
        auto toDevice = [s](const QRectF& r) {
            return QRectF(r.left() * s, r.top() * s, r.width() * s, r.height() * s);
        };
        QImage frame = m_baseImage;
        {
            QPainter p(&frame);
            if (m_searchPageIdx == m_page && !m_searchRects.isEmpty()) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 235, 59, 110));
                for (const QRectF& r : m_searchRects)
                    p.drawRect(toDevice(r));
            }
            if (m_showBounds) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(120, 135, 155), 1.0, Qt::DotLine));
                for (const PdfDocument::PageObject& o : m_objects)
                    p.drawRect(toDevice(o.bounds));
            }
            if (m_selected >= 0 && m_selected < m_objects.size()) {
                const QRectF r = toDevice(m_objects.at(m_selected).bounds);
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(41, 211, 240), 2.0));
                p.drawRect(r);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(41, 211, 240));
                constexpr double kHandle = 3.0;
                for (const QPointF& c :
                     {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()})
                    p.drawRect(
                        QRectF(c.x() - kHandle, c.y() - kHandle, 2 * kHandle, 2 * kHandle));
            }
        }
        frame.setDevicePixelRatio(dpr);
        m_canvas->setPixmap(QPixmap::fromImage(std::move(frame)));
        m_canvas->adjustSize();
        changed();
    }

    // --- object mode: boundaries, selection, movement, undo ---
    bool showBounds() const { return m_showBounds; }
    void setShowBounds(bool on) {
        m_showBounds = on;
        if (!on)
            m_selected = -1;
        paintOverlay();
    }

    QString selectionSummary() const {
        if (m_selected < 0 || m_selected >= m_objects.size())
            return {};
        const PdfDocument::PageObject& o = m_objects.at(m_selected);
        const char* kind = "object";
        switch (o.kind) {
        case PdfDocument::ObjectKind::Text: kind = "text"; break;
        case PdfDocument::ObjectKind::Path: kind = "path"; break;
        case PdfDocument::ObjectKind::Image: kind = "image"; break;
        case PdfDocument::ObjectKind::Form: kind = "form"; break;
        case PdfDocument::ObjectKind::Shading: kind = "shading"; break;
        case PdfDocument::ObjectKind::Other: break;
        }
        QString text = QStringLiteral("%1 #%2 at (%3, %4)  %5 x %6 pt")
                           .arg(QString::fromLatin1(kind))
                           .arg(o.index)
                           .arg(o.bounds.left(), 0, 'f', 1)
                           .arg(o.bounds.top(), 0, 'f', 1)
                           .arg(o.bounds.width(), 0, 'f', 1)
                           .arg(o.bounds.height(), 0, 'f', 1);
        if (!o.text.trimmed().isEmpty())
            text += QStringLiteral("  \"%1\"").arg(o.text.simplified().left(40));
        return text;
    }

    // Repeated clicks on the same spot cycle through overlapping objects, so a
    // value sitting on top of its underline is still reachable.
    void selectAt(const QPointF& pagePt) {
        QList<int> hits;
        for (int i = m_objects.size() - 1; i >= 0; --i) // topmost first
            if (m_objects.at(i).bounds.adjusted(-2, -2, 2, 2).contains(pagePt))
                hits << i;
        if (hits.isEmpty()) {
            m_selected = -1;
            m_hits.clear();
        } else {
            if (hits != m_hits) {
                m_hits = hits;
                m_hitIndex = 0;
            } else {
                m_hitIndex = (m_hitIndex + 1) % m_hits.size();
            }
            m_selected = m_hits.at(m_hitIndex);
        }
        paintOverlay();
    }

    bool selectionInside(const QPointF& pagePt) const {
        return m_selected >= 0 && m_selected < m_objects.size() &&
               m_objects.at(m_selected).bounds.adjusted(-2, -2, 2, 2).contains(pagePt);
    }

    bool moveSelected(double dx, double dy) {
        if (m_selected < 0 || m_selected >= m_objects.size())
            return false;
        const int objIndex = m_objects.at(m_selected).index;
        if (!m_doc.moveObject(m_page, objIndex, dx, dy))
            return false;
        m_undo.append({m_page, objIndex, dx, dy});
        render(); // bounds moved, so the display list is stale
        return true;
    }

    bool undoMove() {
        if (m_undo.isEmpty())
            return false;
        const MoveCmd c = m_undo.takeLast();
        if (c.page != m_page)
            gotoPage(c.page);
        const bool ok = m_doc.moveObject(c.page, c.index, -c.dx, -c.dy);
        render();
        return ok;
    }
    bool canUndo() const { return !m_undo.isEmpty(); }

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
            // Qt6-safe page rect (QPrinter::pageRect(Unit) is deprecated)
            const QRectF target = printer.pageLayout().paintRectPixels(printer.resolution());
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
                } else if (m_showBounds && selectionInside(toPagePt(m_dragStart))) {
                    m_draggingObject = true; // drag the object, not a marquee
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
                if (m_draggingObject) {
                    m_draggingObject = false;
                    const QPointF delta = toPagePt(end) - toPagePt(m_dragStart);
                    if (!delta.isNull())
                        moveSelected(delta.x(), delta.y()); // one undo entry per drag
                    return true;
                }
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
        case QEvent::KeyPress: {
            const auto* key = static_cast<QKeyEvent*>(ev);
            if (m_showBounds && m_selected >= 0) {
                // Shift = coarse nudge. Page space is points, so 1pt is 1/72in.
                const double step = (key->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
                switch (key->key()) {
                case Qt::Key_Left: moveSelected(-step, 0); return true;
                case Qt::Key_Right: moveSelected(step, 0); return true;
                case Qt::Key_Up: moveSelected(0, -step); return true;
                case Qt::Key_Down: moveSelected(0, step); return true;
                default: break;
                }
            }
            if (m_doc.hasForms()) {
                const auto* ke = static_cast<QKeyEvent*>(ev);
                if (ke->key() == Qt::Key_Backspace) {
                    m_doc.formChar(m_page, 8);
                    render();
                    return true;
                }
                if (!ke->text().isEmpty() && ke->text().at(0).isPrint()) {
                    m_doc.formChar(m_page, ke->text().at(0).unicode());
                    render();
                    return true;
                }
            }
            break;
        }
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
        if (m_showBounds) { // object mode owns the click; links and forms do not
            selectAt(pagePt);
            return;
        }
        if (m_doc.hasForms()) { // toggle checkboxes, focus text fields
            m_doc.formClick(m_page, pagePt);
            render();
        }
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

    // --- object mode ---
    struct MoveCmd {
        int page;
        int index;
        double dx;
        double dy;
    };
    QImage m_baseImage; // last PDFium render; overlays composite onto a copy
    QList<PdfDocument::PageObject> m_objects;
    QList<int> m_hits; // objects under the last click, topmost first
    int m_hitIndex = 0;
    int m_selected = -1;
    bool m_showBounds = false;
    bool m_draggingObject = false;
    // ponytail: move-only undo by inverse translate. The full command journal
    // arrives with text and path editing; this covers all that is undoable now.
    QList<MoveCmd> m_undo;
};

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(theme::kAppName);
        resize(1200, 850);

        setAcceptDrops(true);

        m_tabs = new QTabWidget;
        m_tabs->setTabsClosable(true);
        m_tabs->setMovable(true);
        m_tabs->setDocumentMode(true);
        // Stack, not the tab widget directly: an empty QTabWidget is a grey
        // void, so index 0 holds a start screen and index 1 the documents.
        m_stack = new QStackedWidget;
        m_stack->addWidget(buildStartScreen());
        m_stack->addWidget(m_tabs);
        setCentralWidget(m_stack);
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
        syncCentral();
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
        syncCentral();
        refreshPanels();
        statusBar()->showMessage(tr("Opened %1 (%2 pages)").arg(name).arg(t->doc().pageCount()));
    }

protected:
    void dragEnterEvent(QDragEnterEvent* ev) override {
        if (ev->mimeData()->hasUrls())
            ev->acceptProposedAction();
    }

    void dropEvent(QDropEvent* ev) override {
        for (const QUrl& url : ev->mimeData()->urls()) {
            const QString path = url.toLocalFile();
            if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
                openPath(path);
        }
    }

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

        // Object mode: boundaries + selection + movement. Ctrl+B gates it so a
        // plain click keeps following links and filling form fields.
        m_boundsAct = new QAction(tr("Show Object &Boundaries"), this);
        m_boundsAct->setCheckable(true);
        m_boundsAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
        QObject::connect(m_boundsAct, &QAction::toggled, this, [this](bool on) {
            if (auto* t = tab())
                t->setShowBounds(on);
            statusBar()->showMessage(on ? tr("Object mode: click to select, drag or "
                                             "arrow keys to move, Ctrl+Z to undo")
                                       : tr("Object mode off"));
        });
        m_undoMoveAct = new QAction(tr("&Undo Move"), this);
        m_undoMoveAct->setShortcut(QKeySequence::Undo);
        QObject::connect(m_undoMoveAct, &QAction::triggered, this, [this] {
            if (auto* t = tab())
                statusBar()->showMessage(t->undoMove() ? tr("Move undone")
                                                       : tr("Nothing to undo"));
        });

        // View
        QMenu* view = menuBar()->addMenu(tr("&View"));
        view->addAction(m_boundsAct);
        view->addSeparator();
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
        m_cropAct = docm->addAction(tr("&Crop Page To Selection"), [this] {
            if (auto* t = tab(); t && !t->selectionRects().isEmpty() &&
                                 t->doc().cropPage(t->page(), t->selectionRects().first()))
                t->afterStructureChange();
            else
                statusBar()->showMessage(tr("Select an area to crop to first"));
        });
        m_moveAct = docm->addAction(tr("&Rearrange: Move Page To..."), [this] { movePageTo(); });
        m_pageNumAct = docm->addAction(tr("Add Page &Numbers"), [this] {
            if (auto* t = tab(); t && t->doc().addPageNumbers()) {
                t->render();
                statusBar()->showMessage(tr("Page numbers added"));
            }
        });
        m_watermarkAct = docm->addAction(tr("Add Text &Watermark..."), [this] {
            if (auto* t = tab()) {
                bool ok = false;
                const QString text = QInputDialog::getText(this, tr("Watermark"),
                                                           tr("Watermark text:"),
                                                           QLineEdit::Normal, {}, &ok);
                if (ok && !text.isEmpty() && t->doc().addTextWatermark(text))
                    t->render();
            }
        });
        m_extractImgAct = docm->addAction(tr("Extract &Images From Page..."),
                                          [this] { extractPageImages(); });
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
        m_addTextAct = comment->addAction(tr("Add &Text At Last Click..."), [this] {
            if (auto* t = tab()) {
                bool ok = false;
                const QString text = QInputDialog::getText(this, tr("Add Text"),
                                                           tr("Text:"), QLineEdit::Normal,
                                                           {}, &ok);
                if (ok && !text.isEmpty() &&
                    t->doc().addTextAt(t->page(), t->lastClickPagePt(), text))
                    t->render();
            }
        });
        m_scanAct = comment->addAction(tr("&Scan Text On Page..."),
                                       QKeySequence(Qt::CTRL | Qt::Key_T),
                                       [this] { scanTextRuns(); });
        m_editTextAct = comment->addAction(tr("&Edit Text At Last Click..."),
                                           [this] { editTextAtClick(); });
        m_editBoxAct = comment->addAction(tr("Edit Text &Box (reflow selection)..."),
                                          [this] { editTextBox(); });
        m_signAct = comment->addAction(tr("Place &Signature Image At Last Click..."),
                                       [this] { placeSignature(); });
        comment->addSeparator();
        QAction* selectToolAct = comment->addAction(tr("&Select Tool"));
        m_inkToolAct = comment->addAction(tr("&Draw Ink Tool"));
        selectToolAct->setCheckable(true);
        m_inkToolAct->setCheckable(true);
        selectToolAct->setChecked(true);
        auto* group = new QActionGroup(this);
        group->addAction(selectToolAct);
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
        m_toImagesAct = convert->addAction(tr("PDF to P&NG..."),
                                           [this] { pdfToImages(QStringLiteral("png")); });
        m_toJpgAct = convert->addAction(tr("PDF to &JPG..."),
                                        [this] { pdfToImages(QStringLiteral("jpg")); });
        m_toTextAct = convert->addAction(tr("PDF to Te&xt..."), [this] { pdfToText(); });

        // Protect
        QMenu* protect = menuBar()->addMenu(tr("&Protect"));
        m_redactAct = protect->addAction(tr("&Redact Selection (rasterize page)"),
                                         [this] { redactSelection(); });
#ifdef ANGRA_HAVE_QPDF
        m_encryptAct = protect->addAction(tr("&Encrypt Copy..."), [this] { encryptCopy(); });
        m_decryptAct = protect->addAction(tr("Remove Encr&yption Copy..."),
                                          [this] { qpdfCopy(Op::Decrypt); });
        m_sanitizeAct = protect->addAction(tr("&Sanitized Copy (strip metadata)..."),
                                           [this] { qpdfCopy(Op::Sanitize); });
#endif

        // Tools
        QMenu* tools = menuBar()->addMenu(tr("&Tools"));
        m_compareAct = tools->addAction(tr("&Compare Text With Other Tab..."),
                                        [this] { compareTabs(); });
#ifdef ANGRA_HAVE_QPDF
        m_optimizeAct = tools->addAction(tr("&Optimized Copy..."),
                                         [this] { qpdfCopy(Op::Optimize); });
        m_repairAct = tools->addAction(tr("&Repaired Copy..."),
                                       [this] { qpdfCopy(Op::Repair); });
#endif

        // Share (all local OS integrations; the app itself makes no connections)
        QMenu* share = menuBar()->addMenu(tr("&Share"));
        m_copyFileAct = share->addAction(tr("Copy &File to Clipboard"), [this] {
            if (auto* t = tab()) {
                auto* mime = new QMimeData;
                mime->setUrls({QUrl::fromLocalFile(t->doc().filePath())});
                QApplication::clipboard()->setMimeData(mime);
                statusBar()->showMessage(tr("File copied; paste into mail or chat"));
            }
        });
        m_copyPathAct = share->addAction(tr("Copy File &Path"), [this] {
            if (auto* t = tab())
                QApplication::clipboard()->setText(
                    QDir::toNativeSeparators(t->doc().filePath()));
        });
        m_revealAct = share->addAction(tr("Show in &Explorer"), [this] {
            if (auto* t = tab())
                QProcess::startDetached(
                    QStringLiteral("explorer"),
                    {QStringLiteral("/select,"),
                     QDir::toNativeSeparators(t->doc().filePath())});
        });
        m_emailAct = share->addAction(tr("Send by &Email..."), [this] {
            auto* t = tab();
            if (!t)
                return;
            const QString path = QDir::toNativeSeparators(t->doc().filePath());
            // mailto: carries no attachment on Windows, so put the file on the
            // clipboard and tell the user to paste it into the draft.
            auto* mime = new QMimeData;
            mime->setUrls({QUrl::fromLocalFile(t->doc().filePath())});
            QApplication::clipboard()->setMimeData(mime);
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("subject"), QFileInfo(path).fileName());
            q.addQueryItem(QStringLiteral("body"),
                           tr("Sending: %1\n\nThe file is on your clipboard - press "
                              "Ctrl+V in the message to attach it.")
                               .arg(path));
            QUrl url(QStringLiteral("mailto:"));
            url.setQuery(q);
            QDesktopServices::openUrl(url);
            statusBar()->showMessage(tr("Draft opened; press Ctrl+V to attach"));
        });

        // Help
        QMenu* help = menuBar()->addMenu(tr("&Help"));
        // The only network call in the app, and only when the user asks: opens
        // the release page in the default browser. Nothing is sent or fetched
        // in-process; there is no telemetry and no background check.
        help->addAction(tr("Check for &Updates"), [this] {
            QDesktopServices::openUrl(QUrl(QStringLiteral(
                "https://github.com/HARSHBHINDER/ANGRA-Acrobat/releases/latest")));
        });
        help->addSeparator();
        help->addAction(tr("&About %1").arg(theme::kAppName), [this] {
            QMessageBox::about(
                this, tr("About %1").arg(theme::kAppName),
                tr("<b>%1</b> %2<br>An offline-first PDF workstation for Windows.<br>"
                   "Personal, non-commercial use only - see LICENSE. Uses Qt, "
                   "PDFium and qpdf; see THIRD_PARTY_NOTICES.md.")
                    .arg(theme::kAppName, theme::kAppVersion));
        });

        buildToolPanel(view); // last: the rail reuses every action created above
    }

    // Text scanner: every run on the page in one editable list. This is the
    // discoverable path to text editing - clicking a 10pt glyph box to find a
    // run is a pixel hunt, and a miss looks like the feature is dead.
    void scanTextRuns() {
        auto* t = tab();
        if (!t)
            return;
        const QList<PdfDocument::TextRun> runs = t->doc().textRuns(t->page());
        if (runs.isEmpty()) {
            QMessageBox::information(
                this, tr("Scan Text"),
                tr("No editable text runs on page %1.\n\nA scanned page holds a "
                   "picture of text rather than text objects. Reading those needs "
                   "OCR, which this build does not do.")
                    .arg(t->page() + 1));
            return;
        }

        QDialog dlg(this);
        dlg.setWindowTitle(tr("Scan Text - page %1, %2 runs")
                               .arg(t->page() + 1)
                               .arg(runs.size()));
        dlg.resize(760, 540);
        auto* col = new QVBoxLayout(&dlg);

        auto* table = new QTableWidget(runs.size(), 2, &dlg);
        table->setHorizontalHeaderLabels({tr("Text on page"), tr("Replace with")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->verticalHeader()->setVisible(false);
        for (int row = 0; row < runs.size(); ++row) {
            auto* found = new QTableWidgetItem(runs.at(row).text);
            found->setFlags(Qt::ItemIsEnabled); // reference column, not editable
            table->setItem(row, 0, found);
            table->setItem(row, 1, new QTableWidgetItem(runs.at(row).text));
        }
        col->addWidget(table);

        auto* findEdit = new QLineEdit;
        findEdit->setPlaceholderText(tr("Find across every run"));
        auto* replEdit = new QLineEdit;
        replEdit->setPlaceholderText(tr("Replace with"));
        auto* batchBtn = new QPushButton(tr("Replace All"));
        auto* batchRow = new QHBoxLayout;
        batchRow->addWidget(findEdit);
        batchRow->addWidget(replEdit);
        batchRow->addWidget(batchBtn);
        col->addLayout(batchRow);

        auto* status = new QLabel(tr("Edit any row, or use Find/Replace All. "
                                     "Clearing a row deletes that run."));
        col->addWidget(status);

        // Batch only stages text in the table; nothing touches the PDF until
        // Apply, so a bad replace is undone by cancelling the dialog.
        QObject::connect(batchBtn, &QPushButton::clicked, &dlg, [&] {
            const QString from = findEdit->text();
            if (from.isEmpty())
                return;
            int hits = 0;
            for (int row = 0; row < table->rowCount(); ++row) {
                const QString before = table->item(row, 1)->text();
                QString after = before;
                after.replace(from, replEdit->text());
                if (after != before) {
                    table->item(row, 1)->setText(after);
                    ++hits;
                }
            }
            status->setText(tr("Staged in %1 row(s). Apply to write them.").arg(hits));
        });

        auto* buttons =
            new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Apply)->setText(tr("Apply to PDF"));
        QObject::connect(buttons, &QDialogButtonBox::clicked, &dlg,
                         [&](QAbstractButton* b) {
                             if (buttons->standardButton(b) == QDialogButtonBox::Apply)
                                 dlg.accept();
                             else
                                 dlg.reject();
                         });
        col->addWidget(buttons);

        if (dlg.exec() != QDialog::Accepted)
            return;

        // Highest index first: clearing a run deletes the page object, which
        // shifts every later index down and would corrupt the remaining edits.
        int changed = 0;
        for (int row = runs.size() - 1; row >= 0; --row) {
            const QString edited = table->item(row, 1)->text();
            if (edited == runs.at(row).text)
                continue;
            if (t->doc().setTextObject(t->page(), runs.at(row).index, edited))
                ++changed;
        }
        if (changed) {
            t->render();
            statusBar()->showMessage(tr("Updated %1 text run(s)").arg(changed));
        } else {
            statusBar()->showMessage(tr("No changes applied"));
        }
    }

    // Start screen. Recents come from the same QSettings list the File menu
    // already maintains, so there is no second store to keep in sync.
    QWidget* buildStartScreen() {
        auto* page = new QWidget;
        page->setObjectName(QStringLiteral("startScreen"));
        auto* col = new QVBoxLayout(page);
        col->setAlignment(Qt::AlignCenter);
        col->setSpacing(0);

        auto label = [&](const QString& text, const char* id) {
            auto* l = new QLabel(text);
            l->setObjectName(QString::fromLatin1(id));
            l->setAlignment(Qt::AlignCenter);
            return l;
        };

        auto* open = new QPushButton(tr("Open a PDF"));
        open->setMinimumWidth(210);
        open->setCursor(Qt::PointingHandCursor);
        QObject::connect(open, &QPushButton::clicked, open, [this] { openDialog(); });

        m_recentList = new QListWidget;
        m_recentList->setObjectName(QStringLiteral("startRecents"));
        m_recentList->setFixedWidth(370);
        m_recentList->setMaximumHeight(184);
        QObject::connect(m_recentList, &QListWidget::itemClicked, m_recentList,
                         [this](QListWidgetItem* item) {
                             openPath(item->data(Qt::UserRole).toString());
                         });

        auto* mark = new QLabel;
        mark->setAlignment(Qt::AlignCenter);
        mark->setPixmap(QIcon(QString::fromUtf8(theme::kIconResource))
                            .pixmap(QSize(theme::kStartLogoPx, theme::kStartLogoPx)));
        col->addWidget(mark, 0, Qt::AlignCenter);
        col->addSpacing(18);
        col->addWidget(label(QString::fromUtf8(theme::kAppName), "startTitle"));
        col->addSpacing(7);
        col->addWidget(label(tr("Offline PDF workstation - nothing leaves this machine"),
                             "startSubtitle"));
        col->addSpacing(27);
        col->addWidget(open, 0, Qt::AlignCenter);
        col->addSpacing(11);
        col->addWidget(label(tr("or drop a PDF anywhere in this window"), "startHint"));
        col->addSpacing(30);
        col->addWidget(m_recentList, 0, Qt::AlignCenter);
        return page;
    }

    // Start screen when there is nothing open, documents otherwise.
    void syncCentral() {
        const bool empty = m_tabs->count() == 0;
        if (empty) {
            m_recentList->clear();
            for (const QString& path :
                 QSettings().value("recentFiles").toStringList()) {
                if (!QFileInfo::exists(path)) // a stale entry is worse than none
                    continue;
                auto* item = new QListWidgetItem(QFileInfo(path).fileName());
                item->setToolTip(QDir::toNativeSeparators(path));
                item->setData(Qt::UserRole, path);
                m_recentList->addItem(item);
            }
            m_recentList->setVisible(m_recentList->count() > 0);
        }
        m_stack->setCurrentIndex(empty ? 0 : 1);
    }

    // Left tool rail. Every action here already exists in the menus - this is
    // discovery, not new behaviour. QToolButton mirrors its default action, so
    // updateUi() enables the whole panel for free with no extra wiring.
    void buildToolPanel(QMenu* view) {
        auto* panel = new QWidget;
        panel->setObjectName(QStringLiteral("toolPanel"));
        auto* col = new QVBoxLayout(panel);
        col->setContentsMargins(9, 2, 9, 14);
        col->setSpacing(1);

        auto section = [&](const QString& title) {
            auto* label = new QLabel(title);
            label->setObjectName(QStringLiteral("toolSection"));
            col->addWidget(label);
        };
        // iconText (not text) is what QToolButton renders, and setting it here
        // keeps the long, mnemonic-bearing menu text intact.
        auto entry = [&](QAction* action, const QString& label) {
            action->setIconText(label);
            auto* button = new QToolButton;
            button->setObjectName(QStringLiteral("toolButton"));
            button->setDefaultAction(action);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            button->setCursor(Qt::PointingHandCursor);
            col->addWidget(button);
        };

        section(tr("MODIFY PAGE"));
        entry(m_rotateAct, tr("Rotate page"));
        entry(m_insertAct, tr("Insert pages"));
        entry(m_deletePageAct, tr("Delete page"));
        entry(m_extractAct, tr("Extract page"));
        entry(m_moveAct, tr("Organize pages"));
        entry(m_cropAct, tr("Crop to selection"));

        section(tr("ADD CONTENT"));
        entry(m_addTextAct, tr("Text"));
        entry(m_signAct, tr("Image / signature"));
        entry(m_pageNumAct, tr("Header and footer"));
        entry(m_watermarkAct, tr("Watermark"));

        section(tr("OBJECTS"));
        entry(m_boundsAct, tr("Show boundaries"));
        entry(m_undoMoveAct, tr("Undo move"));

        section(tr("EDIT TEXT"));
        entry(m_scanAct, tr("Scan text on page"));
        entry(m_editTextAct, tr("Edit text at click"));
        entry(m_editBoxAct, tr("Reflow text box"));

        section(tr("COMMENT"));
        entry(m_highlightAct, tr("Highlight"));
        entry(m_noteAct, tr("Sticky note"));
        entry(m_squareAct, tr("Rectangle"));
        entry(m_inkToolAct, tr("Draw"));

        section(tr("ORGANIZE"));
        entry(m_mergeAct, tr("Combine files"));
        entry(m_splitAct, tr("Split into pages"));
        entry(m_extractImgAct, tr("Extract images"));
        entry(m_flattenAct, tr("Flatten annotations"));

        section(tr("CONVERT"));
        entry(m_toImagesAct, tr("PDF to PNG"));
        entry(m_toJpgAct, tr("PDF to JPG"));
        entry(m_toTextAct, tr("PDF to text"));

        section(tr("PROTECT"));
        entry(m_redactAct, tr("Redact a PDF"));
#ifdef ANGRA_HAVE_QPDF
        entry(m_encryptAct, tr("Password protect"));
        entry(m_decryptAct, tr("Remove password"));
        entry(m_sanitizeAct, tr("Strip metadata"));
#endif

        section(tr("SHARE"));
        entry(m_copyFileAct, tr("Copy file"));
        entry(m_emailAct, tr("Send by email"));
        entry(m_revealAct, tr("Show in Explorer"));
        col->addStretch(1);

        auto* scroll = new QScrollArea;
        scroll->setWidget(panel);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        auto* dock = new QDockWidget(tr("Tools"), this);
        dock->setObjectName(QStringLiteral("toolsDock"));
        dock->setWidget(scroll);
        dock->setMinimumWidth(206);
        addDockWidget(Qt::LeftDockWidgetArea, dock);
        view->addAction(dock->toggleViewAction());
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
        syncCentral();
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
            out.importPages(t->doc(), 0, QByteArray::number(t->page() + 1)) &&
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
        if (t->doc().importPages(src, t->page() + 1))
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
                !t->doc().importPages(src, t->doc().pageCount())) {
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
            if (out.createEmpty() && out.importPages(t->doc(), 0, QByteArray::number(i + 1)) &&
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

    void pdfToImages(const QString& ext) {
        auto* t = tab();
        if (!t)
            return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Export Images To"));
        if (dir.isEmpty())
            return;
        int okCount = 0;
        for (int i = 0; i < t->doc().pageCount(); ++i) {
            QImage img = t->doc().renderPage(i, 2.0); // 144 dpi
            if (ext == QStringLiteral("jpg")) // JPG has no alpha; flatten onto white
                img = img.convertToFormat(QImage::Format_RGB32);
            const QString dest = QDir(dir).filePath(
                QStringLiteral("page-%1.%2").arg(i + 1, 3, 10, QLatin1Char('0')).arg(ext));
            if (!img.isNull() && img.save(dest))
                ++okCount;
        }
        statusBar()->showMessage(tr("Exported %1 images").arg(okCount));
    }

    void movePageTo() {
        auto* t = tab();
        if (!t || t->doc().pageCount() < 2)
            return;
        bool ok = false;
        const int to = QInputDialog::getInt(this, tr("Move Page"),
                                            tr("Move page %1 to position:").arg(t->page() + 1),
                                            t->page() + 1, 1, t->doc().pageCount(), 1, &ok);
        if (ok && t->doc().movePage(t->page(), to - 1))
            t->afterStructureChange();
    }

    void editTextAtClick() {
        auto* t = tab();
        if (!t)
            return;
        int idx = -1;
        const QString current = t->doc().textObjectAt(t->page(), t->lastClickPagePt(), &idx);
        if (idx < 0) {
            statusBar()->showMessage(
                tr("Click a text run first (select tool), then Edit Text"));
            return;
        }

        QDialog dlg(this);
        dlg.setWindowTitle(tr("Edit Text"));
        auto* form = new QFormLayout(&dlg);

        auto* textEdit = new QLineEdit(current);
        form->addRow(tr("Text (clear = delete run):"), textEdit);

        auto* family = new QComboBox;
        family->addItems({QStringLiteral("Helvetica"), QStringLiteral("Times"),
                          QStringLiteral("Courier")});
        form->addRow(tr("Font:"), family);

        auto* size = new QSpinBox;
        size->setRange(4, 400);
        size->setValue(12);
        form->addRow(tr("Size:"), size);

        auto* bold = new QCheckBox(tr("Bold"));
        auto* italic = new QCheckBox(tr("Italic"));
        auto* underline = new QCheckBox(tr("Underline"));
        auto* styleRow = new QHBoxLayout;
        styleRow->addWidget(bold);
        styleRow->addWidget(italic);
        styleRow->addWidget(underline);
        form->addRow(tr("Style:"), styleRow);

        QColor chosen = Qt::black;
        auto* colorBtn = new QPushButton(tr("Text Color..."));
        QObject::connect(colorBtn, &QPushButton::clicked, &dlg, [&] {
            const QColor c = QColorDialog::getColor(chosen, &dlg, tr("Text Color"));
            if (c.isValid())
                chosen = c;
        });
        form->addRow(QString(), colorBtn);

        QByteArray ttf;
        auto* fontFileBtn = new QPushButton(tr("Load Custom .ttf..."));
        QObject::connect(fontFileBtn, &QPushButton::clicked, &dlg, [&] {
            const QString p = QFileDialog::getOpenFileName(&dlg, tr("TrueType Font"), {},
                                                           tr("Fonts (*.ttf)"));
            if (p.isEmpty())
                return;
            QFile f(p);
            if (f.open(QIODevice::ReadOnly)) {
                ttf = f.readAll();
                fontFileBtn->setText(QFileInfo(p).fileName());
                family->setEnabled(false); // custom font wins over base-14
            }
        });
        form->addRow(QString(), fontFileBtn);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        form->addRow(buttons);

        if (dlg.exec() != QDialog::Accepted)
            return;

        const QString edited = textEdit->text();
        if (edited.isEmpty()) { // delete
            if (t->doc().setTextObject(t->page(), idx, {}))
                t->render();
            return;
        }
        // Unstyled edit (same font, black, no marks) -> cheap in-place path.
        const bool styled = bold->isChecked() || italic->isChecked() ||
                            underline->isChecked() || chosen != QColor(Qt::black) ||
                            !ttf.isEmpty() || size->value() != 12 ||
                            family->currentText() != QStringLiteral("Helvetica");
        bool okDone;
        if (!styled) {
            okDone = t->doc().setTextObject(t->page(), idx, edited);
        } else {
            PdfDocument::TextStyle s;
            s.family = family->currentText();
            s.size = size->value();
            s.color = chosen;
            s.bold = bold->isChecked();
            s.italic = italic->isChecked();
            s.underline = underline->isChecked();
            okDone = t->doc().styleTextObject(t->page(), idx, edited, s, ttf);
        }
        if (okDone)
            t->render();
        else
            QMessageBox::warning(this, theme::kAppName, tr("Could not apply the edit."));
    }

    void editTextBox() {
        auto* t = tab();
        if (!t || t->selectionRects().isEmpty()) {
            statusBar()->showMessage(
                tr("Drag-select a paragraph region first, then Edit Text Box"));
            return;
        }
        const QRectF rect = t->selectionRects().first();
        const QString current = t->doc().textInRect(t->page(), rect);

        QDialog dlg(this);
        dlg.setWindowTitle(tr("Edit Text Box (reflow)"));
        auto* form = new QFormLayout(&dlg);
        auto* editor = new QPlainTextEdit(current);
        editor->setMinimumSize(420, 220);
        form->addRow(editor);
        auto* family = new QComboBox;
        family->addItems({QStringLiteral("Helvetica"), QStringLiteral("Times"),
                          QStringLiteral("Courier")});
        form->addRow(tr("Font:"), family);
        auto* size = new QSpinBox;
        size->setRange(4, 200);
        size->setValue(11);
        form->addRow(tr("Size:"), size);
        auto* bold = new QCheckBox(tr("Bold"));
        auto* italic = new QCheckBox(tr("Italic"));
        auto* row = new QHBoxLayout;
        row->addWidget(bold);
        row->addWidget(italic);
        form->addRow(tr("Style:"), row);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        form->addRow(buttons);
        if (dlg.exec() != QDialog::Accepted)
            return;

        PdfDocument::TextStyle s;
        s.family = family->currentText();
        s.size = size->value();
        s.bold = bold->isChecked();
        s.italic = italic->isChecked();
        if (t->doc().reflowRegion(t->page(), rect, editor->toPlainText(), s))
            t->render();
        else
            QMessageBox::warning(this, theme::kAppName, tr("Reflow failed."));
    }

    void placeSignature() {
        auto* t = tab();
        if (!t)
            return;
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Signature Image"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp)"));
        if (path.isEmpty())
            return;
        const QImage sig(path);
        if (sig.isNull()) {
            QMessageBox::warning(this, theme::kAppName, tr("Cannot read image."));
            return;
        }
        if (t->doc().placeImage(t->page(), sig, t->lastClickPagePt(), 144.0)) // ~2 inch wide
            t->render();
    }

    void extractPageImages() {
        auto* t = tab();
        if (!t)
            return;
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Extract Images To"));
        if (dir.isEmpty())
            return;
        const int n = t->doc().extractImages(t->page(), dir,
                                             QStringLiteral("page%1-img").arg(t->page() + 1));
        statusBar()->showMessage(n ? tr("Extracted %1 images").arg(n)
                                   : tr("No embedded images on this page"));
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

    void redactSelection() {
        auto* t = tab();
        if (!t || t->selectionRects().isEmpty()) {
            statusBar()->showMessage(tr("Select an area to redact first"));
            return;
        }
        if (QMessageBox::warning(
                this, tr("Redact"),
                tr("The whole page is replaced by an image with the selected area "
                   "blacked out. Text, vectors, and form fields on this page are "
                   "permanently destroyed. Continue?"),
                QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
            return;
        if (t->doc().redactRasterize(t->page(), t->selectionRects())) {
            t->afterStructureChange();
            statusBar()->showMessage(
                tr("Page %1 redacted. Save a copy to persist it.").arg(t->page() + 1));
        }
    }

    void compareTabs() {
        auto* t = tab();
        if (!t || m_tabs->count() < 2) {
            statusBar()->showMessage(tr("Open the second document in another tab first"));
            return;
        }
        QStringList names;
        QList<DocumentTab*> others;
        for (int i = 0; i < m_tabs->count(); ++i) {
            if (m_tabs->widget(i) != t) {
                names << m_tabs->tabText(i);
                others << tabAt(i);
            }
        }
        bool ok = false;
        const QString pick = QInputDialog::getItem(this, tr("Compare"),
                                                   tr("Compare against:"), names, 0,
                                                   false, &ok);
        if (!ok)
            return;
        DocumentTab* other = others.at(names.indexOf(pick));
        const int pages =
            std::max(t->doc().pageCount(), other->doc().pageCount());
        QStringList report;
        for (int i = 0; i < pages; ++i) {
            const QStringList a = t->doc().pageText(i).split(QLatin1Char('\n'));
            const QStringList b = other->doc().pageText(i).split(QLatin1Char('\n'));
            if (a == b)
                continue;
            report << tr("--- Page %1 differs ---").arg(i + 1);
            // ponytail: line-set diff, not LCS; upgrade when someone needs ordering
            for (const QString& line : a)
                if (!b.contains(line) && !line.trimmed().isEmpty())
                    report << QStringLiteral("< %1").arg(line);
            for (const QString& line : b)
                if (!a.contains(line) && !line.trimmed().isEmpty())
                    report << QStringLiteral("> %1").arg(line);
        }
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle(tr("Text Comparison"));
        dlg->resize(700, 500);
        auto* lay = new QHBoxLayout(dlg);
        auto* view = new QPlainTextEdit(
            report.isEmpty() ? tr("No text differences found.") : report.join('\n'));
        view->setReadOnly(true);
        lay->addWidget(view);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    }

#ifdef ANGRA_HAVE_QPDF
    enum class Op { Decrypt, Sanitize, Optimize, Repair };

    // Writes qpdf output only after PDFium confirms it reopens cleanly.
    void writeValidatedCopy(const QByteArray& bytes, const QString& loadPw,
                            const QString& doneMsg) {
        const QString dest = QFileDialog::getSaveFileName(this, tr("Save Copy As"), {},
                                                          tr("PDF files (*.pdf)"));
        if (dest.isEmpty())
            return;
        QSaveFile out(dest);
        if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() ||
            !out.commit()) {
            QMessageBox::warning(this, theme::kAppName, tr("Writing the file failed."));
            return;
        }
        PdfDocument check;
        if (check.load(dest, loadPw) != PdfDocument::Status::Ok &&
            check.load(dest) != PdfDocument::Status::Ok) {
            QFile::remove(dest);
            QMessageBox::warning(this, theme::kAppName,
                                 tr("Validation failed; the copy was removed."));
            return;
        }
        statusBar()->showMessage(doneMsg);
    }

    void encryptCopy() {
        auto* t = tab();
        if (!t)
            return;
        bool ok = false;
        const QString userPw = QInputDialog::getText(
            this, tr("Encrypt Copy"), tr("Open password (required):"),
            QLineEdit::Password, {}, &ok);
        if (!ok || userPw.isEmpty())
            return;
        const QString ownerPw = QInputDialog::getText(
            this, tr("Encrypt Copy"),
            tr("Permission password (optional, Enter to reuse open password):"),
            QLineEdit::Password, {}, &ok);
        if (!ok)
            return;
        QByteArray bytes;
        QString err;
        if (!pdfprotect::encrypt(t->doc().filePath(),
                                 QString::fromUtf8(t->doc().password()), userPw, ownerPw,
                                 &bytes, &err)) {
            QMessageBox::warning(this, theme::kAppName, tr("Encryption failed: %1").arg(err));
            return;
        }
        writeValidatedCopy(bytes, userPw, tr("Encrypted copy saved (AES-256)"));
    }

    void qpdfCopy(Op op) {
        auto* t = tab();
        if (!t)
            return;
        const QString src = t->doc().filePath();
        const QString pw = QString::fromUtf8(t->doc().password());
        QByteArray bytes;
        QString err;
        int warnings = 0;
        bool okRun = false;
        QString doneMsg;
        switch (op) {
        case Op::Decrypt:
            okRun = pdfprotect::decrypt(src, pw, &bytes, &err);
            doneMsg = tr("Decrypted copy saved");
            break;
        case Op::Sanitize:
            okRun = pdfprotect::sanitize(src, pw, &bytes, &err);
            doneMsg = tr("Sanitized copy saved (metadata and revision history removed)");
            break;
        case Op::Optimize:
            okRun = pdfprotect::optimize(src, pw, &bytes, &err);
            doneMsg = tr("Optimized copy saved");
            break;
        case Op::Repair:
            okRun = pdfprotect::repair(src, pw, &bytes, &err, &warnings);
            doneMsg = tr("Repaired copy saved (%1 issues recovered)").arg(warnings);
            break;
        }
        if (!okRun) {
            QMessageBox::warning(this, theme::kAppName, tr("Operation failed: %1").arg(err));
            return;
        }
        writeValidatedCopy(bytes, {}, doneMsg);
    }
#endif

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
              m_toImagesAct, m_toJpgAct, m_toTextAct, m_copyAct, m_redactAct, m_compareAct,
              m_copyFileAct, m_copyPathAct, m_revealAct, m_cropAct, m_moveAct,
              m_pageNumAct, m_watermarkAct, m_extractImgAct, m_addTextAct, m_signAct,
              m_editTextAct, m_editBoxAct, m_emailAct, m_scanAct, m_boundsAct,
              m_undoMoveAct})
            a->setEnabled(loaded);
#ifdef ANGRA_HAVE_QPDF
        for (QAction* a : {m_encryptAct, m_decryptAct, m_sanitizeAct, m_optimizeAct,
                           m_repairAct})
            a->setEnabled(loaded);
#endif
        if (loaded) {
            m_prevAct->setEnabled(t->page() > 0);
            m_nextAct->setEnabled(t->page() + 1 < t->doc().pageCount());
            m_deletePageAct->setEnabled(t->doc().pageCount() > 1);
            m_copyAct->setEnabled(!t->selectionText().isEmpty());
            m_highlightAct->setEnabled(!t->selectionRects().isEmpty());
            m_squareAct->setEnabled(!t->selectionRects().isEmpty());
            m_undoMoveAct->setEnabled(t->canUndo());
            // Blocked: setChecked would fire toggled -> setShowBounds -> repaint
            // -> updateUi, bouncing back through here on every tab switch.
            {
                const QSignalBlocker block(m_boundsAct);
                m_boundsAct->setChecked(t->showBounds());
            }
            const QString sel = t->selectionSummary();
            m_pageLabel->setText(
                sel.isEmpty()
                    ? tr("Page %1 of %2").arg(t->page() + 1).arg(t->doc().pageCount())
                    : sel);
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
    QStackedWidget* m_stack = nullptr;
    QListWidget* m_recentList = nullptr;
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
            *m_squareAct = nullptr, *m_inkToolAct = nullptr,
            *m_toImagesAct = nullptr, *m_toTextAct = nullptr, *m_copyAct = nullptr,
            *m_redactAct = nullptr, *m_compareAct = nullptr, *m_copyFileAct = nullptr,
            *m_copyPathAct = nullptr, *m_revealAct = nullptr, *m_toJpgAct = nullptr,
            *m_cropAct = nullptr, *m_moveAct = nullptr, *m_pageNumAct = nullptr,
            *m_watermarkAct = nullptr, *m_extractImgAct = nullptr, *m_addTextAct = nullptr,
            *m_signAct = nullptr, *m_editTextAct = nullptr, *m_editBoxAct = nullptr,
            *m_emailAct = nullptr, *m_scanAct = nullptr, *m_boundsAct = nullptr,
            *m_undoMoveAct = nullptr;
#ifdef ANGRA_HAVE_QPDF
    QAction *m_encryptAct = nullptr, *m_decryptAct = nullptr, *m_sanitizeAct = nullptr,
            *m_optimizeAct = nullptr, *m_repairAct = nullptr;
#endif
};

// Headless batch mode: returns an exit code, or -1 to launch the UI.
// ANGRA.exe --to-text in.pdf out.txt | --to-images in.pdf outdir |
//           --merge out.pdf in1.pdf in2.pdf...
static int runCli(const QStringList& args) {
    if (args.size() < 2 || !args.at(1).startsWith(QStringLiteral("--")))
        return -1;
    const QString cmd = args.at(1);
    auto fail = [](const char* msg) {
        std::fprintf(stderr, "%s\n", msg);
        return 2;
    };
    if (cmd == QStringLiteral("--to-text") && args.size() == 4) {
        PdfDocument doc;
        if (doc.load(args.at(2)) != PdfDocument::Status::Ok)
            return fail("cannot open input");
        QStringList parts;
        for (int i = 0; i < doc.pageCount(); ++i)
            parts << doc.pageText(i);
        QFile f(args.at(3));
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return fail("cannot write output");
        f.write(parts.join(QStringLiteral("\n\n")).toUtf8());
        return 0;
    }
    if (cmd == QStringLiteral("--to-images") && args.size() == 4) {
        PdfDocument doc;
        if (doc.load(args.at(2)) != PdfDocument::Status::Ok)
            return fail("cannot open input");
        for (int i = 0; i < doc.pageCount(); ++i) {
            const QString dest = QDir(args.at(3)).filePath(
                QStringLiteral("page-%1.png").arg(i + 1, 3, 10, QLatin1Char('0')));
            if (!doc.renderPage(i, 2.0).save(dest))
                return fail("cannot write image");
        }
        return 0;
    }
    if (cmd == QStringLiteral("--merge") && args.size() >= 5) {
        PdfDocument out;
        out.createEmpty();
        for (int i = 3; i < args.size(); ++i) {
            PdfDocument src;
            if (src.load(args.at(i)) != PdfDocument::Status::Ok ||
                !out.importPages(src, out.pageCount()))
                return fail("cannot merge input");
        }
        QString err;
        if (!out.saveCopy(args.at(2), &err))
            return fail("save failed");
        return 0;
    }
    std::fprintf(stderr, "usage: ANGRA --to-text in.pdf out.txt | "
                         "--to-images in.pdf outdir | --merge out.pdf in1 in2...\n");
    return 2;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ANGRA"));
    QApplication::setApplicationName(theme::kAppName);
    QApplication::setApplicationVersion(theme::kAppVersion);
    // Fusion first: it ignores the Windows native theme, so the stylesheet
    // renders the same on every machine instead of fighting the OS palette.
    QApplication::setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(QString::fromUtf8(theme::kStyleSheet));
    PdfDocument::initLibrary();
    if (const int cli = runCli(QApplication::arguments()); cli >= 0) {
        PdfDocument::shutdownLibrary();
        return cli;
    }
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
