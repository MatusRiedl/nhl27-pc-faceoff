#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTORE_H_

#include "common/file.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/shader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

// The state of every pipeline a session created at first use, appended to a file next to the
// program store. Programs are named by a hash of what identifies their permutation, never by a
// session's handle, so the next session can build the same pipelines once its programs are
// compiled and before the game draws, instead of on the execution thread when a scene first
// uses them. The file holds state only (no shader code).
struct StoredPipeline {
	bool compute = false;
	// Graphics: the vertex program. Compute: the compute program.
	uint64_t program_uid = 0;
	bool     pixel       = false;
	uint64_t pixel_uid   = 0;
	// Graphics only: the parts of the pipeline key that are not program handles.
	PipelineRenderingState   rendering;
	PipelineVertexInputState vertex_input;
	PipelineStaticParameters static_params;
	// ShaderVertexInputInfo (graphics) or ShaderComputeInputInfo (compute), `stage` cleared.
	std::vector<uint8_t> input_info;
	// ShaderPixelInputInfo, `stage` cleared; empty without a pixel shader.
	std::vector<uint8_t> pixel_info;
};

// Used by the thread that owns the pipeline cache only.
class PipelineStore {
public:
	static_assert(std::is_trivially_copyable_v<ShaderVertexInputInfo> &&
	              std::is_trivially_copyable_v<ShaderPixelInputInfo> &&
	              std::is_trivially_copyable_v<ShaderComputeInputInfo> &&
	              std::is_trivially_copyable_v<PipelineRenderingState> &&
	              std::is_trivially_copyable_v<PipelineVertexInputState> &&
	              std::is_trivially_copyable_v<PipelineStaticParameters>);

	PipelineStore() = default;
	PipelineStore(const PipelineStore&)            = delete;
	PipelineStore& operator=(const PipelineStore&) = delete;
	~PipelineStore() {
		if (m_file != nullptr) {
			std::fclose(m_file);
		}
	}

	// Loads the distinct valid records of `path` and keeps the file open for appending. Records
	// of another GPU are not used: its features decide how a stage is built.
	std::vector<StoredPipeline> Open(const std::filesystem::path& path, uint32_t vendor,
	                                 uint32_t device) {
		std::vector<StoredPipeline> pipelines;
		const auto                  text       = Common::PathToString(path);
		const auto                  header     = Header(vendor, device);
		bool                        valid      = false;
		uint64_t                    valid_size = 0;
		uint64_t                    file_size  = 0;
		m_mark                                 = path;
		m_mark += ".building";
		std::error_code error;
		// A prebuild that did not finish (a record the driver crashed on, or a killed start-up)
		// must not repeat at every start: the file starts empty again.
		m_discarded = std::filesystem::exists(m_mark, error);
		if (m_discarded) {
			std::filesystem::remove(m_mark, error);
		} else if (auto* file = std::fopen(text.c_str(), "rb"); file != nullptr) {
			std::vector<uint8_t> data;
			std::fseek(file, 0, SEEK_END);
			const auto size = std::ftell(file);
			std::fseek(file, 0, SEEK_SET);
			if (size > 0 && size < (int64_t {1} << 30u)) {
				data.resize(static_cast<size_t>(size));
				data.resize(std::fread(data.data(), 1, data.size(), file));
			}
			std::fclose(file);
			file_size = data.size();
			if (data.size() >= header.size() &&
			    std::memcmp(data.data(), header.data(), header.size()) == 0) {
				valid      = true;
				valid_size = header.size();
				for (size_t offset = header.size();;) {
					StoredPipeline pipeline;
					const auto     next = Parse(data, offset, pipeline);
					if (next == 0) {
						break;
					}
					offset     = next;
					valid_size = next;
					if (m_seen.insert(Identity(pipeline)).second) {
						pipelines.push_back(std::move(pipeline));
					}
				}
			}
		}
		if (!Common::File::CreateDirectories(path.parent_path())) {
			m_seen.clear();
			return {};
		}
		if (valid && valid_size != file_size) {
			// The torn record of a crash ends the file: drop it before appending.
			std::filesystem::resize_file(path, valid_size, error);
			valid = !error;
		}
		m_file = std::fopen(text.c_str(), valid ? "ab" : "wb");
		if (m_file != nullptr && !valid) {
			pipelines.clear();
			m_seen.clear();
			std::fwrite(header.data(), 1, header.size(), m_file);
			std::fflush(m_file);
		}
		if (m_file == nullptr) {
			pipelines.clear();
			m_seen.clear();
		}
		return pipelines;
	}

