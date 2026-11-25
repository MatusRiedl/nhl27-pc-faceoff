#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/nativeIndirect.h"
#include "graphics/host_gpu/renderer/bindEpoch.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "gpu_tiler_shaders/gpu_tiler_native_indirect_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_dispatch_guard_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_color_clear_check_spv.h"
#include "common/assert.h"
#include "kernel/memory.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace Libs::Graphics {
uint32_t NativeIndirectGuardMaxWork() noexcept {
 static const uint32_t max_work = [] {
  // NHL 27's largest direct draws are about 24,000 indices x 4 instances; 64M vertex invocations take
  // a few milliseconds. The hang draw (VS 5862e85c) ran for seconds.
  uint32_t value = 1u << 26u;
  if (const char* text = std::getenv("KYTY_DRAW_GUARD_MAX_WORK"); text != nullptr && text[0] != '\0') {
   char* end = nullptr;
   const unsigned long parsed = std::strtoul(text, &end, 0);
   if (end != text && *end == '\0' && (parsed == 0ul || (parsed >= 65536ul && parsed <= 0xfffffffful))) {
    value = static_cast<uint32_t>(parsed);
   }
  }
  std::printf("NHL27DRAWGUARD: max_work=%u (KYTY_DRAW_GUARD_MAX_WORK, 0 = off)\n", value);
  std::fflush(stdout);
  return value;
 }();
 return max_work;
}
NativeIndirectPrep::NativeIndirectPrep(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_max_work(NativeIndirectGuardMaxWork()) {
 m_report = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
                                     AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, 512);
 std::memset(m_report->Mapped().data(), 0, m_report->Mapped().size());
 m_report->Flush(0, m_report->Size());
 const vk::PushConstantRange range {vk::ShaderStageFlagBits::eCompute, 0, 48};
 vk::PipelineLayoutCreateInfo layout {};
 layout.pushConstantRangeCount = 1;
 layout.pPushConstantRanges = &range;
 RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout, nullptr, &m_layout), "native indirect layout");
 const auto module = CompileSPV(GPU_TILER_NATIVE_INDIRECT_SPV, graphics.device);
 vk::ComputePipelineCreateInfo info {};
 info.layout = m_layout;
 info.stage.stage = vk::ShaderStageFlagBits::eCompute;
 info.stage.module = module;
 info.stage.pName = "main";
 const auto result = graphics.device.createComputePipelines(nullptr, 1, &info, nullptr, &m_pipeline);
 graphics.device.destroyShaderModule(module, nullptr);
 RequireVulkanSuccess(result, "native indirect count preparation");
}
NativeIndirectPrep::~NativeIndirectPrep() {
 m_graphics.device.destroyPipeline(m_pipeline, nullptr);
 m_graphics.device.destroyPipelineLayout(m_layout, nullptr);
}
DispatchArgsGuard::DispatchArgsGuard(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics) {
 m_ring = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                                   AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress,
                                   SlotBytes * SlotCount);
 m_trips = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Stream, 0,
                                    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, 512);
 std::memset(m_trips->Mapped().data(), 0, m_trips->Mapped().size());
 m_trips->Flush(0, m_trips->Size());
 const vk::PushConstantRange range {vk::ShaderStageFlagBits::eCompute, 0, 48};
 vk::PipelineLayoutCreateInfo layout {};
 layout.pushConstantRangeCount = 1;
 layout.pPushConstantRanges = &range;
 RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout, nullptr, &m_layout), "dispatch guard layout");
 const auto module = CompileSPV(GPU_TILER_DISPATCH_GUARD_SPV, graphics.device);
 vk::ComputePipelineCreateInfo info {};
 info.layout = m_layout;
 info.stage.stage = vk::ShaderStageFlagBits::eCompute;
 info.stage.module = module;
 info.stage.pName = "main";
 const auto result = graphics.device.createComputePipelines(nullptr, 1, &info, nullptr, &m_pipeline);
 graphics.device.destroyShaderModule(module, nullptr);
 RequireVulkanSuccess(result, "dispatch guard pipeline");
 // NHL 27 also dispatches a CS (9ba58a11, 8714aa79) with 50,589,920 x 1 x 1 groups from a record
 // that grows over a match: within the limits, but the GPU then runs for seconds (TDR, device
 // lost; caught by breadcrumbs). Its normal dispatches stay below 1,000 groups, a full-screen 4K
 // pass of 8x8 groups is about 130,000: more than 4M groups is taken as garbage. A record that
 // grows to just below 4M groups passes and keeps the GPU busy for about 180 ms per frame (5 fps
 // in play while "CPU memory there holds 0 0 0" guard lines print): a lower limit cuts it earlier.
 if (const char* value = std::getenv("KYTY_DISPATCH_GUARD_MAX_GROUPS"); value != nullptr && value[0] != '\0') {
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 0);
  if (end != value && *end == '\0' && parsed >= 1024ul && parsed <= 0xfffffffful) {
   m_max_groups = static_cast<uint32_t>(parsed);
  }
 }
 std::printf("NHL27GUARD: max_groups=%u (KYTY_DISPATCH_GUARD_MAX_GROUPS)\n", m_max_groups);
 std::fflush(stdout);
}
DispatchArgsGuard::~DispatchArgsGuard() {
 m_graphics.device.destroyPipeline(m_pipeline, nullptr);
 m_graphics.device.destroyPipelineLayout(m_layout, nullptr);
}
std::pair<vk::Buffer, uint64_t> DispatchArgsGuard::Record(CommandBuffer& buffer, const Buffer& args,
                                                          uint64_t offset, uint64_t guest_address) {
 const auto command = buffer.Handle();
 // The trip counter is only reported: look every 64 dispatches (it lives in video memory the
 // CPU reads uncached, about 1% of the execution thread when read every time).
 if ((m_next & 63u) == 0) {
 if (!m_trips->IsCoherent()) m_trips->Invalidate(0, m_trips->Size());
 const auto* trips = reinterpret_cast<const volatile uint32_t*>(m_trips->Mapped().data());
 if (const uint32_t count = trips[0]; count != m_reported_trips) {
  const auto* addresses = reinterpret_cast<const volatile uint64_t*>(m_trips->Mapped().data() + 8);
  // The raw x, y, z the guard read (uvec4 per trip at byte 144, gpu_tiler_dispatch_guard.comp).
  const auto* values = reinterpret_cast<const volatile uint32_t*>(m_trips->Mapped().data() + 144);
  const uint32_t at = (count - 1u) % 16u;
  const uint64_t last = addresses[at];
  uint32_t backing[3] {};
  const bool readable = LibKernel::Memory::TryReadBacking(last, backing, sizeof(backing));
  std::printf("NHL27GUARD: zeroed %u indirect dispatch(es) with out-of-range group counts; last args at "
              "0x%016llx, GPU copy held %u %u %u, CPU memory there holds %u %u %u%s\n",
              count - m_reported_trips, static_cast<unsigned long long>(last), values[at * 4u],
              values[at * 4u + 1u], values[at * 4u + 2u], backing[0], backing[1], backing[2],
              readable ? "" : " (unreadable)");
  std::fflush(stdout);
  m_reported_trips = count;
 }
 }
 const uint64_t slot = (m_next++ % SlotCount) * SlotBytes;
 vk::MemoryBarrier written {};
 written.srcAccessMask = vk::AccessFlagBits::eShaderWrite | vk::AccessFlagBits::eTransferWrite;
 written.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
 command.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader |
                             vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eDrawIndirect,
                         vk::PipelineStageFlagBits::eComputeShader, {}, 1, &written, 0, nullptr, 0, nullptr);
 struct Params { uint64_t source, destination, trips, guest_address; uint32_t max_x, max_y, max_z, max_groups; };
 static_assert(sizeof(Params) == 48);
 const auto& limits = m_graphics.GetPhysicalDeviceProperties().limits;
 // The limit on the group-count product: see the constructor (default 4M groups).
 const Params params {args.BufferDeviceAddress() + offset, m_ring->BufferDeviceAddress() + slot,
                      m_trips->BufferDeviceAddress(), guest_address,
                      limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1],
                      limits.maxComputeWorkGroupCount[2], m_max_groups};
 // Binding another compute pipeline invalidates the elided-bind caches of the dispatch path.
 Libs::Graphics::g_compute_pipeline_epoch.fetch_add(1, std::memory_order_relaxed);
 command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
 InvalidateRedundantBinds();
 command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
 command.dispatch(1, 1, 1);
 vk::MemoryBarrier guarded {};
 guarded.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
 guarded.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
 command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eDrawIndirect,
                         {}, 1, &guarded, 0, nullptr, 0, nullptr);
 return {m_ring->Handle(), slot};
}
ColorClearCheck::ColorClearCheck(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics) {
 m_ring = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                                   AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress |
                                       vk::BufferUsageFlagBits::eConditionalRenderingEXT,
                                   SliceBytes * SlotCount);
 const vk::PushConstantRange range {vk::ShaderStageFlagBits::eCompute, 0, 40};
 vk::PipelineLayoutCreateInfo layout {};
 layout.pushConstantRangeCount = 1;
 layout.pPushConstantRanges = &range;
 RequireVulkanSuccess(graphics.device.createPipelineLayout(&layout, nullptr, &m_layout), "color clear check layout");
 const auto module = CompileSPV(GPU_TILER_COLOR_CLEAR_CHECK_SPV, graphics.device);
 vk::ComputePipelineCreateInfo info {};
 info.layout = m_layout;
 info.stage.stage = vk::ShaderStageFlagBits::eCompute;
 info.stage.module = module;
 info.stage.pName = "main";
 const auto result = graphics.device.createComputePipelines(nullptr, 1, &info, nullptr, &m_pipeline);
 graphics.device.destroyShaderModule(module, nullptr);
 RequireVulkanSuccess(result, "color clear check pipeline");
}
ColorClearCheck::~ColorClearCheck() {
 m_graphics.device.destroyPipeline(m_pipeline, nullptr);
 m_graphics.device.destroyPipelineLayout(m_layout, nullptr);
}
std::pair<vk::Buffer, uint64_t> ColorClearCheck::Record(CommandBuffer& buffer, vk::DeviceAddress metadata,
                                                        uint32_t slice_words, uint32_t slices,
                                                        std::span<const uint8_t> codes, bool expand) {
 EXIT_IF(slices == 0 || slices > SlotCount || codes.empty() || codes.size() > 8);
 const auto command = buffer.Handle();
 if (m_next + slices > SlotCount) m_next = 0;
 const uint64_t first = m_next * SliceBytes;
 m_next += slices;
 // The metadata was written by earlier GPU work; the ring slots by an earlier check.
 vk::MemoryBarrier written {};
 written.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
 written.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
 command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eComputeShader, {}, 1,
                         &written, 0, nullptr, 0, nullptr);
 struct Params { uint64_t metadata, predicates; uint32_t slice_words, candidates_lo, candidates_hi, count, expand, reserved; };
 static_assert(sizeof(Params) == 40);
 Params params {metadata, m_ring->BufferDeviceAddress() + first, slice_words, 0, 0,
                static_cast<uint32_t>(codes.size()), expand ? 1u : 0u, 0};
 for (size_t k = 0; k < codes.size(); ++k) {
  (k < 4 ? params.candidates_lo : params.candidates_hi) |= static_cast<uint32_t>(codes[k]) << (8u * (k & 3u));
 }
 Libs::Graphics::g_compute_pipeline_epoch.fetch_add(1, std::memory_order_relaxed);
 command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
 InvalidateRedundantBinds();
 command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
 command.dispatch(slices, 1, 1);
 vk::MemoryBarrier checked {};
 checked.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
 checked.dstAccessMask = vk::AccessFlagBits::eConditionalRenderingReadEXT | vk::AccessFlagBits::eMemoryRead |
                         vk::AccessFlagBits::eMemoryWrite;
 command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                         vk::PipelineStageFlagBits::eConditionalRenderingEXT | vk::PipelineStageFlagBits::eAllCommands,
                         {}, 1, &checked, 0, nullptr, 0, nullptr);
 return {m_ring->Handle(), first};
}
void NativeIndirectPrep::Report() {
 // The report is written by earlier draws' GPU work and read without waiting: values may lag a frame.
 if (!m_report->IsCoherent()) m_report->Invalidate(0, m_report->Size());
 const auto* base = m_report->Mapped().data();
 const auto* words = reinterpret_cast<const volatile uint32_t*>(base);
 const uint32_t trips = words[0];
 const uint32_t checked = words[1];
 if (trips != m_reported_trips) {
  const uint32_t index = (trips - 1u) % 16u;
  const uint64_t address = reinterpret_cast<const volatile uint64_t*>(base + 32)[index];
  const auto* values = reinterpret_cast<const volatile uint32_t*>(base + 160) + index * 2u;
  uint32_t backing[4] {};
  const bool readable = LibKernel::Memory::TryReadBacking(address, backing, sizeof(backing));
  std::printf("NHL27DRAWGUARD: zeroed %u indirect draw record(s) over %u vertices x instances (%u checked); last at "
              "0x%016llx held %u x %u, CPU memory there holds %u %u %u %u%s\n",
              trips - m_reported_trips, m_max_work, checked, static_cast<unsigned long long>(address), values[0],
              values[1], backing[0], backing[1], backing[2], backing[3], readable ? "" : " (unreadable)");
  std::fflush(stdout);
  m_reported_trips = trips;
 }
 // The largest record under the limit, at most every 1024 records: normal sizes, or a record that grows.
 const uint32_t largest = words[2];
 if (largest != m_reported_max && (m_reported_max == 0 || m_calls - m_max_printed_at >= 1024u)) {
  const uint64_t address = *reinterpret_cast<const volatile uint64_t*>(base + 16);
  const auto* values = reinterpret_cast<const volatile uint32_t*>(base + 24);
  uint32_t backing[4] {};
  const bool readable = LibKernel::Memory::TryReadBacking(address, backing, sizeof(backing));
  std::printf("NHL27DRAWGUARD: checked=%u largest=%u at 0x%016llx (%u x %u), CPU memory there holds %u %u %u %u%s\n",
              checked, largest, static_cast<unsigned long long>(address), values[0], values[1], backing[0],
              backing[1], backing[2], backing[3], readable ? "" : " (unreadable)");
  std::fflush(stdout);
  m_reported_max = largest;
  m_max_printed_at = m_calls;
 }
}
void NativeIndirectPrep::Record(CommandBuffer& buffer, const NativeIndirectDraw& source) {
 // The report lives in memory the CPU reads uncached: look every 16 records.
 if (m_max_work != 0 && (m_calls & 15u) == 0) Report();
 ++m_calls;
 const auto command = buffer.Handle();
 vk::MemoryBarrier ready {};
 ready.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
 ready.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
 command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
                         {}, 1, &ready, 0, nullptr, 0, nullptr);
 struct Params { uint64_t address, report, guest_address; uint32_t stride, maximum, limit, max_work, count_word, reserved; };
 static_assert(sizeof(Params) == 48);
 // The snapshot holds the records, then the draw count (the count buffer's word or max_count).
 const Params params {source.snapshot->BufferDeviceAddress(), m_report->BufferDeviceAddress(), source.args_addr,
                      source.stride / 4, source.max_count, source.indexed ? source.index_count_limit : UINT32_MAX,
                      m_max_work, static_cast<uint32_t>(std::min<uint64_t>(source.ArgsSize() / 4u, UINT32_MAX)), 0};
 Libs::Graphics::g_compute_pipeline_epoch.fetch_add(1, std::memory_order_relaxed);
 command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
 InvalidateRedundantBinds();
 command.pushConstants(m_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params), &params);
 command.dispatch(static_cast<uint32_t>((static_cast<uint64_t>(source.max_count) + 63u) / 64u), 1, 1);
}
}
