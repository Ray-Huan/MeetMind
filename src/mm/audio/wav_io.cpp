#include "mm/audio/wav_io.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "mm/common/path_utils.h"
#include "mm/common/logger.h"
#include "mm/common/string_utils.h"
#include "mm/common/time_utils.h"

namespace mm::wav {
namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void writeU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void writeU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

std::string encodingName(int formatCode, int bits) {
    if (formatCode == kFormatFloat) {
        return bits == 64 ? "FLOAT64" : "FLOAT32";
    }
    switch (bits) {
        case 8: return "PCM8";
        case 16: return "PCM16";
        case 24: return "PCM24";
        case 32: return "PCM32";
        default: return "PCM" + std::to_string(bits);
    }
}

/// 将单帧的多个声道下混为单声道样本。
float mixChannels(const uint8_t* frame, int channels, int bits, int formatCode) {
    double acc = 0.0;
    for (int c = 0; c < channels; ++c) {
        const uint8_t* p = frame;
        double v = 0.0;
        if (formatCode == kFormatFloat) {
            if (bits == 32) {
                float f = 0.0f;
                std::memcpy(&f, p + c * 4, 4);
                v = f;
            } else {
                double d = 0.0;
                std::memcpy(&d, p + c * 8, 8);
                v = d;
            }
        } else {
            switch (bits) {
                case 8: {
                    // WAV 8 bit 为无符号，偏置 128
                    v = (static_cast<int>(p[c]) - 128) / 128.0;
                    break;
                }
                case 16: {
                    int16_t s = 0;
                    std::memcpy(&s, p + c * 2, 2);
                    v = s / 32768.0;
                    break;
                }
                case 24: {
                    const uint8_t* q = p + c * 3;
                    int32_t s = static_cast<int32_t>(q[0]) |
                                (static_cast<int32_t>(q[1]) << 8) |
                                (static_cast<int32_t>(q[2]) << 16);
                    if (s & 0x00800000) s |= static_cast<int32_t>(0xFF000000);
                    v = s / 8388608.0;
                    break;
                }
                case 32: {
                    int32_t s = 0;
                    std::memcpy(&s, p + c * 4, 4);
                    v = s / 2147483648.0;
                    break;
                }
                default:
                    v = 0.0;
            }
        }
        acc += v;
    }
    return static_cast<float>(acc / channels);
}

struct ParsedHeader {
    WavInfo info;
    const uint8_t* dataPtr = nullptr;
    size_t dataSize = 0;
};

Result<ParsedHeader> parseHeader(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 44) {
        return fail(ErrorCode::FormatUnsupported, "文件过小，不足以构成 WAV 头");
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return fail(ErrorCode::FormatUnsupported, "不是合法的 RIFF/WAVE 文件");
    }

    ParsedHeader out;
    bool haveFmt = false;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const char* cid = reinterpret_cast<const char*>(bytes.data() + pos);
        const uint32_t chunkSize = readU32(bytes.data() + pos + 4);
        const size_t body = pos + 8;
        if (std::memcmp(cid, "fmt ", 4) == 0) {
            if (chunkSize < 16 || body + 16 > bytes.size()) {
                return fail(ErrorCode::FormatUnsupported, "fmt 块长度非法");
            }
            const uint8_t* f = bytes.data() + body;
            out.info.formatCode = readU16(f);
            out.info.channels = readU16(f + 2);
            out.info.sampleRate = static_cast<int>(readU32(f + 4));
            out.info.bitsPerSample = readU16(f + 14);
            if (out.info.formatCode == kFormatExtensible && chunkSize >= 40) {
                // SubFormat GUID 前 2 字节即真实格式码
                const uint16_t sub = readU16(f + 24);
                if (sub == kFormatPcm || sub == kFormatFloat) {
                    out.info.formatCode = sub;
                }
            }
            haveFmt = true;
        } else if (std::memcmp(cid, "data", 4) == 0) {
            const size_t available = bytes.size() - body;
            out.dataSize = std::min<size_t>(chunkSize, available);
            out.dataPtr = bytes.data() + body;
        }
        // 块按偶数字节对齐
        size_t advance = static_cast<size_t>(chunkSize) + (chunkSize & 1u);
        if (chunkSize == 0 && std::memcmp(cid, "data", 4) != 0) advance = 0;  // 防死循环
        pos = body + advance;
        if (pos <= body) break;
    }

    if (!haveFmt) return fail(ErrorCode::FormatUnsupported, "缺少 fmt 块");
    if (out.dataPtr == nullptr) return fail(ErrorCode::FormatUnsupported, "缺少 data 块");
    if (out.info.channels <= 0 || out.info.channels > 32) {
        return fail(ErrorCode::FormatUnsupported, "声道数非法: " + std::to_string(out.info.channels));
    }
    if (out.info.sampleRate <= 0 || out.info.sampleRate > 768000) {
        return fail(ErrorCode::FormatUnsupported, "采样率非法: " + std::to_string(out.info.sampleRate));
    }
    const int bytesPerSample = out.info.bitsPerSample / 8;
    if (bytesPerSample <= 0) {
        return fail(ErrorCode::FormatUnsupported, "位深非法: " + std::to_string(out.info.bitsPerSample));
    }
    if (out.info.formatCode != kFormatPcm && out.info.formatCode != kFormatFloat) {
        return fail(ErrorCode::FormatUnsupported,
                    "不支持的编码格式码: " + std::to_string(out.info.formatCode) +
                        "（仅支持 PCM 与 IEEE float）");
    }
    if (out.info.formatCode == kFormatFloat && out.info.bitsPerSample != 32 &&
        out.info.bitsPerSample != 64) {
        return fail(ErrorCode::FormatUnsupported, "浮点编码位深必须为 32 或 64");
    }
    if (out.info.formatCode == kFormatPcm && out.info.bitsPerSample != 8 &&
        out.info.bitsPerSample != 16 && out.info.bitsPerSample != 24 &&
        out.info.bitsPerSample != 32) {
        return fail(ErrorCode::FormatUnsupported,
                    "PCM 位深仅支持 8/16/24/32，当前: " + std::to_string(out.info.bitsPerSample));
    }

    const int frameBytes = bytesPerSample * out.info.channels;
    out.info.dataBytes = static_cast<int64_t>(out.dataSize);
    out.info.frameCount = frameBytes > 0 ? static_cast<int64_t>(out.dataSize) / frameBytes : 0;
    out.info.encodingName = encodingName(out.info.formatCode, out.info.bitsPerSample);
    return out;
}

}  // namespace

