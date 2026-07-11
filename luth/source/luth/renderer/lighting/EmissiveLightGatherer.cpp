#include "luthpch.h"
#include "luth/renderer/lighting/EmissiveLightGatherer.h"
#include "luth/renderer/lighting/EmissiveLight.h"
#include "luth/renderer/settings/EmissiveLightSettings.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/core/RenderSnapshot.h"

#include <cstring>

namespace Luth
{
    namespace
    {
        inline f32 LumOf(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

        // Vose alias-table build over per-light `power`. Fills out[i] = { prob, aliasIdx, pmf }, where pmf
        // is light i's true selection probability (power_i / total) and (prob, alias) drive the O(1) draw.
        void BuildAliasTable(const std::vector<f32>& power, std::vector<LightAliasEntry>& out)
        {
            const u32 n = static_cast<u32>(power.size());
            out.assign(n, LightAliasEntry{});
            if (n == 0) return;

            f64 total = 0.0;
            for (f32 p : power) total += p;
            if (total <= 0.0)   // degenerate: uniform fallback so a draw is still well-defined
            {
                for (u32 i = 0; i < n; ++i) out[i] = { 1.0f, i, 1.0f / f32(n), 0.0f };
                return;
            }

            std::vector<f32> scaled(n);
            std::vector<u32> small, large;
            small.reserve(n); large.reserve(n);
            for (u32 i = 0; i < n; ++i)
            {
                scaled[i]    = static_cast<f32>(power[i] / total * f64(n));   // p_i * n
                out[i].pmf   = static_cast<f32>(power[i] / total);
                out[i].alias = i;
                out[i].prob  = 1.0f;
                (scaled[i] < 1.0f ? small : large).push_back(i);
            }
            while (!small.empty() && !large.empty())
            {
                const u32 s = small.back(); small.pop_back();
                const u32 l = large.back(); large.pop_back();
                out[s].prob  = scaled[s];
                out[s].alias = l;
                scaled[l]    = (scaled[l] + scaled[s]) - 1.0f;
                (scaled[l] < 1.0f ? small : large).push_back(l);
            }
            // Numerical leftovers on either worklist settle to prob 1 (self-alias).
            for (u32 l : large) { out[l].prob = 1.0f; out[l].alias = l; }
            for (u32 s : small) { out[s].prob = 1.0f; out[s].alias = s; }
        }
    }

    void EmissiveLightGatherer::Gather(const RenderSnapshot& snapshot, GatheredLights& lights,
                                       const EmissiveLightSettings& settings, bool diEnabled)
    {
        LH_PROFILE_FUNCTION();

        // Feature or DI off: no emissive area lights. Emitters keep self-glow + the GI on-hit seed
        // (TlasBuilder leaves the emitter bit unset under the same gate, so the two agree).
        if (!settings.enabled || !diEnabled)
        {
            lights.tris.clear();
            lights.alias.clear();
            m_LastHash = 0;
            return;
        }

        // Hash the emissive-relevant state: each emissive instance's transform + emission (world-space
        // triangles depend on the matrix) and each point light's POWER (color + intensity drive the alias
        // pmf; positions do not, so they are excluded). Unchanged -> reuse last frame's tris/alias.
        u64 h = 0xcbf29ce484222325ull;
        auto mix  = [&h](u64 v) { h ^= v; h *= 0x100000001b3ull; };
        auto mixF = [&](f32 f) { u32 u; std::memcpy(&u, &f, 4); mix(u); };
        for (const auto& m : snapshot.meshes)
        {
            auto mat = AssetManager::GetAsset<Material>(m.materialUUID);
            if (!mat || !IsEmissiveLightMaterial(*mat, m.isSkinned, m.isDeformable)) continue;
            mix(m.entity); mix(m.modelUUID.GetHalf0()); mix(m.meshIndex);
            const f32* wm = &m.worldMatrix[0][0];
            for (int i = 0; i < 16; ++i) mixF(wm[i]);
            const Vec3 le = mat->GetEmissiveColor() * mat->GetEmissiveStrength();
            mixF(le.x); mixF(le.y); mixF(le.z);
        }
        for (const auto& p : lights.points) { mixF(p.color.x); mixF(p.color.y); mixF(p.color.z); mixF(p.intensity); }
        mix(lights.points.size());
        if (h == m_LastHash && !lights.tris.empty()) return;   // static scene: keep cached tris/alias
        m_LastHash = h;

        // Rebuild the world-space emissive triangle list.
        lights.tris.clear();
        for (const auto& m : snapshot.meshes)
        {
            auto mat = AssetManager::GetAsset<Material>(m.materialUUID);
            if (!mat || !IsEmissiveLightMaterial(*mat, m.isSkinned, m.isDeformable)) continue;
            auto model = AssetManager::GetAsset<Model>(m.modelUUID);
            if (!model) continue;
            auto& meshes = model->GetMeshesData();
            if (m.meshIndex >= meshes.size()) continue;
            const MeshData& md = meshes[m.meshIndex];
            if (md.Vertices.empty() || md.Indices.empty()) continue;   // skinned mesh keeps verts in SkinnedVertices

            const Vec3 avgLe = mat->GetEmissiveColor() * mat->GetEmissiveStrength();
            const f32  leLum = LumOf(avgLe);
            if (leLum <= 0.0f) continue;

            const Mat4& W = m.worldMatrix;
            const auto& V = md.Vertices;
            const auto& I = md.Indices;
            const u32   vn = static_cast<u32>(V.size());
            for (size_t t = 0; t + 2 < I.size(); t += 3)
            {
                const u32 i0 = I[t + 0], i1 = I[t + 1], i2 = I[t + 2];
                if (i0 >= vn || i1 >= vn || i2 >= vn) continue;
                const Vec3 p0 = Vec3(W * Vec4(V[i0].Position, 1.0f));
                const Vec3 e1 = Vec3(W * Vec4(V[i1].Position, 1.0f)) - p0;
                const Vec3 e2 = Vec3(W * Vec4(V[i2].Position, 1.0f)) - p0;
                const f32  area = 0.5f * Math::Length(Math::Cross(e1, e2));
                if (area <= 1e-8f) continue;                          // degenerate triangle
                if (Math::TwoPi<f32> * area * leLum < settings.minPowerLum) continue;
                lights.tris.push_back({ p0, area, e1, 0.0f, e2, 0.0f, avgLe, 0.0f });
            }
        }

        // Unified power-weighted alias table over [points | tris]. Skipped when no triangles survive, so a
        // point-only scene keeps ReSTIR DI's legacy uniform point sampling (zero regression when unused).
        if (lights.tris.empty()) { lights.alias.clear(); return; }
        const u32 pc = static_cast<u32>(lights.points.size());
        const u32 tc = static_cast<u32>(lights.tris.size());
        std::vector<f32> power(pc + tc);
        for (u32 i = 0; i < pc; ++i)
            power[i] = 4.0f * Math::Pi<f32> * lights.points[i].intensity * LumOf(lights.points[i].color);
        for (u32 i = 0; i < tc; ++i)
            power[pc + i] = Math::TwoPi<f32> * lights.tris[i].area * LumOf(lights.tris[i].avgLe);
        BuildAliasTable(power, lights.alias);
    }
}