	[[nodiscard]] bool IsOpen() const { return m_file != nullptr; }
	// The file was emptied because the last prebuild did not finish.
	[[nodiscard]] bool Discarded() const { return m_discarded; }

	// False when the pipeline is already in the file (or the file is not open).
	bool Append(const StoredPipeline& pipeline) {
		if (m_file == nullptr || !m_seen.insert(Identity(pipeline)).second) {
			return false;
		}
		std::vector<uint8_t> record;
		Serialize(pipeline, record);
		std::fwrite(record.data(), 1, record.size(), m_file);
		std::fflush(m_file);
		return true;
	}

	void BeginPrebuild() {
		if (auto* mark = std::fopen(Common::PathToString(m_mark).c_str(), "wb"); mark != nullptr) {
			std::fclose(mark);
		}
	}
	void EndPrebuild() {
		std::error_code error;
		std::filesystem::remove(m_mark, error);
	}

	// What makes two records the same pipeline: the programs and the key state, not the copies
	// of the stage inputs (those carry values of the draw that happened to come first).
	static uint64_t Identity(const StoredPipeline& pipeline) {
		std::vector<uint8_t> bytes;
		Put(bytes, static_cast<uint8_t>(pipeline.compute));
		Put(bytes, pipeline.program_uid);
		if (!pipeline.compute) {
			Put(bytes, static_cast<uint8_t>(pipeline.pixel));
			Put(bytes, pipeline.pixel ? pipeline.pixel_uid : uint64_t {0});
			Put(bytes, pipeline.rendering.color_count);
			for (const auto format: pipeline.rendering.color_formats) {
				Put(bytes, static_cast<uint32_t>(format));
			}
			Put(bytes, static_cast<uint32_t>(pipeline.rendering.depth_format));
			Put(bytes, static_cast<uint32_t>(pipeline.rendering.stencil_format));
			const auto& input = pipeline.vertex_input;
			Put(bytes, input.binding_count);
			for (uint32_t index = 0; index < input.binding_count; index++) {
				Put(bytes, input.bindings[index].stride);
				Put(bytes, static_cast<uint8_t>(input.bindings[index].instance));
			}
			Put(bytes, input.attribute_count);
			for (uint32_t index = 0; index < input.attribute_count; index++) {
				Put(bytes, input.attributes[index].offset);
				Put(bytes, input.attributes[index].binding);
			}
			Put(bytes, pipeline.static_params);
		}
		return XXH3_64bits(bytes.data(), bytes.size());
	}

private:
	static std::string Header(uint32_t vendor, uint32_t device) {
		// The layouts the records copy, and the GPU they were made on.
		return fmt::format("KytyPipelines1:{}:{}:{}:{}:{}:{}:{:08x}:{:08x}\n",
		                   sizeof(ShaderVertexInputInfo), sizeof(ShaderPixelInputInfo),
		                   sizeof(ShaderComputeInputInfo), sizeof(PipelineRenderingState),
		                   sizeof(PipelineVertexInputState), sizeof(PipelineStaticParameters),
		                   vendor, device);
	}

