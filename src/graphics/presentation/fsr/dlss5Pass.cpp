#include "graphics/presentation/fsr/dlss5Pass.h"

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/bindEpoch.h"
#include "graphics/presentation/fsr/dlss5Math.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if KYTY_ENABLE_DLSS && defined(_WIN32)
#define KYTY_DLSS5_PASS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_params.h"
#include "nvsdk_ngx_vk.h"
#else
#define KYTY_DLSS5_PASS 0
#endif

namespace Libs::Graphics::Dlss5 {

#if KYTY_DLSS5_PASS
namespace {

// NGX feature 18 ("Neural Rendering"; NVSDK_NGX_Feature_Reserved18 in SDK 310.9.16).
constexpr int      kFeatureNeuralRendering = 18;
constexpr int      kNgxSuccess             = 1; // NVSDK_NGX_Result_Success
constexpr VkFormat kWorkingFormat          = VK_FORMAT_R16G16B16A16_SFLOAT;
// The bridge's depth and motion formats (dlssBridge.cpp kFormats).
constexpr VkFormat kDepthFormat  = VK_FORMAT_R32_SFLOAT;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr uint32_t kTimingSlots  = 8;

using FwdLoad     = int (*)(const wchar_t*);
using FwdInit     = int (*)(const wchar_t*, void*, void*, void*, int);
using FwdCreate   = int (*)(void*, int, void*, void**);
using FwdEvaluate = int (*)(void*, void*, void*);

auto& Vk() {
	return VULKAN_HPP_DEFAULT_DISPATCHER;
}

bool SwitchOn(const char* name) {
	const char* value = std::getenv(name);
	return value != nullptr && std::strcmp(value, "1") == 0;
}

std::atomic<bool> g_failed {false};

[[gnu::noinline]] void Fail(const std::string& reason) {
	if (!g_failed.exchange(true)) {
		std::printf("NHL27DLSS5 status=off reason=%s\n", reason.c_str());
		std::fflush(stdout);
	}
}

std::string Hex(int value) {
	char text[16];
	std::snprintf(text, sizeof(text), "0x%x", static_cast<unsigned int>(value));
	return text;
}

bool Wanted() {
	static const bool wanted = SwitchOn("KYTY_DLSS5") && SwitchOn("KYTY_DLSS");
	return wanted;
}

std::wstring ExeDir() {
	std::wstring path(32768, L'\0');
	const DWORD  length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	const auto slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring {} : path.substr(0, slash);
}

// --- Loading (once, the early warm-up or the first use) ---

struct Forwarder {
	std::mutex  mutex;
	bool        tried    = false;
	bool        ok       = false;
	FwdInit     init     = nullptr;
	FwdCreate   create   = nullptr;
	FwdEvaluate evaluate = nullptr;
};
Forwarder g_forwarder;

[[gnu::noinline]] bool Load() {
	std::lock_guard lock(g_forwarder.mutex);
	if (g_forwarder.tried) return g_forwarder.ok;
	g_forwarder.tried = true;
	const std::wstring dir = ExeDir();
	// The model: KYTY_DLSS5_MODEL_PATH (absolute), else next to the exe. The log names only the source.
	std::wstring   model;
	const char*    source = "exe-folder";
	const wchar_t* env    = _wgetenv(L"KYTY_DLSS5_MODEL_PATH");
	if (env != nullptr && env[0] != L'\0') {
		if (!Dlss5Math::IsAbsoluteWindowsPath(env)) {
			Fail("KYTY_DLSS5_MODEL_PATH is not an absolute path");
			return false;
		}
		model  = env;
		source = "KYTY_DLSS5_MODEL_PATH";
	} else {
		model = dir + L"\\nvngx_dlssnr.dll";
	}
	if (GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES) {
		Fail(std::string("model file not found (") + source + ")");
		return false;
	}
	const std::wstring forwarder_path = dir + L"\\nvngx.dll_dlssnr.dll";
	HMODULE forwarder = LoadLibraryExW(forwarder_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (forwarder == nullptr) {
		Fail("nvngx.dll_dlssnr.dll did not load (error " + std::to_string(GetLastError()) + ")");
		return false;
	}
	const auto load = reinterpret_cast<FwdLoad>(GetProcAddress(forwarder, "FaceoffNrLoad"));
	g_forwarder.init     = reinterpret_cast<FwdInit>(GetProcAddress(forwarder, "FaceoffNrInit"));
	g_forwarder.create   = reinterpret_cast<FwdCreate>(GetProcAddress(forwarder, "FaceoffNrCreate"));
	g_forwarder.evaluate = reinterpret_cast<FwdEvaluate>(GetProcAddress(forwarder, "FaceoffNrEvaluate"));
	if (load == nullptr || g_forwarder.init == nullptr || g_forwarder.create == nullptr ||
	    g_forwarder.evaluate == nullptr) {
		Fail("the forwarder is missing exports");
		return false;
	}
	const auto start   = std::chrono::steady_clock::now();
	const int  mask    = load(model.c_str());
	const auto loading = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
	std::printf("NHL27DLSS5 status=load model=%s entry_points=0x%x ms=%lld\n", source, static_cast<unsigned>(mask),
	            static_cast<long long>(loading.count()));
	std::fflush(stdout);
	if (mask != 0xf) {
		Fail(mask == 0 ? "the model did not load" : "the model lacks the Vulkan entry points (" + Hex(mask) + ")");
		return false;
	}
	g_forwarder.ok = true;
	return true;
}

// --- Images (each lives until the process exits) ---

struct OwnedImage {
	VkImage        image  = VK_NULL_HANDLE;
	VkImageView    view   = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool CreateImage(GraphicContext& graphics, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                 OwnedImage& out) {
	const auto        device = static_cast<VkDevice>(graphics.device);
	VkImageCreateInfo info {};
	info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType     = VK_IMAGE_TYPE_2D;
	info.format        = format;
	info.extent        = {extent.width, extent.height, 1};
	info.mipLevels     = 1;
	info.arrayLayers   = 1;
	info.samples       = VK_SAMPLE_COUNT_1_BIT;
	info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	info.usage         = usage;
	info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (Vk().vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
	VkMemoryRequirements requirements {};
	Vk().vkGetImageMemoryRequirements(device, out.image, &requirements);
	VkPhysicalDeviceMemoryProperties memory_properties {};
	Vk().vkGetPhysicalDeviceMemoryProperties(static_cast<VkPhysicalDevice>(graphics.physical_device),
	                                         &memory_properties);
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory_properties.memoryTypeCount && type == UINT32_MAX; ++i) {
		if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
		    (memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
			type = i;
		}
	}
	if (type == UINT32_MAX) return false;
	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	if (Vk().vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) return false;
	if (Vk().vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) return false;
	VkImageViewCreateInfo view {};
	view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image            = out.image;
	view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
	view.format           = format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return Vk().vkCreateImageView(device, &view, nullptr, &out.view) == VK_SUCCESS;
}

bool Blittable(GraphicContext& graphics, VkFormat format, VkFormatFeatureFlags needed) {
	VkFormatProperties properties {};
	Vk().vkGetPhysicalDeviceFormatProperties(static_cast<VkPhysicalDevice>(graphics.physical_device), format,
	                                         &properties);
	return (properties.optimalTilingFeatures & needed) == needed;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                                  VkAccessFlags dst_access) {
	VkImageMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask       = src_access;
	barrier.dstAccessMask       = dst_access;
	barrier.oldLayout           = from;
	barrier.newLayout           = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image               = image;
	barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return barrier;
}

VkImageBlit FullBlit(VkExtent2D from, VkExtent2D to) {
	VkImageBlit region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.srcOffsets[1]  = {static_cast<int32_t>(from.width), static_cast<int32_t>(from.height), 1};
	region.dstOffsets[1]  = {static_cast<int32_t>(to.width), static_cast<int32_t>(to.height), 1};
	return region;
}

constexpr VkAccessFlags kAnyAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

// --- What the bridge thread and the present thread share (under mutex) ---

struct Shared {
	std::mutex           mutex;
	bool                 sr_seen = false; // an SR frame came before the size was known
	VkExtent2D           target {}; // the pass's size, set once by the present thread (TargetSize)
	bool                 created = false;
	OwnedImage           depth, motion;
	VkExtent2D           size {}; // of depth and motion: the target
	NVSDK_NGX_Parameter* parameters = nullptr; // allocated on the bridge thread, beside SR's block
	// Recorded into the bridge's command buffer, not yet submitted.
	bool  pending       = false;
	float pending_mv_x  = 0;
	float pending_mv_y  = 0;
	bool  pending_reset = false;
	// Submitted.
	uint64_t serial = 0;
	float    mv_x   = 0;
	float    mv_y   = 0;
	bool     reset  = false; // sticky until the present thread takes it
};
Shared g_shared;

// --- The present thread's own state (the presenter's mutex serializes RecordPresent) ---

struct Present {
	bool                  initialised = false;
	OwnedImage            input, output;
	VkExtent2D            size {};
	void*                 feature   = nullptr;
	float                 intensity = 1.0f;
	uint64_t              last_picture        = 0;
	bool                  output_valid        = false;
	uint64_t              last_inputs         = 0;
	uint32_t              presents_since_new  = 0;
	bool                  reset_next          = true;
	bool                  format_skip_logged  = false;
	bool                  hdr_skip_logged     = false;
	VkFormat              filter_format       = VK_FORMAT_UNDEFINED;
	bool                  filter_linear       = false;
	uint64_t              frames              = 0;
	NVSDK_NGX_Resource_VK color_resource {}, depth_resource {}, motion_resource {}, output_resource {};
	// GPU time of the copy and the evaluate, from timestamps read kTimingSlots presents later.
	VkQueryPool                          timing = VK_NULL_HANDLE;
	double                               timestamp_ms = 0;
	uint64_t                             timestamp_mask = 0;
	std::array<bool, kTimingSlots>       written {};
	double                               window_ms      = 0;
	uint32_t                             window_samples = 0;
	uint64_t                             window_frames  = 0;
	std::chrono::steady_clock::time_point next_report {};
};
Present g_present;

void SetUI(NVSDK_NGX_Parameter* parameters, const char* name, unsigned int value) {
	NVSDK_NGX_Parameter_SetUI(parameters, name, value);
}
void SetF(NVSDK_NGX_Parameter* parameters, const char* name, float value) {
	NVSDK_NGX_Parameter_SetF(parameters, name, value);
}

// The model's controls: read when the feature is built, and again at each evaluate.
void SetTuning(NVSDK_NGX_Parameter* parameters, float intensity) {
	SetF(parameters, "DLSSNR.Intensity", intensity);
	SetUI(parameters, "DLSSNR.Style", 0u);
	SetF(parameters, "DLSSNR.LocalStructureStrength", 1.0f);
	SetF(parameters, "DLSSNR.LocalToneStrength", 1.0f);
	SetF(parameters, "DLSSNR.SkinStructureStrength", -1.0f);
	SetUI(parameters, "DLSSNR.UseAutoMask", 1u);
}

bool SourceFormatOk(VkFormat format) {
	return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM ||
	       format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}
// The result is blitted to the swapchain as display-encoded values: a UNORM target only.
bool TargetFormatOk(VkFormat format) {
	return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM ||
	       format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
}

// The copy into the model's input shrinks the picture with a linear blit where the format allows it.
VkFilter CopyFilter(GraphicContext& graphics, VkFormat format, VkExtent2D from, VkExtent2D to) {
	if (from.width == to.width && from.height == to.height) return VK_FILTER_NEAREST;
	auto& p = g_present;
	if (p.filter_format != format) {
		p.filter_format = format;
		p.filter_linear = Blittable(graphics, format,
		                            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
	}
	return p.filter_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

[[gnu::noinline]] bool Initialise(GraphicContext& graphics, VkExtent2D extent) {
	auto& p       = g_present;
	p.initialised = true;
	if (!Load()) return false;
	if (!Blittable(graphics, kWorkingFormat, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
		Fail("RGBA16F images cannot be blitted on this GPU");
		return false;
	}
	const std::wstring data_path = ExeDir();
	const auto         start     = std::chrono::steady_clock::now();
	const int          inited =
	    g_forwarder.init(data_path.c_str(), static_cast<VkInstance>(graphics.instance),
	                     static_cast<VkPhysicalDevice>(graphics.physical_device),
	                     static_cast<VkDevice>(graphics.device), static_cast<int>(NVSDK_NGX_Version_API));
	const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
	std::printf("NHL27DLSS5 status=init result=%s ms=%lld\n", Hex(inited).c_str(), static_cast<long long>(took.count()));
	std::fflush(stdout);
	if (inited != kNgxSuccess) {
		Fail("the model would not initialise on this device (" + Hex(inited) + ")");
		return false;
	}
	constexpr VkImageUsageFlags input_usage =
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	constexpr VkImageUsageFlags output_usage =
	    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	if (!CreateImage(graphics, kWorkingFormat, extent, input_usage, p.input) ||
	    !CreateImage(graphics, kWorkingFormat, extent, output_usage, p.output)) {
		Fail("could not create the RGBA16F images");
		return false;
	}
	p.size      = extent;
	p.intensity = Dlss5Math::ParseIntensity(std::getenv("KYTY_DLSS5_INTENSITY"));
	// Timestamps when the graphics queue has them (the ms line); the pass works without.
	uint32_t family_count = 0;
	Vk().vkGetPhysicalDeviceQueueFamilyProperties(static_cast<VkPhysicalDevice>(graphics.physical_device),
	                                              &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	Vk().vkGetPhysicalDeviceQueueFamilyProperties(static_cast<VkPhysicalDevice>(graphics.physical_device),
	                                              &family_count, families.data());
	const uint32_t bits = graphics.queue_family < family_count ? families[graphics.queue_family].timestampValidBits : 0;
	if (bits != 0) {
		VkQueryPoolCreateInfo pool {};
		pool.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		pool.queryType  = VK_QUERY_TYPE_TIMESTAMP;
		pool.queryCount = kTimingSlots * 2;
		if (Vk().vkCreateQueryPool(static_cast<VkDevice>(graphics.device), &pool, nullptr, &p.timing) != VK_SUCCESS) {
			p.timing = VK_NULL_HANDLE;
		}
		p.timestamp_ms   = static_cast<double>(graphics.physical_device_properties.limits.timestampPeriod) / 1.0e6;
		p.timestamp_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
	}
	p.next_report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::printf("NHL27DLSS5 status=ready size=%ux%u intensity=%.2f timing=%d\n", extent.width, extent.height,
	            static_cast<double>(p.intensity), p.timing != VK_NULL_HANDLE ? 1 : 0);
	std::fflush(stdout);
	return true;
}

// The timestamps of the slot about to be reused (kTimingSlots presents ago), then the 5 s line.
void CollectTiming(GraphicContext& graphics, uint32_t slot) {
	auto& p = g_present;
	if (p.written[slot]) {
		std::array<uint64_t, 2> stamps {};
		if (Vk().vkGetQueryPoolResults(static_cast<VkDevice>(graphics.device), p.timing, slot * 2, 2, sizeof(stamps),
		                               stamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
			p.window_ms += static_cast<double>((stamps[1] - stamps[0]) & p.timestamp_mask) * p.timestamp_ms;
			++p.window_samples;
		}
	}
	if (const auto now = std::chrono::steady_clock::now(); now >= p.next_report) {
		p.next_report = now + std::chrono::seconds(5);
		std::printf("NHL27DLSS5 ms=%.2f frames=%llu in 5 s\n",
		            p.window_samples != 0 ? p.window_ms / p.window_samples : 0.0,
		            static_cast<unsigned long long>(p.window_frames));
		std::fflush(stdout);
		p.window_ms      = 0;
		p.window_samples = 0;
		p.window_frames  = 0;
	}
}

} // namespace

bool Active() {
	// Frame generation (KYTY_DLSSG=1) may run too: it makes its frames at the present from the picture this pass
	// wrote, so the generated frames are made from DLSS 5's pictures (the user's decision, 2026-11-24).
	return Wanted() && !g_failed.load(std::memory_order_relaxed);
}

void LoadEarly() {
	if (Active()) (void)Load();
}

void AppendDeviceExtensions(const std::vector<vk::ExtensionProperties>& available,
                            std::vector<const char*>& extensions) {
	if (!Active()) return;
	static constexpr std::array<const char*, 2> kNeeded = {"VK_NVX_binary_import", "VK_NVX_image_view_handle"};
	for (const char* name: kNeeded) {
		bool exposed = false;
		for (const auto& e: available) exposed = exposed || std::strcmp(e.extensionName.data(), name) == 0;
		bool present = false;
		for (const char* e: extensions) present = present || std::strcmp(e, name) == 0;
		const bool appended = exposed && !present;
		if (appended) extensions.push_back(name);
		std::printf("NHL27DLSS5 vk extension=%s exposed=%d present=%d appended=%d\n", name, exposed ? 1 : 0,
		            present ? 1 : 0, appended ? 1 : 0);
		std::fflush(stdout);
		if (!exposed) Fail(std::string("the device lacks ") + name);
	}
}

void RecordInputs(GraphicContext& graphics, VkCommandBuffer cmd, VkImage depth, VkImage motion, VkExtent2D render,
                  VkExtent2D display, float mv_scale_x, float mv_scale_y, bool reset) {
	if (!Active()) return;
	std::lock_guard lock(g_shared.mutex);
	if (g_shared.target.width == 0) { // the present thread chooses the pass's size on its next picture
		g_shared.sr_seen = true;
		return;
	}
	if (!g_shared.created) {
		g_shared.created = true;
		constexpr VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
		if (!Blittable(graphics, kDepthFormat, blit) || !Blittable(graphics, kMotionFormat, blit)) {
			Fail("depth or motion images cannot be blitted on this GPU");
			return;
		}
		constexpr VkImageUsageFlags usage =
		    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		const VkExtent2D target = g_shared.target;
		if (!CreateImage(graphics, kDepthFormat, target, usage, g_shared.depth) ||
		    !CreateImage(graphics, kMotionFormat, target, usage, g_shared.motion)) {
			Fail("could not create the depth and motion images");
			return;
		}
		// A second parameter block from the NGX core SR already initialised, beside SR's own.
		const NVSDK_NGX_Result allocated = NVSDK_NGX_VULKAN_AllocateParameters(&g_shared.parameters);
		std::printf("NHL27DLSS5 status=inputs size=%ux%u render=%ux%u sr_output=%ux%u parameters=%s\n",
		            target.width, target.height, render.width, render.height, display.width, display.height,
		            Hex(static_cast<int>(allocated)).c_str());
		std::fflush(stdout);
		if (allocated != NVSDK_NGX_Result_Success || g_shared.parameters == nullptr) {
			g_shared.parameters = nullptr;
			Fail("NVSDK_NGX_VULKAN_AllocateParameters (" + Hex(static_cast<int>(allocated)) + ")");
			return;
		}
		g_shared.size = target;
	}
	if (g_shared.depth.image == VK_NULL_HANDLE || g_shared.parameters == nullptr) return;
	if (render.width == 0 || render.height == 0) return;
	// Discard the last inputs (the present that read them was submitted earlier on this queue),
	// make the bridge's images readable by the transfer, blit, then hand ours to the present.
	const std::array<VkImageMemoryBarrier, 4> before = {
	    ImageBarrier(g_shared.depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAnyAccess,
	                 VK_ACCESS_TRANSFER_WRITE_BIT),
	    ImageBarrier(g_shared.motion.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 kAnyAccess, VK_ACCESS_TRANSFER_WRITE_BIT),
	    ImageBarrier(depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kAnyAccess, VK_ACCESS_TRANSFER_READ_BIT),
	    ImageBarrier(motion, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kAnyAccess, VK_ACCESS_TRANSFER_READ_BIT),
	};
	Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
	                          0, nullptr, static_cast<uint32_t>(before.size()), before.data());
	// SR's render size changes with the scene (so does its output size); the pass's size does not.
	const VkImageBlit region = FullBlit(render, g_shared.size);
	Vk().vkCmdBlitImage(cmd, depth, VK_IMAGE_LAYOUT_GENERAL, g_shared.depth.image,
	                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
	Vk().vkCmdBlitImage(cmd, motion, VK_IMAGE_LAYOUT_GENERAL, g_shared.motion.image,
	                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
	const std::array<VkImageMemoryBarrier, 2> after = {
	    ImageBarrier(g_shared.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	                 VK_ACCESS_TRANSFER_WRITE_BIT, kAnyAccess),
	    ImageBarrier(g_shared.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	                 VK_ACCESS_TRANSFER_WRITE_BIT, kAnyAccess),
	};
	Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
	                          0, nullptr, static_cast<uint32_t>(after.size()), after.data());
	g_shared.pending       = true;
	g_shared.pending_mv_x  = Dlss5Math::MotionScale(mv_scale_x, render.width, g_shared.size.width);
	g_shared.pending_mv_y  = Dlss5Math::MotionScale(mv_scale_y, render.height, g_shared.size.height);
	g_shared.pending_reset = reset;
}

void InputsSubmitted(bool submitted) {
	if (!Wanted()) return;
	std::lock_guard lock(g_shared.mutex);
	if (!g_shared.pending) return;
	g_shared.pending = false;
	if (!submitted) return;
	++g_shared.serial;
	g_shared.mv_x  = g_shared.pending_mv_x;
	g_shared.mv_y  = g_shared.pending_mv_y;
	g_shared.reset = g_shared.reset || g_shared.pending_reset;
}

PresentResult RecordPresent(GraphicContext& graphics, VkCommandBuffer cmd, const PresentSource& source) {
	if (!Active()) return {};
	auto& p = g_present;
	// The same picture again (another layer changed): the last result, never a second evaluate.
	if (source.serial != 0 && source.serial == p.last_picture) {
		return p.output_valid ? PresentResult {p.output.image, p.size} : PresentResult {};
	}
	p.last_picture = source.serial;
	p.output_valid = false;
	if (!SourceFormatOk(source.format) || !TargetFormatOk(source.target_format)) {
		if (!p.format_skip_logged) {
			p.format_skip_logged = true;
			std::printf("NHL27DLSS5 status=skip reason=format source=%d target=%d\n", static_cast<int>(source.format),
			            static_cast<int>(source.target_format));
			std::fflush(stdout);
		}
		p.reset_next = true;
		return {};
	}
	if (source.hdr) {
		if (!p.hdr_skip_logged) {
			p.hdr_skip_logged = true;
			std::printf("NHL27DLSS5 status=skip reason=hdr\n");
			std::fflush(stdout);
		}
		p.reset_next = true;
		return {};
	}
	// The pass's size, once, from the first picture after Super Resolution's first frame (dlss5Math.h
	// TargetSize): NHL 27's boot pictures are 2560x1440, its game pictures 3840x2160. The bridge thread
	// records no inputs until it is known.
	if (p.size.width == 0) {
		if (source.blank) return {};
		{
			std::lock_guard lock(g_shared.mutex);
			if (!g_shared.sr_seen) return {};
		}
		const auto target = Dlss5Math::TargetSize(source.extent.width, source.extent.height, source.box.width,
		                                          source.box.height);
		if (target.width == 0) return {};
		p.size = VkExtent2D {target.width, target.height};
		{
			std::lock_guard lock(g_shared.mutex);
			g_shared.target = p.size;
		}
		std::printf("NHL27DLSS5 status=size size=%ux%u picture=%ux%u box=%ux%u\n", p.size.width, p.size.height,
		            source.extent.width, source.extent.height, source.box.width, source.box.height);
		std::fflush(stdout);
	}

	uint64_t             inputs = 0;
	float                mv_x = 0, mv_y = 0;
	bool                 reset = false;
	VkExtent2D           inputs_size {};
	OwnedImage           depth, motion;
	NVSDK_NGX_Parameter* parameters = nullptr;
	{
		std::lock_guard lock(g_shared.mutex);
		inputs      = g_shared.serial;
		mv_x        = g_shared.mv_x;
		mv_y        = g_shared.mv_y;
		inputs_size = g_shared.size;
		depth       = g_shared.depth;
		motion      = g_shared.motion;
		parameters  = g_shared.parameters;
		if (inputs != p.last_inputs) {
			reset          = g_shared.reset;
			g_shared.reset = false;
		}
	}
	if (inputs == 0 || parameters == nullptr) return {}; // no Super Resolution frame yet
	if (inputs != p.last_inputs) {
		p.last_inputs        = inputs;
		p.presents_since_new = 0;
	} else {
		++p.presents_since_new;
	}
	p.reset_next = p.reset_next || reset;
	if (!Dlss5Math::InputsFresh(p.presents_since_new) || source.hdr || source.blank) {
		p.reset_next = true;
		return {};
	}
	// Depth and motion are made at the pass's size (it is set once, before any inputs).
	if (inputs_size.width != p.size.width || inputs_size.height != p.size.height) return {};
	if (!p.initialised && !Initialise(graphics, p.size)) return {};
	if (g_failed.load(std::memory_order_relaxed)) return {};

	const uint32_t slot = static_cast<uint32_t>(p.frames % kTimingSlots);
	if (p.timing != VK_NULL_HANDLE) {
		CollectTiming(graphics, slot);
		Vk().vkCmdResetQueryPool(cmd, p.timing, slot * 2, 2);
		Vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, p.timing, slot * 2);
	}
	// The picture into the RGBA16F input (values unchanged: display-encoded 0..1).
	const std::array<VkImageMemoryBarrier, 4> start = {
	    ImageBarrier(p.input.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAnyAccess,
	                 VK_ACCESS_TRANSFER_WRITE_BIT),
	    ImageBarrier(p.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, kAnyAccess, kAnyAccess),
	    ImageBarrier(depth.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kAnyAccess, kAnyAccess),
	    ImageBarrier(motion.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, kAnyAccess, kAnyAccess),
	};
	Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
	                          nullptr, 0, nullptr, static_cast<uint32_t>(start.size()), start.data());
	// The game's 4K picture in a smaller window shrinks here (linear), so the window blit is 1:1.
	const VkImageBlit copy = FullBlit(source.extent, p.size);
	Vk().vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.input.image,
	                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy,
	                    CopyFilter(graphics, source.format, source.extent, p.size));
	const VkImageMemoryBarrier to_model = ImageBarrier(p.input.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                                                   VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, kAnyAccess);
	Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
	                          nullptr, 1, &to_model);

	const unsigned int width  = p.size.width;
	const unsigned int height = p.size.height;
	if (p.feature == nullptr) {
		SetUI(parameters, "DLSSNR.Enabled", 1u);
		SetUI(parameters, "DLSSNR.Width", width);
		SetUI(parameters, "DLSSNR.Height", height);
		SetUI(parameters, "CreationNodeMask", 1u);
		SetUI(parameters, "VisibilityNodeMask", 1u);
		SetUI(parameters, "DLSSNR.Hint.Render.Preset", 0u);
		SetTuning(parameters, p.intensity);
		SetUI(parameters, "DLSSNR.UICorrection", 1u);
		const int created = g_forwarder.create(cmd, kFeatureNeuralRendering, parameters, &p.feature);
		std::printf("NHL27DLSS5 status=create result=%s size=%ux%u\n", Hex(created).c_str(), width, height);
		std::fflush(stdout);
		if (created != kNgxSuccess || p.feature == nullptr) {
			p.feature = nullptr;
			Fail("the model would not build its feature (" + Hex(created) + ")");
			return {};
		}
		p.reset_next = true;
	}
	const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	p.color_resource  = NVSDK_NGX_Create_ImageView_Resource_VK(p.input.view, p.input.image, range, kWorkingFormat,
                                                               width, height, false);
	p.depth_resource  = NVSDK_NGX_Create_ImageView_Resource_VK(depth.view, depth.image, range, kDepthFormat, width,
                                                               height, false);
	p.motion_resource = NVSDK_NGX_Create_ImageView_Resource_VK(motion.view, motion.image, range, kMotionFormat, width,
                                                               height, false);
	p.output_resource = NVSDK_NGX_Create_ImageView_Resource_VK(p.output.view, p.output.image, range, kWorkingFormat,
                                                               width, height, true);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, "DLSSNR.Color", &p.color_resource);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, "DLSSNR.Depth", &p.depth_resource);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, "DLSSNR.MVec", &p.motion_resource);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, "DLSSNR.Output", &p.output_resource);
	SetUI(parameters, "DLSSNR.Enabled", 1u);
	SetUI(parameters, "DLSSNR.Width", width);
	SetUI(parameters, "DLSSNR.Height", height);
	// Super Resolution is created with NVSDK_NGX_DLSS_Feature_Flags_DepthInverted (dlssBridge.cpp).
	SetUI(parameters, "DLSSNR.DepthInverted", 1u);
	SetUI(parameters, "DLSSNR.Reset", p.reset_next ? 1u : 0u);
	for (const char* rect: {"DLSSNR.ColorSubrect", "DLSSNR.OutputSubrect", "DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect"}) {
		const std::string prefix(rect);
		SetUI(parameters, (prefix + "BaseX").c_str(), 0u);
		SetUI(parameters, (prefix + "BaseY").c_str(), 0u);
		SetUI(parameters, (prefix + "Width").c_str(), width);
		SetUI(parameters, (prefix + "Height").c_str(), height);
	}
	SetF(parameters, "DLSSNR.MVecScaleX", mv_x);
	SetF(parameters, "DLSSNR.MVecScaleY", mv_y);
	SetTuning(parameters, p.intensity);
	const int evaluated = g_forwarder.evaluate(cmd, p.feature, parameters);
	if (evaluated != kNgxSuccess) {
		std::printf("NHL27DLSS5 status=evaluate result=%s frame=%llu\n", Hex(evaluated).c_str(),
		            static_cast<unsigned long long>(p.frames + 1));
		std::fflush(stdout);
		Fail("the model refused to evaluate (" + Hex(evaluated) + ")");
		return {};
	}
	const VkImageMemoryBarrier to_blit = ImageBarrier(p.output.image, VK_IMAGE_LAYOUT_GENERAL,
	                                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kAnyAccess,
	                                                  VK_ACCESS_TRANSFER_READ_BIT);
	Vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                          nullptr, 1, &to_blit);
	if (p.timing != VK_NULL_HANDLE) {
		Vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, p.timing, slot * 2 + 1);
		p.written[slot] = true;
	}
	// NGX leaves bound state undefined.
	InvalidateRedundantBinds();
	if (p.frames++ == 0) {
		std::printf("NHL27DLSS5 status=evaluated first frame %ux%u mv_scale=%.3f,%.3f\n", width, height,
		            static_cast<double>(mv_x), static_cast<double>(mv_y));
		std::fflush(stdout);
	}
	++p.window_frames;
	p.reset_next   = false;
	p.output_valid = true;
	return {p.output.image, p.size};
}

#else

bool Active() {
	return false;
}
void LoadEarly() {}
void AppendDeviceExtensions(const std::vector<vk::ExtensionProperties>&, std::vector<const char*>&) {}
void RecordInputs(GraphicContext&, VkCommandBuffer, VkImage, VkImage, VkExtent2D, VkExtent2D, float, float, bool) {}
void InputsSubmitted(bool) {}
PresentResult RecordPresent(GraphicContext&, VkCommandBuffer, const PresentSource&) {
	return {};
}

#endif

} // namespace Libs::Graphics::Dlss5
