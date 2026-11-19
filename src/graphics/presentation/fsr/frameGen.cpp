#include "graphics/presentation/fsr/frameGen.h"

#include "common/processExit.h"

// DLSS frame generation through Streamline 2.14 (see frameGen.h). Everything here is compiled only
// with KYTY_ENABLE_DLSSG; the decisions live in frameGenPolicy.h / frameGenMath.h (frame_gen_tests).
// Log lines start with "NHL27FG:".

#if KYTY_ENABLE_DLSSG

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/presentation/fsr/frameGenMath.h"
#include "graphics/presentation/fsr/frameGenPolicy.h"
#include "graphics/presentation/fsr/nhl27Fsr3Constants.h"
#include "kernel/memory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vulkan/vulkan_win32.h>

#include <SDL3/SDL.h>

#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss_g.h"
#include "sl_pcl.h"
#include "sl_reflex.h"
// Non-inline definitions: included by this file only.
#include "sl_security.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace Libs::Graphics::FrameGen {

namespace {

// The same self-generated project id as the DLSS bridge (dlssBridge.cpp kProjectId).
constexpr const char* kProjectId = "bca9f413-41aa-46c2-b12b-f3bfc9e2130e";

#if defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void Log(const char* format, ...) {
	char    line[768];
	va_list args;
	va_start(args, format);
	std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	std::printf("NHL27FG: %s\n", line);
	std::fflush(stdout);
}

// Streamline's own log (it then does not use OutputDebugString). Any thread.
void SlLog(sl::LogType type, const char* message) {
	if (message == nullptr) {
		return;
	}
	size_t length = std::strlen(message);
	while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
		length--;
	}
	const char* kind = type == sl::LogType::eError ? "error" : type == sl::LogType::eWarn ? "warn" : "info";
	std::printf("NHL27FG: sl %s %.*s\n", kind, static_cast<int>(length), message);
	std::fflush(stdout);
}

const char* ResultName(sl::Result result) {
	switch (result) {
		case sl::Result::eOk: return "ok";
		case sl::Result::eErrorIO: return "io";
		case sl::Result::eErrorDriverOutOfDate: return "driver-out-of-date";
		case sl::Result::eErrorOSOutOfDate: return "os-out-of-date";
		case sl::Result::eErrorOSDisabledHWS: return "hw-scheduling-off";
		case sl::Result::eErrorDeviceNotCreated: return "device-not-created";
		case sl::Result::eErrorNoSupportedAdapterFound: return "no-supported-adapter";
		case sl::Result::eErrorAdapterNotSupported: return "adapter-not-supported";
		case sl::Result::eErrorNoPlugins: return "no-plugins";
		case sl::Result::eErrorVulkanAPI: return "vulkan-api";
		case sl::Result::eErrorReflexAPI: return "reflex-api";
		case sl::Result::eErrorNGXFailed: return "ngx-failed";
		case sl::Result::eErrorMissingInputParameter: return "missing-input";
		case sl::Result::eErrorNotInitialized: return "not-initialized";
		case sl::Result::eErrorInitNotCalled: return "init-not-called";
		case sl::Result::eErrorInvalidParameter: return "invalid-parameter";
		case sl::Result::eErrorMissingConstants: return "missing-constants";
		case sl::Result::eErrorInvalidIntegration: return "invalid-integration";
		case sl::Result::eErrorFeatureMissing: return "feature-missing";
		case sl::Result::eErrorFeatureNotSupported: return "feature-not-supported";
		case sl::Result::eErrorFeatureFailedToLoad: return "feature-failed-to-load";
		case sl::Result::eErrorInvalidState: return "invalid-state";
		case sl::Result::eWarnOutOfVRAM: return "out-of-vram";
		default: return "error";
	}
}

// The first failure bit of DLSSGState::status (a literal: ModeGate keeps it).
const char* StatusName(sl::DLSSGStatus status) {
	const auto bits = static_cast<uint32_t>(status);
	if ((bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailResolutionTooLow)) != 0) return "status-resolution-too-low";
	if ((bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)) != 0) return "status-reflex";
	if ((bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailHDRFormatNotSupported)) != 0) return "status-hdr-format";
	if ((bits & static_cast<uint32_t>(sl::DLSSGStatus::eFailCommonConstantsInvalid)) != 0) return "status-constants";
	return "status-other";
}

// Test switches, read once before the instance:
//   KYTY_DLSSG=1                frame generation (also needs KYTY_DLSS=1 and the DLL set)
//   KYTY_DLSSG_FRAMES=n         generated frames per game frame, 1..3 within the driver's maximum
//   KYTY_DLSSG_HOLD=1           Streamline loaded and fed, frame generation never turned on
//   KYTY_DLSSG_TAG_LIFETIME=present  tags valid until present (default: copied at tag time)
//   KYTY_DLSSG_REFLEX_SLEEP=n   0 never, 1 on the CPU flip path (default), 2 also on the GPU path
//   KYTY_DLSSG_ALLOW_UNSIGNED=1 skip the NVIDIA signature check of sl.interposer.dll (development DLLs)
struct Config {
	bool     switch_on      = false;
	bool     dlss           = false;
	bool     hold           = false;
	bool     until_present  = false;
	bool     allow_unsigned = false;
	uint32_t frames         = 1;
	int      reflex_sleep   = 1;
};

bool EnvIs(const char* name, const char* value) {
	const char* v = std::getenv(name);
	return v != nullptr && std::strcmp(v, value) == 0;
}

