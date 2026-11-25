// nvngx.dll_dlssnr.dll: the forwarder between the emulator and the player's own DLSS 5 model
// (nvngx_dlssnr.dll, KYTY_DLSS5, src/graphics/presentation/fsr/dlss5Pass.h).
//
// The model refuses calls whose caller module path lacks "nvngx.dll"; this DLL's name has it.
// Every result is kept in a volatile before it is returned, so the call stays a call (a tail call
// would become a jump and the model would see the emulator as its caller).
//
// Built only with KYTY_ENABLE_DLSS, without the emulator's PGO flags, installed next to the exe.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// The model's own exports. Init's last two arguments are in the order the model expects
// (feature info, then SDK version), which differs from the NGX core's public declaration.
using ModelInit     = int (*)(unsigned long long, const wchar_t*, void*, void*, void*, const void*, int);
using ModelCreate   = int (*)(void*, int, void*, void**);
using ModelEvaluate = int (*)(void*, void*, void*, void*);
using ModelRelease  = int (*)(void*);

HMODULE       g_model    = nullptr;
ModelInit     g_init     = nullptr;
ModelCreate   g_create   = nullptr;
ModelEvaluate g_evaluate = nullptr;
ModelRelease  g_release  = nullptr;

constexpr int kNotLoaded = -1;

} // namespace

extern "C" {

// Loads the model (once). Returns a mask of the entry points found: 1 init, 2 create,
// 4 evaluate, 8 release; 0xf when all are there, 0 when the file did not load.
__declspec(dllexport) int FaceoffNrLoad(const wchar_t* model_path) {
	if (g_model == nullptr && model_path != nullptr) {
		g_model = LoadLibraryExW(model_path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (g_model != nullptr) {
			g_init     = reinterpret_cast<ModelInit>(GetProcAddress(g_model, "NVSDK_NGX_VULKAN_Init_Ext"));
			g_create   = reinterpret_cast<ModelCreate>(GetProcAddress(g_model, "NVSDK_NGX_VULKAN_CreateFeature"));
			g_evaluate = reinterpret_cast<ModelEvaluate>(GetProcAddress(g_model, "NVSDK_NGX_VULKAN_EvaluateFeature"));
			g_release  = reinterpret_cast<ModelRelease>(GetProcAddress(g_model, "NVSDK_NGX_VULKAN_ReleaseFeature"));
		}
	}
	return (g_init != nullptr ? 1 : 0) | (g_create != nullptr ? 2 : 0) | (g_evaluate != nullptr ? 4 : 0) |
	       (g_release != nullptr ? 8 : 0);
}

__declspec(dllexport) int FaceoffNrInit(const wchar_t* data_path, void* instance, void* physical_device, void* device,
                                        int sdk_version) {
	if (g_init == nullptr) return kNotLoaded;
	volatile int result = g_init(0, data_path, instance, physical_device, device, nullptr, sdk_version);
	return result;
}

__declspec(dllexport) int FaceoffNrCreate(void* command_buffer, int feature, void* parameters, void** handle) {
	if (g_create == nullptr) return kNotLoaded;
	volatile int result = g_create(command_buffer, feature, parameters, handle);
	return result;
}

__declspec(dllexport) int FaceoffNrEvaluate(void* command_buffer, void* handle, void* parameters) {
	if (g_evaluate == nullptr) return kNotLoaded;
	volatile int result = g_evaluate(command_buffer, handle, parameters, nullptr);
	return result;
}

__declspec(dllexport) int FaceoffNrRelease(void* handle) {
	if (g_release == nullptr) return kNotLoaded;
	volatile int result = g_release(handle);
	return result;
}

} // extern "C"
