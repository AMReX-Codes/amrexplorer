#include "UserGuideDialog.hpp"

#include <QApplication>
#include <QFile>
#include <QTextBrowser>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    [[maybe_unused]] QApplication application(argc, argv);

    amrvis::qt::UserGuideDialog dialog;
    const auto* browser =
        dialog.findChild<QTextBrowser*>(QStringLiteral("userGuideBrowser"));
    require(browser != nullptr, "the guide should contain a text browser");
    require(browser->source()
            == QUrl(QStringLiteral("qrc:/docs/user-guide.md")),
        "the browser should load the bundled Markdown resource");

    const auto text = browser->toPlainText();
    require(text.contains(QStringLiteral("AMReXplorer User Guide")),
        "the bundled guide should have its title");
    require(text.contains(QStringLiteral("A basic 2-D workflow")),
        "the bundled guide should include the basic workflow");
    require(text.contains(QStringLiteral("Keyboard and mouse quick reference")),
        "the bundled guide should include the quick reference");

    QFile image(QStringLiteral(":/docs/images/user-guide-overview.png"));
    require(image.exists(), "the overview image should be bundled");
    require(image.open(QIODevice::ReadOnly) && image.size() > 0,
        "the bundled overview image should be readable");
}
