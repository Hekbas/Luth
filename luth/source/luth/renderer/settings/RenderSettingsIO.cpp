#include "luthpch.h"
#include "luth/renderer/settings/RenderSettingsIO.h"

#include "luth/scene/systems/RenderingSystem.h"

#include <fstream>

namespace Luth
{
    using json = nlohmann::json;

    namespace
    {
        // Direction is picked by overload on the json's const-ness: Save passes json&, Load passes
        // const json&. Each Visit below is therefore written once and drives both, which is the whole
        // point -- adding a field makes it persist AND reload in one edit, so the two directions can
        // never disagree about the field set.
        template<typename T> void Field(json& j, const char* k, T& v)       { j[k] = v; }
        template<typename T> void Field(const json& j, const char* k, T& v) { v = j.value(k, v); }

        void Field(json& j, const char* k, Vec3& v) { j[k] = json::array({ v.x, v.y, v.z }); }
        void Field(const json& j, const char* k, Vec3& v)
        {
            if (!j.contains(k)) return;
            const auto& a = j.at(k);
            if (a.is_array() && a.size() >= 3)
                v = Vec3(a[0].get<f32>(), a[1].get<f32>(), a[2].get<f32>());
        }

        // Enums persist as their integer value: a name table would be friendlier to hand-edit but would
        // need maintaining in lockstep with every enum, and these files are tool-written.
        template<typename E> void FieldEnum(json& j, const char* k, E& v) { j[k] = static_cast<i32>(v); }
        template<typename E> void FieldEnum(const json& j, const char* k, E& v)
        {
            v = static_cast<E>(j.value(k, static_cast<i32>(v)));
        }

#define F(name)  Field(j, #name, s.name)
#define FE(name) FieldEnum(j, #name, s.name)

        template<typename J> void Visit(J& j, GTAOSettings& s)
        {
            F(enabled); F(halfRes); F(visualize); F(intensity); F(radius);
            F(falloff); F(power); F(sliceCount); F(stepsPerSlice);
        }

        template<typename J> void Visit(J& j, PostProcessSettings& s)
        {
            F(bloomThreshold); F(bloomStrength); F(bloomRadius);
            FE(tonemapOp); F(exposure); F(contrast); F(saturation);
            F(shadowBalance); F(midtoneBalance); F(highlightBalance);
            F(vignetteAmount); F(vignetteHardness); F(grainAmount);
            F(sharpness); F(chromaticAberration);
            F(specularAaEnabled); F(specularAaSigma);
            F(taaEnabled); F(taaTemporalAlpha);
            // gtao is a sibling section, not flattened in here: --set takes a single <section>.<field>,
            // so giving GTAO its own section keeps "gtao.enabled" addressable and unambiguous.
        }

        template<typename J> void Visit(J& j, VolumetricSettings& s)
        {
            F(distanceFogEnabled); F(distanceFogDensity); F(distanceFogStart);
            F(distanceFogMaxOpacity); F(distanceFogColor);
            F(heightFogEnabled); F(heightFogDensity); F(heightFogRefHeight);
            F(heightFogFalloff); F(heightFogColor);
            F(anisotropy); F(scatteringIntensity); F(multiScatterIntensity);
            F(temporalAlpha); F(sunFogAbsorptionSteps); F(rtShadows); F(skyFogStrength);
            F(vizScaleDensity); F(vizScaleInScatter); F(vizOpacity);
            F(noiseScale); F(noiseStrength); F(noiseWind); F(blueNoiseDither);
            FE(quality);
        }

        template<typename J> void Visit(J& j, TransparencySettings& s)
        {
            FE(mode); F(avgLayersBudget); F(maxResolveK);
        }

        template<typename J> void Visit(J& j, RestirSettings& s)
        {
            F(enabled); F(halfResolution); F(candidateCount); F(temporalMCap);
            F(temporalDepthThreshold); F(temporalNormalThreshold);
            F(spatialNeighbours); F(spatialRadius);
            F(spatialDepthThreshold); F(spatialNormalThreshold);
            F(roughnessThreshold); F(boilingStrength);
            F(specular); F(specularIntensity); F(diSpecClamp); F(confidenceNorm);
        }

        template<typename J> void Visit(J& j, RestirGiSettings& s)
        {
            F(enabled); F(halfResolution); F(secondaryAlbedo); F(maxIndirect);
            F(temporalMCap); F(maxReservoirAge);
            F(temporalDepthThreshold); F(temporalNormalThreshold);
            F(spatialNeighbours); F(spatialRadius);
            F(spatialDepthThreshold); F(spatialNormalThreshold);
            F(boilingStrength); F(confidenceNorm);
        }

