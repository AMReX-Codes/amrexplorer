#include "RemoteOpenDialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include <utility>

namespace amrvis::qt {

RemoteOpenDialog::RemoteOpenDialog(bool sequence,
    const QString& sessionDestination, const QString& lastDestination,
    std::function<QString(const QString&)> executableFor, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(sequence ? tr("Open Remote Plotfile Sequence")
                            : tr("Open Remote Plotfile"));
    // Wide enough that a typical scratch-filesystem path is visible whole.
    setMinimumWidth(560);
    auto* layout = new QFormLayout(this);
    auto* explanation = new QLabel(
        tr("AMReXplorer runs amrexplorer-server on the destination through "
           "ssh and talks to it over that connection. Any destination that "
           "works for the ssh command works here, including aliases from "
           "~/.ssh/config. An unchanged destination keeps the current "
           "session. Enter the plotfile path, or use Browse... to pick it "
           "on the remote machine."),
        this);
    explanation->setWordWrap(true);
    layout->addRow(explanation);
    // Prefill from the live session so opening another path reuses it; a
    // fresh window falls back to the last destination used anywhere.
    m_destinationEdit = new QLineEdit(
        sessionDestination.isEmpty() ? lastDestination : sessionDestination,
        this);
    m_destinationEdit->setPlaceholderText(tr("user@host or ssh alias"));
    layout->addRow(tr("SSH destination:"), m_destinationEdit);
    m_executableEdit = new QLineEdit(
        executableFor(m_destinationEdit->text().trimmed()), this);
    m_executableEdit->setToolTip(
        tr("Name on the remote PATH, or a path such as "
           "~/bin/amrexplorer-server. Remembered per destination."));
    layout->addRow(tr("Remote amrexplorer-server:"), m_executableEdit);
    // The executable follows the destination (each remembers its own) until
    // the user edits it in this dialog; textEdited fires only on user edits.
    connect(m_executableEdit, &QLineEdit::textEdited, this,
        [this] { m_executableEdited = true; });
    connect(m_destinationEdit, &QLineEdit::textChanged, this,
        [this, executableFor = std::move(executableFor)](const QString& text) {
            if (!m_executableEdited) {
                m_executableEdit->setText(executableFor(text.trimmed()));
            }
        });
    if (sequence) {
        m_pathsEdit = new QPlainTextEdit(this);
        m_pathsEdit->setPlaceholderText(
            tr("One plotfile path per line, in playback order"));
        m_pathsEdit->setTabChangesFocus(true);
        layout->addRow(tr("Plotfile paths on the remote machine:"), m_pathsEdit);
    } else {
        m_pathEdit = new QLineEdit(this);
        m_pathEdit->setPlaceholderText(
            tr("/path/to/plt00010 or ~/run/plt00010"));
        layout->addRow(tr("Plotfile path on the remote machine:"), m_pathEdit);
    }
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* openButton = buttons->button(QDialogButtonBox::Ok);
    openButton->setText(tr("Open"));
    // Browse... needs only the connection fields: it starts (or reuses) the
    // session and picks the path in the remote browser once it is ready.
    auto* browseButton
        = buttons->addButton(tr("Browse..."), QDialogButtonBox::ActionRole);
    browseButton->setToolTip(
        tr("Connect and choose the plotfile on the remote machine"));
    // Return follows the path contents, rather than whichever button last
    // had focus. The sequence editor still handles Return as a newline.
    for (auto* button : {openButton, browseButton, buttons->button(QDialogButtonBox::Cancel)}) {
        button->setAutoDefault(false);
    }
    const auto updateDefaultButton = [this, openButton, browseButton] {
        const bool browse = paths().empty();
        openButton->setDefault(!browse);
        browseButton->setDefault(browse);
    };
    if (m_pathEdit != nullptr) {
        connect(m_pathEdit, &QLineEdit::textChanged, this, updateDefaultButton);
    } else {
        connect(m_pathsEdit, &QPlainTextEdit::textChanged, this, updateDefaultButton);
    }
    updateDefaultButton();
    connect(browseButton, &QPushButton::clicked, this, [this] {
        m_browseRequested = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addRow(buttons);
}

QString RemoteOpenDialog::destination() const
{
    return m_destinationEdit->text().trimmed();
}

QString RemoteOpenDialog::executable() const
{
    return m_executableEdit->text().trimmed();
}

std::vector<std::string> RemoteOpenDialog::paths() const
{
    std::vector<std::string> paths;
    if (m_pathsEdit != nullptr) {
        for (const auto& line : m_pathsEdit->toPlainText().split(
                 QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const auto path = line.trimmed();
            if (!path.isEmpty()) {
                paths.push_back(path.toStdString());
            }
        }
    } else if (!m_pathEdit->text().trimmed().isEmpty()) {
        paths.push_back(m_pathEdit->text().trimmed().toStdString());
    }
    return paths;
}

} // namespace amrvis::qt
