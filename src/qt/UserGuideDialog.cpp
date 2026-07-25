#include "UserGuideDialog.hpp"

#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace amrvis::qt {

UserGuideDialog::UserGuideDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("AMReXplorer User Guide"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    resize(1000, 760);

    auto* browser = new QTextBrowser(this);
    browser->setObjectName(QStringLiteral("userGuideBrowser"));
    browser->setOpenExternalLinks(true);
    browser->document()->setDocumentMargin(16.0);
    browser->setSource(
        QUrl(QStringLiteral("qrc:/docs/user-guide.md")),
        QTextDocument::MarkdownResource);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(browser);
    layout->addWidget(buttons);
}

} // namespace amrvis::qt
