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

#include <nlohmann/json.hpp>

namespace Luth::ComponentDrawers
{
    using namespace Component;

    void RegisterMeshRenderer()
    {
        ComponentDrawerOptions opts;
        opts.OnCopy = [](Entity e) {
            const auto& mr = e.GetComponent<MeshRenderer>();
            nlohmann::json j;
            j["modelUUID"]    = mr.ModelUUID.ToString();
            j["meshIndex"]    = mr.MeshIndex;
            j["materialUUID"] = mr.MaterialUUID.ToString();
            j["isSkinned"]    = mr.isSkinned;
            return j.dump();
        };
        opts.OnPaste = [](Entity e, const std::string& data) -> bool {
            try {
                auto j = nlohmann::json::parse(data);
                MeshRenderer newMr;
                newMr.ModelUUID    = UUID::FromString(j.value("modelUUID", ""));
                newMr.MeshIndex    = j.value("meshIndex", 0u);
                newMr.MaterialUUID = UUID::FromString(j.value("materialUUID", ""));
                newMr.isSkinned    = j.value("isSkinned", false);
                CommandHistory::Execute(std::make_unique<ComponentReplaceCommand<MeshRenderer>>(
                    "Paste MeshRenderer", e.GetScene(), (entt::entity)e, std::move(newMr)));
                return true;
            } catch (...) { return false; }
        };

        ComponentDrawerRegistry::Register<MeshRenderer>(
            "Mesh Renderer",
            [](Entity entity, MeshRenderer& meshRenderer) {
                if (UI::BeginProperties()) {
                    Scene* scene = entity.GetScene();
                    entt::entity ent = (entt::entity)entity;

                    {
                        auto state = UI::PropertyAsset("Model", meshRenderer.ModelUUID, AssetType::Model);
                        if (state.committed)
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, UUID>>(
                                "Change Model", scene, ent, &MeshRenderer::ModelUUID,
                                UI::ConsumeItemPreEdit<UUID>(state.itemId), meshRenderer.ModelUUID));
                    }

                    if (meshRenderer.ModelUUID.IsValid() && !AssetManager::IsLoaded(meshRenderer.ModelUUID) && !AssetManager::IsLoading(meshRenderer.ModelUUID))
                         AssetManager::LoadAsync(meshRenderer.ModelUUID);

                    if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID)) {
                        int meshIndex = (int)meshRenderer.MeshIndex;
                        auto state = UI::Property("Mesh Index", meshIndex, 0, (int)model->GetMeshes().size() - 1);
                        if (state.changed) meshRenderer.MeshIndex = (u32)meshIndex;
                        if (state.committed) {
                            u32 prev = (u32)UI::ConsumeItemPreEdit<int>(state.itemId);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, uint32_t>>(
                                "Change Mesh Index", scene, ent, &MeshRenderer::MeshIndex, prev, meshRenderer.MeshIndex));
                        }
                    }

                    {
                        auto state = UI::PropertyAsset("Material", meshRenderer.MaterialUUID, AssetType::Material);
                        if (state.committed) {
                            if (auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID))
                                model->AddMaterial(meshRenderer.MaterialUUID, meshRenderer.MeshIndex);
                            CommandHistory::Execute(std::make_unique<ComponentPropertyCommand<MeshRenderer, UUID>>(
                                "Change Material", scene, ent, &MeshRenderer::MaterialUUID,
                                UI::ConsumeItemPreEdit<UUID>(state.itemId), meshRenderer.MaterialUUID));
                        }
                    }

                    UI::EndProperties();
                }

                // Handoff to the trailing MaterialEditor panel drawn by InspectorPanel
                // after the component loop completes.
                if (auto* insp = Editor::GetPanel<InspectorPanel>())
                    insp->SetActiveMaterial(meshRenderer.MaterialUUID);
            },
            std::move(opts));
    }
}
