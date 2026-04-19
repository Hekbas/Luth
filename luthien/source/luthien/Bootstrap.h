#pragma once

namespace Luth
{
    // Must be called before App is constructed so engine code can query
    // EditorHooks::Get() safely during startup.
    void InstallLuthienEditorHooks();
}