Result<WavInfo> probe(const std::string& path) {
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::FileNotFound, "无法打开文件: " + path);
    std::vector<uint8_t> head(64 * 1024);
    in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    const size_t got = static_cast<size_t>(in.gcount());
    head.resize(got);
    Result<ParsedHeader> parsed = parseHeader(head);
    if (!parsed.ok()) return fail(parsed.code(), path + ": " + parsed.message());
    return parsed.value().info;
}

Result<AudioBuffer> readFromMemory(const std::vector<uint8_t>& bytes,
                                   AudioQualityReport* report,
                                   WavInfo* info) {
    Result<ParsedHeader> parsed = parseHeader(bytes);
    if (!parsed.ok()) return fail(parsed.code(), parsed.message());
    const ParsedHeader& h = parsed.value();
    if (info) *info = h.info;

    const int channels = h.info.channels;
    const int bits = h.info.bitsPerSample;
    const int bytesPerSample = bits / 8;
    const size_t frameBytes = static_cast<size_t>(bytesPerSample) * static_cast<size_t>(channels);
    const size_t frames = frameBytes > 0 ? h.dataSize / frameBytes : 0;

    AudioBuffer out;
    out.sampleRate = h.info.sampleRate;
    out.samples.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        out.samples[i] = mixChannels(h.dataPtr + i * frameBytes, channels, bits, h.info.formatCode);
    }

    if (report) *report = analyzeQuality(out, h.info);
    return out;
}

Result<AudioBuffer> read(const std::string& path, AudioQualityReport* report) {
    std::ifstream in = pathutil::openInput(path);
    if (!in) return fail(ErrorCode::FileNotFound, "无法打开文件: " + path);

    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) {
        return fail(ErrorCode::DecodeFailed, "文件为空: " + path);
    }
    if (size > static_cast<std::streamoff>(2) * 1024 * 1024 * 1024) {
        return fail(ErrorCode::DecodeFailed, "文件超过 2 GiB 上限: " + path);
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (in.gcount() != size) {
        return fail(ErrorCode::IoError, "读取文件不完整: " + path);
    }

    Result<AudioBuffer> result = readFromMemory(bytes, report, nullptr);
    if (!result.ok()) return fail(result.code(), path + ": " + result.message());
    return result;
}

