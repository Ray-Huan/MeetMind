// MeetMind — WAV 读写
// 支持 RIFF/WAVE 容器，PCM 8/16/24/32 bit 与 IEEE float 32/64 bit，含 WAVE_FORMAT_EXTENSIBLE。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mm/audio/audio_types.h"
#include "mm/common/result.h"

namespace mm::wav {

/// 仅读取头部元信息，不加载采样数据。
Result<WavInfo> probe(const std::string& path);

/// 读取 WAV 文件并转换为单声道 float 缓冲。
/// @param path    文件路径
/// @param report  非空时写入音频质量报告
Result<AudioBuffer> read(const std::string& path, AudioQualityReport* report = nullptr);

/// 从内存字节流解析（用于测试与内嵌资源）。
Result<AudioBuffer> readFromMemory(const std::vector<uint8_t>& bytes,
                                   AudioQualityReport* report = nullptr,
                                   WavInfo* info = nullptr);

/// 写出 16 bit PCM WAV（单声道）。
Result<void> write16(const std::string& path, const AudioBuffer& buffer);

/// 写出指定位深的 PCM WAV；bitsPerSample ∈ {16, 32}（32 表示 float32）。
Result<void> write(const std::string& path, const AudioBuffer& buffer, int bitsPerSample = 16);

/// 序列化为 WAV 字节流（内存中构造，便于测试）。
std::vector<uint8_t> encodeToMemory(const AudioBuffer& buffer, int bitsPerSample = 16);

/// 计算音频质量指标。
AudioQualityReport analyzeQuality(const AudioBuffer& buffer, const WavInfo& info);

}  // namespace mm::wav
