#include "luthpch.h"
#include "luth/editor/inspectors/MaterialEditor.h"
#include "luth/editor/UI.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/ShaderLibrary.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetSerializer.h"
#include "luth/resources/importers/MaterialImporter.h"

namespace Luth
{
    void MaterialEditor::Draw(Material& material)
    {
        // Material header with name, dirty indicator, and Save button
        if (ImGui::BeginChild("##Header", { 0, 30 })) {
            ImGui::Dummy({ 0, 4 }); ImGui::Dummy({ 4, 0 }); ImGui::SameLine();
            if (material.IsDirty())
                ImGui::TextColored({ 0.2f, 0.9f, 0.4f, 1.0f }, "%s* (Material)", material.GetName().c_str());
            else
                ImGui::TextColored({ 0.2f, 0.9f, 0.4f, 1.0f }, "%s (Material)", material.GetName().c_str());

            if (material.IsDirty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Save")) {
                    nlohmann::json json;
                    material.Serialize(json);

                    // Write source .mat file
                    auto sourcePath = AssetDatabase::GetMetadata(material.Handle).Path;
                    if (!sourcePath.empty())
                    {
                        std::ofstream file(sourcePath);
                        file << json.dump(4);
                    }

                    // Write binary artifact
                    MaterialAssetData data;
                    data.JsonData = json;
                    auto artifactPath = AssetDatabase::GetArtifactPath(material.Handle);
                    AssetSerializer::SerializeMaterial(artifactPath, data);

                    material.ClearDirty();
                }
            }
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

        // Albedo color picker
        {
            UI::BeginProperties();
            glm::vec4 color = material.GetColor();
            if (UI::PropertyColor("Albedo Color", color)) {
                material.SetColor(color);
                material.MarkDirty();
            }
            UI::EndProperties();
        }

        ImGui::Dummy({ 0, 4 });

        // Dynamic Uniform Editor
        if (auto shader = material.GetShader())
        {
            if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen))
            {
                UI::BeginProperties();
                for (const auto& [buffName, buffer] : shader->GetBuffers())
                {
                    if (buffer.Set != 1) continue; // Only edit Material set

                    for (const auto& [name, uniform] : buffer.Uniforms)
                    {
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
        }

        // Render Settings
        if (ImGui::CollapsingHeader("Render Settings", ImGuiTreeNodeFlags_DefaultOpen))
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

            if (material.GetRenderMode() == Material::RenderMode::Cutout) {
                float cutoff = material.GetAlphaCutoff();
                ImGui::Text("Alpha Cutoff"); ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                if (ImGui::SliderFloat("##Alpha Cutoff", &cutoff, 0.0f, 1.0f)) {
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

            // Face culling
            int cullIndex = static_cast<int>(material.GetCullMode());
            ImGui::Text("Face Cull  "); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            const char* cullModes[] = { "Back", "Front", "None" };
            if (ImGui::Combo("##CullMode", &cullIndex, cullModes, IM_ARRAYSIZE(cullModes))) {
                material.SetCullMode(static_cast<Material::CullMode>(cullIndex));
                material.MarkDirty();
            }
        }

        ImGui::Dummy({ 0, 4 });

        // Texture properties with collapsable headers
        const auto& textures = material.GetTextures();

        auto DrawTextureProperty = [&](MapType type, const char* label) {
            std::shared_ptr<Texture> texture;
            bool hasTexture = false;
            UUID textureUUID = UUID::Invalid();
            u32 uvIndex = 0;

            for (const auto& texInfo : textures) {
                if (texInfo.type == type) {
                    textureUUID = texInfo.Uuid;
                    uvIndex = texInfo.uvIndex;
                    // Try load if needed for preview
                    if (texInfo.Uuid.IsValid() && !AssetManager::IsLoaded(texInfo.Uuid) && !AssetManager::IsLoading(texInfo.Uuid))
                        AssetManager::LoadAsync(texInfo.Uuid);

                    if (texture = AssetManager::GetAsset<Texture>(texInfo.Uuid)) {
                        hasTexture = true;
                        break;
                    }
                }
            }

            // Header setup
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Framed |
                ImGuiTreeNodeFlags_AllowItemOverlap |
                ImGuiTreeNodeFlags_NoTreePushOnOpen |
                ImGuiTreeNodeFlags_DefaultOpen;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
            bool headerOpen = ImGui::CollapsingHeader(label, flags);

            // Checkbox control
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 12);
            std::string toggleId = "##Toggle_" + std::string(label);
            bool enabled = material.IsUseMapEnabled(type);
            if (ImGui::Checkbox(toggleId.c_str(), &enabled)) {
                material.EnableUseMap(type, enabled);
            }

            ImGui::PopStyleVar();

            if (headerOpen) {
                ImGui::BeginDisabled(!enabled);
                ImGui::Indent();

                // Texture slot with drag-drop support
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                std::string textureId = "##Texture_" + std::string(label);
                if (hasTexture) {
                    ImGui::ImageButton(textureId.c_str(), UI::GetTextureID(texture), { 32, 32 }, { 0, 1 }, { 1, 0 });
                }
                else if (textureUUID.IsValid() && AssetManager::IsLoading(textureUUID)) {
                    ImGui::Button("...", { 32, 32 }); // Loading placeholder
                }
                else {
                    ImGui::Button(textureId.c_str(), { 32, 32 });
                }
                ImGui::PopStyleVar();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID")) {
                        const UUID* droppedUUID = static_cast<const UUID*>(payload->Data);
                        material.SetTexture({ *droppedUUID, type, uvIndex });
                        material.EnableUseTexture(type, true);
                        material.MarkDirty();
                    }
                    ImGui::EndDragDropTarget();
                }

                // [SUPR] Handle texture deletion
                if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                    material.SetTexture({ UUID::Invalid(), type, 0 });
                    material.EnableUseTexture(type, false);
                    material.MarkDirty();
                }

                // Texture properties
                if (hasTexture) {
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    ImGui::Text("%s", texture->GetName().c_str());
                    ImGui::Text("%dx%d", texture->GetWidth(), texture->GetHeight());
                    ImGui::EndGroup();
                }

                // UV index selector
                {
                    int uv = static_cast<int>(uvIndex);
                    std::string uvId = "##UV_" + std::string(label);
                    ImGui::Text("UV Channel"); ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);
                    if (ImGui::InputInt(uvId.c_str(), &uv, 1, 1)) {
                        if (uv < 0) uv = 0;
                        if (uv > 3) uv = 3;
                        material.SetTexture({ textureUUID, type, static_cast<u32>(uv) });
                        material.MarkDirty();
                    }
                }

                ImGui::Unindent();
                ImGui::EndDisabled();
            }
            ImGui::Spacing();
        };

        DrawTextureProperty(MapType::Diffuse, "Albedo");
        DrawTextureProperty(MapType::Alpha, "Alpha");
        DrawTextureProperty(MapType::Normal, "Normal");
        DrawTextureProperty(MapType::Metalness, "Metallic");
        DrawTextureProperty(MapType::Roughness, "Roughness");
        DrawTextureProperty(MapType::Specular, "Specular");
        DrawTextureProperty(MapType::Occlusion, "Occlusion");
        DrawTextureProperty(MapType::Emissive, "Emissive");
        DrawTextureProperty(MapType::Thickness, "Thickness");
    }
}
