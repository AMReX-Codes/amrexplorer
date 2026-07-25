#include <amrexplorer/viewer/ViewerSession.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace amrvis {
namespace {

std::optional<FieldId> resolveBinding(const std::vector<FieldBinding>& bindings,
                                      const FieldKey& key) {
    const auto found =
        std::find_if(bindings.begin(), bindings.end(),
                     [&key](const FieldBinding& binding) { return binding.key == key; });
    if (found == bindings.end()) {
        return std::nullopt;
    }
    return found->field;
}

std::optional<FieldKey> keyForBinding(const std::vector<FieldBinding>& bindings, FieldId field) {
    const auto found =
        std::find_if(bindings.begin(), bindings.end(), [field](const FieldBinding& binding) {
            return binding.field.value == field.value;
        });
    if (found == bindings.end()) {
        return std::nullopt;
    }
    return found->key;
}

std::pair<FieldKey, FieldId> resolveOrFallback(const std::vector<FieldBinding>& bindings,
                                               const std::optional<FieldKey>& requested) {
    if (requested) {
        if (const auto field = resolveBinding(bindings, *requested)) {
            return {*requested, *field};
        }
    }
    if (bindings.empty()) {
        throw std::runtime_error("dataset has no scalar fields to display");
    }
    return {bindings.front().key, bindings.front().field};
}

} // namespace

ViewerRevision ExpressionDraft::baseRevision() const noexcept {
    return m_baseRevision;
}

const std::vector<ExpressionDefinition>& ExpressionDraft::definitions() const noexcept {
    return m_definitions;
}

std::vector<ExpressionDefinition>& ExpressionDraft::definitions() noexcept {
    return m_definitions;
}

ExpressionId ExpressionDraft::append(std::string name, std::string source) {
    if (m_nextExpressionId == 0) {
        throw std::overflow_error("expression id space is exhausted");
    }
    const auto id = ExpressionId{m_nextExpressionId++};
    m_definitions.push_back(ExpressionDefinition{id, std::move(name), std::move(source)});
    return id;
}

void ExpressionDraft::erase(std::size_t index) {
    if (index >= m_definitions.size()) {
        throw std::out_of_range("expression draft index is out of range");
    }
    m_definitions.erase(m_definitions.begin() + static_cast<std::ptrdiff_t>(index));
}

void ExpressionDraft::replaceImported(
    const std::vector<std::pair<std::string, std::string>>& definitions) {
    m_definitions.clear();
    m_definitions.reserve(definitions.size());
    for (const auto& [name, source] : definitions) {
        append(name, source);
    }
}

std::optional<FieldId> PreparedViewerSnapshot::resolve(const FieldKey& key) const {
    return resolveBinding(fieldBindings, key);
}

std::optional<FieldKey> PreparedViewerSnapshot::keyFor(FieldId field) const {
    return keyForBinding(fieldBindings, field);
}

std::vector<std::pair<std::string, std::string>>
PreparedViewerSnapshot::installedDefinitions() const {
    std::vector<std::pair<std::string, std::string>> definitions;
    for (const auto& installation : expressionInstallations) {
        if (const auto* installed = std::get_if<InstalledExpression>(&installation)) {
            definitions.emplace_back(installed->definition.name, installed->definition.source);
        }
    }
    return definitions;
}

PreparedViewerSnapshot prepareViewerSnapshot(ViewerDatasetSource source, const ViewerPlan& plan,
                                             StopToken cancellation) {
    if (cancellation.stop_requested()) {
        throw std::runtime_error("viewer preparation was cancelled");
    }

    PreparedViewerSnapshot result;
    result.baseRevision = plan.baseRevision;
    result.revision = plan.resultRevision;
    result.replacesCatalog = plan.replacesCatalog;
    result.expressions = plan.expressions;
    result.requestedField = plan.requestedField;
    result.requestedVectorFields = plan.requestedVectorFields;

    if (source.preparedMetadata) {
        result.dataset = std::make_shared<PlotfileDataset>(
            std::move(source.dataRoot), source.datasetId, source.cacheBudgetBytes,
            std::move(*source.preparedMetadata));
    } else {
        result.dataset = std::make_shared<PlotfileDataset>(std::move(source.path), source.datasetId,
                                                           source.cacheBudgetBytes);
    }

    const auto& storedFields = result.dataset->sourceMetadata().metadata->fields;
    result.fieldBindings.reserve(storedFields.size() + plan.expressions.size());
    for (std::size_t index = 0; index < storedFields.size(); ++index) {
        if (index > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("field id exceeds supported range");
        }
        result.fieldBindings.push_back(FieldBinding{NativeFieldKey{storedFields[index].name},
                                                    FieldId{static_cast<std::uint32_t>(index)},
                                                    storedFields[index].name});
    }

    result.expressionInstallations.reserve(plan.expressions.size());
    for (const auto& definition : plan.expressions) {
        if (cancellation.stop_requested()) {
            throw std::runtime_error("viewer preparation was cancelled");
        }
        try {
            const auto field = result.dataset->addDerivedField(
                {.name = definition.name, .expression = definition.source});
            result.expressionInstallations.push_back(InstalledExpression{definition, field});
            result.fieldBindings.push_back(
                FieldBinding{DerivedFieldKey{definition.id}, field, definition.name});
        } catch (const std::exception& error) {
            if (plan.expressionPolicy == ExpressionInstallPolicy::Strict) {
                throw;
            }
            result.expressionInstallations.push_back(
                UnavailableExpression{definition, error.what()});
            result.warnings.push_back("Skipped derived field '" + definition.name +
                                      "': " + error.what());
        }
    }

    const auto [resolvedKey, resolvedId] =
        resolveOrFallback(result.fieldBindings, result.requestedField);
    result.resolvedField = resolvedKey;
    result.resolvedFieldId = resolvedId;
    for (std::size_t axis = 0; axis < result.resolvedVectorFields.size(); ++axis) {
        const auto [key, field] =
            resolveOrFallback(result.fieldBindings, result.requestedVectorFields[axis]);
        result.resolvedVectorFields[axis] = key;
        result.resolvedVectorFieldIds[axis] = field;
    }
    return result;
}

