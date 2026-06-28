#pragma once

#include <cstddef>

namespace Luth::UI
{
    // One selectable row in a CategoryList. `value` is the caller's enum (cast to int);
    // `enabled == false` greys the row and shows `disabledReason` on hover.
    struct CategoryItem
    {
        const char* category;        // group header (items sharing a category must be contiguous)
        const char* label;
        int         value;
        bool        enabled        = true;
        const char* disabledReason = nullptr;
    };

    // Filterable, category-grouped radio list — intended as a popup/dropdown body (the caller
    // owns the popup). Renders a FilterBox, then grouped radios; highlights the item whose
    // value == *current. Click writes the value into *current and returns true. filterBuf is
    // caller-owned and persists the query across frames. Replaces hand-rolled radio menus and
    // their fragile contiguous-enum index assumptions.
    bool CategoryList(const char* id, const CategoryItem* items, int count,
                      int* current, char* filterBuf, std::size_t filterBufSize);
}
