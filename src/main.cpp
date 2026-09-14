// Path: labs/tailzlayer/src/main.cpp
// Purpose: Benchmark harness for experimental H-VMA CXL page straddle scrubbing & re-alignment.
// Max Column: 80 Columns (comments/docs) / 120 Chars (code)

#include "../include/hvma.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <ctime>
#include <iomanip>
#include <unistd.h>

int main() {
    std::time_t now = std::time(nullptr);
    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));

    std::cout << "========================================================================================" << std::endl;
    std::cout << "🚀 TAILZLAYER: CXL H-VMA PAGE STRADDLE SCRUBBING & HARDWARE COORDINATE EVALUATION HARNESS" << std::endl;
    std::cout << "========================================================================================" << std::endl;
    std::cout << "   Executable       : hvma_benchmark (v2.4.0-cxl)" << std::endl;
    std::cout << "   Process PID      : " << getpid() << std::endl;
    std::cout << "   Timestamp        : " << time_str << std::endl;
    std::cout << "   Allocator Mode   : Dual-Channel Hedged Race Read (Ch0 vs Ch1)" << std::endl;
    std::cout << "   Granularities    : 1x (4KB Page) vs 2x (8KB Dual-Channel Block)" << std::endl;
    std::cout << "   Hardware Mapping : Coordinate Schema [PFN | Row | BG | Bank | Ch | PageOff | CL | CLOff]" << std::endl;
    std::cout << "   Page Scrubbing   : Dynamic De-straddling & Zero-Downtime Pointer Re-alignment" << std::endl;
    std::cout << "========================================================================================" << std::endl;

    HedgedVMA vma(1000);

    const char* payload_alpha = "AOMAKER-CXL-PAYLOAD-ALPHA-4K-ALIGNED";
    const char* payload_beta  = "AOMAKER-CXL-PAYLOAD-BETA-4K-UNALIGNED";
    const char* payload_gamma = "AOMAKER-CXL-PAYLOAD-GAMMA-8K-ALIGNED";
    const char* payload_delta = "AOMAKER-CXL-PAYLOAD-DELTA-8K-UNALIGNED";

    // Populate allocations
    vma.allocate_sparse_chunk("chunk-alpha", reinterpret_cast<const uint8_t*>(payload_alpha),
                               std::strlen(payload_alpha) + 1, true, 1);
    vma.allocate_sparse_chunk("chunk-beta", reinterpret_cast<const uint8_t*>(payload_beta),
                               std::strlen(payload_beta) + 1, false, 1);
    vma.allocate_sparse_chunk("chunk-gamma", reinterpret_cast<const uint8_t*>(payload_gamma),
                               std::strlen(payload_gamma) + 1, true, 2);
    vma.allocate_sparse_chunk("chunk-delta", reinterpret_cast<const uint8_t*>(payload_delta),
                               std::strlen(payload_delta) + 1, false, 2);

    std::cout << "\n[ Phase 1 ] Populated 4 sparse page chunks. Executing 2,500 hedged read races..." << std::endl;

    std::vector<std::string> guids = {"chunk-alpha", "chunk-beta", "chunk-gamma", "chunk-delta"};
    uint8_t read_buf[8192];
    int ch0_wins = 0, ch1_wins = 0;

    for (int iter = 0; iter < 2500; ++iter) {
        std::string target_guid = guids[iter % guids.size()];
        int winning_channel = -1;
        if (vma.read_chunk_hedged(target_guid, read_buf, sizeof(read_buf), winning_channel)) {
            if (winning_channel == 0) ch0_wins++;
            else if (winning_channel == 1) ch1_wins++;
        }
    }

    std::cout << "\n[ Phase 1 Telemetry Report Before Scrubbing ]" << std::endl;
    vma.print_telemetry_report();

    std::cout << "\n========================================================================================" << std::endl;
    std::cout << "🧹 EXECUTING DYNAMIC PAGE SCRUBBING & RE-ALIGNMENT DEFENSE" << std::endl;
    std::cout << "========================================================================================" << std::endl;

    size_t scrubbed = vma.scrub_and_realign_straddled_chunks();
    std::cout << "[+] Successfully scrubbed and re-aligned " << scrubbed << " unaligned memory allocations." << std::endl;

    std::cout << "\n[ Phase 2 ] Executing 2,500 hedged read races post-scrubbing..." << std::endl;

    for (int iter = 0; iter < 2500; ++iter) {
        std::string target_guid = guids[iter % guids.size()];
        int winning_channel = -1;
        if (vma.read_chunk_hedged(target_guid, read_buf, sizeof(read_buf), winning_channel)) {
            if (winning_channel == 0) ch0_wins++;
            else if (winning_channel == 1) ch1_wins++;
        }
    }

    std::cout << "\n[ Phase 2 Final Telemetry Report Post-Scrubbing ]" << std::endl;
    vma.print_telemetry_report();

    return 0;
}

// end of file: labs/tailzlayer/src/main.cpp
