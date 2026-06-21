#pragma once

#include <cstddef>

namespace Luth::UI
{
    // Search input with a magnifying-glass hint + inline clear button. Fills the available
    // content width; returns true on any edit (including the clear). buf is caller-owned and
    // persists the query across frames.
    bool FilterBox(const char* id, char* buf, std::size_t bufSize, const char* hint = "Search...");

    // Case-insensitive substring test. Empty/null filter matches everything; null text never
    // matches. Pairs with FilterBox to gate list rows.
    bool PassesFilter(const char* filter, const char* text);
}
