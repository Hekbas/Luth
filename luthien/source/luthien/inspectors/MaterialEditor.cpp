#include "lepch.h"
#include "luthien/inspectors/MaterialEditor.h"
#include "luthien/Editor.h"
#include "luthien/EditorSettings.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/material/MaterialGraphCodegen.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/core/time/Time.h"
#include "luth/jobs/IOThread.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"

#include <imgui/imgui_internal.h>

#include <algorithm>

namespace Luth
{
    void MaterialEditor::Draw(Material& material)
    {
        // Skip all widget calls when the parent window is clipped/collapsed. Without this,
        // ImGui::BeginChild/EndChild can crash if the host window is not expecting child submissions
        // (e.g. during rapid dock/undock).
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return;

        // Feature-reveal state is per-material; drop it when the inspected material changes.
        if (material.Handle != m_RevealHandle) { m_RevealHandle = material.Handle; m_RevealMask = 0; }

        // Header: thumbnail-on-left, name + Shader combo on right.
        // Live 3D preview pinned in the footer.
        ImTextureID headerThumb = UI::ThumbnailCache::Get(material.Handle, AssetType::Material);
        UI::InspectorHeader(headerThumb, ICON_MATERIAL, 48.0f, [&]() {
            const ImVec4 nameCol = { 0.2f, 0.9f, 0.4f, 1.0f };
            ImGui::TextColored(nameCol, "%s%s (Material)",
                material.GetName().c_str(), material.NeedsSave() ? "*" : "");

            auto shader = material.GetShader();
            const std::string currentName = shader ? shader->GetName() : "<none>";

            // AlignTextToFramePadding so "Shader" sits at the same baseline as the combo's text.
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Shader"); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo("##Shader", currentName.c_str())) {
                for (const auto& [name, s] : ShaderLibrary::GetAll()) {
                    bool selected = shader && (s->Handle == material.GetShaderUUID());
                    if (ImGui::Selectable(name.c_str(), selected)) {
                        material.SetShader(s->Handle);
                        material.MarkDirty();
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        });

        ImGui::Dummy({ 0, 4 });

        // Pinned-footer layout: settings scroll above, splitter, 3D preview pinned bottom.
        // invariant: layout uses a SNAPSHOT of footerH frozen at frame start.
        // The Splitter mutates the persisted footerH (so next frame picks up the new height) but this
        // frame's Settings AND Preview both size with the snapshot, avoiding one-frame overshoot when
        // dragging UP. See git history for the failed in-place mutation attempt.
        const float kSplitterH    = 4.0f;
        const float kMinSettingsH = 80.0f;
        const float kMinFooterH   = 80.0f;
        const float kMaxFooterAbs = 400.0f;
        const float spacingY      = ImGui::GetStyle().ItemSpacing.y;
        const float availH        = ImGui::GetContentRegionAvail().y;
        float& footerH = Editor::GetSettings().texturePreviewFooterHeight;
        const float kMaxFooterH = std::max(kMinFooterH,
            std::min(kMaxFooterAbs, availH - kMinSettingsH - kSplitterH - 2.0f * spacingY));
        footerH = std::clamp(footerH, kMinFooterH, kMaxFooterH);
        const float footerH_snap = footerH;
        const float topH = availH - footerH_snap - kSplitterH - 2.0f * spacingY;

        if (ImGui::BeginChild("##Settings", { -1, topH }, false))
        {

        if (UI::BeginCollapsingHeader("Surface Settings", true))
        {
            Material::RenderMode currentMode = material.GetRenderMode();
            int modeIndex = static_cast<int>(currentMode);

            ImGui::Text("Render Mode"); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const char* renderModes[] = { "Opaque", "Cutout", "Transparent", "Fade" };
            if (ImGui::Combo("##RenderMode", &modeIndex, renderModes, IM_ARRAYSIZE(renderModes))) {
                material.SetRenderMode(static_cast<Material::RenderMode>(modeIndex));
                material.MarkDirty();
            }

            // Cutoff lives with the mode that consumes it (was buried in the retired Alpha map row).
            if (material.GetRenderMode() == Material::RenderMode::Cutout)
            {
                float cutoff = material.GetAlphaCutoff();
                ImGui::Text("Cutoff     "); ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::SliderFloat("##AlphaCutoff", &cutoff, 0.0f, 1.0f, "%.2f")) {
                    material.SetAlphaCutoff(cutoff);
                    material.MarkDirty();
                }
            }

            if (material.GetRenderMode() == Material::RenderMode::Transparent ||
                material.GetRenderMode() == Material::RenderMode::Fade)
            {
                int srcFactor = static_cast<int>(material.GetBlendSrc());
                int dstFactor = static_cast<int>(material.GetBlendDst());

                const char* blendFactors[] = { "Zero", "One", "SrcAlpha", "OneMinusSrcAlpha" };

                ImGui::Text("Blend Src  "); ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::Combo("##Blend Src", &srcFactor, blendFactors, IM_ARRAYSIZE(blendFactors))) {
                    material.SetBlendSrc(static_cast<Material::BlendFactor>(srcFactor));
                    material.MarkDirty();
                }

                ImGui::Text("Blend Dst  "); ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::Combo("##Blend Dst", &dstFactor, blendFactors, IM_ARRAYSIZE(blendFactors))) {
                    material.SetBlendDst(static_cast<Material::BlendFactor>(dstFactor));
                    material.MarkDirty();
                }
            }

            int cullIndex = static_cast<int>(material.GetCullMode());
            ImGui::Text("Face Cull  "); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const char* cullModes[] = { "Back", "Front", "None" };
            if (ImGui::Combo("##CullMode", &cullIndex, cullModes, IM_ARRAYSIZE(cullModes))) {
                material.SetCullMode(static_cast<Material::CullMode>(cullIndex));
                material.MarkDirty();
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 4 });

        // Shared texture-slot row (drag-drop / Delete-clear / context-menu / UV), reused by every group.
        // idSuffix disambiguates ImGui IDs when one MapType shows in two groups (the Thickness map lives
        // in both Transmission and Subsurface). Call only inside an open 4-column map table.
        const auto& textures = material.GetTextures();
        auto DrawSurfaceInput = [&](MapType type, const char* label, const char* idSuffix, std::function<void()> drawControl) {
            ImGui::TableNextRow();

            std::shared_ptr<Texture> texture;
            bool hasTexture = false;
            UUID textureUUID = UUID::Invalid();
            u32 uvIndex = 0;

            for (const auto& texInfo : textures) {
                if (texInfo.type == type) {
                    textureUUID = texInfo.Uuid;
                    uvIndex = texInfo.uvIndex;
                    if (texInfo.Uuid.IsValid() && !AssetManager::IsLoaded(texInfo.Uuid) && !AssetManager::IsLoading(texInfo.Uuid))
                        AssetManager::LoadAsync(texInfo.Uuid);

                    if (texture = AssetManager::GetAsset<Texture>(texInfo.Uuid)) {
                        hasTexture = true;
                        break;
                    }
                }
            }

            ImGui::PushID(label);
            ImGui::PushID(idSuffix);

            // Label column
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            bool enabled = material.IsUseMapEnabled(type);
            ImGui::Text("%s", label);

            // Rows whose factor is authorable without a texture stay live (color/scalar-only workflow).
            ImGui::BeginDisabled(!enabled && type != MapType::Diffuse && type != MapType::Metalness
                && type != MapType::Emissive && type != MapType::Subsurface);

            // Texture slot
            ImGui::TableNextColumn();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            std::string textureId = "##Texture_" + std::string(label);

            ImVec2 buttonSize(24, 24);
            if (hasTexture) {
                ImGui::ImageButton(textureId.c_str(), UI::GetTextureID(texture), buttonSize, { 0, 0 }, { 1, 1 });
            }
            else if (textureUUID.IsValid() && AssetManager::IsLoading(textureUUID)) {
                ImGui::Button("...", buttonSize);
            }
            else {
                ImGui::Button(textureId.c_str(), buttonSize);
            }
            ImGui::PopStyleVar();

            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID")) {
                    const UUID* droppedUUID = static_cast<const UUID*>(payload->Data);
                    material.SetTexture({ *droppedUUID, type, uvIndex });
                    material.EnableUseTexture(type, true);
                    material.EnableUseMap(type, true);
                    material.MarkDirty();
                }
                ImGui::EndDragDropTarget();
            }

