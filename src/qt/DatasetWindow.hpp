#pragma once

#include "DatasetExtract.hpp"

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class QCloseEvent;
class QLabel;
class QTabWidget;

namespace amrvis {
class PlotfileDataset;
}

namespace amrvis::qt {

// Everything one read of the dataset window needs: the dataset and field,
// the physical region of the source view, and for 3-D the view normal plus
// the current slice position.
struct DatasetRequest {
    std::shared_ptr<PlotfileDataset> dataset;
    FieldId field;
    QString fieldName;
    RealBox region;
    int normalAxis = 1;
    double slicePosition = 0.0;
};

// Modeless spreadsheet of the raw sample values in the active view's region
// (the legacy Dataset window): one tab per AMR level with the i/j sample
// indices as headers and the level min/max above the table; clicking a value
// highlights the corresponding sample in the image. Reads run off the GUI
// thread and are cancelled on close or refresh.
class DatasetWindow final : public QWidget {
    Q_OBJECT

public:
    explicit DatasetWindow(DatasetRequest request, QWidget* parent = nullptr);
    ~DatasetWindow() override;

    // Re-reads with fresh app state (field, region, slice position),
    // cancelling any read still in flight.
    void reload(DatasetRequest request);
    // Applies the printf-style readout format, re-rendering the already
    // loaded values (no re-read) when the tabs are populated.
    void setNumberFormat(QString format);

signals:
    // Physical bounds of the clicked sample at its level's resolution (2-D
    // leaves axis 2 zeroed).
    void cellActivated(const amrvis::RealBox& physicalCell);
    // The Refresh button; the owner rebuilds the request from app state.
    void refreshRequested();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct LevelData {
        int level = 0;
        DatasetLevelExtract extract;
    };

    static std::vector<LevelData> extractLevels(
        const DatasetRequest& request, StopToken cancellation);
    void startLoad();
    void populateTabs();
    void cellClicked(std::size_t levelEntry, int row, int column);

    DatasetRequest m_request;
    QString m_numberFormat;
    QLabel* m_status = nullptr;
    QTabWidget* m_tabs = nullptr;
    std::vector<LevelData> m_levels;
    StopSource m_stopSource;
    std::uint64_t m_generation = 0;
};

} // namespace amrvis::qt
