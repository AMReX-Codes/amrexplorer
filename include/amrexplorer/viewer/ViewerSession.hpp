#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/io/PlotfileDataset.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace amrvis {

enum class RangeMode { Visible, Level, File, User };

struct FieldRange {
    RangeMode mode = RangeMode::File;
    std::optional<std::pair<double, double>> userRange;

    bool operator==(const FieldRange&) const = default;
};

struct ExpressionId {
    std::uint64_t value = 0;

    constexpr auto operator<=>(const ExpressionId&) const = default;
};

struct ExpressionDefinition {
    ExpressionId id;
    std::string name;
    std::string source;

    bool operator==(const ExpressionDefinition&) const = default;
};

struct NativeFieldKey {
    std::string name;

    auto operator<=>(const NativeFieldKey&) const = default;
};

struct DerivedFieldKey {
    ExpressionId expression;

    constexpr auto operator<=>(const DerivedFieldKey&) const = default;
};

using FieldKey = std::variant<NativeFieldKey, DerivedFieldKey>;

struct ViewerRevision {
    std::uint64_t value = 0;

    constexpr auto operator<=>(const ViewerRevision&) const = default;
};

class ExpressionDraft {
  public:
    [[nodiscard]] ViewerRevision baseRevision() const noexcept;
    [[nodiscard]] const std::vector<ExpressionDefinition>& definitions() const noexcept;
    [[nodiscard]] std::vector<ExpressionDefinition>& definitions() noexcept;

    ExpressionId append(std::string name = {}, std::string source = {});
    void erase(std::size_t index);
    void replaceImported(const std::vector<std::pair<std::string, std::string>>& definitions);

  private:
    friend class ViewerSession;

    ViewerRevision m_baseRevision;
    std::uint64_t m_nextExpressionId = 1;
    std::vector<ExpressionDefinition> m_definitions;
};

enum class ExpressionInstallPolicy { Strict, BestEffort };

struct ViewerPlan {
    ViewerRevision baseRevision;
    ViewerRevision resultRevision;
    bool replacesCatalog = false;
    ExpressionInstallPolicy expressionPolicy = ExpressionInstallPolicy::BestEffort;
    std::vector<ExpressionDefinition> expressions;
    std::optional<FieldKey> requestedField;
    std::array<std::optional<FieldKey>, 3> requestedVectorFields;
    std::map<FieldKey, FieldRange> fieldRanges;
};

struct ViewerDatasetSource {
    std::filesystem::path path;
    std::filesystem::path dataRoot;
    DatasetId datasetId;
    std::uint64_t cacheBudgetBytes = 0;
    std::optional<PlotfileMetadataResult> preparedMetadata;
};

struct InstalledExpression {
    ExpressionDefinition definition;
    FieldId field;
};

struct UnavailableExpression {
    ExpressionDefinition definition;
    std::string reason;
};

using ExpressionInstallation = std::variant<InstalledExpression, UnavailableExpression>;

struct FieldBinding {
    FieldKey key;
    FieldId field;
    std::string name;
};

struct PreparedViewerSnapshot {
    ViewerRevision baseRevision;
    ViewerRevision revision;
    bool replacesCatalog = false;
    std::vector<ExpressionDefinition> expressions;
    std::shared_ptr<PlotfileDataset> dataset;
    std::vector<ExpressionInstallation> expressionInstallations;
    std::vector<FieldBinding> fieldBindings;
    std::optional<FieldKey> requestedField;
    FieldKey resolvedField = NativeFieldKey{};
    FieldId resolvedFieldId{};
    std::array<std::optional<FieldKey>, 3> requestedVectorFields;
    std::array<FieldKey, 3> resolvedVectorFields{NativeFieldKey{}, NativeFieldKey{},
                                                 NativeFieldKey{}};
    std::array<FieldId, 3> resolvedVectorFieldIds{};
    std::vector<std::string> warnings;

    [[nodiscard]] std::optional<FieldId> resolve(const FieldKey& key) const;
    [[nodiscard]] std::optional<FieldKey> keyFor(FieldId field) const;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> installedDefinitions() const;
};

[[nodiscard]] PreparedViewerSnapshot prepareViewerSnapshot(ViewerDatasetSource source,
                                                           const ViewerPlan& plan,
                                                           StopToken cancellation = {});

class ViewerSession {
  public:
    [[nodiscard]] ViewerRevision revision() const noexcept;
    ViewerRevision touch() noexcept;
    void reset();
    void clearSnapshot();

    [[nodiscard]] ExpressionDraft beginExpressionEdit() const;
    [[nodiscard]] ViewerPlan
    planCurrent(ExpressionInstallPolicy policy = ExpressionInstallPolicy::BestEffort) const;
    [[nodiscard]] ViewerPlan
    planExpressions(const ExpressionDraft& draft,
                    ExpressionInstallPolicy policy = ExpressionInstallPolicy::Strict) const;

    [[nodiscard]] bool accept(PreparedViewerSnapshot snapshot);
    [[nodiscard]] bool hasSnapshot() const noexcept;
    [[nodiscard]] const PreparedViewerSnapshot& snapshot() const;
    [[nodiscard]] std::shared_ptr<PlotfileDataset> dataset() const noexcept;

    [[nodiscard]] const std::vector<ExpressionDefinition>& expressions() const noexcept;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> expressionPairs() const;
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> installedExpressionPairs() const;

    void setRequestedField(std::optional<FieldKey> field);
    [[nodiscard]] const std::optional<FieldKey>& requestedField() const noexcept;
    void setRequestedVectorField(std::size_t axis, std::optional<FieldKey> field);
    [[nodiscard]] const std::array<std::optional<FieldKey>, 3>&
    requestedVectorFields() const noexcept;

    void setFieldRange(const FieldKey& field, FieldRange range);
    [[nodiscard]] std::optional<FieldRange> fieldRange(const FieldKey& field) const;
    void clearFieldRanges();

    [[nodiscard]] std::optional<FieldKey> keyFor(FieldId field) const;
    [[nodiscard]] std::optional<FieldId> resolve(const FieldKey& key) const;

  private:
    static void validateDraft(const ExpressionDraft& draft);

    ViewerRevision m_revision;
    std::uint64_t m_nextExpressionId = 1;
    std::vector<ExpressionDefinition> m_expressions;
    std::optional<FieldKey> m_requestedField;
    std::array<std::optional<FieldKey>, 3> m_requestedVectorFields;
    std::map<FieldKey, FieldRange> m_fieldRanges;
    std::optional<PreparedViewerSnapshot> m_snapshot;
};

} // namespace amrvis