Config ReadConfig() {
	Config c;
	c.switch_on      = EnvIs("KYTY_DLSSG", "1");
	c.dlss           = EnvIs("KYTY_DLSS", "1");
	c.hold           = EnvIs("KYTY_DLSSG_HOLD", "1");
	c.until_present  = EnvIs("KYTY_DLSSG_TAG_LIFETIME", "present");
	c.allow_unsigned = EnvIs("KYTY_DLSSG_ALLOW_UNSIGNED", "1");
	if (const char* v = std::getenv("KYTY_DLSSG_FRAMES")) {
		c.frames = static_cast<uint32_t>(std::clamp(std::atoi(v), 1, 3));
	}
	if (const char* v = std::getenv("KYTY_DLSSG_REFLEX_SLEEP")) {
		c.reflex_sleep = std::clamp(std::atoi(v), 0, 2);
	}
	return c;
}

struct Api {
	HMODULE                       module = nullptr;
	PFun_slInit*                  init                 = nullptr;
	PFun_slShutdown*              shutdown             = nullptr;
	PFun_slIsFeatureSupported*    is_feature_supported = nullptr;
	PFun_slGetFeatureFunction*    get_feature_function = nullptr;
	PFun_slGetNewFrameToken*      new_frame_token      = nullptr;
	PFun_slSetTagForFrame*        set_tag_for_frame    = nullptr;
	PFun_slSetConstants*          set_constants        = nullptr;
	PFN_vkGetInstanceProcAddr     get_instance_proc    = nullptr;
	PFN_vkGetDeviceProcAddr       get_device_proc      = nullptr;
	PFun_slDLSSGSetOptions*       dlssg_set_options    = nullptr;
	PFun_slDLSSGGetState*         dlssg_get_state      = nullptr;
	PFun_slPCLSetMarker*          pcl_set_marker       = nullptr;
	PFun_slReflexSleep*           reflex_sleep         = nullptr;
	PFun_slReflexSetOptions*      reflex_set_options   = nullptr;
};

struct CachedView {
	VkImage     image  = VK_NULL_HANDLE;
	VkFormat    format = VK_FORMAT_UNDEFINED;
	VkImageView view   = VK_NULL_HANDLE;
};

struct State {
	Config cfg;
	Api    api;

	std::atomic<bool>        active {false};    // Streamline is the Vulkan loader
	std::atomic<bool>        ready {false};     // device side set up, feature functions resolved
	std::atomic<bool>        tags_off {false};  // a tag call failed: no more tags this session
	std::atomic<bool>        reflex_on {false};
	std::atomic<const char*> pending_fail {nullptr}; // a failure seen off the present thread
	std::atomic<uint32_t>    api_errors {0};
	std::atomic<uint64_t>    presenting {0}; // request id of the flip about to be presented

	// Ledger, frame tokens, gate, sequencer, matrices, views. Never held across a present or a sleep.
	std::mutex      mutex;
	FrameLedger     ledger;
	ModeGate        gate;
	ModeSequencer   sequencer;
	uint64_t        started       = 0; // index the flip-submit side started (simulation start marker)
	sl::FrameToken* present_token = nullptr;
	uint32_t        frames_max    = 0;
	uint32_t        min_size      = 0;
	uint32_t        bad_status    = 0;
	bool            have_previous = false;
	Mat4            previous_view_to_clip  = Mat4::Identity();
	Mat4            previous_world_to_view = Mat4::Identity();
	std::array<CachedView, 2> views {};
	VkDevice        device = VK_NULL_HANDLE;
	uint64_t        tagged             = 0;
	uint64_t        presents           = 0;
	uint64_t        new_frame_presents = 0;
	uint64_t        multi_presents     = 0; // presents where Streamline showed more than one frame
};

State& G() {
	static State state;
	return state;
}

// KYTY_DLSSG_CAMERA_SCAN. GPU thread only (FlipPrepared, the draws, the upscale pass): no lock.
struct CameraScan {
	CameraTracker tracker;
	uint64_t      frame = 0; // guest flips prepared (FlipPrepared), the tracker's frame number
};

CameraScan& Cam() {
	static CameraScan scan;
	return scan;
}

const sl::ViewportHandle& Viewport() {
	static const sl::ViewportHandle viewport {0u};
	return viewport;
}

std::wstring ExeDirectory() {
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (length == 0) {
			return {};
		}
		if (length < path.size()) {
			path.resize(length);
			break;
		}
		path.resize(path.size() * 2);
	}
	const auto slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring {} : path.substr(0, slash + 1);
}

std::wstring Widen(std::string_view ascii) {
	return {ascii.begin(), ascii.end()};
}

// The DLL a function pointer points into (for the dispatcher log).
std::string ModuleOf(const void* address) {
	if (address == nullptr) {
		return "null";
	}
	HMODULE module = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       static_cast<LPCWSTR>(address), &module) == 0) {
		return "unknown";
	}
	char path[MAX_PATH] {};
	if (GetModuleFileNameA(module, path, MAX_PATH) == 0) {
		return "unknown";
	}
	const std::string_view full(path);
	const auto             slash = full.find_last_of("\\/");
	return std::string(slash == std::string_view::npos ? full : full.substr(slash + 1));
}

template <typename Function>
bool ResolveExport(HMODULE module, const char* name, Function*& out) {
	out = reinterpret_cast<Function*>(reinterpret_cast<void*>(GetProcAddress(module, name)));
	return out != nullptr;
}

