#pragma once

#include <QException>
#include <QString>

#include <exception>

namespace amrvis::qt {

// Qt Concurrent masks worker exceptions behind QUnhandledException. Rethrows
// the exception it wraps so the caller can catch the real type; returns
// normally when `error` is not a wrapper, leaving it to be inspected directly.
// Every place that inspects a QtConcurrent failure goes through here, so the
// unwrapping rule has one definition.
inline void rethrowUnwrapped(const std::exception& error)
{
    const auto* unhandled = dynamic_cast<const QUnhandledException*>(&error);
    if (unhandled != nullptr && unhandled->exception()) {
        std::rethrow_exception(unhandled->exception());
    }
}

// The underlying library error text, unwrapped before it is shown.
inline QString exceptionMessage(const std::exception& error)
{
    try {
        rethrowUnwrapped(error);
    } catch (const std::exception& inner) {
        return QString::fromUtf8(inner.what());
    } catch (...) {
        return QStringLiteral("unknown non-std exception");
    }
    return QString::fromUtf8(error.what());
}

} // namespace amrvis::qt
