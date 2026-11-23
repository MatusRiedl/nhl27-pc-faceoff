#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/hostStats.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vramPolicy.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

namespace {

// Deleted images are kept for reuse by an image created with the same parameters: the texture
// cache recreates images whose use alternates (for example a buffer rendered as a color target
// and sampled as a depth texture every frame), and each dedicated allocation can stall the driver
// for hundreds of milliseconds when video memory is under pressure.
struct RecycledImage {
	vk::ImageCreateInfo create;
	VkImage             image      = VK_NULL_HANDLE;
	VmaAllocation       allocation = nullptr;
	uint64_t            size       = 0;
};

constexpr size_t   RecycledImagesMax     = 32;
// NHL 27 frees and recreates about seven aliased full-screen targets (some 125 MiB) every frame.
constexpr uint64_t RecycledImageBytesMax = 768ull * 1024 * 1024;

std::mutex                 g_recycled_mutex;
std::vector<RecycledImage> g_recycled;
uint64_t                   g_recycled_bytes = 0;

bool Recyclable(const vk::ImageCreateInfo& create) {
	return create.pNext == nullptr && create.tiling == vk::ImageTiling::eOptimal &&
	       create.sharingMode == vk::SharingMode::eExclusive &&
	       create.initialLayout == vk::ImageLayout::eUndefined;
}

bool SameCreateInfo(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b) {
	return a.flags == b.flags && a.imageType == b.imageType && a.format == b.format &&
	       a.extent == b.extent && a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers &&
	       a.samples == b.samples && a.usage == b.usage;
}

void DestroyRecycledImages(VmaAllocator allocator) {
	std::lock_guard lock(g_recycled_mutex);
	for (const auto& entry: g_recycled) {
		vmaDestroyImage(allocator, entry.image, entry.allocation);
	}
	g_recycled.clear();
	g_recycled_bytes = 0;
}

std::atomic<bool>     g_memory_priority {false};
std::atomic<uint64_t> g_image_spills {0};

// Bytes the image needs, without creating it (Vulkan 1.3 vkGetDeviceImageMemoryRequirements); 0 when unknown.
uint64_t ImageMemoryBytes(VmaAllocator allocator, const VkImageCreateInfo* create) noexcept {
	const auto query = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceImageMemoryRequirements;
	if (query == nullptr) {
		return 0;
	}
	VmaAllocatorInfo allocator_info {};
	vmaGetAllocatorInfo(allocator, &allocator_info);
	VkDeviceImageMemoryRequirements info {VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
	info.pCreateInfo = create;
	VkMemoryRequirements2 requirements {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	query(allocator_info.device, &info, &requirements);
	return requirements.memoryRequirements.size;
}

// The last try of an image allocation (CreateImage, after the kept images were freed): video memory as before; if the
// driver refuses, a memory type outside video memory. Sampling from system memory goes over PCIe and is slower, but
// the game keeps running instead of exiting (a 6 GB card fills up in a match, docs/notes/low-vram-crash-4050.md).
// Game buffers already end up there through VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE (streamBuffer.cpp). The image stays
// where it lands. Noinline and noexcept: CreateImage swaps one call for this one and keeps its PGO profile.
[[gnu::noinline]] VkResult CreateImageOrSpill(VmaAllocator allocator, const VkImageCreateInfo* create,
                                              const VmaAllocationCreateInfo* alloc_info, VkImage* image,
                                              VmaAllocation* allocation) noexcept {
	const VkResult device_result = vmaCreateImage(allocator, create, alloc_info, image, allocation, nullptr);
	if (device_result == VK_SUCCESS) {
		return VK_SUCCESS;
	}
	const VkPhysicalDeviceMemoryProperties* properties = nullptr;
	vmaGetMemoryProperties(allocator, &properties);
	uint32_t system_types = 0;
	for (uint32_t type = 0; type < properties->memoryTypeCount; type++) {
		if ((properties->memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
			system_types |= 1u << type;
		}
	}
	VkResult system_result = VK_ERROR_FEATURE_NOT_PRESENT;
	if (system_types != 0) {
		VmaAllocationCreateInfo system_info {};
		system_info.memoryTypeBits = system_types;
		*image                     = VK_NULL_HANDLE;
		*allocation                = nullptr;
		system_result              = vmaCreateImage(allocator, create, &system_info, image, allocation, nullptr);
	}
	const uint64_t bytes = ImageMemoryBytes(allocator, create);
	const double   mb    = static_cast<double>(bytes) / (1024.0 * 1024.0);
	if (system_result == VK_SUCCESS) {
		const uint64_t count = g_image_spills.fetch_add(1, std::memory_order_relaxed) + 1;
		if ((count & (count - 1)) == 0) { // the 1st, 2nd, 4th, 8th... spill
			std::printf("NHL27VRAM: image %ux%ux%u format=%d layers=%u levels=%u (%.1f MB) placed in system memory, "
			            "video memory full (%s) n=%llu\n",
			            create->extent.width, create->extent.height, create->extent.depth,
			            static_cast<int>(create->format), create->arrayLayers, create->mipLevels, mb,
			            vk::to_string(static_cast<vk::Result>(device_result)).c_str(),
			            static_cast<unsigned long long>(count));
			std::fflush(stdout);
		}
		return VK_SUCCESS;
	}
	std::printf("NHL27VRAM: image %ux%ux%u format=%d layers=%u levels=%u (%.1f MB) refused: video memory %s, system "
	            "memory %s\n",
	            create->extent.width, create->extent.height, create->extent.depth, static_cast<int>(create->format),
	            create->arrayLayers, create->mipLevels, mb,
	            vk::to_string(static_cast<vk::Result>(device_result)).c_str(),
	            vk::to_string(static_cast<vk::Result>(system_result)).c_str());
	Common::PrintOutOfCommit("Vulkan image", bytes);
	std::fflush(stdout);
	return device_result;
}

// KYTY_DBG_VRAM_RESERVE_MB=N (tests only, vramPolicy.h): hold N MB of video memory for the whole run, in 512 MiB
// blocks at the highest priority (when memory priority is on), so a 16 GB card behaves like a smaller one. The blocks
// are never freed; process exit returns them.
[[gnu::noinline]] void ReserveDebugVram(VkDevice device, const vk::PhysicalDeviceMemoryProperties& properties) noexcept {
	const char* env = std::getenv("KYTY_DBG_VRAM_RESERVE_MB");
	const long long reserve_mb = env != nullptr ? std::atoll(env) : 0;
	if (reserve_mb <= 0) {
		return;
	}
	uint32_t type_index = UINT32_MAX;
	for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
		const auto flags = static_cast<VkMemoryPropertyFlags>(properties.memoryTypes[type].propertyFlags);
		if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
			type_index = type;
			break;
		}
	}
	if (type_index == UINT32_MAX) {
		std::printf("NHL27VRAM: debug reserve skipped, no video-memory type\n");
		return;
	}
	constexpr uint64_t Block    = 512ull * 1024 * 1024;
	const uint64_t     wanted   = static_cast<uint64_t>(reserve_mb) * 1024 * 1024;
	const bool         priority = Vram::PriorityEnabled();
	uint64_t           held     = 0;
	uint32_t           blocks   = 0;
	VkResult           result   = VK_SUCCESS;
	while (held < wanted) {
		VkMemoryPriorityAllocateInfoEXT priority_info {VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT};
		priority_info.priority = 1.0f;
		VkMemoryAllocateInfo info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		info.pNext           = priority ? &priority_info : nullptr;
		info.allocationSize  = std::min(Block, wanted - held);
		info.memoryTypeIndex = type_index;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory(device, &info, nullptr, &memory);
		if (result != VK_SUCCESS) {
			break;
		}
		held += info.allocationSize;
		blocks++;
	}
	std::printf("NHL27VRAM: debug reserve %llu of %lld MB held in %u blocks (priority=%s, type %u)%s%s\n",
	            static_cast<unsigned long long>(held >> 20u), reserve_mb, blocks, priority ? "1.0" : "default",
	            type_index, result == VK_SUCCESS ? "" : ", stopped: ",
	            result == VK_SUCCESS ? "" : vk::to_string(static_cast<vk::Result>(result)).c_str());
	std::fflush(stdout);
}

} // namespace

namespace Vram {

bool PageableWanted() noexcept {
	static const bool wanted = [] {
		const char* value = std::getenv("KYTY_VRAM_PAGEABLE");
		return value == nullptr || value[0] != '0';
	}();
	return wanted;
}

void SetPriorityEnabled(bool enabled) noexcept {
	g_memory_priority.store(enabled, std::memory_order_relaxed);
}

bool PriorityEnabled() noexcept {
	return g_memory_priority.load(std::memory_order_relaxed);
}

} // namespace Vram

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	ReserveDebugVram(static_cast<VkDevice>(device), physical_device_memory_properties);
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	DestroyRecycledImages(allocator);
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	uint64_t memory_objects = 0;
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		// printf, not LOGF: LOGF is silent by default and these lines explain allocation failures.
		printf("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		       ", blocks=%" PRIu64 ", block_count=%u, allocation_count=%u\n",
		       i, static_cast<uint64_t>(budgets[i].usage), static_cast<uint64_t>(budgets[i].budget),
		       static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		       static_cast<uint64_t>(budgets[i].statistics.blockBytes),
		       budgets[i].statistics.blockCount, budgets[i].statistics.allocationCount);
		memory_objects += budgets[i].statistics.blockCount;
	}
	// Every VMA block and every dedicated allocation is one VkDeviceMemory object; the driver
	// refuses new ones past maxMemoryAllocationCount even with memory free.
	printf("VMA memory objects: %" PRIu64 " of maxMemoryAllocationCount=%u\n", memory_objects,
	       physical_device.getProperties().limits.maxMemoryAllocationCount);
	// Deleted images kept for reuse still hold video memory (counted in heap 0's usage above).
	size_t   recycled_count = 0;
	uint64_t recycled_bytes = 0;
	{
		std::lock_guard lock(g_recycled_mutex);
		recycled_count = g_recycled.size();
		recycled_bytes = g_recycled_bytes;
	}
	printf("VMA recycled images: %zu kept, %" PRIu64 " MB; images placed in system memory so far: %" PRIu64 "\n",
	       recycled_count, recycled_bytes >> 20u, g_image_spills.load(std::memory_order_relaxed));
}

