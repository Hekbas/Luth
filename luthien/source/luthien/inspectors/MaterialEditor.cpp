#include "lepch.h"
#include "luthien/inspectors/MaterialEditor.h"
#include "luthien/UI.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Texture.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/MaterialImporter.h"
#include "luth/core/time/Time.h"
#include "luth/jobs/IOThread.h"
#include "luthien/Command.h"
#include "luthien/CommandHistory.h"

#include <imgui/imgui_internal.h>

namespace Luth
{
    void MaterialEditor::Draw(Material& material)
    {
        // Guard: skip all widget calls when the parent window is clipped/collapsed.
        // Without this, ImGui::BeginChild/EndChild can crash if the host window
        // is not expecting child submissions (e.g. during rapid dock/undock).
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return;

        // Material header with name and unsaved indicator
        if (ImGui::BeginChild("##Header", { 0, 30 })) {
            ImGui::Dummy({ 0, 4 }); ImGui::Dummy({ 4, 0 }); ImGui::SameLine();
            if (material.NeedsSave())
                ImGui::TextColored({ 0.2f, 0.9f, 0.4f, 1.0f }, "%s* (Material)", material.GetName().c_str());
            else
                ImGui::TextColored({ 0.2f, 0.9f, 0.4f, 1.0f }, "%s (Material)", material.GetName().c_str());
        }
        ImGui::EndChild();
        ImGui::Dummy({ 0, 8 });

        // Shader selection
        ImGui::Text("Shader     ");
        ImGui::SameLine();
        {
            auto shader = material.GetShader();
            std::string currentName = shader ? shader->GetName() : "<none>";

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
        }

        ImGui::Dummy({ 0, 4 });

        // Surface Settings
        if (UI::BeginCollapsingHeader("Surface Settings", true))
        {
            // Render mode
            Material::RenderMode currentMode = material.GetRenderMode();
            int modeIndex = static_cast<int>(currentMode);

            ImGui::Text("Render Mode"); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const char* renderModes[] = { "Opaque", "Cutout", "Transparent", "Fade" };
            if (ImGui::Combo("##RenderMode", &modeIndex, renderModes, IM_ARRAYSIZE(renderModes))) {
                material.SetRenderMode(static_cast<Material::RenderMode>(modeIndex));
                material.MarkDirty();
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

            // Face culling
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

        // Surface Inputs
        if (UI::BeginCollapsingHeader("Surface Inputs", true))
        {
            if (ImGui::BeginTable("SurfaceInputsTable", 4, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthFixed, 36.0f);
                ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("UV", ImGuiTableColumnFlags_WidthFixed, 30.0f);

                const auto& textures = material.GetTextures();

                auto DrawSurfaceInput = [&](MapType type, const char* label, std::function<void()> drawControl) {
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

                    // 1. Label column
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    bool enabled = material.IsUseMapEnabled(type);
                    ImGui::Text("%s", label);

                    ImGui::BeginDisabled(!enabled && type != MapType::Diffuse && type != MapType::Metalness && type != MapType::Roughness);
                    
                    // 2. Texture slot
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

                    // 3. Control column
                    ImGui::TableNextColumn();
                    ImGui::PushItemWidth(-1);
                    if (drawControl) drawControl();
                    else ImGui::Dummy(buttonSize);
                    ImGui::PopItemWidth();

                    // 4. UV Channel
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
                };

                DrawSurfaceInput(MapType::Diffuse, "Albedo", [&]() {
                    Vec4 color = material.GetColor();
                    if (ImGui::ColorEdit4("##AlbedoColor", &color.x, ImGuiColorEditFlags_NoInputs)) {
                        material.SetColor(color);
                        material.MarkDirty();
                    }
                });

                DrawSurfaceInput(MapType::Alpha, "Alpha", [&]() {
                    if (material.GetRenderMode() == Material::RenderMode::Cutout) {
                        float cutoff = material.GetAlphaCutoff();
                        if (ImGui::SliderFloat("##AlphaCutoff", &cutoff, 0.0f, 1.0f, "%.2f")) {
                            material.SetAlphaCutoff(cutoff);
                            material.MarkDirty();
                        }
                    } else {
                        ImGui::Dummy({0, 24});
                    }
                });

                DrawSurfaceInput(MapType::Normal, "Normal", nullptr);

                DrawSurfaceInput(MapType::Metalness, "Metallic", [&]() {
                    float met = material.Get<float>("u_Metalness", 0.0f);
                    if (ImGui::SliderFloat("##Met", &met, 0.0f, 1.0f, "%.2f")) {
                        material.Set("u_Metalness", met);
                        material.MarkDirty();
                    }
                });

                DrawSurfaceInput(MapType::Roughness, "Roughness", [&]() {
                    float ro = material.Get<float>("u_Roughness", 0.5f);
                    if (ImGui::SliderFloat("##Rou", &ro, 0.0f, 1.0f, "%.2f")) {
                        material.Set("u_Roughness", ro);
                        material.MarkDirty();
                    }
                });

                DrawSurfaceInput(MapType::Specular, "Specular", nullptr);
                DrawSurfaceInput(MapType::Occlusion, "Occlusion", nullptr);
                DrawSurfaceInput(MapType::Emissive, "Emissive", [&]() {
                    Vec3 emColor = material.Get<Vec3>("u_EmissiveColor", Vec3(0.0f));
                    if (ImGui::ColorEdit3("##EmissiveColor", &emColor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_NoInputs)) {
                        material.Set("u_EmissiveColor", emColor);
                        material.MarkDirty();
                    }
                });

                DrawSurfaceInput(MapType::Thickness, "Thickness", nullptr);

                ImGui::EndTable();
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 4 });

        // Dynamic Uniform Editor (Properties)
        if (auto shader = material.GetShader())
        {
            if (UI::BeginCollapsingHeader("Properties", true))
            {
                if (UI::BeginProperties()) {
                    for (const auto& [buffName, buffer] : shader->GetBuffers())
                    {
                        if (buffer.Set != 1) continue; // Only edit Material set

                        for (const auto& [name, uniform] : buffer.Uniforms)
                        {
                            // Skip uniforms already displayed in Surface Inputs
                            if (name == "u_Metalness" || name == "u_Roughness" || name == "u_EmissiveColor")
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

        // --- Auto-save debounce + Undo snapshot ---
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
                // Material changed — reset
                m_PendingSave = false;
                m_SaveTimer = 0.0f;
                m_HasUndoSnapshot = false;
            }
            else if (ImGui::IsAnyItemActive())
            {
                // User still interacting — hold the timer
                m_SaveTimer = 0.0f;
            }
            else
            {
                m_SaveTimer += Time::UnscaledDeltaTime();
                if (m_SaveTimer >= kAutoSaveDelay)
                {
                    // Push undo command with old/new state
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

        // Write source .mat file (async)
        auto sourcePath = AssetDatabase::GetMetadata(material.Handle).Path;
        if (!sourcePath.empty())
        {
            std::string jsonStr = json.dump(4);
            std::vector<u8> buf(jsonStr.begin(), jsonStr.end());
            IOThread::WriteFile(sourcePath.string(), std::move(buf));
        }

        // Write binary artifact (async) — build blob in-memory
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
