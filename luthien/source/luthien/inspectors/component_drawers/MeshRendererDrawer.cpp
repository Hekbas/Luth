#include "lepch.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/panels/InspectorPanel.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/commands/Commands.h"
#include "luthien/CommandHistory.h"
#include "luth/scene/Components.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/resources/Model.h"

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterMeshRenderer()
    {
        ComponentDrawerRegistry::RegisterSimple<MeshRenderer>(
            "Mesh Renderer",
            [](Entity entity, MeshRenderer& meshRenderer) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto oldUUID = meshRenderer.ModelUUID;
                        if (UI::PropertyAsset("Model", meshRenderer.ModelUUID, AssetType::Model)) {
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, UUID>>(
                                "Change Model", scene, ent, &MeshRenderer::ModelUUID, oldUUID, meshRenderer.ModelUUID));
                        }
                    }

                    if (meshRenderer.ModelUUID.IsValid() && !AssetManager::IsLoaded(meshRenderer.ModelUUID) && !AssetManager::IsLoading(meshRenderer.ModelUUID))
                         AssetManager::LoadAsync(meshRenderer.ModelUUID);

                    if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID)) {
                        int meshIndex = (int)meshRenderer.MeshIndex;
                        auto oldIndex = meshRenderer.MeshIndex;
                        if (UI::Property("Mesh Index", meshIndex, 0, (int)model->GetMeshes().size() - 1)) {
                            meshRenderer.MeshIndex = (u32)meshIndex;
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, uint32_t>>(
                                "Change Mesh Index", scene, ent, &MeshRenderer::MeshIndex, oldIndex, meshRenderer.MeshIndex));
                        }
                    }

                    {
                        auto oldMatUUID = meshRenderer.MaterialUUID;
                        if (UI::PropertyAsset("Material", meshRenderer.MaterialUUID, AssetType::Material))
                        {
                            if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID))
                                model->AddMaterial(meshRenderer.MaterialUUID, meshRenderer.MeshIndex);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, UUID>>(
                                "Change Material", scene, ent, &MeshRenderer::MaterialUUID, oldMatUUID, meshRenderer.MaterialUUID));
                        }
                    }

                    UI::EndProperties();
                }

                // Handoff to the trailing MaterialEditor panel drawn by InspectorPanel
                // after the component loop completes.
                if (auto* insp = Editor::GetPanel<InspectorPanel>())
                    insp->SetActiveMaterial(meshRenderer.MaterialUUID);
            });
    }
}