void GraphicContext::ReleaseRecycledImages() {
	if (allocator != nullptr) {
		DestroyRecycledImages(allocator);
	}
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint32_t GraphicContext::GetHeapUsage(std::span<HeapUsage, VK_MAX_MEMORY_HEAPS> heaps) const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const auto count = physical_device_memory_properties.memoryHeapCount;
	for (uint32_t heap = 0; heap < count; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		heaps[heap] = {.usage        = budgets[heap].usage,
		               .budget       = budgets[heap].budget,
		               .allocations  = budgets[heap].statistics.allocationCount,
		               .device_local = static_cast<bool>(properties.flags &
		                                                 vk::MemoryHeapFlagBits::eDeviceLocal)};
	}
	return count;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	vk::Image::CType native_image = VK_NULL_HANDLE;
	if (Recyclable(image_info)) {
		std::lock_guard lock(g_recycled_mutex);
		const auto      found =
		    std::find_if(g_recycled.begin(), g_recycled.end(), [&](const auto& entry) {
			    return SameCreateInfo(entry.create, image_info);
		    });
		if (found != g_recycled.end()) {
			native_image     = found->image;
			image.allocation = found->allocation;
			g_recycled_bytes -= found->size;
			g_recycled.erase(found);
		}
	}
	if (native_image == VK_NULL_HANDLE) {
		auto result = static_cast<vk::Result>(vmaCreateImage(
		    allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info), &alloc_info,
		    &native_image, &image.allocation, nullptr));
		if (result != vk::Result::eSuccess) {
			// Memory kept for reuse may be what the new image needs; if video memory still refuses,
			// the image goes to system memory (CreateImageOrSpill).
			DestroyRecycledImages(allocator);
			result = static_cast<vk::Result>(CreateImageOrSpill(
			    allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
			    &alloc_info, &native_image, &image.allocation));
		}
		if (result != vk::Result::eSuccess) {
			image.image = native_image;
			LogMemoryBudget();
			return false;
		}
	}
	image.image = native_image;

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.recyclable = Recyclable(image_info);
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	// Deleted images are idle (their owners defer deletion until the GPU is done with them).
	vk::ImageCreateInfo create {};
	create.flags         = image.flags;
	create.imageType     = image.image_type;
	create.format        = image.format;
	create.extent        = image.extent;
	create.mipLevels     = image.mip_levels;
	create.arrayLayers   = image.layers;
	create.samples       = static_cast<vk::SampleCountFlagBits>(image.samples);
	create.tiling        = vk::ImageTiling::eOptimal;
	create.usage         = image.usage;
	create.sharingMode   = vk::SharingMode::eExclusive;
	create.initialLayout = vk::ImageLayout::eUndefined;
	if (image.recyclable) {
		VmaAllocationInfo allocation_info {};
		vmaGetAllocationInfo(allocator, image.allocation, &allocation_info);
		std::lock_guard lock(g_recycled_mutex);
		g_recycled.push_back({create, image.image, image.allocation, allocation_info.size});
		g_recycled_bytes += allocation_info.size;
		while (g_recycled.size() > RecycledImagesMax || g_recycled_bytes > RecycledImageBytesMax) {
			const auto& oldest = g_recycled.front();
			vmaDestroyImage(allocator, oldest.image, oldest.allocation);
			g_recycled_bytes -= oldest.size;
			g_recycled.erase(g_recycled.begin());
		}
	} else {
		vmaDestroyImage(allocator, image.image, image.allocation);
	}
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
