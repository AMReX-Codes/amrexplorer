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

void checkCells(QTabWidget& tabs, bool distinct)
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
            RecordingPaintDevice device(width, height);
            QPainter painter(&device);
            QStyleOptionViewItem option;
            option.initFrom(table);
            option.font = table->font();
            option.fontMetrics = table->fontMetrics();
            option.widget = table;
            option.rect = QRect(0, 0, width - 1, height - 1);
            option.textElideMode = table->textElideMode();
            table->itemDelegate()->paint(&painter, option, index);
            painter.end();
            require(device.text().size() == 1 && device.text().front().value == expected,
                "dataset cell elided digits or the exponent");
            require(device.text().front().bounds.left() >= -0.5
                    && device.text().front().bounds.right() <= width - 0.5,
                "dataset cell text escaped its column");
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
            checkCells(*tabs, true);
            // Changing the format rebuilds the tables and must size them again.
            window.setNumberFormat("%.3e");
            checkCells(*tabs, false);
            window.setNumberFormat("rho=%.17g kg/m3");
            checkCells(*tabs, true);
            window.setNumberFormat("%g");
            checkCells(*tabs, true);
        }
    }
}
