#pragma once

#include "luth/core/types/LuthMath.h"

#include <string>
#include <unordered_map>
#include <utility>

namespace Luth::UI
{
    // Per-widget edit lifecycle. `changed` mirrors the old bool return; `committed` is
    // true on the frame ImGui reports IsItemDeactivatedAfterEdit (= one undo per release).
    // `itemId` is the leaf ImGui item ID; pass to ConsumeItemPreEdit<T>(state.itemId).
    struct EditState {
        bool         changed   = false;
        bool         committed = false;
        unsigned int itemId    = 0;  // ImGuiID
        constexpr operator bool() const noexcept { return changed; }
    };

    // Per-T cache of pre-edit values, keyed by ImGuiID. Main-thread only by ImGui contract,
    // so no sync needed. Bounded by # of simultaneously-active items (<= 1 in practice);
    // size guard handles aborted-edit leakage (window closed mid-drag).
    template<typename T>
    inline std::unordered_map<unsigned int, T>& PreEditStore() {
        static std::unordered_map<unsigned int, T> s;
        return s;
    }

    template<typename T>
    inline void SaveItemPreEdit(unsigned int id, const T& v) {
        auto& s = PreEditStore<T>();
        if (s.size() > 32) s.clear();
        s[id] = v;
    }

    template<typename T>
    inline T ConsumeItemPreEdit(unsigned int id) {
        auto& s = PreEditStore<T>();
        auto it = s.find(id);
        if (it == s.end()) return T{};
        T v = std::move(it->second);
        s.erase(it);
        return v;
    }

    template<typename T>
    inline bool TryGetItemPreEdit(unsigned int id, T& out) {
        auto& s = PreEditStore<T>();
        auto it = s.find(id);
        if (it == s.end()) return false;
        out = it->second;
        return true;
    }

    bool BeginProperties(const char* id = "Properties");
    void EndProperties();

    // Call between BeginProperties() and the value widget to lay out the label cell.
    void PropertyLabel(const char* label);

    EditState Property(const char* label, std::string& value);
    EditState Property(const char* label, bool& value);
    EditState Property(const char* label, int& value, int min = 0, int max = 0);
    EditState Property(const char* label, float& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f);

    // Vector widgets with per-axis reset buttons (X/Y/Z/W).
    EditState Property(const char* label, Vec2& value, float speed = 0.1f, float resetValue = 0.0f);
    EditState Property(const char* label, Vec3& value, float speed = 0.1f, float resetValue = 0.0f);
    EditState Property(const char* label, Vec4& value, float speed = 0.1f, float resetValue = 0.0f);

    EditState PropertyColor(const char* label, Vec3& value);
    EditState PropertyColor(const char* label, Vec4& value);

    EditState PropertyCombo(const char* label, int& currentIndex, const char* const items[], int count);
}
