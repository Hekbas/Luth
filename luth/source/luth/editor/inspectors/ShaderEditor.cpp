#include "luthpch.h"
#include "luth/editor/inspectors/ShaderEditor.h"
#include "luth/editor/UI.h"
#include "luth/renderer/shader/Shader.h"

namespace Luth
{
    void ShaderEditor::Draw(Shader& shader)
    {
        // Load source code when shader changes
        if (shader.Handle != m_LastShaderUUID)
        {
            m_LastShaderUUID = shader.Handle;
            m_SourceCode.clear();

            const fs::path& path = shader.GetPath();
            if (fs::exists(path))
            {
                std::ifstream file(path);
                if (file.is_open())
                {
                    std::stringstream ss;
                    ss << file.rdbuf();
                    m_SourceCode = ss.str();
                }
            }
        }

        // Reflection Data
        if (UI::BeginCollapsingHeader("Reflection", true))
        {
            // Uniform Buffers
            const auto& buffers = shader.GetBuffers();
            if (!buffers.empty())
            {
                ImGui::Text("Uniform Buffers (%d)", (int)buffers.size());
                ImGui::Indent();
                for (const auto& [name, buffer] : buffers)
                {
                    if (ImGui::TreeNode(name.c_str(), "%s (set=%d, binding=%d, size=%d)", name.c_str(), buffer.Set, buffer.Binding, buffer.Size))
                    {
                        if (ImGui::BeginTable(("##UBO_" + name).c_str(), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("Type");
                            ImGui::TableSetupColumn("Offset");
                            ImGui::TableHeadersRow();

                            for (const auto& [uName, uniform] : buffer.Uniforms)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn(); ImGui::Text("%s", uName.c_str());
                                ImGui::TableNextColumn(); ImGui::Text("%d", (int)uniform.Type);
                                ImGui::TableNextColumn(); ImGui::Text("%d", uniform.Offset);
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::Unindent();
            }

            // Push Constants
            const auto& pushConstants = shader.GetPushConstants();
            if (!pushConstants.empty())
            {
                ImGui::Dummy({ 0, 4 });
                ImGui::Text("Push Constants (%d)", (int)pushConstants.size());
                ImGui::Indent();
                for (const auto& [name, pc] : pushConstants)
                {
                    ImGui::Text("%s (size=%d)", name.c_str(), pc.Size);
                }
                ImGui::Unindent();
            }

            // Resources (Samplers, Images)
            const auto& resources = shader.GetResources();
            if (!resources.empty())
            {
                ImGui::Dummy({ 0, 4 });
                ImGui::Text("Resources (%d)", (int)resources.size());
                ImGui::Indent();
                for (const auto& [name, res] : resources)
                {
                    ImGui::BulletText("%s (set=%d, binding=%d, array=%d)", name.c_str(), res.Set, res.Binding, res.ArraySize);
                }
                ImGui::Unindent();
            }
            UI::EndCollapsingHeader();
        }

        ImGui::Dummy({ 0, 4 });

        // Source Code (read-only)
        if (UI::BeginCollapsingHeader("Source Code"))
        {
            if (!m_SourceCode.empty())
            {
                float availHeight = ImGui::GetContentRegionAvail().y - 8.0f;
                if (availHeight < 200.0f) availHeight = 200.0f;

                ImGui::InputTextMultiline("##ShaderSource", m_SourceCode.data(), m_SourceCode.size() + 1,
                    ImVec2(-1, availHeight), ImGuiInputTextFlags_ReadOnly);
            }
            else
            {
                ImGui::TextDisabled("Source file not found");
            }
            UI::EndCollapsingHeader();
        }
    }
}
