// v0.30.0 (2026-11-19): the compile queue switch (KYTY_COMPILE_URGENT_FIRST), the worker count, and
// the graphics pipelines a background thread builds (KYTY_ASYNC_GRAPHICS_PIPELINES). Written with
// the code; run with --async-pipelines-only (needs a Vulkan device). The order rules of the queue
// itself are covered by compile_queue_tests, without a device. Each check makes its own
// PipelineCache on the harness's runtime context (its device has the features the pipeline
// creation relies on), after setting the switches the cache reads in its constructor.
namespace AsyncPipelineTest {

void SetEnv(const char* name, const char* value) {
#if defined(_WIN32)
  // An empty value removes the variable.
  _putenv_s(name, value != nullptr ? value : "");
#else
  if (value != nullptr) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

void ClearSwitches() {
  for (const char* name : {"KYTY_COMPILE_URGENT_FIRST", "KYTY_COMPILE_WORKERS", "KYTY_ASYNC_GRAPHICS_PIPELINES",
                           "KYTY_PIPELINE_WAIT_BUDGET_MS", "KYTY_ASYNC_COMPUTE_PIPELINES"}) {
    SetEnv(name, nullptr);
  }
}

// A direct dispatch of 1x1x1 groups: only dispatches with known group counts are queued ahead
// (QueueAllowed), as the resolve thread fills them in before it asks.
void SetOneGroup(ShaderComputeInputInfo& input) {
  for (auto& count : input.workgroup_counts) {
    count = 1;
  }
}

// The compile workers finish what was queued (or the test fails after 30 s).
bool WaitQueueIdle(PipelineCache& cache) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < until) {
    const auto state = PipelineCacheTestAccess::CompileState(cache);
    if (state.queued == 0 && state.in_flight == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

} // namespace AsyncPipelineTest

// Every new switch unset: one queue class as before, nothing queued, no background thread, no budget
// counter, and the worker count of the rule.
void CheckAsyncPipelinesSwitchesOff(VulkanHarness& vulkan) {
  constexpr const char* name = "AsyncPipelinesOff";
  AsyncPipelineTest::ClearSwitches();
  GraphicContext& graphics = vulkan.RuntimeContext();
  {
    PipelineCache cache(graphics, nullptr);
    const auto state = PipelineCacheTestAccess::CompileState(cache);
    Require(name, "one queue class", !state.urgent_first && !state.two_classes,
        "the compile queue left first-in-first-out order without KYTY_COMPILE_URGENT_FIRST");
    Require(name, "nothing queued", state.queued == 0 && state.in_flight == 0 && state.stored_pending == 0,
        "a cache without a store queued compile jobs");
    Require(name, "worker count rule",
        state.workers == Common::Thread::CompileWorkerCount(Common::Thread::GetCpuTopology(), nullptr),
        "the number of compile workers is not the rule's");
    Require(name, "no background graphics",
        !PipelineCacheTestAccess::AsyncGraphics(cache) && !PipelineCacheTestAccess::AsyncGraphicsThread(cache) &&
            !PipelineCacheTestAccess::HasLateRecords(cache),
        "graphics pipelines are built in the background without KYTY_ASYNC_GRAPHICS_PIPELINES");
    Require(name, "no budget counter", PipelineCacheTestAccess::WaitBudgetMs(cache) == 0,
        "the pipeline wait budget is on without KYTY_PIPELINE_WAIT_BUDGET_MS");
  }
  for (const auto& [value, expected] : {std::pair {"0", 6u}, std::pair {"3", 3u}}) {
    AsyncPipelineTest::SetEnv("KYTY_COMPILE_WORKERS", value);
    PipelineCache cache(graphics, nullptr);
    Require(name, "KYTY_COMPILE_WORKERS", PipelineCacheTestAccess::CompileState(cache).workers == expected,
        "KYTY_COMPILE_WORKERS did not set the number of compile workers");
  }
  AsyncPipelineTest::ClearSwitches();
  std::printf("[gpu]     %-32s ok\n", name);
}

// KYTY_COMPILE_URGENT_FIRST=1 with one worker: the execution thread asks for the last of many compute
// programs queued ahead, takes its job back and compiles it itself; no worker compiles it again and no
// worker-made pipeline is left for it (KYTY_ASYNC_COMPUTE_PIPELINES queues compute programs). The
// programs have no user data, so neither the ahead resolution nor the workers read guest memory.
void CheckCompileUrgentFirst(VulkanHarness& vulkan) {
  constexpr const char* name = "CompileUrgentFirst";
  AsyncPipelineTest::ClearSwitches();
  AsyncPipelineTest::SetEnv("KYTY_COMPILE_URGENT_FIRST", "1");
  AsyncPipelineTest::SetEnv("KYTY_ASYNC_COMPUTE_PIPELINES", "1");
  AsyncPipelineTest::SetEnv("KYTY_COMPILE_WORKERS", "1");
  GraphicContext& graphics = vulkan.RuntimeContext();
  {
    PipelineCache cache(graphics, nullptr);
    const auto before = PipelineCacheTestAccess::CompileState(cache);
    Require(name, "two queue classes", before.urgent_first && before.two_classes && before.workers == 1,
        "KYTY_COMPILE_URGENT_FIRST=1 / KYTY_COMPILE_WORKERS=1 were not applied");
    constexpr uint32_t Programs = 64;
    std::vector<std::vector<u32>> codes(Programs);
    std::vector<HW::ComputeShaderInfo> regs(Programs);
    for (uint32_t index = 0; index < Programs; index++) {
      auto& code = codes[index];
      AppendVMovU32(&code, 0, 0x1000u + index); // a different program (hash) each
      AppendEnd(&code);
      ShaderMapUserData(reinterpret_cast<uint64_t>(code.data()),
          {.type = Prospero::ShaderBinaryType::kCs,
           .code_size_bytes = static_cast<u32>(code.size() * sizeof(u32))});
      regs[index].cs_regs.data_addr = reinterpret_cast<uint64_t>(code.data());
      regs[index].cs_regs.num_thread_x = 1;
      regs[index].cs_regs.num_thread_y = 1;
      regs[index].cs_regs.num_thread_z = 1;
      regs[index].cs_regs.wave_size = 32;
    }
    for (uint32_t index = 0; index < Programs; index++) {
      ShaderComputeInputInfo input{};
      AsyncPipelineTest::SetOneGroup(input);
      ProgramResolution ahead;
      (void)cache.GetComputeProgramAhead(regs[index], {}, input, ahead);
    }
    const auto queued = PipelineCacheTestAccess::CompileState(cache);
    Require(name, "queued ahead", queued.in_flight != 0 && queued.taken == 0,
        "the ahead resolution queued no compile job");
    ShaderComputeInputInfo input{};
    AsyncPipelineTest::SetOneGroup(input);
    const auto program = cache.GetComputeProgram(regs[Programs - 1], {}, input);
    Require(name, "program", static_cast<bool>(program), "the taken program did not compile inline");
    // One worker against 64 queued jobs: the last one is still queued when the execution thread
    // asks for it (a machine that compiled all 63 before it would fail here, not wrongly pass).
    Require(name, "job taken", PipelineCacheTestAccess::CompileState(cache).taken == 1,
        "the execution thread waited behind the queue instead of taking its job");
    Require(name, "pipeline", cache.GetComputePipeline(input, program).pipeline != nullptr,
        "the taken program's pipeline was not created inline");
    Require(name, "workers idle", AsyncPipelineTest::WaitQueueIdle(cache), "the compile workers did not finish");
    Require(name, "no orphan pipeline", !PipelineCacheTestAccess::AsyncComputeHas(cache, program.id),
        "a worker made a pipeline for a program whose job was taken");
    ShaderComputeInputInfo again{};
    AsyncPipelineTest::SetOneGroup(again);
    Require(name, "one permutation", cache.GetComputeProgram(regs[Programs - 1], {}, again).id == program.id,
        "the taken program was compiled a second time");
  }
  AsyncPipelineTest::ClearSwitches();
  std::printf("[gpu]     %-32s ok\n", name);
}

// KYTY_ASYNC_GRAPHICS_PIPELINES: a pipeline built for a key is used when the draw brings the inputs
// it was built from, made again when it does not, and a key no pipeline was built for is listed so
// the background thread does not build it. The key and the vertex inputs are those of the harness's
// draw tests (a float2 position and a float4 color in one 24-byte binding, one RGBA32F target, no
// pixel shader), so the pipeline made again is one the driver accepts.
void CheckAsyncGraphicsAdoption(VulkanHarness& vulkan) {
  constexpr const char* name = "AsyncGraphicsAdoption";
  AsyncPipelineTest::ClearSwitches();
  GraphicContext& graphics = vulkan.RuntimeContext();
  const auto spirv = TestSpv::MakePassthroughVertexSpirv(false);
  vk::ShaderModuleCreateInfo module_info{};
  module_info.codeSize = spirv.size() * sizeof(u32);
  module_info.pCode = spirv.data();
  vk::ShaderModule module{};
  Require(name, "vertex module", graphics.device.createShaderModule(&module_info, nullptr, &module) == vk::Result::eSuccess,
      "vkCreateShaderModule failed");
  {
    PipelineCache cache(graphics, nullptr);
    PipelineCacheTestAccess::SetAsyncGraphics(cache, true);
    ShaderRecompiler::IR::CompiledShaderInfo vertex_program{};
    vertex_program.stage = ShaderType::Vertex;
    vertex_program.info.vertex_fetch_components[0] = 2;
    vertex_program.info.vertex_fetch_components[1] = 4;
    ShaderRecompiler::IR::ResourceSnapshot snapshot;
    ShaderVertexInputInfo vertex{};
    vertex.logical_stage = ShaderType::Vertex;
    vertex.stage.program = &vertex_program;
    vertex.stage.resources = &snapshot;
    vertex.resources_num = 2;
    vertex.buffers_num = 1;
    vertex.buffers[0].stride = 6 * sizeof(float);
    vertex.resources[1].UpdateAddress48(2 * sizeof(float));
    for (uint32_t i = 0; i < 2; i++) {
      const auto format = i == 0 ? Prospero::BufferFormat::k32_32Float : Prospero::BufferFormat::k32_32_32_32Float;
      vertex.resources[i].fields[3] = DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(format) << 12u);
      vertex.resources_dst[i].registers_num = i == 0 ? 2 : 4;
    }
    const PipelineCache::GraphicsPrograms programs{{ShaderProgram{0x0a5a0001ull, module}}, ShaderProgram{}};

    using Key = PipelineCacheTestAccess::GraphicsKey;
    Key exact{};
    exact.rendering.color_count = 1;
    exact.rendering.color_formats[0] = vk::Format::eR32G32B32A32Sfloat;
    exact.vertex_shader_ids[0] = 0x0a5a0001ull;
    exact.vertex_input.binding_count = 1;
    exact.vertex_input.bindings[0].stride = 6 * sizeof(float);
    exact.vertex_input.attribute_count = 2;
    exact.vertex_input.attributes[1].offset = 2 * sizeof(float);
    exact.static_params.topology = vk::PrimitiveTopology::eTriangleList;
    exact.static_params.color_mask[0] = 0xf;
    // A stand-in for the pipeline the background thread made: adopting it as it is takes no handle.
    auto built = std::make_unique<PipelineCache::Pipeline>();
    built->prebuilt = true;
    built->input_check = PipelineCacheTestAccess::InputCheck(exact, vertex, nullptr);
    auto* built_pointer = built.get();
    PipelineCacheTestAccess::Offer(cache, exact, std::move(built));
    auto* adopted = PipelineCacheTestAccess::Adopt(cache, exact, std::span {&vertex, 1u}, nullptr, programs);
    auto state = PipelineCacheTestAccess::AsyncGraphicsState(cache);
    Require(name, "exact inputs adopted",
        adopted == built_pointer && !adopted->prebuilt && state.adopted == 1 && state.rebuilt == 0 &&
            state.offered == 0 && state.pipelines == 1,
        "a pipeline built for the draw's key and inputs was not used as it is");

    Key other = exact;
    other.static_params.topology = vk::PrimitiveTopology::eTriangleStrip;
    auto stale = std::make_unique<PipelineCache::Pipeline>();
    stale->prebuilt = true;
    stale->input_check = PipelineCacheTestAccess::InputCheck(other, vertex, nullptr) ^ 1u; // another draw's inputs
    PipelineCacheTestAccess::Offer(cache, other, std::move(stale));
    auto* rebuilt = PipelineCacheTestAccess::Adopt(cache, other, std::span {&vertex, 1u}, nullptr, programs);
    state = PipelineCacheTestAccess::AsyncGraphicsState(cache);
    Require(name, "other inputs rebuilt",
        rebuilt != nullptr && rebuilt->pipeline != nullptr && rebuilt->pipeline_layout != nullptr &&
            !rebuilt->prebuilt && state.rebuilt == 1 && state.offered == 0 && state.pipelines == 2,
        "a pipeline built from other inputs was used, or not made again for the draw");

    Key missing = exact;
    missing.vertex_shader_ids[0] = 0x0a5a0002ull;
    Require(name, "not built", PipelineCacheTestAccess::Adopt(cache, missing, std::span {&vertex, 1u}, nullptr, programs) == nullptr,
        "a pipeline was adopted for a key nothing was built for");
    state = PipelineCacheTestAccess::AsyncGraphicsState(cache);
    Require(name, "drawn key listed", state.inline_keys == 1 && state.pipelines == 2,
        "the key the draw makes itself was not listed for the background thread");
  }
  graphics.device.destroyShaderModule(module, nullptr);
  AsyncPipelineTest::ClearSwitches();
  std::printf("[gpu]     %-32s ok\n", name);
}
