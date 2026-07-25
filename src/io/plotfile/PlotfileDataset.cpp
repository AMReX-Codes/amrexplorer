#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/io/StandaloneMetadataReader.hpp>

#include <amrexplorer/expression/Expression.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace amrvis {
namespace {

std::mutex& globalAmrexIoMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::uint64_t residentBytes(const FabBlock& block)
{
    return static_cast<std::uint64_t>(sizeof(FabBlock))
        + block.values.residentBytes();
}

std::filesystem::path sourceDataRoot(const std::filesystem::path& path)
{
    if (std::filesystem::is_directory(path)
        && std::filesystem::is_regular_file(path / "Header")) {
        return path;
    }
    const auto parent = path.parent_path();
    return parent.empty() ? std::filesystem::path(".") : parent;
}

std::shared_ptr<DatasetMetadata> mutableMetadata(
    const PlotfileMetadataResult& result)
{
    if (!result.metadata) {
        throw std::invalid_argument(
            "selected FAB dataset requires metadata and an id");
    }
    return std::make_shared<DatasetMetadata>(*result.metadata);
}

constexpr std::size_t maximumDerivedInputs = 16;

struct ParserFieldAlias {
    std::string fieldName;
    std::string parserName;
};

struct AliasedExpression {
    std::string parserExpression;
    std::vector<ParserFieldAlias> aliases;
    std::vector<std::size_t> sourceOffsets;
};

bool isParserIdentifierStart(char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isParserIdentifierContinuation(char value)
{
    return isParserIdentifierStart(value)
        || (value >= '0' && value <= '9') || value == '.';
}

bool isReferenceableFieldName(const std::string& name)
{
    if (name.empty() || !isParserIdentifierStart(name.front())) {
        return false;
    }
    for (std::size_t position = 1; position < name.size(); ++position) {
        if (isParserIdentifierContinuation(name[position])) {
            continue;
        }
        if (name[position] != '-' || position + 1 == name.size()
            || !isParserIdentifierStart(name[position + 1])) {
            return false;
        }
        ++position;
    }
    return true;
}

AliasedExpression aliasDashedFieldNames(
    const std::string& expression, const std::vector<FieldMetadata>& fields)
{
    std::vector<ParserFieldAlias> aliases;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& fieldName = fields[index].name;
        if (fieldName.find('-') == std::string::npos) {
            continue;
        }

        auto parserName = "amrvis_field_alias_" + std::to_string(index);
        while (expression.find(parserName) != std::string::npos
            || std::any_of(
                fields.begin(), fields.end(),
                [&parserName](const FieldMetadata& field) {
                    return field.name == parserName;
                })) {
            parserName.push_back('_');
        }
        aliases.push_back({fieldName, std::move(parserName)});
    }
    std::sort(
        aliases.begin(), aliases.end(),
        [](const ParserFieldAlias& left, const ParserFieldAlias& right) {
            return left.fieldName.size() > right.fieldName.size();
        });

    std::string rewritten;
    rewritten.reserve(expression.size());
    std::vector<std::size_t> sourceOffsets;
    sourceOffsets.reserve(expression.size() + 1);
    for (std::size_t position = 0; position < expression.size();) {
        const auto match = std::find_if(
            aliases.begin(), aliases.end(),
            [&](const ParserFieldAlias& alias) {
                const auto length = alias.fieldName.size();
                if (expression.compare(position, length, alias.fieldName) != 0) {
                    return false;
                }
                const auto beginsAtBoundary = position == 0
                    || !isParserIdentifierContinuation(expression[position - 1]);
                const auto end = position + length;
                const auto endsAtBoundary = end == expression.size()
                    || !isParserIdentifierContinuation(expression[end]);
                return beginsAtBoundary && endsAtBoundary;
            });
        if (match == aliases.end()) {
            sourceOffsets.push_back(position);
            rewritten.push_back(expression[position]);
            ++position;
        } else {
            sourceOffsets.insert(
                sourceOffsets.end(), match->parserName.size(), position);
            rewritten += match->parserName;
            position += match->fieldName.size();
        }
    }
    sourceOffsets.push_back(expression.size());
    return {
        std::move(rewritten), std::move(aliases), std::move(sourceOffsets)};
}

IntBox ghostGrownBox(
    const IntBox& source, const Int3& ghost, int dimension)
{
    auto result = source;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto lower = static_cast<std::int64_t>(source.lower[i]) - ghost[i];
        const auto upper = static_cast<std::int64_t>(source.upper[i]) + ghost[i];
        if (lower < std::numeric_limits<int>::min()
            || lower > std::numeric_limits<int>::max()
            || upper < std::numeric_limits<int>::min()
            || upper > std::numeric_limits<int>::max()) {
            throw BlockReadError(
                "ghost-grown derived-field box exceeds supported integer range");
        }
        result.lower[i] = static_cast<int>(lower);
        result.upper[i] = static_cast<int>(upper);
    }
    return result;
}

