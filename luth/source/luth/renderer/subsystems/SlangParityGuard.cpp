#include "luthpch.h"
#include "luth/renderer/subsystems/SlangParityGuard.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/settings/SlangParitySettings.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/core/diagnostics/Log.h"

namespace Luth
{
    namespace
    {
        // The production shader the gate watches — a self-contained rayQuery+bindless+BDA consumer.
        constexpr const char* kGateName = "restir_gi_initial.slang";
        constexpr const char* kGatePath = "shaders/restir_gi_initial.slang";

        // Minimal SPIR-V walk for the bindless-codegen regression signals (slang#10525): count the
        // NonUniform decorations and verify the capabilities that make them valid. No full parse, no dep.
        struct SpirvScan { u32 nonUniform = 0; bool caps = false; };
        SpirvScan ScanBindlessSpirv(const std::vector<u32>& spv)
        {
            SpirvScan r{};
            if (spv.size() < 5 || spv[0] != 0x07230203u) return r;   // SPIR-V magic word
            bool nonUni = false, runtimeArr = false, psb = false, rayQuery = false;
            for (size_t i = 5; i < spv.size(); )                     // skip the 5-word header
            {
                const u32 word = spv[i];
                const u32 wc   = word >> 16;
                const u32 op   = word & 0xFFFFu;
                if (wc == 0 || i + wc > spv.size()) break;           // malformed — stop walking
                if (op == 17u && wc >= 2u)                           // OpCapability
                {
                    switch (spv[i + 1])
                    {
                        case 5301u: nonUni     = true; break;        // ShaderNonUniform
                        case 5302u: runtimeArr = true; break;        // RuntimeDescriptorArray
                        case 5347u: psb        = true; break;        // PhysicalStorageBufferAddresses
                        case 4472u: rayQuery   = true; break;        // RayQueryKHR
                    }
                }
                else if (op == 71u && wc >= 3u && spv[i + 2] == 5300u) ++r.nonUniform;   // OpDecorate NonUniform
                else if (op == 72u && wc >= 4u && spv[i + 3] == 5300u) ++r.nonUniform;   // OpMemberDecorate NonUniform
                i += wc;
            }
            r.caps = nonUni && runtimeArr && psb && rayQuery;
            return r;
        }
    }

    void SlangParityGuard::Init(RenderPipeline& pipeline)
    {
        LH_PROFILE_FUNCTION();
        m_Pipeline = &pipeline;
        RunGate();   // RtRestirGiSubsystem::Init already compiled the watched shader (cached) before us
    }

    void SlangParityGuard::RunGate()
    {
        LH_PROFILE_FUNCTION();
        // SPIR-V is free from the ShaderLibrary cache — the watched shader is a production consumer that a
        // subsystem already loaded; LoadEngine returns the cached artifact, no recompile.
        if (auto sh = ShaderLibrary::LoadEngine(kGatePath))
            m_GateSpv = sh->GetSpirV();
        if (m_GateSpv.empty())
        {
            LH_CORE_WARN("SlangParity: {} SPIR-V unavailable — codegen gate not run", kGateName);
            return;
        }
        LH_CORE_INFO("SlangParity: gate on {} -> {} SPIR-V words", kGateName, m_GateSpv.size());
        CheckSlangSpirv();
    }

    // Scan the compiled SPIR-V for the bindless-codegen signals slang#10525-class bugs break — the
    // NonUniform decorations on the bindless texture accesses and the caps that make them valid. Sets the
    // verdict; logs once per check (INFO on pass, WARN on regression). Deterministic — no GPU, no frame.
    void SlangParityGuard::CheckSlangSpirv()
    {
        LH_PROFILE_FUNCTION();
        SpirvScan scan = ScanBindlessSpirv(m_GateSpv);
        SlangParitySettings& s = m_Pipeline->GetSystem().GetSlangParitySettings();
        s.spirvChecked    = true;
        s.nonUniformCount = scan.nonUniform;
        s.capsOk          = scan.caps;
        s.spirvPass       = scan.caps && scan.nonUniform > 0;

        if (s.spirvPass)
            LH_CORE_INFO("SlangParity: bindless SPIR-V OK — {} NonUniform decorations, caps present",
                         scan.nonUniform);
        else
            LH_CORE_WARN("SlangParity: SPIR-V REGRESSION — caps {}, {} NonUniform decorations (slang#10525?)",
                         scan.caps ? "present" : "MISSING", scan.nonUniform);
    }

    bool SlangParityGuard::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv)
    {
        LH_PROFILE_FUNCTION();
        if (name != kGateName) return false;
        m_GateSpv = spv;
        CheckSlangSpirv();   // re-run the gate against the hot-reloaded SPIR-V
        return true;
    }

    void SlangParityGuard::Shutdown()
    {
        LH_PROFILE_FUNCTION();
        m_GateSpv.clear();
        m_Pipeline = nullptr;
    }
}
