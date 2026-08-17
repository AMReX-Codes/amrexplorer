#pragma once

#include <QSettings>
#include <QString>

#include <memory>

namespace amrvis::qt {

// The one QSettings store every window and controller of this application
// reads and writes. Collaborators that persist state take a settings factory
// through their Hooks rather than calling this directly, so their unit tests
// can point them at a scratch file.
inline constexpr auto kSettingsOrganization = "amrex-codes";
inline constexpr auto kSettingsApplication = "amrexplorer";

inline QSettings makeSettings()
{
    return QSettings(QString::fromLatin1(kSettingsOrganization),
        QString::fromLatin1(kSettingsApplication));
}

inline std::unique_ptr<QSettings> makeSettingsPtr()
{
    return std::make_unique<QSettings>(
        QString::fromLatin1(kSettingsOrganization),
        QString::fromLatin1(kSettingsApplication));
}

} // namespace amrvis::qt