template <typename Function>
bool ResolveFeature(sl::Feature feature, const char* name, Function*& out) {
	void* function = nullptr;
	if (G().api.get_feature_function(feature, name, function) != sl::Result::eOk || function == nullptr) {
		out = nullptr;
		Log("feature-function name=%s missing", name);
		return false;
	}
	out = reinterpret_cast<Function*>(function);
	return true;
}

// Points one dispatcher entry at the interposer's function and logs where it pointed before.
template <typename Pfn>
void Repoint(Pfn& entry, PFN_vkVoidFunction from_streamline, const char* name) {
	const std::string before = ModuleOf(reinterpret_cast<const void*>(entry));
	if (from_streamline != nullptr) {
		entry = reinterpret_cast<Pfn>(from_streamline);
	}
	Log("dispatch %s before=%s after=%s", name, before.c_str(),
	    ModuleOf(reinterpret_cast<const void*>(entry)).c_str());
}

// Streamline's error callback runs on the present thread inside a failing present or acquire and
// must not block: count only.
void OnApiError(const sl::APIError& /*error*/) {
	G().api_errors.fetch_add(1, std::memory_order_relaxed);
}

sl::float4x4 ToSl(const Mat4& m) {
	sl::float4x4 r;
	for (uint32_t row = 0; row < 4; row++) {
		const int i = static_cast<int>(row);
		r.setRow(row, sl::float4(m.At(i, 0), m.At(i, 1), m.At(i, 2), m.At(i, 3)));
	}
	return r;
}

sl::float3 ToSl(const std::array<float, 3>& v) {
	return {v[0], v[1], v[2]};
}

PresentMode ToMode(VkPresentModeKHR mode) {
	switch (mode) {
		case VK_PRESENT_MODE_MAILBOX_KHR: return PresentMode::Mailbox;
		case VK_PRESENT_MODE_IMMEDIATE_KHR: return PresentMode::Immediate;
		case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return PresentMode::FifoRelaxed;
		default: return PresentMode::Fifo;
	}
}

// Caller holds the mutex. Streamline applies the options at the next present.
void SetModeLocked(bool on) {
	auto&            g = G();
	sl::DLSSGOptions options {};
	options.mode                = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	options.numFramesToGenerate = FramesToGenerate(g.cfg.frames, g.frames_max);
	options.onErrorCallback     = &OnApiError;
	const sl::Result result     = g.api.dlssg_set_options(Viewport(), options);
	// DLSS-G needs Reflex on while it runs (sl_dlss_g.h).
	sl::ReflexOptions reflex {};
	reflex.mode                    = on ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
	const sl::Result reflex_result = g.api.reflex_set_options(reflex);
	g.reflex_on.store(on && reflex_result == sl::Result::eOk, std::memory_order_relaxed);
	Log("mode=%s frames=%u max=%u result=%s reflex=%s", on ? "on" : "off", options.numFramesToGenerate,
	    g.frames_max, ResultName(result), ResultName(reflex_result));
	if (on && result != sl::Result::eOk) {
		g.gate.Fail("set-options");
	}
}

sl::FrameToken* TokenLocked(uint64_t index) {
	sl::FrameToken* token = nullptr;
	const auto      value = static_cast<uint32_t>(index);
	if (G().api.new_frame_token(token, &value) != sl::Result::eOk) {
		return nullptr;
	}
	return token;
}

void Marker(sl::PCLMarker marker, const sl::FrameToken* token) {
	if (token != nullptr) {
		G().api.pcl_set_marker(marker, *token);
	}
}

// Caller holds the mutex. A view of one input image, cached while the image stays the same.
VkImageView ViewLocked(size_t slot, const TagImage& image) {
	auto& g      = G();
	auto& cached = g.views[slot];
	if (cached.view != VK_NULL_HANDLE && cached.image == image.image && cached.format == image.format) {
		return cached.view;
	}
	if (cached.view != VK_NULL_HANDLE) {
		// The image was replaced (size change): its owner waited for the GPU before that.
		VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImageView(g.device, cached.view, nullptr);
	}
	cached = {};
	VkImageViewCreateInfo info {};
	info.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image                       = image.image;
	info.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
	info.format                      = image.format;
	info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	info.subresourceRange.levelCount = 1;
	info.subresourceRange.layerCount = 1;
	VkImageView view                 = VK_NULL_HANDLE;
	if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateImageView(g.device, &info, nullptr, &view) != VK_SUCCESS) {
		return VK_NULL_HANDLE;
	}
	cached = {image.image, image.format, view};
	return view;
}

sl::Resource MakeResource(const TagImage& image, VkDeviceMemory memory, VkImageView view) {
	sl::Resource resource(sl::ResourceType::eTex2d, reinterpret_cast<void*>(image.image),
	                      reinterpret_cast<void*>(memory), reinterpret_cast<void*>(view),
	                      static_cast<uint32_t>(image.layout));
	resource.width        = image.width;
	resource.height       = image.height;
	resource.nativeFormat = static_cast<uint32_t>(image.format);
	resource.mipLevels    = 1;
	resource.arrayLayers  = 1;
	resource.flags        = 0; // VkImageCreateFlags (must be set)
	resource.usage        = static_cast<uint32_t>(image.usage);
	return resource;
}

} // namespace

