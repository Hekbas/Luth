#pragma once

#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"

namespace Luth
{
    class EditorSelection
    {
    public:
        static void SelectEntity(Entity entity) {
            s_SelectedEntity = entity;
            s_SelectedResource = {};
            s_Version++;
        }

        static void SelectResource(UUID resource) {
            s_SelectedResource = resource;
            s_SelectedEntity = {};
            s_Version++;
        }

        static void ClearSelection() {
            s_SelectedEntity = {};
            s_SelectedResource = {};
            s_Version++;
        }

        static Entity GetSelectedEntity()  { return s_SelectedEntity; }
        static UUID GetSelectedResource()   { return s_SelectedResource; }
        static u32 GetVersion()             { return s_Version; }

    private:
        static inline Entity s_SelectedEntity;
        static inline UUID   s_SelectedResource;
        static inline u32    s_Version = 0;
    };
}