	template <typename T>
	static void Put(std::vector<uint8_t>& out, const T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		out.insert(out.end(), bytes, bytes + sizeof(T));
	}
	static void PutBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& values) {
		Put(out, static_cast<uint32_t>(values.size()));
		out.insert(out.end(), values.begin(), values.end());
	}

	// A record is its payload size (u32), the payload and the XXH3 of the payload (u64).
	static void Serialize(const StoredPipeline& pipeline, std::vector<uint8_t>& out) {
		std::vector<uint8_t> payload;
		Put(payload, static_cast<uint8_t>(pipeline.compute));
		Put(payload, pipeline.program_uid);
		Put(payload, static_cast<uint8_t>(pipeline.pixel));
		Put(payload, pipeline.pixel_uid);
		Put(payload, pipeline.rendering);
		Put(payload, pipeline.vertex_input);
		Put(payload, pipeline.static_params);
		PutBytes(payload, pipeline.input_info);
		PutBytes(payload, pipeline.pixel_info);
		Put(out, static_cast<uint32_t>(payload.size()));
		out.insert(out.end(), payload.begin(), payload.end());
		Put(out, XXH3_64bits(payload.data(), payload.size()));
	}

	// The offset after the record, or 0 when no complete valid record starts at `offset`.
	static size_t Parse(const std::vector<uint8_t>& data, size_t offset, StoredPipeline& pipeline) {
		uint32_t payload_size = 0;
		if (data.size() - offset < sizeof(payload_size)) {
			return 0;
		}
		std::memcpy(&payload_size, data.data() + offset, sizeof(payload_size));
		const auto begin = offset + sizeof(payload_size);
		if (data.size() - begin < uint64_t {payload_size} + sizeof(uint64_t)) {
			return 0;
		}
		uint64_t stored_hash = 0;
		std::memcpy(&stored_hash, data.data() + begin + payload_size, sizeof(stored_hash));
		if (XXH3_64bits(data.data() + begin, payload_size) != stored_hash) {
			return 0;
		}
		size_t     cursor = begin;
		const auto end    = begin + payload_size;
		bool       ok     = true;
		const auto get    = [&](auto& value) {
			if (!ok || end - cursor < sizeof(value)) {
				ok = false;
				return;
			}
			std::memcpy(&value, data.data() + cursor, sizeof(value));
			cursor += sizeof(value);
		};
		const auto get_bytes = [&](std::vector<uint8_t>& values) {
			uint32_t count = 0;
			get(count);
			if (!ok || end - cursor < count) {
				ok = false;
				return;
			}
			values.assign(data.data() + cursor, data.data() + cursor + count);
			cursor += count;
		};
		uint8_t compute = 0;
		uint8_t pixel   = 0;
		get(compute);
		get(pipeline.program_uid);
		get(pixel);
		get(pipeline.pixel_uid);
		get(pipeline.rendering);
		get(pipeline.vertex_input);
		get(pipeline.static_params);
		get_bytes(pipeline.input_info);
		get_bytes(pipeline.pixel_info);
		pipeline.compute = compute == 1;
		pipeline.pixel   = pixel == 1;
		if (!ok || cursor != end || compute > 1 || pixel > 1 || (pipeline.compute && pipeline.pixel)) {
			return 0;
		}
		const auto input_size =
		    pipeline.compute ? sizeof(ShaderComputeInputInfo) : sizeof(ShaderVertexInputInfo);
		const auto pixel_size = pipeline.pixel ? sizeof(ShaderPixelInputInfo) : size_t {0};
		if (pipeline.input_info.size() != input_size || pipeline.pixel_info.size() != pixel_size ||
		    pipeline.rendering.color_count > pipeline.rendering.color_formats.size() ||
		    pipeline.vertex_input.binding_count > pipeline.vertex_input.bindings.size() ||
		    pipeline.vertex_input.attribute_count > pipeline.vertex_input.attributes.size()) {
			return 0;
		}
		return end + sizeof(uint64_t);
	}

	std::FILE*                   m_file = nullptr;
	std::filesystem::path        m_mark;
	bool                         m_discarded = false;
	std::unordered_set<uint64_t> m_seen;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINESTORE_H_
