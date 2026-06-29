#include "lepch.h"

#include "luthien/inspectors/PhysicsMaterialEditor.h"

#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/Icons.h"
#include "luth/physics/PhysicsMaterial.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/PhysicsMaterialImporter.h"
#include "luth/core/time/Time.h"

#include <fstream>

namespace Luth
{
    void PhysicsMaterialEditor::Draw(PhysicsMaterial& material)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return;

        // Reset the debounce timer when switching to a different material — leftover state from the
        // previous one would otherwise trigger an unrelated Save.
        if (material.Handle != m_LastUUID)
        {
            m_LastUUID    = material.Handle;
            m_PendingSave = false;
            m_SaveTimer   = 0.0f;
        }

        UI::InspectorHeader(static_cast<ImTextureID>(0), ICON_FA_BOWLING_BALL, 48.0f, [&]() {
            const ImVec4 nameCol = { 0.95f, 0.55f, 0.2f, 1.0f };
            ImGui::TextColored(nameCol, "%s%s (Physics Material)",
                material.GetName().c_str(), m_PendingSave ? "*" : "");
        });

        ImGui::Dummy({ 0, 4 });

        if (UI::BeginCollapsingHeader("Properties", true))
        {
            bool changed = false;
            if (UI::BeginProperties("PhysicsMaterialProps"))
            {
                changed |= UI::Property("Friction",    material.friction,    0.01f, 0.0f, 2.0f);
                changed |= UI::Property("Restitution", material.restitution, 0.01f, 0.0f, 1.0f);
                changed |= UI::Property("Density",     material.density,     1.0f,  1.0f, 20000.0f);
                UI::EndProperties();
            }
            if (changed)
            {
                m_PendingSave = true;
                m_SaveTimer   = kAutoSaveDelay;
            }
            UI::EndCollapsingHeader();
        }

        // Tick the debounce timer outside the property block so paused editing eventually
        // commits even when the user moves focus elsewhere within the inspector.
        if (m_PendingSave)
        {
            m_SaveTimer -= Time::DeltaTime();
            if (m_SaveTimer <= 0.0f)
            {
                Save(material);
                m_PendingSave = false;
            }
        }
    }

    void PhysicsMaterialEditor::Save(PhysicsMaterial& material)
    {
        const auto& meta = AssetDatabase::GetMetadata(material.Handle);
        if (meta.Path.empty()) return;

        // Source-of-truth is the .physmat JSON next to the user's project. Re-write it (cheap),
        // then re-serialize the binary artifact so subsequent Loads pick up the new values.
        nlohmann::json json;
        material.Serialize(json);

        {
            std::ofstream out(meta.Path);
            if (!out.is_open())
            {
                LH_LOG(Editor, error, "PhysicsMaterialEditor: cannot open {} for write", meta.Path.string());
                return;
            }
            out << json.dump(4);
        }

        PhysicsMaterialAssetData data;
        data.JsonData = std::move(json);
        const fs::path artifact = AssetDatabase::GetArtifactPath(material.Handle);
        AssetSerializer::SerializePhysicsMaterial(artifact, data);
    }
}
