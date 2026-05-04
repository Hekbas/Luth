#include "lepch.h"
#include "luthien/inspectors/ModelViewer.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/MetaFile.h"

namespace Luth
{
    void ModelViewer::Draw(Model& model)
    {
        // Model header with name and type
        if (ImGui::BeginChild("##Header", { 0, 30 })) {
			ImGui::Dummy({ 0, 4 }); ImGui::Dummy({ 4, 0 }); ImGui::SameLine();
            ImGui::TextColored({ 0.4f, 0.8f, 1.0f, 1.0f }, "%s (Model)", model.GetName().c_str());
        }
        ImGui::EndChild();
        ImGui::Dummy({ 0, 4 });

        // Live preview via ThumbnailCache. Cascade refreshes on Apply re-import.
        constexpr float kPreviewSize = 192.0f;
        if (ImGui::BeginChild("##Preview", { 0, kPreviewSize + 8 }, false))
        {
            ImTextureID thumb = UI::ThumbnailCache::Get(model.Handle, AssetType::Model);
            float availW = ImGui::GetContentRegionAvail().x;
            float xOff = (availW - kPreviewSize) * 0.5f;
            if (xOff > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + xOff);
            if (thumb) {
                ImGui::Image(thumb, { kPreviewSize, kPreviewSize });
            } else {
                ImGui::Dummy({ kPreviewSize, kPreviewSize * 0.4f });
                if (xOff > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + xOff);
                ImGui::TextDisabled("%s", ICON_FA_CUBE);
            }
        }
        ImGui::EndChild();
        ImGui::Dummy({ 0, 4 });

        // Per-model state: reset when selected model changes
        if (model.Handle != m_LastModelUUID)
        {
            m_LastModelUUID = model.Handle;
            m_Settings = ModelImportSettings{};

            auto modelPath = AssetDatabase::GetMetadata(model.Handle).Path;
            if (!modelPath.empty())
            {
                fs::path metaPath = modelPath.string() + ".meta";
                MetaFile meta(model.Handle);
                if (meta.Load(metaPath))
                    m_Settings = ModelImportSettings::FromJson(meta.GetTypeSettings());
            }
        }

        // Import Settings
        if (UI::BeginCollapsingHeader("Import Settings", true))
        {
            const char* upAxes[] = { "Auto", "X-Up", "Y-Up", "Z-Up" };
            const char* meshTransformModes[] = { "Auto", "Bake", "Identity (Legacy)" };

            if (UI::BeginProperties("ModelImport")) {
                UI::Property("Scale Factor", m_Settings.ScaleFactor, 0.01f, 0.001f, 1000.0f);

                // Up axis: offset by 1 for the "Auto" entry (-1 maps to index 0)
                int upAxisUI = m_Settings.UpAxis + 1;
                if (UI::PropertyCombo("Up Axis", upAxisUI, upAxes, IM_ARRAYSIZE(upAxes)))
                    m_Settings.UpAxis = upAxisUI - 1;

                UI::Property("Import Normals", m_Settings.ImportNormals);
                UI::Property("Import Tangents", m_Settings.ImportTangents);
                UI::Property("Optimize Mesh", m_Settings.OptimizeMesh);

                ImGui::Separator();
                ImGui::TextDisabled("Skinning");

                int meshTransformInt = static_cast<int>(m_Settings.SkinMeshTransform);
                if (UI::PropertyCombo("Mesh Transform", meshTransformInt, meshTransformModes, IM_ARRAYSIZE(meshTransformModes)))
                    m_Settings.SkinMeshTransform = static_cast<ModelImportSettings::MeshTransformMode>(meshTransformInt);

                UI::EndProperties();
            }

            ImGui::Dummy({ 0, 4 });

            float buttonWidth = ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - buttonWidth);
            if (ImGui::Button("Apply##ModelImport")) {
                auto modelPath = AssetDatabase::GetMetadata(model.Handle).Path;
                fs::path metaPath = modelPath.string() + ".meta";
                MetaFile meta(model.Handle);
                if (meta.Load(metaPath))
                {
                    meta.GetTypeSettings() = m_Settings.ToJson();
                    meta.Save(metaPath);

                    // Force reimport
                    fs::path artifactPath = AssetDatabase::GetArtifactPath(model.Handle);
                    if (fs::exists(artifactPath))
                        fs::remove(artifactPath);

                    AssetManager::Import(model.Handle);
                    AssetManager::Evict(model.Handle);
                    m_LastModelUUID = UUID::Invalid();
                }
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 4 });

