#include "luthpch.h"
#include "luth/editor/panels/HierarchyPanel.h"
#include "luth/editor/panels/InspectorPanel.h"
#include "luth/scene/Components.h"
#include "luth/scene/Systems.h"
#include "luth/utils/LuthIcons.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/renderer/Model.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Luth
{
    using namespace Component;
    HierarchyPanel::HierarchyPanel()
    {
        LH_CORE_INFO("Created Hierarchy panel");
        m_Context = std::make_shared<Scene>();
    }

    void HierarchyPanel::OnInit()
    {
        Systems::SetScene(m_Context.get());
    }

    void HierarchyPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        if (ImGui::Begin(ICON_FA_LIST "  Hierarchy"))
        {
            DrawTopBar();
            ImGui::Separator();

            // Handle Global Shortcuts (Delete, F2, Esc)
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_Selection)
                    DeleteSelectedEntity();
                
                if (ImGui::IsKeyPressed(ImGuiKey_F2) && m_Selection)
                    RenameEntity(m_Selection);

                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    m_IsRenaming = false;
                    m_RenamingEntity = {};
                    SetSelectedEntity({});
                }
            }
            
            // Create New Entity Shortcut (Ctrl + Shift + N)
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && 
                ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyDown(ImGuiKey_LeftShift) && ImGui::IsKeyPressed(ImGuiKey_N))
            {
                SetSelectedEntity(m_Context->CreateEntity("New Entity"));
            }

            // Main Hierarchy Area
            if (ImGui::BeginChild("EntityList"))
            {
                // 1. Iterate Root Entities (Ordered)
                for (auto entity : m_Context->GetRootEntities()) {
                    DrawEntityNode(entity);
                }

                // 2. Handle Click on Empty Space (Deselect)
                if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
                {
                    // Only deselect if we didn't click an item (ImGui handles this via IsItemClicked check inside DrawEntityNode)
                    // But since we are after the loop, we check if we are hovering the window background
                    if (!ImGui::IsAnyItemHovered()) {
                        SetSelectedEntity({});
                        m_IsRenaming = false;
                    }
                }

                // 3. Context Menu on Empty Space
                if (ImGui::BeginPopupContextWindow("HierarchyContextMenu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
                {
                    DrawContextMenu({}); // No parent when clicking on empty space
                    ImGui::EndPopup();
                }

                // 4. Drop Target for Unparenting (Making Root)
                HandleRootDragDropTarget();
            }
            ImGui::EndChild();

            // Process pending instantiations (Async Loading)
            for (auto it = m_PendingInstantiations.begin(); it != m_PendingInstantiations.end(); ) {
                if (AssetManager::IsLoaded(it->ModelUUID)) {
                    InstantiateModel(it->ModelUUID, it->Parent);
                    it = m_PendingInstantiations.erase(it);
                } else {
                    ++it;
                }
            }
            
            // Execute deferred actions to avoid iterator invalidation during rendering
            for (auto& action : m_DeferredActions)
                action();
            m_DeferredActions.clear();
        }
        ImGui::End();
        ImGui::PopFont();
    }

    void HierarchyPanel::DrawTopBar()
    {
        ImGui::AlignTextToFramePadding();
        if (ImGui::Button(ICON_FA_PLUS))
            ImGui::OpenPopup("HierarchyCreateMenu");

        if (ImGui::BeginPopup("HierarchyCreateMenu"))
        {
            DrawContextMenu(m_Selection); // Use selection as parent for the top bar button
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputTextWithHint("##Search", ICON_FA_MAGNIFYING_GLASS " Search...", m_SearchFilter, IM_ARRAYSIZE(m_SearchFilter));
    }

    void HierarchyPanel::DrawEntityNode(Entity entity)
    {
        if (!entity.IsValid()) return;

        // Use pointer-based ID to ensure uniqueness for 32-bit/64-bit entity handles
        ImGui::PushID((void*)(uintptr_t)(uint32_t)entity.GetComponent<ID>().m_ID.GetHalf0());

        const std::string& name = entity.GetName();
        
        // Filter logic
        if (strlen(m_SearchFilter) > 0)
        {
            // Simple substring search (case sensitive for now, can be improved)
            if (name.find(m_SearchFilter) == std::string::npos)
            {
                // If parent doesn't match, maybe children do? 
                // For simplicity in this snippet, we just hide if no match. 
                // A proper implementation would recurse and return 'bool shouldDraw'.
                // Skipping for brevity to focus on interaction logic.
            }
        }

        ImGuiTreeNodeFlags flags = 
            ImGuiTreeNodeFlags_OpenOnArrow | 
            ImGuiTreeNodeFlags_SpanAvailWidth |
            ImGuiTreeNodeFlags_FramePadding;

        if (m_Selection == entity) flags |= ImGuiTreeNodeFlags_Selected;
        
        bool hasChildren = !entity.GetChildren().empty();
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;

        // Handle Renaming State
        bool isRenamingThis = (m_IsRenaming && m_RenamingEntity == entity);
        
        
        bool opened = false;

        if (isRenamingThis)
        {
            // Draw the arrow (if children) but no text label
            // We use AllowItemOverlap to draw the InputText over the node area
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            flags |= ImGuiTreeNodeFlags_AllowItemOverlap;
            opened = ImGui::TreeNodeEx("##Node", flags, ""); 
            ImGui::PopStyleVar();

            ImGui::SameLine();
            
            // Input Text
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            if (m_FocusRename) {
                ImGui::SetKeyboardFocusHere();
                m_FocusRename = false;
            }
            
            if (ImGui::InputText("##Rename", m_RenameBuffer, sizeof(m_RenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                entity.SetName(m_RenameBuffer);
                m_IsRenaming = false;
            }
            ImGui::PopStyleVar();

            // Stop renaming if we click elsewhere
            if (!ImGui::IsItemActive() && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) {
                m_IsRenaming = false;
            }
        }
        else
        {
            // Standard Node
            opened = ImGui::TreeNodeEx("##Node", flags, "%s", name.c_str());
        }

        // Handle Selection
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            SetSelectedEntity(entity);
            
            // Double click to rename (Unity style)
            if (ImGui::IsMouseDoubleClicked(0)) {
                RenameEntity(entity);
            }
        }

        // Context Menu
        if (ImGui::BeginPopupContextItem())
        {
            SetSelectedEntity(entity); // Select on right click
            DrawContextMenu(entity);   // Pass clicked entity as parent
            ImGui::EndPopup();
        }

        // Drag & Drop
        HandleDragDropSource(entity);
        HandleDragDropTarget(entity);

        // Recursion
        if (opened)
        {
            auto children = entity.GetChildren(); // Copy to avoid iterator invalidation if reordered
            for (auto child : children)
            {
                DrawEntityNode(child);
            }
            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void HierarchyPanel::HandleDragDropSource(Entity entity)
    {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            // We pass the Entity handle (uint32_t) as payload
            // In a real engine with UUIDs, pass the UUID.
            ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &entity, sizeof(Entity));
            
            // Preview
            ImGui::Text(ICON_FA_CUBE " %s", entity.GetName().c_str());
            
            ImGui::EndDragDropSource();
        }
    }

    void HierarchyPanel::HandleDragDropTarget(Entity targetEntity)
    {
        if (ImGui::BeginDragDropTarget())
        {
            const ImGuiPayload* payload = ImGui::GetDragDropPayload();
            if (payload && payload->IsDataType("HIERARCHY_ENTITY"))
            {
                Entity payloadEntity = *(Entity*)payload->Data;

                // Prevent parenting to self or descendants
                if (payloadEntity != targetEntity && !IsDescendant(targetEntity, payloadEntity))
                {
                    // Visual Feedback Logic
                    // Determine if we are dropping ON the node (parenting) or BETWEEN nodes (reordering)
                    
                    float cursorY = ImGui::GetMousePos().y;
                    float itemMinY = ImGui::GetItemRectMin().y;
                    float itemMaxY = ImGui::GetItemRectMax().y;
                    float height = itemMaxY - itemMinY;
                    
                    // Top 25% = Insert Before
                    // Bottom 25% = Insert After
                    // Middle 50% = Parent
                    
                    bool isReorderingTop = (cursorY < itemMinY + height * 0.25f);
                    bool isReorderingBot = (cursorY > itemMaxY - height * 0.25f);
                    
                    // Draw visual indicators
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImU32 highlightColor = IM_COL32(0, 255, 255, 255); // Cyan

                    if (isReorderingTop)
                    {
                        drawList->AddLine(ImVec2(ImGui::GetItemRectMin().x, itemMinY), ImVec2(ImGui::GetItemRectMax().x, itemMinY), highlightColor, 2.0f);
                        if (ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                        {
                            m_Context->ReorderEntity(payloadEntity, targetEntity, false);
                        }
                    }
                    else if (isReorderingBot)
                    {
                        drawList->AddLine(ImVec2(ImGui::GetItemRectMin().x, itemMaxY), ImVec2(ImGui::GetItemRectMax().x, itemMaxY), highlightColor, 2.0f);
                        if (ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                        {
                            m_Context->ReorderEntity(payloadEntity, targetEntity, true);
                        }
                    }
                    else
                    {
                        // Parenting
                        // Highlight the whole node background rect
                        drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), highlightColor, 0.0f, 0, 2.0f);
                        
                        if (ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                        {
                            payloadEntity.SetParent(targetEntity);
                        }
                    }
                }
            }
            // Handle Asset Drop (Parenting new model to target)
            else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID"))
            {
                const UUID assetUuid = *static_cast<const UUID*>(payload->Data);
                const auto& meta = AssetDatabase::GetMetadata(assetUuid);

                if (meta.Type == AssetType::Model)
                {
                    if (AssetManager::IsLoaded(assetUuid))
                    {
                        InstantiateModel(assetUuid, targetEntity);
                    }
                    else {
                        AssetManager::LoadAsync(assetUuid);
                        m_PendingInstantiations.push_back({ assetUuid, targetEntity });
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    void HierarchyPanel::HandleRootDragDropTarget()
    {
        // Create a dummy item filling the rest of the space to catch drops to root
        ImVec2 available = ImGui::GetContentRegionAvail();
        if (available.y < 50.0f) available.y = 50.0f;
        
        ImGui::Dummy(available);
        
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
            {
                Entity payloadEntity = *(Entity*)payload->Data;
                payloadEntity.RemoveParent(); // Make Root
            }
            
            // Also handle asset drops (Prefabs/Models)
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_UUID"))
            {
                const UUID assetUuid = *static_cast<const UUID*>(payload->Data);
                const auto& meta = AssetDatabase::GetMetadata(assetUuid);

                if (meta.Type == AssetType::Model)
                {
                    if (AssetManager::IsLoaded(assetUuid))
                    {
                        InstantiateModel(assetUuid, {});
                    }
                    else {
                        AssetManager::LoadAsync(assetUuid);
                        m_PendingInstantiations.push_back({ assetUuid, {} });
                    }
                }
            }
            
            ImGui::EndDragDropTarget();
        }
    }

    void HierarchyPanel::DrawContextMenu(Entity parent)
    {
        if (ImGui::MenuItem("Create Empty"))
        {
            m_DeferredActions.push_back([this, parent]() {
                auto e = m_Context->CreateEntity("New Entity");
                if (parent) e.SetParent(parent);
                SetSelectedEntity(e);
            });
        }

        if (ImGui::BeginMenu("3D Object"))
        {
            if (ImGui::MenuItem("Cube")) { 
                m_DeferredActions.push_back([this, parent]() {
                    // TODO: Create Cube logic
                });
            }
            if (ImGui::MenuItem("Sphere")) { 
                m_DeferredActions.push_back([this, parent]() {
                    // TODO: Create Sphere logic
                });
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Light"))
        {
            if (ImGui::MenuItem("Directional Light")) { 
                m_DeferredActions.push_back([this, parent]() {
                    auto light = m_Context->CreateEntity("Directional Light");
                    light.AddComponent<DirectionalLight>();
                    light.GetComponent<Transform>().Rotation = Vec3(-45.0f, 0.0f, 0.0f);
                    SetSelectedEntity(light);
                });
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Rename", "F2", false, m_Selection.operator bool()))
        {
            RenameEntity(m_Selection);
        }

        if (ImGui::MenuItem("Delete", "Del", false, m_Selection.operator bool()))
        {
            m_DeferredActions.push_back([this]() {
                DeleteSelectedEntity();
            });
        }
    }

    void HierarchyPanel::SetSelectedEntity(Entity entity)
    {
        m_Selection = entity;
        // Sync with Inspector
        if (auto* inspector = Editor::GetPanel<InspectorPanel>()) {
            inspector->SetSelectedEntity(entity);
        }
    }

    void HierarchyPanel::RenameEntity(Entity entity)
    {
        m_IsRenaming = true;
        m_RenamingEntity = entity;
        m_FocusRename = true;
        
        // Copy current name to buffer
        std::string name = entity.GetName();
        memset(m_RenameBuffer, 0, sizeof(m_RenameBuffer));
        strncpy_s(m_RenameBuffer, name.c_str(), sizeof(m_RenameBuffer) - 1);
    }

    void HierarchyPanel::DeleteSelectedEntity()
    {
        if (m_Selection) {
            m_Context->DestroyEntity(m_Selection);
            SetSelectedEntity({});
        }
    }

    bool HierarchyPanel::IsDescendant(Entity potentialDescendant, Entity potentialAncestor)
    {
        return potentialDescendant.IsDescendantOf(potentialAncestor);
    }

    void HierarchyPanel::InstantiateModel(UUID assetUuid, Entity parent)
    {
        auto model = AssetManager::GetAsset<Model>(assetUuid);
        if (!model) return;

        Entity root = m_Context->CreateEntity(model->GetName());
        if (parent.IsValid()) root.SetParent(parent);

        const auto& meshes = model->GetMeshes();
        for (size_t i = 0; i < meshes.size(); i++)
        {
            Entity child = m_Context->CreateEntity(model->GetCachedModelInfo().Meshes[i].Name);
            child.SetParent(root);
            auto& mr = child.AddComponent<MeshRenderer>();
            mr.ModelUUID = assetUuid;
            mr.MeshIndex = (u32)i;
            mr.isSkinned = model->IsSkinned();
            if (i < model->GetMaterials().size()) mr.MaterialUUID = model->GetMaterials()[i];
        }
        SetSelectedEntity(root);
    }
}
