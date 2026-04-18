#include "luthpch.h"
#include "luth/core/EditorHooks.h"

namespace Luth::EditorHooks
{
    static IEditorHooks* s_Hooks = nullptr;

    void Register(IEditorHooks* hooks) { s_Hooks = hooks; }

    IEditorHooks* Get() { return s_Hooks; }
}
