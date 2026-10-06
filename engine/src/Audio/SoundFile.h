#pragma once
// Engine-private: sound files through the virtual file system (pak entries or disk).
#include <miniaudio.h>

#include <filesystem>

namespace Engine {

// Opens `file` for decoding (streaming); ma_decoder_uninit closes it.
ma_result InitFileDecoder(const std::filesystem::path& file, const ma_decoder_config& config, ma_decoder& decoder);

} // namespace Engine