            if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                material.SetTexture({ UUID::Invalid(), type, 0 });
                material.EnableUseTexture(type, false);
                material.EnableUseMap(type, false);
                material.MarkDirty();
            }

            if (hasTexture && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", texture->GetName().c_str());
                ImGui::Text("%dx%d", texture->GetWidth(), texture->GetHeight());
                ImGui::EndTooltip();
            }

            // Control column
            ImGui::TableNextColumn();
            ImGui::PushItemWidth(-1);
            if (drawControl) drawControl();
            else ImGui::Dummy(buttonSize);
            ImGui::PopItemWidth();

            // UV Channel
            ImGui::TableNextColumn();
            ImGui::PushItemWidth(-1);
            int uv = static_cast<int>(uvIndex);
            std::string uvId = "##UV_" + std::string(label);
            // Hide step buttons with 0 step
            if (ImGui::InputInt(uvId.c_str(), &uv, 0, 0)) {
                if (uv < 0) uv = 0;
                if (uv > 3) uv = 3;
                material.SetTexture({ textureUUID, type, static_cast<u32>(uv) });
                material.MarkDirty();
            }
            ImGui::PopItemWidth();

            ImGui::EndDisabled();

            if (ImGui::BeginPopupContextItem("RowContext"))
            {
                if (type != MapType::Diffuse) {
                    if (ImGui::MenuItem(enabled ? "Disable Map" : "Enable Map")) {
                        material.EnableUseMap(type, !enabled);
                        material.MarkDirty();
                    }
                }
                if (ImGui::MenuItem("Clear Texture")) {
                    material.SetTexture({ UUID::Invalid(), type, 0 });
                    material.EnableUseTexture(type, false);
                    material.MarkDirty();
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
            ImGui::PopID();
        };

        // Opens the shared 4-column map table (Label | Texture | Control | UV); caller closes with EndTable.
        auto BeginMapTable = [&](const char* id) -> bool {
            if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchProp)) return false;
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthFixed, 36.0f);
            ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("UV", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            return true;
        };

        // Feature-gated group: a checkbox-in-header section whose body shows only when active. active =
        // weight nonzero OR force-revealed this session; the weight stays the single source of truth the
        // shader reads. Toggling on seeds a visible default, off zeroes it (both routed through the debounce).
        auto FeatureGroup = [&](const char* name, u32 bit, bool weightActive,
                                const std::function<void()>& seedFn, const std::function<void()>& zeroFn,
                                const std::function<void()>& bodyFn) {
            bool wasActive = weightActive || (m_RevealMask & bit) != 0;
            bool en = wasActive;
            if (UI::BeginCollapsingHeader(name, &en, wasActive)) {
                bodyFn();
                UI::EndCollapsingHeader();
            }
            if (en != wasActive) {
                if (en) { m_RevealMask |= bit; seedFn(); }
                else    { m_RevealMask &= ~bit; zeroFn(); }
                material.MarkDirty();
            }
            ImGui::Dummy({ 0, 4 });
        };

        if (UI::BeginCollapsingHeader("Base", true))
        {
            if (BeginMapTable("BaseMaps")) {
                DrawSurfaceInput(MapType::Diffuse, "Albedo", "", [&]() {
                    Vec4 color = material.GetColor();
                    if (ImGui::ColorEdit4("##AlbedoColor", &color.x, ImGuiColorEditFlags_NoInputs)) {
                        material.SetColor(color);
                        material.MarkDirty();
                    }
                });

                // Packed glTF metallic-roughness. While a map is bound+enabled the shader replaces the
                // scalars (material.slang: roughness = G, metallic = B), so hide the sliders and show the
                // channel mapping rather than two dead controls.
                DrawSurfaceInput(MapType::Metalness, "Mask Map", "", [&]() {
                    bool maskActive = material.GetTextureByType(MapType::Metalness) != nullptr
                                   && material.IsUseMapEnabled(MapType::Metalness);
                    if (maskActive) {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("G:Rough  B:Metal");
                        ImGui::SameLine();
                        UI::HelpMarker("Packed metallic-roughness map. Green = roughness, blue = metallic; "
                                       "these replace the sliders while the map is bound.");
                    } else {
                        float met  = material.GetMetalness();
                        float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                        ImGui::SetNextItemWidth(half);
                        if (ImGui::SliderFloat("##Met", &met, 0.0f, 1.0f, "M %.2f")) { material.SetMetalness(met); material.MarkDirty(); }
                        ImGui::SameLine();
                        float ro = material.GetRoughness();
                        ImGui::SetNextItemWidth(-1);
                        if (ImGui::SliderFloat("##Rou", &ro, 0.0f, 1.0f, "R %.2f")) { material.SetRoughness(ro); material.MarkDirty(); }
                    }
                });

                DrawSurfaceInput(MapType::Normal, "Normal", "", nullptr);
                DrawSurfaceInput(MapType::Height, "Height", "", nullptr);
                DrawSurfaceInput(MapType::Occlusion, "Occlusion", "", nullptr);
                DrawSurfaceInput(MapType::Emissive, "Emissive", "", [&]() {
                    // Swatch = LDR factor, drag = HDR strength multiplier (feeds bloom).
                    Vec3 emColor = material.GetEmissiveColor();
                    f32  emStr   = material.GetEmissiveStrength();
                    bool changed = false;
                    if (ImGui::ColorEdit3("##EmissiveColor", &emColor.x, ImGuiColorEditFlags_NoInputs)) {
                        material.SetEmissiveColor(emColor); changed = true;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragFloat("##EmissiveStrength", &emStr, 0.05f, 0.0f, 100.0f, "%.2f")) {
                        material.SetEmissiveStrength(emStr); changed = true;
                    }
                    if (changed) material.MarkDirty();
                });
                DrawSurfaceInput(MapType::Decal, "Decal", "", nullptr);

                ImGui::EndTable();
            }

            // Always-on dielectric scalars (no map). Specular scales the IOR-derived F0; IOR drives that
            // F0 for every surface plus refraction, so it lives in Base rather than under Transmission.
            if (UI::BeginProperties("BaseScalars"))
            {
                float spec = material.GetSpecular();
                if (UI::Property("Specular", spec, 0.005f, 0.0f, 1.0f)) { material.SetSpecular(spec); material.MarkDirty(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dielectric specular F0 weight. 1 = physical; scales the IOR-derived base reflectance. Metals ignore it.");
                float ior = material.GetIor();
                if (UI::Property("IOR", ior, 0.005f, 1.0f, 3.0f)) { material.SetIor(ior); material.MarkDirty(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Index of refraction. Drives dielectric F0 for every surface and refraction for transmissive ones (1.5 = common dielectric).");
                UI::EndProperties();
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 4 });

        // Optional shading-model lobes, each shown only when in use (see FeatureGroup). Bits index m_RevealMask.
        constexpr u32 kFeatClearcoat    = 1u << 0;
        constexpr u32 kFeatAnisotropy   = 1u << 1;
        constexpr u32 kFeatTransmission = 1u << 2;
        constexpr u32 kFeatSheen        = 1u << 3;
        constexpr u32 kFeatSubsurface   = 1u << 4;

        FeatureGroup("Clear Coat", kFeatClearcoat, material.GetClearcoat() != 0.0f,
            [&]() { material.SetClearcoat(1.0f); },
            [&]() { material.SetClearcoat(0.0f); material.SetClearcoatRoughness(0.0f); },
            [&]() {
                if (UI::BeginProperties("CoatProps")) {
                    float cc = material.GetClearcoat();
                    if (UI::Property("Weight", cc, 0.01f, 0.0f, 1.0f)) { material.SetClearcoat(cc); material.MarkDirty(); }
                    float ccr = material.GetClearcoatRoughness();
                    if (UI::Property("Roughness", ccr, 0.01f, 0.0f, 1.0f)) { material.SetClearcoatRoughness(ccr); material.MarkDirty(); }
                    UI::EndProperties();
                }
            });

        FeatureGroup("Anisotropy", kFeatAnisotropy, material.GetAnisotropy() != 0.0f,
            [&]() { material.SetAnisotropy(0.5f); },
            [&]() { material.SetAnisotropy(0.0f); material.SetAnisotropyRotation(0.0f); },
            [&]() {
                if (UI::BeginProperties("AnisoProps")) {
                    float an = material.GetAnisotropy();
                    if (UI::Property("Strength", an, 0.01f, -1.0f, 1.0f)) { material.SetAnisotropy(an); material.MarkDirty(); }
                    float ar = material.GetAnisotropyRotation();
                    if (UI::Property("Rotation", ar, 0.005f, 0.0f, 1.0f)) { material.SetAnisotropyRotation(ar); material.MarkDirty(); }
                    UI::EndProperties();
                }
            });

        FeatureGroup("Transmission", kFeatTransmission, material.GetTransmission() != 0.0f,
            [&]() { material.SetTransmission(1.0f); },
            [&]() { material.SetTransmission(0.0f); },
            [&]() {
                // Raster refraction needs a transparent render mode (TLAS masks key on it); PathTrace refracts regardless.
                if (material.GetRenderMode() == Material::RenderMode::Opaque ||
                    material.GetRenderMode() == Material::RenderMode::Cutout) {
                    ImGui::TextDisabled("Set Render Mode to Transparent/Fade");
                    ImGui::SameLine();
                    UI::HelpMarker("Raster refraction requires a transparent render mode. The path-traced reference refracts regardless.");
                }
                if (UI::BeginProperties("TransProps")) {
                    float tr = material.GetTransmission();
                    if (UI::Property("Transmission", tr, 0.01f, 0.0f, 1.0f)) { material.SetTransmission(tr); material.MarkDirty(); }
                    float th = material.GetThickness();
                    if (UI::Property("Glass Thickness", th, 0.01f, 0.0f, 100.0f)) { material.SetThickness(th); material.MarkDirty(); }
                    Vec4 ac(material.GetAttenuationColor(), 1.0f);
                    if (UI::PropertyColor("Attenuation Color", ac)) { material.SetAttenuationColor(Vec3(ac)); material.MarkDirty(); }
                    float ad = material.GetAttenuationDistance();
                    if (UI::Property("Attenuation Dist", ad, 0.05f, 0.0f, 1000.0f)) { material.SetAttenuationDistance(ad); material.MarkDirty(); }
                    UI::EndProperties();
                }
                if (BeginMapTable("TransMaps")) {
                    DrawSurfaceInput(MapType::Thickness, "Thickness", "glass", nullptr);
                    ImGui::EndTable();
                }
            });

        Vec3 shc = material.GetSheenColor();
        FeatureGroup("Sheen", kFeatSheen, (shc.x > 0.0f || shc.y > 0.0f || shc.z > 0.0f),
            [&]() { material.SetSheenColor(Vec3(0.5f)); },
            [&]() { material.SetSheenColor(Vec3(0.0f)); },
            [&]() {
                if (UI::BeginProperties("SheenProps")) {
                    Vec4 sc(material.GetSheenColor(), 1.0f);
                    if (UI::PropertyColor("Color", sc)) { material.SetSheenColor(Vec3(sc)); material.MarkDirty(); }
                    float shr = material.GetSheenRoughness();
                    if (UI::Property("Roughness", shr, 0.01f, 0.0f, 1.0f)) { material.SetSheenRoughness(shr); material.MarkDirty(); }
                    UI::EndProperties();
                }
            });

        Vec3 ssc = material.GetSubsurfaceColor();
        FeatureGroup("Subsurface", kFeatSubsurface, (ssc.x > 0.0f || ssc.y > 0.0f || ssc.z > 0.0f),
            [&]() { material.SetSubsurfaceColor(Vec3(0.8f)); if (material.GetSubsurfaceRadius() == 0.0f) material.SetSubsurfaceRadius(0.5f); },
            [&]() { material.SetSubsurfaceColor(Vec3(0.0f)); },
            [&]() {
                if (material.GetTransmission() > 0.0f) {
                    ImGui::TextDisabled("Suppressed while Transmission > 0");
                    ImGui::SameLine();
                    UI::HelpMarker("Subsurface and transmission are mutually exclusive; the shader fades subsurface out as transmission rises.");
                }
                if (BeginMapTable("SubsurfaceMaps")) {
                    DrawSurfaceInput(MapType::Subsurface, "Scatter Mask", "", [&]() {
                        // Swatch = diffusion albedo (texture-modulated when a scatter mask is bound).
                        Vec3 c = material.GetSubsurfaceColor();
                        if (ImGui::ColorEdit3("##SubsurfaceColor", &c.x, ImGuiColorEditFlags_NoInputs)) {
                            material.SetSubsurfaceColor(c); material.MarkDirty();
                        }
                    });
                    DrawSurfaceInput(MapType::Thickness, "Thickness", "sss", nullptr);
                    ImGui::EndTable();
                }
                if (UI::BeginProperties("SubsurfaceProps")) {
                    f32 ssRad = material.GetSubsurfaceRadius();
                    if (UI::Property("Radius", ssRad, 0.01f, 0.0f, 10.0f)) { material.SetSubsurfaceRadius(ssRad); material.MarkDirty(); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scatter mean-free-path in world units. 0 collapses the effect.");
                    f32 ssThick = material.GetSubsurfaceThickness();
                    if (UI::Property("Thickness", ssThick, 0.01f, 0.0f, 1.0f)) { material.SetSubsurfaceThickness(ssThick); material.MarkDirty(); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thin-shell back-scatter depth [0,1]; thinner reads as more translucent glow.");
                    UI::EndProperties();
                }
            });

        ImGui::Dummy({ 0, 4 });

        // Exposed graph parameters: named value nodes edited without opening the graph. Value edits land
        // as per-material data (RefreshParams, no recompile); the TextureSample slot is structure and
        // recompiles, mirroring the graph panel's split.
        if (material.HasGraph())
        {
            std::vector<MatNode*> params;   // per-frame walk; pointers must not outlive this Draw
            for (MatNode& n : material.GetGraphMutable().nodes)
                if (IsExposableNode(n.type) && !n.name.empty()) params.push_back(&n);
            std::stable_sort(params.begin(), params.end(),
                [](const MatNode* a, const MatNode* b) { return a->group < b->group; });

            if (!params.empty() && UI::BeginCollapsingHeader("Parameters", true))
            {
                bool valueEdit = false, structureEdit = false;

                auto drawParam = [&](MatNode& n)
                {
                    ImGui::PushID((int)n.id);
                    const char* label = n.name.c_str();
                    switch (n.type)
                    {
                        case MatNodeType::ConstFloat:
                            if (n.ui == 1)
                            {
                                bool b = n.value.x != 0.0f;
                                if (UI::Property(label, b)) { n.value.x = b ? 1.0f : 0.0f; valueEdit = true; }
                            }
                            else if (UI::Property(label, n.value.x, 0.01f))
                                valueEdit = true;
                            break;
                        case MatNodeType::ConstColor:
                            if (UI::PropertyColor(label, n.value)) valueEdit = true;
                            break;
                        case MatNodeType::Remap:
                            if (UI::Property(label, n.value, 0.01f)) valueEdit = true;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("(in min, in max, out min, out max)");
                            break;
                        case MatNodeType::TextureSample:
                        {
                            static const char* kMap[] = { "Diffuse","Alpha","Normal","Metallic","Roughness","Specular","Occlusion","Emissive","Thickness" };
                            int t = (n.tex < 9) ? (int)n.tex : 0;
                            if (UI::PropertyCombo(label, t, kMap, 9)) { n.tex = (u32)t; structureEdit = true; }
                            break;
                        }
                        case MatNodeType::StaticSwitch:
                        {
                            bool on = n.value.x != 0.0f;
                            // Compile-time switch: the state selects the emitted branch, so toggling recompiles.
                            if (UI::Property(label, on)) { n.value.x = on ? 1.0f : 0.0f; structureEdit = true; }
                            break;
                        }
                        case MatNodeType::Noise:
                            if (UI::Property(label, n.value, 0.05f)) valueEdit = true;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("(scale, octaves, -, -)");
                            break;
                        case MatNodeType::Fresnel:
                            if (UI::Property(label, n.value.x, 0.05f)) valueEdit = true;
                            break;
                        case MatNodeType::Triplanar:
                            if (UI::Property(label, n.value.x, 0.05f)) valueEdit = true;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("triplanar tiling");
                            break;
                        case MatNodeType::DetailNormal:
                            if (UI::Property(label, n.value.x, 0.02f)) valueEdit = true;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("detail-normal strength");
                            break;
                        case MatNodeType::Parallax:
                            if (UI::Property(label, n.value.x, 0.002f)) valueEdit = true;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("parallax height scale");
                            break;
                        case MatNodeType::Decal:
                            if (UI::Property(label, n.value, 0.005f)) valueEdit = true;
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("(offset x, offset y, scale, rotation rad)");
                            break;
                        default: break;
                    }
                    ImGui::PopID();
                };

                // Group runs (stable-sorted, ungrouped "" first): ungrouped rows sit directly under the
                // header; each named group is a framed tree node wrapping its own properties table.
                size_t i = 0;
                while (i < params.size())
                {
                    const std::string& grp = params[i]->group;
                    size_t end = i;
                    while (end < params.size() && params[end]->group == grp) ++end;

                    bool open = true;
                    if (!grp.empty())
                        open = ImGui::TreeNodeEx(grp.c_str(), ImGuiTreeNodeFlags_Framed
                            | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
                    if (open)
                    {
                        if (UI::BeginProperties("GraphParams"))
                        {
                            for (size_t k = i; k < end; ++k) drawParam(*params[k]);
                            UI::EndProperties();
                        }
                        if (!grp.empty()) ImGui::TreePop();
                    }
                    i = end;
                }

                if (structureEdit)  { MaterialGraphCodegen::GenerateAndCompile(material); material.MarkDirty(); }
                else if (valueEdit) { MaterialGraphCodegen::RefreshParams(material);      material.MarkDirty(); }
                UI::EndCollapsingHeader();
                ImGui::Dummy({ 0, 4 });
            }
        }

        // Advanced: raw material-set uniforms for custom shaders. Stock PBR reads the bindless SSBO and
        // declares no Set-1 block, so the pre-scan finds nothing and the section stays hidden; metalness/
        // roughness are surfaced first-class in Base, so they are skipped here.
        if (auto shader = material.GetShader())
        {
            u32 advancedCount = 0;
            for (const auto& [buffName, buffer] : shader->GetBuffers()) {
                if (buffer.Set != 1) continue;
                for (const auto& [name, uniform] : buffer.Uniforms)
                    if (name != "u_Metalness" && name != "u_Roughness") ++advancedCount;
            }

            if (advancedCount > 0 && UI::BeginCollapsingHeader("Advanced"))
            {
                if (UI::BeginProperties()) {
                    for (const auto& [buffName, buffer] : shader->GetBuffers())
                    {
                        if (buffer.Set != 1) continue; // Only edit Material set

                        for (const auto& [name, uniform] : buffer.Uniforms)
                        {
                            if (name == "u_Metalness" || name == "u_Roughness")
                                continue;

                            switch (uniform.Type)
                            {
                                case ShaderDataType::Float: {
                                    float val = material.Get<float>(name);
                                    if (UI::Property(name.c_str(), val)) { material.Set(name, val); material.MarkDirty(); }
                                    break;
                                }
                                case ShaderDataType::Float3: {
                                    Vec3 val = material.Get<Vec3>(name);
                                    if (UI::PropertyColor(name.c_str(), val)) { material.Set(name, val); material.MarkDirty(); }
                                    break;
                                }
                                case ShaderDataType::Float4: {
                                    Vec4 val = material.Get<Vec4>(name);
                                    if (UI::PropertyColor(name.c_str(), val)) { material.Set(name, val); material.MarkDirty(); }
                                    break;
                                }
                                default: break;
                            }
                        }
                    }
                    UI::EndProperties();
                }
                UI::EndCollapsingHeader();
            }
        }

        }
        ImGui::EndChild();

        if (UI::Splitter("##MatSplitter", &footerH, kMinFooterH, kMaxFooterH, kSplitterH))
            Editor::SaveSettings();

        // Pinned 3D preview footer with orbit drag input. Sized to the snapshot so this frame's layout
        // matches Settings sizing; Splitter writeback takes effect next frame.
        if (ImGui::BeginChild("##Preview", { -1, footerH_snap }, false))
        {
            const float pAvailW = ImGui::GetContentRegionAvail().x;
            const float pAvailY = ImGui::GetContentRegionAvail().y;
            const float pSz = std::min(pAvailW, pAvailY);
            if (pSz >= 32.0f) {
                const float xOff = (pAvailW - pSz) * 0.5f;
                const float yOff = (pAvailY - pSz) * 0.5f;
                if (xOff > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + xOff);
                if (yOff > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOff);

                ImGui::InvisibleButton("##OrbitInput", { pSz, pSz });
                if (ImGui::IsItemActive()) {
                    const ImVec2 d = ImGui::GetIO().MouseDelta;
                    const float sens = 0.01f;
                    m_OrbitCam.azimuth   -= d.x * sens;
                    m_OrbitCam.elevation += d.y * sens;
                    const float maxElev = Math::Radians(85.0f);
                    m_OrbitCam.elevation = std::clamp(m_OrbitCam.elevation, -maxElev, maxElev);
                }
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

                auto matPtr = AssetManager::GetAsset<Material>(material.Handle);
                ImTextureID tex = matPtr
                    ? UI::ThumbnailPreviewScene::RenderMaterialInspector(matPtr, m_OrbitCam)
                    : (ImTextureID)0;
                if (tex)
                    ImGui::GetWindowDrawList()->AddImage(tex,
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
        }
        ImGui::EndChild();

        // ---- Auto-save debounce + Undo snapshot ----
        if (material.NeedsSave() && !m_PendingSave)
        {
            // Capture undo snapshot before any changes
            if (!m_HasUndoSnapshot || m_PendingHandle != material.Handle) {
                material.Serialize(m_UndoSnapshot);
                m_HasUndoSnapshot = true;
            }
            m_PendingSave = true;
            m_SaveTimer = 0.0f;
            m_PendingHandle = material.Handle;
        }

        if (m_PendingSave)
        {
            if (m_PendingHandle != material.Handle)
            {
                // Material changed; reset
                m_PendingSave = false;
                m_SaveTimer = 0.0f;
                m_HasUndoSnapshot = false;
            }
            else if (ImGui::IsAnyItemActive())
            {
                // User still interacting; hold the timer
                m_SaveTimer = 0.0f;
            }
            else
            {
                // Per-release thumbnail refresh: justReleased = timer-just-zeroed by the
                // IsAnyItemActive branch on the prior frame. One Invalidate per drag/discrete edit
                // cycle, not per-pixel re-bake spam.
                bool justReleased = (m_SaveTimer == 0.0f);
                m_SaveTimer += Time::UnscaledDeltaTime();
                if (justReleased)
                    UI::ThumbnailCache::Invalidate(material.Handle);

                if (m_SaveTimer >= kAutoSaveDelay)
                {
                    if (m_HasUndoSnapshot) {
                        nlohmann::json newState;
                        material.Serialize(newState);
                        CommandHistory::Execute(std::make_unique<MaterialSnapshotCommand>(
                            material.Handle, m_UndoSnapshot, newState));
                        m_HasUndoSnapshot = false;
                    }

                    SaveMaterial(material);
                    m_PendingSave = false;
                    m_SaveTimer = 0.0f;
                }
            }
        }
    }

    void MaterialEditor::SaveMaterial(Material& material)
    {
        nlohmann::json json;
        material.Serialize(json);

        // Write source .mat file (async). Tell the asset DB this is a self-write so the file watcher
        // doesn't bounce it back as a reimport: that would evict the live material being edited and
        // leave the inspector pointing at a stale instance (edits stop showing live).
        auto sourcePath = AssetDatabase::GetMetadata(material.Handle).Path;
        if (!sourcePath.empty())
        {
            AssetDatabase::SuppressNextReimport(material.Handle);
            std::string jsonStr = json.dump(4);
            std::vector<u8> buf(jsonStr.begin(), jsonStr.end());
            IOThread::WriteFile(sourcePath.string(), std::move(buf));
        }

        // Write binary artifact (async); build blob in-memory
        auto artifactPath = AssetDatabase::GetArtifactPath(material.Handle);
        {
            AssetHeader header;
            header.Type = AssetType::Material;

            std::string jsonStr = json.dump();
            u32 size = static_cast<u32>(jsonStr.size());

            std::vector<u8> blob;
            blob.resize(sizeof(AssetHeader) + sizeof(u32) + size);

            u8* dst = blob.data();
            memcpy(dst, &header, sizeof(AssetHeader));        dst += sizeof(AssetHeader);
            memcpy(dst, &size, sizeof(u32));                  dst += sizeof(u32);
            memcpy(dst, jsonStr.data(), size);

            IOThread::WriteFile(artifactPath.string(), std::move(blob));
        }

        material.ClearNeedsSave();
    }
}