        template<typename J> void Visit(J& j, SvgfSettings& s)
        {
            F(enabled); F(alphaColor); F(alphaMoments); F(historyCap);
            F(depthThreshold); F(normalThreshold); F(antiFireflySigma); F(confidenceScale);
            F(atrousIterations); F(phiColor); F(phiNormal); F(phiDepth); F(phiRough);
        }

        template<typename J> void Visit(J& j, WindSettings& s)
        {
            F(enabled); F(direction); F(strength); F(mainBendScale);
            F(detailScale); F(frequency);
            F(gustStrength); F(gustFrequency);
            F(turbulenceAmplitude); F(turbulenceFrequency);
        }

        template<typename J> void Visit(J& j, PathTraceSettings& s)
        {
            F(samplesPerFrame); F(maxBounces); F(rrStartDepth); F(fireflyClamp); F(accumulate);
        }

        template<typename J> void Visit(J& j, ReflectionsSettings& s)
        {
            F(enabled); F(halfResolution); F(roughnessFadeStart); F(roughnessFadeEnd);
            F(maxRayDistance); F(minLobeAlpha); F(neeClamp); F(fireflyClamp); F(denoise);
        }

        template<typename J> void Visit(J& j, EmissiveLightSettings& s)
        {
            F(enabled); F(minPowerLum);
        }

        template<typename J> void Visit(J& j, ViewRenderSettings& s)
        {
            F(iblIntensity); F(skyboxIntensity); F(enableVolumetricFog);
        }

#undef F
#undef FE

        // By value so one visitor serves a const RenderingSystem: these are small PODs.
        template<typename T> void SaveSub(json& j, const char* key, T s)
        {
            json sub;
            Visit(sub, s);
            j[key] = std::move(sub);
        }

        template<typename T> void LoadSub(const json& j, const char* key, T& s)
        {
            if (j.contains(key) && j.at(key).is_object()) Visit(j.at(key), s);
        }

        // Section keys are the --set namespace, so renaming one breaks every sweep script that
        // referenced it. Keep them stable.
        constexpr const char* k_View        = "view";
        constexpr const char* k_PostProcess = "postProcess";
        constexpr const char* k_Gtao        = "gtao";
        constexpr const char* k_Volumetric  = "volumetric";
        constexpr const char* k_Transparency = "transparency";
        constexpr const char* k_Restir      = "restirDi";
        constexpr const char* k_RestirGi    = "restirGi";
        constexpr const char* k_Svgf        = "svgfDi";
        constexpr const char* k_SvgfGi      = "svgfGi";
        constexpr const char* k_SvgfSpec    = "svgfSpec";
        constexpr const char* k_SvgfDiSpec  = "svgfDiSpec";
        constexpr const char* k_Wind        = "wind";
        constexpr const char* k_PathTrace   = "pathTrace";
        constexpr const char* k_Reflections = "reflections";
        constexpr const char* k_Emissive    = "emissiveLights";
        constexpr const char* k_RenderMode  = "renderMode";

        json ToJson(const RenderingSystem& rs)
        {
            json j;
            SaveSub(j, k_View,         rs.GetViewRenderSettings());
            SaveSub(j, k_PostProcess,  rs.GetPostProcessSettings());
            SaveSub(j, k_Gtao,         rs.GetPostProcessSettings().gtao);
            SaveSub(j, k_Volumetric,   rs.GetVolumetricSettings());
            SaveSub(j, k_Transparency, rs.GetTransparencySettings());
            SaveSub(j, k_Restir,       rs.GetRestirSettings());
            SaveSub(j, k_RestirGi,     rs.GetRestirGiSettings());
            SaveSub(j, k_Svgf,         rs.GetSvgfSettings());
            SaveSub(j, k_SvgfGi,       rs.GetSvgfGiSettings());
            SaveSub(j, k_SvgfSpec,     rs.GetSvgfSpecSettings());
            SaveSub(j, k_SvgfDiSpec,   rs.GetSvgfDiSpecSettings());
            SaveSub(j, k_Wind,         rs.GetWindSettings());
            SaveSub(j, k_PathTrace,    rs.GetPathTraceSettings());
            SaveSub(j, k_Reflections,  rs.GetReflectionsSettings());
            SaveSub(j, k_Emissive,     rs.GetEmissiveLightSettings());
            j[k_RenderMode] = static_cast<i32>(rs.GetRenderMode());
            return j;
        }

