#include "luthpch.h"
#include "luth/renderer/rendergraph/FrameEventTree.h"
#include "luth/renderer/rendergraph/FrameCapture.h"

namespace Luth::RG
{
    namespace
    {
        // Pick the most useful archive to display when a pass is selected.
        // Strategy: first non-depth archive if any (color is what users usually
        // want to see); else first depth archive; else -1 (no preview).
        int PickPrimaryArchive(const CapturedFrame& frame, u32 passIdx)
        {
            if (passIdx >= frame.passArchives.size()) return -1;
            const auto& list = frame.passArchives[passIdx];
            if (list.empty()) return -1;

            int firstDepth = -1;
            for (u32 ai : list)
            {
                if (ai >= frame.archivedImages.size()) continue;
                const auto& a = frame.archivedImages[ai];
                if (!a.isDepth) return (int)ai;
                if (firstDepth < 0) firstDepth = (int)ai;
            }
            return firstDepth;
        }

        // [C] marks compute dispatches; indirect draws (the dominant case in PBR)
        // get no prefix — every Geometry/Shadow draw row would otherwise carry [I].
        std::string DrawLabel(const CapturedDrawCall& dc, u32 globalIdx)
        {
            const char* prefix = (dc.kind == DispatchKind::Compute) ? "[C] " : "";

            std::string label = prefix + std::string("Draw ") + std::to_string(globalIdx) + ": " + dc.meshName;
            if (!dc.pipelineState.shaderName.empty())
            {
                label += " (";
                label += dc.pipelineState.shaderName;
                label += ")";
            }
            // Render-mode tag — only useful on PBR draws (Material::RenderMode 0/1/2/3).
            if (dc.kind != DispatchKind::Compute && dc.pipelineState.shaderName == "pbr")
            {
                static const char* k_ModeTags[] = { " [Op]", " [Cu]", " [Tr]", " [Fa]" };
                if (dc.pipelineState.renderMode < 4)
                    label += k_ModeTags[dc.pipelineState.renderMode];
            }
            return label;
        }

        EventNode BuildPassNode(const CapturedFrame& frame, u32 passIdx)
        {
            const auto& pass = frame.passes[passIdx];

            EventNode node;
            node.kind               = EventNodeKind::Pass;
            node.label              = pass.name;
            node.passIndex          = passIdx;
            node.gpuTimeMs          = pass.gpuTimeMs;
            // passArchives is keyed by graph pass index (sparse) — passIdx here
            // is the dense passes[] index, so route through pass.graphPassIndex.
            node.archivedImageIndex = PickPrimaryArchive(frame, pass.graphPassIndex);

            node.children.reserve(pass.drawCallCount);
            for (u32 di = 0; di < pass.drawCallCount; ++di)
            {
                u32 globalIdx = pass.firstDrawIndex + di;
                if (globalIdx >= frame.drawCalls.size()) break;

                EventNode dn;
                dn.kind               = EventNodeKind::Draw;
                dn.passIndex          = passIdx;
                dn.drawIndex          = globalIdx;
                // Phase 14D — Draw nodes inherit the pass's primary archive
                // (i.e. show "the pass output" while inside the pass). Phase 14E
                // overrides this with a per-draw preview from replay-then-copy.
                dn.archivedImageIndex = node.archivedImageIndex;
                dn.label              = DrawLabel(frame.drawCalls[globalIdx], globalIdx);
                node.children.push_back(std::move(dn));
            }
            return node;
        }

        // Post-order walk: each node's lastDrawIndex = max draw index in its
        // subtree (UINT32_MAX if no Draw descendants). Drives Unity-style
        // tree-click snap on the panel's draw scrub slider.
        u32 PopulateLastDrawIndex(EventNode& node)
        {
            if (node.kind == EventNodeKind::Draw)
            {
                node.lastDrawIndex = node.drawIndex;
                return node.drawIndex;
            }

            u32 maxDraw = UINT32_MAX;
            for (auto& child : node.children)
            {
                u32 childLast = PopulateLastDrawIndex(child);
                if (childLast != UINT32_MAX &&
                    (maxDraw == UINT32_MAX || childLast > maxDraw))
                    maxDraw = childLast;
            }
            node.lastDrawIndex = maxDraw;
            return maxDraw;
        }

        // Map "ShadowPass.C0" / "ShadowPass.C12" / etc. to its trailing integer.
        // Returns -1 if the trailing characters are not a non-negative integer.
        int CascadeIndexFromName(const std::string& name)
        {
            constexpr const char* k_Prefix = "ShadowPass.C";
            const size_t prefixLen = std::char_traits<char>::length(k_Prefix);
            if (name.compare(0, prefixLen, k_Prefix) != 0) return -1;
            if (name.size() == prefixLen) return -1;

            int v = 0;
            for (size_t i = prefixLen; i < name.size(); ++i)
            {
                char c = name[i];
                if (c < '0' || c > '9') return -1;
                v = v * 10 + (c - '0');
            }
            return v;
        }
    } // anonymous

    EventNode BuildEventTree(const CapturedFrame& frame)
    {
        EventNode root;
        root.kind  = EventNodeKind::Group;
        root.label = "Frame";

        // invariant: root order matches graph-execution order regardless of group encounter order.
        struct Entry
        {
            EventNode node;       // pass / cascade / group
            u32       sortKey;    // for groups, min graphPassIndex across members
        };
        std::vector<Entry> entries;
        entries.reserve(frame.passes.size());

        int shadowsEntryIdx = -1;
        int cullEntryIdx    = -1;

        for (u32 pi = 0; pi < frame.passes.size(); ++pi)
        {
            const auto& pass = frame.passes[pi];
            const u32 graphIdx = pass.graphPassIndex;
            EventNode passNode = BuildPassNode(frame, pi);

            // --- Group routing (explicit prefix registry, not split-on-dot) ---
            int cascadeIdx = CascadeIndexFromName(pass.name);
            if (cascadeIdx >= 0)
            {
                if (shadowsEntryIdx < 0)
                {
                    EventNode g;
                    g.kind  = EventNodeKind::Group;
                    g.label = "Shadows";
                    entries.push_back({ std::move(g), graphIdx });
                    shadowsEntryIdx = (int)entries.size() - 1;
                }
                else if (graphIdx < entries[shadowsEntryIdx].sortKey)
                {
                    entries[shadowsEntryIdx].sortKey = graphIdx;
                }
                passNode.kind         = EventNodeKind::Cascade;
                passNode.label        = "Cascade " + std::to_string(cascadeIdx);
                passNode.archiveLayer = cascadeIdx;
                entries[shadowsEntryIdx].node.children.push_back(std::move(passNode));
                continue;
            }

            if (pass.name.compare(0, 12, "FrustumCull.") == 0)
            {
                if (cullEntryIdx < 0)
                {
                    EventNode g;
                    g.kind  = EventNodeKind::Group;
                    g.label = "Frustum Culling";
                    entries.push_back({ std::move(g), graphIdx });
                    cullEntryIdx = (int)entries.size() - 1;
                }
                else if (graphIdx < entries[cullEntryIdx].sortKey)
                {
                    entries[cullEntryIdx].sortKey = graphIdx;
                }
                entries[cullEntryIdx].node.children.push_back(std::move(passNode));
                continue;
            }

            entries.push_back({ std::move(passNode), graphIdx });
        }

        std::stable_sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.sortKey < b.sortKey; });

        root.children.reserve(entries.size());
        for (auto& e : entries)
            root.children.push_back(std::move(e.node));

        PopulateLastDrawIndex(root);
        return root;
    }
}