PFN_vkGetInstanceProcAddr LoaderBeforeInstance(PFN_vkGetInstanceProcAddr plain) {
	auto& g = G();
	g.cfg   = ReadConfig();
	if (!g.cfg.switch_on) {
		return plain;
	}
	const std::wstring directory = ExeDirectory();
	std::string_view   missing;
	for (const auto name: RequiredDlls) {
		const std::wstring path = directory + Widen(name);
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			missing = name;
			break;
		}
	}
	const auto decision = DecideLoader({true, true, g.cfg.dlss, missing});
	if (decision.loader == Loader::Plain) {
		Log("loader=plain reason=%.*s%s%.*s", static_cast<int>(decision.reason.size()), decision.reason.data(),
		    missing.empty() ? "" : " dll=", static_cast<int>(missing.size()), missing.data());
		return plain;
	}
	const std::wstring interposer = directory + L"sl.interposer.dll";
	if (!g.cfg.allow_unsigned && !sl::security::verifyEmbeddedSignature(interposer.c_str())) {
		Log("loader=plain reason=unsigned dll=sl.interposer.dll");
		return plain;
	}
	auto& api  = g.api;
	api.module = LoadLibraryW(interposer.c_str());
	if (api.module == nullptr) {
		Log("loader=plain reason=load-failed error=%lu", GetLastError());
		return plain;
	}
	// From here Streamline is in the process (it stays loaded): end it without DLL detach (processExit.h).
	Common::SetTerminateOnExit();
	const bool resolved = ResolveExport(api.module, "slInit", api.init) &&
	                      ResolveExport(api.module, "slShutdown", api.shutdown) &&
	                      ResolveExport(api.module, "slIsFeatureSupported", api.is_feature_supported) &&
	                      ResolveExport(api.module, "slGetFeatureFunction", api.get_feature_function) &&
	                      ResolveExport(api.module, "slGetNewFrameToken", api.new_frame_token) &&
	                      ResolveExport(api.module, "slSetTagForFrame", api.set_tag_for_frame) &&
	                      ResolveExport(api.module, "slSetConstants", api.set_constants);
	api.get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	    reinterpret_cast<void*>(GetProcAddress(api.module, "vkGetInstanceProcAddr")));
	api.get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
	    reinterpret_cast<void*>(GetProcAddress(api.module, "vkGetDeviceProcAddr")));
	if (!resolved || api.get_instance_proc == nullptr || api.get_device_proc == nullptr) {
		// Loaded but never initialized: harmless, the plain loader runs as before.
		Log("loader=plain reason=missing-export");
		return plain;
	}

	static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
	static std::wstring      plugin_directory;
	static const wchar_t*    plugin_paths[1] = {};
	plugin_directory                         = directory;
	plugin_paths[0]                          = plugin_directory.c_str();
	sl::Preferences preferences {};
	preferences.showConsole        = false;
	preferences.logLevel           = sl::LogLevel::eDefault;
	preferences.pathsToPlugins     = plugin_paths;
	preferences.numPathsToPlugins  = 1;
	preferences.logMessageCallback = &SlLog;
	// No downloads at run time (no eAllowOTA / eLoadDownloadedPlugins); tags per frame token.
	preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	preferences.featuresToLoad    = features;
	preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
	preferences.engine            = sl::EngineType::eCustom;
	preferences.engineVersion     = "1.0";
	preferences.projectId         = kProjectId;
	preferences.renderAPI         = sl::RenderAPI::eVulkan;
	const sl::Result result       = api.init(preferences, sl::kSDKVersion);
	if (result != sl::Result::eOk) {
		Log("loader=plain reason=slInit result=%s (%d)", ResultName(result), static_cast<int>(result));
		return plain;
	}
	{
		std::lock_guard lock(g.mutex);
		g.gate = ModeGate(ModeGate::Config {60, 30, g.cfg.hold});
	}
	g.active.store(true);
	Log("loader=streamline frames=%u hold=%d tag_lifetime=%s reflex_sleep=%d signature=%s", g.cfg.frames,
	    g.cfg.hold ? 1 : 0, g.cfg.until_present ? "present" : "now", g.cfg.reflex_sleep,
	    g.cfg.allow_unsigned ? "skipped" : "ok");
	return api.get_instance_proc;
}

bool Active() {
	return G().active.load();
}

VkSurfaceKHR CreateSurface(SDL_Window* window, VkInstance instance) {
	auto& g = G();
	if (!g.active.load() || window == nullptr) {
		return VK_NULL_HANDLE;
	}
	auto* hwnd   = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
	                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	auto  create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        g.api.get_instance_proc(instance, "vkCreateWin32SurfaceKHR"));
	if (hwnd == nullptr || create == nullptr) {
		Log("surface=sdl reason=%s", hwnd == nullptr ? "no-hwnd" : "no-function");
		return VK_NULL_HANDLE;
	}
	VkWin32SurfaceCreateInfoKHR info {};
	info.sType          = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	info.hinstance      = GetModuleHandleW(nullptr);
	info.hwnd           = hwnd;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	const VkResult result = create(instance, &info, nullptr, &surface);
	if (result != VK_SUCCESS) {
		Log("surface=sdl reason=create-failed result=%d", static_cast<int>(result));
		return VK_NULL_HANDLE;
	}
	Log("surface=streamline function=%s", ModuleOf(reinterpret_cast<const void*>(create)).c_str());
	return surface;
}

VkQueueFlags RejectedQueueFlags() {
	return G().active.load() ? static_cast<VkQueueFlags>(VK_QUEUE_OPTICAL_FLOW_BIT_NV) : 0;
}

