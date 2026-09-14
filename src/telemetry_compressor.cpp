// Path: labs/tailzlayer/src/telemetry_compressor.cpp
// Purpose: LZW compression engine and ML feature vector extractor.
// Max Column: 80 Columns (comments/docs) / 120 Chars (code)

#include "../include/telemetry_compressor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>

std::vector<uint8_t> TelemetryCompressor::serialize_and_delta_encode(const std::vector<RDTSCCapture>& captures) {
    std::vector<uint8_t> buffer;
    buffer.reserve(captures.size() * 16);

    uint64_t prev_start = 0;
    for (const auto& cap : captures) {
        uint64_t delta_start = (prev_start == 0) ? cap.rdtsc_start : (cap.rdtsc_start - prev_start);
        prev_start = cap.rdtsc_start;

        // Push delta_start (8 bytes)
        for (int i = 0; i < 8; ++i) {
            buffer.push_back(static_cast<uint8_t>((delta_start >> (i * 8)) & 0xFF));
        }

        // Push elapsed_cycles (4 bytes)
        for (int i = 0; i < 4; ++i) {
            buffer.push_back(static_cast<uint8_t>((cap.elapsed_cycles >> (i * 8)) & 0xFF));
        }

        // Push bitpacked flags (1 byte: bit 0 = is_win, bit 1 = was_stalled)
        uint8_t flags = (cap.is_win & 0x01) | ((cap.was_stalled & 0x01) << 1);
        buffer.push_back(flags);
    }

    return buffer;
}

std::vector<uint8_t> TelemetryCompressor::compress_lzw(const std::vector<uint8_t>& uncompressed_data) {
    if (uncompressed_data.empty()) return {};

    std::map<std::vector<uint8_t>, uint16_t> dictionary;
    for (int i = 0; i < 256; ++i) {
        dictionary[{static_cast<uint8_t>(i)}] = static_cast<uint16_t>(i);
    }

    uint16_t next_code = 256;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> current_pattern;

    for (uint8_t byte : uncompressed_data) {
        std::vector<uint8_t> combined = current_pattern;
        combined.push_back(byte);

        if (dictionary.count(combined)) {
            current_pattern = combined;
        } else {
            uint16_t code = dictionary[current_pattern];
            compressed.push_back(static_cast<uint8_t>(code & 0xFF));
            compressed.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));

            if (next_code < 4095) {
                dictionary[combined] = next_code++;
            }
            current_pattern = {byte};
        }
    }

    if (!current_pattern.empty()) {
        uint16_t code = dictionary[current_pattern];
        compressed.push_back(static_cast<uint8_t>(code & 0xFF));
        compressed.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    }

    return compressed;
}

MLFeatureVector TelemetryCompressor::extract_ml_features(const std::string& guid, int channel_id, uint8_t straddled,
                                                        uint16_t pages_spanned,
                                                        const std::vector<RDTSCCapture>& captures) {
    MLFeatureVector fv;
    fv.guid = guid;
    fv.channel_id = channel_id;
    fv.page_straddled = straddled;
    fv.pages_spanned = pages_spanned;

    if (captures.empty()) return fv;

    uint64_t win_sum = 0, win_cnt = 0;
    uint64_t loss_sum = 0, loss_cnt = 0;
    uint32_t stall_cnt = 0;
    std::vector<uint32_t> cycles;

    for (const auto& cap : captures) {
        cycles.push_back(cap.elapsed_cycles);
        if (cap.was_stalled) stall_cnt++;
        if (cap.is_win) {
            win_sum += cap.elapsed_cycles;
            win_cnt++;
        } else {
            loss_sum += cap.elapsed_cycles;
            loss_cnt++;
        }
    }

    std::sort(cycles.begin(), cycles.end());

    fv.mean_win_cycles = (win_cnt > 0) ? (double)win_sum / win_cnt : 0.0;
    fv.mean_loss_cycles = (loss_cnt > 0) ? (double)loss_sum / loss_cnt : 0.0;
    fv.stall_frequency = (double)stall_cnt / captures.size();
    fv.p50_cycles = cycles[cycles.size() * 50 / 100];
    fv.p90_cycles = cycles[cycles.size() * 90 / 100];
    fv.p99_cycles = cycles[cycles.size() * 99 / 100];

    return fv;
}

void TelemetryCompressor::archive_capture_batch(const std::string& guid, int channel_id, uint8_t straddled,
                                                 uint16_t pages_spanned, const std::vector<RDTSCCapture>& captures) {
    if (captures.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<uint8_t> raw_bytes = serialize_and_delta_encode(captures);
    std::vector<uint8_t> compressed = compress_lzw(raw_bytes);

    m_stats.raw_bytes_captured += raw_bytes.size();
    m_stats.compressed_bytes_written += compressed.size();
    m_stats.batch_count++;
    if (m_stats.raw_bytes_captured > 0) {
        m_stats.overall_ratio = (1.0 - (double)m_stats.compressed_bytes_written / m_stats.raw_bytes_captured) * 100.0;
    }

    MLFeatureVector fv = extract_ml_features(guid, channel_id, straddled, pages_spanned, captures);
    m_ml_dataset.push_back(fv);
}

CompressionStats TelemetryCompressor::get_stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void TelemetryCompressor::print_archival_summary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << "\n========================================================================================\n";
    std::cout << "📦 H-VMA AUTOMATED LZW COMPRESSION & ML MODEL FEATURE DIGEST SUMMARY\n";
    std::cout << "========================================================================================\n";
    printf("   Archived Batches       : %u batches\n", m_stats.batch_count);
    printf("   Raw Captured Bytes     : %zu bytes\n", m_stats.raw_bytes_captured);
    printf("   Compressed Bytes       : %zu bytes\n", m_stats.compressed_bytes_written);
    printf("   LZW Space Savings      : %.2f%%\n", m_stats.overall_ratio);
    printf("   Extracted ML Vectors   : %zu feature samples\n", m_ml_dataset.size());
    std::cout << "----------------------------------------------------------------------------------------\n";
    std::cout << "GUID          | Ch | Straddled | Pages | Mean Win Cyc | Mean Loss Cyc | Stall Freq | P50    | P90    | P99\n";
    std::cout << "----------------------------------------------------------------------------------------\n";

    size_t display_count = std::min(m_ml_dataset.size(), size_t(8));
    for (size_t i = 0; i < display_count; ++i) {
        const auto& fv = m_ml_dataset[i];
        printf("%-13s | %-2d | %-9s | %-5u | %-12.1f | %-13.1f | %-10.2f | %-6u | %-6u | %-6u\n",
               fv.guid.c_str(),
               fv.channel_id,
               fv.page_straddled ? "STRADDLED" : "EXACT",
               fv.pages_spanned,
               fv.mean_win_cycles,
               fv.mean_loss_cycles,
               fv.stall_frequency,
               fv.p50_cycles,
               fv.p90_cycles,
               fv.p99_cycles);
    }
    std::cout << "========================================================================================\n";
}

// end of file: labs/tailzlayer/src/telemetry_compressor.cpp
