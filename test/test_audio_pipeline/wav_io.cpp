#include "wav_io.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace nocturnation {
namespace test_audio {

namespace {

constexpr uint16_t kWaveFormatPCM = 1;

// Read a fixed-width little-endian unsigned integer.
template <typename T>
bool read_le(std::ifstream& f, T& out) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&out), sizeof(T)));
}

template <typename T>
void write_le(std::ofstream& f, T v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

}  // namespace

Sample load_wav(const std::string& path) {
    Sample empty;  // sample_rate_hz=0, samples empty -> the failure value

    std::ifstream f(path, std::ios::binary);
    if (!f) return empty;

    // RIFF chunk: 'RIFF' <size> 'WAVE'
    char     riff[4];
    uint32_t riff_size = 0;
    char     wave[4];
    if (!f.read(riff, 4) || !read_le(f, riff_size) || !f.read(wave, 4)) return empty;
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return empty;
    }

    // Walk sub-chunks looking for 'fmt ' then 'data'. Anything else is
    // skipped via seekg.
    uint16_t format          = 0;
    uint16_t channels        = 0;
    uint32_t sample_rate     = 0;
    uint16_t bits_per_sample = 0;
    std::vector<int16_t> samples;

    char     chunk_id[4];
    uint32_t chunk_size = 0;
    while (f.read(chunk_id, 4) && read_le(f, chunk_size)) {
        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) return empty;
            uint32_t byte_rate;
            uint16_t block_align;
            if (!read_le(f, format) || !read_le(f, channels) ||
                !read_le(f, sample_rate) || !read_le(f, byte_rate) ||
                !read_le(f, block_align) || !read_le(f, bits_per_sample)) {
                return empty;
            }
            // Skip any extension bytes (cbSize, etc.) past the standard 16.
            if (chunk_size > 16) f.seekg(chunk_size - 16, std::ios::cur);
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            // Format check happens here so we can read samples in the
            // same pass. Refuse anything that's not what the project uses.
            if (format != kWaveFormatPCM || channels != 1 || bits_per_sample != 16) {
                return empty;
            }
            const size_t n = chunk_size / 2;
            samples.resize(n);
            if (!f.read(reinterpret_cast<char*>(samples.data()), chunk_size)) {
                return empty;
            }
            break;
        } else {
            // Skip unknown chunk. WAV chunks are padded to even byte
            // boundaries; round up if needed.
            const uint32_t padded = (chunk_size + 1) & ~1u;
            f.seekg(padded, std::ios::cur);
        }
    }

    if (samples.empty() || sample_rate == 0) return empty;

    Sample s;
    s.samples        = std::move(samples);
    s.sample_rate_hz = sample_rate;
    return s;
}

bool write_wav(const std::string& path, const Sample& s) {
    if (s.sample_rate_hz == 0) return false;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    const uint32_t data_bytes  = static_cast<uint32_t>(s.samples.size()) * 2u;
    const uint32_t fmt_size    = 16;
    const uint32_t riff_size   = 4 + (8 + fmt_size) + (8 + data_bytes);
    const uint16_t channels    = 1;
    const uint16_t bits        = 16;
    const uint16_t block_align = (bits / 8) * channels;
    const uint32_t byte_rate   = s.sample_rate_hz * block_align;

    f.write("RIFF", 4);          write_le<uint32_t>(f, riff_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);          write_le<uint32_t>(f, fmt_size);
    write_le<uint16_t>(f, kWaveFormatPCM);
    write_le<uint16_t>(f, channels);
    write_le<uint32_t>(f, s.sample_rate_hz);
    write_le<uint32_t>(f, byte_rate);
    write_le<uint16_t>(f, block_align);
    write_le<uint16_t>(f, bits);
    f.write("data", 4);          write_le<uint32_t>(f, data_bytes);
    f.write(reinterpret_cast<const char*>(s.samples.data()), data_bytes);

    return f.good();
}

}  // namespace test_audio
}  // namespace nocturnation
