#include "luthpch.h"
#include "luth/editor/inspectors/ModelViewer.h"
#include "luth/editor/UI.h"
#include "luth/renderer/Model.h"
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
        ImGui::Dummy({ 0, 8 });

        // Per-model state: reset when selected model changes
        if (model.Handle != m_LastModelUUID)
        {
            m_LastModelUUID = model.Handle;
            m_ScaleFactor = 1.0f;
            m_UpAxis = 1;

            auto modelPath = AssetDatabase::GetMetadata(model.Handle).Path;
            if (!modelPath.empty())
            {
                fs::path metaPath = modelPath.string() + ".meta";
                MetaFile meta(model.Handle);
                if (meta.Load(metaPath))
                {
                    auto& ts = meta.GetTypeSettings();
                    if (ts.contains("scale_factor")) m_ScaleFactor = ts["scale_factor"].get<float>();
                    if (ts.contains("up_axis"))      m_UpAxis = ts["up_axis"].get<int>();
                }
            }
        }

        // Import Settings
        if (UI::BeginCollapsingHeader("Import Settings", true))
        {
            const char* upAxes[] = { "X-Up", "Y-Up", "Z-Up" };

            if (UI::BeginProperties("ModelImport")) {
                UI::Property("Scale Factor", m_ScaleFactor, 0.01f, 0.001f, 1000.0f);
                UI::PropertyCombo("Up Axis", m_UpAxis, upAxes, IM_ARRAYSIZE(upAxes));
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
                    auto& ts = meta.GetTypeSettings();
                    ts["scale_factor"] = m_ScaleFactor;
                    ts["up_axis"] = m_UpAxis;
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

                if (ImGui::BeginTable("AnimationsTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Name");
                    ImGui::TableSetupColumn("Duration");
                    ImGui::TableSetupColumn("TPS");
                    ImGui::TableHeadersRow();

                    for (const auto& anim : info.Animations) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", anim.Name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", anim.Duration);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", anim.TicksPerSecond);
                    }

                    ImGui::EndTable();
                }
                UI::EndCollapsingHeader();
            }
        }
    }
}
