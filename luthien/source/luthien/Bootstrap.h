#pragma once

namespace Luth
{
    /// Register the Luthien editor's IEditorHooks implementation with the
    /// engine. Must be called before any App is constructed.
    /// See runtime/source/LuthienApp.cpp::CreateApp for the call site.
    void InstallLuthienEditorHooks();
}
