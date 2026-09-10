#include "DatasetWindow.hpp"
#include "RecordingPaintDevice.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QTabWidget>
#include <QTest>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// Exercise the actual asynchronous load, model, column setup and delegate.
class PageSession final : public amrvis::DatasetSession {
public:
    PageSession()
    {
        m_metadata.dimension = 2;
        m_metadata.finestLevel = 1;
    }

    amrvis::DatasetId id() const noexcept override { return amrvis::DatasetId{1}; }
    const amrvis::DatasetMetadata& metadata() const noexcept override { return m_metadata; }
    const amrvis::MetadataReadMetrics& metadataReadMetrics() const noexcept override { return m_metrics; }
    const std::string& fileVersion() const noexcept override { return m_version; }
    const std::vector<amrvis::ParticleSpeciesMetadata>& particleSpecies() const noexcept override { return m_species; }
    amrvis::ViewDataResult requestView(const amrvis::ViewDataRequest&, amrvis::StopToken) override { return {}; }
    std::optional<amrvis::ValueRange> requestRange(const amrvis::RangeRequest&, amrvis::StopToken) override { return {}; }
    bool rangeAvailable(const amrvis::RangeRequest&) const noexcept override { return false; }
    amrvis::ParticleSample requestParticleSample(const std::string&, double, std::uint64_t, amrvis::StopToken) override { return {}; }
    amrvis::CacheMetrics cacheMetrics() const override { return {}; }
    bool setCacheBudget(std::uint64_t) override { return false; }
    void clearUnpinnedCache() override {}
    void close() noexcept override {}

    amrvis::DatasetPage requestDatasetPage(
        const amrvis::DatasetPageRequest& request, amrvis::StopToken) override
    {
        amrvis::DatasetPage page;
        page.nx = 3;
        page.ny = 1;
        page.upper = {2, 0};
        page.values = request.level == 0
            ? std::vector<double>{1.25663706212e-6, 1.256637062125e-6, 1.25663706213e-6}
            : std::vector<double>{-1.25663706213e200, -1.256637062125e200, -1.25663706212e200};
        page.covered = {1, 1, 1};
        page.minimum = page.values.front();
        page.maximum = page.values.back();
        page.hasFiniteValues = true;
        return page;
    }

private:
    amrvis::DatasetMetadata m_metadata;
    amrvis::MetadataReadMetrics m_metrics;
    std::string m_version;
    std::vector<amrvis::ParticleSpeciesMetadata> m_species;
};

void checkCells(QTabWidget& tabs, bool distinct, bool recordPaint)
{
    for (int level = 0; level < tabs.count(); ++level) {
        tabs.setCurrentIndex(level);
        auto* table = tabs.currentWidget()->findChild<QTableView*>();
        require(table != nullptr, "dataset tab has no table");
        // Exercise each column, including ones initially outside the viewport.
        for (int column = 0; column < table->model()->columnCount(); ++column) {
            const auto index = table->model()->index(0, column);
            table->scrollTo(index);
            QApplication::processEvents();
            const auto expected = index.data().toString();
            const int width = table->columnWidth(column);
            const int height = table->rowHeight(0);
            QStyleOptionViewItem option;
            option.initFrom(table);
            option.font = table->font();
            option.fontMetrics = table->fontMetrics();
            option.widget = table;
            option.rect = QRect(0, 0, width - 1, height - 1);
            option.textElideMode = table->textElideMode();
            // The style elides exactly when an item is wider than the space
            // it is painted in, so the column has to reserve what this cell's
            // own digits ask for, beside the grid line. Asked of the delegate
            // and the current style, which is what the view itself consults.
            const int needed = table->itemDelegate()->sizeHint(option, index).width();
            if (needed > width - 1) {
                std::cerr << "style=" << table->style()->objectName().toStdString()
                          << " font=" << table->font().family().toStdString()
                          << " text=" << expected.toStdString()
                          << " needs=" << needed << " column=" << width << '\n';
            }
            require(needed <= width - 1,
                "dataset column is narrower than the digits it has to show");
            if (recordPaint) {
                RecordingPaintDevice device(width, height);
                QPainter painter(&device);
                table->itemDelegate()->paint(&painter, option, index);
                painter.end();
                // Font fallback and script changes can split one label into
                // several text draws. Check every glyph run and the complete
                // string, rather than requiring a single paint-engine call.
                QString painted;
                for (const auto& run : device.text()) {
                    painted += run.value;
                    require(run.bounds.left() >= -0.5 && run.bounds.right() <= width - 0.5,
                        "dataset cell text escaped its column");
                }
                require(painted == expected, "dataset cell elided digits or the exponent");
            }
            if (distinct && column > 0) {
                require(expected != table->model()->index(0, column - 1).data().toString(),
                    "adaptive dataset values lost their distinguishing digits");
            }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    for (const auto& style : QStyleFactory::keys()) {
        QApplication::setStyle(QStyleFactory::create(style));
        // Every style sizes its cells, but only Qt's own drawing reaches a
        // recording paint device: a native style paints its items through a
        // platform graphics context that such a device cannot provide, and
        // then draws no text at all.
        const bool recordPaint = style.compare(QLatin1String("fusion"),
            Qt::CaseInsensitive) == 0;
        for (const int pixels : {12, 24}) {
            amrvis::qt::DatasetRequest request;
            request.dataset = std::make_shared<PageSession>();
            amrvis::qt::DatasetWindow window(request);
            auto font = window.font();
            font.setPixelSize(pixels);
            window.setFont(font);
            window.show();
            auto* tabs = window.findChild<QTabWidget*>();
            require(tabs != nullptr && QTest::qWaitFor([tabs] { return tabs->count() == 2; }),
                "dataset pages did not load");
            checkCells(*tabs, true, recordPaint);
            // Changing the format rebuilds the tables and must size them again.
            window.setNumberFormat("%.3e");
            checkCells(*tabs, false, recordPaint);
            window.setNumberFormat("rho=%.17g kg/m3");
            checkCells(*tabs, true, recordPaint);
            // Greek and Latin text exercise multiple glyph runs even when
            // the platform's default font covers the entire numeric label.
            window.setNumberFormat(QStringLiteral("\u03c1=%.17g kg/m3"));
            checkCells(*tabs, true, recordPaint);
            window.setNumberFormat("%g");
            checkCells(*tabs, true, recordPaint);
        }
    }
}