void AfterDevice(GraphicContext& graphics) {
	auto& g = G();
	if (!g.active.load()) {
		return;
	}
	const auto device   = static_cast<VkDevice>(graphics.device);
	const auto instance = static_cast<VkInstance>(graphics.instance);
	g.device            = device;
	{
		// Which family is the optical-flow one (kept free for Streamline by RejectedQueueFlags).
		const auto  families = graphics.physical_device.getQueueFamilyProperties();
		std::string list;
		for (size_t i = 0; i < families.size(); i++) {
			char item[64];
			std::snprintf(item, sizeof(item), " %zu:flags=0x%x,count=%u", i,
			              static_cast<unsigned>(static_cast<VkQueueFlags>(families[i].queueFlags)),
			              families[i].queueCount);
			list += item;
		}
		Log("queue families%s", list.c_str());
	}

	// The functions Streamline hooks, from the interposer itself, whatever init(device) resolved.
	auto&      d   = VULKAN_HPP_DEFAULT_DISPATCHER;
	const auto gdp = g.api.get_device_proc;
	const auto gip = g.api.get_instance_proc;
	Repoint(d.vkQueuePresentKHR, gdp(device, "vkQueuePresentKHR"), "vkQueuePresentKHR");
	Repoint(d.vkCreateSwapchainKHR, gdp(device, "vkCreateSwapchainKHR"), "vkCreateSwapchainKHR");
	Repoint(d.vkDestroySwapchainKHR, gdp(device, "vkDestroySwapchainKHR"), "vkDestroySwapchainKHR");
	Repoint(d.vkGetSwapchainImagesKHR, gdp(device, "vkGetSwapchainImagesKHR"), "vkGetSwapchainImagesKHR");
	Repoint(d.vkAcquireNextImageKHR, gdp(device, "vkAcquireNextImageKHR"), "vkAcquireNextImageKHR");
	Repoint(d.vkDeviceWaitIdle, gdp(device, "vkDeviceWaitIdle"), "vkDeviceWaitIdle");
	Repoint(d.vkDestroySurfaceKHR, gip(instance, "vkDestroySurfaceKHR"), "vkDestroySurfaceKHR");

	sl::AdapterInfo adapter {};
	adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(graphics.physical_device);
	const sl::Result supported = g.api.is_feature_supported(sl::kFeatureDLSS_G, adapter);
	Log("adapter dlss_g=%s", ResultName(supported));
	if (supported != sl::Result::eOk) {
		std::lock_guard lock(g.mutex);
		g.gate.Fail("adapter");
		return;
	}
	auto&      api      = g.api;
	const bool resolved = ResolveFeature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api.dlssg_set_options) &&
	                      ResolveFeature(sl::kFeatureDLSS_G, "slDLSSGGetState", api.dlssg_get_state) &&
	                      ResolveFeature(sl::kFeaturePCL, "slPCLSetMarker", api.pcl_set_marker) &&
	                      ResolveFeature(sl::kFeatureReflex, "slReflexSleep", api.reflex_sleep) &&
	                      ResolveFeature(sl::kFeatureReflex, "slReflexSetOptions", api.reflex_set_options);
	if (!resolved) {
		std::lock_guard lock(g.mutex);
		g.gate.Fail("feature-functions");
		return;
	}
	// Reflex options must be set at least once, even off.
	const sl::ReflexOptions reflex {};
	const sl::Result        reflex_result = api.reflex_set_options(reflex);
	g.ready.store(true);
	Log("ready reflex=%s", ResultName(reflex_result));
}

void Shutdown(VkDevice device) {
	auto& g = G();
	if (!g.active.load()) {
		return;
	}
	g.ready.store(false);
	{
		std::lock_guard lock(g.mutex);
		for (auto& cached: g.views) {
			if (cached.view != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
				VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyImageView(device, cached.view, nullptr);
			}
			cached = {};
		}
	}
	const sl::Result result = g.api.shutdown();
	// sl.interposer.dll stays loaded: the dispatcher still points into it for the device and
	// instance destruction that follow.
	Log("shutdown result=%s tagged=%llu presents=%llu new_frames=%llu multi=%llu api_errors=%u", ResultName(result),
	    static_cast<unsigned long long>(g.tagged), static_cast<unsigned long long>(g.presents),
	    static_cast<unsigned long long>(g.new_frame_presents), static_cast<unsigned long long>(g.multi_presents),
	    g.api_errors.load());
}

VkPresentModeKHR AdjustPresentMode(VkPresentModeKHR requested, bool mailbox_available, bool immediate_available) {
	auto& g = G();
	if (!g.ready.load()) {
		return requested;
	}
	bool frame_gen = false;
	{
		std::lock_guard lock(g.mutex);
		frame_gen = g.sequencer.FrameGenPresentMode();
	}
	if (!frame_gen) {
		return requested;
	}
	VkPresentModeKHR result = requested;
	switch (PresentModeFor(true, ToMode(requested), mailbox_available, immediate_available)) {
		case PresentMode::Mailbox: result = VK_PRESENT_MODE_MAILBOX_KHR; break;
		case PresentMode::Immediate: result = VK_PRESENT_MODE_IMMEDIATE_KHR; break;
		default: break;
	}
	Log("present-mode requested=%d frame_gen=%d", static_cast<int>(requested), static_cast<int>(result));
	return result;
}

