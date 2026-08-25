#pragma once

#include <amrexplorer/core/DerivedField.hpp>

#include <QObject>

#include <cstdint>
#include <vector>

namespace amrvis::qt {

// The derived-field definitions, shared by every window of one running
// viewer. Deliberately process-wide and deliberately not persisted:
//
//  - a definition written in one window belongs in the others, which is what
//    "the expression list" means to someone with two windows open on the same
//    kind of data;
//  - a viewer started separately from the command line gets its own, because
//    it is its own process -- the isolation comes free with the scope;
//  - nothing survives the session, so a list written for one plotfile cannot
//    come back weeks later against unrelated data.
//
// Held by reference rather than reached through a global, so a test can give
// its controllers a store of their own -- and can give two controllers the
// same one to check that they see each other's edits.
class DerivedFieldStore final : public QObject {
    Q_OBJECT

public:
    explicit DerivedFieldStore(QObject* parent = nullptr);

    // The store every window of this process shares.
    [[nodiscard]] static DerivedFieldStore& session();

    [[nodiscard]] const std::vector<DerivedFieldDefinition>& definitions()
        const noexcept
    {
        return m_definitions;
    }

    // Replaces the list and tells everyone. A list equal to the current one
    // changes nothing and emits nothing, so a window re-applying what is
    // already installed does not make every other window reload.
    void set(std::vector<DerivedFieldDefinition> definitions);

    // How many times the list has actually changed. A window is not told to
    // reload while it has no dataset -- which is the whole of an open, not
    // just its start -- so a load launched before a change lands after it,
    // holding a session built from the older list. Capturing this when the
    // load is launched and reading it again when the load lands is how that
    // window notices.
    [[nodiscard]] std::uint64_t revision() const noexcept
    {
        return m_revision;
    }

signals:
    // The list changed. Every window re-reads it: the one that made the change
    // and the ones that did not.
    void changed();

private:
    std::vector<DerivedFieldDefinition> m_definitions;
    std::uint64_t m_revision = 0;
};

} // namespace amrvis::qt
