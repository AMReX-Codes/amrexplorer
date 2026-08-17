// The Open Remote dialog as a widget: prefill rules, the executable following
// the destination until edited, the path fields, and Browse... versus Open.
// What to do with the answer is RemoteSessionController's business.

#include "RemoteOpenDialog.hpp"

#include <QApplication>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

QPushButton* buttonNamed(QDialog& dialog, const QString& text)
{
    const auto buttons = dialog.findChildren<QPushButton*>();
    const auto found = std::find_if(buttons.begin(), buttons.end(),
        [&](QPushButton* button) { return button->text() == text; });
    return found == buttons.end() ? nullptr : *found;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    using amrvis::qt::RemoteOpenDialog;

    // Per-destination executables, the way the controller remembers them.
    std::map<QString, QString> executables{
        {QStringLiteral("frontier"), QStringLiteral("~/bin/amrexplorer-server")}};
    const auto executableFor = [&](const QString& destination) {
        const auto found = executables.find(destination);
        return found == executables.end()
            ? QStringLiteral("amrexplorer-server")
            : found->second;
    };

    // Single mode: prefill from the live session, executable follows the
    // destination, Open reports the trimmed fields and the one path.
    {
        RemoteOpenDialog dialog(false, QStringLiteral("frontier"),
            QStringLiteral("perlmutter"), executableFor);
        const auto edits = dialog.findChildren<QLineEdit*>();
        require(edits.size() == 3, "single dialog does not have three fields");
        auto* destination = edits[0];
        auto* executable = edits[1];
        auto* path = edits[2];
        require(destination->text() == QStringLiteral("frontier"),
            "the live session's destination was not prefilled");
        require(executable->text() == QStringLiteral("~/bin/amrexplorer-server"),
            "the destination's executable was not prefilled");
        destination->setText(QStringLiteral("perlmutter"));
        require(executable->text() == QStringLiteral("amrexplorer-server"),
            "the executable did not follow the destination");
        // A user edit pins the executable; a later destination change leaves
        // it alone. textEdited only fires for user edits, so simulate one.
        executable->setText(QStringLiteral("/opt/amrexplorer-server"));
        emit executable->textEdited(executable->text());
        destination->setText(QStringLiteral("frontier"));
        require(executable->text() == QStringLiteral("/opt/amrexplorer-server"),
            "a user-edited executable was overwritten");
        path->setText(QStringLiteral("  ~/run/plt00010  "));
        require(dialog.destination() == QStringLiteral("frontier")
                && dialog.executable() == QStringLiteral("/opt/amrexplorer-server")
                && dialog.paths().size() == 1
                && dialog.paths().front() == "~/run/plt00010"
                && !dialog.browseRequested(),
            "the single dialog did not report its trimmed fields");
        auto* buttons = dialog.findChild<QDialogButtonBox*>();
        require(buttons != nullptr
                && buttons->button(QDialogButtonBox::Ok)->text()
                    == QStringLiteral("Open"),
            "the accept button is not labelled Open");
        buttons->button(QDialogButtonBox::Ok)->click();
        require(dialog.result() == QDialog::Accepted && !dialog.browseRequested(),
            "Open did not accept without a browse request");
    }

    // No live session: the last destination used anywhere is the prefill;
    // Browse... accepts with the flag set.
    {
        RemoteOpenDialog dialog(
            false, QString(), QStringLiteral("perlmutter"), executableFor);
        require(dialog.destination() == QStringLiteral("perlmutter"),
            "the last destination was not prefilled");
        auto* browse = buttonNamed(dialog, QStringLiteral("Browse..."));
        require(browse != nullptr, "the dialog has no Browse... button");
        browse->click();
        require(dialog.result() == QDialog::Accepted && dialog.browseRequested(),
            "Browse... did not accept with the flag set");
    }

    // Sequence mode: one path per line, blanks and surrounding space dropped,
    // order kept.
    {
        RemoteOpenDialog dialog(
            true, QString(), QString(), executableFor);
        require(dialog.windowTitle().contains(QStringLiteral("Sequence")),
            "the sequence dialog is not titled as one");
        auto* paths = dialog.findChild<QPlainTextEdit*>();
        require(paths != nullptr, "the sequence dialog has no paths field");
        paths->setPlainText(
            QStringLiteral("/scratch/plt00010\n\n  /scratch/plt00020  \n"));
        const auto listed = dialog.paths();
        require(listed.size() == 2 && listed[0] == "/scratch/plt00010"
                && listed[1] == "/scratch/plt00020",
            "the sequence dialog did not report its paths in order");
        require(dialog.executable() == QStringLiteral("amrexplorer-server"),
            "an unknown destination did not default the executable");
    }

    std::cout << "remote open dialog tests passed\n";
    return 0;
}
