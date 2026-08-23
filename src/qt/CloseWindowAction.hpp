#pragma once

#include <QAction>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QWidget>
#include <Qt>

namespace amrvis::qt {

// The one close-window key for every top-level window that has one: Ctrl+W,
// and Cmd+W on macOS, where Qt::CTRL is Command. Spelled out rather than taken
// from QKeySequence::Close, whose first binding here is Ctrl+F4 -- and
// setShortcut would take only that first one.
[[nodiscard]] inline QKeySequence closeWindowShortcut()
{
    return QKeySequence(Qt::CTRL | Qt::Key_W);
}

// Gives one window that key. The action is added to the window itself, so the
// key works in the windows that carry no menu bar (Dataset, Line Plot) as well
// as in the two that show it in a File menu -- those take the returned action
// and add it there.
inline QAction* addCloseWindowAction(QWidget& window, const QString& text)
{
    auto* action = new QAction(text, &window);
    action->setShortcut(closeWindowShortcut());
    QObject::connect(action, &QAction::triggered, &window, &QWidget::close);
    window.addAction(action);
    return action;
}

} // namespace amrvis::qt