std::uint64_t pointCount(const IntBox& box, int dimension)
{
    std::uint64_t result = 1;
    for (int axis = 0; axis < dimension; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        const auto signedLength = static_cast<std::int64_t>(box.upper[i])
            - box.lower[i] + 1;
        if (signedLength <= 0) {
            throw BlockReadError("derived-field block has a non-positive extent");
        }
        const auto length = static_cast<std::uint64_t>(signedLength);
        if (result > std::numeric_limits<std::uint64_t>::max() / length) {
            throw BlockReadError("derived-field point count overflows");
        }
        result *= length;
    }
    return result;
}

} // namespace

struct PlotfileDataset::DerivedField {
    std::vector<FieldId> inputs;
    std::shared_ptr<const CompiledExpression> expression;
};

PlotfileDataset::PlotfileDataset(
    std::filesystem::path plotfile, DatasetId id, std::uint64_t cacheBudgetBytes)
    : m_plotfile(sourceDataRoot(plotfile))
    , m_id(id)
    , m_metadataResult(readDatasetMetadata(plotfile))
    , m_metadata(std::make_shared<DatasetMetadata>(*m_metadataResult.metadata))
    , m_blockReader(m_plotfile, m_metadata)
    , m_cache(cacheBudgetBytes)
    , m_storedFieldCount(m_metadata->fields.size())
{
    if (m_id.value == 0) {
        throw std::invalid_argument("PlotfileDataset id must be nonzero");
    }
}

PlotfileDataset::PlotfileDataset(std::filesystem::path root, DatasetId id,
    std::uint64_t cacheBudgetBytes, PlotfileMetadataResult metadata)
    : m_plotfile(std::move(root))
    , m_id(id)
    , m_metadataResult(std::move(metadata))
    , m_metadata(mutableMetadata(m_metadataResult))
    , m_blockReader(m_plotfile, m_metadata)
    , m_cache(cacheBudgetBytes)
    , m_storedFieldCount(m_metadata->fields.size())
{
    if (m_id.value == 0) {
        throw std::invalid_argument("selected FAB dataset requires metadata and an id");
    }
}

const DatasetMetadata& PlotfileDataset::metadata() const noexcept
{
    return *m_metadata;
}

const PlotfileMetadataResult& PlotfileDataset::sourceMetadata() const noexcept
{
    return m_metadataResult;
}

const MetadataReadMetrics& PlotfileDataset::metadataReadMetrics() const noexcept
{
    return m_metadataResult.metrics;
}

DatasetId PlotfileDataset::id() const noexcept
{
    return m_id;
}

const std::filesystem::path& PlotfileDataset::dataRoot() const noexcept
{
    return m_plotfile;
}

