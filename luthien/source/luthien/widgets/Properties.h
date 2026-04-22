#pragma once

#include "luth/core/types/LuthMath.h"

#include <string>

namespace Luth::UI
{
    bool BeginProperties(const char* id = "Properties");
    void EndProperties();

    // Call between BeginProperties() and the value widget to lay out the label cell.
    void PropertyLabel(const char* label);

    bool Property(const char* label, std::string& value);
    bool Property(const char* label, bool& value);
    bool Property(const char* label, int& value, int min = 0, int max = 0);
    bool Property(const char* label, float& value, float speed = 0.1f, float min = 0.0f, float max = 0.0f);

    // Vector widgets with per-axis reset buttons (X/Y/Z/W).
    bool Property(const char* label, Vec2& value, float speed = 0.1f, float resetValue = 0.0f);
    bool Property(const char* label, Vec3& value, float speed = 0.1f, float resetValue = 0.0f);
    bool Property(const char* label, Vec4& value, float speed = 0.1f, float resetValue = 0.0f);

    bool PropertyColor(const char* label, Vec3& value);
    bool PropertyColor(const char* label, Vec4& value);

    bool PropertyCombo(const char* label, int& currentIndex, const char* const items[], int count);
}
