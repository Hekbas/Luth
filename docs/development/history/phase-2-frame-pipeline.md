# Phase 2: Frame Pipeline ✅ (2026-03-07)

| File | Changes |
|---|---|
| [FrameData.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/FrameData.h) | Complete FrameParams, global `MAX_FRAMES_IN_FLIGHT=3`, SpinLock, V6 overflow |
| [VulkanBackend.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/backend/vulkan/VulkanBackend.h) | Aligned to 3 frames, added `IsFrameComplete()` |
| [VulkanContext.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/backend/vulkan/VulkanContext.h) | Removed local `MAX_FRAMES_IN_FLIGHT=2` shadow |
| [Renderer.h/.cpp](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/renderer/Renderer.h) | Single FrameData owner (App delegates via pointer) |
| [App.cpp](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/App.cpp) | Pipelined loop structure |

### Commit
```
feat(renderer): pipelined frame structure & unified frame limits
```