void FlipPrepared(uint64_t request_id) {
	if (static const bool scan = CameraScanEnabled(); scan) {
		auto& cam = Cam();
		cam.frame++;
		if (const auto& lock = cam.tracker.Lock(); lock && cam.frame % 1800 == 0) {
			std::printf("NHL27CAMSCAN stats frame=%llu shader=%016llx slot=%u reads=%llu rejects=%llu\n",
			            static_cast<unsigned long long>(cam.frame), static_cast<unsigned long long>(lock->shader),
			            lock->slot, static_cast<unsigned long long>(cam.tracker.Reads()),
			            static_cast<unsigned long long>(cam.tracker.Rejects()));
			std::fflush(stdout);
		}
	}
	auto& g = G();
	if (!g.ready.load(std::memory_order_relaxed)) {
		return;
	}
	std::lock_guard lock(g.mutex);
	g.ledger.BindFlip(request_id);
}

void PresentingFlip(uint64_t request_id) {
	auto& g = G();
	if (g.ready.load(std::memory_order_relaxed)) {
		g.presenting.store(request_id, std::memory_order_relaxed);
	}
}

PresentStep BeforePresent(uint32_t width, uint32_t height) {
	auto& g = G();
	if (!g.ready.load(std::memory_order_relaxed)) {
		return {};
	}
	const uint64_t  request = g.presenting.exchange(0, std::memory_order_relaxed);
	std::lock_guard lock(g.mutex);
	const auto      take = g.ledger.TakeForPresent(request);
	g.presents++;
	if (take.new_frame) {
		g.new_frame_presents++;
	}
	if (const char* reason = g.pending_fail.exchange(nullptr)) {
		g.gate.Fail(reason);
	}
	sl::DLSSGState   state {};
	const sl::Result state_result = g.api.dlssg_get_state(Viewport(), state, nullptr);
	if (state_result == sl::Result::eOk) {
		g.frames_max = state.numFramesToGenerateMax;
		g.min_size   = state.minWidthOrHeight;
		if (state.numFramesActuallyPresented > 1) {
			g.multi_presents++;
		}
		// Recovery (DLSS-G guide 14.3): a status other than OK turns the mode off. A few presents of
		// grace after turning on, while Streamline sees the first markers.
		if (g.sequencer.Current() == ModeSequencer::State::On && state.status != sl::DLSSGStatus::eOk) {
			if (++g.bad_status >= 10) {
				const char* reason = StatusName(state.status);
				g.gate.Fail(reason);
				Log("status=0x%x %s: frame generation off for this session", static_cast<unsigned>(state.status),
				    reason);
			}
		} else {
			g.bad_status = 0;
		}
	}
	const bool window_ok = width != 0 && height != 0 && std::min(width, height) >= g.min_size;
	const auto step      = g.gate.Update({take.new_frame, window_ok});
	const auto out       = g.sequencer.BeforePresent(step.on);
	if (out.set_options) {
		SetModeLocked(out.options_on);
	}
	if (step.changed) {
		Log("gate on=%d reason=%.*s present=%llu", step.on ? 1 : 0, static_cast<int>(step.reason.size()),
		    step.reason.data(), static_cast<unsigned long long>(g.presents));
	}
	g.present_token = take.new_frame ? TokenLocked(take.index) : nullptr;
	if (g.presents % 600 == 0) {
		Log("stats presents=%llu new_frames=%llu tagged=%llu multi=%llu state=%d gate=%d status=0x%x min=%u "
		    "max=%u api_errors=%u",
		    static_cast<unsigned long long>(g.presents), static_cast<unsigned long long>(g.new_frame_presents),
		    static_cast<unsigned long long>(g.tagged), static_cast<unsigned long long>(g.multi_presents),
		    static_cast<int>(g.sequencer.Current()), g.gate.On() ? 1 : 0, static_cast<unsigned>(state.status),
		    g.min_size, g.frames_max, g.api_errors.load(std::memory_order_relaxed));
	}
	return {out.request_recreate};
}

bool AfterFrameGenRecreate() {
	auto& g = G();
	if (!g.ready.load()) {
		return false;
	}
	std::lock_guard lock(g.mutex);
	const auto      out = g.sequencer.Recreated(g.gate.On());
	if (out.set_options) {
		SetModeLocked(out.options_on);
	}
	Log("recreated state=%d again=%d", static_cast<int>(g.sequencer.Current()), out.request_recreate ? 1 : 0);
	return out.request_recreate;
}

void BeforeOutsideRecreate() {
	auto& g = G();
	if (!g.ready.load()) {
		return;
	}
	std::lock_guard lock(g.mutex);
	const auto      out = g.sequencer.OutsideRecreate();
	if (out.set_options) {
		SetModeLocked(false);
	}
}

void PresentStart() {
	auto& g = G();
	if (!g.ready.load(std::memory_order_relaxed)) {
		return;
	}
	sl::FrameToken* token = nullptr;
	{
		std::lock_guard lock(g.mutex);
		token = g.present_token;
	}
	Marker(sl::PCLMarker::ePresentStart, token);
}

void PresentEnd() {
	auto& g = G();
	if (!g.ready.load(std::memory_order_relaxed)) {
		return;
	}
	sl::FrameToken* token = nullptr;
	{
		std::lock_guard lock(g.mutex);
		token = g.present_token;
	}
	Marker(sl::PCLMarker::ePresentEnd, token);
}

void FlipSubmitted(bool guest_thread) {
	auto& g = G();
	if (!g.ready.load(std::memory_order_relaxed)) {
		return;
	}
	sl::FrameToken* ending = nullptr;
	sl::FrameToken* next   = nullptr;
	{
		std::lock_guard lock(g.mutex);
		if (g.started != 0) {
			ending = TokenLocked(g.started);
		}
		g.started = std::max(g.started + 1, g.ledger.LastTagged() + 1);
		next      = TokenLocked(g.started);
	}
	Marker(sl::PCLMarker::eSimulationEnd, ending);
	Marker(sl::PCLMarker::eRenderSubmitStart, ending);
	Marker(sl::PCLMarker::eRenderSubmitEnd, ending);
	const bool may_sleep = g.cfg.reflex_sleep == 2 || (g.cfg.reflex_sleep == 1 && guest_thread);
	if (next != nullptr && may_sleep && g.reflex_on.load(std::memory_order_relaxed)) {
		g.api.reflex_sleep(*next);
	}
	Marker(sl::PCLMarker::eSimulationStart, next);
}