ViewerRevision ViewerSession::revision() const noexcept {
    return m_revision;
}

ViewerRevision ViewerSession::touch() noexcept {
    ++m_revision.value;
    return m_revision;
}

void ViewerSession::reset() {
    touch();
    m_nextExpressionId = 1;
    m_expressions.clear();
    m_requestedField.reset();
    m_requestedVectorFields = {};
    m_fieldRanges.clear();
    m_snapshot.reset();
}

void ViewerSession::clearSnapshot() {
    m_snapshot.reset();
}

ExpressionDraft ViewerSession::beginExpressionEdit() const {
    ExpressionDraft draft;
    draft.m_baseRevision = m_revision;
    draft.m_nextExpressionId = m_nextExpressionId;
    draft.m_definitions = m_expressions;
    return draft;
}

ViewerPlan ViewerSession::planCurrent(ExpressionInstallPolicy policy) const {
    ViewerPlan plan;
    plan.baseRevision = m_revision;
    plan.resultRevision = m_revision;
    plan.expressionPolicy = policy;
    plan.expressions = m_expressions;
    plan.requestedField = m_requestedField;
    plan.requestedVectorFields = m_requestedVectorFields;
    plan.fieldRanges = m_fieldRanges;
    return plan;
}

ViewerPlan ViewerSession::planExpressions(const ExpressionDraft& draft,
                                          ExpressionInstallPolicy policy) const {
    if (draft.m_baseRevision != m_revision) {
        throw std::invalid_argument("expression draft is stale");
    }
    validateDraft(draft);
    ViewerPlan plan;
    plan.baseRevision = draft.m_baseRevision;
    plan.resultRevision = ViewerRevision{draft.m_baseRevision.value + 1};
    if (plan.resultRevision.value == 0) {
        throw std::overflow_error("viewer revision space is exhausted");
    }
    plan.replacesCatalog = true;
    plan.expressionPolicy = policy;
    plan.expressions = draft.m_definitions;
    plan.requestedField = m_requestedField;
    plan.requestedVectorFields = m_requestedVectorFields;
    plan.fieldRanges = m_fieldRanges;
    return plan;
}

