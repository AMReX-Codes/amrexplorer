#pragma once

#include "DatasetColoring.hpp"
#include "DatasetExtract.hpp"
#include "DatasetSelection.hpp"

#include <amrexplorer/core/Geometry.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetSession.hpp>

#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class QCloseEvent;
class QLabel;
class QTableView;
class QTabWidget;

namespace amrvis::qt {

// Everything one read of the dataset window needs: the dataset and field,
// the physical region of the source view, and for 3-D the view normal plus
// the current slice position.
struct DatasetRequest {
    std::shared_ptr<DatasetSession> dataset;
    FieldId field;
    QString fieldName;
    RealBox region;
    int normalAxis = 1;
    double slicePosition = 0.0;
};

// Modeless spreadsheet of the raw sample values in the active view's region
// (the legacy Dataset window): one tab per AMR level (for standalone
// MultiFabs and FABs, which have no AMR hierarchy, the single tab is named
// after the format instead) with the i/j sample indices as headers and the
// level min/max above the table; each value is drawn in its color-bar color
// (see DatasetColoring), and selecting values -- one click, or a drag across
// a block -- highlights the samples they stand for in the image. Reads run
// off the GUI thread and are cancelled on close or refresh.
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
    // The palette and display range the values are drawn in -- the active
    // view's, so the table agrees with the image and the color bar. Repaints
    // the loaded values in place; the values themselves are untouched, so a
    // palette or range change recolors the window without a re-read and
    // without disturbing the current tab or scroll position.
    void setColoring(DatasetColoring coloring);

signals:
    // Physical bounds of the selected samples at their level's resolution (one
    // cell for a click, the bounding box of a dragged block; 2-D leaves axis 2
    // zeroed).
    void cellActivated(const amrvis::RealBox& physicalCell);
    // The Refresh button; the owner rebuilds the request from app state.
    void refreshRequested();
    // A real (non-cancelled) extraction failure; the owner reports it through
    // its non-modal background-error channel instead of a blocking dialog.
    void extractionFailed(const QString& message);

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
    // A table's selection became non-empty: the other levels' tables give up
    // theirs (one selection at a time, so the highlight always names the level
    // whose tab it was made on) and the image marks what this one holds.
    void selectionChanged(std::size_t levelEntry, const QTableView& table);
    void clearOtherSelections(const QTableView& keep);
    void cellsSelected(
        std::size_t levelEntry, std::span<const CellRange> ranges);

    DatasetRequest m_request;
    QString m_numberFormat;
    DatasetColoring m_coloring;
    QLabel* m_status = nullptr;
    QTabWidget* m_tabs = nullptr;
    std::vector<LevelData> m_levels;
    StopSource m_stopSource;
    std::uint64_t m_generation = 0;
};

} // namespace amrvis::qt