        void FromJson(RenderingSystem& rs, const json& j)
        {
            LoadSub(j, k_View,         rs.GetViewRenderSettings());
            LoadSub(j, k_PostProcess,  rs.GetPostProcessSettings());
            LoadSub(j, k_Gtao,         rs.GetPostProcessSettings().gtao);
            LoadSub(j, k_Volumetric,   rs.GetVolumetricSettings());
            LoadSub(j, k_Transparency, rs.GetTransparencySettings());
            LoadSub(j, k_Restir,       rs.GetRestirSettings());
            LoadSub(j, k_RestirGi,     rs.GetRestirGiSettings());
            LoadSub(j, k_Svgf,         rs.GetSvgfSettings());
            LoadSub(j, k_SvgfGi,       rs.GetSvgfGiSettings());
            LoadSub(j, k_SvgfSpec,     rs.GetSvgfSpecSettings());
            LoadSub(j, k_SvgfDiSpec,   rs.GetSvgfDiSpecSettings());
            LoadSub(j, k_Wind,         rs.GetWindSettings());
            LoadSub(j, k_PathTrace,    rs.GetPathTraceSettings());
            LoadSub(j, k_Reflections,  rs.GetReflectionsSettings());
            LoadSub(j, k_Emissive,     rs.GetEmissiveLightSettings());
            if (j.contains(k_RenderMode))
                rs.SetRenderMode(static_cast<RenderMode>(
                    j.value(k_RenderMode, static_cast<i32>(rs.GetRenderMode()))));
        }
    }

    namespace RenderSettingsIO
    {
        std::filesystem::path DefaultPath(const std::filesystem::path& projectRoot)
        {
            return projectRoot / ".luth" / "render.json";
        }

        bool Load(RenderingSystem& rs, const std::filesystem::path& path)
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) return false;

            std::ifstream file(path);
            if (!file.is_open()) {
                LH_CORE_WARN("RenderSettingsIO: cannot open '{}'", path.string());
                return false;
            }

            // A malformed file must not silently fall back to defaults: that is exactly the failure
            // mode this system exists to remove, so say so loudly and keep whatever is in memory.
            json j;
            try { j = json::parse(file); }
            catch (const std::exception& e) {
                LH_CORE_ERROR("RenderSettingsIO: parse failed for '{}': {}", path.string(), e.what());
                return false;
            }

            FromJson(rs, j);
            LH_CORE_INFO("RenderSettingsIO: loaded render settings from '{}'", path.string());
            return true;
        }

        bool Save(const RenderingSystem& rs, const std::filesystem::path& path)
        {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);

            std::ofstream file(path, std::ios::trunc);
            if (!file.is_open()) {
                LH_CORE_ERROR("RenderSettingsIO: cannot write '{}'", path.string());
                return false;
            }
            file << ToJson(rs).dump(4);
            return true;
        }

        bool ApplyOverride(RenderingSystem& rs, const std::string& assignment)
        {
            const auto eq = assignment.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 >= assignment.size()) {
                LH_CORE_ERROR("RenderSettingsIO: --set expects <section>.<field>=<value>, got '{}'", assignment);
                return false;
            }
            const std::string key = assignment.substr(0, eq);
            const std::string val = assignment.substr(eq + 1);

            // Round-trip through the visitor's own json so every serialized field is settable and the
            // existing value decides the type. No second name->member table to fall out of sync.
            json j = ToJson(rs);

            // A dotted key addresses a field inside a section; a bare key addresses a top-level scalar
            // such as renderMode, which is how a sweep selects the path-traced reference.
            const auto dot = key.find('.');
            json* secPtr = &j;
            std::string field = key;
            if (dot != std::string::npos) {
                const std::string section = key.substr(0, dot);
                field = key.substr(dot + 1);
                if (!j.contains(section) || !j.at(section).is_object()) {
                    LH_CORE_ERROR("RenderSettingsIO: --set unknown section '{}'", section);
                    return false;
                }
                secPtr = &j.at(section);
            }
            json& sec = *secPtr;
            if (!sec.contains(field) || sec.at(field).is_object()) {
                LH_CORE_ERROR("RenderSettingsIO: --set unknown field '{}'", key);
                return false;
            }

            const json& existing = sec.at(field);
            try {
                if (existing.is_boolean())
                    sec[field] = (val == "1" || val == "true" || val == "on");
                else if (existing.is_number_integer())
                    sec[field] = std::stoll(val);
                else if (existing.is_number_float())
                    sec[field] = std::stod(val);
                else if (existing.is_array())
                    sec[field] = json::parse(val);   // "[1,0,0]"
                else
                    sec[field] = val;
            }
            catch (const std::exception& e) {
                LH_CORE_ERROR("RenderSettingsIO: --set bad value '{}' for '{}': {}", val, key, e.what());
                return false;
            }

            FromJson(rs, j);
            LH_CORE_INFO("RenderSettingsIO: --set {}={}", key, val);
            return true;
        }
    }
}