FieldId PlotfileDataset::addDerivedField(
    const DerivedFieldDefinition& definition)
{
    if (!isReferenceableFieldName(definition.name)) {
        throw std::invalid_argument(
            "derived-field name must match "
            "[A-Za-z_][A-Za-z0-9_.]*"
            "(-[A-Za-z_][A-Za-z0-9_.]*)*");
    }
    if (definition.expression.empty()) {
        throw std::invalid_argument("derived-field expression must not be empty");
    }
    const auto duplicate = std::find_if(
        m_metadata->fields.begin(), m_metadata->fields.end(),
        [&definition](const FieldMetadata& field) {
            return field.name == definition.name;
        });
    if (duplicate != m_metadata->fields.end()) {
        throw std::invalid_argument(
            "field name '" + definition.name + "' is already in use");
    }
    const auto nonCellLevel = std::find_if(
        m_metadata->levels.begin(), m_metadata->levels.end(),
        [this](const LevelMetadata& level) {
            return centeringFromIndexType(
                level.domain.centering, m_metadata->dimension)
                != Centering::Cell;
        });
    if (nonCellLevel != m_metadata->levels.end()) {
        throw std::invalid_argument(
            "derived fields require cell-centered dataset levels");
    }

    auto derived = std::make_shared<DerivedField>();
    auto aliased =
        aliasDashedFieldNames(definition.expression, m_metadata->fields);
    try {
        derived->expression = std::make_shared<const CompiledExpression>(
            CompiledExpression::compile(aliased.parserExpression));
    } catch (const ExpressionError& error) {
        auto message = std::string(error.what());
        const auto offsetSuffix =
            " at byte " + std::to_string(error.offset());
        if (message.ends_with(offsetSuffix)) {
            message.erase(message.size() - offsetSuffix.size());
        }
        const auto mappedOffset = aliased.sourceOffsets[
            std::min(error.offset(), aliased.sourceOffsets.size() - 1)];
        throw ExpressionError(std::move(message), mappedOffset);
    }

    const auto symbols = derived->expression->symbols();
    if (symbols.size() > maximumDerivedInputs) {
        throw std::invalid_argument(
            "derived-field expressions may reference at most "
            + std::to_string(maximumDerivedInputs) + " fields");
    }
    derived->inputs.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        const auto alias = std::find_if(
            aliased.aliases.begin(), aliased.aliases.end(),
            [&symbol](const ParserFieldAlias& candidate) {
                return candidate.parserName == symbol;
            });
        const auto& fieldName =
            alias == aliased.aliases.end() ? symbol : alias->fieldName;
        const auto input = std::find_if(
            m_metadata->fields.begin(), m_metadata->fields.end(),
            [&fieldName](const FieldMetadata& field) {
                return field.name == fieldName;
            });
        if (input == m_metadata->fields.end()) {
            throw std::invalid_argument(
                "unknown field '" + fieldName
                + "' in derived-field expression");
        }
        if (input->componentCount != 1) {
            throw std::invalid_argument(
                "derived-field inputs must be scalar fields");
        }
        if (input->centering != Centering::Cell) {
            throw std::invalid_argument(
                "derived-field inputs must be cell-centered");
        }
        const auto index = static_cast<std::size_t>(
            std::distance(m_metadata->fields.begin(), input));
        if (index > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("derived-field id exceeds supported range");
        }
        derived->inputs.push_back(
            FieldId{static_cast<std::uint32_t>(index)});
    }

    if (m_metadata->fields.size()
        > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("derived-field id exceeds supported range");
    }
    const auto id = FieldId{
        static_cast<std::uint32_t>(m_metadata->fields.size())};
    m_metadata->fields.push_back(FieldMetadata{
        .name = definition.name,
        .componentCount = 1,
        .centering = Centering::Cell,
        .componentNames = {}
    });
    m_derivedFields.push_back(std::move(derived));
    return id;
}

bool PlotfileDataset::isDerivedField(FieldId field) const noexcept
{
    return field.value >= m_storedFieldCount
        && static_cast<std::size_t>(field.value) < m_metadata->fields.size();
}

