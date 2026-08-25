#include "DerivedFieldStore.hpp"

#include <utility>

namespace amrvis::qt {

DerivedFieldStore::DerivedFieldStore(QObject* parent)
    : QObject(parent)
{
}

DerivedFieldStore& DerivedFieldStore::session()
{
    // Function-local, so it exists from first use and outlives every window
    // that reaches for it. No parent: it belongs to the process, not to a
    // widget tree that comes and goes.
    static DerivedFieldStore store;
    return store;
}

void DerivedFieldStore::set(std::vector<DerivedFieldDefinition> definitions)
{
    if (definitions == m_definitions) {
        return;
    }
    m_definitions = std::move(definitions);
    ++m_revision;
    emit changed();
}

} // namespace amrvis::qt