std::vector<uint8_t> encodeToMemory(const AudioBuffer& buffer, int bitsPerSample) {
    const bool isFloat = (bitsPerSample == 32);
    const int bytesPerSample = bitsPerSample / 8;
    const uint32_t dataBytes = static_cast<uint32_t>(buffer.samples.size() * bytesPerSample);

    std::vector<uint8_t> out;
    out.reserve(44 + dataBytes);

    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    writeU32(out, 36 + dataBytes);
    out.insert(out.end(), {'W', 'A', 'V', 'E'});

    out.insert(out.end(), {'f', 'm', 't', ' '});
    writeU32(out, 16);
    writeU16(out, static_cast<uint16_t>(isFloat ? kFormatFloat : kFormatPcm));
    writeU16(out, 1);  // 单声道
    writeU32(out, static_cast<uint32_t>(buffer.sampleRate));
    writeU32(out, static_cast<uint32_t>(buffer.sampleRate * bytesPerSample));
    writeU16(out, static_cast<uint16_t>(bytesPerSample));
    writeU16(out, static_cast<uint16_t>(bitsPerSample));

    out.insert(out.end(), {'d', 'a', 't', 'a'});
    writeU32(out, dataBytes);

    for (float sample : buffer.samples) {
        const float clamped = std::max(-1.0f, std::min(1.0f, sample));
        if (isFloat) {
            uint8_t b[4];
            std::memcpy(b, &clamped, 4);
            out.insert(out.end(), b, b + 4);
        } else {
            const int32_t s = static_cast<int32_t>(std::lround(clamped * 32767.0f));
            writeU16(out, static_cast<uint16_t>(static_cast<int16_t>(std::max(-32768, std::min(32767, s)))));
        }
    }
    return out;
}

Result<void> write16(const std::string& path, const AudioBuffer& buffer) {
    return write(path, buffer, 16);
}

Result<void> write(const std::string& path, const AudioBuffer& buffer, int bitsPerSample) {
    if (buffer.sampleRate <= 0) {
        return fail(ErrorCode::InvalidArgument, "采样率必须为正数");
    }
    if (bitsPerSample != 16 && bitsPerSample != 32) {
        return fail(ErrorCode::InvalidArgument, "输出位深仅支持 16 或 32(float)");
    }
    const std::string parent = pathutil::parentPath(path);
    if (!parent.empty() && !pathutil::createDirectories(parent)) {
        return fail(ErrorCode::IoError, "无法创建目录: " + parent);
    }

    const std::vector<uint8_t> bytes = encodeToMemory(buffer, bitsPerSample);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out = pathutil::openOutput(tmp);
        if (!out) return fail(ErrorCode::IoError, "无法写入: " + tmp);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) return fail(ErrorCode::IoError, "写入失败: " + tmp);
    }
    if (!pathutil::rename(tmp, path)) {
        return fail(ErrorCode::IoError, "无法替换目标文件: " + path);
    }
    return okStatus();
}

AudioQualityReport analyzeQuality(const AudioBuffer& buffer, const WavInfo& info) {
    AudioQualityReport r;
    r.durationMs = buffer.durationMs();
    r.sampleRate = buffer.sampleRate;
    r.channels = info.channels > 0 ? info.channels : 1;
    r.bitsPerSample = info.bitsPerSample > 0 ? info.bitsPerSample : 16;
    r.encoding = info.encodingName.empty() ? "PCM16" : info.encodingName;
    r.totalSamples = static_cast<int64_t>(buffer.samples.size());

    if (buffer.samples.empty()) return r;

    double peak = 0.0;
    double sumSq = 0.0;
    double sum = 0.0;
    int64_t clipped = 0;
    for (float s : buffer.samples) {
        const double v = static_cast<double>(s);
        const double a = std::fabs(v);
        if (a > peak) peak = a;
        if (a >= 0.999) ++clipped;
        sumSq += v * v;
        sum += v;
    }
    const double n = static_cast<double>(buffer.samples.size());
    r.peakDbfs = peak > 0 ? 20.0 * std::log10(peak) : -100.0;
    const double rms = std::sqrt(sumSq / n);
    r.rmsDbfs = rms > 0 ? 20.0 * std::log10(rms) : -100.0;
    r.clippingRatio = static_cast<double>(clipped) / n;
    r.dcOffset = sum / n;

    // 20 ms 块 RMS 统计近静音时长
    const int blockSamples = std::max(1, buffer.sampleRate / 50);
    int64_t silentBlocks = 0;
    const size_t totalBlocks = buffer.samples.size() / static_cast<size_t>(blockSamples);
    for (size_t b = 0; b < totalBlocks; ++b) {
        const size_t start = b * static_cast<size_t>(blockSamples);
        double bs = 0.0;
        for (int i = 0; i < blockSamples; ++i) {
            const double v = buffer.samples[start + static_cast<size_t>(i)];
            bs += v * v;
        }
        const double brms = std::sqrt(bs / blockSamples);
        const double db = brms > 0 ? 20.0 * std::log10(brms) : -100.0;
        if (db < -60.0) ++silentBlocks;
    }
    r.nearSilentMs = silentBlocks * blockSamples * 1000 / std::max(1, buffer.sampleRate);
    return r;
}

}  // namespace mm::wav
