#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <string>
#include <vector>

class QLineEdit;
class QPlainTextEdit;

namespace amrvis::qt {

// The Open Remote Plotfile / Open Remote Plotfile Sequence dialog: the SSH
// destination, the remote amrexplorer-server executable, and the plotfile
// path (or paths, one per line, when `sequence`). Open accepts with the
// fields; Browse... accepts with browseRequested() set, and only the
// connection fields matter then. The executable follows the destination --
// each destination remembers its own, looked up through `executableFor` --
// until the user edits it here. What to do with the answer (reuse the live
// session, start one, browse) is RemoteSessionController's decision, so the
// dialog just reports the fields, trimmed.
class RemoteOpenDialog final : public QDialog {
public:
    // `sessionDestination` prefills the destination while a session is live
    // (opening another path reuses it); otherwise `lastDestination`, the one
    // last used anywhere.
    RemoteOpenDialog(bool sequence, const QString& sessionDestination,
        const QString& lastDestination,
        std::function<QString(const QString&)> executableFor,
        QWidget* parent = nullptr);

    [[nodiscard]] QString destination() const;
    [[nodiscard]] QString executable() const;
    // The typed path(s), trimmed, empties dropped; one entry at most unless
    // the dialog is the sequence one.
    [[nodiscard]] std::vector<std::string> paths() const;
    [[nodiscard]] bool browseRequested() const noexcept
    {
        return m_browseRequested;
    }

private:
    QLineEdit* m_destinationEdit = nullptr;
    QLineEdit* m_executableEdit = nullptr;
    QLineEdit* m_pathEdit = nullptr;
    QPlainTextEdit* m_pathsEdit = nullptr;
    bool m_executableEdited = false;
    bool m_browseRequested = false;
};

} // namespace amrvis::qt