bool ViewerSession::accept(PreparedViewerSnapshot snapshot) {
    if (snapshot.baseRevision != m_revision) {
        return false;
    }
    if (snapshot.replacesCatalog && m_revision.value == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    const auto expectedRevision =
        snapshot.replacesCatalog ? ViewerRevision{m_revision.value + 1} : m_revision;
    if (snapshot.revision != expectedRevision) {
        return false;
    }
    if (snapshot.replacesCatalog) {
        auto nextExpressionId = m_nextExpressionId;
        for (const auto& definition : snapshot.expressions) {
            if (definition.id.value >= nextExpressionId) {
                if (definition.id.value == std::numeric_limits<std::uint64_t>::max()) {
                    throw std::overflow_error("expression id space is exhausted");
                }
                nextExpressionId = definition.id.value + 1;
            }
        }
        m_expressions = snapshot.expressions;
        m_revision = snapshot.revision;
        m_nextExpressionId = nextExpressionId;
        const auto expressionExists = [this](ExpressionId id) {
            return std::any_of(
                m_expressions.begin(), m_expressions.end(),
                [id](const ExpressionDefinition& definition) { return definition.id == id; });
        };
        const auto reconcileRemoved = [&expressionExists](std::optional<FieldKey>& requested,
                                                          const FieldKey& fallback) {
            if (requested) {
                if (const auto* derived = std::get_if<DerivedFieldKey>(&*requested);
                    derived && !expressionExists(derived->expression)) {
                    requested = fallback;
                }
            }
        };
        reconcileRemoved(m_requestedField, snapshot.resolvedField);
        for (std::size_t axis = 0; axis < m_requestedVectorFields.size(); ++axis) {
            reconcileRemoved(m_requestedVectorFields[axis], snapshot.resolvedVectorFields[axis]);
        }
        for (auto it = m_fieldRanges.begin(); it != m_fieldRanges.end();) {
            const auto* derived = std::get_if<DerivedFieldKey>(&it->first);
            if (derived && !expressionExists(derived->expression)) {
                it = m_fieldRanges.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!m_requestedField) {
        m_requestedField = snapshot.resolvedField;
    }
    for (std::size_t axis = 0; axis < m_requestedVectorFields.size(); ++axis) {
        if (!m_requestedVectorFields[axis]) {
            m_requestedVectorFields[axis] = snapshot.resolvedVectorFields[axis];
        }
    }
    m_snapshot = std::move(snapshot);
    return true;
}

bool ViewerSession::hasSnapshot() const noexcept {
    return m_snapshot.has_value();
}

const PreparedViewerSnapshot& ViewerSession::snapshot() const {
    if (!m_snapshot) {
        throw std::logic_error("viewer session has no accepted snapshot");
    }
    return *m_snapshot;
}

std::shared_ptr<PlotfileDataset> ViewerSession::dataset() const noexcept {
    return m_snapshot ? m_snapshot->dataset : nullptr;
}

const std::vector<ExpressionDefinition>& ViewerSession::expressions() const noexcept {
    return m_expressions;
}

std::vector<std::pair<std::string, std::string>> ViewerSession::expressionPairs() const {
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(m_expressions.size());
    for (const auto& definition : m_expressions) {
        pairs.emplace_back(definition.name, definition.source);
    }
    return pairs;
}

std::vector<std::pair<std::string, std::string>> ViewerSession::installedExpressionPairs() const {
    return m_snapshot ? m_snapshot->installedDefinitions()
                      : std::vector<std::pair<std::string, std::string>>{};
}

void ViewerSession::setRequestedField(std::optional<FieldKey> field) {
    if (m_requestedField != field) {
        m_requestedField = std::move(field);
        touch();
    }
}

const std::optional<FieldKey>& ViewerSession::requestedField() const noexcept {
    return m_requestedField;
}

void ViewerSession::setRequestedVectorField(std::size_t axis, std::optional<FieldKey> field) {
    if (axis >= m_requestedVectorFields.size()) {
        throw std::out_of_range("vector-field axis is out of range");
    }
    if (m_requestedVectorFields[axis] != field) {
        m_requestedVectorFields[axis] = std::move(field);
        touch();
    }
}

const std::array<std::optional<FieldKey>, 3>&
ViewerSession::requestedVectorFields() const noexcept {
    return m_requestedVectorFields;
}

void ViewerSession::setFieldRange(const FieldKey& field, FieldRange range) {
    const auto found = m_fieldRanges.find(field);
    if (found == m_fieldRanges.end() || found->second != range) {
        m_fieldRanges[field] = std::move(range);
        touch();
    }
}

std::optional<FieldRange> ViewerSession::fieldRange(const FieldKey& field) const {
    if (const auto found = m_fieldRanges.find(field); found != m_fieldRanges.end()) {
        return found->second;
    }
    return std::nullopt;
}

void ViewerSession::clearFieldRanges() {
    if (!m_fieldRanges.empty()) {
        m_fieldRanges.clear();
        touch();
    }
}

std::optional<FieldKey> ViewerSession::keyFor(FieldId field) const {
    return m_snapshot ? m_snapshot->keyFor(field) : std::nullopt;
}

std::optional<FieldId> ViewerSession::resolve(const FieldKey& key) const {
    return m_snapshot ? m_snapshot->resolve(key) : std::nullopt;
}

void ViewerSession::validateDraft(const ExpressionDraft& draft) {
    std::set<std::uint64_t> ids;
    std::set<std::string> names;
    for (const auto& definition : draft.m_definitions) {
        if (definition.id.value == 0 || !ids.insert(definition.id.value).second) {
            throw std::invalid_argument("expression draft contains an invalid or duplicate id");
        }
        if (definition.name.empty()) {
            throw std::invalid_argument("derived-field name must not be empty");
        }
        if (definition.source.empty()) {
            throw std::invalid_argument("derived-field expression must not be empty");
        }
        if (!names.insert(definition.name).second) {
            throw std::invalid_argument("field name '" + definition.name + "' is already in use");
        }
    }
}

} // namespace amrvis
