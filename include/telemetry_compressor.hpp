// Path: labs/tailzlayer/include/telemetry_compressor.hpp
// Purpose: LZW telemetry compression and ML feature dataset extractor for H-VMA.
// Max Column: 80 Columns (comments/docs) / 120 Chars (code)

#ifndef TELEMETRY_COMPRESSOR_HPP
#define TELEMETRY_COMPRESSOR_HPP

#include "hvma.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct CompressionStats {
    size_t raw_bytes_captured = 0;
    size_t compressed_bytes_written = 0;
    uint32_t batch_count = 0;
    double overall_ratio = 0.0;
};

struct MLFeatureVector {
    std::string guid;
    int channel_id;
    uint8_t page_straddled;
    uint16_t pages_spanned;
    double mean_win_cycles;
    double mean_loss_cycles;
    double stall_frequency;
    uint32_t p50_cycles;
    uint32_t p90_cycles;
    uint32_t p99_cycles;
};

class TelemetryCompressor {
public:
    TelemetryCompressor() = default;
    ~TelemetryCompressor() = default;

    // LZW compression on RDTSC capture sample batch
    std::vector<uint8_t> compress_lzw(const std::vector<uint8_t>& uncompressed_data);

    // Delta-encodes and bitpacks RDTSCCapture samples into a byte stream
    std::vector<uint8_t> serialize_and_delta_encode(const std::vector<RDTSCCapture>& captures);

    // Flushes and archives a full/watermarked capture batch with LZW compression
    void archive_capture_batch(const std::string& guid, int channel_id, uint8_t straddled,
                               uint16_t pages_spanned, const std::vector<RDTSCCapture>& captures);

    // Extracts ML feature vector digest for model training
    MLFeatureVector extract_ml_features(const std::string& guid, int channel_id, uint8_t straddled,
                                         uint16_t pages_spanned, const std::vector<RDTSCCapture>& captures);

    // Returns overall compression and archival statistics
    CompressionStats get_stats() const;

    // Prints archival summary report
    void print_archival_summary() const;

private:
    mutable std::mutex m_mutex;
    CompressionStats m_stats;
    std::vector<MLFeatureVector> m_ml_dataset;
};

#endif // TELEMETRY_COMPRESSOR_HPP

// end of file: labs/tailzlayer/include/telemetry_compressor.hpp
