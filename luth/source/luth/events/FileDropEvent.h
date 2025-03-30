#pragma once

#include "Event.h"
#include <vector>
#include <filesystem>

namespace Luth
{
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
