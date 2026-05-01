#include "lepch.h"
#include "luthien/panels/HierarchyPanel.h"
#include "luthien/EditorColors.h"
#include "luthien/EditorSelection.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luthien/widgets/Icons.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/FileSystem.h"
#include "luth/renderer/resources/Model.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace Luth
{
    using namespace Component;
    HierarchyPanel::HierarchyPanel()
    {
        LH_CORE_INFO("Created Hierarchy panel");
    }

    void HierarchyPanel::OnInit()
    {
    }

    void HierarchyPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Worker fiber. No ImGui, no Vulkan recording (V3). Only safe ECS reads.
        // For v2.9.0 the snapshot just carries change-detection versions so the
        // panel can later short-circuit gather when nothing changed. The actual
        // tree pre-walk lives in OnDraw inline against the live Scene — moving it
        // here is a follow-on polish commit (see editor-foundation history).
        auto* snap = builder.Add<HierarchySnapshot>();
        if (m_Context) snap->hierarchyVersion = m_Context->GetHierarchyVersion();
        snap->selectionVersion = EditorSelection::GetVersion();
    }

    void HierarchyPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetFASolid());
        if (ImGui::Begin(ICON_FA_LIST "  Hierarchy") && m_Context)
        {
            // Sync primary selection from EditorSelection (may have changed via viewport)
            m_Selection = EditorSelection::GetSelectedEntity();

            DrawTopBar();
            ImGui::Separator();

            // Handle Global Shortcuts (Delete, F2, Esc)
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                if (ImGui::IsKeyPressed(ImGuiKey_Delete) && m_Selection) {
                    Entity sel = m_Selection;
                    m_DeferredActions.push_back([this, sel]() {
                        if (sel && sel.IsValid()) {
                            CommandHistory::Execute(std::make_unique<EntityDestroyCommand>(m_Context.get(), sel));
                            SetSelectedEntity({});
                        }
                    });
                }
                
                if (ImGui::IsKeyPressed(ImGuiKey_F2) && m_Selection)
                    RenameEntity(m_Selection);

                if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    m_IsRenaming = false;
                    m_RenamingEntity = {};
                    EditorSelection::ClearSelection();
                    m_Selection = {};
                }
            }
            
            // Create New Entity Shortcut (Ctrl + Shift + N)
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyDown(ImGuiKey_LeftShift) && ImGui::IsKeyPressed(ImGuiKey_N))
            {
                auto cmd = std::make_unique<EntityCreateCommand>(m_Context.get(), "New Entity");
                auto* rawCmd = cmd.get();
                CommandHistory::Execute(std::move(cmd));
                SetSelectedEntity(rawCmd->GetCreatedEntity());
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
                        EditorSelection::ClearSelection();
                        m_Selection = {};
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
        ImGui::PushID((void*)(uintptr_t)(uint32_t)entity.GetComponent<ID>().Value.GetHalf0());

        const std::string& name = entity.GetName();

        // Determine icon based on entity components
        const char* icon = ICON_FA_CIRCLE_DOT;
        if (entity.HasComponent<Camera>())               icon = ICON_FA_VIDEO;
        else if (entity.HasComponent<DirectionalLight>()) icon = ICON_FA_SUN;
        else if (entity.HasComponent<PointLight>())       icon = ICON_FA_LIGHTBULB;
        else if (entity.HasComponent<Animation>())        icon = ICON_FA_PERSON_RUNNING;
        else if (entity.HasComponent<MeshRenderer>())     icon = ICON_FA_CUBE;
        
        // Filter: skip subtrees with no matching descendants
        if (strlen(m_SearchFilter) > 0 && !SubtreeMatchesFilter(entity, m_SearchFilter))
        {
            ImGui::PopID();
            return;
        }

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanAvailWidth |
            ImGuiTreeNodeFlags_FramePadding;

        if (EditorSelection::IsSelected(entity)) flags |= ImGuiTreeNodeFlags_Selected;

        bool hasChildren = !entity.GetChildren().empty();
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf;

        // Force open if this node doesn't match but a descendant does
        if (strlen(m_SearchFilter) > 0 && hasChildren)
        {
            std::string nameLower = name;
            std::string filterLower = m_SearchFilter;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
            if (nameLower.find(filterLower) == std::string::npos)
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }

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
                std::string oldName = entity.GetName();
                std::string newName = m_RenameBuffer;
                if (oldName != newName) {
                    CommandHistory::Execute(std::make_unique<EntityRenameCommand>(
                        entity.GetScene(), (entt::entity)entity, oldName, newName));
                }
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
            opened = ImGui::TreeNodeEx("##Node", flags, "%s  %s", icon, name.c_str());
        }

        // Handle Selection (with Ctrl/Shift multi-select)
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            bool ctrlHeld  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
            bool shiftHeld = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

            if (ctrlHeld)
                EditorSelection::ToggleEntity(entity);
            else if (shiftHeld)
                EditorSelection::AddEntity(entity);
            else
                SetSelectedEntity(entity);

            m_Selection = EditorSelection::GetSelectedEntity();

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
            // Vertical-only tree connector line (matches ProjectPanel style)
            const ImColor treeLineColor = EditorColors::TreeLine;
            const float smallOffsetX = -6.0f;
            ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();
            verticalLineStart.x += smallOffsetX;
            ImVec2 verticalLineEnd = verticalLineStart;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            auto children = entity.GetChildren(); // Copy to avoid iterator invalidation if reordered
            for (auto child : children)
            {
                ImVec2 currentPos = ImGui::GetCursorScreenPos();
                DrawEntityNode(child);
                verticalLineEnd.y = currentPos.y + ImGui::GetFontSize() * 0.5f;
            }

            // Draw vertical trunk line
            if (!children.empty())
                drawList->AddLine(verticalLineStart, verticalLineEnd, treeLineColor);

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
                    ImU32 highlightColor = EditorColors::DragHighlight;

                    if (isReorderingTop)
                    {
                        drawList->AddLine(ImVec2(ImGui::GetItemRectMin().x, itemMinY), ImVec2(ImGui::GetItemRectMax().x, itemMinY), highlightColor, 2.0f);
                        if (ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                        {
                            CommandHistory::Execute(std::make_unique<EntityReorderCommand>(
                                m_Context.get(), payloadEntity, targetEntity, false));
                        }
                    }
                    else if (isReorderingBot)
                    {
                        drawList->AddLine(ImVec2(ImGui::GetItemRectMin().x, itemMaxY), ImVec2(ImGui::GetItemRectMax().x, itemMaxY), highlightColor, 2.0f);
                        if (ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                        {
                            CommandHistory::Execute(std::make_unique<EntityReorderCommand>(
                                m_Context.get(), payloadEntity, targetEntity, true));
                        }
                    }
                    else
                    {
                        // Parenting
                        drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), highlightColor, 0.0f, 0, 2.0f);

                        if (ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                        {
                            CommandHistory::Execute(std::make_unique<EntityReparentCommand>(
                                m_Context.get(), payloadEntity, targetEntity));
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
                CommandHistory::Execute(std::make_unique<EntityReparentCommand>(
                    m_Context.get(), payloadEntity, Entity{})); // Make Root
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
            UUID parentUUID = (parent && parent.IsValid())
                ? parent.GetComponent<Component::ID>().Value : UUID::Invalid();
            m_DeferredActions.push_back([this, parentUUID]() {
                auto cmd = std::make_unique<EntityCreateCommand>(m_Context.get(), "New Entity", parentUUID);
                auto* rawCmd = cmd.get();
                CommandHistory::Execute(std::move(cmd));
                SetSelectedEntity(rawCmd->GetCreatedEntity());
            });
        }

        if (ImGui::BeginMenu("3D Object"))
        {
            auto spawnPrimitive = [this](const char* relPath, const char* name, Entity parent) {
                UUID uuid = AssetDatabase::GetUUID(FileSystem::EngineAssetsPath(relPath));
                if (!uuid.IsValid()) return;
                if (AssetManager::IsLoaded(uuid))
                    InstantiateModel(uuid, parent);
                else {
                    AssetManager::LoadAsync(uuid);
                    m_PendingInstantiations.push_back({ uuid, parent });
                }
            };

            if (ImGui::MenuItem("Plane")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Plane.fbx", "Plane", parent);
                });
            }
            if (ImGui::MenuItem("Cube")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Cube.fbx", "Cube", parent);
                });
            }
            if (ImGui::MenuItem("Sphere")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Sphere.fbx", "Sphere", parent);
                });
            }
            if (ImGui::MenuItem("Icosphere")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Icosphere.fbx", "Icosphere", parent);
                });
            }
            if (ImGui::MenuItem("Cylinder")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Cylinder.fbx", "Cylinder", parent);
                });
            }
            if (ImGui::MenuItem("Cone")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Cone.fbx", "Cone", parent);
                });
            }
            if (ImGui::MenuItem("Torus")) {
                m_DeferredActions.push_back([this, parent, spawnPrimitive]() {
                    spawnPrimitive("models/primitives/Torus.fbx", "Torus", parent);
                });
            }
            
            ImGui::EndMenu();
        }
        
        if (ImGui::MenuItem("Camera"))
        {
            UUID parentUUID = (parent && parent.IsValid())
                ? parent.GetComponent<Component::ID>().Value : UUID::Invalid();
            m_DeferredActions.push_back([this, parentUUID]() {
                CommandHistory::BeginCompound("Create Camera");
                auto cmd = std::make_unique<EntityCreateCommand>(m_Context.get(), "Camera", parentUUID);
                auto* rawCmd = cmd.get();
                CommandHistory::Execute(std::move(cmd));
                Entity cam = rawCmd->GetCreatedEntity();
                if (cam.IsValid())
                    CommandHistory::Execute(std::make_unique<ComponentAddCommand<Camera>>(
                        "Add Camera", m_Context.get(), (entt::entity)cam));
                CommandHistory::EndCompound();
                SetSelectedEntity(cam);
            });
        }

        if (ImGui::BeginMenu("Light"))
        {
            if (ImGui::MenuItem("Directional Light")) {
                UUID parentUUID = (parent && parent.IsValid())
                    ? parent.GetComponent<Component::ID>().Value : UUID::Invalid();
                m_DeferredActions.push_back([this, parentUUID]() {
                    CommandHistory::BeginCompound("Create Directional Light");
                    auto cmd = std::make_unique<EntityCreateCommand>(m_Context.get(), "Directional Light", parentUUID);
                    auto* rawCmd = cmd.get();
                    CommandHistory::Execute(std::move(cmd));
                    Entity light = rawCmd->GetCreatedEntity();
                    if (light.IsValid()) {
                        CommandHistory::Execute(std::make_unique<ComponentAddCommand<DirectionalLight>>(
                            "Add DirectionalLight", m_Context.get(), (entt::entity)light));
                        auto& tc = light.GetComponent<Transform>();
                        auto oldRot = tc.Rotation;
                        tc.Rotation = Vec3(-45.0f, 0.0f, 0.0f);
                        tc.IsDirty = true;
                        CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<Transform, Vec3>>(
                            "Set Rotation", m_Context.get(), (entt::entity)light,
                            &Transform::Rotation, oldRot, tc.Rotation));
                    }
                    CommandHistory::EndCompound();
                    SetSelectedEntity(light);
                });
            }
            if (ImGui::MenuItem("Point Light"))
            {
                UUID parentUUID = (parent && parent.IsValid())
                    ? parent.GetComponent<Component::ID>().Value : UUID::Invalid();
                m_DeferredActions.push_back([this, parentUUID]() {
                    CommandHistory::BeginCompound("Create Point Light");
                    auto cmd = std::make_unique<EntityCreateCommand>(m_Context.get(), "Point Light", parentUUID);
                    auto* rawCmd = cmd.get();
                    CommandHistory::Execute(std::move(cmd));
                    Entity light = rawCmd->GetCreatedEntity();
                    if (light.IsValid())
                        CommandHistory::Execute(std::make_unique<ComponentAddCommand<PointLight>>(
                            "Add PointLight", m_Context.get(), (entt::entity)light));
                    CommandHistory::EndCompound();
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
            Entity sel = m_Selection;
            m_DeferredActions.push_back([this, sel]() {
                if (sel && sel.IsValid()) {
                    CommandHistory::Execute(std::make_unique<EntityDestroyCommand>(m_Context.get(), sel));
                    SetSelectedEntity({});
                }
            });
        }
    }

    void HierarchyPanel::SetSelectedEntity(Entity entity)
    {
        m_Selection = entity;
        EditorSelection::SelectEntity(entity);
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

    bool HierarchyPanel::SubtreeMatchesFilter(Entity entity, const char* filter)
    {
        std::string nameLower = entity.GetName();
        std::string filterLower = filter;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
        std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
        if (nameLower.find(filterLower) != std::string::npos)
            return true;

        for (auto child : entity.GetChildren())
        {
            if (SubtreeMatchesFilter(child, filter))
                return true;
        }
        return false;
    }

    void HierarchyPanel::InstantiateModel(UUID assetUuid, Entity parent)
    {
        UUID parentUUID = (parent && parent.IsValid())
            ? parent.GetComponent<Component::ID>().Value : UUID::Invalid();

        auto cmd = std::make_unique<ModelInstantiateCommand>(m_Context.get(), assetUuid, parentUUID);
        auto* rawCmd = cmd.get();
        CommandHistory::Execute(std::move(cmd));
        Entity root = rawCmd->GetRootEntity();
        if (root.IsValid())
            SetSelectedEntity(root);
    }
}
