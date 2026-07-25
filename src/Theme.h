#pragma once

// Design tokens — single source of truth for visual constants.
namespace theme {
inline constexpr const char* kAppName = "ANGRA Acrobat";
inline constexpr const char* kAppVersion = "0.1.0";

// Canvas sits darker than the chrome so white pages read as the focal point.
inline constexpr const char* kCanvasBackground = "#0d1117";
inline constexpr int kCanvasMargin = 12;
inline constexpr double kZoomStep = 1.25;
inline constexpr double kZoomMin = 0.10;
inline constexpr double kZoomMax = 8.0;

// Graphite base, cyan accent. One stylesheet applied to the whole app; the
// Fusion style underneath makes it render identically on every Windows build.
inline constexpr const char* kStyleSheet = R"(
QWidget {
    background: #161b22;
    color: #d7dee7;
    font-family: "Segoe UI Variable Text", "Segoe UI";
    font-size: 13px;
}
QMainWindow::separator { background: #22293a; width: 1px; height: 1px; }

/* --- menu bar: flat, wide hit targets, cyan accent on the open menu --- */
QMenuBar { background: #10141a; border-bottom: 1px solid #22293a; padding: 2px 6px; }
QMenuBar::item { padding: 7px 13px; background: transparent; border-radius: 4px; }
QMenuBar::item:selected { background: #1d2530; color: #ffffff; }
QMenuBar::item:pressed { background: #1d2530; color: #29d3f0; }

QMenu { background: #1b212b; border: 1px solid #2c3542; padding: 5px; border-radius: 6px; }
QMenu::item { padding: 7px 26px 7px 22px; border-radius: 4px; }
QMenu::item:selected { background: #16394a; color: #ffffff; }
QMenu::item:disabled { color: #5b6675; }
QMenu::separator { height: 1px; background: #2c3542; margin: 5px 8px; }

/* --- toolbar --- */
QToolBar {
    background: #10141a;
    border: none;
    border-bottom: 1px solid #22293a;
    spacing: 3px;
    padding: 5px 7px;
}
QToolBar QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 5px;
    padding: 6px 11px;
    color: #c2ccd8;
}
QToolBar QToolButton:hover { background: #1e2632; border-color: #303a49; color: #ffffff; }
QToolBar QToolButton:pressed { background: #16394a; }
QToolBar QToolButton:checked { background: #16394a; border-color: #29d3f0; color: #29d3f0; }
QToolBar QToolButton:disabled { color: #566171; }
QToolBar::separator { background: #2a3341; width: 1px; margin: 5px 7px; }

/* --- tool panel: labelled sections, full-width rows, accent bar on hover --- */
#toolPanel { background: #1b212b; border-right: 1px solid #22293a; }
#toolSection {
    color: #7b8797;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 1.3px;
    padding: 15px 4px 6px 4px;
    background: transparent;
}
#toolButton {
    background: transparent;
    border: none;
    border-left: 2px solid transparent;
    border-radius: 5px;
    padding: 8px 10px;
    text-align: left;
    color: #ccd5e0;
}
#toolButton:hover { background: #232c39; border-left-color: #29d3f0; color: #ffffff; }
#toolButton:pressed { background: #16394a; }
#toolButton:disabled { color: #576274; }

/* --- tabs --- */
QTabWidget::pane { border: none; background: #0d1117; }
QTabBar { background: #10141a; qproperty-drawBase: 0; }
QTabBar::tab {
    background: #171d26;
    color: #97a3b2;
    border: 1px solid #22293a;
    border-bottom: none;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 8px 17px;
    margin-right: 2px;
}
QTabBar::tab:selected { background: #1b212b; color: #29d3f0; border-top: 2px solid #29d3f0; }
QTabBar::tab:hover:!selected { background: #1e2632; color: #d7dee7; }

/* --- docks --- */
QDockWidget::title {
    background: #10141a;
    color: #8794a4;
    padding: 8px 11px;
    border-bottom: 1px solid #22293a;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 1.2px;
}

/* --- inputs --- */
QLineEdit, QSpinBox, QComboBox, QPlainTextEdit {
    background: #0f141b;
    border: 1px solid #2c3542;
    border-radius: 5px;
    padding: 6px 9px;
    selection-background-color: #16556b;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {
    border-color: #29d3f0;
}
QLineEdit:disabled { color: #566171; background: #131820; }
QComboBox::drop-down { border: none; width: 18px; }

/* --- buttons --- */
QPushButton {
    background: #232c39;
    border: 1px solid #333d4d;
    border-radius: 5px;
    padding: 7px 17px;
    color: #d7dee7;
    font-weight: 600;
}
QPushButton:hover { background: #2a3542; border-color: #29d3f0; }
QPushButton:pressed { background: #16394a; }
QPushButton:default { background: #17708a; border-color: #29d3f0; color: #ffffff; }
QPushButton:default:hover { background: #1c86a3; }
QPushButton:disabled { background: #1a2029; color: #566171; border-color: #262f3b; }

/* --- lists, trees, thumbnails --- */
QListWidget, QTreeWidget {
    background: #1b212b;
    border: none;
    outline: none;
    padding: 4px;
}
QListWidget::item { padding: 4px; border-radius: 5px; margin-bottom: 3px; }
QListWidget::item:selected { background: #16394a; border: 1px solid #29d3f0; }
QListWidget::item:hover:!selected { background: #232c39; }
QTreeWidget::item { padding: 5px 3px; border-radius: 4px; }
QTreeWidget::item:selected { background: #16394a; color: #ffffff; }
QHeaderView::section {
    background: #10141a;
    color: #8794a4;
    border: none;
    border-bottom: 1px solid #22293a;
    padding: 6px;
    font-weight: 700;
}

/* --- scrollbars: thin, no arrows --- */
QScrollBar:vertical { background: transparent; width: 11px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0; }
QScrollBar::handle {
    background: #394454;
    border-radius: 5px;
    min-height: 28px;
    min-width: 28px;
}
QScrollBar::handle:hover { background: #4a586b; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* --- status bar --- */
QStatusBar { background: #10141a; color: #8794a4; border-top: 1px solid #22293a; }
QStatusBar::item { border: none; }

QSplitter::handle { background: #22293a; }
QSplitter::handle:horizontal { width: 1px; }
QToolTip {
    background: #0f141b;
    color: #d7dee7;
    border: 1px solid #29d3f0;
    padding: 5px 8px;
    border-radius: 4px;
}
)";
} // namespace theme
