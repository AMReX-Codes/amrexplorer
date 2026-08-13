#pragma once

#include <amrexplorer/core/Geometry.hpp>

#include <QDockWidget>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

class QLineEdit;
class QPushButton;
class QTableView;
class QEvent;

namespace amrvis::qt {

struct FabSelectorEntry {
    std::size_t ordinal = 0;
    int level = 0;
    std::size_t blockIndex = 0;
    std::filesystem::path filePath;
    std::uint64_t fileOffset = 0;
    IntBox validBox;
    IntBox storedBox;
    int dimension = 0;
    int components = 0;
    QString precision;
    bool rawRecord = false;
};

class FabSelectorDock final : public QDockWidget {
    Q_OBJECT

public:
    explicit FabSelectorDock(QWidget* parent = nullptr);

    void setEntries(std::vector<FabSelectorEntry> entries);
    [[nodiscard]] const std::vector<FabSelectorEntry>& entries() const noexcept;
    void setBackAvailable(bool available);
    [[nodiscard]] bool backAvailable() const noexcept;
    void selectEntry(std::size_t ordinal);
    // The ordinal the table currently highlights, or nullopt if nothing is
    // selected. Together with backAvailable this is the selector state a
    // caller has to put back when an open it committed to optimistically
    // turns out to have failed.
    [[nodiscard]] std::optional<std::size_t> selectedOrdinal() const;
    void clearSelection();

signals:
    void viewRequested(std::size_t entry);
    void backRequested();

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void activateCurrent();
    void promptForPoint();

    class Model;
    class ProxyModel;
    Model* m_model = nullptr;
    ProxyModel* m_proxy = nullptr;
    QLineEdit* m_filter = nullptr;
    QPushButton* m_clearFilter = nullptr;
    QTableView* m_table = nullptr;
    QPushButton* m_view = nullptr;
    QPushButton* m_back = nullptr;
    bool m_backAvailable = false;
    int m_dimension = 0;
};

} // namespace amrvis::qt
