#include "lepch.h"
#include "luthien/inspectors/ModelViewer.h"
#include "luthien/Editor.h"
#include "luthien/EditorSettings.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"
#include "luthien/widgets/Icons.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/MetaFile.h"

#include <algorithm>

namespace Luth
{
    void ModelViewer::Draw(Model& model)
    {
        // Header: thumbnail-on-left, name + summary on right.
        // Interactive 3D preview pinned in the footer.
        ImTextureID headerThumb = UI::ThumbnailCache::Get(model.Handle, AssetType::Model);
        const auto& headerInfo = model.GetCachedModelInfo();
        UI::InspectorHeader(headerThumb, ICON_CUBE, 48.0f, [&]() {
            const ImVec4 nameCol = { 0.4f, 0.8f, 1.0f, 1.0f };
            ImGui::TextColored(nameCol, "%s (Model)", model.GetName().c_str());
            ImGui::TextDisabled("%d meshes  ·  %d verts  ·  %s",
                headerInfo.TotalMeshCount,
                headerInfo.TotalVertexCount,
                headerInfo.IsSkinned ? "skinned" : "static");
        });

        ImGui::Dummy({ 0, 4 });

        // Pinned-footer layout, snapshot pattern: Settings + Preview size with the same frame-start
        // value; Splitter mutates the persisted height so the change takes effect next frame (no
        // one-frame overshoot).
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
                UI::Property("Mark Deformable", m_Settings.MarkDeformable);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Route this model's STATIC meshes through the GPU deform seam so global\nwind bends them, RT-correct. Ignored for skinned. Needs Apply (reimport).");

                ImGui::Separator();
                ImGui::TextDisabled("Skinning");

                int meshTransformInt = static_cast<int>(m_Settings.SkinMeshTransform);
                if (UI::PropertyCombo("Mesh Transform", meshTransformInt, meshTransformModes, IM_ARRAYSIZE(meshTransformModes)))
                    m_Settings.SkinMeshTransform = static_cast<ModelImportSettings::MeshTransformMode>(meshTransformInt);

                ImGui::Separator();
                ImGui::TextDisabled("Physics");

                static const char* physicsBakeModes[] = { "None", "Auto" };
                int physicsBakeInt = static_cast<int>(m_Settings.PhysicsBake);
                if (UI::PropertyCombo("Bake Mode", physicsBakeInt, physicsBakeModes, IM_ARRAYSIZE(physicsBakeModes)))
                    m_Settings.PhysicsBake = static_cast<ModelImportSettings::PhysicsBakeMode>(physicsBakeInt);

                ImGui::Separator();
                ImGui::TextDisabled("Scene (static models)");

                UI::Property("Import Cameras", m_Settings.ImportCameras);
                UI::Property("Import Lights", m_Settings.ImportLights);

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

        const auto& info = model.GetCachedModelInfo();

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

        if (info.IsSkinned) {
            if (UI::BeginCollapsingHeader("Bones")) {
                ImGui::Text("Total Bones: %d", info.BoneCount);

                if (ImGui::TreeNode("Bone Hierarchy")) {
                    std::function<void(int)> DisplayBoneNode = [&](int index) {
                        const auto& node = info.BoneHierarchy[index];
                        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
                        if (node.BoneIndex == -1) flags |= ImGuiTreeNodeFlags_Leaf;

                        bool isOpen = ImGui::TreeNodeEx(node.Name.c_str(), flags);

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
        ImGui::EndChild();

        if (UI::Splitter("##ModelSplitter", &footerH, kMinFooterH, kMaxFooterH, kSplitterH))
            Editor::SaveSettings();

        // Pinned 3D preview footer with orbit drag input. Snapshot-sized.
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

                auto modelPtr = AssetManager::GetAsset<Model>(model.Handle);
                ImTextureID tex = modelPtr
                    ? UI::ThumbnailPreviewScene::RenderMeshInspector(modelPtr, m_OrbitCam)
                    : (ImTextureID)0;
                if (tex)
                    ImGui::GetWindowDrawList()->AddImage(tex,
                        ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
        }
        ImGui::EndChild();
    }
}
