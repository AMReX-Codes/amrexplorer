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
    // widget tree that comes and goes. Never destroyed, deliberately: a
    // QObject with static storage duration is torn down after QApplication
    // has gone, which is the ordering Qt asks callers to stay out of.
    static auto* const store = new DerivedFieldStore;
    return *store;
}

void DerivedFieldStore::set(std::vector<DerivedFieldDefinition> definitions)
{
    if (definitions == m_definitions) {
        return;
    }
    m_definitions = std::move(definitions);
    emit changed();
}

} // namespace amrvis::qt
