#pragma once

#include "luth/resources/ResourceManager.h"
#include "luth/renderer/Mesh.h"
#include "luth/renderer/Texture.h"
#include "luth/renderer/Model.h"
#include "luth/renderer/Shader.h"

//#include <assimp/Importer.hpp>
//#include <assimp/scene.h>
//#include <assimp/postprocess.h>
//#include <json/json.hpp>
//#include <filesystem>
//#include <memory>

namespace Luth
{
    template<typename T>
    struct Loader
    {
        static_assert(sizeof(T) == 0, "No loader specialization for this type");
        static std::shared_ptr<T> Load(const fs::path& path) = delete;
        static std::shared_ptr<T> GetFallback() = delete;
        static size_t CalculateMemoryUsage(const std::shared_ptr<T>& resource) = delete;
    };

    template<>
    struct Loader<Texture>
    {
        static std::shared_ptr<Texture> Load(const fs::path& path) {
            return std::make_shared<GLTexture>(path);
        }

        static std::shared_ptr<Texture> GetFallback() {
            static auto fallback = CreateFallbackTexture();
            return fallback;
        }

        static size_t CalculateMemoryUsage(const std::shared_ptr<Texture>& tex) {
            return tex ? tex->GetWidth() * tex->GetHeight() * 4 : 0; // RGBA8
        }

    private:
        static std::shared_ptr<Texture> CreateFallbackTexture() {
            auto tex = std::make_shared<GLTexture>(1, 1, TextureFormat::RGBA8);
            const uint32_t white = 0xFFFFFFFF;
            //tex->SetData(&white, sizeof(uint32_t));
            return tex;
        }
    };

    template<>
    struct Loader<Model>
    {
        static std::shared_ptr<Model> Load(const fs::path& path) {
            return std::make_shared<Model>(path);
        }

        static std::shared_ptr<Model> GetFallback() {
            static auto fallback = CreateFallbackModel();
            return fallback;
        }

        static size_t CalculateMemoryUsage(const std::shared_ptr<Model>& model) {
            size_t total = 0;
            if (model) {
                for (const auto& mesh : model->GetMeshes()) {
                    total += mesh.Vertices.size() * sizeof(Vertex);
                    total += mesh.Indices.size() * sizeof(uint32_t);
                }
            }
            return total;
        }

    private:
        static std::shared_ptr<Model> CreateFallbackModel() {
            auto model = std::make_shared<Model>("");

            MeshData cube;
            // Add cube vertices and indices
            const std::array vertices = {
                Vertex{{-0.5f, -0.5f, 0.0f}, {0,0,1}, {0,0}},
                // ... complete cube data
            };
            const std::array indices = { 0,1,2,2,3,0 };

            cube.Vertices.assign(vertices.begin(), vertices.end());
            cube.Indices.assign(indices.begin(), indices.end());

            //model->AddMesh(cube);
            return model;
        }
    };

    template<>
    struct Loader<Shader>
    {
        static std::shared_ptr<Shader> Load(const fs::path& path) {
            return Shader::Create(path.string());
        }

        static std::shared_ptr<Shader> GetFallback() {
            static auto fallback = CreateFallbackShader();
            return fallback;
        }

        static size_t CalculateMemoryUsage(const std::shared_ptr<Shader>&) {
            return 4096; // Estimated shader memory
        }

    private:
        static std::shared_ptr<Shader> CreateFallbackShader() {
            const char* vs = R"glsl(
                #version 330 core
                layout(location=0) in vec3 aPos;
                void main() {
                    gl_Position = vec4(aPos, 1.0);
                })glsl";

            const char* fs = R"glsl(
                #version 330 core
                out vec4 FragColor;
                void main() {
                    FragColor = vec4(1,0,1,1); // Magenta
                })glsl";

            return Shader::Create(vs, fs);
        }
    };
}