        // Get cached model info
        const auto& info = model.GetCachedModelInfo();

        // Basic model info section
        if (UI::BeginCollapsingHeader("Model Info", true)) {
            if (UI::BeginInfoTable("ModelProps")) {
                UI::InfoRow("Meshes",    "%d", info.TotalMeshCount);
                UI::InfoRow("Vertices",  "%d", info.TotalVertexCount);
                UI::InfoRow("Indices",   "%d", info.TotalIndexCount);
                UI::InfoRow("Materials", "%d", info.MaterialCount);
                UI::InfoRow("Skinned",   "%s", info.IsSkinned ? "Yes" : "No");
                UI::EndInfoTable();
            }
            UI::EndCollapsingHeader();
        }
        ImGui::Dummy({ 0, 4 });

        // Meshes section
        if (UI::BeginCollapsingHeader("Meshes")) {
            if (ImGui::BeginTable("MeshesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Vertices");
                ImGui::TableSetupColumn("Indices");
                ImGui::TableSetupColumn("Material");
                ImGui::TableHeadersRow();

                for (const auto& mesh : info.Meshes) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", mesh.Name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", mesh.VertexCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", mesh.IndexCount);
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", mesh.MaterialIndex);
                }

                ImGui::EndTable();
            }
            UI::EndCollapsingHeader();
        }
        ImGui::Dummy({ 0, 4 });

        // Skinned model specific sections
        if (info.IsSkinned) {
            // Bones section
            if (UI::BeginCollapsingHeader("Bones")) {
                ImGui::Text("Total Bones: %d", info.BoneCount);

                if (ImGui::TreeNode("Bone Hierarchy")) {
                    // Recursive function to display bone hierarchy
                    std::function<void(int)> DisplayBoneNode = [&](int index) {
                        const auto& node = info.BoneHierarchy[index];
                        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
                        if (node.BoneIndex == -1) flags |= ImGuiTreeNodeFlags_Leaf;

                        bool isOpen = ImGui::TreeNodeEx(node.Name.c_str(), flags);

                        // Tooltip for bone info
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::Text("Bone Index: %d", node.BoneIndex);
                            ImGui::Text("Parent Index: %d", node.ParentIndex);
                            ImGui::EndTooltip();
                        }

                        if (isOpen) {
                            for (int childIndex = 0; childIndex < (int)info.BoneHierarchy.size(); ++childIndex) {
                                if (info.BoneHierarchy[childIndex].ParentIndex == index) {
                                    DisplayBoneNode(childIndex);
                                }
                            }
                            ImGui::TreePop();
                        }
                    };

                    // Find root nodes (parentIndex == -1)
                    for (int i = 0; i < (int)info.BoneHierarchy.size(); ++i) {
                        if (info.BoneHierarchy[i].ParentIndex == -1) {
                            DisplayBoneNode(i);
                        }
                    }

                    ImGui::TreePop();
                }
                UI::EndCollapsingHeader();
            }
            ImGui::Dummy({ 0, 4 });

            // Animations section
            if (UI::BeginCollapsingHeader("Animations")) {
                ImGui::Text("Total Animations: %d", info.AnimationCount);

                const auto& clipUUIDs = model.GetAnimationClipUUIDs();

                if (ImGui::BeginTable("AnimationsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Duration (ticks)");
                    ImGui::TableSetupColumn("Duration (s)");
                    ImGui::TableSetupColumn("TPS");
                    ImGui::TableSetupColumn("Events");
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < info.Animations.size(); i++) {
                        const auto& anim = info.Animations[i];
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", anim.Name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", anim.Duration);
                        ImGui::TableNextColumn();
                        std::shared_ptr<AnimationClip> clipPtr;
                        if (i < clipUUIDs.size())
                            clipPtr = AssetManager::GetAsset<AnimationClip>(clipUUIDs[i]);
                        if (clipPtr)
                            ImGui::Text("%.2f", clipPtr->GetDurationSeconds());
                        else
                            ImGui::Text("-");
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", anim.TicksPerSecond);
                        ImGui::TableNextColumn();
                        if (clipPtr)
                            ImGui::Text("%d", (int)clipPtr->Events.size());
                        else
                            ImGui::Text("-");
                    }

                    ImGui::EndTable();
                }
                UI::EndCollapsingHeader();
            }
        }
    }
}
