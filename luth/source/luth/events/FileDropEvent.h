#pragma once

#include "luth/events/Event.h"
#include <vector>
#include <filesystem>

namespace Luth
{
    // Posted when the OS reports paths dropped onto the window. App handles asset-import for
    // *.luthproj and recognized texture extensions; the editor's ProjectPanel + ResourcePanel
    // may also subscribe for in-panel drop targets.
    class FileDropEvent : public Event
    {
    public:
        explicit FileDropEvent(std::vector<fs::path>&& paths)
            : m_Paths(std::move(paths)) {
        }

        const char* GetName() const override { return "FileDropEvent"; }

        u32 GetCategoryFlags() const override {
            return EventCategoryInput | EventCategoryFileDrop;
        }

        const std::vector<fs::path>& GetPaths() const {
            return m_Paths;
        }

    private:
        std::vector<fs::path> m_Paths;
    };
}