TagReservation ReserveTag(const Nhl27Fsr3Constants& constants) {
	auto&          g = G();
	TagReservation reservation;
	if (!g.ready.load(std::memory_order_relaxed) || g.tags_off.load(std::memory_order_relaxed)) {
		return reservation;
	}
	const Projection projection = ProjectionFromConstants(constants.Float(0), constants.Float(1),
	                                                      constants.Float(3), constants.Width(), constants.Height());
	if (!projection.Valid()) {
		return reservation;
	}
	reservation.world_to_view = Cam().tracker.View().m; // same thread as the scan
	std::lock_guard lock(g.mutex);
	reservation.index = g.ledger.Tag(g.started);
	return reservation;
}

bool CameraScanEnabled() {
	static const bool on = [] {
		const bool value = EnvIs("KYTY_DLSSG_CAMERA_SCAN", "1");
		if (value) {
			std::printf("NHL27CAMSCAN on every=%u buffers=%u max_bytes=%zu confirm=%u\n", CameraTracker::ScanEvery,
			            CameraTracker::MaxBuffers, CameraTracker::MaxBytes, CameraTracker::ConfirmFrames);
			std::fflush(stdout);
		}
		return value;
	}();
	return on;
}

void CameraScanConstants(const Nhl27Fsr3Constants& constants) {
	if (!CameraScanEnabled()) {
		return;
	}
	const Projection projection = ProjectionFromConstants(constants.Float(0), constants.Float(1),
	                                                      constants.Float(3), constants.Width(), constants.Height());
	if (projection.Valid()) {
		Cam().tracker.SetExpected(projection);
	}
}

void CameraScanBuffer(uint64_t shader_hash, uint32_t slot, uint64_t address, uint64_t size) {
	auto& cam = Cam();
	if (address == 0 || !cam.tracker.Wants(cam.frame, shader_hash, slot, size)) {
		return;
	}
	std::array<uint32_t, CameraTracker::MaxBytes / 4> words;
	const uint64_t bytes = std::min<uint64_t>(size, CameraTracker::MaxBytes) & ~uint64_t {15};
	if (bytes < 64 || !LibKernel::Memory::TryReadBacking(address, words.data(), bytes)) {
		return;
	}
	const CameraTracker::Candidate* candidate = nullptr;
	const auto event = cam.tracker.Feed(cam.frame, shader_hash, slot, size,
	                                    std::span<const uint32_t>(words.data(), static_cast<size_t>(bytes / 4)),
	                                    &candidate);
	if (event == CameraTracker::Event::None || candidate == nullptr) {
		return;
	}
	const char* kind = event == CameraTracker::Event::Candidate ? "candidate"
	                   : event == CameraTracker::Event::Varies  ? "varies"
	                                                            : "lock";
	const auto& m = candidate->match;
	std::printf("NHL27CAMSCAN %s frame=%llu shader=%016llx slot=%u size=%llu offset=%zu view_offset=%zu score=%d "
	            "transposed=%d/%d y_flipped=%d minus_z=%d\n",
	            kind, static_cast<unsigned long long>(cam.frame), static_cast<unsigned long long>(shader_hash), slot,
	            static_cast<unsigned long long>(size), m.projection_offset, m.view_offset.value_or(0), m.score,
	            m.projection_transposed ? 1 : 0, m.view_transposed ? 1 : 0, m.y_flipped ? 1 : 0,
	            m.looks_down_minus_z ? 1 : 0);
	std::fflush(stdout);
}