PlotfileDataset::BlockAccess PlotfileDataset::requestBlock(
    const BlockRequest& request, StopToken cancellation)
{
    if (request.dataset != m_id) {
        throw BlockReadError("block request targets a different dataset");
    }
    const auto key = makeBlockKey(request);
    if (auto cached = m_cache.findAndPin(key)) {
        return {std::move(cached), true, {}};
    }

    if (request.field.value >= m_metadata->fields.size()) {
        throw BlockReadError("requested field is unavailable");
    }
    if (isDerivedField(request.field)) {
        const auto derivedIndex =
            static_cast<std::size_t>(request.field.value) - m_storedFieldCount;
        auto read = readDerivedBlock(
            request, *m_derivedFields.at(derivedIndex), cancellation);
        auto handle = m_cache.insertAndPin(
            key, read.block, residentBytes(*read.block));
        return {std::move(handle), false, read.metrics};
    }

    std::scoped_lock ioLock(globalAmrexIoMutex());
    if (cancellation.stop_requested()) {
        throw ReadCancelled();
    }
    if (auto cached = m_cache.findAndPin(key)) {
        return {std::move(cached), true, {}};
    }

    auto read = m_blockReader.readBlock(request, cancellation);
    auto handle = m_cache.insertAndPin(
        key, read.block, residentBytes(*read.block));
    return {std::move(handle), false, read.metrics};
}

BlockReadResult PlotfileDataset::readDerivedBlock(
    const BlockRequest& request, const DerivedField& field,
    StopToken cancellation)
{
    if (request.componentCount != 1 || request.firstComponent != 0) {
        throw BlockReadError("derived fields are scalar");
    }
    if (request.level < 0
        || static_cast<std::size_t>(request.level) >= m_metadata->levels.size()) {
        throw BlockReadError("requested level is unavailable");
    }
    const auto& level = m_metadata->levels[static_cast<std::size_t>(request.level)];
    if (request.gridIndex < 0
        || static_cast<std::size_t>(request.gridIndex) >= level.blocks.size()) {
        throw BlockReadError("requested grid is unavailable");
    }

    std::vector<BlockCache::Handle> inputBlocks;
    inputBlocks.reserve(field.inputs.size());
    BlockReadMetrics metrics;
    for (const auto inputField : field.inputs) {
        auto inputRequest = request;
        inputRequest.field = inputField;
        auto access = requestBlock(inputRequest, cancellation);
        metrics.filesRead += access.io.filesRead;
        metrics.bytesRead += access.io.bytesRead;
        metrics.valuesRead += access.io.valuesRead;
        inputBlocks.push_back(std::move(access.handle));
    }

    auto block = std::make_shared<FabBlock>();
    block->box = inputBlocks.empty()
        ? ghostGrownBox(
            level.blocks[static_cast<std::size_t>(request.gridIndex)].box,
            level.ghostWidth, m_metadata->dimension)
        : inputBlocks.front()->box;
    block->field = request.field;
    block->component = 0;
    const auto count = pointCount(block->box, m_metadata->dimension);
    if (count > std::numeric_limits<std::size_t>::max()) {
        throw BlockReadError("derived-field block exceeds addressable memory");
    }
    std::vector<double> values(static_cast<std::size_t>(count));

    for (const auto& input : inputBlocks) {
        if (!(input->box == block->box)
            || input->values.size() != values.size()) {
            throw BlockReadError(
                "derived-field inputs do not share the same cell box");
        }
    }
    std::vector<double> arguments(inputBlocks.size());
    auto evaluator = field.expression->makeEvaluator();
    for (std::size_t cell = 0; cell < values.size(); ++cell) {
        if ((cell & 4095U) == 0U && cancellation.stop_requested()) {
            throw ReadCancelled();
        }
        for (std::size_t input = 0; input < inputBlocks.size(); ++input) {
            arguments[input] = inputBlocks[input]->values[cell];
        }
        values[cell] = evaluator.evaluate(arguments);
    }
    block->values = FabValues{std::move(values)};
    return {std::shared_ptr<const FabBlock>(std::move(block)), metrics};
}

CacheMetrics PlotfileDataset::cacheMetrics() const
{
    return m_cache.metrics();
}

bool PlotfileDataset::setCacheBudget(std::uint64_t bytes)
{
    return m_cache.setBudget(bytes);
}

void PlotfileDataset::clearUnpinnedCache()
{
    m_cache.clearUnpinned();
}

} // namespace amrvis
