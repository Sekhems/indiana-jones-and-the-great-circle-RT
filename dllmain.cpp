// Vulkan proxy v3 for Windows x64 (vulkan-1.dll replacement that forwards to the
// real system loader).
//
// Adds to v2:
//   * Emulates VK_KHR_dynamic_rendering by translating vkCmdBeginRendering /
//     vkCmdEndRendering into a synthesized VkRenderPass + vkCmdBeginRenderPass /
//     vkCmdEndRenderPass pair. vkCreateGraphicsPipelines substitutes the
//     renderPass on pipelines that carry VkPipelineRenderingCreateInfo.
//   * Implements maintenance4 (vkGetDevice*MemoryRequirements) by creating a
//     temporary resource and querying its legacy memory requirements.
//   * Tracks VkImageView -> format and VkCommandBuffer -> VkDevice so
//     BeginRendering works even before a pipeline is bound (vkd3d-style order).
//   * The Vulkan 1.3 compatibility structs are still passed through by the
//     original v3 path. This is a known portability limitation: older ICDs are
//     not required to ignore unknown core-version sTypes safely.
//
// The v2 RT spoofing (features, extensions, stubs) is retained verbatim.

#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#ifdef _MSC_VER
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>

#include "vk_proxy_exports.generated.h"

// Draw/dispatch hooks (defined further below). They skip commands recorded while a FAKE/NULL pipeline is bound.
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t);


static_assert(sizeof(void*) == 8, "Build this proxy for x64.");

extern "C" {
    PVOID g_vkSlots[VK_PROXY_EXPORT_COUNT] = {};
}

// ---------------------------------------------------------------------------
// Forward declarations of proxy hooks (defined below, after the helpers).
// These must be visible to stub::Find, so they go before the anonymous
// namespace.
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipeline);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* pInfo);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* pInfo);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdEndRendering(VkCommandBuffer cb);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdEndRenderingKHR(VkCommandBuffer cb);

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImageView* pView);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* pAllocator);

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pInfo,
    VkCommandBuffer* pBuffers);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer* pBuffers);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetDeviceBufferMemoryRequirements(VkDevice device,
    const VkDeviceBufferMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetDeviceImageMemoryRequirements(VkDevice device,
    const VkDeviceImageMemoryRequirements* pInfo, VkMemoryRequirements2* pMemoryRequirements);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetDeviceImageSparseMemoryRequirements(VkDevice device,
    const VkDeviceImageMemoryRequirements* pInfo, uint32_t* pSparseMemoryRequirementCount,
    VkSparseImageMemoryRequirements2* pSparseMemoryRequirements);
	
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines);
	
extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator);

// Implemented further below (uniform shader profile hook).
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* pFeatures);
	
	

// ---------------------------------------------------------------------------
// Configuration + helpers
// ---------------------------------------------------------------------------
namespace {
    // Version reported to the game when the real driver reports less.
    const uint32_t kSpoofMinApiMajorMinor = VK_MAKE_API_VERSION(0, 1, 3, 0);

    // Extensions we advertise when the real device lacks them.
    constexpr const char* kFakeExtensions[] = {
        "VK_KHR_deferred_host_operations",
        "VK_KHR_pipeline_library",
        "VK_KHR_ray_tracing_pipeline",
        "VK_KHR_acceleration_structure",
        "VK_KHR_ray_query",
        "VK_KHR_dynamic_rendering"
    };
    constexpr size_t kFakeExtensionCount = sizeof(kFakeExtensions) / sizeof(kFakeExtensions[0]);

    // sType values written as numbers so this compiles with slightly older headers.
    constexpr uint32_t ST_VULKAN11_FEATURES        = 49;
    constexpr uint32_t ST_VULKAN11_PROPERTIES      = 50;
    constexpr uint32_t ST_VULKAN12_FEATURES        = 51;
    constexpr uint32_t ST_FLOAT16_INT8_FEAT        = 1000082000u;
    constexpr uint32_t ST_VULKAN13_FEATURES        = 53;
    constexpr uint32_t ST_DYNAMIC_RENDERING_FEAT   = 1000044003u;
    constexpr uint32_t ST_SYNCHRONIZATION2_FEAT    = 1000314007u;
    constexpr uint32_t ST_MAINTENANCE4_FEAT        = 1000413000u;
    constexpr uint32_t ST_AS_FEATURES              = 1000150013u;
    constexpr uint32_t ST_AS_PROPERTIES            = 1000150014u;
    constexpr uint32_t ST_RTP_FEATURES             = 1000347000u;
    constexpr uint32_t ST_RTP_PROPERTIES           = 1000347001u;
    constexpr uint32_t ST_RAY_QUERY_FEATURES       = 1000348013u;
    constexpr uint32_t ST_RT_MAINTENANCE1_FEATURES = 1000386000u;

    INIT_ONCE g_loaderInitOnce = INIT_ONCE_STATIC_INIT;
    HMODULE g_realLoader = nullptr;
    HMODULE g_thisModule = nullptr;
    PFN_vkGetInstanceProcAddr g_realGipa = nullptr;
    PFN_vkGetDeviceProcAddr g_realGdpa = nullptr;
    VkDevice g_lastDevice = nullptr;   // for resolving driver function names in crash reports
    std::vector<std::pair<uint32_t, uint32_t>> g_createdFamilies;   // (queue family, queue count) from vkCreateDevice
    SRWLOCK g_logLock = SRWLOCK_INIT;
    SRWLOCK g_stateLock = SRWLOCK_INIT;
    std::set<std::string>* g_onceKeys = nullptr;

    // Forward declarations used by the RT policy block (definitions follow below).
    void GetLogPath(wchar_t* path, size_t capacity) noexcept;
    void LogLine(const char* format, ...) noexcept;
    bool FirstTime(const std::string& key);
    template<typename T> T RealExport(const char* name) noexcept;
    bool QueryRealExtensions(VkPhysicalDevice pd, const char* layer, std::vector<VkExtensionProperties>& out);
    bool HasExt(const std::vector<VkExtensionProperties>& v, const char* name);

    // RT compatibility policy. Auto selects the strongest path the driver really supports.
    // Hybrid keeps native AS/RTP where available and virtualizes only missing components.
    // SoftwareCompat is deliberately a no-RT fallback: no-hit RayQuery rewrite + RT no-op stubs.
    enum RtCompatMode : LONG { RT_MODE_AUTO = 0, RT_MODE_NATIVE = 1, RT_MODE_HYBRID = 2, RT_MODE_SOFTWARE = 3 };
    enum RtCompatTier : LONG { RT_TIER_NATIVE = 0, RT_TIER_HYBRID = 1, RT_TIER_SOFTWARE = 2 };

    struct RtDeviceCaps {
        bool extAS = false, extRTP = false, extRQ = false;
        bool extDeferred = false, extPipelineLibrary = false, extSpirv14 = false;
        bool extRTMaintenance1 = false;
        bool extDynamicRendering = false;
        bool featureAS = false, featureASUpdateAfterBind = false;
        bool featureRTP = false, featureRTPIndirect = false, featureRTPCulling = false;
        bool featureRQ = false, featureBDA = false, featureDescriptorIndexing = false;
        bool nativeAS = false, nativeRTP = false, nativeRQ = false;
        bool virtualAS = false, virtualRTP = false, virtualRQ = false;
        bool virtualDeferred = false, virtualPipelineLibrary = false, virtualDynamicRendering = false;
        RtCompatTier tier = RT_TIER_SOFTWARE;
        uint32_t apiVersion = VK_API_VERSION_1_0;
        // ---- what the DRIVER really reports, independent of the RT policy ----
        // NOTE: when FollowHardwareCaps=1 these bits override the INI deny flags for
        // shader classification. That is deliberate and safe: the previously released
        // builds made the INI authoritative, so any NEW capability a card reports can
        // only move modules from "denied/fake" to "sent to the driver", which is the
        // direction that restores rendering (a working shader was previously faked).
        // Set FollowHardwareCaps=0 to get the old INI-authoritative behaviour back.
        // Capability denial for shader modules must follow the hardware, not the
        // RT compatibility mode and not a fixed INI list: otherwise a GTX 1060
        // (which has no RT but does have float16/int16) is treated exactly like
        // a GCN2 card, and the modules it produces stop matching spv_replace.
        bool realFloat16 = false;     // shaderFloat16
        bool realInt16 = false;       // shaderInt16
        bool realDotProduct = false;  // shaderIntegerDotProduct
        bool realFloat64 = false;     // shaderFloat64
        bool realInt64 = false;       // shaderInt64
        bool realStorage16 = false;   // storageBuffer16BitAccess / storageInputOutput16
        bool realStorage8 = false;    // storageBuffer8BitAccess
        bool realMesh = false;        // VK_EXT_mesh_shader
        bool capsProbed = false;
    };

    struct ActiveRtDevice {
        // Shader capabilities of the physical device this VkDevice was created from.
        // Stored per device so a second GPU in the machine is judged on its own bits.
        bool hwProbed = false;
        bool hwFloat16 = false;
        bool hwInt16 = false;
        bool hwDotProduct = false;
        bool hwMesh = false;
        bool nativeAS = false;
        bool nativeRTP = false;
        bool nativeRQ = false;
        bool nativeDeferred = false;
        bool virtualAS = false;
        bool virtualRTP = false;
        bool virtualRQ = false;
        bool virtualDeferred = false;
    };

    volatile LONG g_rtSpoof = 1; // compatibility mirror: 1 = fallback policy enabled
    volatile LONG g_rtCompatMode = RT_MODE_AUTO;
    // 1 = report "this GPU has no RT at all" on every machine. With FollowHardwareCaps
    // this makes the game generate the SAME RT-less shader variants everywhere, so a
    // single spv_replace folder is valid on GCN2, Pascal/Turing and Intel alike.
    volatile LONG g_forceVirtualRt = 0;
    // 1 = pin the game-visible shader feature booleans (Int16/Int64/Float64/8-bit
    // storage/dot product) to the reference profile, so every GPU asks the game for
    // the same shader permutations and one spv_replace folder fits all of them.
    // Read from [spirv] UniformShaderProfile (VulkanProxy.ini); see LoadSpvConfig.
    volatile LONG g_uniformShaderProfile = 0;

    inline bool UniformShaderProfileEnabled() noexcept {
        return InterlockedCompareExchange(&g_uniformShaderProfile, 0, 0) != 0;
    }
    INIT_ONCE g_rtCompatInitOnce = INIT_ONCE_STATIC_INIT;
    SRWLOCK g_rtCapsLock = SRWLOCK_INIT;
    SRWLOCK g_activeRtLock = SRWLOCK_INIT;
    std::map<VkPhysicalDevice, RtDeviceCaps> g_rtCapsCache;
    std::map<VkDevice, ActiveRtDevice> g_activeRtDevices;
    VkPhysicalDevice g_lastRtPhysicalDevice = VK_NULL_HANDLE;

    BOOL CALLBACK LoadRtCompatConfig(PINIT_ONCE, PVOID, PVOID*) noexcept {
        wchar_t iniPath[2048]{};
        GetLogPath(iniPath, _countof(iniPath));
        wchar_t* slash = wcsrchr(iniPath, L'\\');
        if (slash) wcscpy_s(slash + 1, _countof(iniPath) - static_cast<size_t>(slash + 1 - iniPath), L"VulkanProxy.ini");
        else wcscpy_s(iniPath, _countof(iniPath), L"VulkanProxy.ini");

        wchar_t mode[32]{};
        GetPrivateProfileStringW(L"compat", L"Mode", L"auto", mode, _countof(mode), iniPath);
        LONG selected = RT_MODE_AUTO;
        if (_wcsicmp(mode, L"native") == 0) selected = RT_MODE_NATIVE;
        else if (_wcsicmp(mode, L"hybrid") == 0) selected = RT_MODE_HYBRID;
        else if (_wcsicmp(mode, L"software") == 0 || _wcsicmp(mode, L"software_compat") == 0) selected = RT_MODE_SOFTWARE;
        const LONG forceVirtual = static_cast<LONG>(GetPrivateProfileIntW(L"compat", L"ForceVirtualRt", 0, iniPath));
        InterlockedExchange(&g_rtCompatMode, selected);
        InterlockedExchange(&g_rtSpoof, selected == RT_MODE_NATIVE ? 0 : 1);
        InterlockedExchange(&g_forceVirtualRt, forceVirtual != 0 ? 1 : 0);
        LogLine("RT capability policy: ForceVirtualRt=%ld (1 = no native RT reported on any GPU)", (long)g_forceVirtualRt);
        LogLine("RT compatibility policy: Mode=%ls (modeId=%ld, fallback=%ld, ini=%ls)",
            mode, selected, (long)g_rtSpoof, iniPath);
        return TRUE;
    }

    LONG GetRtCompatMode() noexcept {
        InitOnceExecuteOnce(&g_rtCompatInitOnce, LoadRtCompatConfig, nullptr, nullptr);
        return InterlockedCompareExchange(&g_rtCompatMode, 0, 0);
    }

    const char* RtTierName(RtCompatTier tier) noexcept {
        switch (tier) {
        case RT_TIER_NATIVE: return "NATIVE";
        case RT_TIER_HYBRID: return "HYBRID";
        default: return "SOFTWARE_COMPAT";
        }
    }

    // A tiny ABI-compatible feature probe: every feature struct used here begins
    // with sType/pNext followed by its first VkBool32 feature member.
    bool QueryFirstRtFeature(VkPhysicalDevice pd, uint32_t sType, bool* supported) {
        if (!supported) return false;
        *supported = false;
        auto real = RealExport<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
        if (!real) real = RealExport<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2KHR");
        if (!real) return false;
        struct FeatureProbe { VkStructureType sType; void* pNext; VkBool32 firstFeature; } probe{};
        probe.sType = static_cast<VkStructureType>(sType);
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &probe;
        real(pd, &f2);
        *supported = probe.firstFeature == VK_TRUE;
        return true;
    }

    // Vulkan 1.2 defines descriptorIndexing as a minimum feature bundle.
    // Vulkan 1.1 exposes the same functionality as individual feature bits in
    // VkPhysicalDeviceDescriptorIndexingFeaturesEXT. Extension presence alone
    // is not enough to claim that the minimum bundle is supported.
    bool SupportsMinimumDescriptorIndexing(
        const VkPhysicalDeviceFeatures2& core,
        const VkPhysicalDeviceDescriptorIndexingFeatures& f) noexcept {
        return
            core.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE &&
            core.features.shaderStorageBufferArrayDynamicIndexing == VK_TRUE &&
            f.shaderUniformTexelBufferArrayDynamicIndexing == VK_TRUE &&
            f.shaderStorageTexelBufferArrayDynamicIndexing == VK_TRUE &&
            f.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
            f.shaderStorageBufferArrayNonUniformIndexing == VK_TRUE &&
            f.shaderUniformTexelBufferArrayNonUniformIndexing == VK_TRUE &&
            f.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE &&
            f.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE &&
            f.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE &&
            f.descriptorBindingUniformTexelBufferUpdateAfterBind == VK_TRUE &&
            f.descriptorBindingStorageTexelBufferUpdateAfterBind == VK_TRUE &&
            f.descriptorBindingUpdateUnusedWhilePending == VK_TRUE &&
            f.descriptorBindingPartiallyBound == VK_TRUE &&
            f.runtimeDescriptorArray == VK_TRUE;
    }

    RtDeviceCaps ProbeRtDeviceCaps(VkPhysicalDevice pd) {
        RtDeviceCaps c{};
        std::vector<VkExtensionProperties> exts;
        if (!QueryRealExtensions(pd, nullptr, exts)) return c;

        c.extAS = HasExt(exts, "VK_KHR_acceleration_structure");
        c.extRTP = HasExt(exts, "VK_KHR_ray_tracing_pipeline");
        c.extRQ = HasExt(exts, "VK_KHR_ray_query");
        c.extDeferred = HasExt(exts, "VK_KHR_deferred_host_operations");
        c.extPipelineLibrary = HasExt(exts, "VK_KHR_pipeline_library");
        c.extSpirv14 = HasExt(exts, "VK_KHR_spirv_1_4");
        c.extDynamicRendering = HasExt(exts, "VK_KHR_dynamic_rendering");
        c.extRTMaintenance1 = HasExt(exts, "VK_KHR_ray_tracing_maintenance1");

        auto getProps = RealExport<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        if (getProps) {
            VkPhysicalDeviceProperties p{};
            getProps(pd, &p);
            c.apiVersion = p.apiVersion;
        }

        const uint32_t major = VK_API_VERSION_MAJOR(c.apiVersion);
        const uint32_t minor = VK_API_VERSION_MINOR(c.apiVersion);
        const bool api11 = major > 1 || (major == 1 && minor >= 1);
        const bool api12 = major > 1 || (major == 1 && minor >= 2);
        const bool api13 = major > 1 || (major == 1 && minor >= 3);
        c.extDynamicRendering = c.extDynamicRendering || api13;

        // Query the complete mandatory RT feature bits. Extension presence by
        // itself is not enough: GTX-class drivers can expose AS/RTP while not
        // exposing RayQuery, or can report an extension with a feature disabled.
        auto getFeatures2 = RealExport<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
        if (!getFeatures2)
            getFeatures2 = RealExport<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2KHR");
        if (getFeatures2) {
            bool hasDescriptorIndexingStruct = false;
            VkPhysicalDeviceFeatures2 f2{};
            f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

            VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{};
            asf.sType = static_cast<VkStructureType>(ST_AS_FEATURES);
            if (c.extAS) { asf.pNext = f2.pNext; f2.pNext = &asf; }

            VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp{};
            rtp.sType = static_cast<VkStructureType>(ST_RTP_FEATURES);
            if (c.extRTP) { rtp.pNext = f2.pNext; f2.pNext = &rtp; }

            VkPhysicalDeviceRayQueryFeaturesKHR rq{};
            rq.sType = static_cast<VkStructureType>(ST_RAY_QUERY_FEATURES);
            if (c.extRQ) { rq.pNext = f2.pNext; f2.pNext = &rq; }

            VkPhysicalDeviceVulkan12Features v12{};
            v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            VkPhysicalDeviceBufferDeviceAddressFeatures bda{};
            bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
            VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
            indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
            if (api12) {
                v12.pNext = f2.pNext;
                f2.pNext = &v12;
            } else if (api11) {
                if (HasExt(exts, "VK_KHR_buffer_device_address")) {
                    bda.pNext = f2.pNext;
                    f2.pNext = &bda;
                }
                if (HasExt(exts, "VK_EXT_descriptor_indexing")) {
                    indexing.pNext = f2.pNext;
                    f2.pNext = &indexing;
                    hasDescriptorIndexingStruct = true;
                }
            }

            getFeatures2(pd, &f2);
            if (c.extAS) {
                c.featureAS = asf.accelerationStructure == VK_TRUE;
                c.featureASUpdateAfterBind = asf.descriptorBindingAccelerationStructureUpdateAfterBind == VK_TRUE;
            }
            if (c.extRTP) {
                c.featureRTP = rtp.rayTracingPipeline == VK_TRUE;
                c.featureRTPIndirect = rtp.rayTracingPipelineTraceRaysIndirect == VK_TRUE;
                c.featureRTPCulling = rtp.rayTraversalPrimitiveCulling == VK_TRUE;
            }
            if (c.extRQ) c.featureRQ = rq.rayQuery == VK_TRUE;
            if (api12) {
                c.featureBDA = v12.bufferDeviceAddress == VK_TRUE;
                c.featureDescriptorIndexing = v12.descriptorIndexing == VK_TRUE;
            } else if (api11) {
                c.featureBDA = bda.bufferDeviceAddress == VK_TRUE;
                c.featureDescriptorIndexing = hasDescriptorIndexingStruct &&
                    SupportsMinimumDescriptorIndexing(f2, indexing);
            }
        }

        const bool asDependency = api12
            ? (c.extDeferred && c.featureBDA && c.featureDescriptorIndexing)
            : (api11 && HasExt(exts, "VK_EXT_descriptor_indexing") &&
               HasExt(exts, "VK_KHR_buffer_device_address") &&
               c.featureBDA && c.featureDescriptorIndexing);
        // Both KHR ray tracing extensions require SPIR-V 1.4 or Vulkan 1.2.
        const bool spirvDependency = api12 || c.extSpirv14;

        c.nativeAS = c.extAS && c.featureAS && c.featureASUpdateAfterBind && asDependency;
        const bool nativeRQCandidate = c.nativeAS && c.extRQ && c.featureRQ && spirvDependency;
        c.nativeRTP = c.nativeAS && c.extRTP && c.featureRTP && c.featureRTPIndirect &&
                      spirvDependency && (!nativeRQCandidate || c.featureRTPCulling);
        c.nativeRQ = nativeRQCandidate;

        const LONG mode = GetRtCompatMode();
        const bool allowFallback = mode != RT_MODE_NATIVE;
        const bool forceSoftware = mode == RT_MODE_SOFTWARE;
        // Uniform RT story: never let the game discover RT hardware (avoids a
        // completely different shader permutation on RTX / Arc / RDNA3).
        if (g_forceVirtualRt) { c.nativeAS = false; c.nativeRTP = false; c.nativeRQ = false; }
        c.virtualAS = allowFallback && (forceSoftware || !c.nativeAS);
        c.virtualRTP = allowFallback && (forceSoftware || !c.nativeRTP);
        c.virtualRQ = allowFallback && (forceSoftware || !c.nativeRQ);
        c.virtualDeferred = allowFallback && (forceSoftware || (!c.extDeferred && !c.nativeAS && !c.nativeRTP));
        c.virtualPipelineLibrary = allowFallback && (forceSoftware || (!c.extPipelineLibrary && !c.nativeRTP));
        // Dynamic Rendering is independent of RT mode: keep its existing
        // compatibility implementation available even in forced Native RT mode.
        c.virtualDynamicRendering = !c.extDynamicRendering;

        if (forceSoftware || (!c.nativeAS && !c.nativeRTP && !c.nativeRQ)) c.tier = RT_TIER_SOFTWARE;
        else if (c.nativeAS && c.nativeRTP && c.nativeRQ && mode != RT_MODE_SOFTWARE) c.tier = RT_TIER_NATIVE;
        else c.tier = RT_TIER_HYBRID;

        // Independent capability probe: ask the driver for the concrete shader
        // features and remember them on the device profile. Shader classification
        // later uses these bits instead of the "GCN2 has nothing" defaults, so an
        // NVIDIA/Intel card is no longer treated as if it were an R7 260X.
        if (getFeatures2) {
            VkPhysicalDeviceFeatures2 f2{}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            VkPhysicalDeviceVulkan12Features v12{}; v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            VkPhysicalDeviceShaderFloat16Int8Features f16i8{}; f16i8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
            VkPhysicalDeviceVulkan11Features v11{}; v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            VkPhysicalDevice8BitStorageFeatures s8{}; s8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES;
            VkPhysicalDeviceVulkan13Features v13{}; v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            if (api13) { v13.pNext = f2.pNext; f2.pNext = &v13; }
            if (api12) { v12.pNext = f2.pNext; f2.pNext = &v12; }
            if (api11) { v11.pNext = f2.pNext; f2.pNext = &v11; }
            s8.pNext = f2.pNext; f2.pNext = &s8;
            f16i8.pNext = f2.pNext; f2.pNext = &f16i8;
            getFeatures2(pd, &f2);
            c.realFloat16 = (api12 ? v12.shaderFloat16 : f16i8.shaderFloat16) == VK_TRUE;
            c.realInt16 = f2.features.shaderInt16 == VK_TRUE;
            c.realFloat64 = f2.features.shaderFloat64 == VK_TRUE;
            c.realInt64 = f2.features.shaderInt64 == VK_TRUE;
            c.realStorage16 = (api11 ? v11.storageBuffer16BitAccess : VK_FALSE) == VK_TRUE ||
                              v11.storageInputOutput16 == VK_TRUE;
            c.realStorage8 = s8.storageBuffer8BitAccess == VK_TRUE;
            if (api13) c.realDotProduct = v13.shaderIntegerDotProduct == VK_TRUE;
            c.capsProbed = true;
        }
        c.realMesh = HasExt(exts, "VK_EXT_mesh_shader") || HasExt(exts, "VK_NV_mesh_shader");

        LogLine("RT feature probe: AS=%u AS_UAB=%u BDA=%u DescriptorIndexingMin=%u RTP=%u RTP_Indirect=%u RTP_Culling=%u RQ=%u SPIRV14=%u",
            c.featureAS, c.featureASUpdateAfterBind, c.featureBDA, c.featureDescriptorIndexing,
            c.featureRTP, c.featureRTPIndirect, c.featureRTPCulling, c.featureRQ, c.extSpirv14);
        return c;
    }

    RtDeviceCaps GetRtDeviceCaps(VkPhysicalDevice pd) {
        GetRtCompatMode();
        g_lastRtPhysicalDevice = pd;
        AcquireSRWLockShared(&g_rtCapsLock);
        auto it = g_rtCapsCache.find(pd);
        if (it != g_rtCapsCache.end()) {
            RtDeviceCaps cached = it->second;
            ReleaseSRWLockShared(&g_rtCapsLock);
            return cached;
        }
        ReleaseSRWLockShared(&g_rtCapsLock);

        RtDeviceCaps probed = ProbeRtDeviceCaps(pd);
        AcquireSRWLockExclusive(&g_rtCapsLock);
        auto inserted = g_rtCapsCache.emplace(pd, probed);
        RtDeviceCaps result = inserted.first->second;
        ReleaseSRWLockExclusive(&g_rtCapsLock);
        if (inserted.second) {
            LogLine("GPU SHADER PROFILE: Float16=%u Int16=%u DotProduct=%u Float64=%u Int64=%u Storage16=%u Storage8=%u Mesh=%u (probed=%u)",
                result.realFloat16, result.realInt16, result.realDotProduct, result.realFloat64,
                result.realInt64, result.realStorage16, result.realStorage8, result.realMesh, result.capsProbed);
            LogLine("GPU RT PROFILE: tier=%s API=%u.%u AS(ext=%u feature=%u native=%u virtual=%u) RTP(ext=%u feature=%u native=%u virtual=%u) RQ(ext=%u feature=%u native=%u virtual=%u)",
                RtTierName(result.tier), VK_API_VERSION_MAJOR(result.apiVersion), VK_API_VERSION_MINOR(result.apiVersion),
                result.extAS, result.featureAS, result.nativeAS, result.virtualAS,
                result.extRTP, result.featureRTP, result.nativeRTP, result.virtualRTP,
                result.extRQ, result.featureRQ, result.nativeRQ, result.virtualRQ);
        }
        return result;
    }

    bool ShouldVirtualizeRtExtension(const char* name, const RtDeviceCaps& c) noexcept {
        if (!name) return false;
        // Dynamic rendering is an independent compatibility feature; keep it
        // emulated even when the user selects native RT mode.
        if (strcmp(name, "VK_KHR_dynamic_rendering") == 0) return c.virtualDynamicRendering;
        if (!g_rtSpoof) return false;
        if (strcmp(name, "VK_KHR_acceleration_structure") == 0) return c.virtualAS;
        if (strcmp(name, "VK_KHR_ray_tracing_pipeline") == 0) return c.virtualRTP;
        if (strcmp(name, "VK_KHR_ray_query") == 0) return c.virtualRQ;
        if (strcmp(name, "VK_KHR_deferred_host_operations") == 0) return c.virtualDeferred;
        if (strcmp(name, "VK_KHR_pipeline_library") == 0) return c.virtualPipelineLibrary && (c.virtualRTP || c.tier == RT_TIER_SOFTWARE);
        return false;
    }

    void RegisterActiveRtDevice(VkDevice device, const RtDeviceCaps& c) {
        if (!device) return;
        ActiveRtDevice a{};
        a.hwProbed = c.capsProbed;
        a.hwFloat16 = c.realFloat16;
        a.hwInt16 = c.realInt16;
        a.hwDotProduct = c.realDotProduct;
        a.hwMesh = c.realMesh;
        a.nativeAS = c.nativeAS && !c.virtualAS;
        a.nativeRTP = c.nativeRTP && !c.virtualRTP;
        a.nativeRQ = c.nativeRQ && !c.virtualRQ;
        a.nativeDeferred = c.extDeferred && !c.virtualDeferred;
        a.virtualAS = c.virtualAS;
        a.virtualRTP = c.virtualRTP;
        a.virtualRQ = c.virtualRQ;
        a.virtualDeferred = c.virtualDeferred;
        AcquireSRWLockExclusive(&g_activeRtLock);
        g_activeRtDevices[device] = a;
        ReleaseSRWLockExclusive(&g_activeRtLock);
    }

    ActiveRtDevice GetActiveRtDevice(VkDevice device) {
        AcquireSRWLockShared(&g_activeRtLock);
        auto it = g_activeRtDevices.find(device);
        ActiveRtDevice a = it != g_activeRtDevices.end() ? it->second : ActiveRtDevice{};
        ReleaseSRWLockShared(&g_activeRtLock);
        return a;
    }

    bool CanUseNativeRtProc(VkDevice device, const char* name, bool fromGipa) {
        if (!name || GetRtCompatMode() == RT_MODE_SOFTWARE) return false;
        ActiveRtDevice active{};
        if (device) active = GetActiveRtDevice(device);
        else if (g_lastRtPhysicalDevice) {
            const RtDeviceCaps c = GetRtDeviceCaps(g_lastRtPhysicalDevice);
            active.nativeAS = c.nativeAS && !c.virtualAS;
            active.nativeRTP = c.nativeRTP && !c.virtualRTP;
            active.nativeRQ = c.nativeRQ && !c.virtualRQ;
            active.nativeDeferred = c.extDeferred && !c.virtualDeferred;
        } else {
            // Before a VkPhysicalDevice/VkDevice is selected, ask the real
            // loader first rather than prematurely caching a compatibility stub.
            return fromGipa;
        }
        if (strstr(name, "RayQuery")) return active.nativeRQ;
        if (strstr(name, "AccelerationStructure")) return active.nativeAS;
        if (strstr(name, "RayTracing") || strstr(name, "TraceRays")) return active.nativeRTP;
        if (strstr(name, "DeferredOperation")) return active.nativeDeferred;
        return fromGipa;
    }

    bool CanUseRtFallback(VkDevice device, const char* name, bool fromGipa) {
        if (!name || !g_rtSpoof) return false;
        ActiveRtDevice active{};
        if (device) active = GetActiveRtDevice(device);
        else if (g_lastRtPhysicalDevice) {
            const RtDeviceCaps c = GetRtDeviceCaps(g_lastRtPhysicalDevice);
            active.virtualAS = c.virtualAS;
            active.virtualRTP = c.virtualRTP;
            active.virtualRQ = c.virtualRQ;
            active.virtualDeferred = c.virtualDeferred;
        } else {
            return fromGipa;
        }
        if (strstr(name, "RayQuery")) return active.virtualRQ;
        if (strstr(name, "AccelerationStructure")) return active.virtualAS;
        if (strstr(name, "RayTracing") || strstr(name, "TraceRays")) return active.virtualRTP;
        if (strstr(name, "DeferredOperation")) return active.virtualDeferred;
        return false;
    }


    // -----------------------------------------------------------------------
    // Logging
    // -----------------------------------------------------------------------
    void GetLogPath(wchar_t* path, size_t capacity) noexcept {
        if (!path || capacity == 0) return;
        path[0] = L'\0';
        DWORD length = g_thisModule ? GetModuleFileNameW(g_thisModule, path, static_cast<DWORD>(capacity)) : 0;
        if (length == 0 || length >= capacity) {
            wcscpy_s(path, capacity, L"VulkanProxy_diagnostic.log");
            return;
        }
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) {
            wcscpy_s(path, capacity, L"VulkanProxy_diagnostic.log");
            return;
        }
        const size_t offset = static_cast<size_t>(slash - path) + 1;
        constexpr wchar_t fileName[] = L"VulkanProxy_diagnostic.log";
        if (offset + (sizeof(fileName) / sizeof(fileName[0])) > capacity) {
            wcscpy_s(path, capacity, L"VulkanProxy_diagnostic.log");
            return;
        }
        memcpy(path + offset, fileName, sizeof(fileName));
    }

    volatile LONG g_logVerbose = 0;   // VulkanProxy.ini [spirv] Verbose=1 disables the noise filter

    // Per-call chatter that makes the log huge and tells us nothing.
    bool IsLogNoise(const char* m) noexcept {
        if (strstr(m, "FAIL") || strstr(m, "fail") || strstr(m, "CAUGHT") || strstr(m, "!!!") ||
            strstr(m, "ERROR") || strstr(m, "error")) return false;
        static const char* const kNoise[] = {
            "vkAllocateDescriptorSets", "vkUpdateDescriptorSets", "vkGetImageMemoryRequirements",
            "vkCreateSampler", "vkCreateGraphicsPipelines ENTER", "  pipeline[", "vkCreatePipelineLayout:",
            "vkCreateDescriptorSetLayout", "vkCreateDescriptorPool", "vkCreateCommandPool", "vkCreateFence",
            "vkCreateRenderPass", "vkCreateImageView", "vkCreateImage", "vkCreateBuffer", "vkAllocateMemory",
            "vkBindImageMemory", "vkBindBufferMemory", "vkCmd", "vkQueueSubmit",
            "RESOURCE ENTER", "RESOURCE EXIT" };
        for (const char* n : kNoise) if (strncmp(m, n, strlen(n)) == 0) return true;
        return false;
    }

    void LogLine(const char* format, ...) noexcept {
        char message[2048]{};
        va_list args;
        va_start(args, format);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
        va_end(args);
        if (!g_logVerbose && IsLogNoise(message)) return;

        char line[2304]{};
        sprintf_s(line, sizeof(line), "[%llu] %s\r\n",
            static_cast<unsigned long long>(GetTickCount64()), message);
        OutputDebugStringA(line);

        wchar_t logPath[2048]{};
        GetLogPath(logPath, sizeof(logPath) / sizeof(logPath[0]));
        AcquireSRWLockExclusive(&g_logLock);
        HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
            CloseHandle(file);
        }
        ReleaseSRWLockExclusive(&g_logLock);
    }

    // True the first time `key` is seen. Used to keep the log readable.
    bool FirstTime(const std::string& key) {
        AcquireSRWLockExclusive(&g_stateLock);
        if (!g_onceKeys) g_onceKeys = new (std::nothrow) std::set<std::string>();
        bool inserted = g_onceKeys ? g_onceKeys->insert(key).second : false;
        ReleaseSRWLockExclusive(&g_stateLock);
        return inserted;
    }

    // -----------------------------------------------------------------------
    // System loader
    // -----------------------------------------------------------------------
    BOOL CALLBACK LoadSystemVulkanLoader(PINIT_ONCE, PVOID, PVOID*) noexcept {
        wchar_t systemDirectory[2048]{};
        const UINT capacity = static_cast<UINT>(sizeof(systemDirectory) / sizeof(systemDirectory[0]));
        const UINT length = GetSystemDirectoryW(systemDirectory, capacity);
        if (length == 0 || length >= capacity) {
            LogLine("GetSystemDirectoryW failed; Win32 error=%lu", GetLastError());
            return TRUE;
        }
        size_t pathLength = static_cast<size_t>(length);
        if (systemDirectory[pathLength - 1] != L'\\') {
            if (pathLength + 1 >= capacity) return TRUE;
            systemDirectory[pathLength++] = L'\\';
        }
        constexpr wchar_t loaderName[] = L"vulkan-1.dll";
        constexpr size_t loaderNameChars = sizeof(loaderName) / sizeof(loaderName[0]);
        if (pathLength + loaderNameChars > capacity) return TRUE;
        memcpy(systemDirectory + pathLength, loaderName, sizeof(loaderName));

        g_realLoader = LoadLibraryExW(systemDirectory, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!g_realLoader) {
            LogLine("Failed to load system Vulkan Loader: %lu", GetLastError());
            return TRUE;
        }
        g_realGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(g_realLoader, "vkGetInstanceProcAddr"));
        g_realGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(g_realLoader, "vkGetDeviceProcAddr"));
        if (!g_realGipa || !g_realGdpa) {
            LogLine("System Loader missing vkGetInstanceProcAddr or vkGetDeviceProcAddr");
            g_realGipa = nullptr;
            g_realGdpa = nullptr;
            return TRUE;
        }
        LogLine("=== VulkanProxy v16 RT-TIERS === Loaded system Loader: %ls", systemDirectory);
        return TRUE;
    }

    bool EnsureSystemLoader() noexcept {
        if (!InitOnceExecuteOnce(&g_loaderInitOnce, LoadSystemVulkanLoader, nullptr, nullptr)) return false;
        return g_realLoader && g_realGipa && g_realGdpa;
    }

    template<typename T>
    T RealExport(const char* name) noexcept {
        if (!EnsureSystemLoader()) return nullptr;
        return reinterpret_cast<T>(GetProcAddress(g_realLoader, name));
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    bool IsFakeExtension(const char* name) noexcept {
        if (!name) return false;
        for (const char* e : kFakeExtensions) if (strcmp(name, e) == 0) return true;
        return false;
    }

    bool IsRayTracingProc(const char* name) noexcept {
        if (!name) return false;
        return strstr(name, "RayTracing") || strstr(name, "AccelerationStructure") ||
               strstr(name, "TraceRays") || strstr(name, "RayQuery") ||
               strstr(name, "DeferredOperation");
    }

    // Real (unmodified) device extension list.
    bool QueryRealExtensions(VkPhysicalDevice pd, const char* layer,
                             std::vector<VkExtensionProperties>& out) {
        out.clear();
        auto real = RealExport<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        if (!real) return false;
        uint32_t count = 0;
        VkResult r = real(pd, layer, &count, nullptr);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count > 8192u) return false;
        out.resize(count);
        r = real(pd, layer, &count, out.data());
        if (r != VK_SUCCESS && r != VK_INCOMPLETE) { out.clear(); return false; }
        out.resize(count);
        return true;
    }

    bool HasExt(const std::vector<VkExtensionProperties>& v, const char* name) {
        for (const auto& e : v) if (strcmp(e.extensionName, name) == 0) return true;
        return false;
    }

    // Does this physical device REALLY support RT (so we must not spoof)?
    bool DeviceHasRealRT(VkPhysicalDevice pd) {
        const RtDeviceCaps c = GetRtDeviceCaps(pd);
        return c.nativeAS && c.nativeRTP && c.nativeRQ;
    }

    const char* KnownSTypeName(uint32_t s) {
        switch (s) {
        case 49: return "Vulkan11Features";
        case 51: return "Vulkan12Features";
        case ST_VULKAN13_FEATURES: return "Vulkan13Features";
        case 1000280000u: return "ShaderIntegerDotProductFeatures";
        case ST_DYNAMIC_RENDERING_FEAT: return "DynamicRenderingFeatures";
        case ST_SYNCHRONIZATION2_FEAT: return "Synchronization2Features";
        case ST_MAINTENANCE4_FEAT: return "Maintenance4Features";
        case ST_AS_FEATURES: return "AccelerationStructureFeaturesKHR";
        case ST_RTP_FEATURES: return "RayTracingPipelineFeaturesKHR";
        case ST_RAY_QUERY_FEATURES: return "RayQueryFeaturesKHR";
        case ST_RT_MAINTENANCE1_FEATURES: return "RayTracingMaintenance1FeaturesKHR";
        case ST_AS_PROPERTIES: return "AccelerationStructurePropertiesKHR";
        case ST_RTP_PROPERTIES: return "RayTracingPipelinePropertiesKHR";
        case 1000059000u: return "Features2";
        case 1000059001u: return "Properties2";
        case 50: return "Vulkan11Properties";
        case 52: return "Vulkan12Properties";
        case ST_FLOAT16_INT8_FEAT: return "ShaderFloat16Int8Features";
        case 1000094000u: return "SubgroupProperties";
        case 1000286000u: return "Robustness2Features";
        default: return "";
        }
    }

    // Log every distinct sType the application puts in a chain, once.
    void LogChainOnce(const char* label, const void* pNext) {
        const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pNext);
        unsigned n = 0;
        while (node && n < 64) {
            const uint32_t s = static_cast<uint32_t>(node->sType);
            char key[96];
            sprintf_s(key, sizeof(key), "chain:%s:%u", label, s);
            if (FirstTime(key)) LogLine("%s requests sType=%u %s", label, s, KnownSTypeName(s));
            node = node->pNext;
            ++n;
        }
    }

    // Fake non-dispatchable handles carry a recognisable tag in the top 16 bits.
    constexpr uint64_t kFakeTag = 0xFA4Eull;
    volatile LONG64 g_fakeCounter = 0;

    template<typename H> H MakeFake() {
        const uint64_t v = (kFakeTag << 48) | static_cast<uint64_t>(InterlockedIncrement64(&g_fakeCounter));
        return reinterpret_cast<H>(static_cast<uintptr_t>(v));
    }
    template<typename H> bool IsFake(H h) {
        return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(h)) >> 48) == kFakeTag;
    }

    // -----------------------------------------------------------------------
    // Spoofing of properties / features
    // -----------------------------------------------------------------------
    void FixupProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
        if (!p) return;
        const uint32_t real = p->apiVersion;
        char key[64];
        sprintf_s(key, sizeof(key), "gpu:%p", static_cast<void*>(pd));
        if (FirstTime(key)) {
            LogLine("GPU: name=\"%s\" vendor=0x%04X device=0x%04X type=%u driverVersion=0x%08X apiVersion(real)=%u.%u.%u",
                p->deviceName, p->vendorID, p->deviceID, static_cast<unsigned>(p->deviceType),
                p->driverVersion, VK_API_VERSION_MAJOR(real), VK_API_VERSION_MINOR(real),
                VK_API_VERSION_PATCH(real));
        }
        const uint32_t realMM = real & ~0xFFFu;
        if (realMM < kSpoofMinApiMajorMinor) {
            p->apiVersion = kSpoofMinApiMajorMinor | (real & 0xFFFu);
            if (FirstTime("spoof:apiVersion"))
                LogLine("SPOOF: apiVersion %u.%u -> 1.3", VK_API_VERSION_MAJOR(real), VK_API_VERSION_MINOR(real));
        }
    }

    void FixupPropertiesChain(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p2) {
        const RtDeviceCaps rtCaps = GetRtDeviceCaps(pd);
        const bool spoofPipeline = rtCaps.virtualRTP;
        const bool spoofAS = rtCaps.virtualAS;
        auto* node = reinterpret_cast<VkBaseOutStructure*>(p2->pNext);
        unsigned i = 0;
        while (node && i < 64) {
            const uint32_t s = static_cast<uint32_t>(node->sType);
            if (s == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES && spoofPipeline) {
                auto* sg = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(node);
                const VkShaderStageFlags before = sg->supportedStages;
                sg->supportedStages |= 0xF00u;
                if (FirstTime("spoof:subgroup"))
                    LogLine("SPOOF: subgroup supportedStages 0x%X -> 0x%X (size=%u ops=0x%X)",
                        static_cast<unsigned>(before), static_cast<unsigned>(sg->supportedStages),
                        sg->subgroupSize, static_cast<unsigned>(sg->supportedOperations));
            } else if (s == ST_VULKAN11_PROPERTIES && spoofPipeline) {
                auto* p = reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(node);
                const VkShaderStageFlags before = p->subgroupSupportedStages;
                p->subgroupSupportedStages |= 0xF00u;
                if (FirstTime("spoof:subgroup11"))
                    LogLine("SPOOF: Vulkan11Properties subgroupSupportedStages 0x%X -> 0x%X",
                        static_cast<unsigned>(before), static_cast<unsigned>(p->subgroupSupportedStages));
            } else if (s == ST_RTP_PROPERTIES && spoofPipeline) {
                auto* p = reinterpret_cast<VkPhysicalDeviceRayTracingPipelinePropertiesKHR*>(node);
                p->shaderGroupHandleSize = 32;
                p->maxRayRecursionDepth = 31;
                p->maxShaderGroupStride = 4096;
                p->shaderGroupBaseAlignment = 64;
                p->shaderGroupHandleCaptureReplaySize = 32;
                p->maxRayDispatchInvocationCount = 1u << 30;
                p->shaderGroupHandleAlignment = 32;
                p->maxRayHitAttributeSize = 32;
                if (FirstTime("spoof:rtpprops")) LogLine("SPOOF: RayTracingPipelineProperties filled");
            } else if (s == ST_AS_PROPERTIES && spoofAS) {
                auto* p = reinterpret_cast<VkPhysicalDeviceAccelerationStructurePropertiesKHR*>(node);
                p->maxGeometryCount = (1ull << 24) - 1;
                p->maxInstanceCount = (1ull << 24) - 1;
                p->maxPrimitiveCount = (1ull << 29) - 1;
                p->maxPerStageDescriptorAccelerationStructures = 16;
                p->maxPerStageDescriptorUpdateAfterBindAccelerationStructures = 16;
                p->maxDescriptorSetAccelerationStructures = 16;
                p->maxDescriptorSetUpdateAfterBindAccelerationStructures = 16;
                p->minAccelerationStructureScratchOffsetAlignment = 128;
                if (FirstTime("spoof:asprops")) LogLine("SPOOF: AccelerationStructureProperties filled");
            }
            node = node->pNext;
            ++i;
        }
    }

    inline void SetBool(void* node, size_t index) {
        reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(node) + 16)[index] = VK_TRUE;
    }

    inline void ClearBool(void* node, size_t index) {
        reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(node) + 16)[index] = VK_FALSE;
    }

    // Uniform shader feature profile.
    //
    // The game compiles its shader permutations from the feature bits the driver
    // reports. If a GTX 1060 says "shaderInt16 = yes" while the R7 260X says "no",
    // the game emits a DIFFERENT module for the same material; the file name
    // spv_<hash of the original>.spv can then never match and the replacement
    // (with the working lighting) is silently skipped.
    //
    // Pinning the game-visible booleans to the profile of the reference card makes
    // every GPU request the same shader set, so one spv_replace folder is valid
    // everywhere. Only booleans are pinned (never sizes/limits), so nothing can
    // become inconsistent: the game simply does not ask for variants it does not
    // need. On the reference GCN2 card the pinned values already equal the real
    // ones, so its behaviour is unchanged.
    bool g_uniformProfileLogged = false;

    void ApplyUniformShaderProfile(VkPhysicalDeviceFeatures* core) {
        if (!core || !UniformShaderProfileEnabled()) return;
        bool changed = false;
        // Reference profile: no 16-bit integers, no 64-bit ALU, no 8-bit storage,
        // no dot product, no float64. (float16 on/off is handled by the existing
        // spoof below: the reference build always reported shaderFloat16 = true.)
        if (core->shaderInt16)  { core->shaderInt16 = VK_FALSE;  changed = true; }
        if (core->shaderInt64)  { core->shaderInt64 = VK_FALSE;  changed = true; }
        if (core->shaderFloat64){ core->shaderFloat64 = VK_FALSE; changed = true; }
        if (changed && !g_uniformProfileLogged) {
            g_uniformProfileLogged = true;
            LogLine("SPOOF: uniform shader profile - cleared shaderInt16/shaderInt64/shaderFloat64 so this GPU requests the same shader variants as the reference card");
        }
    }

    void FixupFeaturesChain(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* f2) {
        const RtDeviceCaps rtCaps = GetRtDeviceCaps(pd);
        auto* node = reinterpret_cast<VkBaseOutStructure*>(f2->pNext);
        unsigned i = 0;
        while (node && i < 64) {
            const uint32_t s = static_cast<uint32_t>(node->sType);
            if (s == ST_VULKAN12_FEATURES) {
                auto* f = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node);
                if (FirstTime("real:vk12features"))
                    LogLine("REAL Vulkan12Features: shaderFloat16=%u shaderInt8=%u storageBuffer8BitAccess=%u bufferDeviceAddress=%u descriptorIndexing=%u sampledImageNonUniform=%u storageImageNonUniform=%u runtimeDescriptorArray=%u partiallyBound=%u sampledImageUAB=%u storageImageUAB=%u timelineSemaphore=%u scalarBlockLayout=%u",
                        f->shaderFloat16, f->shaderInt8, f->storageBuffer8BitAccess, f->bufferDeviceAddress,
                        f->descriptorIndexing, f->shaderSampledImageArrayNonUniformIndexing,
                        f->shaderStorageImageArrayNonUniformIndexing, f->runtimeDescriptorArray,
                        f->descriptorBindingPartiallyBound, f->descriptorBindingSampledImageUpdateAfterBind,
                        f->descriptorBindingStorageImageUpdateAfterBind, f->timelineSemaphore, f->scalarBlockLayout);
                if (!f->shaderFloat16 && FirstTime("spoof:fp16"))
                    LogLine("SPOOF: Vulkan12Features.shaderFloat16 0 -> 1 (will rewrite fp16 shaders to fp32)");
                f->shaderFloat16 = VK_TRUE;
                if (UniformShaderProfileEnabled()) {
                    // 8-bit storage / dot product change the shader variants the game
                    // asks for; pin them to the reference profile.
                    f->shaderInt8 = VK_FALSE;
                    f->storageBuffer8BitAccess = VK_FALSE;
                    f->uniformAndStorageBuffer8BitAccess = VK_FALSE;
                    f->storagePushConstant8 = VK_FALSE;
                }
            } else if (s == ST_FLOAT16_INT8_FEAT) {
                auto* f = reinterpret_cast<VkPhysicalDeviceShaderFloat16Int8Features*>(node);
                if (UniformShaderProfileEnabled()) f->shaderInt8 = VK_FALSE;
                if (FirstTime("real:f16i8"))
                    LogLine("REAL ShaderFloat16Int8Features: shaderFloat16=%u shaderInt8=%u", f->shaderFloat16, f->shaderInt8);
                f->shaderFloat16 = VK_TRUE;
            } else if (s == ST_VULKAN11_FEATURES) {
                auto* f = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(node);
                if (FirstTime("real:vk11features"))
                    LogLine("REAL Vulkan11Features: storageBuffer16BitAccess=%u uniformAndStorageBuffer16BitAccess=%u storagePushConstant16=%u storageInputOutput16=%u multiview=%u shaderDrawParameters=%u",
                        f->storageBuffer16BitAccess, f->uniformAndStorageBuffer16BitAccess,
                        f->storagePushConstant16, f->storageInputOutput16, f->multiview, f->shaderDrawParameters);
                if (UniformShaderProfileEnabled()) {
                    // 16-bit storage changes how a shader packs data, so it also changes
                    // the module the game compiles. Pin it to the reference profile.
                    f->storageBuffer16BitAccess = VK_FALSE;
                    f->uniformAndStorageBuffer16BitAccess = VK_FALSE;
                    f->storagePushConstant16 = VK_FALSE;
                    f->storageInputOutput16 = VK_FALSE;
                }
            } else if (s == ST_VULKAN13_FEATURES) {
                auto* f = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(node);
                if (FirstTime("real:vk13features"))
                    LogLine("REAL Vulkan13Features: dynamicRendering=%u synchronization2=%u maintenance4=%u subgroupSizeControl=%u computeFullSubgroups=%u shaderIntegerDotProduct=%u",
                        f->dynamicRendering, f->synchronization2, f->maintenance4,
                        f->subgroupSizeControl, f->computeFullSubgroups, f->shaderIntegerDotProduct);
                f->dynamicRendering = VK_TRUE;
                f->synchronization2 = VK_TRUE;
                f->maintenance4 = VK_TRUE;
                if (UniformShaderProfileEnabled()) f->shaderIntegerDotProduct = VK_FALSE;
            } else if (s == ST_DYNAMIC_RENDERING_FEAT) {
                if (FirstTime("real:dynrendfeat"))
                    LogLine("REAL DynamicRenderingFeatures: dynamicRendering=%u",
                        reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(node)->dynamicRendering);
                SetBool(node, 0);
            } else if (s == ST_SYNCHRONIZATION2_FEAT) {
                SetBool(node, 0);
            } else if (s == ST_MAINTENANCE4_FEAT) {
                SetBool(node, 0);
            } else if (UniformShaderProfileEnabled() && s == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES) {
                memset(reinterpret_cast<uint8_t*>(node) + 16, 0, sizeof(VkPhysicalDevice8BitStorageFeatures) - 16);
            } else if (UniformShaderProfileEnabled() && s == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
                memset(reinterpret_cast<uint8_t*>(node) + 16, 0, sizeof(VkPhysicalDevice16BitStorageFeatures) - 16);
            } else if (rtCaps.virtualAS && s == ST_AS_FEATURES) {
                SetBool(node, 0); SetBool(node, 4);
                if (FirstTime("spoof:asfeat")) LogLine("SPOOF: AccelerationStructureFeatures = TRUE");
            } else if (rtCaps.virtualRTP && s == ST_RTP_FEATURES) {
                // Advertise only the mandatory pipeline switches used by the proxy.
                // Primitive culling is optional and is not implemented by the fallback.
                SetBool(node, 0); SetBool(node, 3);
                if (FirstTime("spoof:rtpfeat")) LogLine("SPOOF: RayTracingPipelineFeatures = TRUE");
            } else if (rtCaps.virtualRQ && s == ST_RAY_QUERY_FEATURES) {
                SetBool(node, 0);
                if (FirstTime("spoof:rqfeat")) LogLine("SPOOF: RayQueryFeatures = TRUE");
            }
            node = node->pNext;
            ++i;
        }
    }

    class PNextChainFilter {
        struct Entry {
            VkBaseOutStructure** link = nullptr;
            VkBaseOutStructure* node = nullptr;
        };
        Entry removed_[64]{};
        size_t count_ = 0;

        static void ZeroPayload(VkBaseOutStructure* node, size_t size) noexcept {
            if (!node || size <= sizeof(VkBaseOutStructure)) return;
            memset(reinterpret_cast<uint8_t*>(node) + sizeof(VkBaseOutStructure), 0,
                   size - sizeof(VkBaseOutStructure));
        }

    public:
        PNextChainFilter() = default;
        PNextChainFilter(const PNextChainFilter&) = delete;
        PNextChainFilter& operator=(const PNextChainFilter&) = delete;

        template<typename Predicate>
        void Apply(VkBaseOutStructure** root, Predicate shouldRemove) noexcept {
            if (!root) return;
            VkBaseOutStructure** link = root;
            unsigned visited = 0;
            while (*link && visited++ < 64) {
                VkBaseOutStructure* node = *link;
                size_t size = 0;
                if (shouldRemove(static_cast<uint32_t>(node->sType), &size) &&
                    count_ < (sizeof(removed_) / sizeof(removed_[0]))) {
                    ZeroPayload(node, size);
                    removed_[count_++] = { link, node };
                    *link = node->pNext;
                    continue;
                }
                link = &node->pNext;
            }
        }

        ~PNextChainFilter() noexcept {
            // Reverse order restores consecutive removed nodes correctly.
            while (count_ > 0) {
                const Entry& e = removed_[--count_];
                *e.link = e.node;
            }
        }
    };

    static bool RtFeatureStructSize(uint32_t sType, size_t* size) noexcept {
        if (!size) return false;
        switch (sType) {
        case ST_AS_FEATURES: *size = sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR); return true;
        case ST_RTP_FEATURES: *size = sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR); return true;
        case ST_RAY_QUERY_FEATURES: *size = sizeof(VkPhysicalDeviceRayQueryFeaturesKHR); return true;
        case ST_RT_MAINTENANCE1_FEATURES: *size = sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR); return true;
        default: return false;
        }
    }

    static bool RtPropertyStructSize(uint32_t sType, size_t* size) noexcept {
        if (!size) return false;
        switch (sType) {
        case ST_AS_PROPERTIES: *size = sizeof(VkPhysicalDeviceAccelerationStructurePropertiesKHR); return true;
        case ST_RTP_PROPERTIES: *size = sizeof(VkPhysicalDeviceRayTracingPipelinePropertiesKHR); return true;
        default: return false;
        }
    }

    const char* const kNamesCore[] = { "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat", "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing", "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance", "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries" };
    const char* const kNames11[] = { "storageBuffer16BitAccess", "uniformAndStorageBuffer16BitAccess", "storagePushConstant16", "storageInputOutput16", "multiview", "multiviewGeometryShader", "multiviewTessellationShader", "variablePointersStorageBuffer", "variablePointers", "protectedMemory", "samplerYcbcrConversion", "shaderDrawParameters" };
    const char* const kNames12[] = { "samplerMirrorClampToEdge", "drawIndirectCount", "storageBuffer8BitAccess", "uniformAndStorageBuffer8BitAccess", "storagePushConstant8", "shaderBufferInt64Atomics", "shaderSharedInt64Atomics", "shaderFloat16", "shaderInt8", "descriptorIndexing", "shaderInputAttachmentArrayDynamicIndexing", "shaderUniformTexelBufferArrayDynamicIndexing", "shaderStorageTexelBufferArrayDynamicIndexing", "shaderUniformBufferArrayNonUniformIndexing", "shaderSampledImageArrayNonUniformIndexing", "shaderStorageBufferArrayNonUniformIndexing", "shaderStorageImageArrayNonUniformIndexing", "shaderInputAttachmentArrayNonUniformIndexing", "shaderUniformTexelBufferArrayNonUniformIndexing", "shaderStorageTexelBufferArrayNonUniformIndexing", "descriptorBindingUniformBufferUpdateAfterBind", "descriptorBindingSampledImageUpdateAfterBind", "descriptorBindingStorageImageUpdateAfterBind", "descriptorBindingStorageBufferUpdateAfterBind", "descriptorBindingUniformTexelBufferUpdateAfterBind", "descriptorBindingStorageTexelBufferUpdateAfterBind", "descriptorBindingUpdateUnusedWhilePending", "descriptorBindingPartiallyBound", "descriptorBindingVariableDescriptorCount", "runtimeDescriptorArray", "samplerFilterMinmax", "scalarBlockLayout", "imagelessFramebuffer", "uniformBufferStandardLayout", "shaderSubgroupExtendedTypes", "separateDepthStencilLayouts", "hostQueryReset", "timelineSemaphore", "bufferDeviceAddress", "bufferDeviceAddressCaptureReplay", "bufferDeviceAddressMultiDevice", "vulkanMemoryModel", "vulkanMemoryModelDeviceScope", "vulkanMemoryModelAvailabilityVisibilityChains", "shaderOutputViewportIndex", "shaderOutputLayer", "subgroupBroadcastDynamicId" };
    const char* const kNames13[] = { "robustImageAccess", "inlineUniformBlock", "descriptorBindingInlineUniformBlockUpdateAfterBind", "pipelineCreationCacheControl", "privateData", "shaderDemoteToHelperInvocation", "shaderTerminateInvocation", "subgroupSizeControl", "computeFullSubgroups", "synchronization2", "textureCompressionASTC_HDR", "shaderZeroInitializeWorkgroupMemory", "dynamicRendering", "shaderIntegerDotProduct", "maintenance4" };

    struct ClampInfo { uint32_t sType; size_t size; const char* name; const char* const* fields; size_t fieldCount; };
#define CL(st, T, nm) { static_cast<uint32_t>(st), sizeof(T), nm, nullptr, 0 }
#define CLN(st, T, nm, arr) { static_cast<uint32_t>(st), sizeof(T), nm, arr, sizeof(arr) / sizeof(arr[0]) }
    const ClampInfo kClampable[] = {
        CLN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features, "Vulkan11Features", kNames11),
        CLN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features, "Vulkan12Features", kNames12),
        CLN(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features, "Vulkan13Features", kNames13),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features, "ShaderFloat16Int8"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures, "DynamicRendering"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features, "Synchronization2"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features, "Maintenance4"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT, "Robustness2(robustBufferAccess2,robustImageAccess2,nullDescriptor)"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, "ExtendedDynamicState"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT, "DepthClipEnable"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures, "SubgroupSizeControl(subgroupSizeControl,computeFullSubgroups)"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures, "8BitStorage"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures, "16BitStorage"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures, "BufferDeviceAddress"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures, "DescriptorIndexing"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures, "TimelineSemaphore"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures, "ScalarBlockLayout"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures, "HostQueryReset"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, "ShaderDemoteToHelper"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures, "PipelineCreationCacheControl"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures, "PrivateData"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures, "ImageRobustness"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures, "InlineUniformBlock"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT, VkPhysicalDeviceColorWriteEnableFeaturesEXT, "ColorWriteEnable"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, VkPhysicalDevice4444FormatsFeaturesEXT, "4444Formats"),
        CL(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT, "VertexAttributeDivisor"),
        { ST_AS_FEATURES, sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR), "AccelerationStructureFeatures", nullptr, 0 },
        { ST_RTP_FEATURES, sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR), "RayTracingPipelineFeatures", nullptr, 0 },
        { ST_RAY_QUERY_FEATURES, sizeof(VkPhysicalDeviceRayQueryFeaturesKHR), "RayQueryFeatures", nullptr, 0 },
        { ST_RT_MAINTENANCE1_FEATURES, sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR), "RayTracingMaintenance1Features", nullptr, 0 },
    };
#undef CL
#undef CLN

    struct DropInfo { uint32_t sType; const char* ext; };
    const DropInfo kDropIfExtMissing[] = {
        { static_cast<uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT), "VK_EXT_mutable_descriptor_type" },
    };

    void ClampBools(VkBool32* dst, const VkBool32* real, size_t count, const char* label,
                    const char* const* fields, size_t fieldCount) {
        for (size_t k = 0; k < count; ++k) {
            const VkBool32 nv = (dst[k] && real[k]) ? VK_TRUE : VK_FALSE;
            if (dst[k] && !real[k]) {
                if (fields && k < fieldCount) LogLine("    CLEARED %s.%s (driver lacks it)", label, fields[k]);
                else LogLine("    CLEARED %s field#%u (driver lacks it)", label, static_cast<unsigned>(k));
            }
            dst[k] = nv;
        }
    }

    void ClampToReal(VkPhysicalDevice pd, VkBaseOutStructure* node, const ClampInfo& c) {
        auto realFn = RealExport<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
        if (!realFn) return;
        std::vector<uint64_t> buf((c.size + 7) / 8, 0);
        auto* r = reinterpret_cast<VkBaseOutStructure*>(buf.data());
        r->sType = node->sType;
        r->pNext = nullptr;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = r;
        realFn(pd, &f2);
        auto* dst = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(node) + 16);
        const auto* src = reinterpret_cast<const VkBool32*>(reinterpret_cast<const uint8_t*>(r) + 16);
        ClampBools(dst, src, (c.size - 16) / 4, c.name, c.fields, c.fieldCount);
    }

    void ClampCoreFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* f, const char* label) {
        auto realFn = RealExport<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
        if (!realFn || !f) return;
        VkPhysicalDeviceFeatures real{};
        realFn(pd, &real);
        ClampBools(reinterpret_cast<VkBool32*>(f), reinterpret_cast<const VkBool32*>(&real),
                   sizeof(VkPhysicalDeviceFeatures) / 4, label, kNamesCore,
                   sizeof(kNamesCore) / sizeof(kNamesCore[0]));
    }

    // -----------------------------------------------------------------------
    // Dynamic-rendering -> VkRenderPass emulation
    // -----------------------------------------------------------------------
    namespace dynrend {
        // Vulkan: VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO = 1000044002.
        // 1000044001 is VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; using it here
        // made the proxy miss VkPipelineRenderingCreateInfo and pass a NULL renderPass
        // to the Vulkan 1.2 AMD driver, which cannot create these dynamic-rendering pipelines.
        constexpr uint32_t ST_PIPELINE_RENDERING_CREATE_INFO = 1000044002u;

        struct RPKey {
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        uint32_t viewMask = 0;
        std::vector<VkAttachmentLoadOp> colorLoadOps;
        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkAttachmentLoadOp stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        std::vector<VkImageLayout> colorLayouts;
        VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkImageLayout stencilLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bool operator<(const RPKey& o) const {
            if (colorFormats != o.colorFormats) return colorFormats < o.colorFormats;
            if (depthFormat != o.depthFormat) return depthFormat < o.depthFormat;
            if (stencilFormat != o.stencilFormat) return stencilFormat < o.stencilFormat;
            if (samples != o.samples) return samples < o.samples;
            if (viewMask != o.viewMask) return viewMask < o.viewMask;
            if (colorLoadOps != o.colorLoadOps) return colorLoadOps < o.colorLoadOps;
            if (depthLoadOp != o.depthLoadOp) return depthLoadOp < o.depthLoadOp;
            if (stencilLoadOp != o.stencilLoadOp) return stencilLoadOp < o.stencilLoadOp;
            if (colorLayouts != o.colorLayouts) return colorLayouts < o.colorLayouts;
            if (depthLayout != o.depthLayout) return depthLayout < o.depthLayout;
            return stencilLayout < o.stencilLayout;
        }
    };
    struct PipeInfo {
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkDevice device = VK_NULL_HANDLE;
            std::vector<VkFormat> colorFormats;
            VkFormat depthFormat = VK_FORMAT_UNDEFINED;
            VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            uint32_t viewMask = 0;
        };

        struct ImageViewInfo {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = 0;
        VkImage image = VK_NULL_HANDLE;
    };
    struct ActiveState {
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
        };

        struct FBKey {
            VkRenderPass rp = VK_NULL_HANDLE;
            std::vector<VkImageView> views;
            uint32_t width = 0, height = 0, layers = 0;
            bool operator<(const FBKey& o) const {
                if (rp != o.rp) return rp < o.rp;
                if (views != o.views) return views < o.views;
                if (width != o.width) return width < o.width;
                if (height != o.height) return height < o.height;
                return layers < o.layers;
            }
        };

        SRWLOCK g_lock = SRWLOCK_INIT;
        std::map<RPKey, VkRenderPass>* g_rpCache = nullptr;
        std::map<VkPipeline, PipeInfo>* g_pipes = nullptr;
        std::map<VkCommandBuffer, VkPipeline>* g_boundPipes = nullptr;
        std::map<VkCommandBuffer, VkDevice>* g_cbDevices = nullptr;
        std::map<VkCommandBuffer, ActiveState>* g_active = nullptr;
        std::map<FBKey, VkFramebuffer>* g_fbCache = nullptr;
        std::map<VkImageView, ImageViewInfo>* g_imageViews = nullptr;
    std::map<VkImage, VkImageLayout>* g_imageLayouts = nullptr;
    void TrackImages(uint32_t n, const VkImageMemoryBarrier* b) {
        if (!n || !b) return;
        AcquireSRWLockExclusive(&g_lock);
        if (!g_imageLayouts) g_imageLayouts = new std::map<VkImage, VkImageLayout>();
        for (uint32_t i = 0; i < n; ++i)
            if (b[i].newLayout != VK_IMAGE_LAYOUT_UNDEFINED) (*g_imageLayouts)[b[i].image] = b[i].newLayout;
        ReleaseSRWLockExclusive(&g_lock);
    }
    VkImageLayout LayoutOf(VkImage img, VkImageLayout def) {
        VkImageLayout l = def;
        AcquireSRWLockShared(&g_lock);
        if (g_imageLayouts) { auto it = g_imageLayouts->find(img); if (it != g_imageLayouts->end()) l = it->second; }
        ReleaseSRWLockShared(&g_lock);
        return l;
    }

        VkRenderPass CreateOrGetRenderPass(VkDevice device, const RPKey& key) {
        AcquireSRWLockExclusive(&g_lock);
        if (!g_rpCache) g_rpCache = new std::map<RPKey, VkRenderPass>();
        auto it = g_rpCache->find(key);
        if (it != g_rpCache->end()) { VkRenderPass rp = it->second; ReleaseSRWLockExclusive(&g_lock); return rp; }
        ReleaseSRWLockExclusive(&g_lock);
        std::vector<VkAttachmentDescription> atts;
        std::vector<VkAttachmentReference> colorRefs;
        VkAttachmentReference dsRef{};
        const bool hasDepth = key.depthFormat != VK_FORMAT_UNDEFINED;
        const bool hasStencil = key.stencilFormat != VK_FORMAT_UNDEFINED;
        const bool hasDS = hasDepth || hasStencil;
        for (size_t i = 0; i < key.colorFormats.size(); ++i) {
            VkAttachmentReference ref{};
            if (key.colorFormats[i] == VK_FORMAT_UNDEFINED) {
                ref.attachment = VK_ATTACHMENT_UNUSED;
                ref.layout = VK_IMAGE_LAYOUT_UNDEFINED;
                colorRefs.push_back(ref);
                continue;
            }
            const VkImageLayout lay = (i < key.colorLayouts.size()) ? key.colorLayouts[i] : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentLoadOp lo = (i < key.colorLoadOps.size()) ? key.colorLoadOps[i] : VK_ATTACHMENT_LOAD_OP_LOAD;
            VkAttachmentDescription ad{};
            ad.format = key.colorFormats[i];
            ad.samples = key.samples;
            ad.loadOp = lo;
            ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.initialLayout = (lo == VK_ATTACHMENT_LOAD_OP_LOAD) ? lay : VK_IMAGE_LAYOUT_UNDEFINED;
            ad.finalLayout = lay;
            ref.attachment = static_cast<uint32_t>(atts.size());
            ref.layout = lay;
            atts.push_back(ad);
            colorRefs.push_back(ref);
        }
        if (hasDS) {
            const VkImageLayout lay = hasDepth ? key.depthLayout : key.stencilLayout;
            VkAttachmentDescription ad{};
            ad.format = hasDepth ? key.depthFormat : key.stencilFormat;
            ad.samples = key.samples;
            ad.loadOp = hasDepth ? key.depthLoadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.storeOp = hasDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.stencilLoadOp = hasStencil ? key.stencilLoadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.stencilStoreOp = hasStencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            const bool keep = (ad.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) && (!hasStencil || ad.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
            ad.initialLayout = keep ? lay : VK_IMAGE_LAYOUT_UNDEFINED;
            ad.finalLayout = lay;
            dsRef.attachment = static_cast<uint32_t>(atts.size());
            dsRef.layout = lay;
            atts.push_back(ad);
        }
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        sub.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
        sub.pDepthStencilAttachment = hasDS ? &dsRef : nullptr;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = deps[0].srcStageMask;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = deps[0].srcStageMask;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[1].srcAccessMask = deps[0].srcAccessMask;
        deps[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = static_cast<uint32_t>(atts.size());
        rpci.pAttachments = atts.empty() ? nullptr : atts.data();
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies = deps;
        VkRenderPassMultiviewCreateInfo multiview{};
        int32_t multiviewOffsets[2] = { 0, 0 };
        if (key.viewMask != 0) {
            multiview.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
            multiview.subpassCount = 1;
            multiview.pViewMasks = &key.viewMask;
            multiview.dependencyCount = 2;
            multiview.pViewOffsets = multiviewOffsets;
            rpci.pNext = &multiview;
        }
        auto createRp = RealExport<PFN_vkCreateRenderPass>("vkCreateRenderPass");
        if (!createRp) return VK_NULL_HANDLE;
        VkRenderPass rp = VK_NULL_HANDLE;
        VkResult r = createRp(device, &rpci, nullptr, &rp);
        if (r != VK_SUCCESS) {
            LogLine("dynrend: vkCreateRenderPass FAILED result=%d (colors=%u depth=0x%X stencil=0x%X samples=%u)",
                static_cast<int>(r), static_cast<unsigned>(key.colorFormats.size()),
                key.depthFormat, key.stencilFormat, static_cast<unsigned>(key.samples));
            return VK_NULL_HANDLE;
        }
        AcquireSRWLockExclusive(&g_lock);
        (*g_rpCache)[key] = rp;
        ReleaseSRWLockExclusive(&g_lock);
        LogLine("dynrend: render pass created (colors=%u depth=0x%X stencil=0x%X samples=%u)",
            static_cast<unsigned>(key.colorFormats.size()), key.depthFormat, key.stencilFormat,
            static_cast<unsigned>(key.samples));
        return rp;
    }
    void RegisterPipeline(VkPipeline pipeline, VkDevice device,
                              const VkPipelineRenderingCreateInfo* ri,
                              VkSampleCountFlagBits samples) {
            if (!ri || !pipeline) return;
            PipeInfo pi;
            pi.device = device;
            for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i)
                pi.colorFormats.push_back(ri->pColorAttachmentFormats[i]);
            pi.depthFormat = ri->depthAttachmentFormat;
            pi.stencilFormat = ri->stencilAttachmentFormat;
            pi.samples = samples;
            pi.viewMask = ri->viewMask;

            RPKey k;
            k.colorFormats = pi.colorFormats;
            k.depthFormat = pi.depthFormat;
            k.stencilFormat = pi.stencilFormat;
            k.samples = samples;
            k.viewMask = pi.viewMask;
            for (size_t i = 0; i < pi.colorFormats.size(); ++i)
                k.colorLoadOps.push_back(VK_ATTACHMENT_LOAD_OP_LOAD);
        k.colorLayouts.push_back(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            k.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            k.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

            pi.renderPass = CreateOrGetRenderPass(device, k);
            if (pi.renderPass == VK_NULL_HANDLE) return;

            AcquireSRWLockExclusive(&g_lock);
            if (!g_pipes) g_pipes = new std::map<VkPipeline, PipeInfo>();
            (*g_pipes)[pipeline] = pi;
            ReleaseSRWLockExclusive(&g_lock);
            LogLine("dynrend: pipeline %p registered (rp=%p)", pipeline, pi.renderPass);
        }

        void UnregisterPipeline(VkPipeline p) {
            AcquireSRWLockExclusive(&g_lock);
            if (g_pipes) g_pipes->erase(p);
            ReleaseSRWLockExclusive(&g_lock);
        }

        bool LookupImageView(VkImageView v, ImageViewInfo* out) {
            AcquireSRWLockShared(&g_lock);
            bool found = false;
            if (g_imageViews) {
                auto it = g_imageViews->find(v);
                if (it != g_imageViews->end()) { *out = it->second; found = true; }
            }
            ReleaseSRWLockShared(&g_lock);
            return found;
        }

        VkDevice LookupCBDevice(VkCommandBuffer cb) {
            AcquireSRWLockShared(&g_lock);
            VkDevice d = VK_NULL_HANDLE;
            if (g_cbDevices) {
                auto it = g_cbDevices->find(cb);
                if (it != g_cbDevices->end()) d = it->second;
            }
            ReleaseSRWLockShared(&g_lock);
            return d;
        }

        bool LookupBoundPipeline(VkCommandBuffer cb, PipeInfo* out) {
            AcquireSRWLockShared(&g_lock);
            bool found = false;
            if (g_boundPipes && g_pipes) {
                auto bit = g_boundPipes->find(cb);
                if (bit != g_boundPipes->end()) {
                    auto pit = g_pipes->find(bit->second);
                    if (pit != g_pipes->end()) { *out = pit->second; found = true; }
                }
            }
            ReleaseSRWLockShared(&g_lock);
            return found;
        }
    }


    // ---------------------------------------------------------------------------
// v4: sync2 -> sync1 translation helpers
// ---------------------------------------------------------------------------
    // v8 early sync diagnostics. These are deliberately separate from the
    // SPIR-V helpers in the nested anonymous namespace below.
    thread_local char g_v8SyncJob[256] = {};
    inline void V8SyncSetJob(const char* fmt, ...) noexcept {
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(g_v8SyncJob, sizeof(g_v8SyncJob), _TRUNCATE, fmt, args);
        va_end(args);
    }
    static volatile LONG g_v8SyncCrumbCount = 0;
    inline void V8SyncCrumb(const char* fmt, ...) noexcept {
        const LONG n = InterlockedIncrement(&g_v8SyncCrumbCount);
        if (n > 48) return; // bounded diagnostics: prevent the log growing per frame
        char message[224]{};
        va_list args;
        va_start(args, fmt);
        vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
        va_end(args);
        LogLine("SYNC breadcrumb #%ld: %s", (long)n, message);
    }

// VulkanProxy v8 namespace fix (unique sync helpers).
namespace {
    VkPipelineStageFlags ConvertStage2(VkPipelineStageFlags2 s2) {
    if (s2 == 0) return 0;
    constexpr VkPipelineStageFlags kKnown =
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT |
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags out = static_cast<VkPipelineStageFlags>(s2) & kKnown;
    if (s2 & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
              VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT))
        out |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s2 & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT))
        out |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (s2 & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT)
        out |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
               VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    if (s2 & (0x00080000ull | 0x00100000ull | 0x00200000ull | 0x00400000ull | 0x02000000ull))
        out |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (out == 0) out = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return out;
}
VkAccessFlags ConvertAccess2(VkAccessFlags2 a2) {
    if (a2 == 0) return 0;
    VkAccessFlags out = static_cast<VkAccessFlags>(a2) & 0x1FFFFu;
    if (a2 & ((1ull << 32) | (1ull << 33))) out |= VK_ACCESS_SHADER_READ_BIT;
    if (a2 & (1ull << 34)) out |= VK_ACCESS_SHADER_WRITE_BIT;
    if (a2 & ~(0x1FFFFull | (7ull << 32))) out |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (out == 0) out = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    return out;
}
}
// ---------------------------------------------------------------------------// v4: sync2 implementations (translate to sync1)
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Proxy_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    auto real = RealExport<PFN_vkQueueSubmit>("vkQueueSubmit");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 256 || (id % 512) == 0);
    V8SyncSetJob("vkQueueSubmit id=%ld queue=%p submitCount=%u fence=%p",
        (long)id, (void*)queue, submitCount, (void*)fence);
    V8SyncCrumb("%s", g_v8SyncJob);
    if (trace) LogLine("SYNC vkQueueSubmit ENTER id=%ld thread=%lu queue=%p submits=%u fence=%p",
        (long)id, (unsigned long)GetCurrentThreadId(), (void*)queue, submitCount, (void*)fence);
    const ULONGLONG start = GetTickCount64();
    VkResult result = real(queue, submitCount, pSubmits, fence);
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (trace || result != VK_SUCCESS || elapsed >= 250)
        LogLine("SYNC vkQueueSubmit EXIT id=%ld result=%d elapsed=%llu ms",
            (long)id, (int)result, (unsigned long long)elapsed);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    auto real = RealExport<PFN_vkQueueSubmit>("vkQueueSubmit");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 256 || (id % 512) == 0);
    V8SyncSetJob("vkQueueSubmit2 id=%ld queue=%p submitCount=%u fence=%p",
        (long)id, (void*)queue, submitCount, (void*)fence);
    V8SyncCrumb("%s", g_v8SyncJob);
    if (trace) LogLine("SYNC vkQueueSubmit2 ENTER id=%ld thread=%lu queue=%p submits=%u fence=%p",
        (long)id, (unsigned long)GetCurrentThreadId(), (void*)queue, submitCount, (void*)fence);
    const ULONGLONG start = GetTickCount64();
    if (submitCount == 0 || !pSubmits) {
        VkResult emptyResult = real(queue, 0, nullptr, fence);
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (trace || emptyResult != VK_SUCCESS || elapsed >= 250)
            LogLine("SYNC vkQueueSubmit2 EXIT id=%ld result=%d elapsed=%llu ms",
                (long)id, (int)emptyResult, (unsigned long long)elapsed);
        return emptyResult;
    }

    std::vector<VkSubmitInfo> submits(submitCount);
    std::vector<std::vector<VkSemaphore>> waitSemaphores(submitCount);
    std::vector<std::vector<VkPipelineStageFlags>> waitStages(submitCount);
    std::vector<std::vector<VkSemaphore>> signalSemaphores(submitCount);
    std::vector<std::vector<VkCommandBuffer>> commandBuffers(submitCount);
    std::vector<std::vector<uint64_t>> waitValues(submitCount), signalValues(submitCount);
    std::vector<std::vector<uint32_t>> waitDeviceIndices(submitCount), signalDeviceIndices(submitCount);
    std::vector<std::vector<uint32_t>> commandBufferMasks(submitCount);
    std::vector<VkTimelineSemaphoreSubmitInfo> timelineInfos(submitCount);
    std::vector<VkDeviceGroupSubmitInfo> deviceGroupInfos(submitCount);
    std::vector<VkProtectedSubmitInfo> protectedInfos(submitCount);
    std::vector<uint8_t> useDeviceGroup(submitCount, 0), useProtected(submitCount, 0);

    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo2& s2 = pSubmits[i];
        if ((s2.flags & ~(VkSubmitFlags)VK_SUBMIT_PROTECTED_BIT) != 0) {
            LogLine("SYNC2: vkQueueSubmit2 has unsupported flags=0x%x", (unsigned)s2.flags);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if ((s2.waitSemaphoreInfoCount && !s2.pWaitSemaphoreInfos) ||
            (s2.signalSemaphoreInfoCount && !s2.pSignalSemaphoreInfos) ||
            (s2.commandBufferInfoCount && !s2.pCommandBufferInfos)) {
            LogLine("SYNC2: vkQueueSubmit2 received a null submit-info array");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        waitSemaphores[i].reserve(s2.waitSemaphoreInfoCount);
        waitStages[i].reserve(s2.waitSemaphoreInfoCount);
        waitValues[i].reserve(s2.waitSemaphoreInfoCount);
        waitDeviceIndices[i].reserve(s2.waitSemaphoreInfoCount);
        for (uint32_t j = 0; j < s2.waitSemaphoreInfoCount; ++j) {
            const VkSemaphoreSubmitInfo& w = s2.pWaitSemaphoreInfos[j];
            waitSemaphores[i].push_back(w.semaphore);
            waitStages[i].push_back(ConvertStage2(w.stageMask));
            waitValues[i].push_back(w.value);
            waitDeviceIndices[i].push_back(w.deviceIndex);
            if (w.deviceIndex != 0) useDeviceGroup[i] = 1;
        }

        signalSemaphores[i].reserve(s2.signalSemaphoreInfoCount);
        signalValues[i].reserve(s2.signalSemaphoreInfoCount);
        signalDeviceIndices[i].reserve(s2.signalSemaphoreInfoCount);
        for (uint32_t j = 0; j < s2.signalSemaphoreInfoCount; ++j) {
            const VkSemaphoreSubmitInfo& sig = s2.pSignalSemaphoreInfos[j];
            signalSemaphores[i].push_back(sig.semaphore);
            signalValues[i].push_back(sig.value);
            signalDeviceIndices[i].push_back(sig.deviceIndex);
            if (sig.deviceIndex != 0) useDeviceGroup[i] = 1;
        }

        commandBuffers[i].reserve(s2.commandBufferInfoCount);
        commandBufferMasks[i].reserve(s2.commandBufferInfoCount);
        for (uint32_t j = 0; j < s2.commandBufferInfoCount; ++j) {
            const VkCommandBufferSubmitInfo& cb = s2.pCommandBufferInfos[j];
            commandBuffers[i].push_back(cb.commandBuffer);
            commandBufferMasks[i].push_back(cb.deviceMask);
            if (cb.deviceMask != 1) useDeviceGroup[i] = 1;
        }

        VkSubmitInfo& s1 = submits[i];
        s1.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        s1.waitSemaphoreCount = (uint32_t)waitSemaphores[i].size();
        s1.pWaitSemaphores = waitSemaphores[i].empty() ? nullptr : waitSemaphores[i].data();
        s1.pWaitDstStageMask = waitStages[i].empty() ? nullptr : waitStages[i].data();
        s1.commandBufferCount = (uint32_t)commandBuffers[i].size();
        s1.pCommandBuffers = commandBuffers[i].empty() ? nullptr : commandBuffers[i].data();
        s1.signalSemaphoreCount = (uint32_t)signalSemaphores[i].size();
        s1.pSignalSemaphores = signalSemaphores[i].empty() ? nullptr : signalSemaphores[i].data();

        // VkSubmitInfo2 carries timeline values directly in VkSemaphoreSubmitInfo.
        // The legacy API needs VkTimelineSemaphoreSubmitInfo with matching array lengths.
        VkTimelineSemaphoreSubmitInfo& timeline = timelineInfos[i];
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount = (uint32_t)waitValues[i].size();
        timeline.pWaitSemaphoreValues = waitValues[i].empty() ? nullptr : waitValues[i].data();
        timeline.signalSemaphoreValueCount = (uint32_t)signalValues[i].size();
        timeline.pSignalSemaphoreValues = signalValues[i].empty() ? nullptr : signalValues[i].data();

        // Device-group indices/masks are not representable by the base VkSubmitInfo.
        // Preserve them through VkDeviceGroupSubmitInfo when non-default values occur.
        const void* chain = s2.pNext;
        if (useDeviceGroup[i]) {
            VkDeviceGroupSubmitInfo& dg = deviceGroupInfos[i];
            dg.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO;
            dg.waitSemaphoreCount = (uint32_t)waitDeviceIndices[i].size();
            dg.pWaitSemaphoreDeviceIndices = waitDeviceIndices[i].empty() ? nullptr : waitDeviceIndices[i].data();
            dg.commandBufferCount = (uint32_t)commandBufferMasks[i].size();
            dg.pCommandBufferDeviceMasks = commandBufferMasks[i].empty() ? nullptr : commandBufferMasks[i].data();
            dg.signalSemaphoreCount = (uint32_t)signalDeviceIndices[i].size();
            dg.pSignalSemaphoreDeviceIndices = signalDeviceIndices[i].empty() ? nullptr : signalDeviceIndices[i].data();
            dg.pNext = chain;
            chain = &dg;
        }
        if ((s2.flags & VK_SUBMIT_PROTECTED_BIT) != 0) {
            useProtected[i] = 1;
            VkProtectedSubmitInfo& ps = protectedInfos[i];
            ps.sType = VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;
            ps.protectedSubmit = VK_TRUE;
            ps.pNext = chain;
            chain = &ps;
        }
        timeline.pNext = chain;
        s1.pNext = &timeline;
    }

    if (FirstTime("sync2:queueSubmit2"))
        LogLine("SYNC2: vkQueueSubmit2 -> vkQueueSubmit (timeline/device-group data preserved)");
    VkResult submitResult = real(queue, submitCount, submits.data(), fence);
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (trace || submitResult != VK_SUCCESS || elapsed >= 250)
        LogLine("SYNC vkQueueSubmit2 EXIT id=%ld result=%d elapsed=%llu ms",
            (long)id, (int)submitResult, (unsigned long long)elapsed);
    return submitResult;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    return vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* pInfo) {
    if (!pInfo) return;
    auto real = RealExport<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    if (!real) return;

    VkPipelineStageFlags srcStage = 0, dstStage = 0;
    std::vector<VkMemoryBarrier> mem(pInfo->memoryBarrierCount);
    for (uint32_t i = 0; i < pInfo->memoryBarrierCount; ++i) {
        const auto& s = pInfo->pMemoryBarriers[i];
        srcStage |= ConvertStage2(s.srcStageMask);
        dstStage |= ConvertStage2(s.dstStageMask);
        mem[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mem[i].srcAccessMask = ConvertAccess2(s.srcAccessMask);
        mem[i].dstAccessMask = ConvertAccess2(s.dstAccessMask);
    }
    std::vector<VkBufferMemoryBarrier> buf(pInfo->bufferMemoryBarrierCount);
    for (uint32_t i = 0; i < pInfo->bufferMemoryBarrierCount; ++i) {
        const auto& s = pInfo->pBufferMemoryBarriers[i];
        srcStage |= ConvertStage2(s.srcStageMask);
        dstStage |= ConvertStage2(s.dstStageMask);
        buf[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        buf[i].srcAccessMask = ConvertAccess2(s.srcAccessMask);
        buf[i].dstAccessMask = ConvertAccess2(s.dstAccessMask);
        buf[i].srcQueueFamilyIndex = s.srcQueueFamilyIndex;
        buf[i].dstQueueFamilyIndex = s.dstQueueFamilyIndex;
        buf[i].buffer = s.buffer;
        buf[i].offset = s.offset;
        buf[i].size = s.size;
    }
    std::vector<VkImageMemoryBarrier> img(pInfo->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < pInfo->imageMemoryBarrierCount; ++i) {
        const auto& s = pInfo->pImageMemoryBarriers[i];
        srcStage |= ConvertStage2(s.srcStageMask);
        dstStage |= ConvertStage2(s.dstStageMask);
        img[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        img[i].srcAccessMask = ConvertAccess2(s.srcAccessMask);
        img[i].dstAccessMask = ConvertAccess2(s.dstAccessMask);
        img[i].oldLayout = s.oldLayout;
        img[i].newLayout = s.newLayout;
        img[i].srcQueueFamilyIndex = s.srcQueueFamilyIndex;
        img[i].dstQueueFamilyIndex = s.dstQueueFamilyIndex;
        img[i].image = s.image;
        img[i].subresourceRange = s.subresourceRange;
    }
    dynrend::TrackImages((uint32_t)img.size(), img.empty() ? nullptr : img.data());
if (FirstTime("sync2:cmdPipelineBarrier2")) LogLine("SYNC2: vkCmdPipelineBarrier2 -> vkCmdPipelineBarrier");
    real(cb, srcStage, dstStage, pInfo->dependencyFlags,
         pInfo->memoryBarrierCount, mem.empty() ? nullptr : mem.data(),
         pInfo->bufferMemoryBarrierCount, buf.empty() ? nullptr : buf.data(),
         pInfo->imageMemoryBarrierCount, img.empty() ? nullptr : img.data());
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo* pInfo) {
    vkCmdPipelineBarrier2(cb, pInfo);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    auto real = RealExport<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
    if (real) real(cb, static_cast<VkPipelineStageFlagBits>(ConvertStage2(stage)), pool, query);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp2KHR(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query) {
    vkCmdWriteTimestamp2(cb, stage, pool, query);
}

// ---------------------------------------------------------------------------
// ---- FIX: full sync2 / 1.3 command coverage ----
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBuffer2(VkCommandBuffer cb, const VkCopyBufferInfo2* p) {
    auto real = RealExport<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    if (!real || !p) return;
    std::vector<VkBufferCopy> r(p->regionCount);
    for (uint32_t i = 0; i < p->regionCount; ++i)
        r[i] = { p->pRegions[i].srcOffset, p->pRegions[i].dstOffset, p->pRegions[i].size };
    if (FirstTime("sync2:CopyBuffer2")) LogLine("SYNC2: vkCmdCopyBuffer2 -> vkCmdCopyBuffer");
    real(cb, p->srcBuffer, p->dstBuffer, p->regionCount, r.empty() ? nullptr : r.data());
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer2KHR(VkCommandBuffer cb, const VkCopyBufferInfo2* p) { vkCmdCopyBuffer2(cb, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2* p) {
    auto real = RealExport<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    if (!real || !p) return;
    std::vector<VkImageCopy> r(p->regionCount);
    for (uint32_t i = 0; i < p->regionCount; ++i) {
        const auto& s = p->pRegions[i];
        r[i] = { s.srcSubresource, s.srcOffset, s.dstSubresource, s.dstOffset, s.extent };
    }
    if (FirstTime("sync2:CopyImage2")) LogLine("SYNC2: vkCmdCopyImage2 -> vkCmdCopyImage");
    real(cb, p->srcImage, p->srcImageLayout, p->dstImage, p->dstImageLayout, p->regionCount, r.empty() ? nullptr : r.data());
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2KHR(VkCommandBuffer cb, const VkCopyImageInfo2* p) { vkCmdCopyImage2(cb, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* p) {
    auto real = RealExport<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    if (!real || !p) return;
    std::vector<VkBufferImageCopy> r(p->regionCount);
    for (uint32_t i = 0; i < p->regionCount; ++i) {
        const auto& s = p->pRegions[i];
        r[i] = { s.bufferOffset, s.bufferRowLength, s.bufferImageHeight, s.imageSubresource, s.imageOffset, s.imageExtent };
    }
    if (FirstTime("sync2:CopyBufferToImage2")) LogLine("SYNC2: vkCmdCopyBufferToImage2 -> vkCmdCopyBufferToImage");
    real(cb, p->srcBuffer, p->dstImage, p->dstImageLayout, p->regionCount, r.empty() ? nullptr : r.data());
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage2KHR(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* p) { vkCmdCopyBufferToImage2(cb, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* p) {
    auto real = RealExport<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    if (!real || !p) return;
    std::vector<VkBufferImageCopy> r(p->regionCount);
    for (uint32_t i = 0; i < p->regionCount; ++i) {
        const auto& s = p->pRegions[i];
        r[i] = { s.bufferOffset, s.bufferRowLength, s.bufferImageHeight, s.imageSubresource, s.imageOffset, s.imageExtent };
    }
    real(cb, p->srcImage, p->srcImageLayout, p->dstBuffer, p->regionCount, r.empty() ? nullptr : r.data());
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer2KHR(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* p) { vkCmdCopyImageToBuffer2(cb, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2* p) {
    auto real = RealExport<PFN_vkCmdBlitImage>("vkCmdBlitImage");
    if (!real || !p) return;
    std::vector<VkImageBlit> r(p->regionCount);
    for (uint32_t i = 0; i < p->regionCount; ++i) {
        const auto& s = p->pRegions[i];
        r[i] = { s.srcSubresource, { s.srcOffsets[0], s.srcOffsets[1] }, s.dstSubresource, { s.dstOffsets[0], s.dstOffsets[1] } };
    }
    if (FirstTime("sync2:BlitImage2")) LogLine("SYNC2: vkCmdBlitImage2 -> vkCmdBlitImage");
    real(cb, p->srcImage, p->srcImageLayout, p->dstImage, p->dstImageLayout, p->regionCount, r.empty() ? nullptr : r.data(), p->filter);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage2KHR(VkCommandBuffer cb, const VkBlitImageInfo2* p) { vkCmdBlitImage2(cb, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdResolveImage2(VkCommandBuffer cb, const VkResolveImageInfo2* p) {
    auto real = RealExport<PFN_vkCmdResolveImage>("vkCmdResolveImage");
    if (!real || !p) return;
    std::vector<VkImageResolve> r(p->regionCount);
    for (uint32_t i = 0; i < p->regionCount; ++i) {
        const auto& s = p->pRegions[i];
        r[i] = { s.srcSubresource, s.srcOffset, s.dstSubresource, s.dstOffset, s.extent };
    }
    real(cb, p->srcImage, p->srcImageLayout, p->dstImage, p->dstImageLayout, p->regionCount, r.empty() ? nullptr : r.data());
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage2KHR(VkCommandBuffer cb, const VkResolveImageInfo2* p) { vkCmdResolveImage2(cb, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdSetEvent2(VkCommandBuffer cb, VkEvent ev, const VkDependencyInfo* p) {
    auto real = RealExport<PFN_vkCmdSetEvent>("vkCmdSetEvent");
    if (!real) return;
    VkPipelineStageFlags s = 0;
    if (p) {
        for (uint32_t i = 0; i < p->memoryBarrierCount; ++i) s |= ConvertStage2(p->pMemoryBarriers[i].srcStageMask);
        for (uint32_t i = 0; i < p->bufferMemoryBarrierCount; ++i) s |= ConvertStage2(p->pBufferMemoryBarriers[i].srcStageMask);
        for (uint32_t i = 0; i < p->imageMemoryBarrierCount; ++i) s |= ConvertStage2(p->pImageMemoryBarriers[i].srcStageMask);
    }
    if (!s) s = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (FirstTime("sync2:SetEvent2")) LogLine("SYNC2: vkCmdSetEvent2 -> vkCmdSetEvent");
    real(cb, ev, s);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2KHR(VkCommandBuffer cb, VkEvent ev, const VkDependencyInfo* p) { vkCmdSetEvent2(cb, ev, p); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdResetEvent2(VkCommandBuffer cb, VkEvent ev, VkPipelineStageFlags2 stage) {
    auto real = RealExport<PFN_vkCmdResetEvent>("vkCmdResetEvent");
    if (real) real(cb, ev, ConvertStage2(stage));
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdResetEvent2KHR(VkCommandBuffer cb, VkEvent ev, VkPipelineStageFlags2 s) { vkCmdResetEvent2(cb, ev, s); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdWaitEvents2(VkCommandBuffer cb, uint32_t n, const VkEvent* evs, const VkDependencyInfo* deps) {
    auto real = RealExport<PFN_vkCmdWaitEvents>("vkCmdWaitEvents");
    if (!real || !n || !evs) return;
    VkPipelineStageFlags src = 0, dst = 0;
    std::vector<VkMemoryBarrier> mem; std::vector<VkBufferMemoryBarrier> buf; std::vector<VkImageMemoryBarrier> img;
    for (uint32_t d = 0; d < n && deps; ++d) {
        const auto& p = deps[d];
        for (uint32_t i = 0; i < p.memoryBarrierCount; ++i) {
            const auto& s = p.pMemoryBarriers[i];
            src |= ConvertStage2(s.srcStageMask); dst |= ConvertStage2(s.dstStageMask);
            VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, ConvertAccess2(s.srcAccessMask), ConvertAccess2(s.dstAccessMask) };
            mem.push_back(b);
        }
        for (uint32_t i = 0; i < p.bufferMemoryBarrierCount; ++i) {
            const auto& s = p.pBufferMemoryBarriers[i];
            src |= ConvertStage2(s.srcStageMask); dst |= ConvertStage2(s.dstStageMask);
            VkBufferMemoryBarrier b{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, ConvertAccess2(s.srcAccessMask), ConvertAccess2(s.dstAccessMask),
                s.srcQueueFamilyIndex, s.dstQueueFamilyIndex, s.buffer, s.offset, s.size };
            buf.push_back(b);
        }
        for (uint32_t i = 0; i < p.imageMemoryBarrierCount; ++i) {
            const auto& s = p.pImageMemoryBarriers[i];
            src |= ConvertStage2(s.srcStageMask); dst |= ConvertStage2(s.dstStageMask);
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, ConvertAccess2(s.srcAccessMask), ConvertAccess2(s.dstAccessMask),
                s.oldLayout, s.newLayout, s.srcQueueFamilyIndex, s.dstQueueFamilyIndex, s.image, s.subresourceRange };
            img.push_back(b);
        }
    }
    if (!src) src = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (!dst) dst = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (FirstTime("sync2:WaitEvents2")) LogLine("SYNC2: vkCmdWaitEvents2 -> vkCmdWaitEvents");
    dynrend::TrackImages((uint32_t)img.size(), img.empty() ? nullptr : img.data());
    real(cb, n, evs, src, dst,
        (uint32_t)mem.size(), mem.empty() ? nullptr : mem.data(),
        (uint32_t)buf.size(), buf.empty() ? nullptr : buf.data(),
        (uint32_t)img.size(), img.empty() ? nullptr : img.data());
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdWaitEvents2KHR(VkCommandBuffer cb, uint32_t n, const VkEvent* e, const VkDependencyInfo* d) { vkCmdWaitEvents2(cb, n, e, d); }
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count,
    const VkBuffer* bufs, const VkDeviceSize* offs, const VkDeviceSize* sizes, const VkDeviceSize* strides) {
    auto real2 = RealExport<PFN_vkCmdBindVertexBuffers2>("vkCmdBindVertexBuffers2");
    if (real2) { real2(cb, first, count, bufs, offs, sizes, strides); return; }
    auto real = RealExport<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
    if (real) real(cb, first, count, bufs, offs);
}
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(VkCommandBuffer cb,
    VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags flags,
    uint32_t mc, const VkMemoryBarrier* mb, uint32_t bc, const VkBufferMemoryBarrier* bb,
    uint32_t ic, const VkImageMemoryBarrier* ib) {
    dynrend::TrackImages(ic, ib);
    auto real = RealExport<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    if (real) real(cb, src, dst, flags, mc, mb, bc, bb, ic, ib);
}
namespace pdata {
    SRWLOCK lock = SRWLOCK_INIT;
    std::map<uint64_t, std::map<uint64_t, uint64_t>>* slots = nullptr;
    inline uint64_t K(VkPrivateDataSlot s) { return (uint64_t)(uintptr_t)s; }
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePrivateDataSlot(VkDevice, const VkPrivateDataSlotCreateInfo*, const VkAllocationCallbacks*, VkPrivateDataSlot* p) {
    if (!p) return VK_ERROR_INITIALIZATION_FAILED;
    *p = MakeFake<VkPrivateDataSlot>();
    AcquireSRWLockExclusive(&pdata::lock);
    if (!pdata::slots) pdata::slots = new std::map<uint64_t, std::map<uint64_t, uint64_t>>();
    (*pdata::slots)[pdata::K(*p)];
    ReleaseSRWLockExclusive(&pdata::lock);
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyPrivateDataSlot(VkDevice, VkPrivateDataSlot s, const VkAllocationCallbacks*) {
    AcquireSRWLockExclusive(&pdata::lock);
    if (pdata::slots) pdata::slots->erase(pdata::K(s));
    ReleaseSRWLockExclusive(&pdata::lock);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkSetPrivateData(VkDevice, VkObjectType, uint64_t obj, VkPrivateDataSlot s, uint64_t data) {
    AcquireSRWLockExclusive(&pdata::lock);
    if (pdata::slots) (*pdata::slots)[pdata::K(s)][obj] = data;
    ReleaseSRWLockExclusive(&pdata::lock);
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPrivateData(VkDevice, VkObjectType, uint64_t obj, VkPrivateDataSlot s, uint64_t* pData) {
    uint64_t v = 0;
    AcquireSRWLockShared(&pdata::lock);
    if (pdata::slots) {
        auto it = pdata::slots->find(pdata::K(s));
        if (it != pdata::slots->end()) { auto j = it->second.find(obj); if (j != it->second.end()) v = j->second; }
    }
    ReleaseSRWLockShared(&pdata::lock);
    if (pData) *pData = v;
}
static volatile LONG g_noopCmdCalls = 0;
static VKAPI_ATTR void VKAPI_CALL Stub_NoopCmd() {
    const LONG n = InterlockedIncrement(&g_noopCmdCalls);
    if (n <= 32 || (n % 1000) == 0)
        LogLine("MISSING dynamic-state NO-OP was actually CALLED (count=%ld)", (long)n);
}
// v7: SPIR-V analysis, crash tracer, driver protection
// ---------------------------------------------------------------------------
namespace {
    // ---- config (VulkanProxy.ini next to the dll, [spirv] section) ----------
    struct SpvConfig {
        // Defaults for GCN2 (R7 260): more conservative so the game reaches the menu.
        int denyFloat16 = 1, denyInt16 = 1, denyDotProduct = 1, denyRayTracing = 1, denyMesh = 1;
        int patchVersion = 1;   // rewrite SPIR-V 1.6 -> 1.5 (driver supports up to 1.5)
        int logAll = 0;         // log every shader, not just unusual ones
        int rewriteRayQuery = 1;   // strip ray queries from shaders (no-hit semantics) instead of denying them
        int dumpRewrite = 0;       // dump the first N rewritten shaders (input + output)
        int dumpRewriteFail = 3;   // dump first N shaders rejected by ray-query rewrite or FP16/INT16 lowering
        int useReplacements = 1;   // load replacement shaders from spv_replace\\spv_<hash>.spv
        float brightness = 1.0f;   // post-process gain on the final image (1.0 = off)
        int tryVariants = 1;       // if the driver crashes/rejects a module, retry with workaround variants
        int keepCode = 1;
        int traceReplacementShaders = 0; // bounded tracing for modules loaded from spv_replace
        // Which capabilities this machine really has. Replacing hardware denial with
        // "DenyFloat16=1" forced every GPU to behave like GCN2: capable cards got the
        // SAME denied/fake modules, so spv_replace hashes (computed from the game's
        // real modules on GCN2) never matched on NVIDIA/Intel and lighting asset
        // shaders stayed fake -> dark interiors. With these switches on, every GPU
        // classifies modules the same way (denial only when the driver really lacks
        // the feature), which makes the replacement map portable.
        int followHardwareCaps = 1; // honour the real driver feature bits instead of the fixed deny list
        int structureMatchFallback = 1; // second-chance matching when the byte hash differs
        int fuzzyMatch = 1;             // similarity matching when the exact keys miss
        int forceVirtualRtByDefault = 0; // 1 = treat every GPU as "no RT at all" (old, uniform behaviour)
        int dumpSamples = 0;       // dump N working compute shaders of two kinds (for comparison with crashing ones)
        int serializeCompile = 1; // one thread at a time inside driver shader/pipeline compilation
        int promoteFloat16 = 0;   // старый промоутер - ВЫКЛ (ломает layout буферов)
        int lowerFp16 = 0;        // ВЫКЛ: заменяем на комбинированный проход
        int lowerFp16AndInt16 = 1; // новый: fp16 + int16 ALU -> 32 бит
        int catchDriverCrash = 1; // catch access violations inside the driver and fall back to FAKE objects
		int dropF16Cap = 0;       // 1 = ещё и убрать OpCapability Float16 после понижения
        uint32_t denyComputeCaps[16] = {}; int nDenyComputeCaps = 0;  // deny COMPUTE shaders that use any of these capabilities
        uint32_t denyAnyCaps[16] = {};     int nDenyAnyCaps = 0;      // deny ANY shader that uses any of these capabilities
        uint32_t denyComputeAll[16] = {};  int nDenyComputeAll = 0;   // deny COMPUTE shaders that use ALL of these capabilities together
        uint64_t dumpActiveComputeHashes[16] = {}; int nDumpActiveComputeHashes = 0; // exact original hashes to dump on first real dispatch
    };
    SpvConfig g_spvCfg;
    volatile LONG g_spvCfgLoaded = 0;

    bool IsActiveComputeDumpHash(uint64_t hash) {
        for (int i = 0; i < g_spvCfg.nDumpActiveComputeHashes; ++i)
            if (g_spvCfg.dumpActiveComputeHashes[i] == hash) return true;
        return false;
    }

    // ---- replacement shaders: spv_replace\\spv_<fnv1a64 of the ORIGINAL module>.spv ------------------
    inline uint64_t Fnv1a64(const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
        return h;
    }
    std::unordered_map<uint64_t, std::wstring>* g_replMap = nullptr;
    // The replacement index is declared later (see FindReplacement); the config
    // loader invalidates it there.
    void ForgetReplacementIndex();
    void ScanReplacements(const wchar_t* iniPath) {
        wchar_t dir[2048]{};
        wcsncpy_s(dir, iniPath, _TRUNCATE);
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (!slash) return;
        *(slash + 1) = 0;
        wchar_t pattern[2200]{};
        _snwprintf_s(pattern, 2200, _TRUNCATE, L"%lsspv_replace\\spv_*.spv", dir);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        g_replMap = new std::unordered_map<uint64_t, std::wstring>();
        do {
            // name: spv_<16 hex digits>.spv
            const wchar_t* n = fd.cFileName;
            if (wcslen(n) < 4 + 16) continue;
            wchar_t hex[17]{};
            wcsncpy_s(hex, n + 4, 16);
            wchar_t* e = nullptr;
            const unsigned long long v = wcstoull(hex, &e, 16);
            if (e != hex + 16) continue;
            wchar_t full[2400]{};
            _snwprintf_s(full, 2400, _TRUNCATE, L"%lsspv_replace\\%ls", dir, n);
            (*g_replMap)[v] = full;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        LogLine("SPIRV: %zu replacement shader file(s) found in spv_replace", g_replMap->size());
    }
    bool ReadReplacement(const std::wstring& path, std::vector<uint32_t>& out) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        bool ok = false;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart > 20 && sz.QuadPart < (64ll << 20) && (sz.QuadPart % 4) == 0) {
            out.resize(static_cast<size_t>(sz.QuadPart / 4));
            DWORD got = 0;
            ok = ReadFile(f, out.data(), static_cast<DWORD>(sz.QuadPart), &got, nullptr) && got == sz.QuadPart && out[0] == 0x07230203u;
        }
        CloseHandle(f);
        if (!ok) out.clear();
        return ok;
    }

    // Validate the SPIR-V container before trusting a replacement file. This is a
    // defensive envelope check (magic/version/schema/id bound/instruction lengths),
    // not a replacement for Khronos spirv-val. The driver remains the final validator.
    bool ValidateReplacementEnvelope(const std::vector<uint32_t>& words, std::string& err) {
        if (words.size() < 5 || words[0] != 0x07230203u) {
            err = "bad SPIR-V header or magic";
            return false;
        }
        const uint32_t version = words[1];
        const uint32_t major = (version >> 16) & 0xFFu;
        const uint32_t minor = (version >> 8) & 0xFFu;
        if ((version & 0xFF0000FFu) != 0 || major != 1u || minor > 6u) {
            err = "unsupported or malformed SPIR-V version word";
            return false;
        }
        if (words[3] == 0 || words[3] > 0x003FFFFFu) {
            err = "invalid SPIR-V id bound";
            return false;
        }
        if (words[4] != 0) {
            err = "reserved SPIR-V schema word is not zero";
            return false;
        }
        for (size_t i = 5; i < words.size();) {
            const uint32_t wc = words[i] >> 16;
            if (wc == 0 || i + wc > words.size()) {
                err = "invalid SPIR-V instruction word count";
                return false;
            }
            i += wc;
        }
        return true;
    }

    void LoadSpvConfigImpl() {
        wchar_t path[2048]{};
        GetLogPath(path, sizeof(path) / sizeof(path[0]));
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) wcscpy_s(slash + 1, (sizeof(path) / sizeof(path[0])) - static_cast<size_t>(slash + 1 - path), L"VulkanProxy.ini");
        else wcscpy_s(path, sizeof(path) / sizeof(path[0]), L"VulkanProxy.ini");
        auto rd = [&](const wchar_t* k, int d) { return static_cast<int>(GetPrivateProfileIntW(L"spirv", k, d, path)); };
        g_spvCfg.denyFloat16    = rd(L"DenyFloat16", 1);
        g_spvCfg.denyInt16      = rd(L"DenyInt16", 1);
        g_spvCfg.denyDotProduct = rd(L"DenyDotProduct", 1);
        g_spvCfg.denyRayTracing = rd(L"DenyRayTracing", 1);
        g_spvCfg.denyMesh       = rd(L"DenyMesh", 1);
        g_spvCfg.patchVersion   = rd(L"PatchVersion", 1);
        g_spvCfg.logAll         = rd(L"LogAll", 0);
        g_spvCfg.serializeCompile = rd(L"SerializeCompile", 1);
        g_spvCfg.keepCode = rd(L"KeepShaderCode", 1);
        g_spvCfg.traceReplacementShaders = rd(L"TraceReplacementShaders", 0);
        g_spvCfg.followHardwareCaps = rd(L"FollowHardwareCaps", 1);
        g_spvCfg.structureMatchFallback = rd(L"StructureMatchFallback", 1);
        g_spvCfg.fuzzyMatch = rd(L"FuzzyMatch", 1);
        InterlockedExchange(&g_uniformShaderProfile, rd(L"UniformShaderProfile", 0));
        g_spvCfg.forceVirtualRtByDefault = rd(L"ForceVirtualRt", 0);
        g_spvCfg.tryVariants = rd(L"TryVariants", 1);
        {
            wchar_t bb[64]{};
            GetPrivateProfileStringW(L"spirv", L"BrightnessGain", L"1.0", bb, 64, path);
            const double g = wcstod(bb, nullptr);
            g_spvCfg.brightness = (g > 1.0 && g <= 8.0) ? static_cast<float>(g) : 1.0f;
        }
        g_spvCfg.useReplacements = rd(L"UseReplacements", 1);
        g_spvCfg.dumpSamples = rd(L"DumpSamples", 0);
        g_spvCfg.rewriteRayQuery = rd(L"RewriteRayQuery", 1);
        g_spvCfg.dumpRewrite = rd(L"DumpRewrite", 0);
        g_spvCfg.dumpRewriteFail = rd(L"DumpRewriteFail", 3);
        InterlockedExchange(&g_logVerbose, rd(L"Verbose", 0));
        g_spvCfg.catchDriverCrash = rd(L"CatchDriverCrash", 1);
        g_spvCfg.promoteFloat16 = rd(L"PromoteFloat16", 0);
        g_spvCfg.lowerFp16 = rd(L"LowerFp16", 0);
        g_spvCfg.lowerFp16AndInt16 = rd(L"LowerFp16AndInt16", 1);
		g_spvCfg.dropF16Cap = rd(L"DropF16Cap", 0);
        auto rdList = [&](const wchar_t* key, uint32_t* out, int& n) {
            wchar_t buf[256]{};
            GetPrivateProfileStringW(L"spirv", key, L"", buf, 256, path);
            n = 0;
            for (wchar_t* p = buf; *p && n < 16;) {
                while (*p == L' ' || *p == L',') ++p;
                if (!*p) break;
                wchar_t* e = nullptr;
                const unsigned long v = wcstoul(p, &e, 10);
                if (e == p) break;
                out[n++] = static_cast<uint32_t>(v);
                p = e;
            }
        };
        {
            wchar_t hashBuf[512]{};
            GetPrivateProfileStringW(L"spirv", L"DumpActiveComputeHashes", L"", hashBuf, _countof(hashBuf), path);
            g_spvCfg.nDumpActiveComputeHashes = 0;
            wchar_t* p = hashBuf;
            while (*p && g_spvCfg.nDumpActiveComputeHashes < 16) {
                while (*p == L' ' || *p == L'\t' || *p == L',' || *p == L';') ++p;
                if (!*p) break;
                wchar_t* e = nullptr;
                const unsigned long long value = wcstoull(p, &e, 16);
                if (e == p) {
                    while (*p && *p != L',' && *p != L';') ++p;
                    continue;
                }
                g_spvCfg.dumpActiveComputeHashes[g_spvCfg.nDumpActiveComputeHashes++] = static_cast<uint64_t>(value);
                p = e;
            }
        }
        rdList(L"DenyComputeCaps", g_spvCfg.denyComputeCaps, g_spvCfg.nDenyComputeCaps);
        rdList(L"DenyAnyCaps", g_spvCfg.denyAnyCaps, g_spvCfg.nDenyAnyCaps);
        rdList(L"DenyComputeAllCaps", g_spvCfg.denyComputeAll, g_spvCfg.nDenyComputeAll);
        if (g_spvCfg.useReplacements) {
            ScanReplacements(path);
            // The ini (and therefore spv_replace) may be re-read; drop the derived
            // index so it is rebuilt from the folder that is on disk now. The lazy
            // builder publishes "ready" while holding the lock, so a concurrent
            // lookup can never observe a half-built index.
            ForgetReplacementIndex();
        }
        LogLine("SPIRV: FollowHardwareCaps=%d StructureMatchFallback=%d FuzzyMatch=%d UniformShaderProfile=%d ForceVirtualRt=%d",
            g_spvCfg.followHardwareCaps, g_spvCfg.structureMatchFallback, g_spvCfg.fuzzyMatch,
            (int)g_uniformShaderProfile, g_spvCfg.forceVirtualRtByDefault);
        LogLine("SPIRV config (%ls): DenyFloat16=%d DenyInt16=%d DenyDotProduct=%d DenyRayTracing=%d RewriteRayQuery=%d DenyMesh=%d PatchVersion=%d LogAll=%d Serialize=%d Catch=%d DenyComputeAll=%d caps Brightness=%.2f",
            path, g_spvCfg.denyFloat16, g_spvCfg.denyInt16, g_spvCfg.denyDotProduct,
            g_spvCfg.denyRayTracing, g_spvCfg.rewriteRayQuery, g_spvCfg.denyMesh, g_spvCfg.patchVersion, g_spvCfg.logAll,
            g_spvCfg.serializeCompile, g_spvCfg.catchDriverCrash, g_spvCfg.nDenyComputeAll, g_spvCfg.brightness);
    }

    // Blocking, thread-safe: other threads must WAIT for the ini to be read, otherwise shaders created at the
    // same moment would be analysed with default values (this made the first few shaders ignore the ini).
    SRWLOCK g_spvCfgLock = SRWLOCK_INIT;
    void LoadSpvConfig() {
        if (g_spvCfgLoaded == 2) return;
        AcquireSRWLockExclusive(&g_spvCfgLock);
        if (g_spvCfgLoaded != 2) { LoadSpvConfigImpl(); g_spvCfgLoaded = 2; }
        ReleaseSRWLockExclusive(&g_spvCfgLock);
    }

    // ---- breadcrumb ring buffer, dumped by the crash handler -----------------
    constexpr int kCrumbCount = 64;
    constexpr int kCrumbLen = 240;
    char g_crumbs[kCrumbCount][kCrumbLen];
    volatile LONG g_crumbIdx = 0;

    void Crumb(const char* fmt, ...) noexcept {
        const LONG i = (InterlockedIncrement(&g_crumbIdx) - 1) & (kCrumbCount - 1);
        char tmp[kCrumbLen];
        va_list a; va_start(a, fmt);
        vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, fmt, a);
        va_end(a);
        _snprintf_s(g_crumbs[i], kCrumbLen, _TRUNCATE, "[t%lu %llu] %s", GetCurrentThreadId(),
            static_cast<unsigned long long>(GetTickCount64()), tmp);
    }

// ---------------------------------------------------------------------------
// SPIR-V rewriter: removes ray-query usage so a GPU/driver without ray tracing can still run the shader.
//   * OpTypeAccelerationStructureKHR / OpTypeRayQueryKHR and everything built from them are deleted
//   * ray-query instructions that return a value are replaced by "null" (no hit) constants
//   * the RayQueryKHR capability and SPV_KHR_ray_query extension are removed
// Pure C++17, no Windows dependencies (unit-tested offline with spirv-val).
// ---------------------------------------------------------------------------
namespace spvrw {
// generated from spirv.core.grammar.json
struct SpvOpDesc { uint16_t op; const char* kinds; };
static const SpvOpDesc kSpvOps[] = {
    {0,""},
    {1,"TR"},
    {2,"S"},
    {3,"LLIS"},
    {4,"S"},
    {5,"IS"},
    {6,"ILS"},
    {7,"RS"},
    {8,"ILL"},
    {10,"S"},
    {11,"RS"},
    {12,"TRIL(I)"},
    {14,"LL"},
    {15,"LIS(I)"},
    {16,"IL"},
    {17,"L"},
    {19,"R"},
    {20,"R"},
    {21,"RLL"},
    {22,"RLL"},
    {23,"RIL"},
    {24,"RIL"},
    {25,"RILLLLLLL"},
    {26,"R"},
    {27,"RI"},
    {28,"RII"},
    {29,"RI"},
    {30,"R(I)"},
    {31,"RS"},
    {32,"RLI"},
    {33,"RI(I)"},
    {34,"R"},
    {35,"R"},
    {36,"R"},
    {37,"R"},
    {38,"RL"},
    {39,"IL"},
    {41,"TR"},
    {42,"TR"},
    {43,"TRN"},
    {44,"TR(I)"},
    {45,"TRLLL"},
    {46,"TR"},
    {48,"TR"},
    {49,"TR"},
    {50,"TRN"},
    {51,"TR(I)"},
    {52,"TRL"},
    {54,"TRLI"},
    {55,"TR"},
    {56,""},
    {57,"TRI(I)"},
    {59,"TRLI"},
    {60,"TRIII"},
    {61,"TRIA"},
    {62,"IIA"},
    {63,"IIAA"},
    {64,"IIIAA"},
    {65,"TRI(I)"},
    {66,"TRI(I)"},
    {67,"TRII(I)"},
    {68,"TRIL"},
    {69,"TRI"},
    {70,"TRII(I)"},
    {71,"IL"},
    {72,"ILL"},
    {73,"R"},
    {74,"I(I)"},
    {75,"I(IL)"},
    {77,"TRII"},
    {78,"TRIII"},
    {79,"TRII(L)"},
    {80,"TR(I)"},
    {81,"TRI(L)"},
    {82,"TRII(L)"},
    {83,"TRI"},
    {84,"TRI"},
    {86,"TRII"},
    {87,"TRIIM"},
    {88,"TRIIM"},
    {89,"TRIIIM"},
    {90,"TRIIIM"},
    {91,"TRIIM"},
    {92,"TRIIM"},
    {93,"TRIIIM"},
    {94,"TRIIIM"},
    {95,"TRIIM"},
    {96,"TRIIIM"},
    {97,"TRIIIM"},
    {98,"TRIIM"},
    {99,"IIIM"},
    {100,"TRI"},
    {101,"TRI"},
    {102,"TRI"},
    {103,"TRII"},
    {104,"TRI"},
    {105,"TRII"},
    {106,"TRI"},
    {107,"TRI"},
    {109,"TRI"},
    {110,"TRI"},
    {111,"TRI"},
    {112,"TRI"},
    {113,"TRI"},
    {114,"TRI"},
    {115,"TRI"},
    {116,"TRI"},
    {117,"TRI"},
    {118,"TRI"},
    {119,"TRI"},
    {120,"TRI"},
    {121,"TRI"},
    {122,"TRI"},
    {123,"TRIL"},
    {124,"TRI"},
    {126,"TRI"},
    {127,"TRI"},
    {128,"TRII"},
    {129,"TRII"},
    {130,"TRII"},
    {131,"TRII"},
    {132,"TRII"},
    {133,"TRII"},
    {134,"TRII"},
    {135,"TRII"},
    {136,"TRII"},
    {137,"TRII"},
    {138,"TRII"},
    {139,"TRII"},
    {140,"TRII"},
    {141,"TRII"},
    {142,"TRII"},
    {143,"TRII"},
    {144,"TRII"},
    {145,"TRII"},
    {146,"TRII"},
    {147,"TRII"},
    {148,"TRII"},
    {149,"TRII"},
    {150,"TRII"},
    {151,"TRII"},
    {152,"TRII"},
    {154,"TRI"},
    {155,"TRI"},
    {156,"TRI"},
    {157,"TRI"},
    {158,"TRI"},
    {159,"TRI"},
    {160,"TRI"},
    {161,"TRII"},
    {162,"TRII"},
    {163,"TRII"},
    {164,"TRII"},
    {165,"TRII"},
    {166,"TRII"},
    {167,"TRII"},
    {168,"TRI"},
    {169,"TRIII"},
    {170,"TRII"},
    {171,"TRII"},
    {172,"TRII"},
    {173,"TRII"},
    {174,"TRII"},
    {175,"TRII"},
    {176,"TRII"},
    {177,"TRII"},
    {178,"TRII"},
    {179,"TRII"},
    {180,"TRII"},
    {181,"TRII"},
    {182,"TRII"},
    {183,"TRII"},
    {184,"TRII"},
    {185,"TRII"},
    {186,"TRII"},
    {187,"TRII"},
    {188,"TRII"},
    {189,"TRII"},
    {190,"TRII"},
    {191,"TRII"},
    {194,"TRII"},
    {195,"TRII"},
    {196,"TRII"},
    {197,"TRII"},
    {198,"TRII"},
    {199,"TRII"},
    {200,"TRI"},
    {201,"TRIIII"},
    {202,"TRIII"},
    {203,"TRIII"},
    {204,"TRI"},
    {205,"TRI"},
    {207,"TRI"},
    {208,"TRI"},
    {209,"TRI"},
    {210,"TRI"},
    {211,"TRI"},
    {212,"TRI"},
    {213,"TRI"},
    {214,"TRI"},
    {215,"TRI"},
    {218,""},
    {219,""},
    {220,"I"},
    {221,"I"},
    {224,"III"},
    {225,"II"},
    {227,"TRIII"},
    {228,"IIII"},
    {229,"TRIIII"},
    {230,"TRIIIIII"},
    {231,"TRIIIIII"},
    {232,"TRIII"},
    {233,"TRIII"},
    {234,"TRIIII"},
    {235,"TRIIII"},
    {236,"TRIIII"},
    {237,"TRIIII"},
    {238,"TRIIII"},
    {239,"TRIIII"},
    {240,"TRIIII"},
    {241,"TRIIII"},
    {242,"TRIIII"},
    {245,"TR(II)"},
    {246,"IIL"},
    {247,"IL"},
    {248,"R"},
    {249,"I"},
    {250,"III(L)"},
    {251,"II(LI)"},
    {252,""},
    {253,""},
    {254,"I"},
    {255,""},
    {256,"IL"},
    {257,"IL"},
    {259,"TRIIIIII"},
    {260,"III"},
    {261,"TRII"},
    {262,"TRII"},
    {263,"TRIII"},
    {264,"TRILI"},
    {265,"TRILI"},
    {266,"TRILI"},
    {267,"TRILI"},
    {268,"TRILI"},
    {269,"TRILI"},
    {270,"TRILI"},
    {271,"TRILI"},
    {274,"TRIIII"},
    {275,"TRIIII"},
    {276,"TRIIIIII"},
    {277,"TRIIIIII"},
    {278,"TRIIII"},
    {279,"TRIIII"},
    {280,"IIII"},
    {281,"IIII"},
    {282,"TRI"},
    {283,"TRIII"},
    {284,"TRIII"},
    {285,"TRIIIII"},
    {286,"TRIIIII"},
    {287,"IIIII"},
    {288,"IIIII"},
    {291,"TRIIII"},
    {292,"TRIIIIIIIIII(I)"},
    {293,"TRIIIII"},
    {294,"TRIIIII"},
    {295,"TRIIII"},
    {296,"TRIIII"},
    {297,"I"},
    {298,"I"},
    {299,"TR"},
    {300,"TRI"},
    {301,"II"},
    {302,"III"},
    {303,"TR"},
    {304,"TRIII"},
    {305,"TRIIM"},
    {306,"TRIIM"},
    {307,"TRIIIM"},
    {308,"TRIIIM"},
    {309,"TRIIM"},
    {310,"TRIIM"},
    {311,"TRIIIM"},
    {312,"TRIIIM"},
    {313,"TRIIM"},
    {314,"TRIIIM"},
    {315,"TRIIIM"},
    {316,"TRI"},
    {317,""},
    {318,"TRIII"},
    {319,"III"},
    {320,"TRIIM"},
    {321,"TRI"},
    {322,"R"},
    {323,"TRLLL"},
    {324,"TRI"},
    {325,"TRIIIII"},
    {326,"TRIIII"},
    {327,"R"},
    {328,"TRI"},
    {329,"III"},
    {330,"S"},
    {331,"IL"},
    {332,"IL"},
    {333,"TRI"},
    {334,"TRII"},
    {335,"TRII"},
    {336,"TRII"},
    {337,"TRIII"},
    {338,"TRII"},
    {339,"TRII"},
    {340,"TRII"},
    {341,"TRIII"},
    {342,"TRILI"},
    {343,"TRII"},
    {344,"TRII"},
    {345,"TRIII"},
    {346,"TRIII"},
    {347,"TRIII"},
    {348,"TRIII"},
    {349,"TRILII"},
    {350,"TRILII"},
    {351,"TRILII"},
    {352,"TRILII"},
    {353,"TRILII"},
    {354,"TRILII"},
    {355,"TRILII"},
    {356,"TRILII"},
    {357,"TRILII"},
    {358,"TRILII"},
    {359,"TRILII"},
    {360,"TRILII"},
    {361,"TRILII"},
    {362,"TRILII"},
    {363,"TRILII"},
    {364,"TRILII"},
    {365,"TRIII"},
    {366,"TRIII"},
    {400,"TRI"},
    {401,"TRII"},
    {402,"TRII"},
    {403,"TRII"},
    {4160,"TRII"},
    {4161,"TRI"},
    {4162,"TRI"},
    {4416,""},
    {4417,"RL"},
    {4418,"TRLII"},
    {4419,"TRII(I)"},
    {4420,"TRII(I)"},
    {4421,"TRI"},
    {4422,"TRI"},
    {4423,"TRIII(I)"},
    {4424,"TRIII(I)"},
    {4425,"TRIIL"},
    {4426,"IIIII"},
    {4428,"TRI"},
    {4429,"TRI"},
    {4430,"TRI"},
    {4431,"TRIIII"},
    {4432,"TRII"},
    {4433,"TRIL(I)"},
    {4445,"IIIIIIIIIII"},
    {4446,"II"},
    {4447,"TRI"},
    {4448,""},
    {4449,""},
    {4450,"TRIIL"},
    {4451,"TRIIL"},
    {4452,"TRIIL"},
    {4453,"TRIIIL"},
    {4454,"TRIIIL"},
    {4455,"TRIIIL"},
    {4456,"RIIIII"},
    {4457,"TRIIIA"},
    {4458,"IIIIA"},
    {4459,"TRIIIL"},
    {4460,"TRI"},
    {4461,"TRI"},
    {4462,"TRI"},
    {4463,"TRI"},
    {4472,"R"},
    {4473,"IIIIIIII"},
    {4474,"I"},
    {4475,"II"},
    {4476,"I"},
    {4477,"TRI"},
    {4479,"TRII"},
    {4480,"TRIII"},
    {4481,"TRIII"},
    {4482,"TRIIIII"},
    {4483,"TRIIIII"},
    {4500,"TRIIIII"},
    {4501,"TRIIIII"},
    {4502,"TRIIIII"},
    {4503,"TRIIIII"},
    {5000,"TRILI"},
    {5001,"TRILI"},
    {5002,"TRILI"},
    {5003,"TRILI"},
    {5004,"TRILI"},
    {5005,"TRILI"},
    {5006,"TRILI"},
    {5007,"TRILI"},
    {5011,"TRII"},
    {5012,"TRIII"},
    {5056,"TRI"},
    {5074,"TRIII"},
    {5075,"I"},
    {5076,"RI"},
    {5078,"TRI"},
    {5090,"TRI"},
    {5101,"TRII"},
    {5103,"RS"},
    {5104,"RS"},
    {5110,"TRI"},
    {5111,"TRI"},
    {5249,"IIIIIIIIIIIIII"},
    {5250,"IIIIIIIIIIIII"},
    {5251,"IIIIIII"},
    {5252,"TRI"},
    {5253,"TRI"},
    {5254,"TRI"},
    {5255,"TRI"},
    {5256,"IIIIIIIIIIIII"},
    {5257,"TRI"},
    {5258,"TRI"},
    {5259,"I"},
    {5260,"IIIIIIIIIIII"},
    {5261,"IIIIIIIIIIIII"},
    {5262,"IIIIIIIIIIII"},
    {5263,"IIIIII"},
    {5264,"II"},
    {5265,"TRI"},
    {5266,"II"},
    {5267,"TRI"},
    {5268,"TRI"},
    {5269,"TRI"},
    {5270,"TRI"},
    {5271,"TRI"},
    {5272,"TRI"},
    {5273,"TRI"},
    {5274,"TRI"},
    {5275,"TRI"},
    {5276,"TRI"},
    {5277,"TRI"},
    {5278,"TRI"},
    {5279,"III"},
    {5280,"II"},
    {5281,"R"},
    {5283,"TRIIIIM"},
    {5288,"RII"},
    {5289,"TRIIIIIIIIIIL"},
    {5290,"IIIIIII"},
    {5291,"III"},
    {5292,"TRIIIIIIIIIIIIIL"},
    {5293,"TRI"},
    {5294,"IIII"},
    {5295,"II"},
    {5296,"TRI"},
    {5299,"II"},
    {5300,"TRIIIII"},
    {5301,"TRIIIII"},
    {5302,"TRIIA"},
    {5303,"IIIA"},
    {5334,"TRII"},
    {5335,""},
    {5336,""},
    {5337,"IIIIIIIIIII"},
    {5338,"IIIIIIIIIIII"},
    {5339,"IIIIIIIIIIII"},
    {5340,"TRII"},
    {5341,"R"},
    {5344,"II"},
    {5345,"TRII"},
    {5346,"TRI"},
    {5358,"RIIII"},
    {5359,"TRIIIA"},
    {5360,"IIIIA"},
    {5361,"TRIII"},
    {5362,"TRI"},
    {5364,""},
    {5365,""},
    {5366,"TRILI"},
    {5367,"TRIIIAL"},
    {5368,"IIIAL"},
    {5369,"TRII(I)"},
    {5370,"RII"},
    {5371,"RII(I)"},
    {5372,"TR"},
    {5373,"TRI(I)"},
    {5374,"TRI(I)"},
    {5375,"TRI(I)"},
    {5376,"TRII"},
    {5377,"TR"},
    {5378,"TRI(I)"},
    {5379,"TRI(I)"},
    {5380,""},
    {5381,"TR"},
    {5382,"TRIIIII"},
    {5384,"TRI(I)"},
    {5390,"TRI"},
    {5391,"TRI"},
    {5392,"TRI"},
    {5393,"TRI"},
    {5394,"TRI"},
    {5395,"TRI"},
    {5396,"TRI"},
    {5397,"L"},
    {5398,"TRIIIIL"},
    {5427,"TRII"},
    {5428,"TRII"},
    {5429,"TRII"},
    {5430,"TRII"},
    {5431,"TRII"},
    {5432,"TRI"},
    {5433,"TRI"},
    {5434,"TRI"},
    {5435,"TRI"},
    {5436,"TRI"},
    {5437,"TRI"},
    {5438,"TRII"},
    {5439,"TRII"},
    {5571,"TRII"},
    {5572,"TRIII"},
    {5573,"TRIII"},
    {5574,"TRII"},
    {5575,"TRI"},
    {5576,"II"},
    {5577,"TRII"},
    {5578,"III"},
    {5580,"TRIIII"},
    {5581,"IIIII"},
    {5585,"TRI"},
    {5586,"TRI"},
    {5587,"TRII"},
    {5588,"TRII"},
    {5589,"TRII"},
    {5590,"TRII"},
    {5591,"TRII"},
    {5592,"TRII"},
    {5593,"TRII"},
    {5594,"TRII"},
    {5595,"TRII"},
    {5596,"TRII"},
    {5597,"TRII"},
    {5598,"TRII"},
    {5600,"TRI"},
    {5601,"TR(I)"},
    {5609,"RS"},
    {5610,"TRIISS"},
    {5611,"TRI(I)"},
    {5614,"TRIIII"},
    {5615,"TRIIII"},
    {5630,"I"},
    {5631,"TRII"},
    {5632,"IL"},
    {5633,"ILL"},
    {5699,"TRII"},
    {5700,"RI"},
    {5701,"R"},
    {5702,"R"},
    {5703,"R"},
    {5704,"R"},
    {5705,"R"},
    {5706,"R"},
    {5707,"R"},
    {5708,"R"},
    {5709,"R"},
    {5710,"R"},
    {5711,"R"},
    {5712,"R"},
    {5713,"TRII"},
    {5714,"TRII"},
    {5715,"TRII"},
    {5716,"TRII"},
    {5717,"TRII"},
    {5718,"TRII"},
    {5719,"TRII"},
    {5720,"TRII"},
    {5721,"TR"},
    {5722,"TR"},
    {5723,"TR"},
    {5724,"TRIIII"},
    {5725,"TRII"},
    {5726,"TR"},
    {5727,"TR"},
    {5728,"TRI"},
    {5729,"TRII"},
    {5730,"TRII"},
    {5731,"TRIII"},
    {5732,"TRI"},
    {5733,"TRI"},
    {5734,"TRI"},
    {5735,"TRI"},
    {5736,"TRI"},
    {5737,"TRI"},
    {5738,"TRI"},
    {5739,"TRI"},
    {5740,"TRI"},
    {5741,"TRI"},
    {5742,"TRI"},
    {5743,"TRI"},
    {5744,"TRI"},
    {5745,"TRI"},
    {5746,"TRIII"},
    {5747,"TRIII"},
    {5748,"TRIII"},
    {5749,"TRIIII"},
    {5750,"TRII"},
    {5751,"TRIIII"},
    {5752,"TRI"},
    {5753,"TRII"},
    {5754,"TRI"},
    {5755,"TRII"},
    {5756,"TRII"},
    {5757,"TRIII"},
    {5758,"TRIIII"},
    {5759,"TRIIII"},
    {5760,"TRIIIII"},
    {5761,"TRIII"},
    {5762,"TRIIII"},
    {5763,"TRIIII"},
    {5764,"TRIIIII"},
    {5765,"TRI"},
    {5766,"TRI"},
    {5767,"TRI"},
    {5768,"TRI"},
    {5769,"TRI"},
    {5770,"TRII"},
    {5771,"TRII"},
    {5772,"TRII"},
    {5773,"TRIII"},
    {5774,"TRIII"},
    {5775,"TRIII"},
    {5776,"TRII"},
    {5777,"TRI"},
    {5778,"TRI"},
    {5779,"TRI"},
    {5780,"TRI"},
    {5781,"TRIIIIIII"},
    {5782,"TRIIIIIIII"},
    {5783,"TRI"},
    {5784,"TRI"},
    {5785,"TRI"},
    {5786,"TRIII"},
    {5787,"TRIIII"},
    {5788,"TRIII"},
    {5789,"TRIIII"},
    {5790,"TRI"},
    {5791,"TRI"},
    {5792,"TRIIIIII"},
    {5793,"TRIIIIIIII"},
    {5794,"TRIIIIIIIIIII"},
    {5795,"TRII"},
    {5796,"TRI"},
    {5797,"TRII"},
    {5798,"TRIIII"},
    {5799,"TRII"},
    {5800,"TRI"},
    {5801,"TRII"},
    {5802,"TRII"},
    {5803,"TRII"},
    {5804,"TRIII"},
    {5805,"TRIIII"},
    {5806,"TRIII"},
    {5807,"TRIIII"},
    {5808,"TRI"},
    {5809,"TRI"},
    {5810,"TRI"},
    {5811,"TRI"},
    {5812,"TRI"},
    {5813,"TRI"},
    {5814,"TRI"},
    {5815,"TRI"},
    {5816,"TRI"},
    {5818,"TRI"},
    {5819,"TR"},
    {5820,"I"},
    {5840,"TRILLLLLL"},
    {5841,"TRILLLLL"},
    {5842,"TRILLLLL"},
    {5843,"TRILLLL"},
    {5846,"TRILILLLLL"},
    {5847,"TRILILLLLL"},
    {5848,"TRILILLLLL"},
    {5849,"TRILILLLLL"},
    {5850,"TRILIL"},
    {5851,"TRILIL"},
    {5852,"TRILIL"},
    {5853,"TRILIL"},
    {5854,"TRILIL"},
    {5855,"TRILLLLL"},
    {5856,"TRILLLLL"},
    {5857,"TRILLLLL"},
    {5858,"TRILILLLLL"},
    {5859,"TRILLLLL"},
    {5860,"TRILLLLL"},
    {5861,"TRILLLLL"},
    {5862,"TRILLLLL"},
    {5863,"TRILLLLL"},
    {5864,"TRILLLLL"},
    {5865,"TRILLLLL"},
    {5866,"TRILLLLL"},
    {5867,"TRILLLLL"},
    {5868,"TRILLLLL"},
    {5869,"TRILLLLL"},
    {5870,"TRILLLLL"},
    {5871,"TRILLLLL"},
    {5872,"TRILLLLL"},
    {5873,"TRILLLLL"},
    {5874,"TRILLLLL"},
    {5875,"TRILLLLL"},
    {5876,"TRILLLLL"},
    {5877,"TRILLLLL"},
    {5878,"TRILLLLL"},
    {5879,"TRILILLLLL"},
    {5880,"TRILILLLLL"},
    {5881,"TRILILLLLL"},
    {5882,"TRILILLLL"},
    {5887,"(L)"},
    {5911,"RI"},
    {5912,"RII"},
    {5913,"R(I)"},
    {5923,"TRIILLLLL"},
    {5924,"TRIILLLLL"},
    {5925,"TRIILLLLL"},
    {5926,"TRIILLLLL"},
    {5927,"TRIILLLLL"},
    {5928,"TRIILLLLL"},
    {5929,"TRIILLLLL"},
    {5930,"TRIILLLLL"},
    {5931,"TRIILLLLL"},
    {5932,"TRIILLLLL"},
    {5933,"TRIILLLLL"},
    {5934,"TRI"},
    {5938,"TRI"},
    {5946,"TRII"},
    {5947,"TRII"},
    {5949,"TRII"},
    {6016,"TRI"},
    {6017,"TRI"},
    {6018,"TRII"},
    {6019,"TRII"},
    {6020,"TRII"},
    {6021,"TRII"},
    {6022,"TRII"},
    {6023,"TRII"},
    {6024,"TRII"},
    {6025,"TRII"},
    {6026,"TRI"},
    {6027,"TRII"},
    {6028,"TRII"},
    {6029,"TRI"},
    {6030,"TRI"},
    {6031,"TRII"},
    {6032,"TRII"},
    {6035,"TRIIII"},
    {6086,"RL"},
    {6090,"(I)"},
    {6091,"(I)"},
    {6092,"(I)"},
    {6096,"TR(I)"},
    {6116,"TRI"},
    {6117,"TRI"},
    {6142,"III"},
    {6143,"III"},
    {6145,"TRI"},
    {6221,"IIA"},
    {6231,"IIIIIIIIII"},
    {6232,"IIIIIIIIII"},
    {6233,"IIIIIIIIII"},
    {6234,"IIIIIIIII"},
    {6235,"IIIIIIIIII"},
    {6237,"TRIIIIL"},
    {6401,"TRILI"},
    {6402,"TRILI"},
    {6403,"TRILI"},
    {6404,"TRILI"},
    {6405,"TRILI"},
    {6406,"TRILI"},
    {6407,"TRILI"},
    {6408,"TRILI"},
    {6428,"TRILII"},
    {6429,"IILI"},
};
    inline const char* KindsFor(uint32_t op) {
        size_t lo = 0, hi = sizeof(kSpvOps) / sizeof(kSpvOps[0]);
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (kSpvOps[mid].op < op) lo = mid + 1; else hi = mid;
        }
        if (lo < sizeof(kSpvOps) / sizeof(kSpvOps[0]) && kSpvOps[lo].op == op) return kSpvOps[lo].kinds;
        return nullptr;
    }

    // Calls f(wordIndex, role) for every id operand of an instruction. role: 'T' result type, 'R' result id, 'I' id use.
    template<class F> void ForEachId(const uint32_t* w, uint32_t op, uint32_t wc, F&& f) {
        const char* k = KindsFor(op);
        if (!k) return;
        size_t pos = 1;
        auto token = [&](char t) {
            if (pos >= wc) return;
            switch (t) {
            case 'T': f(pos, 'T'); ++pos; break;
            case 'R': f(pos, 'R'); ++pos; break;
            case 'I': f(pos, 'I'); ++pos; break;
            case 'L': ++pos; break;
            case 'N': pos = wc; break;
            case 'S': {
                while (pos < wc) {
                    const uint32_t x = w[pos++];
                    if ((x & 0xFF) == 0 || (x & 0xFF00) == 0 || (x & 0xFF0000) == 0 || (x & 0xFF000000) == 0) break;
                }
                break;
            }
            case 'M': {
                ++pos;                                 // image operand mask
                while (pos < wc) { f(pos, 'I'); ++pos; }
                break;
            }
            case 'A': {
                const uint32_t mask = w[pos++];
                if (mask & 0x2) ++pos;                 // Aligned literal
                if ((mask & 0x8) && pos < wc) { f(pos, 'I'); ++pos; }
                if ((mask & 0x10) && pos < wc) { f(pos, 'I'); ++pos; }
                break;
            }
            default: break;
            }
        };
        for (size_t i = 0; k[i];) {
            if (k[i] == '(') {
                size_t j = i + 1;
                while (k[j] && k[j] != ')') ++j;
                while (pos < wc) {
                    const size_t before = pos;
                    for (size_t c = i + 1; c < j; ++c) token(k[c]);
                    if (pos == before) break;
                }
                i = j + 1;
            } else { token(k[i]); ++i; }
        }
    }

    struct Inst { uint32_t op, wc; size_t off; };

    inline bool IsRqValueOp(uint32_t op) {
        return op == 4477 || op == 4479 || op == 5340 || (op >= 6016 && op <= 6032);
    }
    inline bool IsRqVoidOp(uint32_t op) { return op >= 4473 && op <= 4476; }

    // Experimental variants for modules that crash the old driver's SPIR-V front end.
    //   1: OpConvertUToPtr -> OpBitcast (equivalent for PhysicalStorageBuffer pointers)
    //   2: variant 1 + drop AliasedPointer / RestrictPointer decorations
    inline bool ApplyVariant(int variant, const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err) {
        if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }
        out.clear();
        out.reserve(n);
        out.insert(out.end(), in, in + 5);
        bool changed = false;
        for (size_t i = 5; i < n;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
            if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
            if (variant >= 1 && op == 120 && wc == 4) {                      // OpConvertUToPtr
                out.push_back((4u << 16) | 124u);                            // OpBitcast
                out.insert(out.end(), in + i + 1, in + i + wc);
                changed = true;
            } else if (variant >= 2 && op == 71 && wc >= 3 && (in[i + 2] == 5355 || in[i + 2] == 5356)) {
                changed = true;                                              // drop decoration
            } else {
                out.insert(out.end(), in + i, in + i + wc);
            }
            i += wc;
        }
        if (!changed) { err = "variant not applicable"; return false; }
        return true;
    }

    // ConvertUToPtr is a known crash trigger in older AMDVLK front ends.
    // For PhysicalStorageBuffer64, u64 -> physical pointer has exactly the
    // same bit-pattern semantics as OpBitcast. Rewrite only this provably
    // equivalent case; leave all width-changing / other pointer conversions.
    inline bool RewriteSameWidthPhysicalUToPtr(
        const uint32_t* in, size_t n, std::vector<uint32_t>& out,
        uint32_t& rewrittenCount, std::string& err) {
        rewrittenCount = 0;
        if (!in || n < 5 || in[0] != 0x07230203u) {
            err = "not spir-v";
            return false;
        }

        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> intTypes;
        std::unordered_map<uint32_t, uint32_t> pointerStorage;
        std::unordered_map<uint32_t, uint32_t> valueTypes;
        bool physicalStorageBuffer64 = false;

        // First pass: collect the memory model, scalar/pointer types and
        // the result type of all value-producing instructions.
        for (size_t i = 5; i < n;) {
            const uint32_t wc = in[i] >> 16;
            const uint32_t op = in[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) {
                err = "bad instruction length";
                return false;
            }

            if (op == 14 && wc >= 3 && in[i + 1] == 5348u)
                physicalStorageBuffer64 = true; // PhysicalStorageBuffer64
            if (op == 21 && wc >= 4)
                intTypes[in[i + 1]] = { in[i + 2], in[i + 3] }; // width, signedness
            if (op == 32 && wc >= 4)
                pointerStorage[in[i + 1]] = in[i + 2];

            uint32_t resultType = 0;
            uint32_t resultId = 0;
            ForEachId(in + i, op, wc, [&](size_t idx, char role) {
                if (role == 'T') resultType = in[i + idx];
                else if (role == 'R') resultId = in[i + idx];
            });
            if (resultType && resultId)
                valueTypes[resultId] = resultType;

            i += wc;
        }

        if (!physicalStorageBuffer64) {
            out.clear();
            return true;
        }

        out.clear();
        out.reserve(n);
        out.insert(out.end(), in, in + 5);

        for (size_t i = 5; i < n;) {
            const uint32_t wc = in[i] >> 16;
            const uint32_t op = in[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) {
                err = "bad instruction length";
                out.clear();
                return false;
            }

            bool safe = false;
            if (op == 120 && wc == 4) { // OpConvertUToPtr
                const uint32_t pointerType = in[i + 1];
                const uint32_t integerId = in[i + 3];
                auto ps = pointerStorage.find(pointerType);
                auto vt = valueTypes.find(integerId);
                if (ps != pointerStorage.end() && ps->second == 5349u &&
                    vt != valueTypes.end()) { // PhysicalStorageBuffer
                    auto it = intTypes.find(vt->second);
                    safe = it != intTypes.end() && it->second.first == 64u &&
                           it->second.second == 0u; // unsigned 64-bit integer
                }
            }

            if (safe) {
                out.push_back((wc << 16) | 124u); // OpBitcast; same 64-bit pattern
                out.insert(out.end(), in + i + 1, in + i + wc);
                ++rewrittenCount;
            } else {
                out.insert(out.end(), in + i, in + i + wc);
            }
            i += wc;
        }
        return true;
    }
	
	
	
	
	// v8: убираем инструкции, которых старый драйвер не знает.
//  * OpReadClockKHR (5056, cap ShaderClockKHR=5055) -> нулевая константа
//  * старые subgroup-опы SPV_KHR_shader_ballot -> core GroupNonUniform
inline bool LowerUnsupportedOps(const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err) {
    if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }
    std::vector<Inst> insts;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
        if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
        insts.push_back({ op, wc, i });
        i += wc;
    }
    auto oldBallot = [](uint32_t op) {
        return op == 4421 || op == 4422 || op == 4428 || op == 4429 || op == 4430 || op == 4432;
    };
    bool any = false, needScope = false, needShuffle = false, hasShuffle = false;
    uint32_t uintTy = 0; bool uintTyExists = false;
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 21 && I.wc >= 4 && w[2] == 32 && w[3] == 0) { uintTy = w[1]; uintTyExists = true; }
        if (I.op == 17 && I.wc >= 2 && w[1] == 65) hasShuffle = true;
        if (I.op == 5056) any = true;
        if (oldBallot(I.op)) { any = true; needScope = true; }
        if (I.op == 4432) needShuffle = true;
    }
    if (!any) { err = "nothing to lower"; return false; }

    uint32_t bound = in[3];
    std::map<uint32_t, uint32_t> nullFor;
    std::vector<uint32_t> nullOrder;
    for (const Inst& I : insts) {
        if (I.op != 5056) continue;
        if (I.wc < 4) { err = "short OpReadClockKHR"; return false; }
        const uint32_t t = in[I.off + 1];
        if (!nullFor.count(t)) { nullFor[t] = bound++; nullOrder.push_back(t); }
    }
    uint32_t scopeId = 0;
    if (needScope) { if (!uintTyExists) uintTy = bound++; scopeId = bound++; }

    out.clear();
    out.reserve(n + 32);
    out.insert(out.end(), in, in + 5);
    bool capDone = false, extras = false;
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        const uint32_t op = I.op;
        if (op == 17 && I.wc >= 2) {
            const uint32_t c = w[1];
            if (c == 5055 || c == 4423) continue;
            out.insert(out.end(), w, w + I.wc);
            if (!capDone) {
                capDone = true;
                if (needShuffle && !hasShuffle) { out.push_back((2u << 16) | 17u); out.push_back(65); }
            }
            continue;
        }
        if (op == 10 && I.wc >= 2) {
            const char* s = reinterpret_cast<const char*>(w + 1);
            const size_t lim = (I.wc - 1) * 4;
            if (strncmp(s, "SPV_KHR_shader_clock", lim) == 0 || strncmp(s, "SPV_KHR_shader_ballot", lim) == 0) continue;
        }
        if (op == 54 && !extras) {
            extras = true;
            if (needScope && !uintTyExists) {
                out.push_back((4u << 16) | 21u); out.push_back(uintTy); out.push_back(32); out.push_back(0);
            }
            if (needScope) { out.push_back((4u << 16) | 43u); out.push_back(uintTy); out.push_back(scopeId); out.push_back(3); }
            for (uint32_t t : nullOrder) { out.push_back((3u << 16) | 46u); out.push_back(t); out.push_back(nullFor[t]); }
        }
        if (op == 5056) {
            out.push_back((4u << 16) | 83u);
            out.push_back(w[1]); out.push_back(w[2]); out.push_back(nullFor[w[1]]);
            continue;
        }
        if (oldBallot(op)) {
            uint32_t nop;
            switch (op) {
            case 4421: nop = 339; break;   // Ballot
            case 4422: nop = 338; break;   // BroadcastFirst
            case 4428: nop = 334; break;   // All
            case 4429: nop = 335; break;   // Any
            case 4430: nop = 336; break;   // AllEqual
            default:   nop = 345; break;   // Shuffle
            }
            out.push_back(((I.wc + 1) << 16) | nop);
            out.push_back(w[1]); out.push_back(w[2]); out.push_back(scopeId);
            for (uint32_t k = 3; k < I.wc; ++k) out.push_back(w[k]);
            continue;
        }
        out.insert(out.end(), w, w + I.wc);
    }
    out[3] = bound;
    return true;
}
	
	

    // Replace every OpImageFetch (texel fetch) by a null constant of the same result type. Used as a last resort
    // for pipelines the old driver refuses; geometry still draws, only the fetched data (e.g. vertex offsets) is lost.
    inline bool StripImageFetch(const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err) {
        if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }
        std::vector<Inst> insts;
        for (size_t i = 5; i < n;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
            if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
            insts.push_back({ op, wc, i });
            i += wc;
        }
        uint32_t bound = in[3];
        std::unordered_map<uint32_t, uint32_t> nullFor;
        std::vector<uint32_t> order;
        for (const Inst& I : insts)
            if (I.op == 95 && I.wc >= 5) {                      // OpImageFetch
                const uint32_t t = in[I.off + 1];
                if (!nullFor.count(t)) { nullFor[t] = bound++; order.push_back(t); }
            }
        if (order.empty()) { err = "no OpImageFetch"; return false; }
        out.clear();
        out.reserve(n + 16);
        out.insert(out.end(), in, in + 5);
        bool consts = false;
        for (const Inst& I : insts) {
            const uint32_t* w = in + I.off;
            if (I.op == 54 && !consts) {
                consts = true;
                for (uint32_t t : order) { out.push_back((3u << 16) | 46u); out.push_back(t); out.push_back(nullFor[t]); }
            }
            if (I.op == 95 && I.wc >= 5) {
                out.push_back((4u << 16) | 83u);                // OpCopyObject %type %result %null
                out.push_back(w[1]); out.push_back(w[2]); out.push_back(nullFor[w[1]]);
            } else out.insert(out.end(), w, w + I.wc);
        }
        out[3] = bound;
        return true;
    }

    // Returns true on success (out filled). On failure, err explains and the caller keeps the original module.
    
    // Convert IEEE-754 binary16 bits to binary32 bits.
    inline uint32_t Float16BitsToFloat32Bits(uint16_t h) {
        const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
        const uint32_t exp  = (h >> 10) & 0x1Fu;
        const uint32_t mant = h & 0x3FFu;
        if (exp == 0) {
            if (mant == 0) return sign; // +/- 0
            // subnormal -> normalize
            uint32_t m = mant;
            uint32_t e = 127 - 15 + 1;
            while ((m & 0x400u) == 0) { m <<= 1; --e; }
            m &= 0x3FFu;
            return sign | (e << 23) | (m << 13);
        }
        if (exp == 31) {
            // Inf / NaN
            return sign | 0x7F800000u | (mant << 13);
        }
        return sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }

    
	
	
	// Структурная самопроверка. Никогда не отдаём драйверу модуль, который мы сами сломали.
// Проверяем: (1) каждое используемое id объявлено раньше, (2) инструкции с результатом
// стоят внутри базового блока. При провале вызывающий оставляет ОРИГИНАЛ.
inline bool CheckStructure(const uint32_t* w, size_t n, std::string& err) {
    if (!w || n < 5 || w[0] != 0x07230203u) { err = "not spir-v"; return false; }
    std::unordered_set<uint32_t> defined;
    bool inBlock = false;
    uint32_t currentFunction = 0;
    uint32_t currentBlock = 0;
    size_t i = 5;
    while (i < n) {
        const uint32_t first = w[i];
        const uint32_t wc = first >> 16, op = first & 0xFFFFu;
        if (wc == 0 || i + wc > n) { err = "bad word count"; return false; }
        const char* k = KindsFor(op);
        const bool hasRes = (k && k[0] == 'T' && k[1] == 'R' && wc >= 3);
        if (op == 54) {
            inBlock = false;
            currentFunction = (wc >= 3) ? w[i + 2] : 0;
            currentBlock = 0;
        }
        else if (op == 248) {
            inBlock = true;
            currentBlock = (wc >= 2) ? w[i + 1] : 0;
        }
        else if (op == 56) {
            inBlock = false;
            currentFunction = 0;
            currentBlock = 0;
        }
        // Ссылки вперёд, разрешённые спекой SPIR-V:
        //  15 EntryPoint / 16 ExecutionMode / 331 ExecutionModeId -> id функции (объявлена ниже)
        //  5/6 имена, 71..75/332/5632/5633 декорации, 245 Phi, 246/247 merge,
        //  249..251 переходы, 39 TypeForwardPointer
        const bool fwdOk = op == 15 || op == 16 || op == 331 || op == 5 || op == 6 ||
                           op == 71 || op == 72 || op == 73 || op == 74 || op == 75 ||
                           op == 332 || op == 5632 || op == 5633 || op == 245 ||
                           op == 246 || op == 247 || op == 249 || op == 250 ||
                           op == 251 || op == 39;
        if (k && !fwdOk) {
            bool bad = false; uint32_t badId = 0;
            // OpFunctionCall: слово 3 - id вызываемой функции, она может быть объявлена ниже.
            // Аргументы (слова 4+) проверяем как обычно.
            ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                if (role == 'R') return;
                if (op == 57 && idx == 3) return;
                if (!defined.count(w[i + idx])) { bad = true; badId = w[i + idx]; }
            });
            if (bad) { err = "op " + std::to_string(op) + " uses undefined id " + std::to_string(badId); return false; }
        }
        if (hasRes) {
            const bool globalOk = op == 1 || op == 54 || op == 55 || op == 59 ||
                                  (op >= 41 && op <= 46) || (op >= 48 && op <= 52);
            if (!globalOk && !inBlock) {
                err = "op " + std::to_string(op) + " result outside a block (function=" +
                    std::to_string(currentFunction) + ", block=" + std::to_string(currentBlock) +
                    ", word=" + std::to_string(i) + ")";
                return false;
            }
        }
        if (k && k[0] == 'R' && wc >= 2) defined.insert(w[i + 1]);
        else if (hasRes) defined.insert(w[i + 2]);
        if (op == 39 && wc >= 2) defined.insert(w[i + 1]);
        if (op == 249 || op == 250 || op == 251 || op == 252 || op == 253 ||
            op == 254 || op == 255 || op == 4416) inBlock = false;
        i += wc;
    }
    return true;
}
	
	
// v13: register synthetic aggregate types for recursive widening; keep OpCopyLogical safe.
// - 16-bit values in externally visible StorageBuffer/Uniform memory retain their layout.
// - Function/Private/Workgroup pointers and their access chains are widened.
// - Function by-value signatures are widened.
// - Aggregate local values containing 16-bit members are cloned recursively with 32-bit members.
// - StorageBuffer/Uniform/PhysicalStorageBuffer layouts remain 16-bit; unsupported interface
//   storage classes fail closed so the caller does not send invalid SPIR-V to the driver.
inline bool LowerFp16AndInt16Alu(
    const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err, bool dropCaps,
    size_t* outLocalPointerTypes = nullptr, size_t* outLocalAggregateTypes = nullptr) {
    if (outLocalPointerTypes) *outLocalPointerTypes = 0;
    if (outLocalAggregateTypes) *outLocalAggregateTypes = 0;
    if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }

    std::vector<Inst> insts;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
        if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
        insts.push_back({ op, wc, i });
        i += wc;
    }

    auto constLike = [](uint32_t op) {
        return op == 1 || op == 41 || op == 42 || op == 43 || op == 44 || op == 45 ||
               op == 46 || op == 48 || op == 49 || op == 50 || op == 51;
    };
    auto keepNarrow = [](uint32_t op) {
        // Pointer bitcasts and explicit float/int conversions retain their bit-level semantics.
        // OpCopyLogical (400) must retain its original result type: its result and operand
        // types must logically match. The generated bridge below widens aggregate members.
        return op == 54 || op == 61 || op == 55 || op == 57 || op == 59 || op == 60 || op == 124 ||
               op == 400 || (op >= 110 && op <= 115) || (op >= 86 && op <= 98) ||
               (op >= 100 && op <= 107);
    };

    std::unordered_map<uint32_t, uint32_t> tKind, tWidth, tSign, vComp, vLen, mCol, mCnt;
    std::unordered_map<uint32_t, uint32_t> arrayElem, arrayLengthId, runtimeArrayElem;
    std::unordered_map<uint32_t, std::vector<uint32_t>> structMembers;
    std::unordered_map<uint32_t, uint32_t> pointerStorage, pointerPointee;
    std::unordered_map<uint32_t, uint64_t> constantValues;
    std::unordered_set<uint32_t> decoratedTypes, memberDecoratedTypes;
    std::unordered_map<uint32_t, std::vector<uint32_t>> functionTypeParams;
    std::unordered_map<uint32_t, uint32_t> functionTypeReturn, functionIdToType;

    uint32_t halfTy = 0, floatTy = 0;
    std::vector<uint32_t> int16Types;
    std::unordered_set<uint32_t> int16TypeSet;

    // Collect all type information before the first function. Function signatures are also
    // collected here so calls can be rewritten according to their actual parameter types.
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 54) break;
        if (I.op == 22 && I.wc >= 3) {
            tKind[w[1]] = 22; tWidth[w[1]] = w[2];
            if (w[2] == 16) halfTy = w[1];
            if (w[2] == 32 && !floatTy) floatTy = w[1];
        }
        if (I.op == 21 && I.wc >= 4) {
            tKind[w[1]] = 21; tWidth[w[1]] = w[2]; tSign[w[1]] = w[3];
            if (w[2] == 16 && int16TypeSet.insert(w[1]).second) int16Types.push_back(w[1]);
        }
        if (I.op == 23 && I.wc >= 4) { vComp[w[1]] = w[2]; vLen[w[1]] = w[3]; }
        if (I.op == 24 && I.wc >= 4) { mCol[w[1]] = w[2]; mCnt[w[1]] = w[3]; }
        if (I.op == 28 && I.wc >= 4) { arrayElem[w[1]] = w[2]; arrayLengthId[w[1]] = w[3]; }
        if (I.op == 29 && I.wc >= 3) runtimeArrayElem[w[1]] = w[2];
        if (I.op == 30 && I.wc >= 2) {
            structMembers[w[1]] = std::vector<uint32_t>(w + 2, w + I.wc);
        }
        if (I.op == 32 && I.wc >= 4) { pointerStorage[w[1]] = w[2]; pointerPointee[w[1]] = w[3]; }
        if ((I.op == 43 || I.op == 50) && I.wc >= 4) {
            uint64_t value = w[3];
            if (tWidth[w[1]] == 64 && I.wc >= 5) value |= (uint64_t(w[4]) << 32);
            constantValues[w[2]] = value;
        }
        if ((I.op == 71 || I.op == 332) && I.wc >= 3) decoratedTypes.insert(w[1]);
        if (I.op == 72 && I.wc >= 4) memberDecoratedTypes.insert(w[1]);
        if (I.op == 74 && I.wc >= 3) { // OpGroupDecorate: group, target IDs...
            for (uint32_t k = 2; k < I.wc; ++k) decoratedTypes.insert(w[k]);
        }
        if (I.op == 75 && I.wc >= 4) { // OpGroupMemberDecorate: group, (struct, member)...
            for (uint32_t k = 2; k + 1 < I.wc; k += 2) memberDecoratedTypes.insert(w[k]);
        }
        if (I.op == 33 && I.wc >= 3) {
            functionTypeReturn[w[1]] = w[2];
            functionTypeParams[w[1]] = std::vector<uint32_t>(w + 3, w + I.wc);
        }
    }
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 54 && I.wc >= 5) functionIdToType[w[2]] = w[4];
    }

    if (!halfTy && int16Types.empty()) { err = "no 16-bit scalar type"; return false; }

    auto scalarOf = [&](uint32_t t) {
        if (mCol.count(t)) t = mCol[t];
        if (vComp.count(t)) t = vComp[t];
        return t;
    };

    std::unordered_set<uint32_t> narrow;
    if (halfTy) narrow.insert(halfTy);
    for (uint32_t t : int16Types) narrow.insert(t);
    for (const auto& kv : vComp) if (narrow.count(kv.second)) narrow.insert(kv.first);
    for (const auto& kv : mCol) if (narrow.count(kv.second)) narrow.insert(kv.first);

    // Determine which aggregate types are reachable from local-address-space pointers and
    // contain a 16-bit leaf. Only these aggregates are cloned; external buffer structs keep
    // their original type/layout and continue to use the existing narrow<->wide value bridge.
    std::function<bool(uint32_t, std::unordered_set<uint32_t>&)> contains16;
    contains16 = [&](uint32_t t, std::unordered_set<uint32_t>& seen) -> bool {
        if (!seen.insert(t).second) return false;
        auto k = tKind.find(t);
        if (k != tKind.end() && (k->second == 21 || k->second == 22)) {
            auto wi = tWidth.find(t);
            return wi != tWidth.end() && wi->second == 16;
        }
        auto vi = vComp.find(t);
        if (vi != vComp.end()) return contains16(vi->second, seen);
        auto mi = mCol.find(t);
        if (mi != mCol.end()) return contains16(mi->second, seen);
        auto ai = arrayElem.find(t);
        if (ai != arrayElem.end()) return contains16(ai->second, seen);
        auto ri = runtimeArrayElem.find(t);
        if (ri != runtimeArrayElem.end()) return contains16(ri->second, seen);
        auto si = structMembers.find(t);
        if (si != structMembers.end()) {
            for (uint32_t memberType : si->second) {
                std::unordered_set<uint32_t> branch = seen;
                if (contains16(memberType, branch)) return true;
            }
        }
        return false;
    };

    std::unordered_set<uint32_t> localNarrowPointerTypes;
    std::unordered_set<uint32_t> localAggregateTypes;
    std::function<void(uint32_t, std::unordered_set<uint32_t>&)> collectLocalNarrow;
    collectLocalNarrow = [&](uint32_t t, std::unordered_set<uint32_t>& seen) {
        if (!seen.insert(t).second) return;
        std::unordered_set<uint32_t> probe;
        if (!contains16(t, probe)) return;
        narrow.insert(t);
        if (arrayElem.count(t) || runtimeArrayElem.count(t) || structMembers.count(t)) localAggregateTypes.insert(t);
        auto vi = vComp.find(t); if (vi != vComp.end()) collectLocalNarrow(vi->second, seen);
        auto mi = mCol.find(t); if (mi != mCol.end()) collectLocalNarrow(mi->second, seen);
        auto ai = arrayElem.find(t); if (ai != arrayElem.end()) collectLocalNarrow(ai->second, seen);
        auto ri = runtimeArrayElem.find(t); if (ri != runtimeArrayElem.end()) collectLocalNarrow(ri->second, seen);
        auto si = structMembers.find(t);
        if (si != structMembers.end()) for (uint32_t memberType : si->second) collectLocalNarrow(memberType, seen);
    };

    for (const auto& kv : pointerStorage) {
        const uint32_t storage = kv.second;
        const uint32_t ptrType = kv.first;
        const uint32_t pointee = pointerPointee[ptrType];
        std::unordered_set<uint32_t> probe;
        if (!contains16(pointee, probe)) continue;

        // These local address spaces can be widened without changing an external ABI.
        if (storage == 4 || storage == 6 || storage == 7) { // Workgroup, Private, Function
            localNarrowPointerTypes.insert(ptrType);
            std::unordered_set<uint32_t> seen;
            collectLocalNarrow(pointee, seen);
            continue;
        }

        // The target reports storageBuffer16BitAccess=1 and
        // uniformAndStorageBuffer16BitAccess=1. PhysicalStorageBuffer is covered by
        // StorageBuffer16BitAccess. Preserve those externally visible layouts.
        if (storage == 2 || storage == 12 || storage == 5349) continue;

        // Input, Output, PushConstant, UniformConstant, Generic, and other address
        // spaces need a separate ABI-aware conversion path. Fail closed.
        err = "16-bit type in unsupported storage class " + std::to_string(storage);
        return false;
    }

    // Pointer-containing aggregates are not handled by the value bridge below (it recursively
    // handles scalars, vectors, matrices, arrays, and structs). Fail closed rather than leave
    // an old pointer type embedded in a widened aggregate.
    for (const auto& st : structMembers) {
        for (uint32_t memberType : st.second) {
            if (localNarrowPointerTypes.count(memberType)) {
                err = "aggregate containing a local 16-bit pointer is not safely lowerable";
                return false;
            }
        }
    }
    for (const auto& ar : arrayElem) {
        if (localNarrowPointerTypes.count(ar.second)) {
            err = "array containing a local 16-bit pointer is not safely lowerable";
            return false;
        }
    }

    // Runtime arrays and explicitly laid-out local aggregates/pointer types cannot be cloned
    // without changing their layout contract. Fail closed instead of producing bad SPIR-V.
    for (uint32_t ptrType : localNarrowPointerTypes) {
        if (decoratedTypes.count(ptrType)) {
            err = "local 16-bit pointer type has explicit decorations";
            return false;
        }
    }
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 39 && I.wc >= 3 && localNarrowPointerTypes.count(w[1])) {
            err = "recursive local 16-bit pointer is not safely lowerable";
            return false;
        }
    }
    for (uint32_t t : localAggregateTypes) {
        if (runtimeArrayElem.count(t)) {
            err = "local aggregate containing 16-bit runtime array is not safely lowerable";
            return false;
        }
        if (decoratedTypes.count(t) || memberDecoratedTypes.count(t)) {
            err = "local aggregate with explicit layout decorations cannot be safely widened";
            return false;
        }
    }

    std::unordered_map<uint32_t, uint32_t> vTy;
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        const char* k = KindsFor(I.op);
        // OpFunction's result ID is a function ID, not an SSA value.
        if (I.op != 54 && k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3) vTy[w[2]] = w[1];
    }

    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 52 && I.wc >= 3 && narrow.count(w[1])) {
            err = "16-bit spec-const-op"; return false;
        }
    }

    uint32_t bound = in[3];
    std::vector<uint32_t> extras;
    std::unordered_map<uint32_t, uint32_t> wide;
    auto existing32 = [&](uint32_t t16) -> uint32_t {
        for (const auto& kv : tKind) {
            const uint32_t other = kv.first;
            if (kv.second == tKind[t16] && tWidth[other] == 32 && tSign[other] == tSign[t16]) return other;
        }
        return 0;
    };

    if (halfTy) {
        if (!floatTy) floatTy = existing32(halfTy);
        if (!floatTy) {
            floatTy = bound++;
            extras.push_back((3u << 16) | 22u); extras.push_back(floatTy); extras.push_back(32);
            tKind[floatTy] = 22; tWidth[floatTy] = 32;
        }
        wide[halfTy] = floatTy;
    }
    for (uint32_t int16Ty : int16Types) {
        uint32_t i32 = existing32(int16Ty);
        if (!i32) {
            i32 = bound++;
            extras.push_back((4u << 16) | 21u); extras.push_back(i32); extras.push_back(32); extras.push_back(tSign[int16Ty]);
            tKind[i32] = 21; tWidth[i32] = 32; tSign[i32] = tSign[int16Ty];
        }
        wide[int16Ty] = i32;
    }

    // Build vectors before matrices so a matrix never observes an uncreated wide column type.
    for (uint32_t t : narrow) {
        if (!vLen.count(t)) continue;
        const uint32_t len = vLen[t], base = wide[vComp[t]];
        if (!base) { err = "missing widened vector component type"; return false; }
        uint32_t f = 0;
        for (const auto& kv : vComp) if (kv.second == base && vLen[kv.first] == len) { f = kv.first; break; }
        if (!f) {
            f = bound++; extras.push_back((4u << 16) | 23u); extras.push_back(f); extras.push_back(base); extras.push_back(len);
            vComp[f] = base; vLen[f] = len;
        }
        wide[t] = f;
    }
    for (uint32_t t : narrow) {
        if (!mCnt.count(t)) continue;
        const uint32_t cnt = mCnt[t], col = wide[mCol[t]];
        if (!col) { err = "missing widened matrix column type"; return false; }
        uint32_t f = 0;
        for (const auto& kv : mCol) if (kv.second == col && mCnt[kv.first] == cnt) { f = kv.first; break; }
        if (!f) {
            f = bound++; extras.push_back((4u << 16) | 24u); extras.push_back(f); extras.push_back(col); extras.push_back(cnt);
            mCol[f] = col; mCnt[f] = cnt;
        }
        wide[t] = f;
    }

    // Recursively clone only Function-reachable arrays/structures that contain narrow leaves.
    std::unordered_set<uint32_t> buildingTypes;
    std::function<uint32_t(uint32_t)> buildWideType;
    buildWideType = [&](uint32_t t) -> uint32_t {
        auto wi = wide.find(t);
        if (wi != wide.end()) return wi->second;
        if (!localAggregateTypes.count(t)) return t;
        if (!buildingTypes.insert(t).second) return 0;

        uint32_t result = 0;
        auto ai = arrayElem.find(t);
        if (ai != arrayElem.end()) {
            const uint32_t newElem = buildWideType(ai->second);
            if (!newElem) { buildingTypes.erase(t); return 0; }
            if (newElem == ai->second) result = t;
            else {
                result = bound++;
                extras.push_back((4u << 16) | 28u); extras.push_back(result); extras.push_back(newElem); extras.push_back(arrayLengthId[t]);
                // Register the synthetic type in the same metadata tables used by emitConv.
                // Without this, the destination is seen as an unknown type while the source
                // is recognized as an array, so recursive lowering rejects every such value.
                arrayElem[result] = newElem;
                arrayLengthId[result] = arrayLengthId[t];
            }
        } else {
            auto si = structMembers.find(t);
            if (si == structMembers.end()) { buildingTypes.erase(t); return 0; }
            std::vector<uint32_t> members; members.reserve(si->second.size());
            bool changed = false;
            for (uint32_t oldMember : si->second) {
                const uint32_t newMember = buildWideType(oldMember);
                if (!newMember) { buildingTypes.erase(t); return 0; }
                members.push_back(newMember);
                if (newMember != oldMember) changed = true;
            }
            if (!changed) result = t;
            else {
                result = bound++;
                extras.push_back((uint32_t(members.size() + 2) << 16) | 30u);
                extras.push_back(result);
                extras.insert(extras.end(), members.begin(), members.end());
                // Keep the new OpTypeStruct visible to the recursive value converter.
                // The SPIR-V declaration alone is not enough: emitConv uses this map to
                // find member types for OpCompositeExtract/OpCompositeConstruct.
                structMembers[result] = members;
            }
        }
        buildingTypes.erase(t);
        wide[t] = result;
        return result;
    };
    for (uint32_t t : localAggregateTypes) {
        if (!buildWideType(t)) { err = "failed to clone Function aggregate type"; return false; }
    }

    // A local-address-space pointer must point to the cloned 32-bit pointee type. External
    // StorageBuffer/Uniform/PhysicalStorageBuffer pointer types remain untouched.
    std::unordered_map<uint32_t, uint32_t> localPointerTypeMap;
    std::unordered_set<uint32_t> widenedLocalPointerTypes;
    for (uint32_t oldPtrType : localNarrowPointerTypes) {
        const uint32_t oldPointee = pointerPointee[oldPtrType];
        auto wi = wide.find(oldPointee);
        if (wi == wide.end() || wi->second == oldPointee) continue;
        const uint32_t newPtrType = bound++;
        extras.push_back((4u << 16) | 32u); extras.push_back(newPtrType);
        extras.push_back(pointerStorage[oldPtrType]); extras.push_back(wi->second);
        localPointerTypeMap[oldPtrType] = newPtrType;
        widenedLocalPointerTypes.insert(newPtrType);
    }

    auto remapType = [&](uint32_t t) -> uint32_t {
        auto pi = localPointerTypeMap.find(t);
        if (pi != localPointerTypeMap.end()) return pi->second;
        auto wi = wide.find(t);
        return wi != wide.end() ? wi->second : t;
    };

    // Function-local arrays used for value conversion need a finite compile-time length.
    for (uint32_t t : localAggregateTypes) {
        auto ai = arrayElem.find(t);
        if (ai != arrayElem.end()) {
            auto li = constantValues.find(arrayLengthId[t]);
            if (li == constantValues.end() || li->second > 4096) {
                err = "Function array containing 16-bit values has non-literal or excessive length";
                return false;
            }
        }
    }

    // Clone narrow scalar/vector/aggregate constants with their widened result types.
    // These clones are emitted after all new type declarations and before OpTypeFunction.
    std::unordered_map<uint32_t, uint32_t> wId, nId;
    auto W = [&](uint32_t id) { auto it = wId.find(id); return it == wId.end() ? id : it->second; };
    auto N = [&](uint32_t id) { auto it = nId.find(id); return it == nId.end() ? id : it->second; };
    for (const Inst& I : insts) {
        if (!constLike(I.op) || I.wc < 3) continue;
        const uint32_t* w = in + I.off;
        const uint32_t t = w[1], id = w[2];
        if (!narrow.count(t)) continue;
        const uint32_t nid = bound++;
        if (I.op == 43 || I.op == 50) {
            if (I.wc < 4) { err = "short 16-bit constant"; return false; }
            const uint16_t bits = static_cast<uint16_t>(w[3] & 0xFFFFu);
            uint32_t value = 0;
            if (tKind[scalarOf(t)] == 22) value = Float16BitsToFloat32Bits(bits);
            else if (tSign[scalarOf(t)]) value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(bits)));
            else value = bits;
            extras.push_back((4u << 16) | I.op); extras.push_back(wide[t]); extras.push_back(nid); extras.push_back(value);
        } else if (I.op == 44 || I.op == 51) {
            extras.push_back((I.wc << 16) | I.op); extras.push_back(wide[t]); extras.push_back(nid);
            for (uint32_t k = 3; k < I.wc; ++k) extras.push_back(W(w[k]));
        } else {
            extras.push_back((3u << 16) | I.op); extras.push_back(wide[t]); extras.push_back(nid);
        }
        wId[id] = nid;
        nId[id] = id;
    }

    // Clone only those OpTypeFunction declarations whose by-value signature or Function
    // pointer parameters changed. They are emitted with the other generated declarations
    // immediately before the first OpFunction, after every original type is available.
    std::unordered_map<uint32_t, uint32_t> functionTypeMap;
    for (const auto& ft : functionTypeParams) {
        const uint32_t oldTypeId = ft.first;
        const uint32_t oldRet = functionTypeReturn[oldTypeId];
        const uint32_t newRet = remapType(oldRet);
        std::vector<uint32_t> newParams; newParams.reserve(ft.second.size());
        bool changed = (newRet != oldRet);
        for (uint32_t oldParam : ft.second) {
            const uint32_t newParam = remapType(oldParam);
            newParams.push_back(newParam);
            if (newParam != oldParam) changed = true;
        }
        if (!changed) { functionTypeMap[oldTypeId] = oldTypeId; continue; }
        const uint32_t newTypeId = bound++;
        extras.push_back((uint32_t(newParams.size() + 3) << 16) | 33u);
        extras.push_back(newTypeId); extras.push_back(newRet);
        extras.insert(extras.end(), newParams.begin(), newParams.end());
        functionTypeMap[oldTypeId] = newTypeId;
    }

    // Remove the obsolete narrow local pointer and function-signature declarations. Every
    // OpVariable/access-chain/OpFunction now names the widened type. Keeping stale declarations
    // could leave unused Function->f16/i16 type graphs behind after Float16/Int16 capabilities
    // are removed, confusing older driver front ends.
    std::unordered_set<uint32_t> obsoleteTypeIds;
    for (const auto& p : localPointerTypeMap) obsoleteTypeIds.insert(p.first);
    for (const auto& ft : functionTypeMap) {
        if (ft.second != ft.first) obsoleteTypeIds.insert(ft.first);
    }

    auto convOp = [&](uint32_t ty) -> uint32_t {
        const uint32_t s = scalarOf(ty);
        if (tKind[s] == 22) return 115u; // OpFConvert
        return tSign[s] ? 114u : 113u;   // OpSConvert / OpUConvert
    };

    std::string convError;
    std::function<bool(std::vector<uint32_t>&, uint32_t, uint32_t, uint32_t, uint32_t)> emitConv;
    emitConv = [&](std::vector<uint32_t>& dst, uint32_t toTy, uint32_t fromTy, uint32_t newId, uint32_t srcId) -> bool {
        if (toTy == fromTy) {
            dst.push_back((4u << 16) | 83u); dst.push_back(toTy); dst.push_back(newId); dst.push_back(srcId);
            return true;
        }

        // Matrices, fixed arrays, and structures are converted member-by-member. This keeps
        // nested layout types coherent instead of treating an aggregate as a scalar conversion.
        std::vector<uint32_t> values;
        uint32_t fromElementType = 0, toElementType = 0, count = 0;
        if (mCnt.count(toTy) && mCnt.count(fromTy)) {
            if (mCnt[toTy] != mCnt[fromTy]) {
                convError = "matrix conversion column count mismatch";
                return false;
            }
            count = mCnt[toTy]; fromElementType = mCol[fromTy]; toElementType = mCol[toTy];
        } else if (arrayElem.count(toTy) && arrayElem.count(fromTy)) {
            const uint64_t fromLen = constantValues[arrayLengthId[fromTy]];
            const uint64_t toLen = constantValues[arrayLengthId[toTy]];
            if (fromLen != toLen || fromLen > 4096) { convError = "array conversion length mismatch"; return false; }
            count = static_cast<uint32_t>(fromLen); fromElementType = arrayElem[fromTy]; toElementType = arrayElem[toTy];
        } else if (structMembers.count(toTy) && structMembers.count(fromTy)) {
            const auto& fromMembers = structMembers[fromTy];
            const auto& toMembers = structMembers[toTy];
            if (fromMembers.size() != toMembers.size()) { convError = "structure conversion member count mismatch"; return false; }
            count = static_cast<uint32_t>(fromMembers.size());
            values.reserve(count);
            for (uint32_t iMember = 0; iMember < count; ++iMember) {
                const uint32_t extId = bound++;
                dst.push_back((5u << 16) | 81u); dst.push_back(fromMembers[iMember]); dst.push_back(extId); dst.push_back(srcId); dst.push_back(iMember);
                if (toMembers[iMember] == fromMembers[iMember]) values.push_back(extId);
                else {
                    const uint32_t childId = bound++;
                    if (!emitConv(dst, toMembers[iMember], fromMembers[iMember], childId, extId)) return false;
                    values.push_back(childId);
                }
            }
            dst.push_back((uint32_t(count + 3) << 16) | 80u); dst.push_back(toTy); dst.push_back(newId);
            dst.insert(dst.end(), values.begin(), values.end());
            return true;
        } else {
            // Do not encode an aggregate shape mismatch as a scalar conversion opcode.
            // OpUConvert/OpSConvert/OpFConvert are not valid for structures or arrays;
            // this was the source of the malformed OpUConvert %struct result in v11.
            const bool toAggregate =
                mCnt.count(toTy) != 0 || arrayElem.count(toTy) != 0 ||
                runtimeArrayElem.count(toTy) != 0 || structMembers.count(toTy) != 0;
            const bool fromAggregate =
                mCnt.count(fromTy) != 0 || arrayElem.count(fromTy) != 0 ||
                runtimeArrayElem.count(fromTy) != 0 || structMembers.count(fromTy) != 0;
            if (toAggregate || fromAggregate) {
                auto typeKind = [&](uint32_t ty) -> const char* {
                    if (structMembers.count(ty)) return "struct";
                    if (arrayElem.count(ty)) return "array";
                    if (runtimeArrayElem.count(ty)) return "runtime-array";
                    if (mCnt.count(ty)) return "matrix";
                    if (vComp.count(ty)) return "vector";
                    if (pointerPointee.count(ty)) return "pointer";
                    auto tk = tKind.find(ty);
                    if (tk != tKind.end()) return tk->second == 22 ? "float" : "integer";
                    return "unknown";
                };
                convError = std::string("aggregate conversion shape mismatch: to type ") +
                    std::to_string(toTy) + " (" + typeKind(toTy) + "), from type " +
                    std::to_string(fromTy) + " (" + typeKind(fromTy) + ")";
                return false;
            }

            // Vector conversions are legal only when both sides are vectors of the same
            // component count. A scalar/vector mismatch is not a numeric conversion.
            const bool toVector = vComp.count(toTy) != 0;
            const bool fromVector = vComp.count(fromTy) != 0;
            if (toVector != fromVector || (toVector && vLen[toTy] != vLen[fromTy])) {
                convError = "vector conversion shape mismatch";
                return false;
            }

            // Scalar and same-width-vector numeric conversions use native SPIR-V opcodes.
            dst.push_back((4u << 16) | convOp(toTy)); dst.push_back(toTy); dst.push_back(newId); dst.push_back(srcId);
            return true;
        }

        values.reserve(count);
        for (uint32_t element = 0; element < count; ++element) {
            const uint32_t extId = bound++;
            dst.push_back((5u << 16) | 81u); dst.push_back(fromElementType); dst.push_back(extId); dst.push_back(srcId); dst.push_back(element);
            if (toElementType == fromElementType) values.push_back(extId);
            else {
                const uint32_t childId = bound++;
                if (!emitConv(dst, toElementType, fromElementType, childId, extId)) return false;
                values.push_back(childId);
            }
        }
        dst.push_back((uint32_t(count + 3) << 16) | 80u); dst.push_back(toTy); dst.push_back(newId);
        dst.insert(dst.end(), values.begin(), values.end());
        return true;
    };

    out.clear(); out.reserve(n + 8192); out.insert(out.end(), in, in + 5);
    std::vector<uint32_t> pend;
    bool extrasDone = false;
    bool inFunctionBody = false;
    uint32_t currentFunctionReturnType = 0;
    auto flush = [&]() {
        if (inFunctionBody && !pend.empty()) out.insert(out.end(), pend.begin(), pend.end());
        pend.clear();
    };

    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        const uint32_t op = I.op;

        if (op == 54) {
            pend.clear();
            inFunctionBody = false;
            currentFunctionReturnType = w[1];
        }
        if (op == 56) {
            if (inFunctionBody) flush(); else pend.clear();
            inFunctionBody = false;
            currentFunctionReturnType = 0;
        }
        if (op == 8 || op == 9) continue;
        if (op == 5 && I.wc >= 2 && obsoleteTypeIds.count(w[1])) continue; // OpName of dropped type
        if (op == 32 && I.wc >= 4 && localPointerTypeMap.count(w[1])) continue; // old OpTypePointer
        if (op == 33 && I.wc >= 3) {
            auto ft = functionTypeMap.find(w[1]);
            if (ft != functionTypeMap.end() && ft->second != w[1]) continue; // old OpTypeFunction
        }
        if (dropCaps && op == 17 && I.wc >= 2 && (w[1] == 9 || w[1] == 8 || w[1] == 22)) continue;

        // OpPhi must remain at the beginning of a block; local variables must remain before
        // ordinary entry-block instructions. Parameter bridges are flushed after OpLabel.
        if (inFunctionBody && op != 245 && op != 248 && op != 55 && op != 59 && op != 54 && op != 56)
            flush();

        // Generated types and cloned function signatures must appear after the original type
        // declarations (which may be interleaved) and before the first function body.
        if (op == 54 && !extrasDone) {
            extrasDone = true;
            out.insert(out.end(), extras.begin(), extras.end());
        }

        std::vector<uint32_t> v(w, w + I.wc);
        const char* k = KindsFor(op);
        const bool hasRes = (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3);
        const uint32_t resTy = hasRes ? w[1] : 0;
        const uint32_t resId = hasRes ? w[2] : 0;
        const bool resNarrow = hasRes && narrow.count(resTy) != 0;

        if (op == 54 && I.wc >= 5) {
            auto ft = functionTypeMap.find(w[4]);
            if (ft != functionTypeMap.end()) v[4] = ft->second;
        }

        // Rewrite local pointer result types on variables, access chains, parameters,
        // pointer phis/selects/copies, and pointer-returning calls.
        if (hasRes) {
            auto pi = localPointerTypeMap.find(resTy);
            if (pi != localPointerTypeMap.end()) {
                v[1] = pi->second;
                vTy[resId] = pi->second;
            }
        }

        // Preserve original OpTypeFunction declarations. OpFunction below is redirected to
        // the cloned, widened function type generated above.

        bool localWidenedLoad = false;
        if (op == 61 && I.wc >= 4) {
            auto pt = vTy.find(w[3]);
            localWidenedLoad = pt != vTy.end() && widenedLocalPointerTypes.count(pt->second) != 0;
        }
        bool localWidenedStore = false;
        if (op == 62 && I.wc >= 3) {
            auto pt = vTy.find(w[1]);
            localWidenedStore = pt != vTy.end() && widenedLocalPointerTypes.count(pt->second) != 0;
        }
        bool localWidenedInitializer = false;
        if (op == 59 && I.wc >= 5) {
            auto pt = localPointerTypeMap.find(resTy);
            localWidenedInitializer = pt != localPointerTypeMap.end();
        }

        bool keep = keepNarrow(op);
        // Function signatures and local-address-space memory are widened at their source,
        // rather than keeping an unsupported 16-bit parameter/load and bridging it afterwards.
        if ((op == 54 || op == 55 || op == 57) && resNarrow) keep = false;
        if (op == 61 && localWidenedLoad) keep = false;
        if ((op == 81 || op == 77) && I.wc >= 4) {
            auto it = vTy.find(w[3]); if (it != vTy.end() && narrow.count(it->second)) keep = false;
        }
        const bool resWide = resNarrow && !keep && !constLike(op);
        if (resWide) v[1] = wide[resTy];

        ForEachId(v.data(), op, I.wc, [&](size_t idx, char role) {
            if (role != 'I') return;

            // Call arguments are mapped using the callee's declared original signature.
            // This is required even when the call returns void or a non-narrow type.
            if (op == 57 && idx >= 4 && I.wc >= 4) {
                auto ft = functionIdToType.find(w[3]);
                if (ft != functionIdToType.end()) {
                    auto fp = functionTypeParams.find(ft->second);
                    const size_t argIndex = idx - 4;
                    if (fp != functionTypeParams.end() && argIndex < fp->second.size() && narrow.count(fp->second[argIndex]))
                        v[idx] = W(v[idx]);
                }
                return;
            }

            // Values stored to widened Function variables must use their wide representation.
            if (op == 62 && localWidenedStore && idx == 2) { v[idx] = W(v[idx]); return; }
            if (op == 59 && localWidenedInitializer && idx == 4) { v[idx] = W(v[idx]); return; }

            // Function returns use the widened return type from OpTypeFunction.
            if (op == 254 && idx == 1 && currentFunctionReturnType && narrow.count(currentFunctionReturnType)) {
                v[idx] = W(v[idx]); return;
            }

            auto it = vTy.find(v[idx]);
            if (it == vTy.end() || !narrow.count(it->second)) return;
            v[idx] = resWide ? W(v[idx]) : N(v[idx]);
        });

        out.insert(out.end(), v.begin(), v.end());
        if (op == 248) {
            inFunctionBody = true;
            continue;
        }
        // OpFunction's result ID is a function ID, not an SSA result; its signature was
        // widened above, so it must not receive a value-conversion instruction.
        if (!resNarrow || constLike(op) || op == 54) continue;
        if (!wide.count(resTy)) continue;

        const uint32_t nid = bound++;
        uint32_t toTy = 0, fromTy = 0;
        if (resWide) { toTy = resTy; fromTy = wide[resTy]; wId[resId] = resId; nId[resId] = nid; }
        else { toTy = wide[resTy]; fromTy = resTy; nId[resId] = resId; wId[resId] = nid; }

        if (op == 55 || op == 245) {
            std::vector<uint32_t> tmp;
            if (!emitConv(tmp, toTy, fromTy, nid, resId)) { err = convError.empty() ? "failed to convert Function parameter/phi" : convError; return false; }
            pend.insert(pend.end(), tmp.begin(), tmp.end());
        } else {
            if (!emitConv(out, toTy, fromTy, nid, resId)) { err = convError.empty() ? "failed to convert narrow result" : convError; return false; }
        }
    }

    flush();
    if (!extrasDone) out.insert(out.end(), extras.begin(), extras.end());
    out[3] = bound;
    if (outLocalPointerTypes) *outLocalPointerTypes = localPointerTypeMap.size();
    if (outLocalAggregateTypes) *outLocalAggregateTypes = localAggregateTypes.size();
    return true;
}


	
	
// v9b: fp16 ALU -> fp32, 16-битная память не тронута.
// Исправлено: OpUndef=1, spec-const опкоды 48..51, все новые типы/константы
// идут ОДНИМ блоком перед первым OpFunction, матрицы поддержаны.
inline bool LowerFp16Alu(const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err, bool dropF16Cap) {
    if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }
    std::vector<Inst> insts;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
        if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
        insts.push_back({ op, wc, i });
        i += wc;
    }
    auto constLike = [](uint32_t op) {
        return op == 1 || op == 41 || op == 42 || op == 43 || op == 44 || op == 45 ||
               op == 46 || op == 48 || op == 49 || op == 50 || op == 51;
    };
    auto keepNarrow = [](uint32_t op) {
        return op == 61 || op == 55 || op == 57 || (op >= 110 && op <= 115) ||
               (op >= 86 && op <= 98) || (op >= 100 && op <= 107);
    };
    uint32_t halfTy = 0, floatTy = 0;
    std::unordered_map<uint32_t, uint32_t> vComp, vLen, mCol, mCnt;
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 22 && I.wc >= 3) {
            if (w[2] == 16) halfTy = w[1];
            if (w[2] == 32 && !floatTy) floatTy = w[1];
        }
        if (I.op == 23 && I.wc >= 4) { vComp[w[1]] = w[2]; vLen[w[1]] = w[3]; }
        if (I.op == 24 && I.wc >= 4) { mCol[w[1]] = w[2]; mCnt[w[1]] = w[3]; }
        if (I.op == 54) break;
    }
    if (!halfTy) { err = "no fp16 type"; return false; }
    std::unordered_set<uint32_t> narrow;
    narrow.insert(halfTy);
    for (const auto& kv : vComp) if (kv.second == halfTy) narrow.insert(kv.first);
    for (const auto& kv : mCol) if (narrow.count(kv.second)) narrow.insert(kv.first);
    std::unordered_map<uint32_t, uint32_t> vTy;
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        const char* k = KindsFor(I.op);
        if (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3) vTy[w[2]] = w[1];
        if (I.op == 59 && I.wc >= 4) vTy[w[2]] = w[1];
    }
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        if (I.op == 52 && I.wc >= 3 && narrow.count(w[1])) { err = "fp16 spec-const-op"; return false; }
        if (I.op == 124 && I.wc >= 4) {
            if (narrow.count(w[1])) { err = "fp16 bitcast"; return false; }
            auto it = vTy.find(w[3]);
            if (it != vTy.end() && narrow.count(it->second)) { err = "fp16 bitcast"; return false; }
        }
    }
    uint32_t bound = in[3];
    std::vector<uint32_t> extras;
    if (!floatTy) {
        floatTy = bound++;
        extras.push_back((3u << 16) | 22u); extras.push_back(floatTy); extras.push_back(32);
    }
    std::unordered_map<uint32_t, uint32_t> wide;
    wide[halfTy] = floatTy;
    for (uint32_t t : narrow) {
        if (t == halfTy || !vLen.count(t)) continue;
        const uint32_t len = vLen[t];
        uint32_t f = 0;
        for (const auto& kv : vComp) if (kv.second == floatTy && vLen[kv.first] == len) { f = kv.first; break; }
        if (!f) {
            f = bound++;
            extras.push_back((4u << 16) | 23u); extras.push_back(f); extras.push_back(floatTy); extras.push_back(len);
            vComp[f] = floatTy; vLen[f] = len;
        }
        wide[t] = f;
    }
    for (uint32_t t : narrow) {
        if (!mCnt.count(t)) continue;
        const uint32_t cnt = mCnt[t], col = wide[mCol[t]];
        uint32_t f = 0;
        for (const auto& kv : mCol) if (kv.second == col && mCnt[kv.first] == cnt) { f = kv.first; break; }
        if (!f) {
            f = bound++;
            extras.push_back((4u << 16) | 24u); extras.push_back(f); extras.push_back(col); extras.push_back(cnt);
            mCol[f] = col; mCnt[f] = cnt;
        }
        wide[t] = f;
    }
    std::unordered_map<uint32_t, uint32_t> wId, nId;
    auto W = [&](uint32_t id) { auto it = wId.find(id); return it == wId.end() ? id : it->second; };
    auto N = [&](uint32_t id) { auto it = nId.find(id); return it == nId.end() ? id : it->second; };
    for (const Inst& I : insts) {
        if (!constLike(I.op) || I.wc < 3) continue;
        const uint32_t* w = in + I.off;
        const uint32_t t = w[1], id = w[2];
        if (!narrow.count(t)) continue;
        const uint32_t nid = bound++;
        if (I.op == 43 || I.op == 50) {
            if (I.wc < 4) { err = "short fp16 constant"; return false; }
            extras.push_back((4u << 16) | I.op); extras.push_back(wide[t]); extras.push_back(nid);
            extras.push_back(Float16BitsToFloat32Bits(static_cast<uint16_t>(w[3] & 0xFFFFu)));
        } else if (I.op == 44 || I.op == 51) {
            extras.push_back((I.wc << 16) | I.op); extras.push_back(wide[t]); extras.push_back(nid);
            for (uint32_t k = 3; k < I.wc; ++k) extras.push_back(W(w[k]));
        } else {
            extras.push_back((3u << 16) | I.op); extras.push_back(wide[t]); extras.push_back(nid);
        }
        wId[id] = nid; nId[id] = id;
    }
    auto emitConv = [&](std::vector<uint32_t>& dst, uint32_t toTy, uint32_t fromTy, uint32_t newId, uint32_t srcId) {
        const bool toMat = mCnt.count(toTy) != 0, fromMat = mCnt.count(fromTy) != 0;
        if (!toMat && !fromMat) {
            dst.push_back((4u << 16) | 115u); dst.push_back(toTy); dst.push_back(newId); dst.push_back(srcId);
            return;
        }
        if (!toMat || !fromMat || mCnt[toTy] != mCnt[fromTy]) {
            dst.push_back((4u << 16) | 115u); dst.push_back(toTy); dst.push_back(newId); dst.push_back(srcId);
            return;
        }
        const uint32_t C = mCnt[toTy], toCol = mCol[toTy], fromCol = mCol[fromTy];
        std::vector<uint32_t> cols;
        for (uint32_t i = 0; i < C; ++i) {
            const uint32_t e = bound++;
            dst.push_back((5u << 16) | 81u); dst.push_back(fromCol); dst.push_back(e); dst.push_back(srcId); dst.push_back(i);
            const uint32_t c = bound++;
            dst.push_back((4u << 16) | 115u); dst.push_back(toCol); dst.push_back(c); dst.push_back(e);
            cols.push_back(c);
        }
        dst.push_back(((3u + C) << 16) | 80u); dst.push_back(toTy); dst.push_back(newId);
        for (uint32_t c : cols) dst.push_back(c);
    };
    out.clear(); out.reserve(n + 2048);
    out.insert(out.end(), in, in + 5);
    std::vector<uint32_t> pend;
    bool extrasDone = false;
    auto flush = [&]() { out.insert(out.end(), pend.begin(), pend.end()); pend.clear(); };
    for (const Inst& I : insts) {
        const uint32_t* w = in + I.off;
        const uint32_t op = I.op;
		        // OpLine(8)/OpNoLine(9) - чисто отладочная информация. Вырезаем: они стоят
        // между OpFunctionParameter и OpLabel (и между OpPhi) и ломают порядок блока,
        // из-за чего отложенный OpFConvert оказывался вне блока.
        if (op == 8 || op == 9) continue;
        if (dropF16Cap && op == 17 && I.wc >= 2 && (w[1] == 9 || w[1] == 8 || w[1] == 22)) continue;
        if (op == 54 && !extrasDone) { extrasDone = true; out.insert(out.end(), extras.begin(), extras.end()); }
        if (op != 245 && op != 248 && op != 55) flush();
        std::vector<uint32_t> v(w, w + I.wc);
        const char* k = KindsFor(op);
        const bool hasRes = (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3);
        const uint32_t resTy = hasRes ? w[1] : 0;
        const uint32_t resId = hasRes ? w[2] : 0;
        const bool resNarrow = hasRes && narrow.count(resTy) != 0;
        bool keep = keepNarrow(op);
        if ((op == 81 || op == 77) && I.wc >= 4) {
            auto it = vTy.find(w[3]);
            if (it != vTy.end() && narrow.count(it->second)) keep = false;
        }
        const bool resWide = resNarrow && !keep && !constLike(op);
        if (resWide) v[1] = wide[resTy];
        ForEachId(v.data(), op, I.wc, [&](size_t idx, char role) {
            if (role != 'I') return;
            auto it = vTy.find(v[idx]);
            if (it == vTy.end() || !narrow.count(it->second)) return;
            v[idx] = resWide ? W(v[idx]) : N(v[idx]);
        });
        out.insert(out.end(), v.begin(), v.end());
        if (op == 248) { flush(); continue; }
        // OpFunction's result ID names a function. Its return type belongs to
        // the function signature and must not get an OpFConvert appended here.
        if (!resNarrow || constLike(op) || op == 54) continue;
        const uint32_t nid = bound++;
        uint32_t toTy, fromTy;
        if (resWide) { toTy = resTy; fromTy = wide[resTy]; wId[resId] = resId; nId[resId] = nid; }
        else { toTy = wide[resTy]; fromTy = resTy; nId[resId] = resId; wId[resId] = nid; }
        if (op == 55 || op == 245) emitConv(pend, toTy, fromTy, nid, resId);
        else emitConv(out, toTy, fromTy, nid, resId);
    }
    flush();
    if (!extrasDone) out.insert(out.end(), extras.begin(), extras.end());
    out[3] = bound;
    return true;
}
	
	
	
	
	
	
	
	
	// Promote 16-bit floats to 32-bit (for GPUs without fp16 ALU / drivers that fail on fp16 subgroup ops).
    // Produces VALID SPIR-V: the 16-bit float type and the 32-bit float type are merged into one id,
    // every type that becomes identical afterwards (vectors, pointers, images, function types...) is merged too,
    // constants are converted, and OpFConvert between the (now equal) types becomes OpCopyObject.
    inline bool PromoteFloat16(const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err) {
        if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }
        std::vector<Inst> insts;
        for (size_t i = 5; i < n;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
            if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
            insts.push_back({ op, wc, i });
            i += wc;
        }
        std::unordered_set<uint32_t> f16;
        for (const Inst& I : insts) if (I.op == 22 && I.wc >= 3 && in[I.off + 2] == 16) f16.insert(in[I.off + 1]);
        if (f16.empty()) { err = "no 16-bit float"; return false; }
        // refuse modules where a 16-bit float is bit-cast to/from 16-bit integers (cannot be promoted safely)
            // Int16 сам по себе не мешает. Опасен только OpBitcast с участием 16-битного float.
    {
        std::unordered_set<uint32_t> f16Any = f16;
        for (const Inst& I : insts)
            if (I.op == 23 && I.wc >= 4 && f16.count(in[I.off + 2])) f16Any.insert(in[I.off + 1]);
        std::unordered_map<uint32_t, uint32_t> vType;
        for (const Inst& I : insts) {
            const char* k = KindsFor(I.op);
            if (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3) vType[in[I.off + 2]] = in[I.off + 1];
            if (I.op == 59 && I.wc >= 4) vType[in[I.off + 2]] = in[I.off + 1];
        }
        for (const Inst& I : insts) {
            if (I.op != 124 || I.wc < 4) continue;
            if (f16Any.count(in[I.off + 1])) { err = "bitcast of 16-bit float"; return false; }
            auto it = vType.find(in[I.off + 3]);
            if (it != vType.end() && f16Any.count(it->second)) { err = "bitcast of 16-bit float"; return false; }
        }
    }

        std::unordered_map<uint32_t, uint32_t> remap;               // id -> surviving id
        auto R = [&](uint32_t id) { auto it = remap.find(id); return it == remap.end() ? id : it->second; };

        // survivor of the float types: the first declared one
        uint32_t surv = 0;
        for (const Inst& I : insts) {
            if (I.op == 22 && I.wc >= 3 && (in[I.off + 2] == 32 || in[I.off + 2] == 16)) {
                const uint32_t id = in[I.off + 1];
                if (!surv) surv = id; else remap[id] = surv;
            }
        }

        // type declarations that must be unique (non-aggregates)
        auto uniqueType = [](uint32_t op) {
            return op == 19 || op == 20 || op == 21 || op == 22 || op == 23 || op == 24 || op == 25 || op == 26 ||
                   op == 27 || op == 32 || op == 33;
        };
        std::map<std::vector<uint32_t>, uint32_t> seen;
        std::vector<char> drop(insts.size(), 0);
        // first pass: build keys in declaration order with progressively refined remap
        std::vector<std::vector<uint32_t>> rewritten(insts.size());
        for (size_t ii = 0; ii < insts.size(); ++ii) {
            const Inst& I = insts[ii];
            const uint32_t* w = in + I.off;
            rewritten[ii].assign(w, w + I.wc);
            if (I.op == 54) break;                                    // functions start: all types/constants are before
            // apply the remap to id operands so far
            ForEachId(rewritten[ii].data(), I.op, I.wc, [&](size_t idx, char role) {
                if (role != 'R') rewritten[ii][idx] = R(rewritten[ii][idx]);
            });
            if (!uniqueType(I.op)) continue;
            const uint32_t id = rewritten[ii][1];
            if (I.op == 22) {                                         // float: only the survivor is kept, as 32-bit
                if (id != surv) { drop[ii] = 1; continue; }
                rewritten[ii][2] = 32;
            }
            std::vector<uint32_t> key(rewritten[ii].begin() + 2, rewritten[ii].end());
            key.insert(key.begin(), I.op);
            if (I.op == 32 || I.op == 33 || I.op == 23 || I.op == 24 || I.op == 25 || I.op == 27) {}
            auto it = seen.find(key);
            if (it == seen.end()) { seen[key] = id; }
            else if (it->second != id) { remap[id] = it->second; drop[ii] = 1; }
        }
        // second pass over the whole module: apply the final remap, convert constants, fix conversions
        std::unordered_map<uint32_t, uint32_t> typeOf;               // value id -> result type id (remapped)
        out.clear();
        out.reserve(n);
        out.insert(out.end(), in, in + 5);
        std::set<std::vector<uint32_t>> decorations;
        bool pastTypes = false;
        for (size_t ii = 0; ii < insts.size(); ++ii) {
            const Inst& I = insts[ii];
            if (drop[ii]) continue;
            std::vector<uint32_t> w(in + I.off, in + I.off + I.wc);
            if (I.op == 54) pastTypes = true;
            const bool wasF16Const = (I.op == 43 || I.op == 48) && I.wc >= 4 && f16.count(in[I.off + 1]);
            ForEachId(w.data(), I.op, I.wc, [&](size_t idx, char role) {
                if (role != 'R') w[idx] = R(w[idx]);
            });
            if (I.op == 17 && I.wc >= 2 && (w[1] == 9 || w[1] == 8)) continue;                    // Float16 / Float16Buffer
            if (I.op == 22 && I.wc >= 3 && w[2] == 16) w[2] = 32;
            if (wasF16Const) w[3] = Float16BitsToFloat32Bits(static_cast<uint16_t>(in[I.off + 3] & 0xFFFFu));
            if (I.op == 71 || I.op == 72) {                                                        // drop exact duplicate decorations
                std::vector<uint32_t> key(w.begin() + 1, w.end());
                key.insert(key.begin(), I.op);
                if (!decorations.insert(key).second) continue;
            }
            const char* k = KindsFor(I.op);
            if (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3) typeOf[w[2]] = w[1];
            if (I.op == 115 /*OpFConvert*/ && I.wc == 4) {
                auto it = typeOf.find(w[3]);
                if (it != typeOf.end() && it->second == w[1]) w[0] = (4u << 16) | 83u;            // OpCopyObject
            }
            out.insert(out.end(), w.begin(), w.end());
        }
        (void)pastTypes;
        return true;
    }

inline bool StripRayQuery(const uint32_t* in, size_t n, std::vector<uint32_t>& out, std::string& err) {
        if (!in || n < 5 || in[0] != 0x07230203u) { err = "not spir-v"; return false; }
        std::vector<Inst> insts;
        for (size_t i = 5; i < n;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xFFFF;
            if (wc == 0 || i + wc > n) { err = "bad instruction length"; return false; }
            insts.push_back({ op, wc, i });
            i += wc;
        }

        std::unordered_set<uint32_t> removedTypes, removedValues;
        bool any = false;
        // ---- pass 1: find acceleration-structure / ray-query types and everything derived from them
        for (const Inst& I : insts) {
            const uint32_t* w = in + I.off;
            switch (I.op) {
            case 5341: case 4472: removedTypes.insert(w[1]); any = true; break;
            case 32: if (I.wc >= 4 && removedTypes.count(w[3])) removedTypes.insert(w[1]); break;
            case 28: if (I.wc >= 4 && removedTypes.count(w[2])) removedTypes.insert(w[1]); break;
            case 29: if (I.wc >= 3 && removedTypes.count(w[2])) removedTypes.insert(w[1]); break;
            case 30: for (uint32_t k = 2; k < I.wc; ++k) if (removedTypes.count(w[k])) { err = "struct contains AS/RayQuery"; return false; } break;
            case 33: for (uint32_t k = 2; k < I.wc; ++k) if (removedTypes.count(w[k])) { err = "function type uses AS/RayQuery"; return false; } break;
            case 59: if (I.wc >= 4 && removedTypes.count(w[1])) removedValues.insert(w[2]); break;
            default: {
                const char* k = KindsFor(I.op);
                if (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3 && removedTypes.count(w[1])) {
                    if (I.op == 55 || I.op == 245 || I.op == 169 || I.op == 57) { err = "AS/RayQuery passed through function/phi/select"; return false; }
                    removedValues.insert(w[2]);
                }
            }
            }
        }
        if (!any) { err = "no ray query"; return false; }

        // ---- pass 2: which result types need a null constant for replaced instructions
        uint32_t bound = in[3];
        std::unordered_map<uint32_t, uint32_t> nullFor;     // type id -> new constant id
        std::vector<uint32_t> nullOrder;
        for (const Inst& I : insts) {
            if (!IsRqValueOp(I.op)) continue;
            const uint32_t* w = in + I.off;
            if (I.wc < 4) { err = "short rq op"; return false; }
            if (removedTypes.count(w[1])) { err = "rq op returns removed type"; return false; }
            if (!nullFor.count(w[1])) { nullFor[w[1]] = bound++; nullOrder.push_back(w[1]); }
        }

        // ---- pass 3: emit
        out.clear();
        out.reserve(n);
        out.insert(out.end(), in, in + 5);
        bool constsEmitted = false;
        auto emitConsts = [&]() {
            if (constsEmitted) return;
            constsEmitted = true;
            for (uint32_t t : nullOrder) { out.push_back((3u << 16) | 46u); out.push_back(t); out.push_back(nullFor[t]); }
        };
        auto isRemoved = [&](uint32_t id) { return removedTypes.count(id) || removedValues.count(id); };

        for (const Inst& I : insts) {
            const uint32_t* w = in + I.off;
            const uint32_t op = I.op;
            if (op == 54 /*OpFunction*/) emitConsts();
            if (op == 17 && I.wc >= 2 && (w[1] == 4472 || w[1] == 4478 || w[1] == 4471)) continue;   // capabilities
            if (op == 10 && I.wc >= 2) {                                                              // extension
                const char* s = reinterpret_cast<const char*>(w + 1);
                if (strncmp(s, "SPV_KHR_ray_query", (I.wc - 1) * 4) == 0 && strlen("SPV_KHR_ray_query") <= (I.wc - 1) * 4) continue;
            }
            if ((op == 5 /*Name*/ || op == 71 /*Decorate*/ || op == 72 /*MemberDecorate*/ || op == 6 /*MemberName*/ ||
                 op == 332 /*DecorateId*/ || op == 5632 /*DecorateString*/ || op == 5633 /*MemberDecorateString*/) &&
                I.wc >= 2 && isRemoved(w[1])) continue;
            if (op == 5341 || op == 4472) continue;
            if ((op == 32 || op == 28 || op == 29) && I.wc >= 3 && removedTypes.count(w[1])) continue;
            if (op == 59 && I.wc >= 3 && removedValues.count(w[2])) continue;
            if (IsRqVoidOp(op)) continue;
            if (IsRqValueOp(op)) {
                out.push_back((4u << 16) | 83u);           // OpCopyObject
                out.push_back(w[1]); out.push_back(w[2]); out.push_back(nullFor[w[1]]);
                continue;
            }
            {
                const char* k = KindsFor(op);
                if (k && k[0] == 'T' && k[1] == 'R' && I.wc >= 3 && removedValues.count(w[2])) continue;   // loads/access chains of AS
            }
            if (op == 15 /*OpEntryPoint*/) {
                // keep everything up to the name string, filter the interface list
                size_t pos = 3;
                while (pos < I.wc) {
                    const uint32_t x = w[pos++];
                    if ((x & 0xFF) == 0 || (x & 0xFF00) == 0 || (x & 0xFF0000) == 0 || (x & 0xFF000000) == 0) break;
                }
                std::vector<uint32_t> tmp(w, w + pos);
                for (size_t k2 = pos; k2 < I.wc; ++k2) if (!removedValues.count(w[k2])) tmp.push_back(w[k2]);
                tmp[0] = (static_cast<uint32_t>(tmp.size()) << 16) | 15u;
                out.insert(out.end(), tmp.begin(), tmp.end());
                continue;
            }
            out.insert(out.end(), w, w + I.wc);
        }
        out[3] = bound;

        // ---- verify nothing in the output still refers to removed ids
        for (size_t i = 5; i < out.size();) {
            const uint32_t wc = out[i] >> 16, op = out[i] & 0xFFFF;
            bool bad = false;
            ForEachId(out.data() + i, op, wc, [&](size_t idx, char role) {
                if (role != 'R' && isRemoved(out[i + idx])) bad = true;
            });
            if (bad) { err = "dangling reference to removed id in op " + std::to_string(op); return false; }
            i += wc;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Structure-aware replacement keys (vendor independent).
    //
    // spv_replace files are named after the FNV-1a hash of the module the game
    // handed to the proxy. That byte hash is only stable if the GAME produces
    // exactly the same bytes on every machine. In practice it does not: the
    // game asks the driver which features exist and bakes the answer into the
    // shader variant it compiles (fp16 on/off, RT on/off, subgroup size, ...).
    // On the reference R7 260X the proxy reports "no RT / no fp16", so the game
    // emits the fallback variants the replacements were made from. On a GTX
    // 1060/1080 the driver really reports RT/fp16, the game emits different
    // modules, the hash never matches and spv_replace is silently not applied.
    //
    // Fix: classify every module the same way on every GPU (see the
    // FollowHardwareCaps handling) AND accept a replacement when the module is
    // instruction-for-instruction the same apart from ids, layout decorations,
    // enum/type widths and the SPIR-V header. Ray tracing instructions are
    // ignored as well, so a RayQuery variant of the same shader still finds its
    // non-RT replacement (the replacement is the one that is actually wanted).
    // -----------------------------------------------------------------------
    // Ray tracing / mesh capabilities that only exist on some driver profiles.
    inline bool IsRayTracingCapability(uint32_t cap) {
        return cap == 4472 || cap == 4479 || cap == 5340 || cap == 5341 || cap == 5342;
    }

    inline bool IsRayTracingOpForSignature(uint32_t op) {
        // OpTypeAccelerationStructureKHR / OpTypeRayQueryKHR
        if (op == 5341 || op == 4472) return true;
        // OpTraceRayKHR, OpExecuteCallableKHR, OpConvertUToAccelerationStructureKHR
        if (op == 4445 || op == 4446 || op == 4447) return true;
        // RayQueryKHR instructions (4473..4479)
        if (op >= 4473 && op <= 4479) return true;
        // OpRayQueryGetRayTMinKHR / GetRayFlagsKHR / GetWorldRayOriginKHR / GetWorldRayDirectionKHR
        if (op >= 6016 && op <= 6032) return true;
        return false;
    }

    // True for instructions that describe a module's interface / declarations.
    // Their enum values (formats, builtin numbers, execution modes, decorator
    // numbers) vary between driver-capability profiles, so they are reduced to
    // "shape" only. Function bodies are kept verbatim (minus ids).
    inline bool IsDeclarationOpForSignature(uint32_t op) {
        switch (op) {
        case 10:  // OpExtension
        case 11:  // OpExtInstImport
        case 12:  // OpExtInst
        case 15:  // OpEntryPoint
        case 16:  // OpExecutionMode
        case 17:  // OpCapability
        case 19:  // OpTypeVoid
        case 20:  // OpTypeBool
        case 21:  // OpTypeInt
        case 22:  // OpTypeFloat
        case 23:  // OpTypeVector
        case 24:  // OpTypeMatrix
        case 25:  // OpTypeImage
        case 26:  // OpTypeSampler
        case 27:  // OpTypeSampledImage
        case 28:  // OpTypeArray
        case 29:  // OpTypeRuntimeArray
        case 30:  // OpTypeStruct
        case 31:  // OpTypeOpaque
        case 32:  // OpTypePointer
        case 33:  // OpTypeFunction
        case 71:  // OpDecorate
        case 72:  // OpMemberDecorate
        case 73:  // OpDecorationGroup
        case 74:  // OpGroupDecorate
        case 75:  // OpGroupMemberDecorate
        case 331: // OpDecorateId
        case 5632: // OpDecorateString
        case 5633: // OpMemberDecorateString
            return true;
        default:
            return false;
        }
    }

    // Structural signature: same key <=> same program shape on any GPU.
    inline std::string StructuralSignature(const uint32_t* w, size_t n, size_t* instructionCount = nullptr,
                                           bool* hasRayTracing = nullptr) {
        std::string sig;
        if (instructionCount) *instructionCount = 0;
        if (hasRayTracing) *hasRayTracing = false;
        if (!w || n < 5 || w[0] != 0x07230203u) return sig;

        std::unordered_map<uint32_t, uint32_t> idMap;   // original id -> stable id
        uint32_t nextId = 1;
        auto mapId = [&](uint32_t id) -> uint32_t {
            if (id == 0) return 0;
            auto it = idMap.find(id);
            if (it != idMap.end()) return it->second;
            idMap[id] = nextId;
            return nextId++;
        };

        sig.reserve(n * 3);
        char buf[48];

        // ---- main pass -------------------------------------------------------
        for (size_t i = 5; i < n;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) break;

            if (IsRayTracingOpForSignature(op)) {
                if (hasRayTracing) *hasRayTracing = true;
                if (instructionCount) ++*instructionCount;
                i += wc;
                continue;                               // ignore RT-only code entirely
            }
            if (op == 10 || op == 11) { i += wc; continue; }   // extension/import strings
            if (op == 17 && wc >= 2 && IsRayTracingCapability(w[i + 1])) {
                // OpCapability RayQueryKHR / RayTracingKHR / RayTracingNV: the module
                // declares RT support. Ignored here so an RT-enabled variant of a
                // shader still matches its RT-free replacement.
                if (hasRayTracing) *hasRayTracing = true;
                i += wc;
                continue;
            }
            sprintf_s(buf, sizeof(buf), "|%u:%u", op, wc);
            sig += buf;
            if (op == 15 && wc >= 2) {
                // OpEntryPoint: the execution model is identical on every GPU, so it
                // stays part of the key (a fragment shader must never match a compute
                // replacement). Ray-tracing stages have no equivalent, so they are
                // never matched at all.
                const uint32_t model = w[i + 1];
                if (model >= 5313 && model <= 5321) return std::string();
                sprintf_s(buf, sizeof(buf), "E%u", model);
                sig += buf;
            }

            if (op == 42 || op == 43 || op == 44 || op == 45) {
                // OpConstant/OpConstantTrue/OpConstantFalse/OpConstantComposite:
                // ids may embed 16-bit or 64-bit encodings that differ by profile.
                // Keep the type only, drop the literal payload.
                sig += "#T";
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T') { sig += std::to_string(mapId(w[i + idx])); }
                });
            } else if (IsDeclarationOpForSignature(op)) {
                // Keep ids (mapped) and drop every enum / literal number.
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T' || role == 'R' || role == 'I') {
                        sig += role;
                        sig += std::to_string(mapId(w[i + idx]));
                    } else {
                        sig += 'L';                     // literal placeholder
                    }
                });
            } else if (op == 59) {
                // OpVariable: storage class matters (interface), type/result ids do not.
                sig += "#S";
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T' || role == 'R') { sig += role; sig += std::to_string(mapId(w[i + idx])); }
                    else sig += 'L';
                });
            } else {
                // Function body / control flow: ids and width-bearing literals only.
                ForEachId(w + i, op, wc, [&](size_t idx, char role) {
                    if (role == 'T' || role == 'R' || role == 'I') {
                        sig += std::to_string(mapId(w[i + idx]));
                        sig += ',';
                    }
                });
            }

            if (instructionCount) ++*instructionCount;
            i += wc;
        }

        return sig;
    }
}

    // -----------------------------------------------------------------------
    // Universal replacement lookup.
    //
    // Why the byte hash and even the order-sensitive structural signature are not
    // enough: every machine can receive a different build of the same shader. The
    // log from a GTX 1080 Ti shows the 3 replacement files never matching while the
    // game supplies 10 different modules for the same material set. A mismatch
    // cannot be recovered from the bytes alone, so the lookup degrades step by step,
    // each step being strictly typed so a replacement for another shader is never
    // used:
    //
    //   exact     - FNV-1a of the bytes the game supplied (reference machine)
    //   names/plan- identical entry point + debug names + descriptor interface
    //   identical - same instruction multiset and same interface
    //   similar   - same shader with a small build difference
    //   loose     - same shader with heavy divergence (different capability profile,
    //               ray-tracing code lowered on one side only)
    //
    // The decisive signal is the instruction multiset (order independent!) and the
    // debug names, which come from the shader source and are therefore identical on
    // every GPU. The execution model is always part of the comparison, so a vertex
    // shader can never be replaced by a fragment or compute module.
    // -----------------------------------------------------------------------

    // Everything that identifies "the same shader" without depending on the exact
    // bytes a particular driver-capability profile produced.
    struct ReplacementFingerprint {
        uint32_t stage = 0xFFFFFFFFu;
        std::string norm;
        std::string names;
        std::string layout;
        std::map<uint64_t, uint32_t> shapeHistogram;   // instruction shape -> count
        size_t words = 0;
        size_t interfaceCount = 0;
    };

    // Cosine similarity of two integer histograms, in [0,1]. Independent of ids, of
    // the instruction order and of the module length.
    inline double HistogramSimilarity(const std::map<uint64_t, uint32_t>& a,
                                     const std::map<uint64_t, uint32_t>& b) {
        if (a.empty() || b.empty()) return 0.0;
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (const auto& kv : a) {
            na += double(kv.second) * double(kv.second);
            auto it = b.find(kv.first);
            if (it != b.end()) dot += double(kv.second) * double(it->second);
        }
        for (const auto& kv : b) nb += double(kv.second) * double(kv.second);
        if (na <= 0.0 || nb <= 0.0) return 0.0;
        return dot / (std::sqrt(na) * std::sqrt(nb));
    }

    // Splits a "name1|name2|" list into a set.
    inline void SplitNames(const std::string& s, std::unordered_set<std::string>& out) {
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find('|', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.insert(s.substr(start, end - start));
            start = end + 1;
        }
    }

    // Jaccard overlap of two interface-layout key strings.
    inline double LayoutSimilarity(const std::string& a, const std::string& b) {
        if (a.empty() && b.empty()) return 1.0;
        std::unordered_set<std::string> sa, sb;
        SplitNames(a, sa);   // layout keys are also ';'-separated; split below
        sa.clear(); sb.clear();
        auto split = [](const std::string& s, char sep, std::unordered_set<std::string>& out) {
            size_t start = 0;
            while (start < s.size()) {
                size_t end = s.find(sep, start);
                if (end == std::string::npos) end = s.size();
                if (end > start) out.insert(s.substr(start, end - start));
                start = end + 1;
            }
        };
        split(a, ';', sa); split(b, ';', sb);
        if (sa.empty() && sb.empty()) return 1.0;
        size_t inter = 0;
        for (const std::string& k : sa) if (sb.count(k)) ++inter;
        const size_t uni = sa.size() + sb.size() - inter;
        return uni ? double(inter) / double(uni) : 0.0;
    }

    // Extracts a SPIR-V literal string (OpName / OpExtension / OpExtInstImport).
    inline std::string LiteralString(const uint32_t* w, uint32_t wc) {
        std::string out;
        for (uint32_t k = 0; k < wc; ++k) {
            for (unsigned b = 0; b < 4; ++b) {
                const char c = static_cast<char>((w[k] >> (b * 8)) & 0xFFu);
                if (c == 0) return out;
                out.push_back(c);
            }
        }
        return out;
    }

    inline bool ExtractFingerprint(const uint32_t* w, size_t n, ReplacementFingerprint& fp) {
        fp = ReplacementFingerprint{};
        if (!w || n < 5 || w[0] != 0x07230203u) return false;
        fp.words = n;
        fp.norm = spvrw::StructuralSignature(w, n);

        std::map<uint32_t, uint32_t> setOf, bindingOf, locationOf;
        std::vector<std::string> nameList;
        std::vector<std::pair<uint32_t, uint32_t>> variables;   // resultId, storageClass

        for (size_t i = 5; i < n;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > n) break;
            const uint32_t* b = w + i;
            if (op == 15 && wc >= 3 && fp.stage == 0xFFFFFFFFu) {        // OpEntryPoint
                fp.stage = b[1];
            } else if (op == 5 && wc >= 3) {                            // OpName
                nameList.push_back(LiteralString(b + 2, wc - 2));
            } else if (op == 6 && wc >= 4) {                            // OppMemberName
                nameList.push_back(LiteralString(b + 3, wc - 3));
            } else if (op == 71 && wc >= 4) {                           // OpDecorate
                const uint32_t target = b[1], dec = b[2];
                if (dec == 34) setOf[target] = b[3];                    // DescriptorSet
                else if (dec == 33) bindingOf[target] = b[3];           // Binding
                else if (dec == 30) locationOf[target] = b[3];          // Location
            } else if (op == 59 && wc >= 4) {                           // OpVariable
                variables.push_back({ b[2], b[3] });
            }

            // Order-independent structural shape of this instruction: the opcode
            // plus, for every operand, whether it is an id ("I") or a literal ("L").
            // Ids and literal values are dropped, so the same shader built by a
            // different frontend or for a different capability profile keeps the same
            // set of shapes. Ray-tracing code is excluded because our own lowering may
            // remove it on one machine and not on another.
            const bool rtOp = spvrw::IsRayTracingOpForSignature(op);
            const bool rtCap = (op == 17 && wc >= 2 && spvrw::IsRayTracingCapability(w[i + 1]));
            if (!rtOp && !rtCap) {
                std::vector<char> roles(wc, 'L');
                spvrw::ForEachId(w + i, op, wc, [&](size_t idx, char) { if (idx < roles.size()) roles[idx] = 'I'; });
                uint64_t h = 0xcbf29ce484222325ull;
                auto mixByte = [&](uint8_t byte) { h ^= byte; h *= 0x100000001b3ull; };
                mixByte(static_cast<uint8_t>(op & 0xFF));
                mixByte(static_cast<uint8_t>(op >> 8));
                for (uint32_t k = 1; k < wc; ++k) mixByte(static_cast<uint8_t>(roles[k]));
                ++fp.shapeHistogram[h];
            }
            i += wc;
        }

        std::sort(nameList.begin(), nameList.end());
        nameList.erase(std::unique(nameList.begin(), nameList.end()), nameList.end());
        for (const std::string& nm : nameList) { fp.names += nm; fp.names += '|'; }

        std::vector<std::string> keys;
        keys.reserve(variables.size());
        for (const auto& v : variables) {
            const uint32_t resultId = v.first, storageClass = v.second;
            if (storageClass == 6 || storageClass == 7) continue;   // Private / Function
            char key[96];
            auto sit = setOf.find(resultId), bit = bindingOf.find(resultId);
            auto lit = locationOf.find(resultId);
            if (sit != setOf.end() || bit != bindingOf.end()) {
                _snprintf_s(key, sizeof(key), _TRUNCATE, "D%u:%u:%u",
                    storageClass,
                    sit != setOf.end() ? sit->second : 0xFFFFFFFFu,
                    bit != bindingOf.end() ? bit->second : 0xFFFFFFFFu);
            } else {
                _snprintf_s(key, sizeof(key), _TRUNCATE, "L%u:%u",
                    storageClass,
                    lit != locationOf.end() ? lit->second : 0xFFFFFFFFu);
            }
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        fp.interfaceCount = keys.size();
        for (const std::string& k : keys) { fp.layout += k; fp.layout += ';'; }
        return !fp.norm.empty();
    }

    // Portable keys: the descriptor interface (which the pipeline layout requires
    // anyway) plus the debug names. Instruction order is never part of a key.
    struct ReplacementKeySet {
        std::string names;
        std::string plan;
        bool valid = false;
        uint32_t stage = 0xFFFFFFFFu;
    };

    inline ReplacementKeySet MakeKeySet(const spvrw::ReplacementFingerprint& fp) {
        ReplacementKeySet k;
        if (fp.norm.empty()) return k;
        k.valid = true;
        k.stage = fp.stage;
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u#", fp.stage);
        k.plan = buf;  k.plan += fp.layout;
        k.names = k.plan;  k.names += '#'; k.names += fp.names;
        return k;
    }

    struct ReplacementEntry {
        std::wstring path;
        uint32_t stage = 0xFFFFFFFFu;
        spvrw::ReplacementFingerprint fp;
    };

    struct ReplacementIndex {
        std::unordered_map<std::string, std::wstring> names;
        std::unordered_map<std::string, std::wstring> plan;
        std::unordered_set<std::string> ambNames, ambPlan;
        std::unordered_map<uint64_t, std::wstring> learned;   // hash -> file
        std::vector<ReplacementEntry> entries;
        size_t files = 0, unreadable = 0;
        bool ready = false;
    };

    // kFuzzyMargin keeps two equally plausible candidates from being swapped.
    constexpr double kFuzzyMargin = 0.05;

    ReplacementIndex* g_replIndex = nullptr;
    SRWLOCK g_replIndexLock = SRWLOCK_INIT;
    volatile LONG g_replIndexState = 0;
    volatile LONG g_replHitsExact = 0, g_replHitsSimilar = 0, g_replMisses = 0;

    void AddToIndex(std::unordered_map<std::string, std::wstring>& m,
                    std::unordered_set<std::string>& ambiguous,
                    const std::string& key, const std::wstring& path) {
        if (key.empty()) return;
        auto it = m.find(key);
        if (it == m.end()) { m.emplace(key, path); return; }
        if (it->second != path) ambiguous.insert(key);   // never guess between two shaders
    }

    // Chooses the replacement for one incoming module by similarity. The tiers go
    // from strict to loose:
    //
    //   identical - same instruction multiset and descriptor interface
    //   similar   - same shader after a small build difference
    //   loose     - same shader with heavy divergence (different capability profile,
    //               ray-tracing code lowered on one side only)
    //
    // Debug names are the decisive signal: they come from the shader source, so they
    // are identical for the same shader on every GPU and different for a different
    // shader even when the descriptor interface coincides. When one of the modules
    // carries no names at all (stripped build) the name gate is skipped instead of
    // failing the match. The best candidate must also beat the runner-up by
    // kFuzzyMargin, otherwise nothing is picked.
    inline const std::wstring* BestSimilarityMatch(const std::vector<ReplacementEntry>& entries,
                                                  const ReplacementFingerprint& fp,
                                                  const char** matchedLevel) {
        if (matchedLevel) *matchedLevel = nullptr;
        struct Tier { double shape, layout, names; const char* name; };
        const Tier tiers[] = {
            { 0.980, 1.00, 1.00, "identical" },
            { 0.900, 0.75, 0.50, "similar" },
            { 0.750, 0.50, 0.34, "loose" },
        };
        for (const Tier& tier : tiers) {
            double best = -1.0, second = -1.0;
            const std::wstring* bestPath = nullptr;
            for (const ReplacementEntry& e : entries) {
                if (e.stage != fp.stage) continue;
                const double shape = HistogramSimilarity(fp.shapeHistogram, e.fp.shapeHistogram);
                if (shape < tier.shape) continue;
                const double lay = LayoutSimilarity(fp.layout, e.fp.layout);
                if (lay < tier.layout) continue;

                std::unordered_set<std::string> mine, theirs;
                SplitNames(fp.names, mine);
                SplitNames(e.fp.names, theirs);
                double namesScore = 0.0;
                if (!mine.empty() && !theirs.empty()) {
                    size_t inter = 0;
                    for (const std::string& nm : mine) if (theirs.count(nm)) ++inter;
                    const size_t uni = mine.size() + theirs.size() - inter;
                    namesScore = uni ? double(inter) / double(uni) : 0.0;
                    if (namesScore < tier.names) continue;
                }

                const double score = shape + lay + namesScore;
                if (score > best) { second = best; best = score; bestPath = &e.path; }
                else if (score > second) { second = score; }
            }
            if (bestPath && (second < 0.0 || best - second >= kFuzzyMargin)) {
                if (matchedLevel) *matchedLevel = tier.name;
                return bestPath;
            }
        }
        return nullptr;
    }

    void BuildReplacementIndexLocked() {
        auto* idx = new ReplacementIndex();
        if (g_replMap) {
            idx->files = g_replMap->size();
            idx->entries.reserve(idx->files);
            for (const auto& kv : *g_replMap) {
                std::vector<uint32_t> words;
                if (!ReadReplacement(kv.second, words)) { ++idx->unreadable; continue; }
                std::string err;
                if (!ValidateReplacementEnvelope(words, err)) { ++idx->unreadable; continue; }
                spvrw::ReplacementFingerprint fp;
                if (!spvrw::ExtractFingerprint(words.data(), words.size(), fp)) { ++idx->unreadable; continue; }
                const ReplacementKeySet keys = MakeKeySet(fp);
                if (!keys.valid) { ++idx->unreadable; continue; }
                // The pipeline layout has to match for the replacement to be usable at
                // all, so an interface shared by two different replacement files is a
                // genuine conflict: drop it rather than guess.
                AddToIndex(idx->plan,  idx->ambPlan,  keys.plan,  kv.second);
                AddToIndex(idx->names, idx->ambNames, keys.names, kv.second);
                ReplacementEntry entry;
                entry.path = kv.second;
                entry.stage = fp.stage;
                entry.fp = std::move(fp);
                idx->entries.push_back(std::move(entry));
            }
        }
        for (const std::string& k : idx->ambPlan)  idx->plan.erase(k);
        for (const std::string& k : idx->ambNames) idx->names.erase(k);
        idx->ready = true;

        g_replIndex = idx;
        LogLine("SPIRV: replacement index: %zu file(s), %zu unreadable | exact keys names=%zu plan=%zu (ambiguous ignored: %zu/%zu) | similarity candidates=%zu",
            idx->files, idx->unreadable,
            idx->names.size(), idx->plan.size(),
            idx->ambNames.size(), idx->ambPlan.size(),
            idx->entries.size());
    }

    ReplacementIndex* EnsureReplacementIndex() {
        if (InterlockedCompareExchange(&g_replIndexState, 0, 0) == 2) return g_replIndex;
        AcquireSRWLockExclusive(&g_replIndexLock);
        if (g_replIndexState != 2) { BuildReplacementIndexLocked(); g_replIndexState = 2; }
        ReleaseSRWLockExclusive(&g_replIndexLock);
        return g_replIndex;
    }

    // Finds the replacement for one incoming module. Returns false when nothing
    // plausible is in spv_replace for it (the original module is then kept).
    bool FindReplacement(const uint32_t* code, size_t nWords, uint64_t hash,
                         std::wstring& outPath, const char** how) {
        if (how) *how = "none";
        if (hash == 0 || !g_spvCfg.structureMatchFallback) return false;
        ReplacementIndex* idx = EnsureReplacementIndex();
        if (!idx || !idx->ready || idx->files == 0) return false;

        // Learned mapping from an earlier identical lookup (cheap and exact).
        {
            AcquireSRWLockShared(&g_replIndexLock);
            auto it = idx->learned.find(hash);
            if (it != idx->learned.end()) {
                outPath = it->second;
                ReleaseSRWLockShared(&g_replIndexLock);
                if (how) *how = "learned";
                return true;
            }
            ReleaseSRWLockShared(&g_replIndexLock);
        }

        spvrw::ReplacementFingerprint fp;
        if (!spvrw::ExtractFingerprint(code, nWords, fp)) return false;
        const ReplacementKeySet keys = MakeKeySet(fp);
        if (!keys.valid) return false;

        const char* level = nullptr;
        std::wstring found;
        AcquireSRWLockShared(&g_replIndexLock);
        auto probe = [&](const std::unordered_map<std::string, std::wstring>& m,
                         const std::string& key, const char* name) {
            if (level || key.empty()) return;
            auto it = m.find(key);
            if (it != m.end()) { found = it->second; level = name; }
        };
        probe(idx->names, keys.names, "names");
        probe(idx->plan,  keys.plan,  "plan");

        if (!level && g_spvCfg.fuzzyMatch) {
            const std::wstring* pick = BestSimilarityMatch(idx->entries, fp, &level);
            if (pick) found = *pick;
        }
        ReleaseSRWLockShared(&g_replIndexLock);
        if (!level) return false;

        outPath = found;
        if (how) *how = level;
        AcquireSRWLockExclusive(&g_replIndexLock);
        if (g_replIndex) g_replIndex->learned[hash] = found;
        ReleaseSRWLockExclusive(&g_replIndexLock);
        return true;
    }

    void ForgetReplacementIndex() {
        InterlockedExchange(&g_replIndexState, 0);
        ReplacementIndex* stale = nullptr;
        AcquireSRWLockExclusive(&g_replIndexLock);
        stale = g_replIndex;
        g_replIndex = nullptr;
        ReleaseSRWLockExclusive(&g_replIndexLock);
        delete stale;
    }

    // ---- SPIR-V module info ---------------------------------------------------
    struct SpvInfo {
        uint32_t version = 0;
        uint32_t model = 0xFFFFFFFFu;
        std::string caps;
        bool deny = false;
        std::string reason;
        bool rqOnly = false;      // uses RayQuery but no other ray-tracing feature (can be rewritten)
        bool denyOther = false;   // denied for a reason other than ray tracing
        bool rewritten = false;
        std::shared_ptr<std::vector<uint32_t>> code;   // final SPIR-V sent to the driver (for post-mortem dumps)
        uint64_t originalHash = 0;       // FNV-1a hash of the game-provided module
        uint64_t effectiveHash = 0;      // hash of the exact module handed to the ICD
        bool replacement = false;       // loaded from spv_replace
        uint32_t localSizeX = 1, localSizeY = 1, localSizeZ = 1;
    };
    SRWLOCK g_spvLock = SRWLOCK_INIT;
    std::map<uint64_t, SpvInfo>* g_spvMap = nullptr;

    const char* CapName(uint32_t c) {
        switch (c) {
        case 1: return "Shader"; case 9: return "Float16"; case 10: return "Float64"; case 11: return "Int64";
        case 22: return "Int16"; case 39: return "Int8"; case 61: return "GroupNonUniform";
        case 62: return "GroupNonUniformVote"; case 63: return "GroupNonUniformArithmetic";
        case 64: return "GroupNonUniformBallot"; case 65: return "GroupNonUniformShuffle";
        case 66: return "GroupNonUniformShuffleRelative"; case 67: return "GroupNonUniformClustered";
        case 68: return "GroupNonUniformQuad"; case 4422: return "FragmentShadingRate";
        case 4433: return "StorageBuffer16BitAccess"; case 4434: return "UniformAndStorageBuffer16BitAccess";
        case 4435: return "StoragePushConstant16"; case 4436: return "StorageInputOutput16";
        case 4472: return "RayQueryKHR"; case 4479: return "RayTracingKHR";
        case 5283: return "MeshShadingEXT"; case 5301: return "ShaderNonUniform";
        case 5302: return "RuntimeDescriptorArray"; case 5345: return "VulkanMemoryModel";
        case 5347: return "PhysicalStorageBufferAddresses"; case 5340: return "RayTracingNV";
        case 6016: return "DotProductInputAll"; case 6017: return "DotProductInput4x8Bit";
        case 6018: return "DotProductInput4x8BitPacked"; case 6019: return "DotProduct";
        default: return nullptr;
        }
    }

    // Hardware capability view used to classify a module. It is filled from the
    // driver's real feature bits so that each GPU is judged by what it can
    // actually run instead of by a fixed "GCN2 has nothing" list.
    struct ShaderCaps {
        bool probed = false;      // driver bits were queried successfully
        bool float16 = false;
        bool int16 = false;
        bool dotProduct = false;
        bool mesh = false;
        bool rayQuery = false;
        bool rayPipeline = false;
    };

    // Capability view for the device that owns the shader module. Uses the record
    // captured in vkCreateDevice, so it is correct even with several GPUs present.
    ShaderCaps GetShaderCaps(const ActiveRtDevice& a) {
        ShaderCaps s;
        s.probed = a.hwProbed;
        s.float16 = a.hwFloat16;
        s.int16 = a.hwInt16;
        s.dotProduct = a.hwDotProduct;
        s.mesh = a.hwMesh;
        s.rayQuery = a.nativeRQ;
        s.rayPipeline = a.nativeRTP;
        return s;
    }

    SpvInfo AnalyzeSpirv(const uint32_t* w, size_t nWords, bool nativeRTP, bool nativeRQ, const ShaderCaps& hw) {
        SpvInfo info;
        if (!w || nWords < 5 || w[0] != 0x07230203u) { info.reason = "not SPIR-V"; return info; }
        info.version = w[1];
        bool f16 = false, i16 = false, dot = false, rt = false, mesh = false, rqCap = false, rtHard = false;
        std::vector<uint32_t> capIds;
        size_t i = 5;
        char buf[48];
        while (i < nWords) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > nWords) break;
            if (op == 54) break;                                   // OpFunction: end of declarations
            if (op == 17 && wc >= 2) {                             // OpCapability
                const uint32_t c = w[i + 1];
                capIds.push_back(c);
                const char* n = CapName(c);
                if (n) info.caps += n; else { sprintf_s(buf, sizeof(buf), "cap%u", c); info.caps += buf; }
                info.caps += ' ';
                if (c == 9) f16 = true;
                if (c == 22) i16 = true;
                if (c >= 6016 && c <= 6019) dot = true;
                if (c == 4472 || c == 4479 || c == 5340 || c == 5341 || c == 5342) rt = true;
                if (c == 4472) rqCap = true;
                if (c == 4479 || c == 5340 || c == 5341 || c == 5342) rtHard = true;
                if (c == 5283 || c == 5266) mesh = true;
            } else if (op == 16 && wc >= 6 && w[i + 2] == 17u) {          // OpExecutionMode LocalSize
                info.localSizeX = w[i + 3];
                info.localSizeY = w[i + 4];
                info.localSizeZ = w[i + 5];
            } else if (op == 15 && wc >= 2 && info.model == 0xFFFFFFFFu) {   // OpEntryPoint
                info.model = w[i + 1];
                if (info.model >= 5267 && info.model <= 5268) mesh = true;  // Task/Mesh NV
                if (info.model >= 5313 && info.model <= 5321) { rt = true; rtHard = true; }     // RT stages
                if (info.model == 5364 || info.model == 5365) mesh = true;   // Task/Mesh EXT
            }
            i += wc;
        }
        // ---- capability policy -------------------------------------------------
        // FollowHardwareCaps (default): a module is denied only when the driver
        // genuinely lacks the feature it needs, so the game's shader set (and the
        // spv_replace hash list built from it) is identical on GCN2, Pascal/Turing
        // and Intel instead of depending on the INI's GCN2 defaults.
        // FollowHardwareCaps=0 restores the legacy INI-driven behaviour.
        const bool follow = g_spvCfg.followHardwareCaps != 0 && hw.probed;
        // Deny = "the INI asks for it" AND "the driver really cannot run it".
        //
        // Why an AND (and not "just ask the driver"): the INI flags are the user's
        // explicit choice for THIS machine. Using the AND means the set of blocked
        // modules can only ever SHRINK compared to older builds, so the already
        // working reference card keeps exactly its previous behaviour, while a card
        // that reports a missing feature as supported stops receiving fake modules.
        // With the AND, the deny decision for every capability the INI blocks becomes
        // identical on GCN2 / Pascal / Turing / Intel ("this feature is unavailable
        // here"), which is what makes one spv_replace folder valid on all of them.
        // If the driver could not be probed, the INI value is used as-is (legacy).
        const bool denyF16      = (g_spvCfg.denyFloat16 != 0)    && (!follow || !hw.float16);
        const bool denyI16      = (g_spvCfg.denyInt16 != 0)      && (!follow || !hw.int16);
        const bool denyDot      = (g_spvCfg.denyDotProduct != 0) && (!follow || !hw.dotProduct);
        const bool denyMeshHere = (g_spvCfg.denyMesh != 0)       && (!follow || !hw.mesh);
        if (denyF16 && f16)    { info.deny = true; info.denyOther = true; info.reason += "Float16 "; }
        if (denyI16 && i16)    { info.deny = true; info.denyOther = true; info.reason += "Int16 "; }
        if (denyDot && dot)    { info.deny = true; info.denyOther = true; info.reason += "DotProduct "; }
        // Do not blanket-deny RT on capable GPUs. In Hybrid, native pipeline
        // stages stay native while only unsupported RayQuery is rewritten.
        if (rt) {
            // Hardware capability policy is authoritative. The legacy
            // DenyRayTracing=0 switch must not pass unsupported RT SPIR-V to an ICD.
            const bool unsupportedRQ = rqCap && !nativeRQ;
            const bool unsupportedPipeline = rtHard && !nativeRTP;
            if (unsupportedRQ || unsupportedPipeline) {
                info.deny = true;
                if (unsupportedRQ) info.reason += "RayQuery ";
                if (unsupportedPipeline) info.reason += "RayTracing ";
                if (unsupportedPipeline || (unsupportedRQ && rtHard) || GetRtCompatMode() == RT_MODE_NATIVE)
                    info.denyOther = true;
            }
        }
        if (denyMeshHere && mesh)           { info.deny = true; info.denyOther = true; info.reason += "Mesh "; }
        info.rqOnly = rqCap && !rtHard && !nativeRQ && GetRtCompatMode() != RT_MODE_NATIVE;
        for (uint32_t c : capIds) {
            for (int k = 0; k < g_spvCfg.nDenyAnyCaps; ++k)
                if (g_spvCfg.denyAnyCaps[k] == c) {
                    // Cap46 SampledBuffer: prefer rewrite path when RayQuery is present; otherwise soft-deny.
                    if (c == 46 && rqCap && !rtHard) {
                        info.reason += "AnyCap46(soft) ";
                    } else {
                        info.deny = true; info.denyOther = true;
                        info.reason += "AnyCap" + std::to_string(c) + " ";
                    }
                }
            if (info.model == 5)
                for (int k = 0; k < g_spvCfg.nDenyComputeCaps; ++k)
                    if (g_spvCfg.denyComputeCaps[k] == c) { info.deny = true; info.denyOther = true; info.reason += "ComputeCap" + std::to_string(c) + " "; }
        }
        if (info.model == 5 && g_spvCfg.nDenyComputeAll > 0) {
            bool all = true;
            for (int k = 0; k < g_spvCfg.nDenyComputeAll && all; ++k) {
                bool found = false;
                for (uint32_t c : capIds) if (c == g_spvCfg.denyComputeAll[k]) { found = true; break; }
                if (!found) all = false;
            }
            if (all) { info.deny = true; info.denyOther = true; info.reason += "ComputeCombo "; }
        }
        return info;
    }

    bool GetSpvInfo(VkShaderModule m, SpvInfo& out) {
        bool ok = false;
        AcquireSRWLockShared(&g_spvLock);
        if (g_spvMap) {
            auto it = g_spvMap->find(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m)));
            if (it != g_spvMap->end()) { out = it->second; ok = true; }
        }
        ReleaseSRWLockShared(&g_spvLock);
        return ok;
    }
    // SPVTRACE v3: active compute pipeline census. Captures first-use SPIR-V hashes and bounded resource samples.
    // SPVTRACE patch v1: bounded, opt-in telemetry for SPIR-V replacements only.
    // It does not alter descriptors, pipelines, dispatch sizes, or shader instructions.
    struct TraceDescriptorRecord {
        uint32_t binding = 0;
        uint32_t element = 0;
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_SAMPLER;
        VkSampler sampler = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = 0;
        VkBufferView texelView = VK_NULL_HANDLE;
    };
    struct TracePipelineRecord {
        VkShaderModule module = VK_NULL_HANDLE;
        std::shared_ptr<std::vector<uint32_t>> code; // survives vkDestroyShaderModule until first active dispatch
        VkPipelineLayout layout = VK_NULL_HANDLE;
        uint64_t originalHash = 0;
        uint64_t effectiveHash = 0;
        uint64_t dispatches = 0;
        uint64_t lastResourceSignature = 0;
        uint32_t resourceSnapshots = 0;
        uint32_t localX = 1, localY = 1, localZ = 1;
        bool replacement = false;
        bool activeLogged = false;
        bool resourceCandidateLogged = false;
        uint64_t binds = 0;
        std::string caps;
    };
    void TraceDumpSelectedActiveCompute(const TracePipelineRecord& rec);
    using TraceDescriptorKey = std::pair<uint32_t, uint32_t>;
    using TraceDescriptorSet = std::map<TraceDescriptorKey, TraceDescriptorRecord>;
    struct TraceDescriptorTemplateRecord {
        bool descriptorSetTemplate = false;
        std::vector<VkDescriptorUpdateTemplateEntry> entries;
    };
    SRWLOCK g_traceLock = SRWLOCK_INIT;
    std::map<VkPipeline, TracePipelineRecord>* g_tracePipelines = nullptr;
    std::map<VkDescriptorSet, TraceDescriptorSet>* g_traceSetDescriptors = nullptr;
    std::map<VkDescriptorUpdateTemplate, TraceDescriptorTemplateRecord>* g_traceDescriptorTemplates = nullptr;
    std::map<VkCommandBuffer, std::map<uint32_t, VkDescriptorSet>>* g_traceCommandSets = nullptr;

    void TraceRegisterReplacementPipeline(VkPipeline pipeline, VkShaderModule module, VkPipelineLayout layout) {
        if (!g_spvCfg.traceReplacementShaders || !pipeline || IsFake(pipeline)) return;
        SpvInfo si;
        if (!GetSpvInfo(module, si) || si.model != 5) return;
        TracePipelineRecord rec{};
        rec.module = module;
        rec.code = si.code;
        rec.layout = layout;
        rec.replacement = si.replacement;
        rec.caps = si.caps;
        rec.originalHash = si.originalHash;
        rec.effectiveHash = si.effectiveHash;
        rec.localX = si.localSizeX ? si.localSizeX : 1;
        rec.localY = si.localSizeY ? si.localSizeY : 1;
        rec.localZ = si.localSizeZ ? si.localSizeZ : 1;
        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_tracePipelines) g_tracePipelines = new std::map<VkPipeline, TracePipelineRecord>();
        if (!rec.replacement && g_tracePipelines->size() >= 16384u) {
            static volatile LONG mapLimitNotice = 0;
            if (InterlockedIncrement(&mapLimitNotice) == 1)
                LogLine("SPVTRACE compute pipeline map reached 16384 entries; additional non-replacement pipelines will not be tracked");
            ReleaseSRWLockExclusive(&g_traceLock);
            return;
        }
        (*g_tracePipelines)[pipeline] = rec;
        ReleaseSRWLockExclusive(&g_traceLock);
        if (rec.replacement)
            LogLine("SPVTRACE PIPELINE hash=%016llX effective=%016llX module=%p pipeline=%p layout=%p local=%ux%ux%u caps=[%s]",
            (unsigned long long)rec.originalHash, (unsigned long long)rec.effectiveHash,
            (void*)module, (void*)pipeline, (void*)layout, rec.localX, rec.localY, rec.localZ, si.caps.c_str());
    }

    void TraceForgetReplacementPipeline(VkPipeline pipeline) {
        if (!pipeline) return;
        TracePipelineRecord rec{};
        bool found = false;
        AcquireSRWLockExclusive(&g_traceLock);
        if (g_tracePipelines) {
            auto it = g_tracePipelines->find(pipeline);
            if (it != g_tracePipelines->end()) {
                rec = it->second;
                g_tracePipelines->erase(it);
                found = true;
            }
        }
        ReleaseSRWLockExclusive(&g_traceLock);
        if (found && rec.replacement && rec.dispatches == 0)
            LogLine("SPVTRACE REPLACEMENT_DESTROYED_UNUSED hash=%016llX effective=%016llX pipeline=%p binds=%llu (created but no dispatch command was recorded)",
                (unsigned long long)rec.originalHash, (unsigned long long)rec.effectiveHash,
                (void*)pipeline, (unsigned long long)rec.binds);
    }

    void TraceLogReplacementPipelineBind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipeline) {
        if (!g_spvCfg.traceReplacementShaders || !cb || bp != VK_PIPELINE_BIND_POINT_COMPUTE || !pipeline || IsFake(pipeline))
            return;
        TracePipelineRecord rec{};
        bool found = false;
        AcquireSRWLockExclusive(&g_traceLock);
        if (g_tracePipelines) {
            auto it = g_tracePipelines->find(pipeline);
            if (it != g_tracePipelines->end()) {
                ++it->second.binds;
                rec = it->second;
                found = true;
            }
        }
        ReleaseSRWLockExclusive(&g_traceLock);
        if (!found) return;

        if (rec.replacement) {
            static volatile LONG replacementBindCount = 0;
            const LONG n = InterlockedIncrement(&replacementBindCount);
            if (n <= 32 || (n % 512) == 0)
                LogLine("SPVTRACE BIND_PIPELINE #%ld hash=%016llX effective=%016llX pipeline=%p module=%p cb=%p layout=%p bindCount=%llu",
                    (long)n, (unsigned long long)rec.originalHash, (unsigned long long)rec.effectiveHash,
                    (void*)pipeline, (void*)rec.module, (void*)cb, (void*)rec.layout,
                    (unsigned long long)rec.binds);
        } else if (rec.binds == 1) {
            static volatile LONG genericBindCount = 0;
            const LONG n = InterlockedIncrement(&genericBindCount);
            if (n <= 64)
                LogLine("SPVTRACE COMPUTE_BIND_FIRST #%ld hash=%016llX effective=%016llX pipeline=%p module=%p cb=%p layout=%p local=%ux%ux%u caps=[%s]",
                    (long)n, (unsigned long long)rec.originalHash, (unsigned long long)rec.effectiveHash,
                    (void*)pipeline, (void*)rec.module, (void*)cb, (void*)rec.layout,
                    rec.localX, rec.localY, rec.localZ, rec.caps.c_str());
        }
    }
    void TraceLogBoundDescriptorSetBind(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
        uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* sets) {
        if (!g_spvCfg.traceReplacementShaders || !cb || bp != VK_PIPELINE_BIND_POINT_COMPUTE) return;
        VkPipeline pipeline = VK_NULL_HANDLE;
        AcquireSRWLockShared(&dynrend::g_lock);
        if (dynrend::g_boundPipes) {
            auto it = dynrend::g_boundPipes->find(cb);
            if (it != dynrend::g_boundPipes->end()) pipeline = it->second;
        }
        ReleaseSRWLockShared(&dynrend::g_lock);
        if (!pipeline || IsFake(pipeline)) return;
        TracePipelineRecord rec{};
        bool found = false;
        AcquireSRWLockShared(&g_traceLock);
        if (g_tracePipelines) {
            auto it = g_tracePipelines->find(pipeline);
            if (it != g_tracePipelines->end()) { rec = it->second; found = true; }
        }
        ReleaseSRWLockShared(&g_traceLock);
        if (!found || !rec.replacement) return;

        static volatile LONG bindSetCount = 0;
        const LONG n = InterlockedIncrement(&bindSetCount);
        if (n > 32 && (n % 512) != 0) return;
        LogLine("SPVTRACE BIND_SETS #%ld hash=%016llX pipeline=%p cb=%p layout=%p firstSet=%u count=%u",
            (long)n, (unsigned long long)rec.originalHash, (void*)pipeline, (void*)cb,
            (void*)layout, firstSet, setCount);
        for (uint32_t i = 0; sets && i < setCount && i < 8; ++i)
            LogLine("SPVTRACE BIND_SET hash=%016llX setIndex=%u set=%p",
                (unsigned long long)rec.originalHash, firstSet + i, (void*)sets[i]);
    }
    void TraceDescriptorUpdates(uint32_t writeCount, const VkWriteDescriptorSet* writes,
        uint32_t copyCount, const VkCopyDescriptorSet* copies) {
        if (!g_spvCfg.traceReplacementShaders) return;
        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_traceSetDescriptors) g_traceSetDescriptors = new std::map<VkDescriptorSet, TraceDescriptorSet>();
        auto getSet = [&](VkDescriptorSet set) -> TraceDescriptorSet* {
            if (!set) return nullptr;
            auto it = g_traceSetDescriptors->find(set);
            if (it == g_traceSetDescriptors->end()) {
                if (g_traceSetDescriptors->size() >= 4096) g_traceSetDescriptors->erase(g_traceSetDescriptors->begin());
                it = g_traceSetDescriptors->emplace(set, TraceDescriptorSet{}).first;
            }
            return &it->second;
        };
        for (uint32_t wi = 0; writes && wi < writeCount; ++wi) {
            const VkWriteDescriptorSet& w = writes[wi];
            if (!w.dstSet || !w.descriptorCount) continue;
            TraceDescriptorSet* dst = getSet(w.dstSet);
            if (!dst) continue;
            const uint32_t count = (w.descriptorCount > 512u) ? 512u : w.descriptorCount;
            for (uint32_t n = 0; n < count; ++n) {
                TraceDescriptorRecord d{};
                d.binding = w.dstBinding;
                d.element = w.dstArrayElement + n;
                d.type = w.descriptorType;
                switch (w.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    if (w.pImageInfo) {
                        d.sampler = w.pImageInfo[n].sampler;
                        d.imageView = w.pImageInfo[n].imageView;
                        d.imageLayout = w.pImageInfo[n].imageLayout;
                    }
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    if (w.pBufferInfo) {
                        d.buffer = w.pBufferInfo[n].buffer;
                        d.offset = w.pBufferInfo[n].offset;
                        d.range = w.pBufferInfo[n].range;
                    }
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    if (w.pTexelBufferView) d.texelView = w.pTexelBufferView[n];
                    break;
                default:
                    break;
                }
                (*dst)[TraceDescriptorKey(d.binding, d.element)] = d;
                if (dst->size() > 1024) dst->erase(dst->begin());
            }
        }
        // Apply the copy records after writes, mirroring descriptor update ordering.
        for (uint32_t ci = 0; copies && ci < copyCount; ++ci) {
            const VkCopyDescriptorSet& c = copies[ci];
            if (!c.srcSet || !c.dstSet || !c.descriptorCount) continue;
            auto srcIt = g_traceSetDescriptors->find(c.srcSet);
            if (srcIt == g_traceSetDescriptors->end()) continue;
            std::vector<std::pair<uint32_t, TraceDescriptorRecord>> temp;
            const uint32_t count = (c.descriptorCount > 512u) ? 512u : c.descriptorCount;
            for (uint32_t n = 0; n < count; ++n) {
                auto it = srcIt->second.find(TraceDescriptorKey(c.srcBinding, c.srcArrayElement + n));
                if (it != srcIt->second.end()) temp.push_back({ n, it->second });
            }
            TraceDescriptorSet* dst = getSet(c.dstSet);
            if (!dst) continue;
            for (const auto& item : temp) {
                TraceDescriptorRecord d = item.second;
                d.binding = c.dstBinding;
                d.element = c.dstArrayElement + item.first;
                (*dst)[TraceDescriptorKey(d.binding, d.element)] = d;
                if (dst->size() > 1024) dst->erase(dst->begin());
            }
        }
        ReleaseSRWLockExclusive(&g_traceLock);
    }

    void TraceRegisterDescriptorUpdateTemplate(VkDescriptorUpdateTemplate updateTemplate,
        const VkDescriptorUpdateTemplateCreateInfo* info) {
        if (!g_spvCfg.traceReplacementShaders || !updateTemplate || !info) return;
        TraceDescriptorTemplateRecord record{};
        record.descriptorSetTemplate =
            info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        if (!record.descriptorSetTemplate) {
            LogLine("SPVTRACE DESCRIPTOR_TEMPLATE ignored push-descriptor template=%p entries=%u",
                (void*)(uintptr_t)updateTemplate, info->descriptorUpdateEntryCount);
            return;
        }
        const uint32_t count = (info->descriptorUpdateEntryCount > 4096u)
            ? 4096u : info->descriptorUpdateEntryCount;
        if (info->pDescriptorUpdateEntries && count)
            record.entries.assign(info->pDescriptorUpdateEntries,
                info->pDescriptorUpdateEntries + count);
        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_traceDescriptorTemplates)
            g_traceDescriptorTemplates = new std::map<VkDescriptorUpdateTemplate, TraceDescriptorTemplateRecord>();
        (*g_traceDescriptorTemplates)[updateTemplate] = std::move(record);
        ReleaseSRWLockExclusive(&g_traceLock);
        LogLine("SPVTRACE DESCRIPTOR_TEMPLATE registered template=%p entries=%u set=%u",
            (void*)(uintptr_t)updateTemplate, count, info->set);
    }

    void TraceForgetDescriptorUpdateTemplate(VkDescriptorUpdateTemplate updateTemplate) {
        if (!updateTemplate) return;
        AcquireSRWLockExclusive(&g_traceLock);
        if (g_traceDescriptorTemplates) g_traceDescriptorTemplates->erase(updateTemplate);
        ReleaseSRWLockExclusive(&g_traceLock);
    }

    void TraceDescriptorTemplateUpdate(VkDescriptorSet dstSet,
        VkDescriptorUpdateTemplate updateTemplate, const void* data) {
        if (!g_spvCfg.traceReplacementShaders || !dstSet || !updateTemplate || !data) return;
        TraceDescriptorTemplateRecord record{};
        bool found = false;
        AcquireSRWLockShared(&g_traceLock);
        if (g_traceDescriptorTemplates) {
            auto it = g_traceDescriptorTemplates->find(updateTemplate);
            if (it != g_traceDescriptorTemplates->end()) {
                record = it->second;
                found = true;
            }
        }
        ReleaseSRWLockShared(&g_traceLock);
        if (!found || !record.descriptorSetTemplate) {
            char key[96]{};
            _snprintf_s(key, sizeof(key), _TRUNCATE, "spvtrace-template-untracked:%p",
                (void*)(uintptr_t)updateTemplate);
            if (FirstTime(key))
                LogLine("SPVTRACE DESCRIPTOR_TEMPLATE update not tracked template=%p set=%p",
                    (void*)(uintptr_t)updateTemplate, (void*)(uintptr_t)dstSet);
            return;
        }

        std::vector<TraceDescriptorRecord> pending;
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t total = 0;
        for (const VkDescriptorUpdateTemplateEntry& entry : record.entries) {
            if (!entry.descriptorCount || entry.offset > (16u * 1024u * 1024u)) continue;
            size_t objectSize = 0;
            switch (entry.descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                objectSize = sizeof(VkDescriptorImageInfo); break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                objectSize = sizeof(VkDescriptorBufferInfo); break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                objectSize = sizeof(VkBufferView); break;
            default:
                continue;
            }
            const size_t stride = entry.stride ? entry.stride : objectSize;
            if (stride < objectSize) continue;
            const uint32_t count = (entry.descriptorCount > 512u) ? 512u : entry.descriptorCount;
            for (uint32_t n = 0; n < count && total < 4096u; ++n, ++total) {
                if (n && stride > (static_cast<size_t>(-1) - entry.offset) / n) break;
                const size_t offset = entry.offset + static_cast<size_t>(n) * stride;
                if (offset > (16u * 1024u * 1024u)) break;
                const uint8_t* item = bytes + offset;
                TraceDescriptorRecord d{};
                d.binding = entry.dstBinding;
                d.element = entry.dstArrayElement + n;
                d.type = entry.descriptorType;
                // VkDescriptorImageInfo and VkDescriptorBufferInfo can both be
                // 24 bytes on Win64. Select the payload layout by descriptorType.
                switch (entry.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                    VkDescriptorImageInfo value{};
                    memcpy(&value, item, sizeof(value));
                    d.sampler = value.sampler;
                    d.imageView = value.imageView;
                    d.imageLayout = value.imageLayout;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                    VkDescriptorBufferInfo value{};
                    memcpy(&value, item, sizeof(value));
                    d.buffer = value.buffer;
                    d.offset = value.offset;
                    d.range = value.range;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    memcpy(&d.texelView, item, sizeof(d.texelView));
                    break;
                default:
                    // Unsupported descriptor payloads are deliberately not
                    // interpreted by this diagnostic mirror.
                    continue;
                }
                pending.push_back(d);
            }
            if (total >= 4096u) break;
        }

        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_traceSetDescriptors) g_traceSetDescriptors = new std::map<VkDescriptorSet, TraceDescriptorSet>();
        auto it = g_traceSetDescriptors->find(dstSet);
        if (it == g_traceSetDescriptors->end()) {
            if (g_traceSetDescriptors->size() >= 4096u) g_traceSetDescriptors->erase(g_traceSetDescriptors->begin());
            it = g_traceSetDescriptors->emplace(dstSet, TraceDescriptorSet{}).first;
        }
        TraceDescriptorSet& dst = it->second;
        for (const TraceDescriptorRecord& d : pending) {
            dst[TraceDescriptorKey(d.binding, d.element)] = d;
            if (dst.size() > 1024u) dst.erase(dst.begin());
        }
        ReleaseSRWLockExclusive(&g_traceLock);

        char key[96]{};
        _snprintf_s(key, sizeof(key), _TRUNCATE, "spvtrace-template-update:%p",
            (void*)(uintptr_t)updateTemplate);
        if (FirstTime(key))
            LogLine("SPVTRACE DESCRIPTOR_TEMPLATE first-update template=%p set=%p entries=%zu trackedDescriptors=%zu",
                (void*)(uintptr_t)updateTemplate, (void*)(uintptr_t)dstSet,
                record.entries.size(), pending.size());
    }

    void TraceBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t firstSet,
        uint32_t setCount, const VkDescriptorSet* sets) {
        if (!g_spvCfg.traceReplacementShaders || bp != VK_PIPELINE_BIND_POINT_COMPUTE || !cb || !sets) return;
        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_traceCommandSets) g_traceCommandSets = new std::map<VkCommandBuffer, std::map<uint32_t, VkDescriptorSet>>();
        auto& bound = (*g_traceCommandSets)[cb];
        for (uint32_t i = 0; i < setCount && i < 64; ++i) {
            const uint64_t slot = (uint64_t)firstSet + i;
            if (slot < 64) bound[static_cast<uint32_t>(slot)] = sets[i];
        }
        ReleaseSRWLockExclusive(&g_traceLock);
    }

    void TraceResetAllocatedDescriptorSets(uint32_t count, const VkDescriptorSet* sets) {
        if (!sets || !count) return;
        AcquireSRWLockExclusive(&g_traceLock);
        if (g_traceSetDescriptors)
            for (uint32_t i = 0; i < count; ++i) g_traceSetDescriptors->erase(sets[i]);
        ReleaseSRWLockExclusive(&g_traceLock);
    }

    void TraceForgetCommandBufferBindings(VkCommandBuffer cb) {
        if (!cb) return;
        AcquireSRWLockExclusive(&g_traceLock);
        if (g_traceCommandSets) g_traceCommandSets->erase(cb);
        ReleaseSRWLockExclusive(&g_traceLock);
    }

    void TraceForgetCommandBuffers(uint32_t count, const VkCommandBuffer* buffers) {
        if (!buffers) return;
        AcquireSRWLockExclusive(&g_traceLock);
        if (g_traceCommandSets)
            for (uint32_t i = 0; i < count; ++i) g_traceCommandSets->erase(buffers[i]);
        ReleaseSRWLockExclusive(&g_traceLock);
    }

    // SPVTRACE v3: active compute pipeline census. All compute pipelines are
    // tracked; detailed descriptor snapshots remain bounded and replacement-first.
    void TraceLogReplacementDispatch(VkCommandBuffer cb, uint32_t gx, uint32_t gy, uint32_t gz,
        bool indirect = false, VkBuffer indirectBuffer = VK_NULL_HANDLE, VkDeviceSize indirectOffset = 0,
        bool baseDispatch = false, uint32_t baseGroupX = 0, uint32_t baseGroupY = 0, uint32_t baseGroupZ = 0) {
        if (!g_spvCfg.traceReplacementShaders || !cb) return;
        VkPipeline pipeline = VK_NULL_HANDLE;
        AcquireSRWLockShared(&dynrend::g_lock);
        if (dynrend::g_boundPipes) {
            auto it = dynrend::g_boundPipes->find(cb);
            if (it != dynrend::g_boundPipes->end()) pipeline = it->second;
        }
        ReleaseSRWLockShared(&dynrend::g_lock);
        auto logOtherDispatch = [&]() {
            static volatile LONG otherDispatches = 0;
            const LONG n = InterlockedIncrement(&otherDispatches);
            if (n <= 16 || (n % 4096) == 0) {
                LogLine("SPVTRACE DISPATCH_OTHER #%ld pipeline=%p cb=%p mode=%s groups=%u,%u,%u base=%u,%u,%u buffer=%p offset=%llu fake=%u",
                    (long)n, (void*)pipeline, (void*)cb,
                    indirect ? "indirect" : (baseDispatch ? "base" : "direct"),
                    gx, gy, gz, baseGroupX, baseGroupY, baseGroupZ,
                    (void*)indirectBuffer, (unsigned long long)indirectOffset,
                    (pipeline && IsFake(pipeline)) ? 1u : 0u);
            }
        };
        if (!pipeline || IsFake(pipeline)) { logOtherDispatch(); return; }

        struct BoundSetSnapshot {
            uint32_t index = 0;
            VkDescriptorSet set = VK_NULL_HANDLE;
            std::vector<TraceDescriptorRecord> descriptors;
        };
        TracePipelineRecord rec{};
        std::vector<BoundSetSnapshot> boundSets;
        uint64_t dispatchNo = 0;
        bool collectResources = false;
        bool logActiveFirst = false;
        static volatile LONG genericActivePipelines = 0;
        static volatile LONG genericDirectResourceSnapshots = 0;
        static volatile LONG genericIndirectResourceSnapshots = 0;

        AcquireSRWLockExclusive(&g_traceLock);
        if (!g_tracePipelines) { ReleaseSRWLockExclusive(&g_traceLock); logOtherDispatch(); return; }
        auto pit = g_tracePipelines->find(pipeline);
        if (pit == g_tracePipelines->end()) { ReleaseSRWLockExclusive(&g_traceLock); logOtherDispatch(); return; }
        dispatchNo = ++pit->second.dispatches;
        if (!pit->second.replacement) {
            if (!pit->second.activeLogged) {
                pit->second.activeLogged = true;
                logActiveFirst = InterlockedIncrement(&genericActivePipelines) <= 128;
            }
            // Image-sized workloads and indirect dispatches are the best candidates
            // for lighting/composite passes. Capture their descriptor state once.
            const bool workloadCandidate = indirect ||
                ((unsigned long long)gx * (unsigned long long)gy * (unsigned long long)gz >= 256ull);
            if (!pit->second.resourceCandidateLogged && workloadCandidate) {
                pit->second.resourceCandidateLogged = true;
                // Keep separate quotas so repetitive indirect compute work cannot
                // consume all samples before full-resolution image-space passes.
                collectResources = indirect
                    ? (InterlockedIncrement(&genericIndirectResourceSnapshots) <= 12)
                    : (InterlockedIncrement(&genericDirectResourceSnapshots) <= 48);
            }
        } else {
            collectResources = (dispatchNo <= 32u || (dispatchNo % 256u) == 0);
        }
        rec = pit->second;
        if (collectResources && g_traceCommandSets) {
            auto cbIt = g_traceCommandSets->find(cb);
            if (cbIt != g_traceCommandSets->end()) {
                for (const auto& bound : cbIt->second) {
                    BoundSetSnapshot snap{};
                    snap.index = bound.first;
                    snap.set = bound.second;
                    if (g_traceSetDescriptors && bound.second) {
                        auto ds = g_traceSetDescriptors->find(bound.second);
                        if (ds != g_traceSetDescriptors->end())
                            for (const auto& entry : ds->second) snap.descriptors.push_back(entry.second);
                    }
                    boundSets.push_back(std::move(snap));
                }
            }
        }
        ReleaseSRWLockExclusive(&g_traceLock);
        TraceDumpSelectedActiveCompute(rec);

        bool resourcesChanged = false;
        size_t totalResources = 0;
        if (collectResources) {
            uint64_t signature = 0xcbf29ce484222325ull;
            auto mix = [&](uint64_t v) { signature ^= v; signature *= 0x100000001b3ull; };
            for (const auto& set : boundSets) {
                mix(set.index);
                mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(set.set)));
                for (const auto& d : set.descriptors) {
                    ++totalResources;
                    mix(d.binding); mix(d.element); mix(static_cast<uint32_t>(d.type));
                    mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.imageView)));
                    mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.sampler)));
                    mix(static_cast<uint32_t>(d.imageLayout));
                    mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.buffer)));
                    mix(static_cast<uint64_t>(d.offset)); mix(static_cast<uint64_t>(d.range));
                    mix(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(d.texelView)));
                }
            }
            AcquireSRWLockExclusive(&g_traceLock);
            if (g_tracePipelines) {
                auto it = g_tracePipelines->find(pipeline);
                if (it != g_tracePipelines->end() && it->second.lastResourceSignature != signature) {
                    it->second.lastResourceSignature = signature;
                    if (it->second.resourceSnapshots < 32u ||
                        (it->second.replacement && (dispatchNo % 256u) == 0)) {
                        ++it->second.resourceSnapshots;
                        resourcesChanged = true;
                    }
                }
            }
            ReleaseSRWLockExclusive(&g_traceLock);
        }

        const unsigned long long invocations = indirect ? 0ull :
            (unsigned long long)gx * gy * gz * rec.localX * rec.localY * rec.localZ;
        if (rec.replacement) {
            if (indirect) {
                LogLine("SPVTRACE DISPATCH_INDIRECT #%llu hash=%016llX effective=%016llX pipeline=%p module=%p cb=%p buffer=%p offset=%llu local=%ux%ux%u sets=%zu resources=%zu",
                    (unsigned long long)dispatchNo, (unsigned long long)rec.originalHash,
                    (unsigned long long)rec.effectiveHash, (void*)pipeline, (void*)rec.module,
                    (void*)cb, (void*)indirectBuffer, (unsigned long long)indirectOffset,
                    rec.localX, rec.localY, rec.localZ, boundSets.size(), totalResources);
            } else if (baseDispatch) {
                LogLine("SPVTRACE DISPATCH_BASE #%llu hash=%016llX effective=%016llX pipeline=%p module=%p cb=%p base=%u,%u,%u groups=%u,%u,%u local=%ux%ux%u invocations=%llu sets=%zu resources=%zu",
                    (unsigned long long)dispatchNo, (unsigned long long)rec.originalHash,
                    (unsigned long long)rec.effectiveHash, (void*)pipeline, (void*)rec.module,
                    (void*)cb, baseGroupX, baseGroupY, baseGroupZ, gx, gy, gz,
                    rec.localX, rec.localY, rec.localZ, invocations, boundSets.size(), totalResources);
            } else {
                LogLine("SPVTRACE DISPATCH #%llu hash=%016llX effective=%016llX pipeline=%p module=%p cb=%p groups=%u,%u,%u local=%ux%ux%u invocations=%llu sets=%zu resources=%zu",
                    (unsigned long long)dispatchNo, (unsigned long long)rec.originalHash,
                    (unsigned long long)rec.effectiveHash, (void*)pipeline, (void*)rec.module,
                    (void*)cb, gx, gy, gz, rec.localX, rec.localY, rec.localZ,
                    invocations, boundSets.size(), totalResources);
            }
        } else if (logActiveFirst || collectResources) {
            LogLine("SPVTRACE ACTIVE_COMPUTE_SAMPLE hash=%016llX effective=%016llX pipeline=%p module=%p cb=%p mode=%s dispatch=%llu groups=%u,%u,%u base=%u,%u,%u local=%ux%ux%u invocations=%llu binds=%llu sets=%zu resources=%zu caps=[%s]",
                (unsigned long long)rec.originalHash, (unsigned long long)rec.effectiveHash,
                (void*)pipeline, (void*)rec.module, (void*)cb,
                indirect ? "indirect" : (baseDispatch ? "base" : "direct"),
                (unsigned long long)dispatchNo, gx, gy, gz, baseGroupX, baseGroupY, baseGroupZ,
                rec.localX, rec.localY, rec.localZ, invocations,
                (unsigned long long)rec.binds, boundSets.size(), totalResources, rec.caps.c_str());
        }

        if (resourcesChanged) {
            size_t emitted = 0;
            for (const auto& set : boundSets) {
                if (rec.replacement)
                    LogLine("SPVTRACE SET hash=%016llX setIndex=%u set=%p descriptors=%zu",
                        (unsigned long long)rec.originalHash, set.index, (void*)set.set, set.descriptors.size());
                else
                    LogLine("SPVTRACE ACTIVE_SET hash=%016llX pipeline=%p setIndex=%u set=%p descriptors=%zu",
                        (unsigned long long)rec.originalHash, (void*)pipeline, set.index, (void*)set.set, set.descriptors.size());
                for (const auto& d : set.descriptors) {
                    if (emitted++ >= 512) {
                        LogLine("SPVTRACE RESOURCE list truncated at 512 descriptors for this binding state");
                        break;
                    }
                    dynrend::ImageViewInfo vi{};
                    const bool hasView = d.imageView && dynrend::LookupImageView(d.imageView, &vi);
                    if (rec.replacement) {
                        LogLine("SPVTRACE RESOURCE hash=%016llX set=%u binding=%u elem=%u type=%d sampler=%p view=%p image=%p format=%d aspect=0x%X imageLayout=%d buffer=%p offset=%llu range=%llu texelView=%p viewInfo=%u",
                            (unsigned long long)rec.originalHash, set.index, d.binding, d.element,
                            (int)d.type, (void*)d.sampler, (void*)d.imageView,
                            hasView ? (void*)vi.image : nullptr, hasView ? (int)vi.format : -1,
                            hasView ? (unsigned)vi.aspect : 0u, (int)d.imageLayout,
                            (void*)d.buffer, (unsigned long long)d.offset, (unsigned long long)d.range,
                            (void*)d.texelView, hasView ? 1u : 0u);
                    } else {
                        LogLine("SPVTRACE ACTIVE_RESOURCE hash=%016llX pipeline=%p set=%u binding=%u elem=%u type=%d sampler=%p view=%p image=%p format=%d aspect=0x%X imageLayout=%d buffer=%p offset=%llu range=%llu texelView=%p viewInfo=%u",
                            (unsigned long long)rec.originalHash, (void*)pipeline, set.index, d.binding, d.element,
                            (int)d.type, (void*)d.sampler, (void*)d.imageView,
                            hasView ? (void*)vi.image : nullptr, hasView ? (int)vi.format : -1,
                            hasView ? (unsigned)vi.aspect : 0u, (int)d.imageLayout,
                            (void*)d.buffer, (unsigned long long)d.offset, (unsigned long long)d.range,
                            (void*)d.texelView, hasView ? 1u : 0u);
                    }
                }
                if (emitted >= 512) break;
            }
        }
    }    // ---- learned "toxic" pipeline signatures ----------------------------------------------------------
    // The old driver needs ~5 s and then fails (-13) on some shader families (GroupNonUniform together with fp16/int16/
    // cube-array caps). Instead of faking such pipelines blindly, the proxy LEARNS: a signature is only treated as toxic
    // after 3 failures with no success; before that the driver gets a chance (so working shaders are not lost).
    struct SigStat { int failed = 0, ok = 0; };
    SRWLOCK g_sigLock = SRWLOCK_INIT;
    std::map<std::string, SigStat>& SigMap() { static auto* m = new std::map<std::string, SigStat>(); return *m; }
    // Only shader families known to be risky get a signature (others always go to the driver).
    // Only shader families known to be risky get a signature (others always go to the driver).
 std::string SigOfModule(VkShaderModule m, uint32_t stage) {
     SpvInfo si;
     if (!GetSpvInfo(m, si)) return std::string();
if (si.rewritten) return std::string();
     const bool gnu = si.caps.find("GroupNonUniform") != std::string::npos;
     const bool heavy = si.caps.find("Float16") != std::string::npos ||
                        si.caps.find("Int16") != std::string::npos ||
                        si.caps.find("cap45") != std::string::npos ||
                        si.caps.find("cap46") != std::string::npos;
     if (!gnu || !heavy) return std::string();
     return std::to_string(stage) + ":" + si.caps;
 }
    bool SigToxic(const std::string& sig) {
        if (sig.empty()) return false;
        bool t = false;
        AcquireSRWLockShared(&g_sigLock);
        auto it = SigMap().find(sig);
        if (it != SigMap().end()) t = it->second.failed >= 3 && it->second.ok == 0;
        ReleaseSRWLockShared(&g_sigLock);
        return t;
    }
    void SigRecord(const std::string& sig, bool ok) {
        if (sig.empty()) return;
        AcquireSRWLockExclusive(&g_sigLock);
        SigStat& st = SigMap()[sig];
        if (ok) ++st.ok; else ++st.failed;
        const bool nowToxic = !ok && st.failed == 3 && st.ok == 0;
        ReleaseSRWLockExclusive(&g_sigLock);
        if (nowToxic) LogLine("SIGNATURE learned as TOXIC (3 driver failures, 0 successes): %s", sig.c_str());
    }

    void PutSpvInfo(VkShaderModule m, const SpvInfo& info) {
        AcquireSRWLockExclusive(&g_spvLock);
        if (!g_spvMap) g_spvMap = new std::map<uint64_t, SpvInfo>();
        (*g_spvMap)[static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m))] = info;
        ReleaseSRWLockExclusive(&g_spvLock);
    }
    bool GetSpvInfoPublic(VkShaderModule m, std::string& caps, bool& risky) {
        SpvInfo si;
        if (!GetSpvInfo(m, si)) return false;
        caps = si.caps;
        const bool gnu = si.caps.find("GroupNonUniform") != std::string::npos;
        const bool heavy = si.caps.find("Float16") != std::string::npos || si.caps.find("Int16") != std::string::npos ||
                           si.caps.find("cap45") != std::string::npos || si.caps.find("cap46") != std::string::npos;
        risky = gnu && heavy;
        return true;
    }
    void EraseSpvInfo(VkShaderModule m) {
        AcquireSRWLockExclusive(&g_spvLock);
        if (g_spvMap) g_spvMap->erase(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(m)));
        ReleaseSRWLockExclusive(&g_spvLock);
    }

    // Short text describing a stage module, for breadcrumbs.
    void DescribeModule(VkShaderModule m, char* out, size_t cap) {
        SpvInfo si;
        if (IsFake(m)) { _snprintf_s(out, cap, _TRUNCATE, "FAKE"); return; }
        if (GetSpvInfo(m, si)) _snprintf_s(out, cap, _TRUNCATE, "%p v%x model=%u [%s]", (void*)m, si.version, si.model, si.caps.c_str());
        else _snprintf_s(out, cap, _TRUNCATE, "%p (unknown)", (void*)m);
    }

    // ---- run a driver call with crash recovery + optional serialization -----------
    // Why not only __try/__except: the game installs its own vectored exception handler that kills the
    // process BEFORE frame-based SEH is consulted. So recovery is done from OUR vectored handler:
    // the thread's register context is saved before the driver call (like setjmp) and restored by the
    // handler when the driver faults inside the call.
    SRWLOCK g_compileLock = SRWLOCK_INIT;
    // Resource binds must not queue behind multi-second shader/pipeline compilation.
    // Keep binds serialized with each other for conservative compatibility with AMDVLK.
    SRWLOCK g_resourceLock = SRWLOCK_INIT;
    // Vulkan requires external synchronization of a VkPipelineCache, not one global
    // lock for every cache. This lets independent caches compile concurrently.
    SRWLOCK g_pipelineCacheMapLock = SRWLOCK_INIT;
    std::map<uint64_t, SRWLOCK*> g_pipelineCacheLocks;
    struct ThreadGuard { CONTEXT ctx; volatile LONG active; volatile LONG tripped; };
    thread_local ThreadGuard t_guard;
    thread_local char t_job[256];
    inline void SetJob(const char* fmt, ...) noexcept {
        va_list a; va_start(a, fmt);
        vsnprintf_s(t_job, sizeof(t_job), _TRUNCATE, fmt, a);
        va_end(a);
    }
#ifdef _MSC_VER
#define VKP_NOINLINE __declspec(noinline)
#else
#define VKP_NOINLINE __attribute__((noinline))
#endif
    template<class F> VKP_NOINLINE bool GuardedRun(F& f) {
        t_guard.tripped = 0;
        t_guard.active = 0;
        RtlCaptureContext(&t_guard.ctx);            // execution resumes here after a recovered fault
        if (t_guard.tripped) { t_guard.active = 0; t_guard.tripped = 0; return false; }
        t_guard.active = 1;
        f();
        t_guard.active = 0;
        return true;
    }
#ifdef _MSC_VER
    template<class F> bool SehRun(F& f) {
        __try { return GuardedRun(f); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
#else
    template<class F> bool SehRun(F& f) { return GuardedRun(f); }
#endif
    // Global count of recovered driver crashes. After a threshold we become more aggressive
    // and force remaining modern shaders/pipelines to FAKE so the game can reach the menu.
    volatile LONG g_driverCrashCount = 0;

    // Execute a protected driver call while measuring lock wait separately from driver time.
    template<class F> bool DriverCallWithLock(F&& f, SRWLOCK* lockPtr, bool takeLock, const char* kind) {
        const ULONGLONG waitStart = GetTickCount64();
        if (lockPtr && takeLock) AcquireSRWLockExclusive(lockPtr);
        const ULONGLONG callStart = GetTickCount64();

        bool ok = true;
        if (g_spvCfg.catchDriverCrash) ok = SehRun(f); else f();

        const ULONGLONG callEnd = GetTickCount64();
        if (lockPtr && takeLock) ReleaseSRWLockExclusive(lockPtr);
        const ULONGLONG done = GetTickCount64();
        const ULONGLONG waitMs = callStart - waitStart;
        const ULONGLONG callMs = callEnd - callStart;
        const ULONGLONG totalMs = done - waitStart;

        if (!ok) {
            const LONG c = InterlockedIncrement(&g_driverCrashCount);
            LogLine("CAUGHT driver crash: kind=%s wait=%llu ms call=%llu ms total=%llu ms (recovered=%ld). job: %s",
                kind ? kind : "unknown", (unsigned long long)waitMs, (unsigned long long)callMs,
                (unsigned long long)totalMs, (long)c, t_job);
        } else if (callMs >= 1000 || waitMs >= 250) {
            LogLine("DRIVER TIMING: kind=%s wait=%llu ms call=%llu ms total=%llu ms. job: %s",
                kind ? kind : "unknown", (unsigned long long)waitMs, (unsigned long long)callMs,
                (unsigned long long)totalMs, t_job);
        }
        return ok;
    }

    // Shader-module creation uses its own lock when SerializeCompile=1.
    template<class F> bool DriverCall(F&& f) {
        return DriverCallWithLock(f, &g_compileLock,
            g_spvCfg.serializeCompile != 0, "shader-module");
    }

    SRWLOCK* GetPipelineCacheLock(VkPipelineCache cache) {
        if (cache == VK_NULL_HANDLE) return nullptr;
        const uint64_t key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cache));
        AcquireSRWLockExclusive(&g_pipelineCacheMapLock);
        SRWLOCK*& slot = g_pipelineCacheLocks[key];
        if (!slot) {
            slot = new (std::nothrow) SRWLOCK();
            if (slot) InitializeSRWLock(slot);
        }
        SRWLOCK* result = slot;
        ReleaseSRWLockExclusive(&g_pipelineCacheMapLock);
        return result;
    }

    // Serialize only users of the same non-null VkPipelineCache. A null cache has
    // no shared cache object to synchronize. Allocation failure falls back to the
    // conservative global compile lock.
    // Null VkPipelineCache does not provide a shared cache object to synchronize,
    // but this older AMDVLK driver becomes unstable under many simultaneous
    // pipeline compilations. Serialize null-cache pipeline calls when requested.
    SRWLOCK g_nullPipelineLock = SRWLOCK_INIT;
    static volatile LONG g_pipelineCallSequence = 0;

    template<class F> bool DriverCallPipeline(VkPipelineCache cache, const char* apiName,
        VkResult* resultOut, F&& f) {
        SRWLOCK* lock = nullptr;
        bool takeLock = false;
        const char* kind = "pipeline";
        if (g_spvCfg.serializeCompile != 0) {
            if (cache == VK_NULL_HANDLE) {
                lock = &g_nullPipelineLock;
                takeLock = true;
                kind = "pipeline-null-cache";
            } else {
                lock = GetPipelineCacheLock(cache);
                if (lock) {
                    takeLock = true;
                    kind = "pipeline-cache";
                } else {
                    lock = &g_compileLock;
                    takeLock = true;
                    kind = "pipeline-global-fallback";
                }
            }
        }

        const LONG callId = InterlockedIncrement(&g_pipelineCallSequence);
        const DWORD threadId = GetCurrentThreadId();
        const ULONGLONG startMs = GetTickCount64();
        LogLine("PIPELINE ENTER id=%ld api=%s thread=%lu cache=%p lock=%s job=%s",
            (long)callId, apiName ? apiName : "unknown", (unsigned long)threadId,
            (void*)cache, kind, t_job);

        // This callback starts only after DriverCallWithLock has acquired the
        // selected lock. It separates lock waiting from a call stuck in AMDVLK.
        auto tracedCall = [&]() {
            LogLine("PIPELINE DRIVER_ENTER id=%ld api=%s thread=%lu cache=%p lock=%s job=%s",
                (long)callId, apiName ? apiName : "unknown", (unsigned long)threadId,
                (void*)cache, kind, t_job);
            const ULONGLONG driverStartMs = GetTickCount64();
            f();
            const ULONGLONG driverElapsedMs = GetTickCount64() - driverStartMs;
            LogLine("PIPELINE DRIVER_EXIT id=%ld api=%s thread=%lu result=%d driverMs=%llu cache=%p lock=%s",
                (long)callId, apiName ? apiName : "unknown", (unsigned long)threadId,
                resultOut ? (int)(*resultOut) : (int)VK_ERROR_UNKNOWN,
                (unsigned long long)driverElapsedMs, (void*)cache, kind);
        };
        const bool ok = DriverCallWithLock(tracedCall, lock, takeLock, kind);
        const ULONGLONG elapsedMs = GetTickCount64() - startMs;
        LogLine("PIPELINE EXIT id=%ld api=%s thread=%lu driverOk=%u result=%d elapsed=%llu ms cache=%p lock=%s job=%s",
            (long)callId, apiName ? apiName : "unknown", (unsigned long)threadId,
            ok ? 1u : 0u, resultOut ? (int)(*resultOut) : (int)VK_ERROR_UNKNOWN,
            (unsigned long long)elapsedMs, (void*)cache, kind, t_job);
        return ok;
    }

    // Resource binds use a separate lock: serialized with one another, but they no
    // longer wait behind a multi-second shader/pipeline compilation call.
    static volatile LONG g_resourceCallSequence = 0;
    template<class F> bool DriverCallResource(F&& f) {
        const LONG callId = InterlockedIncrement(&g_resourceCallSequence);
        // Keep the last resource operations in the 64-entry in-memory crash ring;
        // do not write two disk lines for every buffer/image bind.
        Crumb("RESOURCE id=%ld %s", (long)callId, t_job);
        // Small, bounded progress trace: calls 1..8, then every 64th call.
        const bool checkpoint = (callId <= 8 || (callId % 64) == 0);
        if (checkpoint)
            LogLine("RESOURCE CHECKPOINT ENTER id=%ld job=%s", (long)callId, t_job);
        const bool ok = DriverCallWithLock(f, &g_resourceLock, true, "resource-bind");
        if (checkpoint)
            LogLine("RESOURCE CHECKPOINT EXIT id=%ld driverOk=%u job=%s",
                (long)callId, ok ? 1u : 0u, t_job);
        // DriverCallWithLock still writes every crash and every slow bind (>1 s)
        // with separate lock-wait and driver-call timings.
        return ok;
    }

    void DumpSpirv(const char* tag, long idx, const uint32_t* code, size_t bytes) {
        wchar_t path[2048]{};
        GetLogPath(path, sizeof(path) / sizeof(path[0]));
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash) return;
        wchar_t name[96];
        _snwprintf_s(name, 96, _TRUNCATE, L"spv_%S_%ld.spv", tag, idx);
        wcscpy_s(slash + 1, (sizeof(path) / sizeof(path[0])) - static_cast<size_t>(slash + 1 - path), name);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(f, code, static_cast<DWORD>(bytes), &w, nullptr);
            CloseHandle(f);
            LogLine("SPIRV: dumped %zu bytes to %ls", bytes, path);
        }
    }

    // SPVTRACE ACTIVE_COMPUTE_HASH_DUMP v1: write only user-selected shaders
    // after a real dispatch is recorded. The shared_ptr keeps code valid even if
    // the application destroys VkShaderModule immediately after pipeline creation.
    void TraceDumpSelectedActiveCompute(const TracePipelineRecord& rec) {
        if (!IsActiveComputeDumpHash(rec.originalHash)) return;
        if (!rec.code || rec.code->empty()) {
            LogLine("SPVTRACE ACTIVE_COMPUTE_DUMP_SKIPPED hash=%016llX reason=SPIR-V code was not retained",
                (unsigned long long)rec.originalHash);
            return;
        }
        static SRWLOCK dumpLock = SRWLOCK_INIT;
        static uint64_t dumpedHashes[16]{};
        static LONG dumpedCount = 0;
        bool alreadyDumped = false;
        AcquireSRWLockExclusive(&dumpLock);
        for (LONG i = 0; i < dumpedCount; ++i) {
            if (dumpedHashes[i] == rec.originalHash) { alreadyDumped = true; break; }
        }
        if (!alreadyDumped && dumpedCount < static_cast<LONG>(_countof(dumpedHashes)))
            dumpedHashes[dumpedCount++] = rec.originalHash;
        ReleaseSRWLockExclusive(&dumpLock);
        if (alreadyDumped) return;

        char tag[48]{};
        _snprintf_s(tag, sizeof(tag), _TRUNCATE, "activecmp_%016llX", (unsigned long long)rec.originalHash);
        const size_t bytes = rec.code->size() * sizeof(uint32_t);
        DumpSpirv(tag, 0, rec.code->data(), bytes);
        LogLine("SPVTRACE ACTIVE_COMPUTE_DUMP hash=%016llX bytes=%zu words=%zu module=%p",
            (unsigned long long)rec.originalHash, bytes, rec.code->size(), (void*)rec.module);
    }

    // v9: dump final SPIR-V for suspect compute pipelines BEFORE entering the driver.
    // SpvInfo::code is the exact module retained after all rewrites and sent to AMDVLK.
    void DumpSuspiciousComputeShaderForPipeline(const VkComputePipelineCreateInfo& ci, uint32_t pipelineIndex) {
        if (IsFake(ci.stage.module)) return;
        SpvInfo si;
        if (!GetSpvInfo(ci.stage.module, si)) return;
        if (si.model != 5) return;

        const bool group = si.caps.find("GroupNonUniform") != std::string::npos;
        const bool heavy = si.caps.find("Float16") != std::string::npos ||
                           si.caps.find("Int16") != std::string::npos ||
                           si.caps.find("cap45") != std::string::npos ||
                           si.caps.find("cap46") != std::string::npos;
        if (!group || !heavy) return;

        if (!si.code || si.code->empty()) {
            static volatile LONG noCode = 0;
            const LONG n = InterlockedIncrement(&noCode);
            if (n <= 8)
                LogLine("COMPUTE SPIRV DUMP SKIPPED index=%u module=%p reason=KeepShaderCode_not_retained caps=[%s]",
                    pipelineIndex, (void*)ci.stage.module, si.caps.c_str());
            return;
        }

        const size_t bytes = si.code->size() * sizeof(uint32_t);
        const uint64_t hash = Fnv1a64(si.code->data(), bytes);
        static SRWLOCK dumpLock = SRWLOCK_INIT;
        static uint64_t dumpedHashes[64]{};
        static LONG dumpedCount = 0;
        bool alreadyDumped = false;
        bool overLimit = false;

        AcquireSRWLockExclusive(&dumpLock);
        for (LONG i = 0; i < dumpedCount; ++i) {
            if (dumpedHashes[i] == hash) { alreadyDumped = true; break; }
        }
        if (!alreadyDumped) {
            if (dumpedCount < static_cast<LONG>(_countof(dumpedHashes)))
                dumpedHashes[dumpedCount++] = hash;
            else
                overLimit = true;
        }
        ReleaseSRWLockExclusive(&dumpLock);

        if (alreadyDumped) return;
        if (overLimit) {
            static volatile LONG limitNotice = 0;
            if (InterlockedIncrement(&limitNotice) == 1)
                LogLine("COMPUTE SPIRV DUMP LIMIT reached (64 unique suspicious modules)");
            return;
        }

        char tag[48]{};
        _snprintf_s(tag, sizeof(tag), _TRUNCATE, "cmp_%016llX", (unsigned long long)hash);
        LogLine("COMPUTE SPIRV SNAPSHOT index=%u module=%p hash=%016llX bytes=%zu words=%zu version=%x rewritten=%u pipelineFlags=0x%X stage=0x%X stageFlags=0x%X specEntries=%u entry=%s caps=[%s]",
            pipelineIndex, (void*)ci.stage.module, (unsigned long long)hash, bytes, si.code->size(),
            si.version, si.rewritten ? 1u : 0u, (unsigned)ci.flags, (unsigned)ci.stage.stage,
            (unsigned)ci.stage.flags, ci.stage.pSpecializationInfo ? ci.stage.pSpecializationInfo->mapEntryCount : 0u,
            ci.stage.pName ? ci.stage.pName : "(null)", si.caps.c_str());
        DumpSpirv(tag, 0, si.code->data(), bytes);
    }

    // Post-mortem of a pipeline the driver rejected: log the interesting states and dump its shaders.
    void DescribeFailedGraphicsPipeline(const VkGraphicsPipelineCreateInfo& ci, long seq) {
        LogLine("FAILED GRAPHICS PIPELINE #%ld: flags=0x%X stages=%u renderPass=%p subpass=%u", seq, (unsigned)ci.flags,
            ci.stageCount, (void*)ci.renderPass, ci.subpass);
        for (uint32_t j = 0; j < ci.stageCount && ci.pStages; ++j) {
            const auto& st = ci.pStages[j];
            SpvInfo si;
            const bool have = GetSpvInfo(st.module, si);
            LogLine("  stage 0x%X entry=%s spec=%u module: spv=%x model=%u caps=[%s]%s", (unsigned)st.stage,
                st.pName ? st.pName : "?", st.pSpecializationInfo ? st.pSpecializationInfo->mapEntryCount : 0u,
                have ? si.version : 0u, have ? si.model : 0u, have ? si.caps.c_str() : "unknown",
                (have && si.rewritten) ? " (REWRITTEN ray-query shader)" : "");
            if (have && si.code && !si.code->empty()) {
                char tag[48];
                _snprintf_s(tag, sizeof(tag), _TRUNCATE, "gpfail%ld_stage%X", seq, (unsigned)st.stage);
                DumpSpirv(tag, 0, si.code->data(), si.code->size() * 4);
            }
        }
        if (ci.pVertexInputState) {
            const auto* vi = ci.pVertexInputState;
            LogLine("  vertex input: bindings=%u attributes=%u", vi->vertexBindingDescriptionCount, vi->vertexAttributeDescriptionCount);
            for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount && a < 24; ++a)
                LogLine("    attr loc=%u binding=%u format=%d offset=%u", vi->pVertexAttributeDescriptions[a].location,
                    vi->pVertexAttributeDescriptions[a].binding, (int)vi->pVertexAttributeDescriptions[a].format,
                    vi->pVertexAttributeDescriptions[a].offset);
        }
        if (ci.pInputAssemblyState) LogLine("  topology=%d primitiveRestart=%u", (int)ci.pInputAssemblyState->topology, ci.pInputAssemblyState->primitiveRestartEnable);
        if (ci.pRasterizationState) LogLine("  raster: polygonMode=%d cull=%u depthClamp=%u discard=%u", (int)ci.pRasterizationState->polygonMode,
            (unsigned)ci.pRasterizationState->cullMode, ci.pRasterizationState->depthClampEnable, ci.pRasterizationState->rasterizerDiscardEnable);
        if (ci.pMultisampleState) LogLine("  samples=%d sampleShading=%u alphaToCoverage=%u", (int)ci.pMultisampleState->rasterizationSamples,
            ci.pMultisampleState->sampleShadingEnable, ci.pMultisampleState->alphaToCoverageEnable);
        if (ci.pColorBlendState) LogLine("  color attachments=%u logicOp=%u", ci.pColorBlendState->attachmentCount, ci.pColorBlendState->logicOpEnable);
        if (ci.pDynamicState) {
            char dyn[300]{}; size_t l = 0;
            for (uint32_t d = 0; d < ci.pDynamicState->dynamicStateCount && l + 12 < sizeof(dyn); ++d)
                l += _snprintf_s(dyn + l, sizeof(dyn) - l, _TRUNCATE, "%d ", (int)ci.pDynamicState->pDynamicStates[d]);
            LogLine("  dynamic states: %s", dyn);
        }
        for (const VkBaseInStructure* n = reinterpret_cast<const VkBaseInStructure*>(ci.pNext); n; n = n->pNext)
            LogLine("  pNext sType=%u", (unsigned)n->sType);
    }

    // ---- crash handler ---------------------------------------------------------
    // Function name for an address inside THIS proxy dll, from vulkan-1.pdb (build with /Zi + /DEBUG,
    // keep the .pdb next to the dll). Uses a private dbghelp session so the game's own is not disturbed.
    bool ProxySymbolName(const void* addr, char* out, size_t cap) {
#ifdef _MSC_VER
        static HANDLE hs = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x56504B31));
        static bool init = false, ok = false;
        if (!init) {
            init = true;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            if (SymInitialize(hs, nullptr, FALSE) && g_thisModule) {
                wchar_t path[MAX_PATH]{};
                GetModuleFileNameW(g_thisModule, path, MAX_PATH);
                auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_thisModule);
                auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(g_thisModule) + dos->e_lfanew);
                ok = SymLoadModuleExW(hs, nullptr, path, nullptr, reinterpret_cast<DWORD64>(g_thisModule),
                                      nt->OptionalHeader.SizeOfImage, nullptr, 0) != 0;
            }
        }
        if (!ok) return false;
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256]{};
        auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(hs, reinterpret_cast<DWORD64>(addr), &disp, si)) {
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llX", si->Name, static_cast<unsigned long long>(disp));
            return true;
        }
#else
        (void)addr; (void)out; (void)cap;
#endif
        return false;
    }

    void ModuleOffset(const void* addr, char* out, size_t cap) {
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
            char name[MAX_PATH]{};
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llX", base ? base + 1 : name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
            char sym[200];
            if (mod == g_thisModule && ProxySymbolName(addr, sym, sizeof(sym))) {
                const size_t l = strlen(out);
                if (l + 4 < cap) _snprintf_s(out + l, cap - l, _TRUNCATE, "  <%s>", sym);
            }
        } else _snprintf_s(out, cap, _TRUNCATE, "%p", addr);
    }

    void ReportCrash(PEXCEPTION_POINTERS ep);
    LONG CALLBACK CrashHandler(PEXCEPTION_POINTERS ep) {
        if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
        const DWORD c0 = ep->ExceptionRecord->ExceptionCode;
        const bool fatal0 = c0 == EXCEPTION_ACCESS_VIOLATION || 
                            c0 == EXCEPTION_INT_DIVIDE_BY_ZERO || c0 == EXCEPTION_PRIV_INSTRUCTION ||
                            c0 == EXCEPTION_IN_PAGE_ERROR;
        const bool guarded = fatal0 && t_guard.active;
        ReportCrash(ep);
        if (guarded) {
            // Fault inside a protected driver call: resume at the saved context with the "tripped" flag set.
            t_guard.tripped = 1;
            t_guard.active = 0;
            *ep->ContextRecord = t_guard.ctx;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Extra safety: EXEC@0 (null function pointer).
        // Case A: inside a DriverCall-guarded section -> resume at saved context.
        if (fatal0 && c0 == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2 &&
            ep->ExceptionRecord->ExceptionInformation[0] == 8 /* EXEC */ &&
            ep->ExceptionRecord->ExceptionInformation[1] == 0 &&
            t_guard.ctx.Rip != 0 && t_guard.active) {
            LogLine("!!! EXEC@0 recovered via saved DriverCall context");
            t_guard.tripped = 1;
            t_guard.active = 0;
            *ep->ContextRecord = t_guard.ctx;
            InterlockedIncrement(&g_driverCrashCount);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Case B: null call from the game itself (stored a nullptr from GIPA/GDPA).
        // On x64 the return address is still on the stack at [RSP]. Skip the failed
        // call by returning 0 (VK_SUCCESS / null handle) to the caller.
        if (fatal0 && c0 == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2 &&
            ep->ExceptionRecord->ExceptionInformation[0] == 8 /* EXEC */ &&
            ep->ExceptionRecord->ExceptionInformation[1] == 0 &&
            ep->ContextRecord->Rsp) {
            __try {
                const ULONG64 ret = *reinterpret_cast<ULONG64*>(ep->ContextRecord->Rsp);
                if (ret > 0x10000ull) {
                    char caller[300]; ModuleOffset((void*)ret, caller, sizeof(caller));
                LogLine("!!! EXEC@0 skipped null call, caller=%s", caller);
                    ep->ContextRecord->Rip = ret;
                    ep->ContextRecord->Rsp += 8;  // pop return address
                    ep->ContextRecord->Rax = 0;   // return VK_SUCCESS / nullptr
                    InterlockedIncrement(&g_driverCrashCount);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // stack unreadable - fall through
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void ReportCrash(PEXCEPTION_POINTERS ep) {
        if (!ep || !ep->ExceptionRecord) return;
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || 
                           code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                           code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR;
        if (!fatal) return;
        static volatile LONG reported = 0;
        char where[300];
        ModuleOffset(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        {
            // Report every DISTINCT crash location once (the final fatal crash must not be hidden
            // by earlier, already handled driver crashes).
            char sig[400];
            _snprintf_s(sig, sizeof(sig), _TRUNCATE, "crash:%08lX:%s", code, where);
            if (!FirstTime(sig)) return;
            if (InterlockedIncrement(&reported) > 60) return;
        }
        LogLine("!!! EXCEPTION 0x%08lX at %s (thread %lu)%s", code, where, GetCurrentThreadId(),
            t_guard.active ? " [inside guarded driver call: will recover]" : "");
        if (t_job[0]) LogLine("!!! this thread's current job: %s", t_job);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            LogLine("!!! access violation: %s address 0x%llX",
                ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" : (ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC"),
                static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1]));
        if (ep->ContextRecord) {
            const CONTEXT* c = ep->ContextRecord;
            LogLine("!!! RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX", c->Rax, c->Rbx, c->Rcx, c->Rdx);
            LogLine("!!! RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX", c->Rsi, c->Rdi, c->R8, c->R9);
            LogLine("!!! R14=%016llX R15=%016llX RSP=%016llX RIP=%016llX", c->R14, c->R15, c->Rsp, c->Rip);
        }
        void* frames[32]{};
        const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT k = 0; k < n; ++k) {
            ModuleOffset(frames[k], where, sizeof(where));
            LogLine("!!!   frame %u: %s", static_cast<unsigned>(k), where);
        }
        // Name the amdvlk64 functions: compare frame addresses with the driver's real entry points.
        if (g_lastDevice && g_realGdpa) {
            static const char* const kExtra[] = {
                "vkCreateShaderModule", "vkCreateGraphicsPipelines", "vkCreateComputePipelines", "vkCmdBindPipeline",
                "vkCmdDraw", "vkCmdDrawIndexed", "vkCmdDispatch", "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect",
                "vkCmdBindDescriptorSets", "vkCmdPushConstants", "vkCmdBindVertexBuffers", "vkCmdBindIndexBuffer",
                "vkCreatePipelineLayout", "vkCreateDescriptorSetLayout", "vkAllocateDescriptorSets",
                "vkUpdateDescriptorSets", "vkCreateSampler", "vkCreateImage", "vkCreateImageView", "vkCreateBuffer",
                "vkAllocateMemory", "vkBindImageMemory", "vkBindBufferMemory", "vkGetImageMemoryRequirements",
                "vkGetBufferMemoryRequirements", "vkCreateSwapchainKHR", "vkQueuePresentKHR", "vkCreateCommandPool",
                "vkAllocateCommandBuffers", "vkCreateFence", "vkCreateRenderPass", "vkCreateFramebuffer",
                "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkQueueSubmit", "vkDestroyPipeline",
                "vkDestroyShaderModule", "vkCreateDescriptorPool", "vkCreateQueryPool", "vkCreateSemaphore" };
            struct NA { const char* n; uintptr_t a; };
            std::vector<NA> names;
            for (const char* n : kExtra) { auto f = g_realGdpa(g_lastDevice, n); if (f) names.push_back({ n, reinterpret_cast<uintptr_t>(f) }); }
            for (int i = 0; i < VK_PROXY_EXPORT_COUNT; ++i) {
                auto f = g_realGdpa(g_lastDevice, kVkProxyExportNames[i]);
                if (f) names.push_back({ kVkProxyExportNames[i], reinterpret_cast<uintptr_t>(f) });
            }
            for (USHORT k = 0; k < n; ++k) {
                const uintptr_t a = reinterpret_cast<uintptr_t>(frames[k]);
                const NA* best = nullptr;
                for (const NA& e : names) if (e.a <= a && a - e.a < 0x400 && (!best || e.a > best->a)) best = &e;
                if (best) LogLine("!!!   frame %u is inside driver function %s (+0x%llX)", static_cast<unsigned>(k), best->n,
                    static_cast<unsigned long long>(a - best->a));
            }
        }
        LogLine("!!! last API breadcrumbs (oldest first):");
        const LONG end = g_crumbIdx;
        for (int k = t_guard.active ? kCrumbCount - 12 : 0; k < kCrumbCount; ++k) {
            const char* s = g_crumbs[(end + k) & (kCrumbCount - 1)];
            if (s[0]) LogLine("!!!   %s", s);
        }
    }

    PVOID g_vehHandle = nullptr;
    // The game may install its own vectored handler after us; make sure ours runs FIRST.
    void InstallCrashHandlerFirst() {
        if (g_vehHandle) RemoveVectoredExceptionHandler(g_vehHandle);
        g_vehHandle = AddVectoredExceptionHandler(1, CrashHandler);
    }
}

// ---------------------------------------------------------------------------
// v4: diagnostic wrappers for swapchain / shader / present
// ---------------------------------------------------------------------------
// Optional post-process: brightness gain applied to the swapchain image right before it is presented.
// out = dst * (1 + g) is done with fixed-function blending (src = vec4(g), srcFactor = DST_COLOR, dstFactor = ONE),
// repeated N times for gains above 2. Enabled with VulkanProxy.ini  BrightnessGain=1.5  (1.0 = off).
// ---------------------------------------------------------------------------
namespace {
namespace post {
static const uint32_t kGainVert[] = {0x07230203,0x00010000,0x0008000b,0x0000002b,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,0x0007000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x0000001d,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000009,0x00000070,0x00060005,0x0000000c,0x565f6c67,0x65747265,0x646e4978,0x00007865,0x00060005,0x0000001b,0x505f6c67,0x65567265,0x78657472,0x00000000,0x00060006,0x0000001b,0x00000000,0x505f6c67,0x7469736f,0x006e6f69,0x00070006,0x0000001b,0x00000001,0x505f6c67,0x746e696f,0x657a6953,0x00000000,0x00070006,0x0000001b,0x00000002,0x435f6c67,0x4470696c,0x61747369,0x0065636e,0x00070006,0x0000001b,0x00000003,0x435f6c67,0x446c6c75,0x61747369,0x0065636e,0x00030005,0x0000001d,0x00000000,0x00040047,0x0000000c,0x0000000b,0x0000002a,0x00030047,0x0000001b,0x00000002,0x00050048,0x0000001b,0x00000000,0x0000000b,0x00000000,0x00050048,0x0000001b,0x00000001,0x0000000b,0x00000001,0x00050048,0x0000001b,0x00000002,0x0000000b,0x00000003,0x00050048,0x0000001b,0x00000003,0x0000000b,0x00000004,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,0x0000000a,0x00000020,0x00000001,0x00040020,0x0000000b,0x00000001,0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000e,0x00000001,0x0004002b,0x0000000a,0x00000010,0x00000002,0x00040017,0x00000017,0x00000006,0x00000004,0x00040015,0x00000018,0x00000020,0x00000000,0x0004002b,0x00000018,0x00000019,0x00000001,0x0004001c,0x0000001a,0x00000006,0x00000019,0x0006001e,0x0000001b,0x00000017,0x00000006,0x0000001a,0x0000001a,0x00040020,0x0000001c,0x00000003,0x0000001b,0x0004003b,0x0000001c,0x0000001d,0x00000003,0x0004002b,0x0000000a,0x0000001e,0x00000000,0x0004002b,0x00000006,0x00000020,0x40000000,0x0004002b,0x00000006,0x00000022,0x3f800000,0x0004002b,0x00000006,0x00000025,0x00000000,0x00040020,0x00000029,0x00000003,0x00000017,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,0x00000007,0x0004003d,0x0000000a,0x0000000d,0x0000000c,0x000500c4,0x0000000a,0x0000000f,0x0000000d,0x0000000e,0x000500c7,0x0000000a,0x00000011,0x0000000f,0x00000010,0x0004006f,0x00000006,0x00000012,0x00000011,0x0004003d,0x0000000a,0x00000013,0x0000000c,0x000500c7,0x0000000a,0x00000014,0x00000013,0x00000010,0x0004006f,0x00000006,0x00000015,0x00000014,0x00050050,0x00000007,0x00000016,0x00000012,0x00000015,0x0003003e,0x00000009,0x00000016,0x0004003d,0x00000007,0x0000001f,0x00000009,0x0005008e,0x00000007,0x00000021,0x0000001f,0x00000020,0x00050050,0x00000007,0x00000023,0x00000022,0x00000022,0x00050083,0x00000007,0x00000024,0x00000021,0x00000023,0x00050051,0x00000006,0x00000026,0x00000024,0x00000000,0x00050051,0x00000006,0x00000027,0x00000024,0x00000001,0x00070050,0x00000017,0x00000028,0x00000026,0x00000027,0x00000025,0x00000022,0x00050041,0x00000029,0x0000002a,0x0000001d,0x0000001e,0x0003003e,0x0000002a,0x00000028,0x000100fd,0x00010038};
static const uint32_t kGainFrag[] = {0x07230203,0x00010000,0x0008000b,0x00000013,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,0x0006000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00030010,0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,0x00000009,0x4374756f,0x726f6c6f,0x00000000,0x00030005,0x0000000a,0x00004350,0x00040006,0x0000000a,0x00000000,0x00000067,0x00030005,0x0000000c,0x00006370,0x00040047,0x00000009,0x0000001e,0x00000000,0x00030047,0x0000000a,0x00000002,0x00050048,0x0000000a,0x00000000,0x00000023,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x0003001e,0x0000000a,0x00000006,0x00040020,0x0000000b,0x00000009,0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000009,0x00040015,0x0000000d,0x00000020,0x00000001,0x0004002b,0x0000000d,0x0000000e,0x00000000,0x00040020,0x0000000f,0x00000009,0x00000006,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x0000000f,0x00000010,0x0000000c,0x0000000e,0x0004003d,0x00000006,0x00000011,0x00000010,0x00070050,0x00000007,0x00000012,0x00000011,0x00000011,0x00000011,0x00000011,0x0003003e,0x00000009,0x00000012,0x000100fd,0x00010038};

    struct SwapInfo { VkFormat fmt = VK_FORMAT_UNDEFINED; VkExtent2D ext{}; uint32_t generation = 0; };
    struct SwapState {
        uint32_t generation = 0;
        bool ready = false, failed = false;
        VkRenderPass rp = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> fbs;
        std::vector<VkCommandBuffer> cbs;
        std::vector<VkSemaphore> sems;
    };
    SRWLOCK g_lock = SRWLOCK_INIT;
    std::map<uint64_t, SwapInfo>& Infos() { static auto* m = new std::map<uint64_t, SwapInfo>(); return *m; }
    std::map<uint64_t, SwapState>& States() { static auto* m = new std::map<uint64_t, SwapState>(); return *m; }
    uint32_t g_gen = 0;

    inline uint64_t H(VkSwapchainKHR s) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(s)); }
    void Record(VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext) {
        AcquireSRWLockExclusive(&g_lock);
        Infos()[H(sc)] = { fmt, ext, ++g_gen };
        ReleaseSRWLockExclusive(&g_lock);
    }

    template<class T> T Fn(VkDevice d, const char* name) { return reinterpret_cast<T>(g_realGdpa(d, name)); }

    bool Build(VkDevice dev, VkQueue queue, VkSwapchainKHR sc, const SwapInfo& info, SwapState& st) {
        auto createRP = Fn<PFN_vkCreateRenderPass>(dev, "vkCreateRenderPass");
        auto createFB = Fn<PFN_vkCreateFramebuffer>(dev, "vkCreateFramebuffer");
        auto createView = Fn<PFN_vkCreateImageView>(dev, "vkCreateImageView");
        auto createSM = Fn<PFN_vkCreateShaderModule>(dev, "vkCreateShaderModule");
        auto destroySM = Fn<PFN_vkDestroyShaderModule>(dev, "vkDestroyShaderModule");
        auto createPL = Fn<PFN_vkCreatePipelineLayout>(dev, "vkCreatePipelineLayout");
        auto createGP = Fn<PFN_vkCreateGraphicsPipelines>(dev, "vkCreateGraphicsPipelines");
        auto createPool = Fn<PFN_vkCreateCommandPool>(dev, "vkCreateCommandPool");
        auto allocCB = Fn<PFN_vkAllocateCommandBuffers>(dev, "vkAllocateCommandBuffers");
        auto beginCB = Fn<PFN_vkBeginCommandBuffer>(dev, "vkBeginCommandBuffer");
        auto endCB = Fn<PFN_vkEndCommandBuffer>(dev, "vkEndCommandBuffer");
        auto cmdBeginRP = Fn<PFN_vkCmdBeginRenderPass>(dev, "vkCmdBeginRenderPass");
        auto cmdEndRP = Fn<PFN_vkCmdEndRenderPass>(dev, "vkCmdEndRenderPass");
        auto cmdBind = Fn<PFN_vkCmdBindPipeline>(dev, "vkCmdBindPipeline");
        auto cmdPush = Fn<PFN_vkCmdPushConstants>(dev, "vkCmdPushConstants");
        auto cmdDraw = Fn<PFN_vkCmdDraw>(dev, "vkCmdDraw");
        auto createSem = Fn<PFN_vkCreateSemaphore>(dev, "vkCreateSemaphore");
        auto getImages = Fn<PFN_vkGetSwapchainImagesKHR>(dev, "vkGetSwapchainImagesKHR");
        auto getQueue = Fn<PFN_vkGetDeviceQueue>(dev, "vkGetDeviceQueue");
        if (!createRP || !createFB || !createView || !createSM || !destroySM || !createPL || !createGP || !createPool ||
            !allocCB || !beginCB || !endCB || !cmdBeginRP || !cmdEndRP || !cmdBind || !cmdPush || !cmdDraw ||
            !createSem || !getImages || !getQueue) { LogLine("BRIGHTNESS: missing driver functions"); return false; }

        // queue family of the presenting queue
        uint32_t family = 0xFFFFFFFFu;
        for (const auto& f : g_createdFamilies) {
            for (uint32_t qi = 0; qi < f.second && family == 0xFFFFFFFFu; ++qi) {
                VkQueue q = VK_NULL_HANDLE;
                getQueue(dev, f.first, qi, &q);
                if (q == queue) family = f.first;
            }
        }
        if (family == 0xFFFFFFFFu) { LogLine("BRIGHTNESS: could not identify the queue family of the present queue"); return false; }

        uint32_t count = 0;
        if (getImages(dev, sc, &count, nullptr) != VK_SUCCESS || count == 0 || count > 16) return false;
        std::vector<VkImage> images(count);
        if (getImages(dev, sc, &count, images.data()) != VK_SUCCESS) return false;

        // render pass: one color attachment kept in PRESENT_SRC layout
        VkAttachmentDescription att{};
        att.format = info.fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; deps[1].dstAccessMask = 0;
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2; rpci.pDependencies = deps;
        if (createRP(dev, &rpci, nullptr, &st.rp) != VK_SUCCESS) return false;

        VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if (createPL(dev, &plci, nullptr, &st.layout) != VK_SUCCESS) return false;

        VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo smv{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smv.codeSize = sizeof(kGainVert); smv.pCode = kGainVert;
        VkShaderModuleCreateInfo smf{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        smf.codeSize = sizeof(kGainFrag); smf.pCode = kGainFrag;
        if (createSM(dev, &smv, nullptr, &vs) != VK_SUCCESS || createSM(dev, &smf, nullptr, &fs) != VK_SUCCESS) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{ 0.f, 0.f, static_cast<float>(info.ext.width), static_cast<float>(info.ext.height), 0.f, 1.f };
        VkRect2D sc2{ { 0, 0 }, info.ext };
        VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc2;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &ba;
        VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vps;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = st.layout; gp.renderPass = st.rp; gp.subpass = 0;
        const VkResult pr = createGP(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &st.pipe);
        destroySM(dev, vs, nullptr); destroySM(dev, fs, nullptr);
        if (pr != VK_SUCCESS) { LogLine("BRIGHTNESS: pipeline creation failed: %d", (int)pr); return false; }

        VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpi.queueFamilyIndex = family;
        if (createPool(dev, &cpi, nullptr, &st.pool) != VK_SUCCESS) return false;
        st.cbs.resize(count);
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = st.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = count;
        if (allocCB(dev, &cai, st.cbs.data()) != VK_SUCCESS) return false;

        // passes: gain = (1 + g)^N with g <= 1 (source color is clamped to [0,1] for UNORM targets)
        const float G = g_spvCfg.brightness;
        int passes = 1;
        while (passes < 4 && std::pow(2.0f, static_cast<float>(passes)) < G) ++passes;
        const float g = std::pow(G, 1.0f / static_cast<float>(passes)) - 1.0f;
        const float gClamped = g > 1.0f ? 1.0f : g;

        st.views.resize(count); st.fbs.resize(count); st.sems.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = images[i]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = info.fmt;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (createView(dev, &vci, nullptr, &st.views[i]) != VK_SUCCESS) return false;
            VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fci.renderPass = st.rp; fci.attachmentCount = 1; fci.pAttachments = &st.views[i];
            fci.width = info.ext.width; fci.height = info.ext.height; fci.layers = 1;
            if (createFB(dev, &fci, nullptr, &st.fbs[i]) != VK_SUCCESS) return false;
            VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (createSem(dev, &sci, nullptr, &st.sems[i]) != VK_SUCCESS) return false;

            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            if (beginCB(st.cbs[i], &bi) != VK_SUCCESS) return false;
            VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            rbi.renderPass = st.rp; rbi.framebuffer = st.fbs[i]; rbi.renderArea = { { 0, 0 }, info.ext };
            cmdBeginRP(st.cbs[i], &rbi, VK_SUBPASS_CONTENTS_INLINE);
            cmdBind(st.cbs[i], VK_PIPELINE_BIND_POINT_GRAPHICS, st.pipe);
            for (int pidx = 0; pidx < passes; ++pidx) {
                cmdPush(st.cbs[i], st.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &gClamped);
                cmdDraw(st.cbs[i], 3, 1, 0, 0);
            }
            cmdEndRP(st.cbs[i]);
            if (endCB(st.cbs[i]) != VK_SUCCESS) return false;
        }
        LogLine("BRIGHTNESS: post pass ready (%ux%u, format %d, %u images, gain %.2f = (1+%.3f)^%d, queue family %u)",
            info.ext.width, info.ext.height, (int)info.fmt, count, G, gClamped, passes, family);
        return true;
    }

    // Returns a semaphore the present must wait on (signalled by our submit), or VK_NULL_HANDLE to present unchanged.
    VkSemaphore Process(VkQueue queue, const VkPresentInfoKHR& pi) {
        if (pi.swapchainCount != 1 || !pi.pSwapchains || !pi.pImageIndices || pi.waitSemaphoreCount == 0 || !g_lastDevice) return VK_NULL_HANDLE;
        VkDevice dev = g_lastDevice;
        VkSwapchainKHR sc = pi.pSwapchains[0];
        VkSemaphore result = VK_NULL_HANDLE;
        AcquireSRWLockExclusive(&g_lock);
        auto iit = Infos().find(H(sc));
        if (iit != Infos().end()) {
            SwapState& st = States()[H(sc)];
            if (!st.failed && (!st.ready || st.generation != iit->second.generation)) {
                st = SwapState{};
                st.generation = iit->second.generation;
                if (Build(dev, queue, sc, iit->second, st)) st.ready = true;
                else { st.failed = true; LogLine("BRIGHTNESS: setup failed, feature disabled for this swapchain"); }
            }
            const uint32_t idx = *pi.pImageIndices;
            if (st.ready && idx < st.cbs.size()) {
                auto submit = Fn<PFN_vkQueueSubmit>(dev, "vkQueueSubmit");
                if (submit) {
                    std::vector<VkPipelineStageFlags> stageMask(pi.waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                    si.waitSemaphoreCount = pi.waitSemaphoreCount; si.pWaitSemaphores = pi.pWaitSemaphores;
                    si.pWaitDstStageMask = stageMask.data();
                    si.commandBufferCount = 1; si.pCommandBuffers = &st.cbs[idx];
                    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &st.sems[idx];
                    if (submit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) result = st.sems[idx];
                }
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
        return result;
    }
}
}

// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    auto real = RealExport<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    LoadSpvConfig();
    static volatile LONG swapchainSequence = 0;
    const LONG swapchainId = InterlockedIncrement(&swapchainSequence);
    Crumb("vkCreateSwapchainKHR ENTER id=%ld device=%p", (long)swapchainId, (void*)device);
    LogLine("SWAPCHAIN ENTER id=%ld device=%p", (long)swapchainId, (void*)device);
    VkSwapchainCreateInfoKHR patchedInfo{};
    const VkSwapchainCreateInfoKHR* useInfo = pInfo;
    bool postProcessAllowed = pInfo && ((pInfo->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0);
    if (pInfo && g_spvCfg.brightness > 1.0f && !postProcessAllowed) {
        // Do not make an otherwise valid swapchain fail just because the optional
        // brightness pass wants color-attachment usage. Query the surface first;
        // if this usage is unsupported, leave the application's create info untouched.
        auto getSurfaceCaps = RealExport<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        VkSurfaceCapabilitiesKHR caps{};
        const bool supported = g_lastRtPhysicalDevice && getSurfaceCaps &&
            getSurfaceCaps(g_lastRtPhysicalDevice, pInfo->surface, &caps) == VK_SUCCESS &&
            (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        if (supported) {
            patchedInfo = *pInfo;
            patchedInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            useInfo = &patchedInfo;
            postProcessAllowed = true;
        } else {
            LogLine("BRIGHTNESS: disabled for this swapchain; COLOR_ATTACHMENT usage is not supported by the surface");
        }
    }
    VkResult r = real(device, useInfo, pAllocator, pSwapchain);
    LogLine("SWAPCHAIN EXIT id=%ld result=%d handle=%p",
        (long)swapchainId, (int)r, pSwapchain ? (void*)*pSwapchain : nullptr);
    if (r == VK_SUCCESS && pInfo && pSwapchain && postProcessAllowed && g_spvCfg.brightness > 1.0f)
        post::Record(*pSwapchain, pInfo->imageFormat, pInfo->imageExtent);
    LogLine("vkCreateSwapchainKHR: format=%d colorspace=%d presentMode=%d minImg=%u %ux%u result=%d",
        pInfo ? (int)pInfo->imageFormat : -1,
        pInfo ? (int)pInfo->imageColorSpace : -1,
        pInfo ? (int)pInfo->presentMode : -1,
        pInfo ? pInfo->minImageCount : 0,
        pInfo ? pInfo->imageExtent.width : 0,
        pInfo ? pInfo->imageExtent.height : 0,
        (int)r);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
    auto real = RealExport<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    if (!real) {
        LogLine("vkCreateShaderModule: RealExport NULL -> FAKE module");
        if (pShaderModule) *pShaderModule = MakeFake<VkShaderModule>();
        return VK_SUCCESS;
    }
    LoadSpvConfig();
    if (!pInfo || !pInfo->pCode || !pShaderModule) {
        if (pShaderModule) *pShaderModule = MakeFake<VkShaderModule>();
        return VK_SUCCESS;
    }

    // Never truncate a malformed byte count into uint32_t words. Pass malformed input
    // through to the real ICD so it can return the Vulkan validation error normally.
    if (pInfo->codeSize < 20 || (pInfo->codeSize & 3u) != 0) {
        LogLine("SPIRV: malformed vkCreateShaderModule byte size=%zu; forwarding unchanged", pInfo->codeSize);
        return real(device, pInfo, pAllocator, pShaderModule);
    }

    const uint32_t* originalCode = pInfo->pCode;
    const size_t originalBytes = pInfo->codeSize;
    const uint64_t originalHash = Fnv1a64(originalCode, originalBytes);
    const uint32_t* code = originalCode;
    size_t nWords = originalBytes / sizeof(uint32_t);
    std::vector<uint32_t> replacement;
    bool didReplace = false;

    // Replacement selection happens BEFORE capability analysis and compatibility
    // lowering. Previously a replacement was loaded after analysis and blindly
    // cleared si.deny; the chosen SPIR-V could therefore bypass every safety check.
    //
    // Two lookup strategies, in order:
    //   1. exact byte hash  - works when the game emits identical modules (the
    //      reference GCN2 machine the spv_replace folder was built on);
    //   2. structure match  - makes the SAME folder work on NVIDIA/Intel/AMD cards
    //      whose capability profile makes the game emit an equivalent module with
    //      different ids/enums. Without this, spv_replace is silently skipped on
    //      those GPUs and the replacement lighting never appears.
    if (g_replMap && !g_replMap->empty()) {
        std::wstring chosenPath;
        bool havePath = false;
        const char* matchKind = "hash";

        const auto it = g_replMap->find(originalHash);
        if (it != g_replMap->end()) {
            chosenPath = it->second;
            havePath = true;
            InterlockedIncrement(&g_replHitsExact);
        } else if (FindReplacement(originalCode, nWords, originalHash, chosenPath, &matchKind)) {
            havePath = true;
            InterlockedIncrement(&g_replHitsSimilar);
            static volatile LONG matchLogs = 0;
            if (InterlockedIncrement(&matchLogs) <= 24)
                LogLine("SPIRV: shader replacement matched (%s) hash=%016llX - this GPU builds the module differently, substituting the equivalent spv_replace module",
                    matchKind, static_cast<unsigned long long>(originalHash));
        }

        if (havePath) {
            std::string replacementError;
            if (ReadReplacement(chosenPath, replacement) &&
                ValidateReplacementEnvelope(replacement, replacementError)) {
                code = replacement.data();
                nWords = replacement.size();
                didReplace = true;
                LogLine("SPIRV: shader replacement ACCEPTED (%s) hash=%016llX (%zu -> %zu words); capability analysis will run on replacement",
                    matchKind, static_cast<unsigned long long>(originalHash), originalBytes / sizeof(uint32_t), nWords);
            } else {
                if (replacementError.empty()) replacementError = "file read failed or SPIR-V magic invalid";
                replacement.clear();
                LogLine("SPIRV: shader replacement REJECTED (%s) hash=%016llX: %s; original module retained",
                    matchKind, static_cast<unsigned long long>(originalHash), replacementError.c_str());
            }
        } else {
            static volatile LONG misses = 0;
            if (InterlockedIncrement(&misses) <= 15)
                LogLine("SPIRV: no replacement for hash=%016llX (spv_replace has no module of the same stage/interface; see the replacement index line at startup)",
                    static_cast<unsigned long long>(originalHash));
        }
    }

    const ActiveRtDevice activeRt = GetActiveRtDevice(device);
    const ShaderCaps hwCaps = GetShaderCaps(activeRt);
    SpvInfo si = AnalyzeSpirv(code, nWords, activeRt.nativeRTP, activeRt.nativeRQ, hwCaps);
    si.originalHash = originalHash;
    si.replacement = didReplace;
    static volatile LONG seen = 0;
    const LONG n = InterlockedIncrement(&seen);

    // Ray-query fallback is intentionally NOT applied to every shader.
    // Replacing all queries with "no hit" is not visually neutral: reflection/GI shaders can
    // turn missing ray hits into black regions that grow/accumulate in the scene.
    // Keep native RayQuery modules unchanged unless the module is explicitly denied for RT
    // and the user has enabled RewriteRayQuery. This preserves the original PROXY behavior.
    std::vector<uint32_t> rewritten;
    bool didRewrite = false;
    if (si.rqOnly && !si.deny) {
        static volatile LONG rqPreserved = 0;
        const LONG k = InterlockedIncrement(&rqPreserved);
        if (k <= 12)
            LogLine("SPIRV: RayQuery shader #%ld preserved unchanged (DenyRayTracing=0; no-hit fallback not applied)", n);
    }
    // Only rewrite when the module is actually denied for a ray-tracing capability.
    if (si.deny && si.rqOnly && g_spvCfg.rewriteRayQuery) {
        std::string err;
        static volatile LONG rwOk = 0, rwFail = 0;
        if (spvrw::StripRayQuery(code, nWords, rewritten, err)) {
            // Preserve every non-RayQuery capability. StripRayQuery() is responsible for
            // removing only declarations made obsolete by the RayQuery rewrite.
            // Reject near-empty / over-stripped results (invalid SPIR-V → driver -3)
            if ((nWords > 4000 && rewritten.size() < 4000) ||
                (nWords > 3000 && rewritten.size() * 5 < nWords)) {
                LogLine("SPIRV: ray-query strip too aggressive #%ld (%zu -> %zu), discarding", n, nWords, rewritten.size());
                rewritten.clear();
                const LONG k = InterlockedIncrement(&rwFail);
                if (k <= 20) LogLine("SPIRV: ray-query rewrite FAILED for shader #%ld: over-stripped (shader stays denied)", n);
            } else {
                didRewrite = true;
                // Clear only the RayTracing denial. Keep FP16/INT16/DotProduct/Mesh
                // and other independent denials until their own lowering succeeds.
                const size_t rtReasonPos = si.reason.find("RayTracing ");
                if (rtReasonPos != std::string::npos)
                    si.reason.erase(rtReasonPos, std::strlen("RayTracing "));
                const size_t softCapPos = si.reason.find("AnyCap46(soft) ");
                if (softCapPos != std::string::npos)
                    si.reason.erase(softCapPos, std::strlen("AnyCap46(soft) "));
                si.deny = si.denyOther;
                if (!si.deny) si.reason.clear();
                si.rewritten = true;
                if (g_spvCfg.promoteFloat16 && si.caps.find("Float16") != std::string::npos) {
                std::string err2;
                    std::vector<uint32_t> promoted;
                    if (spvrw::PromoteFloat16(rewritten.data(), rewritten.size(), promoted, err2) && promoted.size() > 100) {
                        rewritten.swap(promoted);
                        static volatile LONG fpRq = 0;
                        if (InterlockedIncrement(&fpRq) <= 15)
                            LogLine("SPIRV: Float16 promote after RQ rewrite shader #%ld (%zu words)", n, rewritten.size());
                    }
                }
                const LONG k = InterlockedIncrement(&rwOk);
                if (k <= 5 || k % 100 == 0) LogLine("SPIRV: ray-query shader #%ld rewritten (%zu -> %zu words), total rewritten=%ld", n, nWords, rewritten.size(), (long)k);
                if (k <= g_spvCfg.dumpRewrite) {
                    DumpSpirv("rq_in", n, code, nWords * sizeof(uint32_t));
                    DumpSpirv("rq_out", n, rewritten.data(), rewritten.size() * 4);
                }
            }
        } else {
            rewritten.clear();
            const LONG k = InterlockedIncrement(&rwFail);
            if (k <= 20) LogLine("SPIRV: ray-query rewrite FAILED for shader #%ld: %s (shader stays denied)", n, err.c_str());
            if (k <= g_spvCfg.dumpRewriteFail) DumpSpirv("rqfail", n, code, nWords * sizeof(uint32_t));
        }
    }
    // Promote Float16 ALU to Float32 so GCN2 can run menu/UI shaders that require shaderFloat16.
    // Also run when RQ rewrite did not already promote (RQ path promotes itself).
if (g_spvCfg.lowerFp16AndInt16 && (si.caps.find("Float16") != std::string::npos || si.caps.find("Int16") != std::string::npos)) {
    std::string err;
    std::vector<uint32_t> lowered;
    static volatile LONG loOk = 0, loFail = 0, loDump = 0, loBad = 0;
    const uint32_t* src = didRewrite ? rewritten.data() : code;
    size_t srcN = didRewrite ? rewritten.size() : nWords;
    size_t widenedLocalPointers = 0, widenedLocalAggregates = 0;
    bool ok = spvrw::LowerFp16AndInt16Alu(src, srcN, lowered, err, g_spvCfg.dropF16Cap != 0,
        &widenedLocalPointers, &widenedLocalAggregates) && lowered.size() > 100;
    if (ok && !spvrw::CheckStructure(lowered.data(), lowered.size(), err)) {
        ok = false;
        const LONG b = InterlockedIncrement(&loBad);
        if (b <= 20) LogLine("SPIRV: fp16+int16 lower BAD MODULE #%ld: %s (rejected, original kept)", n, err.c_str());
    }
    if (ok) {
        const LONG d = InterlockedIncrement(&loDump);
        if (d <= g_spvCfg.dumpRewrite) {
            DumpSpirv("low_in", n, src, srcN * 4);
            DumpSpirv("low_out", n, lowered.data(), lowered.size() * 4);
        }
        rewritten = std::move(lowered);
        didRewrite = true;
        si.rewritten = true;
        // Remove only requirements that this lowering actually handled.
        size_t reasonPos = std::string::npos;
        while ((reasonPos = si.reason.find("Float16 ")) != std::string::npos)
            si.reason.erase(reasonPos, std::strlen("Float16 "));
        while ((reasonPos = si.reason.find("Int16 ")) != std::string::npos)
            si.reason.erase(reasonPos, std::strlen("Int16 "));
        si.deny = !si.reason.empty();
        if (!si.deny) si.reason.clear();
        const LONG k = InterlockedIncrement(&loOk);
        if (k <= 10 || k % 50 == 0)
            LogLine("SPIRV: storage-aware fp16+int16 lowering shader #%ld (%zu -> %zu words), LocalPtrTypes=%zu LocalAggregateTypes=%zu total=%ld",
                n, srcN, rewritten.size(), widenedLocalPointers, widenedLocalAggregates, (long)k);
    } else {
        const LONG k = InterlockedIncrement(&loFail);
        if (k <= 15) LogLine("SPIRV: fp16+int16 lower FAILED #%ld: %s", n, err.c_str());
        // Preserve the original input for offline inspection. These failures happen
        // before vkCreateShaderModule reaches the driver, so no spv_crash dump exists.
        if (k <= g_spvCfg.dumpRewriteFail)
            DumpSpirv("low_fail", n, src, srcN * 4);
    }
}
    if (g_spvCfg.dumpSamples > 0 && si.model == 5) {
        static volatile LONG cubeSamples = 0, i64Samples = 0;
        const bool c45 = si.caps.find("cap45") != std::string::npos, i64 = si.caps.find("Int64") != std::string::npos;
        if (c45 && !i64 && InterlockedIncrement(&cubeSamples) <= g_spvCfg.dumpSamples) DumpSpirv("ok_cubearray", n, code, nWords * sizeof(uint32_t));
        else if (i64 && !c45 && InterlockedIncrement(&i64Samples) <= g_spvCfg.dumpSamples) DumpSpirv("ok_int64", n, code, nWords * sizeof(uint32_t));
    }
    static volatile LONG deniedSeen = 0;
    const LONG dn = si.deny ? InterlockedIncrement(&deniedSeen) : 0;
    const bool logIt = g_spvCfg.logAll || (si.deny && dn <= 30) || n <= 10;
    if (n % 200 == 0) LogLine("SHADER stats: total=%ld denied=%ld", n, (long)deniedSeen);
    if (logIt)
        LogLine("SHADER #%ld spv=%u.%u model=%u size=%zu source=%s caps=[%s]%s%s", n,
            (si.version >> 16) & 0xFF, (si.version >> 8) & 0xFF, si.model, nWords * sizeof(uint32_t),
            didReplace ? "spv_replace" : "original", si.caps.c_str(),
            si.deny ? " DENIED: " : "", si.deny ? si.reason.c_str() : "");

    // SafeModeAfterCrashes REMOVED — it FAKE'd menu/UI shaders after a few
    // recovered BindBufferMemory crashes → black menu with audio/cursor only.

    if (si.deny) {
        // The driver cannot run this module (GCN 2 has no fp16/int16/dot-product/RT/mesh).
        static volatile LONG fakeN = 0;
        LONG fn = InterlockedIncrement(&fakeN);
        if (fn <= 20 || fn % 50 == 0)
            LogLine("vkCreateShaderModule #%ld -> FAKE module (reason=%s) totalFake=%ld", n, si.reason.c_str(), (long)fn);
        *pShaderModule = MakeFake<VkShaderModule>();
        PutSpvInfo(*pShaderModule, si);
        return VK_SUCCESS;
    }

    VkShaderModuleCreateInfo ci = *pInfo;
    std::vector<uint32_t> patched;
    // The replacement is the new input module; any successful compatibility rewrite
    // must take precedence over the raw replacement buffer. The previous expression
    // gave didReplace priority and silently discarded subsequent RayQuery/FP16 lowering.
    const uint32_t* useCode = didRewrite ? rewritten.data() : code;
    size_t useWords = didRewrite ? rewritten.size() : nWords;
    if (g_spvCfg.patchVersion && si.version > 0x00010500u && si.version != 0) {
        patched.assign(useCode, useCode + useWords);
        patched[1] = 0x00010500u;                       // driver supports up to SPIR-V 1.5
        useCode = patched.data();
        if (FirstTime("spv:patchver")) LogLine("SPIRV: patching module version %x -> 1.5 (driver limit)", si.version);
    }
	
	
	// OpTerminateInvocation (4416) есть только в SPIR-V 1.6. После патча версии на 1.5
// модуль невалиден -> заменяем на OpKill (252), тот же эффект во фрагментном шейдере.
{
    bool need = false;
    for (size_t j = 0; j < useWords;) {
        const uint32_t wc = useCode[j] >> 16, op = useCode[j] & 0xFFFFu;
        if (wc == 0 || j + wc > useWords) break;
        if (op == 4416) { need = true; break; }
        j += wc;
    }
    if (need) {
        std::vector<uint32_t> tk(useCode, useCode + useWords);
        int fixed = 0;
        for (size_t j = 0; j < tk.size();) {
            const uint32_t wc = tk[j] >> 16, op = tk[j] & 0xFFFFu;
            if (wc == 0 || j + wc > tk.size()) break;
            if (op == 4416) { tk[j] = (wc << 16) | 252u; ++fixed; }
            j += wc;
        }
        static volatile LONG tiN = 0;
        if (InterlockedIncrement(&tiN) <= 10)
            LogLine("SPIRV: OpTerminateInvocation -> OpKill x%d in shader #%ld", fixed, n);
        rewritten.swap(tk);
        useCode = rewritten.data();
        useWords = rewritten.size();
    }
}
	
	
    // v8: сначала понижаем неподдерживаемые инструкции.
{
    std::vector<uint32_t> low;
    std::string lerr;
    if (spvrw::LowerUnsupportedOps(useCode, useWords, low, lerr)) {
        static volatile LONG loN = 0;
        if (InterlockedIncrement(&loN) <= 20)
            LogLine("SPIRV: lowered clock/subgroup-ballot in shader #%ld (%zu -> %zu words)", n, useWords, low.size());
        rewritten.swap(low);
        useCode = rewritten.data();
        useWords = rewritten.size();
    }
}
// Удаляем ТОЛЬКО те capability, которые реально больше не нужны.
// Удаление cap при живых 16-битных инструкциях = невалидный SPIR-V = -13.
{
    bool has16f = false, has16i = false;
    for (size_t j = 0; j < useWords;) {
        const uint32_t wc = useCode[j] >> 16, op = useCode[j] & 0xFFFFu;
        if (wc == 0 || j + wc > useWords) break;
        if (op == 22 && wc >= 3 && useCode[j + 2] == 16) has16f = true;
        if (op == 21 && wc >= 3 && useCode[j + 2] == 16) has16i = true;
        if (op == 54) break;
        j += wc;
    }
    std::vector<uint32_t> out;
    out.reserve(useWords);
    size_t j = 0; int dropped = 0;
    while (j < useWords) {
        const uint32_t wc = useCode[j] >> 16, op = useCode[j] & 0xFFFFu;
        if (wc == 0 || j + wc > useWords) { while (j < useWords) out.push_back(useCode[j++]); break; }
        if (op == 17 && wc >= 2) {
            const uint32_t cap = useCode[j + 1];
            if ((cap == 9 && !has16f) || (cap == 22 && !has16i)) { ++dropped; j += wc; continue; }
        }
        for (uint32_t t = 0; t < wc; ++t) out.push_back(useCode[j + t]);
        j += wc;
    }
    if (dropped > 0) {
        rewritten.swap(out);
        useCode = rewritten.data();
        useWords = rewritten.size();
    }
}



    // OpTerminateInvocation (4416) есть только в SPIR-V 1.6. После патча версии на 1.5
// модуль невалиден -> OpKill (252), тот же эффект во фрагментном шейдере.
{
    bool need = false;
    for (size_t j = 0; j < useWords;) {
        const uint32_t wc = useCode[j] >> 16, op = useCode[j] & 0xFFFFu;
        if (wc == 0 || j + wc > useWords) break;
        if (op == 4416) { need = true; break; }
        j += wc;
    }
    if (need) {
        std::vector<uint32_t> tk(useCode, useCode + useWords);
        int fixed = 0;
        for (size_t j = 0; j < tk.size();) {
            const uint32_t wc = tk[j] >> 16, op = tk[j] & 0xFFFFu;
            if (wc == 0 || j + wc > tk.size()) break;
            if (op == 4416) { tk[j] = (wc << 16) | 252u; ++fixed; }
            j += wc;
        }
        static volatile LONG tiN = 0;
        if (InterlockedIncrement(&tiN) <= 10)
            LogLine("SPIRV: OpTerminateInvocation -> OpKill x%d in shader #%ld", fixed, n);
        rewritten.swap(tk);
        useCode = rewritten.data();
        useWords = rewritten.size();
    }
}


    // Apply the known-safe same-width BDA pointer cast workaround before
    // the first driver call. Waiting for vkCreateShaderModule to fail is not
    // sufficient: some AMDVLK versions accept the module but hang later
    // while compiling its compute pipeline.
    std::vector<uint32_t> physicalPtrVariant;
    if (g_spvCfg.tryVariants) {
        uint32_t ptrCastsRewritten = 0;
        std::string ptrCastErr;
        if (spvrw::RewriteSameWidthPhysicalUToPtr(
                useCode, useWords, physicalPtrVariant, ptrCastsRewritten, ptrCastErr) &&
            ptrCastsRewritten > 0) {
            useCode = physicalPtrVariant.data();
            useWords = physicalPtrVariant.size();
            si.rewritten = true;
            static volatile LONG physicalPtrFixCount = 0;
            const LONG fixN = InterlockedIncrement(&physicalPtrFixCount);
            if (fixN <= 30 || fixN % 100 == 0) {
                LogLine("SPIRV: OpConvertUToPtr -> OpBitcast (same-width u64/PhysicalStorageBuffer) x%u shader #%ld",
                    ptrCastsRewritten, n);
            }
        } else if (!ptrCastErr.empty()) {
            static volatile LONG physicalPtrParseErrors = 0;
            const LONG errN = InterlockedIncrement(&physicalPtrParseErrors);
            if (errN <= 8)
                LogLine("SPIRV: physical pointer workaround skipped shader #%ld: %s", n, ptrCastErr.c_str());
        }
    }

    if (useCode && useWords >= 5)
        si.effectiveHash = Fnv1a64(useCode, useWords * sizeof(uint32_t));
    ci.pCode = useCode;
    ci.codeSize = useWords * 4;
    Crumb("vkCreateShaderModule #%ld caps=[%s]", n, si.caps.c_str());
    SetJob("vkCreateShaderModule #%ld size=%zu original=%zu source=%s caps=[%s]", n,
        useWords * sizeof(uint32_t), originalBytes, didReplace ? "spv_replace" : "original", si.caps.c_str());
    VkResult r = VK_ERROR_UNKNOWN;
    const bool callOk = DriverCall([&]() { r = real(device, &ci, pAllocator, pShaderModule); });
    if (!callOk) {
        LogLine("CAUGHT driver crash in vkCreateShaderModule #%ld (spv=%x size=%zu original=%zu source=%s caps=[%s])",
            n, si.version, useWords * sizeof(uint32_t), originalBytes, didReplace ? "spv_replace" : "original", si.caps.c_str());
        DumpSpirv("crash", n, useCode, useWords * 4);
        r = VK_ERROR_UNKNOWN;
    }
    // The old driver's SPIR-V front end crashes on some constructs (e.g. OpConvertUToPtr). Try equivalent
    // rewrites of the module before giving up on it.
    std::vector<uint32_t> variantCode;
    if (r != VK_SUCCESS && r != static_cast<VkResult>(-13) && g_spvCfg.tryVariants) {
        for (int v = 1; v <= 2 && r != VK_SUCCESS; ++v) {
            std::vector<uint32_t> var;
            std::string verr;
            if (!spvrw::ApplyVariant(v, useCode, useWords, var, verr)) continue;
            VkShaderModuleCreateInfo ci2 = ci;
            ci2.pCode = var.data();
            ci2.codeSize = var.size() * 4;
            SetJob("vkCreateShaderModule #%ld VARIANT %d caps=[%s]", n, v, si.caps.c_str());
            VkResult r2 = VK_ERROR_UNKNOWN;
            const bool ok2 = DriverCall([&]() { r2 = real(device, &ci2, pAllocator, pShaderModule); });
            if (ok2 && r2 == VK_SUCCESS) {
                r = VK_SUCCESS;
                variantCode = std::move(var);
                si.effectiveHash = Fnv1a64(variantCode.data(), variantCode.size() * sizeof(uint32_t));
                static volatile LONG worked = 0;
                const LONG k = InterlockedIncrement(&worked);
                if (k <= 10) LogLine("VARIANT %d WORKED for shader #%ld (caps=[%s]); total variant successes=%ld", v, n, si.caps.c_str(), (long)k);
            } else {
                LogLine("shader #%ld variant %d %s", n, v, ok2 ? "rejected by driver" : "CRASHED the driver again");
            }
        }
    }
    if (r != VK_SUCCESS) {
        LogLine("vkCreateShaderModule #%ld FAILED: result=%d caps=[%s] -> FAKE module", n, (int)r, si.caps.c_str());
        *pShaderModule = MakeFake<VkShaderModule>();
        PutSpvInfo(*pShaderModule, si);
        return VK_SUCCESS;
    }
    if (g_spvCfg.keepCode || (g_spvCfg.traceReplacementShaders && si.replacement) || IsActiveComputeDumpHash(si.originalHash)) {
        if (!variantCode.empty()) si.code = std::make_shared<std::vector<uint32_t>>(variantCode);
        else si.code = std::make_shared<std::vector<uint32_t>>(useCode, useCode + useWords);
    }
    PutSpvInfo(*pShaderModule, si);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    auto real = RealExport<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    LoadSpvConfig();
    VkPresentInfoKHR modifiedPresent{};
    const VkPresentInfoKHR* usePresent = pPresentInfo;
    VkSemaphore postSem = VK_NULL_HANDLE;
    if (g_spvCfg.brightness > 1.0f && pPresentInfo) {
        const VkSemaphore s = post::Process(queue, *pPresentInfo);
        if (s) {
            postSem = s;
            modifiedPresent = *pPresentInfo;
            modifiedPresent.waitSemaphoreCount = 1;
            modifiedPresent.pWaitSemaphores = &postSem;
            usePresent = &modifiedPresent;
        }
    }
    VkResult r = real(queue, usePresent);
    static bool firstPresent = true;
    if (firstPresent) {
        firstPresent = false;
        LoadSpvConfig();
        LogLine("vkQueuePresentKHR: first present done (result=%d)", (int)r);
        InstallCrashHandlerFirst();
        // Old debugging hack, now optional: VulkanProxy.ini [spirv] ExitAfterFirstPresent=1
        wchar_t ip[2048]{};
        GetLogPath(ip, sizeof(ip) / sizeof(ip[0]));
        wchar_t* sl = wcsrchr(ip, L'\\');
        if (sl) {
            wcscpy_s(sl + 1, (sizeof(ip) / sizeof(ip[0])) - static_cast<size_t>(sl + 1 - ip), L"VulkanProxy.ini");
            if (GetPrivateProfileIntW(L"spirv", L"ExitAfterFirstPresent", 0, ip)) {
                Sleep(500);
                ExitProcess(0);
            }
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// v5: диагностика instance / surface / ресурсов
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    auto real = RealExport<PFN_vkCreateInstance>("vkCreateInstance");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (pInfo) {
        LogLine("vkCreateInstance: apiVersion=%u.%u extCount=%u layerCount=%u",
            VK_API_VERSION_MAJOR(pInfo->pApplicationInfo ? pInfo->pApplicationInfo->apiVersion : 0),
            VK_API_VERSION_MINOR(pInfo->pApplicationInfo ? pInfo->pApplicationInfo->apiVersion : 0),
            pInfo->enabledExtensionCount, pInfo->enabledLayerCount);
        for (uint32_t i = 0; i < pInfo->enabledExtensionCount; ++i)
            LogLine("  INSTANCE EXT: %s", pInfo->ppEnabledExtensionNames[i]);
        for (uint32_t i = 0; i < pInfo->enabledLayerCount; ++i)
            LogLine("  INSTANCE LAYER: %s", pInfo->ppEnabledLayerNames[i]);
    }
    VkResult r = real(pInfo, pAllocator, pInstance);
    LogLine("vkCreateInstance result=%d", (int)r);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pInfo,
    const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    auto real = RealExport<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
    if (!real) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = real(instance, pInfo, pAllocator, pSurface);
    LogLine("vkCreateWin32SurfaceKHR result=%d (hwnd=%p)", (int)r, pInfo ? (void*)pInfo->hwnd : nullptr);
    return r;
}


// Safe no-ops for optional debug/utils functions some engines call without null-checking.
static VKAPI_ATTR VkResult VKAPI_CALL Stub_CreateDebugUtilsMessengerEXT(
    VkInstance, const void*, const VkAllocationCallbacks*, void** pMessenger) {
    if (pMessenger) *pMessenger = (void*)(uintptr_t)0x1;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL Stub_DestroyDebugUtilsMessengerEXT(VkInstance, void*, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL Stub_SubmitDebugUtilsMessageEXT(VkInstance, int, int, const void*) {}
static VKAPI_ATTR VkResult VKAPI_CALL Stub_SetDebugUtilsObjectNameEXT(VkDevice, const void*) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL Stub_SetDebugUtilsObjectTagEXT(VkDevice, const void*) { return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL Stub_CmdBeginDebugUtilsLabelEXT(VkCommandBuffer, const void*) {}
static VKAPI_ATTR void VKAPI_CALL Stub_CmdEndDebugUtilsLabelEXT(VkCommandBuffer) {}
static VKAPI_ATTR void VKAPI_CALL Stub_CmdInsertDebugUtilsLabelEXT(VkCommandBuffer, const void*) {}
static VKAPI_ATTR void VKAPI_CALL Stub_QueueBeginDebugUtilsLabelEXT(VkQueue, const void*) {}
static VKAPI_ATTR void VKAPI_CALL Stub_QueueEndDebugUtilsLabelEXT(VkQueue) {}
static VKAPI_ATTR void VKAPI_CALL Stub_QueueInsertDebugUtilsLabelEXT(VkQueue, const void*) {}
static VKAPI_ATTR VkResult VKAPI_CALL Stub_CreateDebugReportCallbackEXT(VkInstance, const void*, const VkAllocationCallbacks*, void** p) {
    if (p) *p = (void*)(uintptr_t)0x1; return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL Stub_DestroyDebugReportCallbackEXT(VkInstance, void*, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL Stub_DebugReportMessageEXT(VkInstance, int, int, uint64_t, size_t, int32_t, const char*, const char*) {}

// vkDestroySurfaceKHR / vkGetPhysicalDeviceSurfaceSupportKHR /
// vkGetPhysicalDeviceFormatProperties2 are provided by generated thunks
// (vk_proxy_thunks.generated.asm). Do NOT redefine them here (LNK2005).
// They are served to the game via GIPA by looking them up in our own module.

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pCaps) {
    auto real = RealExport<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!real) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = real(pd, surface, pCaps);
    if (r == VK_SUCCESS && pCaps) {
        LogLine("vkGetPhysicalDeviceSurfaceCapabilitiesKHR: minImg=%u maxImg=%u extent=%ux%u curTransform=%d maxLayers=%u",
            pCaps->minImageCount, pCaps->maxImageCount,
            pCaps->currentExtent.width, pCaps->currentExtent.height,
            (int)pCaps->currentTransform, pCaps->maxImageArrayLayers);
    } else {
        LogLine("vkGetPhysicalDeviceSurfaceCapabilitiesKHR result=%d", (int)r);
    }
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
    uint32_t* pCount, VkSurfaceFormatKHR* pFormats) {
    auto real = RealExport<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!real) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!pCount) return real(pd, surface, pCount, pFormats);

    // Спросим у драйвера реальный список.
    uint32_t driverCount = 0;
    VkResult r = real(pd, surface, &driverCount, nullptr);
    if (r != VK_SUCCESS || driverCount > 256) return r;
    std::vector<VkSurfaceFormatKHR> driverFormats(driverCount);
    if (driverCount) {
        r = real(pd, surface, &driverCount, driverFormats.data());
        if (r != VK_SUCCESS) return r;
        driverFormats.resize(driverCount);
    }

    // Оставляем только безопасные форматы (B8G8R8A8 и R8G8B8A8, UNORM/SRGB).
    std::vector<VkSurfaceFormatKHR> safe;
    auto pushUnique = [&](VkFormat f, VkColorSpaceKHR cs) {
        for (auto& s : safe) if (s.format == f && s.colorSpace == cs) return;
        VkSurfaceFormatKHR x{}; x.format = f; x.colorSpace = cs; safe.push_back(x);
    };
    // Сначала — то, что реально поддерживает драйвер и что входит в safe set.
    for (auto& f : driverFormats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB ||
             f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            pushUnique(f.format, f.colorSpace);
        }
    }
    // Если ни одного safe не нашлось — хотя бы SRGB_NONLINEAR + B8G8R8A8_UNORM.
    if (safe.empty()) pushUnique(VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

    static volatile LONG logged = 0;
    if (InterlockedExchange(&logged, 1) == 0) {
        LogLine("vkGetPhysicalDeviceSurfaceFormatsKHR: driver had %u formats, returning %u safe",
            driverCount, (unsigned)safe.size());
        for (auto& f : driverFormats) LogLine("  DRIVER FMT: format=%d colorspace=%d", (int)f.format, (int)f.colorSpace);
        for (auto& f : safe) LogLine("  SAFE   FMT: format=%d colorspace=%d", (int)f.format, (int)f.colorSpace);
    }

    const uint32_t total = (uint32_t)safe.size();
    if (!pFormats) { *pCount = total; return VK_SUCCESS; }
    const uint32_t cap = *pCount;
    const uint32_t n = cap < total ? cap : total;
    for (uint32_t i = 0; i < n; ++i) pFormats[i] = safe[i];
    *pCount = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
    uint32_t* pCount, VkPresentModeKHR* pModes) {
    auto real = RealExport<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!real) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult r = real(pd, surface, pCount, pModes);
    // First call is the count query: pModes == nullptr. Never dereference it then.
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && pCount && pModes) {
        static volatile LONG logged = 0;
        if (InterlockedExchange(&logged, 1) == 0) {
            LogLine("vkGetPhysicalDeviceSurfacePresentModesKHR: count=%u", *pCount);
            for (uint32_t i = 0; i < *pCount && i < 16; ++i)
                LogLine("  PRESENT MODE: %d", (int)pModes[i]);
        }
    }
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    static volatile LONG cnt = 0;
    if (pInfo && InterlockedIncrement(&cnt) <= 60) {
        LogLine("vkCreateImage ENTER: format=%d %ux%ux%u mip=%u layers=%u usage=0x%X tiling=%d",
            (int)pInfo->format, pInfo->extent.width, pInfo->extent.height, pInfo->extent.depth,
            pInfo->mipLevels, pInfo->arrayLayers, (unsigned)pInfo->usage, (int)pInfo->tiling);
    }
    auto real = RealExport<PFN_vkCreateImage>("vkCreateImage");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pImage);
    if (r != VK_SUCCESS) {
        LogLine("vkCreateImage FAILED: result=%d format=%d type=%d %ux%ux%u mip=%u layers=%u samples=%u usage=0x%X tiling=%d",
            (int)r,
            pInfo ? (int)pInfo->format : -1,
            pInfo ? (int)pInfo->imageType : -1,
            pInfo ? pInfo->extent.width : 0,
            pInfo ? pInfo->extent.height : 0,
            pInfo ? pInfo->extent.depth : 0,
            pInfo ? pInfo->mipLevels : 0,
            pInfo ? pInfo->arrayLayers : 0,
            pInfo ? (unsigned)pInfo->samples : 0,
            pInfo ? (unsigned)pInfo->usage : 0,
            pInfo ? (int)pInfo->tiling : -1);
    } else {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 40) {
            LogLine("vkCreateImage: format=%d %ux%ux%u mip=%u layers=%u usage=0x%X tiling=%d mem=0x%X",
                pInfo ? (int)pInfo->format : -1,
                pInfo ? pInfo->extent.width : 0,
                pInfo ? pInfo->extent.height : 0,
                pInfo ? pInfo->extent.depth : 0,
                pInfo ? pInfo->mipLevels : 0,
                pInfo ? pInfo->arrayLayers : 0,
                pInfo ? (unsigned)pInfo->usage : 0,
                pInfo ? (int)pInfo->tiling : -1,
                pInfo ? (unsigned)pInfo->sharingMode : 0);
        }
    }
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pMemReq) {
    auto real = RealExport<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    if (real) real(device, image, pMemReq);
    if (pMemReq) LogLine("vkGetImageMemoryRequirements: size=%llu align=%llu memTypeBits=0x%X",
        (unsigned long long)pMemReq->size, (unsigned long long)pMemReq->alignment, pMemReq->memoryTypeBits);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemReq) {
    auto real = RealExport<PFN_vkGetImageMemoryRequirements2>("vkGetImageMemoryRequirements2");
    if (real) real(device, pInfo, pMemReq);
    if (pMemReq) LogLine("vkGetImageMemoryRequirements2: size=%llu align=%llu memTypeBits=0x%X",
        (unsigned long long)pMemReq->memoryRequirements.size,
        (unsigned long long)pMemReq->memoryRequirements.alignment,
        pMemReq->memoryRequirements.memoryTypeBits);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
    auto real = RealExport<PFN_vkAllocateMemory>("vkAllocateMemory");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pMemory);
    if (r != VK_SUCCESS) {
        LogLine("vkAllocateMemory FAILED: result=%d size=%llu typeIdx=%u",
            (int)r, pInfo ? (unsigned long long)pInfo->allocationSize : 0,
            pInfo ? pInfo->memoryTypeIndex : 0);
    } else {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 30) {
            LogLine("vkAllocateMemory: size=%llu typeIdx=%u",
                pInfo ? (unsigned long long)pInfo->allocationSize : 0,
                pInfo ? pInfo->memoryTypeIndex : 0);
        }
    }
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset) {
    auto real = RealExport<PFN_vkBindImageMemory>("vkBindImageMemory");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (!image || !memory || IsFake(image) || IsFake(memory)) {
        LogLine("vkBindImageMemory: refusing null/FAKE handle image=%p memory=%p", (void*)image, (void*)memory);
        return VK_ERROR_DEVICE_LOST;
    }
    SetJob("vkBindImageMemory device=%p image=%p memory=%p offset=%llu",
        (void*)device, (void*)image, (void*)memory, (unsigned long long)offset);
    VkResult r = VK_ERROR_UNKNOWN;
    if (!DriverCallResource([&]() { r = real(device, image, memory, offset); })) {
        LogLine("CAUGHT driver crash in vkBindImageMemory image=%p memory=%p offset=%llu",
            (void*)image, (void*)memory, (unsigned long long)offset);
        return VK_ERROR_DEVICE_LOST;
    }
    if (r != VK_SUCCESS) LogLine("vkBindImageMemory FAILED: result=%d offset=%llu", (int)r, (unsigned long long)offset);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory2(VkDevice device, uint32_t bindCount, const VkBindImageMemoryInfo* pBinds) {
    auto real = RealExport<PFN_vkBindImageMemory2>("vkBindImageMemory2");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (bindCount == 0) return VK_SUCCESS;
    if (!pBinds) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < bindCount; ++i) {
        if (!pBinds[i].image || !pBinds[i].memory || IsFake(pBinds[i].image) || IsFake(pBinds[i].memory)) {
            LogLine("vkBindImageMemory2: refusing invalid bind[%u] image=%p memory=%p",
                i, (void*)pBinds[i].image, (void*)pBinds[i].memory);
            return VK_ERROR_DEVICE_LOST;
        }
    }
    SetJob("vkBindImageMemory2 device=%p count=%u", (void*)device, bindCount);
    VkResult r = VK_ERROR_UNKNOWN;
    if (!DriverCallResource([&]() { r = real(device, bindCount, pBinds); })) {
        LogLine("CAUGHT driver crash in vkBindImageMemory2 count=%u", bindCount);
        return VK_ERROR_DEVICE_LOST;
    }
    if (r != VK_SUCCESS) LogLine("vkBindImageMemory2 FAILED: result=%d count=%u", (int)r, bindCount);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer) {
    auto real = RealExport<PFN_vkCreateBuffer>("vkCreateBuffer");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pBuffer);
    if (r != VK_SUCCESS) {
        LogLine("vkCreateBuffer FAILED: result=%d size=%llu usage=0x%X",
            (int)r, pInfo ? (unsigned long long)pInfo->size : 0, pInfo ? (unsigned)pInfo->usage : 0);
    } else {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 30) {
            LogLine("vkCreateBuffer: size=%llu usage=0x%X",
                pInfo ? (unsigned long long)pInfo->size : 0, pInfo ? (unsigned)pInfo->usage : 0);
        }
    }
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemReq) {
    auto real = RealExport<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements");
    if (real) real(device, buffer, pMemReq);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemReq) {
    auto real = RealExport<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2");
    if (real) real(device, pInfo, pMemReq);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    auto real = RealExport<PFN_vkBindBufferMemory>("vkBindBufferMemory");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (!buffer || !memory || IsFake(buffer) || IsFake(memory)) {
        LogLine("vkBindBufferMemory: refusing null/FAKE handle buffer=%p memory=%p", (void*)buffer, (void*)memory);
        return VK_ERROR_DEVICE_LOST;
    }
    SetJob("vkBindBufferMemory device=%p buffer=%p memory=%p offset=%llu",
        (void*)device, (void*)buffer, (void*)memory, (unsigned long long)offset);
    VkResult r = VK_ERROR_UNKNOWN;
    if (!DriverCallResource([&]() { r = real(device, buffer, memory, offset); })) {
        LogLine("CAUGHT driver crash in vkBindBufferMemory buffer=%p memory=%p offset=%llu",
            (void*)buffer, (void*)memory, (unsigned long long)offset);
        return VK_ERROR_DEVICE_LOST;
    }
    if (r != VK_SUCCESS) LogLine("vkBindBufferMemory FAILED: result=%d offset=%llu", (int)r, (unsigned long long)offset);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory2(VkDevice device, uint32_t bindCount, const VkBindBufferMemoryInfo* pBinds) {
    auto real = RealExport<PFN_vkBindBufferMemory2>("vkBindBufferMemory2");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (bindCount == 0) return VK_SUCCESS;
    if (!pBinds) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < bindCount; ++i) {
        if (!pBinds[i].buffer || !pBinds[i].memory || IsFake(pBinds[i].buffer) || IsFake(pBinds[i].memory)) {
            LogLine("vkBindBufferMemory2: refusing invalid bind[%u] buffer=%p memory=%p",
                i, (void*)pBinds[i].buffer, (void*)pBinds[i].memory);
            return VK_ERROR_DEVICE_LOST;
        }
    }
    SetJob("vkBindBufferMemory2 device=%p count=%u", (void*)device, bindCount);
    VkResult r = VK_ERROR_UNKNOWN;
    if (!DriverCallResource([&]() { r = real(device, bindCount, pBinds); })) {
        LogLine("CAUGHT driver crash in vkBindBufferMemory2 count=%u", bindCount);
        return VK_ERROR_DEVICE_LOST;
    }
    if (r != VK_SUCCESS) LogLine("vkBindBufferMemory2 FAILED: result=%d count=%u", (int)r, bindCount);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout) {
    auto real = RealExport<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pSetLayout);
    if (r != VK_SUCCESS) {
        LogLine("vkCreateDescriptorSetLayout FAILED: result=%d bindingCount=%u flags=0x%X",
            (int)r, pInfo ? pInfo->bindingCount : 0, pInfo ? (unsigned)pInfo->flags : 0);
        if (pInfo) for (uint32_t i = 0; i < pInfo->bindingCount && i < 32; ++i)
            LogLine("  BINDING: %u type=%d count=%u stages=0x%X",
                pInfo->pBindings[i].binding, (int)pInfo->pBindings[i].descriptorType,
                pInfo->pBindings[i].descriptorCount, (unsigned)pInfo->pBindings[i].stageFlags);
    } else {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 10)
            LogLine("vkCreateDescriptorSetLayout: bindingCount=%u", pInfo ? pInfo->bindingCount : 0);
    }
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout) {
    auto real = RealExport<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pPipelineLayout);
    if (r != VK_SUCCESS) {
        LogLine("vkCreatePipelineLayout FAILED: result=%d setLayouts=%u pushRanges=%u",
            (int)r, pInfo ? pInfo->setLayoutCount : 0, pInfo ? pInfo->pushConstantRangeCount : 0);
    } else {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 10)
            LogLine("vkCreatePipelineLayout: setLayouts=%u pushRanges=%u",
                pInfo ? pInfo->setLayoutCount : 0, pInfo ? pInfo->pushConstantRangeCount : 0);
    }
    return r;
}

// ---------------------------------------------------------------------------
// v6: расширенная локализация краша
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
    if (pInfo) {
        LogLine("vkCreateSampler ENTER: magFilter=%d minFilter=%d mipmap=%d addrU/V/W=%d/%d/%d aniso=%f compare=%u minLod=%f maxLod=%f",
            (int)pInfo->magFilter, (int)pInfo->minFilter, (int)pInfo->mipmapMode,
            (int)pInfo->addressModeU, (int)pInfo->addressModeV, (int)pInfo->addressModeW,
            pInfo->maxAnisotropy, (unsigned)pInfo->compareEnable,
            pInfo->minLod, pInfo->maxLod);
    }
    auto real = RealExport<PFN_vkCreateSampler>("vkCreateSampler");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pSampler);
    if (r != VK_SUCCESS) LogLine("vkCreateSampler FAILED: result=%d", (int)r);
    else LogLine("vkCreateSampler OK");
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool) {
    if (pInfo) {
        LogLine("vkCreateDescriptorPool ENTER: maxSets=%u poolSizes=%u flags=0x%X",
            pInfo->maxSets, pInfo->poolSizeCount, (unsigned)pInfo->flags);
    }
    auto real = RealExport<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pDescriptorPool);
    if (r != VK_SUCCESS) LogLine("vkCreateDescriptorPool FAILED: result=%d", (int)r);
    else LogLine("vkCreateDescriptorPool OK");
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pInfo,
    VkDescriptorSet* pDescriptorSets) {
    if (pInfo) LogLine("vkAllocateDescriptorSets ENTER: count=%u", pInfo->descriptorSetCount);
    auto real = RealExport<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pDescriptorSets);
    if (r == VK_SUCCESS && pInfo && pDescriptorSets)
        TraceResetAllocatedDescriptorSets(pInfo->descriptorSetCount, pDescriptorSets);
    if (r != VK_SUCCESS) LogLine("vkAllocateDescriptorSets FAILED: result=%d", (int)r);
    else LogLine("vkAllocateDescriptorSets OK");
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet* pWrites,
    uint32_t copyCount, const VkCopyDescriptorSet* pCopies) {
    static volatile LONG cnt = 0;
    if (InterlockedIncrement(&cnt) <= 20)
        LogLine("vkUpdateDescriptorSets ENTER: writes=%u copies=%u", writeCount, copyCount);
    auto real = RealExport<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    if (!real) return;

    // The old driver does not know acceleration-structure descriptors (ray tracing): it treats them as
    // images and dereferences the NULL pImageInfo (crash at READ 0x0 in amdvlk64). Drop such writes, and
    // any write whose mandatory info pointer / destination set is NULL.
    auto shouldDrop = [](const VkWriteDescriptorSet& w) -> bool {
        const int t = static_cast<int>(w.descriptorType);
        if (t == 1000150000 || t == 1000165000) return true;       // ACCELERATION_STRUCTURE_KHR / _NV
        if (!w.dstSet) return true;
        if (w.descriptorCount == 0) return false;
        switch (w.descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return w.pImageInfo == nullptr;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return w.pBufferInfo == nullptr;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return w.pTexelBufferView == nullptr;
        default: return false;
        }
    };
    bool anyDrop = false;
    for (uint32_t i = 0; i < writeCount && pWrites; ++i) if (shouldDrop(pWrites[i])) { anyDrop = true; break; }
    if (!anyDrop) {
        TraceDescriptorUpdates(writeCount, pWrites, copyCount, pCopies);
        real(device, writeCount, pWrites, copyCount, pCopies);
        return;
    }

    std::vector<VkWriteDescriptorSet> kept;
    kept.reserve(writeCount);
    for (uint32_t i = 0; i < writeCount; ++i) {
        if (shouldDrop(pWrites[i])) {
            char key[64];
            _snprintf_s(key, sizeof(key), _TRUNCATE, "dropdesc:%d", static_cast<int>(pWrites[i].descriptorType));
            if (FirstTime(key))
                LogLine("vkUpdateDescriptorSets: DROPPING writes of descriptor type %d (unsupported by driver / NULL data)",
                    static_cast<int>(pWrites[i].descriptorType));
            continue;
        }
        kept.push_back(pWrites[i]);
    }
    TraceDescriptorUpdates(static_cast<uint32_t>(kept.size()), kept.empty() ? nullptr : kept.data(), copyCount, pCopies);
    real(device, static_cast<uint32_t>(kept.size()), kept.empty() ? nullptr : kept.data(), copyCount, pCopies);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Trace_vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pTemplate) {
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 64 || (id % 512) == 0);
    if (trace) LogLine("SPVTRACE API CREATE_TEMPLATE ENTER id=%ld device=%p entries=%u templateType=%d set=%u",
        (long)id, (void*)device, pInfo ? pInfo->descriptorUpdateEntryCount : 0u,
        pInfo ? (int)pInfo->templateType : -1, pInfo ? pInfo->set : 0u);

    auto real = RealExport<PFN_vkCreateDescriptorUpdateTemplate>("vkCreateDescriptorUpdateTemplate");
    if (!real) real = RealExport<PFN_vkCreateDescriptorUpdateTemplate>("vkCreateDescriptorUpdateTemplateKHR");
    if (!real) {
        LogLine("SPVTRACE API CREATE_TEMPLATE NO_DRIVER_ENTRY id=%ld", (long)id);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult r = real(device, pInfo, pAllocator, pTemplate);
    if (r == VK_SUCCESS && pInfo && pTemplate && *pTemplate)
        TraceRegisterDescriptorUpdateTemplate(*pTemplate, pInfo);
    if (trace || r != VK_SUCCESS)
        LogLine("SPVTRACE API CREATE_TEMPLATE EXIT id=%ld result=%d template=%p",
            (long)id, (int)r, (void*)(pTemplate ? (uintptr_t)*pTemplate : 0));
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
Trace_vkDestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate updateTemplate,
    const VkAllocationCallbacks* pAllocator) {
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 64 || (id % 512) == 0);
    if (trace) LogLine("SPVTRACE API DESTROY_TEMPLATE ENTER id=%ld device=%p template=%p",
        (long)id, (void*)device, (void*)(uintptr_t)updateTemplate);

    TraceForgetDescriptorUpdateTemplate(updateTemplate);
    auto real = RealExport<PFN_vkDestroyDescriptorUpdateTemplate>("vkDestroyDescriptorUpdateTemplate");
    if (!real) real = RealExport<PFN_vkDestroyDescriptorUpdateTemplate>("vkDestroyDescriptorUpdateTemplateKHR");
    if (real) {
        real(device, updateTemplate, pAllocator);
        if (trace) LogLine("SPVTRACE API DESTROY_TEMPLATE EXIT id=%ld forwarded=1", (long)id);
    } else {
        LogLine("SPVTRACE API DESTROY_TEMPLATE NO_DRIVER_ENTRY id=%ld", (long)id);
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL
Trace_vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate updateTemplate, const void* pData) {
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 128 || (id % 512) == 0);
    if (trace) LogLine("SPVTRACE API UPDATE_TEMPLATE ENTER id=%ld device=%p set=%p template=%p data=%p",
        (long)id, (void*)device, (void*)(uintptr_t)descriptorSet,
        (void*)(uintptr_t)updateTemplate, pData);

    TraceDescriptorTemplateUpdate(descriptorSet, updateTemplate, pData);
    auto real = RealExport<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplate");
    if (!real) real = RealExport<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplateKHR");
    if (real) {
        real(device, descriptorSet, updateTemplate, pData);
        if (trace) LogLine("SPVTRACE API UPDATE_TEMPLATE EXIT id=%ld forwarded=1", (long)id);
    } else {
        LogLine("SPVTRACE API UPDATE_TEMPLATE NO_DRIVER_ENTRY id=%ld", (long)id);
    }
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool) {
    if (pInfo) LogLine("vkCreateCommandPool ENTER: queueFamilyIdx=%u flags=0x%X",
        pInfo->queueFamilyIndex, (unsigned)pInfo->flags);
    auto real = RealExport<PFN_vkCreateCommandPool>("vkCreateCommandPool");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pCommandPool);
    if (r != VK_SUCCESS) LogLine("vkCreateCommandPool FAILED: result=%d", (int)r);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice device, const VkFenceCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkFence* pFence) {
    auto real = RealExport<PFN_vkCreateFence>("vkCreateFence");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pFence);
    if (r != VK_SUCCESS) LogLine("vkCreateFence FAILED: result=%d", (int)r);
    return r;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    static volatile LONG cnt = 0;
    if (InterlockedIncrement(&cnt) <= 5 && pInfo) {
        LogLine("vkCreateRenderPass ENTER: attachments=%u subpasses=%u deps=%u",
            pInfo->attachmentCount, pInfo->subpassCount, pInfo->dependencyCount);
    }
    auto real = RealExport<PFN_vkCreateRenderPass>("vkCreateRenderPass");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pAllocator, pRenderPass);
    if (r != VK_SUCCESS) LogLine("vkCreateRenderPass FAILED: result=%d", (int)r);
    return r;
}



    // -----------------------------------------------------------------------
    // RT stubs (unchanged from v2, except vkCmdBindPipeline/vkDestroyPipeline
    // hooks are removed - they are now C++ exports below).
    // -----------------------------------------------------------------------
    namespace stub {
        VKAPI_ATTR VkResult VKAPI_CALL CreateAS(VkDevice, const VkAccelerationStructureCreateInfoKHR*,
            const VkAllocationCallbacks*, VkAccelerationStructureKHR* p) {
            if (!p) return VK_ERROR_INITIALIZATION_FAILED;
            *p = MakeFake<VkAccelerationStructureKHR>();
            return VK_SUCCESS;
        }
        VKAPI_ATTR void VKAPI_CALL DestroyAS(VkDevice, VkAccelerationStructureKHR, const VkAllocationCallbacks*) {}
        VKAPI_ATTR void VKAPI_CALL CmdBuildAS(VkCommandBuffer, uint32_t,
            const VkAccelerationStructureBuildGeometryInfoKHR*, const VkAccelerationStructureBuildRangeInfoKHR* const*) {}
        VKAPI_ATTR void VKAPI_CALL CmdBuildASIndirect(VkCommandBuffer, uint32_t,
            const VkAccelerationStructureBuildGeometryInfoKHR*, const VkDeviceAddress*, const uint32_t*,
            const uint32_t* const*) {}
        VKAPI_ATTR VkResult VKAPI_CALL BuildAS(VkDevice, VkDeferredOperationKHR, uint32_t,
            const VkAccelerationStructureBuildGeometryInfoKHR*, const VkAccelerationStructureBuildRangeInfoKHR* const*) {
            return VK_SUCCESS;
        }
        VKAPI_ATTR VkResult VKAPI_CALL CopyAS(VkDevice, VkDeferredOperationKHR, const VkCopyAccelerationStructureInfoKHR*) { return VK_SUCCESS; }
        VKAPI_ATTR VkResult VKAPI_CALL CopyASToMem(VkDevice, VkDeferredOperationKHR, const VkCopyAccelerationStructureToMemoryInfoKHR*) { return VK_SUCCESS; }
        VKAPI_ATTR VkResult VKAPI_CALL CopyMemToAS(VkDevice, VkDeferredOperationKHR, const VkCopyMemoryToAccelerationStructureInfoKHR*) { return VK_SUCCESS; }
        VKAPI_ATTR VkResult VKAPI_CALL WriteASProps(VkDevice, uint32_t, const VkAccelerationStructureKHR*,
            VkQueryType, size_t dataSize, void* pData, size_t) {
            if (pData && dataSize) memset(pData, 0, dataSize);
            return VK_SUCCESS;
        }
        VKAPI_ATTR void VKAPI_CALL CmdCopyAS(VkCommandBuffer, const VkCopyAccelerationStructureInfoKHR*) {}
        VKAPI_ATTR void VKAPI_CALL CmdCopyASToMem(VkCommandBuffer, const VkCopyAccelerationStructureToMemoryInfoKHR*) {}
        VKAPI_ATTR void VKAPI_CALL CmdCopyMemToAS(VkCommandBuffer, const VkCopyMemoryToAccelerationStructureInfoKHR*) {}
        VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetASAddr(VkDevice, const VkAccelerationStructureDeviceAddressInfoKHR*) {
            static volatile LONG64 n = 0;
            return 0x200000000ull + static_cast<uint64_t>(InterlockedIncrement64(&n)) * 256ull;
        }
        VKAPI_ATTR void VKAPI_CALL CmdWriteASProps(VkCommandBuffer, uint32_t, const VkAccelerationStructureKHR*,
            VkQueryType, VkQueryPool, uint32_t) {}
        VKAPI_ATTR void VKAPI_CALL GetASCompat(VkDevice, const VkAccelerationStructureVersionInfoKHR*,
            VkAccelerationStructureCompatibilityKHR* p) {
            if (p) *p = VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR;
        }
        VKAPI_ATTR void VKAPI_CALL GetASBuildSizes(VkDevice, VkAccelerationStructureBuildTypeKHR,
            const VkAccelerationStructureBuildGeometryInfoKHR*, const uint32_t*,
            VkAccelerationStructureBuildSizesInfoKHR* s) {
            if (!s) return;
            s->accelerationStructureSize = 256;
            s->updateScratchSize = 256;
            s->buildScratchSize = 256;
        }
        VKAPI_ATTR void VKAPI_CALL CmdTraceRays(VkCommandBuffer, const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*, const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*, uint32_t, uint32_t, uint32_t) {}
        VKAPI_ATTR void VKAPI_CALL CmdTraceRaysIndirect(VkCommandBuffer, const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*, const VkStridedDeviceAddressRegionKHR*,
            const VkStridedDeviceAddressRegionKHR*, VkDeviceAddress) {}
        VKAPI_ATTR void VKAPI_CALL CmdTraceRaysIndirect2(VkCommandBuffer, VkDeviceAddress) {}
        VKAPI_ATTR VkResult VKAPI_CALL CreateRTPipelines(VkDevice, VkDeferredOperationKHR, VkPipelineCache,
            uint32_t count, const VkRayTracingPipelineCreateInfoKHR*, const VkAllocationCallbacks*, VkPipeline* p) {
            if (p) for (uint32_t i = 0; i < count; ++i) p[i] = MakeFake<VkPipeline>();
            return VK_SUCCESS;
        }
        VKAPI_ATTR VkResult VKAPI_CALL GetRTHandles(VkDevice, VkPipeline, uint32_t, uint32_t,
            size_t dataSize, void* pData) {
            if (pData && dataSize) memset(pData, 0, dataSize);
            return VK_SUCCESS;
        }
        VKAPI_ATTR VkDeviceSize VKAPI_CALL GetRTStack(VkDevice, VkPipeline, uint32_t, VkShaderGroupShaderKHR) { return 0; }
        VKAPI_ATTR void VKAPI_CALL CmdSetRTStack(VkCommandBuffer, uint32_t) {}

        VKAPI_ATTR VkResult VKAPI_CALL CreateDefOp(VkDevice, const VkAllocationCallbacks*, VkDeferredOperationKHR* p) {
            if (!p) return VK_ERROR_INITIALIZATION_FAILED;
            *p = MakeFake<VkDeferredOperationKHR>();
            return VK_SUCCESS;
        }
        VKAPI_ATTR void VKAPI_CALL DestroyDefOp(VkDevice, VkDeferredOperationKHR, const VkAllocationCallbacks*) {}
        VKAPI_ATTR uint32_t VKAPI_CALL DefOpConcurrency(VkDevice, VkDeferredOperationKHR) { return 1; }
        VKAPI_ATTR VkResult VKAPI_CALL DefOpResult(VkDevice, VkDeferredOperationKHR) { return VK_SUCCESS; }
        VKAPI_ATTR VkResult VKAPI_CALL DefOpJoin(VkDevice, VkDeferredOperationKHR) { return VK_SUCCESS; }

        struct Entry { const char* name; PFN_vkVoidFunction fn; };
#define VKSTUB(n, f) { n, reinterpret_cast<PFN_vkVoidFunction>(&f) }

        // Always-active: dynamic rendering emulation, memory-requirements stubs,
        // and the pipeline bind/destroy hooks (which used to be RT-only).
        const Entry kAlwaysTable[] = {
            VKSTUB("vkCreateGraphicsPipelines", vkCreateGraphicsPipelines),
			VKSTUB("vkCreateComputePipelines",  vkCreateComputePipelines),
            VKSTUB("vkDestroyPipeline",         vkDestroyPipeline),
            VKSTUB("vkCmdBindPipeline",         vkCmdBindPipeline),
            // draws / dispatches: a game that loads functions through vkGet*ProcAddr would otherwise call the
            // driver directly and crash on a FAKE or NULL pipeline
            VKSTUB("vkCmdDraw",                         vkCmdDraw),
            VKSTUB("vkCmdDrawIndexed",                  vkCmdDrawIndexed),
            VKSTUB("vkCmdDrawIndirect",                 vkCmdDrawIndirect),
            VKSTUB("vkCmdDrawIndexedIndirect",          vkCmdDrawIndexedIndirect),
            VKSTUB("vkCmdDrawIndirectCount",            vkCmdDrawIndirectCount),
            VKSTUB("vkCmdDrawIndirectCountKHR",         vkCmdDrawIndirectCount),
            VKSTUB("vkCmdDrawIndexedIndirectCount",     vkCmdDrawIndexedIndirectCount),
            VKSTUB("vkCmdDrawIndexedIndirectCountKHR",  vkCmdDrawIndexedIndirectCount),
            VKSTUB("vkCmdDispatch",                     vkCmdDispatch),
            VKSTUB("vkCmdDispatchIndirect",             vkCmdDispatchIndirect),
            VKSTUB("vkCmdBeginRendering",       vkCmdBeginRendering),
            VKSTUB("vkCmdBeginRenderingKHR",    vkCmdBeginRenderingKHR),
            VKSTUB("vkCmdEndRendering",         vkCmdEndRendering),
            VKSTUB("vkCmdEndRenderingKHR",      vkCmdEndRenderingKHR),
            VKSTUB("vkCreateImageView",         vkCreateImageView),
            VKSTUB("vkDestroyImageView",        vkDestroyImageView),
            VKSTUB("vkAllocateCommandBuffers",  vkAllocateCommandBuffers),
            VKSTUB("vkFreeCommandBuffers",      vkFreeCommandBuffers),
            VKSTUB("vkGetDeviceBufferMemoryRequirements",         vkGetDeviceBufferMemoryRequirements),
            VKSTUB("vkGetDeviceImageMemoryRequirements",          vkGetDeviceImageMemoryRequirements),
            VKSTUB("vkGetDeviceImageSparseMemoryRequirements",    vkGetDeviceImageSparseMemoryRequirements),
			            // v4: sync2 stubs (always active - translate to sync1)
            VKSTUB("vkQueueSubmit2",           vkQueueSubmit2),
            VKSTUB("vkQueueSubmit2KHR",        vkQueueSubmit2KHR),
            VKSTUB("vkCmdPipelineBarrier2",    vkCmdPipelineBarrier2),
            VKSTUB("vkCmdPipelineBarrier2KHR", vkCmdPipelineBarrier2KHR),
            VKSTUB("vkCmdWriteTimestamp2",     vkCmdWriteTimestamp2),
            VKSTUB("vkCmdWriteTimestamp2KHR",  vkCmdWriteTimestamp2KHR),
        VKSTUB("vkCmdCopyBuffer2", vkCmdCopyBuffer2),           VKSTUB("vkCmdCopyBuffer2KHR", vkCmdCopyBuffer2KHR),
        VKSTUB("vkCmdCopyImage2", vkCmdCopyImage2),             VKSTUB("vkCmdCopyImage2KHR", vkCmdCopyImage2KHR),
        VKSTUB("vkCmdCopyBufferToImage2", vkCmdCopyBufferToImage2), VKSTUB("vkCmdCopyBufferToImage2KHR", vkCmdCopyBufferToImage2KHR),
        VKSTUB("vkCmdCopyImageToBuffer2", vkCmdCopyImageToBuffer2), VKSTUB("vkCmdCopyImageToBuffer2KHR", vkCmdCopyImageToBuffer2KHR),
        VKSTUB("vkCmdBlitImage2", vkCmdBlitImage2),             VKSTUB("vkCmdBlitImage2KHR", vkCmdBlitImage2KHR),
        VKSTUB("vkCmdResolveImage2", vkCmdResolveImage2),       VKSTUB("vkCmdResolveImage2KHR", vkCmdResolveImage2KHR),
        VKSTUB("vkCmdSetEvent2", vkCmdSetEvent2),               VKSTUB("vkCmdSetEvent2KHR", vkCmdSetEvent2KHR),
        VKSTUB("vkCmdResetEvent2", vkCmdResetEvent2),           VKSTUB("vkCmdResetEvent2KHR", vkCmdResetEvent2KHR),
        VKSTUB("vkCmdWaitEvents2", vkCmdWaitEvents2),           VKSTUB("vkCmdWaitEvents2KHR", vkCmdWaitEvents2KHR),
        VKSTUB("vkCmdBindVertexBuffers2", vkCmdBindVertexBuffers2),
        VKSTUB("vkCmdPipelineBarrier", vkCmdPipelineBarrier),
        VKSTUB("vkCreatePrivateDataSlot", vkCreatePrivateDataSlot), VKSTUB("vkDestroyPrivateDataSlot", vkDestroyPrivateDataSlot),
        VKSTUB("vkSetPrivateData", vkSetPrivateData),           VKSTUB("vkGetPrivateData", vkGetPrivateData),
            // v4: diagnostic wrappers
            VKSTUB("vkCreateSwapchainKHR",     vkCreateSwapchainKHR),
            VKSTUB("vkCreateShaderModule",     vkCreateShaderModule),
			VKSTUB("vkDestroyShaderModule",    vkDestroyShaderModule),
            VKSTUB("vkQueuePresentKHR",        vkQueuePresentKHR),
			            // v5: диагностика
            VKSTUB("vkCreateInstance",                        vkCreateInstance),
            VKSTUB("vkCreateWin32SurfaceKHR",                 vkCreateWin32SurfaceKHR),
            VKSTUB("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
            VKSTUB("vkGetPhysicalDeviceSurfaceFormatsKHR",    vkGetPhysicalDeviceSurfaceFormatsKHR),
            VKSTUB("vkGetPhysicalDeviceSurfacePresentModesKHR", vkGetPhysicalDeviceSurfacePresentModesKHR),
            VKSTUB("vkCreateImage",                           vkCreateImage),
            VKSTUB("vkGetImageMemoryRequirements",            vkGetImageMemoryRequirements),
            VKSTUB("vkGetImageMemoryRequirements2",           vkGetImageMemoryRequirements2),
            VKSTUB("vkAllocateMemory",                        vkAllocateMemory),
            VKSTUB("vkBindImageMemory",                       vkBindImageMemory),
            VKSTUB("vkBindImageMemory2",                      vkBindImageMemory2),
            VKSTUB("vkCreateBuffer",                          vkCreateBuffer),
            VKSTUB("vkGetBufferMemoryRequirements",           vkGetBufferMemoryRequirements),
            VKSTUB("vkGetBufferMemoryRequirements2",          vkGetBufferMemoryRequirements2),
            VKSTUB("vkBindBufferMemory",                      vkBindBufferMemory),
            VKSTUB("vkBindBufferMemory2",                     vkBindBufferMemory2),
            VKSTUB("vkCreateDescriptorSetLayout",             vkCreateDescriptorSetLayout),
            VKSTUB("vkCreatePipelineLayout",                  vkCreatePipelineLayout),
			            // v6
            VKSTUB("vkCreateSampler",            vkCreateSampler),
            VKSTUB("vkCreateDescriptorPool",     vkCreateDescriptorPool),
            VKSTUB("vkAllocateDescriptorSets",   vkAllocateDescriptorSets),
            VKSTUB("vkUpdateDescriptorSets",     vkUpdateDescriptorSets),
            VKSTUB("vkCreateDescriptorUpdateTemplate", Trace_vkCreateDescriptorUpdateTemplate),
            VKSTUB("vkCreateDescriptorUpdateTemplateKHR", Trace_vkCreateDescriptorUpdateTemplate),
            VKSTUB("vkDestroyDescriptorUpdateTemplate", Trace_vkDestroyDescriptorUpdateTemplate),
            VKSTUB("vkDestroyDescriptorUpdateTemplateKHR", Trace_vkDestroyDescriptorUpdateTemplate),
            VKSTUB("vkUpdateDescriptorSetWithTemplate", Trace_vkUpdateDescriptorSetWithTemplate),
            VKSTUB("vkUpdateDescriptorSetWithTemplateKHR", Trace_vkUpdateDescriptorSetWithTemplate),
            VKSTUB("vkCreateCommandPool",        vkCreateCommandPool),
            VKSTUB("vkCreateFence",              vkCreateFence),
            VKSTUB("vkCreateRenderPass",         vkCreateRenderPass),
			
        };

        // RT-only stubs (served when g_rtSpoof != 0).
        const Entry kTable[] = {
            VKSTUB("vkCreateAccelerationStructureKHR", CreateAS),
            VKSTUB("vkDestroyAccelerationStructureKHR", DestroyAS),
            VKSTUB("vkCmdBuildAccelerationStructuresKHR", CmdBuildAS),
            VKSTUB("vkCmdBuildAccelerationStructuresIndirectKHR", CmdBuildASIndirect),
            VKSTUB("vkBuildAccelerationStructuresKHR", BuildAS),
            VKSTUB("vkCopyAccelerationStructureKHR", CopyAS),
            VKSTUB("vkCopyAccelerationStructureToMemoryKHR", CopyASToMem),
            VKSTUB("vkCopyMemoryToAccelerationStructureKHR", CopyMemToAS),
            VKSTUB("vkWriteAccelerationStructuresPropertiesKHR", WriteASProps),
            VKSTUB("vkCmdCopyAccelerationStructureKHR", CmdCopyAS),
            VKSTUB("vkCmdCopyAccelerationStructureToMemoryKHR", CmdCopyASToMem),
            VKSTUB("vkCmdCopyMemoryToAccelerationStructureKHR", CmdCopyMemToAS),
            VKSTUB("vkGetAccelerationStructureDeviceAddressKHR", GetASAddr),
            VKSTUB("vkCmdWriteAccelerationStructuresPropertiesKHR", CmdWriteASProps),
            VKSTUB("vkGetDeviceAccelerationStructureCompatibilityKHR", GetASCompat),
            VKSTUB("vkGetAccelerationStructureBuildSizesKHR", GetASBuildSizes),
            VKSTUB("vkCmdTraceRaysKHR", CmdTraceRays),
            VKSTUB("vkCmdTraceRaysIndirectKHR", CmdTraceRaysIndirect),
            VKSTUB("vkCmdTraceRaysIndirect2KHR", CmdTraceRaysIndirect2),
            VKSTUB("vkCreateRayTracingPipelinesKHR", CreateRTPipelines),
            VKSTUB("vkGetRayTracingShaderGroupHandlesKHR", GetRTHandles),
            VKSTUB("vkGetRayTracingCaptureReplayShaderGroupHandlesKHR", GetRTHandles),
            VKSTUB("vkGetRayTracingShaderGroupStackSizeKHR", GetRTStack),
            VKSTUB("vkCmdSetRayTracingPipelineStackSizeKHR", CmdSetRTStack),
            VKSTUB("vkCreateDeferredOperationKHR", CreateDefOp),
            VKSTUB("vkDestroyDeferredOperationKHR", DestroyDefOp),
            VKSTUB("vkGetDeferredOperationMaxConcurrencyKHR", DefOpConcurrency),
            VKSTUB("vkGetDeferredOperationResultKHR", DefOpResult),
            VKSTUB("vkDeferredOperationJoinKHR", DefOpJoin),
        };
#undef VKSTUB

        PFN_vkVoidFunction Find(const char* name) {
            if (!name) return nullptr;
            for (const Entry& e : kAlwaysTable) if (strcmp(e.name, name) == 0) return e.fn;
            if (!g_rtSpoof) return nullptr;
            for (const Entry& e : kTable) if (strcmp(e.name, name) == 0) return e.fn;
            return nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// Dynamic-rendering emulation hooks (definitions)
// ---------------------------------------------------------------------------

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    static volatile LONG enterCnt = 0;
    if (InterlockedIncrement(&enterCnt) <= 30) {
        LogLine("vkCreateGraphicsPipelines ENTER: count=%u", count);
        if (pCreateInfos) for (uint32_t i = 0; i < count && i < 4; ++i) {
            LogLine("  pipeline[%u]: renderPass=%p layout=%p stages=%u",
                i, pCreateInfos[i].renderPass, pCreateInfos[i].layout, pCreateInfos[i].stageCount);
        }
    }
    auto real = RealExport<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
    if (!real) {
        LogLine("vkCreateGraphicsPipelines: RealExport returned NULL -> FAKE pipelines");
        if (pPipelines) for (uint32_t i = 0; i < count; ++i) pPipelines[i] = MakeFake<VkPipeline>();
        return VK_SUCCESS;
    }
    if (!pCreateInfos || count == 0) {
        if (pPipelines && count) for (uint32_t i = 0; i < count; ++i) pPipelines[i] = MakeFake<VkPipeline>();
        return VK_SUCCESS;
    }

    // Pipelines that use a FAKE (denied / failed) shader module become FAKE pipelines;
    // the remaining pipelines of the batch are still compiled by the driver.
    {
        std::vector<char> isFakePipe(count, 0);
        bool anyFake = false;
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t j = 0; j < pCreateInfos[i].stageCount && pCreateInfos[i].pStages; ++j)
                if (IsFake(pCreateInfos[i].pStages[j].module)) { isFakePipe[i] = 1; anyFake = true; break; }
        }
        if (anyFake) {
            static volatile LONG fakeBatches = 0;
            if (InterlockedIncrement(&fakeBatches) <= 40)
                LogLine("vkCreateGraphicsPipelines: batch of %u contains FAKE-shader pipelines", count);
            std::vector<VkGraphicsPipelineCreateInfo> sub;
            for (uint32_t i = 0; i < count; ++i) if (!isFakePipe[i]) sub.push_back(pCreateInfos[i]);
            std::vector<VkPipeline> subOut(sub.size(), VK_NULL_HANDLE);
            VkResult sr = VK_SUCCESS;
            if (!sub.empty())
                sr = vkCreateGraphicsPipelines(device, cache, static_cast<uint32_t>(sub.size()), sub.data(), pAllocator, subOut.data());
            size_t k = 0;
            for (uint32_t i = 0; i < count; ++i)
                if (pPipelines) pPipelines[i] = isFakePipe[i] ? MakeFake<VkPipeline>() : subOut[k++];
            (void)sr;
            return VK_SUCCESS;
        }
    }
    // Fast-fail: GCN2 AMDVLK spends ~5s then returns -13 on shaders with GroupNonUniform
    // + Float16/Int16/cap45. Don't call the driver — FAKE immediately so menu can load.
    {
        std::vector<char> toxic(count, 0);
        bool anyToxic = false;
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t j = 0; j < pCreateInfos[i].stageCount && pCreateInfos[i].pStages; ++j) {
                const std::string sig = SigOfModule(pCreateInfos[i].pStages[j].module, static_cast<uint32_t>(pCreateInfos[i].pStages[j].stage));
                if (SigToxic(sig)) { toxic[i] = 1; anyToxic = true; break; }
            }
        }
        if (anyToxic) {
            static volatile LONG toxN = 0;
            std::vector<VkGraphicsPipelineCreateInfo> sub;
            std::vector<uint32_t> subIdx;
            for (uint32_t i = 0; i < count; ++i) {
                if (toxic[i]) {
                    if (pPipelines) pPipelines[i] = MakeFake<VkPipeline>();
                    LONG t = InterlockedIncrement(&toxN);
                    if (t <= 15 || t % 50 == 0)
                        LogLine("FAST-FAKE graphics pipeline (learned toxic signature) total=%ld", (long)t);
                } else {
                    sub.push_back(pCreateInfos[i]);
                    subIdx.push_back(i);
                }
            }
            if (sub.empty()) return VK_SUCCESS;
            std::vector<VkPipeline> subOut(sub.size(), VK_NULL_HANDLE);
            VkResult sr = vkCreateGraphicsPipelines(device, cache, static_cast<uint32_t>(sub.size()),
                sub.data(), pAllocator, subOut.data());
            for (size_t k = 0; k < sub.size(); ++k)
                if (pPipelines) pPipelines[subIdx[k]] = subOut[k];
            (void)sr;
            return VK_SUCCESS;
        }
    }

    // =====================================================================
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < pCreateInfos[i].stageCount && j < 6; ++j) {
            char d[180];
            DescribeModule(pCreateInfos[i].pStages[j].module, d, sizeof(d));
            Crumb("vkCreateGraphicsPipelines[%u/%u] stage%u=0x%X %s", i, count, j,
                (unsigned)pCreateInfos[i].pStages[j].stage, d);
            SetJob("vkCreateGraphicsPipelines cache=%p [%u/%u] stage%u=0x%X %s",
                (void*)cache, i, count, j, (unsigned)pCreateInfos[i].pStages[j].stage, d);
        }
    }

    std::vector<VkGraphicsPipelineCreateInfo> mods(count);
    for (uint32_t i = 0; i < count; ++i) {
        mods[i] = pCreateInfos[i];
        if (mods[i].renderPass != VK_NULL_HANDLE) continue;

        const VkBaseInStructure* n = reinterpret_cast<const VkBaseInStructure*>(mods[i].pNext);
        const VkPipelineRenderingCreateInfo* ri = nullptr;
        while (n) {
            if (static_cast<uint32_t>(n->sType) == dynrend::ST_PIPELINE_RENDERING_CREATE_INFO) {
                ri = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(n);
                break;
            }
            n = n->pNext;
        }
        if (!ri) continue;

        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (mods[i].pMultisampleState) samples = mods[i].pMultisampleState->rasterizationSamples;

        dynrend::RPKey k;
        for (uint32_t j = 0; j < ri->colorAttachmentCount; ++j) {
            k.colorFormats.push_back(ri->pColorAttachmentFormats[j]);
            k.colorLoadOps.push_back(VK_ATTACHMENT_LOAD_OP_LOAD);
        k.colorLayouts.push_back(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        k.depthFormat = ri->depthAttachmentFormat;
        k.stencilFormat = ri->stencilAttachmentFormat;
        k.samples = samples;
        k.viewMask = ri->viewMask;
        k.depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        k.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;

        VkRenderPass rp = dynrend::CreateOrGetRenderPass(device, k);
        if (rp == VK_NULL_HANDLE) continue;
        mods[i].renderPass = rp;
        mods[i].subpass = 0;
        LogLine("dynrend: pipeline[%u] renderPass substituted (colors=%u depth=0x%X stencil=0x%X samples=%u)",
            i, static_cast<unsigned>(k.colorFormats.size()), k.depthFormat, k.stencilFormat,
            static_cast<unsigned>(samples));
    }

    VkResult r = VK_ERROR_UNKNOWN;
    if (!DriverCallPipeline(cache, "vkCreateGraphicsPipelines", &r, [&]() { r = real(device, cache, count, mods.data(), pAllocator, pPipelines); })) {
        LogLine("CAUGHT driver crash in vkCreateGraphicsPipelines (count=%u) -> FAKE pipelines", count);
        r = VK_ERROR_UNKNOWN;
    }
    
    // Если реальный драйвер по каким-то причинам все равно вернул ошибку - подсовываем фейк
    if (r != VK_SUCCESS) {
        LogLine("vkCreateGraphicsPipelines FAILED: result=%d, creating FAKE pipelines", (int)r);
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = 0; j < pCreateInfos[i].stageCount && pCreateInfos[i].pStages; ++j)
                SigRecord(SigOfModule(pCreateInfos[i].pStages[j].module, static_cast<uint32_t>(pCreateInfos[i].pStages[j].stage)), false);
        static volatile LONG gpFails = 0;
        const LONG gf = InterlockedIncrement(&gpFails);
        if (gf <= 5) for (uint32_t i = 0; i < count; ++i) DescribeFailedGraphicsPipeline(pCreateInfos[i], gf * 10 + i);
        // Last resort: retry with texel fetches cut out. Skip when driver already spent
        // ~5s and returned -13 (VK_ERROR_UNKNOWN) — retry almost never helps and freezes menu load.
        bool recovered = false;
        if (g_spvCfg.tryVariants && count == 1 && pPipelines && r != static_cast<VkResult>(-13)) {
            std::vector<VkPipelineShaderStageCreateInfo> stages(mods[0].pStages, mods[0].pStages + mods[0].stageCount);
            std::vector<VkShaderModule> temps;
            auto createSM = RealExport<PFN_vkCreateShaderModule>("vkCreateShaderModule");
            for (uint32_t j = 0; j < stages.size() && createSM; ++j) {
                SpvInfo si;
                if (IsFake(stages[j].module) || !GetSpvInfo(stages[j].module, si) || !si.code || si.code->empty()) continue;
                std::vector<uint32_t> var;
                std::string verr;
                if (!spvrw::StripImageFetch(si.code->data(), si.code->size(), var, verr)) continue;
                if (g_spvCfg.patchVersion && var.size() > 1 && var[1] > 0x00010500u) var[1] = 0x00010500u;
                VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                mci.codeSize = var.size() * 4;
                mci.pCode = var.data();
                VkShaderModule m = VK_NULL_HANDLE;
                VkResult mr = VK_ERROR_UNKNOWN;
                DriverCall([&]() { mr = createSM(device, &mci, pAllocator, &m); });
                if (mr == VK_SUCCESS && m) { stages[j].module = m; temps.push_back(m); }
            }
            if (!temps.empty()) {
                VkGraphicsPipelineCreateInfo ci2 = mods[0];
                ci2.pStages = stages.data();
                VkPipeline np = VK_NULL_HANDLE;
                VkResult rr = VK_ERROR_UNKNOWN;
                SetJob("vkCreateGraphicsPipelines RETRY without texel fetch");
                const bool ok = DriverCallPipeline(cache, "vkCreateGraphicsPipelines-retry", &rr, [&]() { rr = real(device, cache, 1, &ci2, pAllocator, &np); });
                if (ok && rr == VK_SUCCESS && np) {
                    pPipelines[0] = np;
                    recovered = true;
                    LogLine("RETRY WORKED: pipeline accepted after removing texel fetches from %zu shader(s) (their fetched data is lost)", temps.size());
                } else {
                    LogLine("RETRY failed (%s, result=%d): pipeline stays FAKE", ok ? "rejected" : "crash", (int)rr);
                }
                auto destroySM = RealExport<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
                if (destroySM) for (VkShaderModule m : temps) destroySM(device, m, pAllocator);
            }
        }
        if (!recovered) {
            for (uint32_t i = 0; i < count; ++i) {
                if (pPipelines) pPipelines[i] = MakeFake<VkPipeline>();
            }
        }
        r = VK_SUCCESS; 
    }

    if ((r == VK_SUCCESS || r == VK_PIPELINE_COMPILE_REQUIRED_EXT) && pPipelines) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!pPipelines[i]) continue;
            
            if (IsFake(pPipelines[i])) continue; 
            for (uint32_t j = 0; j < pCreateInfos[i].stageCount && pCreateInfos[i].pStages; ++j)
                SigRecord(SigOfModule(pCreateInfos[i].pStages[j].module, static_cast<uint32_t>(pCreateInfos[i].pStages[j].stage)), true);

            const VkBaseInStructure* n = reinterpret_cast<const VkBaseInStructure*>(pCreateInfos[i].pNext);
            while (n) {
                if (static_cast<uint32_t>(n->sType) == dynrend::ST_PIPELINE_RENDERING_CREATE_INFO) {
                    const auto* ri = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(n);
                    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
                    if (pCreateInfos[i].pMultisampleState)
                        samples = pCreateInfos[i].pMultisampleState->rasterizationSamples;
                    dynrend::RegisterPipeline(pPipelines[i], device, ri, samples);
                    break;
                }
                n = n->pNext;
            }
        }
    }
    return r;
}


extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    auto real = RealExport<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfos || createInfoCount == 0)
        return real(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);

    {
        std::vector<char> isFakePipe(createInfoCount, 0);
        bool anyFake = false;
        for (uint32_t i = 0; i < createInfoCount; ++i)
            if (IsFake(pCreateInfos[i].stage.module)) { isFakePipe[i] = 1; anyFake = true; }
        if (anyFake) {
            static volatile LONG fakeBatches = 0;
            if (InterlockedIncrement(&fakeBatches) <= 40)
                LogLine("vkCreateComputePipelines: batch of %u contains FAKE-shader pipelines", createInfoCount);
            std::vector<VkComputePipelineCreateInfo> sub;
            for (uint32_t i = 0; i < createInfoCount; ++i) if (!isFakePipe[i]) sub.push_back(pCreateInfos[i]);
            std::vector<VkPipeline> subOut(sub.size(), VK_NULL_HANDLE);
            if (!sub.empty())
                vkCreateComputePipelines(device, pipelineCache, static_cast<uint32_t>(sub.size()), sub.data(), pAllocator, subOut.data());
            size_t k = 0;
            for (uint32_t i = 0; i < createInfoCount; ++i)
                if (pPipelines) pPipelines[i] = isFakePipe[i] ? MakeFake<VkPipeline>() : subOut[k++];
            return VK_SUCCESS;
        }
    }
    // Fast-fail heavy compute shaders (same 5s -13 problem)
    {
        std::vector<char> toxic(createInfoCount, 0);
        bool anyToxic = false;
        for (uint32_t i = 0; i < createInfoCount; ++i) {
            if (SigToxic(SigOfModule(pCreateInfos[i].stage.module, static_cast<uint32_t>(pCreateInfos[i].stage.stage)))) { toxic[i] = 1; anyToxic = true; }
        }
        if (anyToxic) {
            static volatile LONG toxC = 0;
            std::vector<VkComputePipelineCreateInfo> sub;
            std::vector<uint32_t> subIdx;
            for (uint32_t i = 0; i < createInfoCount; ++i) {
                if (toxic[i]) {
                    if (pPipelines) pPipelines[i] = MakeFake<VkPipeline>();
                    LONG t = InterlockedIncrement(&toxC);
                    if (t <= 10 || t % 50 == 0)
                        LogLine("FAST-FAKE compute pipeline (learned toxic signature) total=%ld", (long)t);
                } else {
                    sub.push_back(pCreateInfos[i]);
                    subIdx.push_back(i);
                }
            }
            if (sub.empty()) return VK_SUCCESS;
            std::vector<VkPipeline> subOut(sub.size(), VK_NULL_HANDLE);
            vkCreateComputePipelines(device, pipelineCache, static_cast<uint32_t>(sub.size()),
                sub.data(), pAllocator, subOut.data());
            for (size_t k = 0; k < sub.size(); ++k)
                if (pPipelines) pPipelines[subIdx[k]] = subOut[k];
            return VK_SUCCESS;
        }
    }

    for (uint32_t i = 0; i < createInfoCount; ++i) {
        char d[180];
        DescribeModule(pCreateInfos[i].stage.module, d, sizeof(d));
        DumpSuspiciousComputeShaderForPipeline(pCreateInfos[i], i);
        const auto& st = pCreateInfos[i].stage;
        const VkBaseInStructure* sp = reinterpret_cast<const VkBaseInStructure*>(st.pNext);
        Crumb("vkCreateComputePipelines[%u/%u] flags=0x%X stageFlags=0x%X stagePNext=%u spec=%u cache=%p %s", i, createInfoCount,
            (unsigned)pCreateInfos[i].flags, (unsigned)st.flags, sp ? (unsigned)sp->sType : 0u,
            st.pSpecializationInfo ? st.pSpecializationInfo->mapEntryCount : 0u, (void*)pipelineCache, d);
        SetJob("vkCreateComputePipelines cache=%p [%u/%u] layout=%p %s",
            (void*)pipelineCache, i, createInfoCount, (void*)pCreateInfos[i].layout, d);
    }
    VkResult cr = VK_ERROR_UNKNOWN;
    if (!DriverCallPipeline(pipelineCache, "vkCreateComputePipelines", &cr, [&]() { cr = real(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines); })) {
        LogLine("CAUGHT driver crash in vkCreateComputePipelines (count=%u) -> FAKE pipelines", createInfoCount);
        for (uint32_t i = 0; i < createInfoCount && i < 4; ++i) {
            char d[180];
            DescribeModule(pCreateInfos[i].stage.module, d, sizeof(d));
            LogLine("  crashed compute pipeline[%u]: flags=0x%X stageFlags=0x%X spec=%u name=%s layout=%p %s", i,
                (unsigned)pCreateInfos[i].flags, (unsigned)pCreateInfos[i].stage.flags,
                pCreateInfos[i].stage.pSpecializationInfo ? pCreateInfos[i].stage.pSpecializationInfo->mapEntryCount : 0u,
                pCreateInfos[i].stage.pName ? pCreateInfos[i].stage.pName : "(null)", (void*)pCreateInfos[i].layout, d);
        }
        for (uint32_t k = 0; k < createInfoCount; ++k) if (pPipelines) pPipelines[k] = MakeFake<VkPipeline>();
        return VK_SUCCESS;
    }
    for (uint32_t i = 0; i < createInfoCount; ++i)
        SigRecord(SigOfModule(pCreateInfos[i].stage.module, static_cast<uint32_t>(pCreateInfos[i].stage.stage)), cr == VK_SUCCESS);
    if (cr != VK_SUCCESS) {
        LogLine("vkCreateComputePipelines FAILED: result=%d (count=%u) -> FAKE pipelines", (int)cr, createInfoCount);
        for (uint32_t k = 0; k < createInfoCount; ++k) if (pPipelines) pPipelines[k] = MakeFake<VkPipeline>();
        return VK_SUCCESS;
    }
    if (pPipelines) {
        for (uint32_t i = 0; i < createInfoCount; ++i)
            TraceRegisterReplacementPipeline(pPipelines[i], pCreateInfos[i].stage.module, pCreateInfos[i].layout);
    }
    return cr;
}




extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator) {
    TraceForgetReplacementPipeline(pipeline);
    if (IsFake(pipeline)) return;  // don't leak RT stub pipelines into the driver
    dynrend::UnregisterPipeline(pipeline);
    auto real = RealExport<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    if (real) real(device, pipeline, pAllocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks* pAllocator) {
    EraseSpvInfo(shaderModule);
    if (IsFake(shaderModule)) return; // Не передаем фейковый хэндл в драйвер!
    auto real = RealExport<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    if (real) real(device, shaderModule, pAllocator);
}



extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline pipeline) {
    // Всегда сохраняем пайплайн в мапу (даже если он фейковый)
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS || bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        AcquireSRWLockExclusive(&dynrend::g_lock);
        if (!dynrend::g_boundPipes) dynrend::g_boundPipes = new std::map<VkCommandBuffer, VkPipeline>();
        (*dynrend::g_boundPipes)[cb] = pipeline;
        ReleaseSRWLockExclusive(&dynrend::g_lock);
    }
    
    TraceLogReplacementPipelineBind(cb, bp, pipeline);
    // Fake or NULL pipeline: never hand it to the driver (a NULL object crashes amdvlk64 at [obj+0x2A0])
    if (!pipeline || IsFake(pipeline)) return;
    
    auto real = RealExport<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    if (real) real(cb, bp, pipeline);
}
// Вспомогательная функция для проверки
static bool IsBoundPipelineFake(VkCommandBuffer cb) {
    AcquireSRWLockShared(&dynrend::g_lock);
    bool isFake = false;
    if (dynrend::g_boundPipes) {
        auto it = dynrend::g_boundPipes->find(cb);
        if (it != dynrend::g_boundPipes->end() && (!it->second || IsFake(it->second))) {
            isFake = true;
        }
    }
    ReleaseSRWLockShared(&dynrend::g_lock);
    return isFake;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    if (IsBoundPipelineFake(cb)) {
        static volatile LONG skipped = 0;
        LONG n = InterlockedIncrement(&skipped);
        if (n <= 5 || n % 500 == 0)
            LogLine("SKIP draw on FAKE pipeline (total skipped draws≈%ld)", (long)n);
        return;
    }
    auto real = RealExport<PFN_vkCmdDraw>("vkCmdDraw");
    if (real) real(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset) {
    if (IsBoundPipelineFake(cb)) return;
    TraceLogReplacementDispatch(cb, 0, 0, 0, true, buffer, offset);
    auto real = RealExport<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect");
    if (real) real(cb, buffer, offset);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdDrawIndirectCount>("vkCmdDrawIndirectCount");
    if (real) real(cb, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdDrawIndexedIndirectCount>("vkCmdDrawIndexedIndirectCount");
    if (real) real(cb, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    if (IsBoundPipelineFake(cb)) return;
    TraceBindDescriptorSets(cb, bp, firstSet, setCount, pDescriptorSets);
    TraceLogBoundDescriptorSetBind(cb, bp, layout, firstSet, setCount, pDescriptorSets);
    auto real = RealExport<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    if (real) real(cb, bp, layout, firstSet, setCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdPushConstants>("vkCmdPushConstants");
    if (real) real(cb, layout, stageFlags, offset, size, pValues);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
    if (real) real(cb, firstBinding, bindingCount, pBuffers, pOffsets);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
    if (real) real(cb, buffer, offset, indexType);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
if (IsBoundPipelineFake(cb)) {
    static volatile LONG skippedIdx = 0;
    LONG k = InterlockedIncrement(&skippedIdx);
    if (k <= 5 || k % 2000 == 0)
        LogLine("SKIP indexed draw on FAKE pipeline (total=%ld)", (long)k);
    return;
}
    auto real = RealExport<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
    if (real) real(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDispatch(VkCommandBuffer cb, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (IsBoundPipelineFake(cb)) return;
    TraceLogReplacementDispatch(cb, groupCountX, groupCountY, groupCountZ);
    auto real = RealExport<PFN_vkCmdDispatch>("vkCmdDispatch");
    if (real) real(cb, groupCountX, groupCountY, groupCountZ);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
Proxy_vkCmdDispatchBase(VkCommandBuffer cb,
    uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ,
    uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (IsBoundPipelineFake(cb)) return;
    TraceLogReplacementDispatch(cb, groupCountX, groupCountY, groupCountZ,
        false, VK_NULL_HANDLE, 0, true, baseGroupX, baseGroupY, baseGroupZ);
    using PFN_vkCmdDispatchBaseCompat = void (VKAPI_PTR *)(
        VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    auto real = RealExport<PFN_vkCmdDispatchBaseCompat>("vkCmdDispatchBase");
    if (!real) real = RealExport<PFN_vkCmdDispatchBaseCompat>("vkCmdDispatchBaseKHR");
    if (real) real(cb, baseGroupX, baseGroupY, baseGroupZ, groupCountX, groupCountY, groupCountZ);
}
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect");
    if (real) real(cb, buffer, offset, drawCount, stride);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    if (IsBoundPipelineFake(cb)) return;
    auto real = RealExport<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect");
    if (real) real(cb, buffer, offset, drawCount, stride);
}



extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
        static volatile LONG cnt = 0;
    if (pCreateInfo && InterlockedIncrement(&cnt) <= 40) {
        LogLine("vkCreateImageView ENTER: format=%d viewType=%d aspect=0x%X mip=%u..%u layer=%u..%u",
            (int)pCreateInfo->format, (int)pCreateInfo->viewType,
            (unsigned)pCreateInfo->subresourceRange.aspectMask,
            pCreateInfo->subresourceRange.baseMipLevel, pCreateInfo->subresourceRange.levelCount,
            pCreateInfo->subresourceRange.baseArrayLayer, pCreateInfo->subresourceRange.layerCount);
    }
    auto real = RealExport<PFN_vkCreateImageView>("vkCreateImageView");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pCreateInfo, pAllocator, pView);
    if (r == VK_SUCCESS && pView && *pView && pCreateInfo) {
        AcquireSRWLockExclusive(&dynrend::g_lock);
        if (!dynrend::g_imageViews) dynrend::g_imageViews = new std::map<VkImageView, dynrend::ImageViewInfo>();
        dynrend::ImageViewInfo info;
        info.format = pCreateInfo->format;
        info.aspect = pCreateInfo->subresourceRange.aspectMask;
info.image = pCreateInfo->image;
        (*dynrend::g_imageViews)[*pView] = info;
        ReleaseSRWLockExclusive(&dynrend::g_lock);
    }
	
	    if (r == VK_SUCCESS && pCreateInfo) {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 20)
            LogLine("vkCreateImageView: format=%d aspect=0x%X",
                (int)pCreateInfo->format, (unsigned)pCreateInfo->subresourceRange.aspectMask);
    } else if (r != VK_SUCCESS) {
        LogLine("vkCreateImageView FAILED: result=%d format=%d",
            (int)r, pCreateInfo ? (int)pCreateInfo->format : -1);
    }
	
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* pAllocator) {
    AcquireSRWLockExclusive(&dynrend::g_lock);
    if (dynrend::g_imageViews) dynrend::g_imageViews->erase(view);
    ReleaseSRWLockExclusive(&dynrend::g_lock);
    auto real = RealExport<PFN_vkDestroyImageView>("vkDestroyImageView");
    if (real) real(device, view, pAllocator);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pInfo,
    VkCommandBuffer* pBuffers) {
    auto real = RealExport<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(device, pInfo, pBuffers);
    if (r == VK_SUCCESS && pInfo && pBuffers) {
        AcquireSRWLockExclusive(&dynrend::g_lock);
        if (!dynrend::g_cbDevices) dynrend::g_cbDevices = new std::map<VkCommandBuffer, VkDevice>();
        for (uint32_t i = 0; i < pInfo->commandBufferCount; ++i)
            (*dynrend::g_cbDevices)[pBuffers[i]] = device;
        ReleaseSRWLockExclusive(&dynrend::g_lock);
    }
	
	    if (r == VK_SUCCESS && pInfo) {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 5)
            LogLine("vkAllocateCommandBuffers: count=%u", pInfo->commandBufferCount);
    }
	
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer* pBuffers) {
    AcquireSRWLockExclusive(&dynrend::g_lock);
    if (dynrend::g_cbDevices && pBuffers)
        for (uint32_t i = 0; i < count; ++i) dynrend::g_cbDevices->erase(pBuffers[i]);
    if (dynrend::g_boundPipes && pBuffers)
        for (uint32_t i = 0; i < count; ++i) dynrend::g_boundPipes->erase(pBuffers[i]);
    if (dynrend::g_active && pBuffers)
        for (uint32_t i = 0; i < count; ++i) dynrend::g_active->erase(pBuffers[i]);
    ReleaseSRWLockExclusive(&dynrend::g_lock);
    TraceForgetCommandBuffers(count, pBuffers);
    auto real = RealExport<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers");
    if (real) real(device, pool, count, pBuffers);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Proxy_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* pInfo) {
    auto real = RealExport<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = real(cb, pInfo);
    if (r == VK_SUCCESS) TraceForgetCommandBufferBindings(cb);
    return r;
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* pInfo) {
    if (!pInfo) return;
    const VkDevice device = dynrend::LookupCBDevice(cb);
    if (device == VK_NULL_HANDLE) { LogLine("dynrend: BeginRendering without device cb=%p", cb); return; }
    dynrend::PipeInfo boundPipe{};
    const bool haveBoundPipe = dynrend::LookupBoundPipeline(cb, &boundPipe);
    dynrend::RPKey k;
    k.samples = haveBoundPipe ? boundPipe.samples : VK_SAMPLE_COUNT_1_BIT;
    k.viewMask = pInfo->viewMask;
    for (uint32_t i = 0; i < pInfo->colorAttachmentCount; ++i) {
        const auto& ca = pInfo->pColorAttachments[i];
        if (!ca.imageView) {
            k.colorFormats.push_back(VK_FORMAT_UNDEFINED);
            k.colorLoadOps.push_back(VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            k.colorLayouts.push_back(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            continue;
        }
        dynrend::ImageViewInfo vi{};
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        if (dynrend::LookupImageView(ca.imageView, &vi)) fmt = vi.format;
        if (fmt == VK_FORMAT_UNDEFINED && haveBoundPipe && i < boundPipe.colorFormats.size()) fmt = boundPipe.colorFormats[i];
        if (fmt == VK_FORMAT_UNDEFINED) fmt = VK_FORMAT_B8G8R8A8_UNORM;
        k.colorFormats.push_back(fmt);
        k.colorLoadOps.push_back(ca.loadOp);
        k.colorLayouts.push_back(ca.imageLayout);
    }
    if (pInfo->pDepthAttachment && pInfo->pDepthAttachment->imageView) {
        dynrend::ImageViewInfo vi{};
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        if (dynrend::LookupImageView(pInfo->pDepthAttachment->imageView, &vi)) fmt = vi.format;
        if (fmt == VK_FORMAT_UNDEFINED && haveBoundPipe) fmt = boundPipe.depthFormat;
        if (fmt == VK_FORMAT_UNDEFINED) fmt = VK_FORMAT_D32_SFLOAT;
        k.depthFormat = fmt;
        k.depthLoadOp = pInfo->pDepthAttachment->loadOp;
        k.depthLayout = pInfo->pDepthAttachment->imageLayout;
    }
    if (pInfo->pStencilAttachment && pInfo->pStencilAttachment->imageView &&
        (!pInfo->pDepthAttachment || pInfo->pStencilAttachment->imageView != pInfo->pDepthAttachment->imageView)) {
        dynrend::ImageViewInfo vi{};
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        if (dynrend::LookupImageView(pInfo->pStencilAttachment->imageView, &vi)) fmt = vi.format;
        if (fmt == VK_FORMAT_UNDEFINED && haveBoundPipe) fmt = boundPipe.stencilFormat;
        if (fmt == VK_FORMAT_UNDEFINED) fmt = VK_FORMAT_S8_UINT;
        k.stencilFormat = fmt;
        k.stencilLoadOp = pInfo->pStencilAttachment->loadOp;
        k.stencilLayout = pInfo->pStencilAttachment->imageLayout;
    }
    VkRenderPass rp = dynrend::CreateOrGetRenderPass(device, k);
    if (rp == VK_NULL_HANDLE) return;
    std::vector<VkImageView> views;
    for (uint32_t i = 0; i < pInfo->colorAttachmentCount; ++i)
        if (pInfo->pColorAttachments[i].imageView) views.push_back(pInfo->pColorAttachments[i].imageView);
    if (pInfo->pDepthAttachment && pInfo->pDepthAttachment->imageView) views.push_back(pInfo->pDepthAttachment->imageView);
    if (pInfo->pStencilAttachment && pInfo->pStencilAttachment->imageView &&
        (!pInfo->pDepthAttachment || pInfo->pStencilAttachment->imageView != pInfo->pDepthAttachment->imageView))
        views.push_back(pInfo->pStencilAttachment->imageView);
    const uint32_t fbW = pInfo->renderArea.offset.x + pInfo->renderArea.extent.width;
    const uint32_t fbH = pInfo->renderArea.offset.y + pInfo->renderArea.extent.height;
    const uint32_t layers = pInfo->layerCount ? pInfo->layerCount : 1;
    dynrend::FBKey fbk; fbk.rp = rp; fbk.views = views; fbk.width = fbW; fbk.height = fbH; fbk.layers = k.viewMask ? 1u : layers;
    VkFramebuffer fb = VK_NULL_HANDLE;
    AcquireSRWLockExclusive(&dynrend::g_lock);
    if (!dynrend::g_fbCache) dynrend::g_fbCache = new std::map<dynrend::FBKey, VkFramebuffer>();
    auto fbit = dynrend::g_fbCache->find(fbk);
    if (fbit != dynrend::g_fbCache->end()) fb = fbit->second;
    ReleaseSRWLockExclusive(&dynrend::g_lock);
    if (fb == VK_NULL_HANDLE) {
        VkFramebufferCreateInfo fbci{};
        fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbci.renderPass = rp;
        fbci.attachmentCount = (uint32_t)views.size();
        fbci.pAttachments = views.empty() ? nullptr : views.data();
        fbci.width = fbW ? fbW : 1; fbci.height = fbH ? fbH : 1; fbci.layers = fbk.layers ? fbk.layers : 1;
        auto createFb = RealExport<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
        if (!createFb) return;
        VkResult r = createFb(device, &fbci, nullptr, &fb);
        if (r != VK_SUCCESS) { LogLine("dynrend: CreateFramebuffer FAILED %d", (int)r); return; }
        AcquireSRWLockExclusive(&dynrend::g_lock);
        (*dynrend::g_fbCache)[fbk] = fb;
        ReleaseSRWLockExclusive(&dynrend::g_lock);
    }
    std::vector<VkClearValue> clears(views.size());
    uint32_t ci2 = 0;
    for (uint32_t i = 0; i < pInfo->colorAttachmentCount && ci2 < clears.size(); ++i) {
        if (!pInfo->pColorAttachments[i].imageView) continue;
        if (pInfo->pColorAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
            clears[ci2].color = pInfo->pColorAttachments[i].clearValue.color;
        ++ci2;
    }
    if (pInfo->pDepthAttachment && pInfo->pDepthAttachment->imageView && ci2 < clears.size()) {
        clears[ci2].depthStencil.depth = pInfo->pDepthAttachment->clearValue.depthStencil.depth;
        clears[ci2].depthStencil.stencil =
            (pInfo->pStencilAttachment && pInfo->pStencilAttachment->imageView == pInfo->pDepthAttachment->imageView)
                ? pInfo->pStencilAttachment->clearValue.depthStencil.stencil : 0;
    }
    VkRenderPassBeginInfo rpbi{};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = rp; rpbi.framebuffer = fb; rpbi.renderArea = pInfo->renderArea;
    rpbi.clearValueCount = (uint32_t)clears.size();
    rpbi.pClearValues = clears.empty() ? nullptr : clears.data();
    auto cmdBegin = RealExport<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    if (cmdBegin) cmdBegin(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    AcquireSRWLockExclusive(&dynrend::g_lock);
    if (!dynrend::g_active) dynrend::g_active = new std::map<VkCommandBuffer, dynrend::ActiveState>();
    dynrend::ActiveState as; as.renderPass = rp; as.framebuffer = fb;
    (*dynrend::g_active)[cb] = as;
    ReleaseSRWLockExclusive(&dynrend::g_lock);
}
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdEndRendering(VkCommandBuffer cb) {
    bool have = false;
    AcquireSRWLockExclusive(&dynrend::g_lock);
    if (dynrend::g_active) {
        auto it = dynrend::g_active->find(cb);
        if (it != dynrend::g_active->end()) {
            dynrend::g_active->erase(it);
            have = true;
        }
    }
    ReleaseSRWLockExclusive(&dynrend::g_lock);

    if (!have) {
        static volatile LONG n = 0;
        if (InterlockedIncrement(&n) <= 8)
            LogLine("dynrend: vkCmdEndRendering without active state for cb=%p", cb);
        return;
    }
    auto cmdEnd = RealExport<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    if (cmdEnd) cmdEnd(cb);
    // Framebuffer stays in g_fbCache (reused next frame). Destroy on process exit.
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* pInfo) {
    vkCmdBeginRendering(cb, pInfo);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdEndRenderingKHR(VkCommandBuffer cb) {
    vkCmdEndRendering(cb);
}

// ---------------------------------------------------------------------------
// maintenance4 stubs (translated to legacy vkGet*MemoryRequirements via a
// temporary resource)
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    if (!pInfo || !pInfo->pCreateInfo || !pMemoryRequirements) return;
    auto create = RealExport<PFN_vkCreateBuffer>("vkCreateBuffer");
    auto getReq = RealExport<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements");
    auto destroy = RealExport<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    if (!create || !getReq || !destroy) return;
    VkBuffer tmp = VK_NULL_HANDLE;
    VkResult r = create(device, pInfo->pCreateInfo, nullptr, &tmp);
    if (r == VK_SUCCESS && tmp) {
        getReq(device, tmp, &pMemoryRequirements->memoryRequirements);
        destroy(device, tmp, nullptr);
    } else {
        LogLine("dynrend: temp vkCreateBuffer for memory reqs failed result=%d", static_cast<int>(r));
        VkMemoryRequirements& mr = pMemoryRequirements->memoryRequirements;
        mr.size = 1024; mr.alignment = 256; mr.memoryTypeBits = 0xFFFFFFFFu;
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo,
    VkMemoryRequirements2* pMemoryRequirements) {
    if (!pInfo || !pInfo->pCreateInfo || !pMemoryRequirements) return;
    auto create = RealExport<PFN_vkCreateImage>("vkCreateImage");
    auto getReq = RealExport<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
    auto destroy = RealExport<PFN_vkDestroyImage>("vkDestroyImage");
    if (!create || !getReq || !destroy) return;
    VkImage tmp = VK_NULL_HANDLE;
    VkResult r = create(device, pInfo->pCreateInfo, nullptr, &tmp);
    if (r == VK_SUCCESS && tmp) {
        getReq(device, tmp, &pMemoryRequirements->memoryRequirements);
        destroy(device, tmp, nullptr);
    } else {
        LogLine("dynrend: temp vkCreateImage for memory reqs failed result=%d", static_cast<int>(r));
        VkMemoryRequirements& mr = pMemoryRequirements->memoryRequirements;
        mr.size = 4096; mr.alignment = 256; mr.memoryTypeBits = 0xFFFFFFFFu;
    }
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetDeviceImageSparseMemoryRequirements(VkDevice, const VkDeviceImageMemoryRequirements*,
    uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements2*) {
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}



// v4 additions
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* pInfo);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier2KHR(VkCommandBuffer cb, const VkDependencyInfo* pInfo);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdWriteTimestamp2KHR(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

// v5 additions
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pCapabilities);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
    uint32_t* pCount, VkSurfaceFormatKHR* pFormats);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface,
    uint32_t* pCount, VkPresentModeKHR* pModes);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkImage* pImage);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* pMemReq);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemReq);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pInfo,
    const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory2(VkDevice device, uint32_t bindCount, const VkBindImageMemoryInfo* pBinds);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkBuffer* pBuffer);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* pMemReq);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
    VkMemoryRequirements2* pMemReq);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory2(VkDevice device, uint32_t bindCount, const VkBindBufferMemoryInfo* pBinds);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout);
	
// v6 additions
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkSampler* pSampler);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkCommandPool* pCommandPool);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(VkDevice device, const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkFence* pFence);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass);
	
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDraw(VkCommandBuffer cb, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDispatch(VkCommandBuffer cb, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride);

extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets);
extern "C" VKAPI_ATTR void VKAPI_CALL
vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType);
	
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);	
	
// ---------------------------------------------------------------------------
// Loader entry points
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName);

extern "C" FARPROC ResolveVkExport(uint32_t index) noexcept {
    if (index >= VK_PROXY_EXPORT_COUNT || !EnsureSystemLoader()) return nullptr;
    PVOID cached = InterlockedCompareExchangePointer(&g_vkSlots[index], nullptr, nullptr);
    if (cached) return reinterpret_cast<FARPROC>(cached);
    FARPROC resolved = GetProcAddress(g_realLoader, kVkProxyExportNames[index]);
    if (!resolved) {
        LogLine("System Loader does not export %s", kVkProxyExportNames[index]);
        return nullptr;
    }
    PVOID previous = InterlockedCompareExchangePointer(
        &g_vkSlots[index], reinterpret_cast<PVOID>(resolved), nullptr);
    return reinterpret_cast<FARPROC>(previous ? previous : reinterpret_cast<PVOID>(resolved));
}

// ---------------------------------------------------------------------------
// Interposed Vulkan commands (properties / features / extensions / device)
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* pProperties) {
    auto real = RealExport<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    if (!real) return;
    real(pd, pProperties);
    FixupProperties(pd, pProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* pProperties) {
    using Fn = PFN_vkGetPhysicalDeviceProperties2;
    Fn real = RealExport<Fn>("vkGetPhysicalDeviceProperties2");
    if (!real) real = RealExport<Fn>("vkGetPhysicalDeviceProperties2KHR");
    if (!real || !pProperties) return;
    LogChainOnce("Properties2", pProperties->pNext);
    const RtDeviceCaps rtCaps = GetRtDeviceCaps(pd);
    {
        PNextChainFilter filter;
        filter.Apply(reinterpret_cast<VkBaseOutStructure**>(&pProperties->pNext),
            [&](uint32_t sType, size_t* size) noexcept {
                if (!RtPropertyStructSize(sType, size)) return false;
                if (sType == ST_AS_PROPERTIES) return !rtCaps.extAS || rtCaps.virtualAS;
                if (sType == ST_RTP_PROPERTIES) return !rtCaps.extRTP || rtCaps.virtualRTP;
                return false;
            });
        real(pd, pProperties);
    }
    FixupProperties(pd, &pProperties->properties);
    FixupPropertiesChain(pd, pProperties);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2* p) {
    vkGetPhysicalDeviceProperties2(pd, p);
}

void LogD5DeviceCapabilities(VkPhysicalDevice pd, const VkPhysicalDeviceFeatures2* features) {
    if (features && FirstTime("d5:core-feature-report")) {
        const VkPhysicalDeviceFeatures& f = features->features;
        LogLine("REAL D5 CORE FEATURES: shaderStorageImageExtendedFormats=%u shaderStorageImageReadWithoutFormat=%u shaderStorageImageWriteWithoutFormat=%u shaderSampledImageArrayDynamicIndexing=%u shaderStorageImageArrayDynamicIndexing=%u imageCubeArray=%u",
            f.shaderStorageImageExtendedFormats, f.shaderStorageImageReadWithoutFormat,
            f.shaderStorageImageWriteWithoutFormat, f.shaderSampledImageArrayDynamicIndexing,
            f.shaderStorageImageArrayDynamicIndexing, f.imageCubeArray);
    }

    auto getFormat = RealExport<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    auto getImageFormat = RealExport<PFN_vkGetPhysicalDeviceImageFormatProperties>("vkGetPhysicalDeviceImageFormatProperties");
    if (getFormat && FirstTime("d5:format-feature-probe")) {
        VkFormatProperties color{};
        getFormat(pd, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &color);
        LogLine("REAL D5 FORMAT B10G11R11_UFLOAT_PACK32 optimal=0x%08X sampled=%u storage=%u linearFilter=%u",
            (unsigned)color.optimalTilingFeatures,
            (color.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1u : 0u,
            (color.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? 1u : 0u,
            (color.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 1u : 0u);
        VkFormatProperties depth{};
        getFormat(pd, VK_FORMAT_D16_UNORM, &depth);
        LogLine("REAL D5 FORMAT D16_UNORM optimal=0x%08X sampled=%u depthAttachment=%u linearFilter=%u",
            (unsigned)depth.optimalTilingFeatures,
            (depth.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1u : 0u,
            (depth.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? 1u : 0u,
            (depth.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 1u : 0u);
    }
    if (getImageFormat && FirstTime("d5:image-format-probe")) {
        const VkFormat colorFormat = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        const VkImageUsageFlags usages[] = { VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT };
        const char* labels[] = { "SAMPLED", "STORAGE", "SAMPLED|STORAGE" };
        for (unsigned i = 0; i < 3; ++i) {
            VkImageFormatProperties p{};
            const VkResult r = getImageFormat(pd, colorFormat, VK_IMAGE_TYPE_3D,
                VK_IMAGE_TILING_OPTIMAL, usages[i], 0, &p);
            LogLine("REAL D5 IMAGE_FORMAT B10G11R11 3D usage=%s result=%d maxExtent=%u,%u,%u maxMip=%u sampleCounts=0x%X",
                labels[i], (int)r, p.maxExtent.width, p.maxExtent.height, p.maxExtent.depth,
                p.maxMipLevels, (unsigned)p.sampleCounts);
        }
        VkImageFormatProperties depthProps{};
        const VkResult depthResult = getImageFormat(pd, VK_FORMAT_D16_UNORM, VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            0, &depthProps);
        LogLine("REAL D5 IMAGE_FORMAT D16_UNORM 2D usage=SAMPLED|DEPTH result=%d maxExtent=%u,%u,%u maxMip=%u sampleCounts=0x%X",
            (int)depthResult, depthProps.maxExtent.width, depthProps.maxExtent.height,
            depthProps.maxExtent.depth, depthProps.maxMipLevels, (unsigned)depthProps.sampleCounts);
    }
}

// Legacy entry point: some engines read the core feature set from here (or mix it
// with Features2), so the uniform profile has to be applied to both.
extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures* pFeatures) {
    auto real = RealExport<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
    if (!real || !pFeatures) return;
    LoadSpvConfig();
    real(pd, pFeatures);
    ApplyUniformShaderProfile(pFeatures);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* pFeatures) {
    using Fn = PFN_vkGetPhysicalDeviceFeatures2;
    Fn real = RealExport<Fn>("vkGetPhysicalDeviceFeatures2");
    if (!real) real = RealExport<Fn>("vkGetPhysicalDeviceFeatures2KHR");
    if (!real || !pFeatures) return;
    LogChainOnce("Features2", pFeatures->pNext);
    const RtDeviceCaps rtCaps = GetRtDeviceCaps(pd);
    {
        PNextChainFilter filter;
        filter.Apply(reinterpret_cast<VkBaseOutStructure**>(&pFeatures->pNext),
            [&](uint32_t sType, size_t* size) noexcept {
                if (!RtFeatureStructSize(sType, size)) return false;
                if (sType == ST_AS_FEATURES) return !rtCaps.extAS || rtCaps.virtualAS;
                if (sType == ST_RTP_FEATURES) return !rtCaps.extRTP || rtCaps.virtualRTP;
                if (sType == ST_RAY_QUERY_FEATURES) return !rtCaps.extRQ || rtCaps.virtualRQ;
                if (sType == ST_RT_MAINTENANCE1_FEATURES)
                    return !rtCaps.extRTMaintenance1 || rtCaps.virtualRTP;
                return false;
            });
        real(pd, pFeatures);
    }
    LogD5DeviceCapabilities(pd, pFeatures);
    ApplyUniformShaderProfile(&pFeatures->features);
    FixupFeaturesChain(pd, pFeatures);
}

extern "C" VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2* p) {
    vkGetPhysicalDeviceFeatures2(pd, p);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char* pLayerName,
    uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<VkExtensionProperties> realExts;
    if (!QueryRealExtensions(pd, pLayerName, realExts)) return VK_ERROR_INITIALIZATION_FAILED;

    const RtDeviceCaps rtCaps = GetRtDeviceCaps(pd);
    if (pLayerName == nullptr) {
        char key[64];
        sprintf_s(key, sizeof(key), "extlist:%p", static_cast<void*>(pd));
        if (FirstTime(key)) {
            LogLine("REAL device extensions (%u):", static_cast<unsigned>(realExts.size()));
            for (const auto& e : realExts) LogLine("    %s v%u", e.extensionName, e.specVersion);
        }
    }

    std::vector<VkExtensionProperties> all;
    all.reserve(realExts.size() + kFakeExtensionCount);
    for (const auto& e : realExts) {
        if (pLayerName == nullptr && ShouldVirtualizeRtExtension(e.extensionName, rtCaps)) {
            if (FirstTime(std::string("mask-ext:") + e.extensionName))
                LogLine("RT COMPAT: masking physical extension %s; proxy will virtualize it", e.extensionName);
            continue;
        }
        all.push_back(e);
    }

    if (pLayerName == nullptr) {
        for (const char* name : kFakeExtensions) {
            if (!ShouldVirtualizeRtExtension(name, rtCaps)) continue;
            bool already = false;
            for (const auto& e : all) if (strcmp(e.extensionName, name) == 0) { already = true; break; }
            if (already) continue;
            VkExtensionProperties ep{};
            strncpy_s(ep.extensionName, VK_MAX_EXTENSION_NAME_SIZE, name, _TRUNCATE);
            ep.specVersion = 1;
            all.push_back(ep);
            if (FirstTime(std::string("virtual-ext:") + name))
                LogLine("RT COMPAT: advertising virtual extension %s", name);
        }
    }

    const uint32_t total = static_cast<uint32_t>(all.size());
    if (!pProperties) { *pPropertyCount = total; return VK_SUCCESS; }
    const uint32_t cap = *pPropertyCount;
    const uint32_t n = cap < total ? cap : total;
    if (n) memcpy(pProperties, all.data(), n * sizeof(VkExtensionProperties));
    *pPropertyCount = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto real = RealExport<PFN_vkCreateDevice>("vkCreateDevice");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    LoadSpvConfig();

    const RtDeviceCaps rtCaps = GetRtDeviceCaps(pd);
    LogLine("vkCreateDevice: RT tier=%s mode=%ld appExtensions=%u", RtTierName(rtCaps.tier), (long)GetRtCompatMode(), pCreateInfo->enabledExtensionCount);
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
        LogLine("  REQUESTED EXT: %s", pCreateInfo->ppEnabledExtensionNames[i]);
    LogChainOnce("DeviceCreateInfo", pCreateInfo->pNext);

    // Do not send RT feature structs to the ICD when their extension is absent,
    // even in strict Native mode. Some applications build broad pNext chains
    // and forget to remove structures for unsupported extensions.
    bool hasUnsupportedRtFeatureStruct = false;
    {
        const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
        unsigned guard = 0;
        while (node && guard++ < 128) {
            const uint32_t st = static_cast<uint32_t>(node->sType);
            if ((st == ST_AS_FEATURES && (!rtCaps.extAS || rtCaps.virtualAS)) ||
                (st == ST_RTP_FEATURES && (!rtCaps.extRTP || rtCaps.virtualRTP)) ||
                (st == ST_RAY_QUERY_FEATURES && (!rtCaps.extRQ || rtCaps.virtualRQ)) ||
                (st == ST_RT_MAINTENANCE1_FEATURES &&
                    (!rtCaps.extRTMaintenance1 || rtCaps.virtualRTP))) {
                hasUnsupportedRtFeatureStruct = true;
                break;
            }
            node = node->pNext;
        }
    }

    // A fully native adapter is passed through unchanged only when its pNext
    // chain also contains no RT structure the selected ICD cannot consume.
    // Do not switch off the process-wide fallback: another adapter may need it.
    if (!hasUnsupportedRtFeatureStruct &&
        !rtCaps.virtualAS && !rtCaps.virtualRTP && !rtCaps.virtualRQ &&
        !rtCaps.virtualDeferred && !rtCaps.virtualPipelineLibrary && !rtCaps.virtualDynamicRendering) {
        VkResult nativeResult = real(pd, pCreateInfo, pAllocator, pDevice);
        LogLine("vkCreateDevice native pass-through result=%d", (int)nativeResult);
        if (nativeResult == VK_SUCCESS && pDevice) {
            g_lastDevice = *pDevice;
            RegisterActiveRtDevice(*pDevice, rtCaps);
            InstallCrashHandlerFirst();
            g_createdFamilies.clear();
            for (uint32_t qi = 0; qi < pCreateInfo->queueCreateInfoCount; ++qi)
                g_createdFamilies.push_back({ pCreateInfo->pQueueCreateInfos[qi].queueFamilyIndex, pCreateInfo->pQueueCreateInfos[qi].queueCount });
        }
        return nativeResult;
    }

    std::vector<VkExtensionProperties> realExts;
    if (!QueryRealExtensions(pd, nullptr, realExts)) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo ci = *pCreateInfo;
    std::vector<const char*> exts;
    exts.reserve(ci.enabledExtensionCount);
    for (uint32_t i = 0; i < ci.enabledExtensionCount; ++i) {
        const char* n = ci.ppEnabledExtensionNames[i];
        if (ShouldVirtualizeRtExtension(n, rtCaps)) {
            LogLine("  VIRTUALIZED (not forwarded to ICD): %s", n);
            continue;
        }
        if (IsFakeExtension(n) && !HasExt(realExts, n)) {
            LogLine("  REJECTED: requested compatibility extension was not advertised/virtualized: %s", n);
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
        exts.push_back(n);
    }
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

    struct Unlink { VkBaseOutStructure** link; VkBaseOutStructure* node; };
    struct Saved  { VkBaseOutStructure* node; std::vector<uint8_t> bytes; };
    std::vector<Unlink> unlinked;
    std::vector<Saved> saved;

    VkPhysicalDeviceFeatures localCore{};
    if (ci.pEnabledFeatures) {
        localCore = *ci.pEnabledFeatures;
        ClampCoreFeatures(pd, &localCore, "pEnabledFeatures");
        ApplyUniformShaderProfile(&localCore);
        ci.pEnabledFeatures = &localCore;
    }

    VkBaseOutStructure** link = reinterpret_cast<VkBaseOutStructure**>(const_cast<void**>(&ci.pNext));
    unsigned guard = 0;
    while (*link && guard++ < 128) {
        VkBaseOutStructure* node = *link;
        const uint32_t st = static_cast<uint32_t>(node->sType);

        bool drop = false;
        if (st == ST_AS_FEATURES && (!rtCaps.extAS || rtCaps.virtualAS)) drop = true;
        if (st == ST_RTP_FEATURES && (!rtCaps.extRTP || rtCaps.virtualRTP)) drop = true;
        if (st == ST_RAY_QUERY_FEATURES && (!rtCaps.extRQ || rtCaps.virtualRQ)) drop = true;
        if (st == ST_RT_MAINTENANCE1_FEATURES &&
            (!rtCaps.extRTMaintenance1 || rtCaps.virtualRTP)) drop = true;
        for (const DropInfo& d : kDropIfExtMissing)
            if (d.sType == st && !HasExt(realExts, d.ext)) drop = true;
        if (drop) {
            LogLine("  UNLINKED virtual feature struct sType=%u %s", st, KnownSTypeName(st));
            unlinked.push_back({ link, node });
            *link = node->pNext;
            continue;
        }

        // Preserve proxy-emulated core 1.3 feature structs exactly as in v13.
        if (st == ST_VULKAN13_FEATURES || st == ST_DYNAMIC_RENDERING_FEAT ||
            st == ST_SYNCHRONIZATION2_FEAT || st == ST_MAINTENANCE4_FEAT) {
            LogLine("  PRESERVED feature struct sType=%u %s (proxy-emulated)", st, KnownSTypeName(st));
            link = &node->pNext;
            continue;
        }

        if (st == static_cast<uint32_t>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
            auto* f2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(node);
            Saved sv;
            sv.node = node;
            sv.bytes.assign(reinterpret_cast<uint8_t*>(node), reinterpret_cast<uint8_t*>(node) + sizeof(VkPhysicalDeviceFeatures2));
            saved.push_back(std::move(sv));
            ClampCoreFeatures(pd, &f2->features, "Features2.features");
            ApplyUniformShaderProfile(&f2->features);
        } else {
            for (const ClampInfo& c : kClampable) {
                if (c.sType == st) {
                    Saved sv;
                    sv.node = node;
                    sv.bytes.assign(reinterpret_cast<uint8_t*>(node), reinterpret_cast<uint8_t*>(node) + c.size);
                    saved.push_back(std::move(sv));
                    ClampToReal(pd, node, c);
                    break;
                }
            }
        }
        link = &node->pNext;
    }

    VkResult r = real(pd, &ci, pAllocator, pDevice);

    for (size_t i = unlinked.size(); i-- > 0;) *unlinked[i].link = unlinked[i].node;
    for (auto& sv : saved) {
        VkBaseOutStructure* keepNext = sv.node->pNext;
        memcpy(sv.node, sv.bytes.data(), sv.bytes.size());
        sv.node->pNext = keepNext;
    }

    LogLine("vkCreateDevice (compat sanitized) result=%d; RT tier=%s", (int)r, RtTierName(rtCaps.tier));
    if (r == VK_SUCCESS && pDevice) {
        g_lastDevice = *pDevice;
        RegisterActiveRtDevice(*pDevice, rtCaps);
        InstallCrashHandlerFirst();
        g_createdFamilies.clear();
        for (uint32_t qi = 0; qi < pCreateInfo->queueCreateInfoCount; ++qi)
            g_createdFamilies.push_back({ pCreateInfo->pQueueCreateInfos[qi].queueFamilyIndex, pCreateInfo->pQueueCreateInfos[qi].queueCount });
    }
    return r;
}

// ---------------------------------------------------------------------------
// Loader entry points (definitions)
// ---------------------------------------------------------------------------
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Proxy_vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* pFences,
    VkBool32 waitAll, uint64_t timeout) {
    auto real = RealExport<PFN_vkWaitForFences>("vkWaitForFences");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 128 || (id % 256) == 0);
    SetJob("vkWaitForFences id=%ld fenceCount=%u waitAll=%u timeout=%llu",
        (long)id, fenceCount, (unsigned)waitAll, (unsigned long long)timeout);
    Crumb("%s", t_job);
    if (trace) LogLine("SYNC WAIT_FENCES ENTER id=%ld thread=%lu count=%u waitAll=%u timeout=%llu",
        (long)id, (unsigned long)GetCurrentThreadId(), fenceCount, (unsigned)waitAll,
        (unsigned long long)timeout);
    const ULONGLONG start = GetTickCount64();
    VkResult result = real(device, fenceCount, pFences, waitAll, timeout);
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (trace || result != VK_SUCCESS || elapsed >= 250)
        LogLine("SYNC WAIT_FENCES EXIT id=%ld result=%d elapsed=%llu ms",
            (long)id, (int)result, (unsigned long long)elapsed);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Proxy_vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout) {
    auto real = RealExport<PFN_vkWaitSemaphores>("vkWaitSemaphores");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    const bool trace = (id <= 128 || (id % 256) == 0);
    SetJob("vkWaitSemaphores id=%ld count=%u timeout=%llu",
        (long)id, pWaitInfo ? pWaitInfo->semaphoreCount : 0u, (unsigned long long)timeout);
    Crumb("%s", t_job);
    if (trace) LogLine("SYNC WAIT_SEMAPHORES ENTER id=%ld thread=%lu count=%u timeout=%llu",
        (long)id, (unsigned long)GetCurrentThreadId(),
        pWaitInfo ? pWaitInfo->semaphoreCount : 0u, (unsigned long long)timeout);
    const ULONGLONG start = GetTickCount64();
    VkResult result = real(device, pWaitInfo, timeout);
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (trace || result != VK_SUCCESS || elapsed >= 250)
        LogLine("SYNC WAIT_SEMAPHORES EXIT id=%ld result=%d elapsed=%llu ms",
            (long)id, (int)result, (unsigned long long)elapsed);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Proxy_vkQueueWaitIdle(VkQueue queue) {
    auto real = RealExport<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    SetJob("vkQueueWaitIdle id=%ld queue=%p", (long)id, (void*)queue);
    Crumb("%s", t_job);
    LogLine("SYNC QUEUE_WAIT_IDLE ENTER id=%ld queue=%p", (long)id, (void*)queue);
    const ULONGLONG start = GetTickCount64();
    VkResult result = real(queue);
    const ULONGLONG elapsed = GetTickCount64() - start;
    LogLine("SYNC QUEUE_WAIT_IDLE EXIT id=%ld result=%d elapsed=%llu ms",
        (long)id, (int)result, (unsigned long long)elapsed);
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL
Proxy_vkDeviceWaitIdle(VkDevice device) {
    auto real = RealExport<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    static volatile LONG sequence = 0;
    const LONG id = InterlockedIncrement(&sequence);
    SetJob("vkDeviceWaitIdle id=%ld device=%p", (long)id, (void*)device);
    Crumb("%s", t_job);
    LogLine("SYNC DEVICE_WAIT_IDLE ENTER id=%ld device=%p", (long)id, (void*)device);
    const ULONGLONG start = GetTickCount64();
    VkResult result = real(device);
    const ULONGLONG elapsed = GetTickCount64() - start;
    LogLine("SYNC DEVICE_WAIT_IDLE EXIT id=%ld result=%d elapsed=%llu ms",
        (long)id, (int)result, (unsigned long long)elapsed);
    return result;
}

namespace {
    // Expanded: every intercepted function must be returned via GIPA/GDPA.
    // Returning nullptr was a primary cause of the EXEC@0 BagSplat crash.
    PFN_vkVoidFunction LocalOverride(const char* name) {
        if (!name) return nullptr;
        if (!strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
        if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
        if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetDeviceProcAddr);
        if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return reinterpret_cast<PFN_vkVoidFunction>(&vkEnumerateDeviceExtensionProperties);
        if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties);
        if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceProperties2);
        if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures2);
        if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))
            return reinterpret_cast<PFN_vkVoidFunction>(&vkGetPhysicalDeviceFeatures);
        if (!strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateDevice);
        if (!strcmp(name, "vkQueueSubmit")) {
            if (FirstTime("sync-proc:vkQueueSubmit"))
                LogLine("SYNC HOOK_RESOLVED vkQueueSubmit -> Proxy_vkQueueSubmit");
            return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkQueueSubmit);
        }
        if (!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) {
            if (FirstTime(std::string("sync-proc:") + name))
                LogLine("SYNC HOOK_RESOLVED %s -> vkQueueSubmit2 (legacy-submit translation)", name);
            return reinterpret_cast<PFN_vkVoidFunction>(&vkQueueSubmit2);
        }
        if (!strcmp(name, "vkWaitForFences")) return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkWaitForFences);
        if (!strcmp(name, "vkWaitSemaphores") || !strcmp(name, "vkWaitSemaphoresKHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkWaitSemaphores);
        if (!strcmp(name, "vkQueueWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkQueueWaitIdle);
        if (!strcmp(name, "vkDeviceWaitIdle")) return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkDeviceWaitIdle);
        if (!strcmp(name, "vkDestroyShaderModule")) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyShaderModule);
        if (!strcmp(name, "vkCreateShaderModule")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateShaderModule);
        if (!strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateGraphicsPipelines);
        if (!strcmp(name, "vkCreateComputePipelines")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateComputePipelines);
        if (!strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyPipeline);
        if (!strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindPipeline);
        if (!strcmp(name, "vkCmdBindDescriptorSets")) {
            if (FirstTime("spvtrace-proc:vkCmdBindDescriptorSets"))
                LogLine("SPVTRACE HOOK_RESOLVED vkCmdBindDescriptorSets");
            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBindDescriptorSets);
        }
        if (!strcmp(name, "vkUpdateDescriptorSets")) return reinterpret_cast<PFN_vkVoidFunction>(&vkUpdateDescriptorSets);
        if (!strcmp(name, "vkCreateImageView")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateImageView);
        if (!strcmp(name, "vkDestroyImageView")) return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyImageView);
        if (!strcmp(name, "vkAllocateCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateCommandBuffers);
        if (!strcmp(name, "vkFreeCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(&vkFreeCommandBuffers);
        if (!strcmp(name, "vkAllocateDescriptorSets")) return reinterpret_cast<PFN_vkVoidFunction>(&vkAllocateDescriptorSets);
        if (!strcmp(name, "vkBeginCommandBuffer")) return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkBeginCommandBuffer);
        if (!strcmp(name, "vkCmdDraw")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDraw);
        if (!strcmp(name, "vkCmdDrawIndexed")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDrawIndexed);
        if (!strcmp(name, "vkCmdDrawIndirect")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDrawIndirect);
        if (!strcmp(name, "vkCmdDrawIndexedIndirect")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDrawIndexedIndirect);
        if (!strcmp(name, "vkCmdDispatch")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDispatch);
        if (!strcmp(name, "vkCmdDispatchBase") || !strcmp(name, "vkCmdDispatchBaseKHR")) {
            if (FirstTime(std::string("spvtrace-proc:") + name))
                LogLine("SPVTRACE HOOK_RESOLVED %s -> Proxy_vkCmdDispatchBase", name);
            return reinterpret_cast<PFN_vkVoidFunction>(&Proxy_vkCmdDispatchBase);
        }
        if (!strcmp(name, "vkCmdDispatchIndirect")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdDispatchIndirect);
        if (!strcmp(name, "vkQueuePresentKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&vkQueuePresentKHR);
        if (!strcmp(name, "vkCreateSwapchainKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateSwapchainKHR);
        if (!strcmp(name, "vkCmdBeginRendering") || !strcmp(name, "vkCmdBeginRenderingKHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdBeginRendering);
        if (!strcmp(name, "vkCmdEndRendering") || !strcmp(name, "vkCmdEndRenderingKHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdEndRendering);
        // Optional debug utils - engines sometimes call without null-check
        if (!strcmp(name, "vkCreateDebugUtilsMessengerEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_CreateDebugUtilsMessengerEXT);
        if (!strcmp(name, "vkDestroyDebugUtilsMessengerEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_DestroyDebugUtilsMessengerEXT);
        if (!strcmp(name, "vkSubmitDebugUtilsMessageEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_SubmitDebugUtilsMessageEXT);
        if (!strcmp(name, "vkSetDebugUtilsObjectNameEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_SetDebugUtilsObjectNameEXT);
        if (!strcmp(name, "vkSetDebugUtilsObjectTagEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_SetDebugUtilsObjectTagEXT);
        if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_CmdBeginDebugUtilsLabelEXT);
        if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_CmdEndDebugUtilsLabelEXT);
        if (!strcmp(name, "vkCmdInsertDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_CmdInsertDebugUtilsLabelEXT);
        if (!strcmp(name, "vkQueueBeginDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_QueueBeginDebugUtilsLabelEXT);
        if (!strcmp(name, "vkQueueEndDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_QueueEndDebugUtilsLabelEXT);
        if (!strcmp(name, "vkQueueInsertDebugUtilsLabelEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_QueueInsertDebugUtilsLabelEXT);
        if (!strcmp(name, "vkCreateDebugReportCallbackEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_CreateDebugReportCallbackEXT);
        if (!strcmp(name, "vkDestroyDebugReportCallbackEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_DestroyDebugReportCallbackEXT);
        if (!strcmp(name, "vkDebugReportMessageEXT")) return reinterpret_cast<PFN_vkVoidFunction>(&Stub_DebugReportMessageEXT);
        return nullptr;
    }
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName || !EnsureSystemLoader()) return nullptr;

    if (IsRayTracingProc(pName)) {
        if (CanUseNativeRtProc(VK_NULL_HANDLE, pName, true)) {
            PFN_vkVoidFunction native = g_realGipa(instance, pName);
            if (native) {
                if (FirstTime(std::string("native-rt-gipa:") + pName)) LogLine("NATIVE RT via GIPA: %s", pName);
                return native;
            }
        }
        if (CanUseRtFallback(VK_NULL_HANDLE, pName, true)) {
            if (PFN_vkVoidFunction fallback = stub::Find(pName)) {
                if (FirstTime(std::string("stub:") + pName)) LogLine("PROXY RT fallback via GIPA: %s", pName);
                return fallback;
            }
        }
        // Do not return a driver address for an RT component that the selected
        // compatibility tier deliberately hid from the application.
        return nullptr;
    }

    if (PFN_vkVoidFunction s = stub::Find(pName)) {
        if (FirstTime(std::string("stub:") + pName)) LogLine("PROXY served via GIPA: %s", pName);
        return s;
    }
    if (PFN_vkVoidFunction o = LocalOverride(pName)) return o;

    PFN_vkVoidFunction realFunction = g_realGipa(instance, pName);
    if (!realFunction && g_realLoader)
        realFunction = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_realLoader, pName));
    if (!realFunction && g_thisModule) {
        realFunction = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_thisModule, pName));
        if (realFunction && FirstTime(std::string("gipa-self:") + pName))
            LogLine("PROXY served via own export (GIPA fallback): %s", pName);
    }
    if (!realFunction && pName) {
        const char* aliases[][2] = {
            { "vkGetPhysicalDeviceFormatProperties2KHR", "vkGetPhysicalDeviceFormatProperties2" },
            { "vkGetPhysicalDeviceImageFormatProperties2KHR", "vkGetPhysicalDeviceImageFormatProperties2" },
            { "vkGetPhysicalDeviceQueueFamilyProperties2KHR", "vkGetPhysicalDeviceQueueFamilyProperties2" },
            { "vkGetPhysicalDeviceMemoryProperties2KHR", "vkGetPhysicalDeviceMemoryProperties2" },
            { "vkGetPhysicalDeviceSparseImageFormatProperties2KHR", "vkGetPhysicalDeviceSparseImageFormatProperties2" },
            { "vkGetPhysicalDeviceProperties2KHR", "vkGetPhysicalDeviceProperties2" },
            { "vkGetPhysicalDeviceFeatures2KHR", "vkGetPhysicalDeviceFeatures2" },
            { "vkEnumeratePhysicalDeviceGroupsKHR", "vkEnumeratePhysicalDeviceGroups" },
            { "vkGetDeviceBufferMemoryRequirementsKHR", "vkGetDeviceBufferMemoryRequirements" },
            { "vkGetDeviceImageMemoryRequirementsKHR", "vkGetDeviceImageMemoryRequirements" },
            { "vkGetDeviceImageSparseMemoryRequirementsKHR", "vkGetDeviceImageSparseMemoryRequirements" },
            { "vkCmdBeginRenderingKHR", "vkCmdBeginRendering" },
            { "vkCmdEndRenderingKHR", "vkCmdEndRendering" },
            { "vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2" },
            { "vkCmdWriteTimestamp2KHR", "vkCmdWriteTimestamp2" },
            { "vkQueueSubmit2KHR", "vkQueueSubmit2" },
            { nullptr, nullptr }
        };
        for (int i = 0; aliases[i][0]; ++i) {
            if (!strcmp(pName, aliases[i][0])) {
                if (g_thisModule) realFunction = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_thisModule, aliases[i][1]));
                if (!realFunction && g_realLoader) realFunction = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_realLoader, aliases[i][1]));
                if (!realFunction) realFunction = g_realGipa(instance, aliases[i][1]);
                if (realFunction && FirstTime(std::string("gipa-alias:") + pName))
                    LogLine("PROXY served via alias %s -> %s", pName, aliases[i][1]);
                break;
            }
        }
    }
    if (!realFunction && !strncmp(pName, "vkCmdSet", 8)) {
        if (FirstTime(std::string("noop-set:") + pName))
            LogLine("!!! MISSING dynamic-state %s -> NO-OP (state NOT applied!)", pName);
        return reinterpret_cast<PFN_vkVoidFunction>(&Stub_NoopCmd);
    }
    if (!realFunction && FirstTime(std::string("gipa-null:") + pName))
        LogLine("GIPA returned NULL for %s", pName);
    return realFunction;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName || !EnsureSystemLoader()) return nullptr;

    if (IsRayTracingProc(pName)) {
        if (CanUseNativeRtProc(device, pName, false)) {
            PFN_vkVoidFunction native = g_realGdpa(device, pName);
            if (native) {
                if (FirstTime(std::string("native-rt-gdpa:") + pName)) LogLine("NATIVE RT via GDPA: %s", pName);
                return native;
            }
        }
        if (CanUseRtFallback(device, pName, false)) {
            if (PFN_vkVoidFunction fallback = stub::Find(pName)) {
                if (FirstTime(std::string("stub:") + pName)) LogLine("PROXY RT fallback via GDPA: %s", pName);
                return fallback;
            }
        }
        if (FirstTime(std::string("gdpa-rt-null:") + pName))
            LogLine("GDPA RT command unavailable in selected tier: %s", pName);
        return nullptr;
    }

    if (PFN_vkVoidFunction s = stub::Find(pName)) {
        if (FirstTime(std::string("stub:") + pName)) LogLine("PROXY served via GDPA: %s", pName);
        return s;
    }
    if (PFN_vkVoidFunction o = LocalOverride(pName)) return o;

    PFN_vkVoidFunction result = g_realGdpa(device, pName);
    if (!result && g_realLoader) result = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_realLoader, pName));
    if (!result && g_thisModule) {
        result = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_thisModule, pName));
        if (result && FirstTime(std::string("gdpa-self:") + pName))
            LogLine("PROXY served via own export (GDPA fallback): %s", pName);
    }
    if (!result && pName) {
        const char* aliases[][2] = {
            { "vkCmdBeginRenderingKHR", "vkCmdBeginRendering" },
            { "vkCmdEndRenderingKHR", "vkCmdEndRendering" },
            { "vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2" },
            { "vkCmdWriteTimestamp2KHR", "vkCmdWriteTimestamp2" },
            { "vkQueueSubmit2KHR", "vkQueueSubmit2" },
            { "vkGetDeviceBufferMemoryRequirementsKHR", "vkGetDeviceBufferMemoryRequirements" },
            { "vkGetDeviceImageMemoryRequirementsKHR", "vkGetDeviceImageMemoryRequirements" },
            { "vkGetDeviceImageSparseMemoryRequirementsKHR", "vkGetDeviceImageSparseMemoryRequirements" },
            { "vkCmdDrawIndirectCountKHR", "vkCmdDrawIndirectCount" },
            { "vkCmdDrawIndexedIndirectCountKHR", "vkCmdDrawIndexedIndirectCount" },
            { nullptr, nullptr }
        };
        for (int i = 0; aliases[i][0]; ++i) {
            if (!strcmp(pName, aliases[i][0])) {
                if (g_thisModule) result = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_thisModule, aliases[i][1]));
                if (!result && g_realLoader) result = reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(g_realLoader, aliases[i][1]));
                if (!result) result = g_realGdpa(device, aliases[i][1]);
                if (result && FirstTime(std::string("gdpa-alias:") + pName))
                    LogLine("PROXY served via alias %s -> %s", pName, aliases[i][1]);
                break;
            }
        }
    }
    if (!result && FirstTime(std::string("gdpa-null:") + pName))
        LogLine("GDPA returned NULL for %s", pName);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // Process is terminating: show what the game was doing last.
        LogLine("DLL_PROCESS_DETACH: process is exiting. Last API breadcrumbs:");
        const LONG end = g_crumbIdx;
        for (int k = kCrumbCount - 16; k < kCrumbCount; ++k) {
            const char* c = g_crumbs[(end + k) & (kCrumbCount - 1)];
            if (c[0]) LogLine("  %s", c);
        }
    }
    if (reason == DLL_PROCESS_ATTACH) {
        g_thisModule = instance;
        DisableThreadLibraryCalls(instance);
        InstallCrashHandlerFirst();
    }
    return TRUE;
}