bool TagUpscale(GraphicContext& graphics, VkCommandBuffer command, const TagImage& depth, const TagImage& motion,
                const Nhl27Fsr3Constants& constants, bool reset, const TagReservation& reservation) {
	auto&          g     = G();
	const uint64_t index = reservation.index;
	if (!g.ready.load(std::memory_order_relaxed) || g.tags_off.load(std::memory_order_relaxed) || index == 0 ||
	    command == VK_NULL_HANDLE) {
		return false;
	}
	// From here on a reserved index without tags would reach a present: any failure turns frame
	// generation off for the session (pending_fail, applied at the next present).
	const auto fail = [&g](const char* reason) {
		g.tags_off.store(true);
		g.pending_fail.store(reason);
	};
	const Projection projection = ProjectionFromConstants(constants.Float(0), constants.Float(1),
	                                                      constants.Float(3), constants.Width(), constants.Height());
	if (depth.image == VK_NULL_HANDLE || motion.image == VK_NULL_HANDLE || depth.width == 0 || depth.height == 0) {
		fail("tag-input");
		Log("tag failed index=%llu: no input image", static_cast<unsigned long long>(index));
		return false;
	}
	VmaAllocationInfo depth_info {}, motion_info {};
	vmaGetAllocationInfo(graphics.allocator, depth.allocation, &depth_info);
	vmaGetAllocationInfo(graphics.allocator, motion.allocation, &motion_info);

	std::lock_guard lock(g.mutex);
	const VkImageView depth_view  = ViewLocked(0, depth);
	const VkImageView motion_view = ViewLocked(1, motion);
	if (depth_view == VK_NULL_HANDLE || motion_view == VK_NULL_HANDLE) {
		fail("tag-view");
		Log("tag failed index=%llu: image view", static_cast<unsigned long long>(index));
		return false;
	}
	sl::FrameToken* const token = TokenLocked(index);
	if (token == nullptr) {
		fail("tag-token");
		Log("tag failed index=%llu: frame token", static_cast<unsigned long long>(index));
		return false;
	}

	// Constants: matrices without jitter, jitter in pixels (DLSS-G guide 7; sl_consts.h).
	const Mat4 view_to_clip  = ViewToClip(projection);
	Mat4 world_to_view;
	world_to_view.m = reservation.world_to_view;
	const bool fresh         = reset || !g.have_previous;
	const Mat4 previous_view_to_clip  = fresh ? view_to_clip : g.previous_view_to_clip;
	const Mat4 previous_world_to_view = fresh ? world_to_view : g.previous_world_to_view;
	const auto clip_to_view   = Inverse(view_to_clip);
	const auto clip_to_prev   = ClipToPrevClip(view_to_clip, world_to_view, previous_view_to_clip,
	                                           previous_world_to_view);
	const auto prev_to_clip   = clip_to_prev ? Inverse(*clip_to_prev) : std::nullopt;
	const auto camera         = CameraFromView(world_to_view);
	if (!clip_to_view || !clip_to_prev || !prev_to_clip || !camera) {
		fail("tag-camera");
		Log("tag failed index=%llu: camera matrices not invertible", static_cast<unsigned long long>(index));
		return false;
	}
	sl::Constants c {};
	c.cameraViewToClip       = ToSl(view_to_clip);
	c.clipToCameraView       = ToSl(*clip_to_view);
	c.clipToLensClip         = ToSl(Mat4::Identity());
	c.clipToPrevClip         = ToSl(*clip_to_prev);
	c.prevClipToClip         = ToSl(*prev_to_clip);
	c.jitterOffset           = {constants.Float(28), constants.Float(29)};
	c.mvecScale              = {constants.Float(32), constants.Float(33)}; // to [-1, 1] screen units
	c.cameraPinholeOffset    = {0.0f, 0.0f};
	c.cameraPos              = ToSl(camera->position);
	c.cameraUp               = ToSl(camera->up);
	c.cameraRight            = ToSl(camera->right);
	c.cameraFwd              = ToSl(camera->forward);
	c.cameraNear             = projection.Near();
	c.cameraFar              = projection.Far();
	c.cameraFOV              = projection.FovY();
	c.cameraAspectRatio      = projection.AspectRatio();
	c.depthInverted          = sl::Boolean::eTrue;
	c.cameraMotionIncluded   = sl::Boolean::eTrue;
	c.motionVectors3D        = sl::Boolean::eFalse;
	c.reset                  = fresh ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	c.orthographicProjection = sl::Boolean::eFalse;
	c.motionVectorsDilated   = sl::Boolean::eFalse;
	c.motionVectorsJittered  = sl::Boolean::eFalse;
	const sl::Result constants_result = g.api.set_constants(c, *token, Viewport());

	sl::Resource depth_resource  = MakeResource(depth, depth_info.deviceMemory, depth_view);
	sl::Resource motion_resource = MakeResource(motion, motion_info.deviceMemory, motion_view);
	const sl::ResourceLifecycle lifecycle =
	    g.cfg.until_present ? sl::ResourceLifecycle::eValidUntilPresent : sl::ResourceLifecycle::eOnlyValidNow;
	const sl::Extent extent {0, 0, depth.width, depth.height};
	const sl::ResourceTag tags[] = {
	    sl::ResourceTag(&depth_resource, sl::kBufferTypeDepth, lifecycle, &extent),
	    sl::ResourceTag(&motion_resource, sl::kBufferTypeMotionVectors, lifecycle, &extent),
	};
	const sl::Result tag_result =
	    g.api.set_tag_for_frame(*token, Viewport(), tags, static_cast<uint32_t>(std::size(tags)), command);
	if (constants_result != sl::Result::eOk || tag_result != sl::Result::eOk) {
		g.tags_off.store(true);
		g.pending_fail.store("tag");
		Log("tag failed index=%llu constants=%s tags=%s: no more tags this session",
		    static_cast<unsigned long long>(index), ResultName(constants_result), ResultName(tag_result));
		return true; // the tag call may have recorded commands
	}
	g.previous_view_to_clip  = view_to_clip;
	g.previous_world_to_view = world_to_view;
	g.have_previous          = true;
	if (++g.tagged <= 3) {
		Log("tag index=%llu size=%ux%u depth_format=%d motion_format=%d layout=%d/%d lifetime=%s reset=%d "
		    "near=%.4f far=%.1f fov=%.4f jitter=%.4f,%.4f",
		    static_cast<unsigned long long>(index), depth.width, depth.height, static_cast<int>(depth.format),
		    static_cast<int>(motion.format), static_cast<int>(depth.layout), static_cast<int>(motion.layout),
		    g.cfg.until_present ? "present" : "now", fresh ? 1 : 0, static_cast<double>(projection.Near()),
		    static_cast<double>(projection.Far()), static_cast<double>(projection.FovY()),
		    static_cast<double>(constants.Float(28)), static_cast<double>(constants.Float(29)));
	}
	return true;
}

} // namespace Libs::Graphics::FrameGen

#endif // KYTY_ENABLE_DLSSG
