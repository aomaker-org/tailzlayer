// Path: labs/tailzlayer/src/hvma.cpp
// Purpose: Implementation of Hedged Virtual Memory Allocator (H-VMA).
// Max Column: 80 Columns (comments/docs) / 120 Chars (code)

#include "../include/hvma.hpp"
#include "../include/telemetry_compressor.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

std::string HardwareMemoryCoordinate::to_hex_string() const {
    std::ostringstream ss;
    ss << "0x" << std::hex << virtual_address;
    return ss.str();
}

std::string HardwareMemoryCoordinate::to_coordinate_string() const {
    std::ostringstream ss;
    ss << "0x" << std::hex << virtual_address << std::dec
       << " [PFN:0x" << std::hex << physical_pfn << std::dec
       << "|Row:0x" << std::hex << dram_row << std::dec
       << "|BG:" << static_cast<int>(dram_bank_group)
       << "|Bank:" << static_cast<int>(dram_bank)
       << "|Ch:" << static_cast<int>(cxl_channel)
       << "|PageOff:" << page_offset_bytes
       << "|CL:" << static_cast<int>(cacheline_index)
       << "|CLOff:" << static_cast<int>(cacheline_offset) << "]";
    return ss.str();
}

HardwareMemoryCoordinate decode_hardware_coordinate(uint64_t address, uint8_t channel_id) {
    HardwareMemoryCoordinate coord;
    coord.virtual_address = address;
    coord.physical_pfn = address >> 12;
    coord.dram_row = static_cast<uint16_t>((address >> 16) & 0xFFFF);
    coord.dram_bank_group = static_cast<uint8_t>((address >> 12) & 0x03);
    coord.dram_bank = static_cast<uint8_t>((address >> 14) & 0x03);
    coord.cxl_channel = channel_id;
    coord.page_offset_bytes = static_cast<uint16_t>(address & 0x0FFF);
    coord.cacheline_index = static_cast<uint8_t>((address & 0x0FFF) / 64);
    coord.cacheline_offset = static_cast<uint8_t>(address & 0x3F);
    return coord;
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <x86intrin.h>
static inline uint64_t read_rdtsc() {
    return __rdtsc();
}
#elif defined(__aarch64__)
static inline uint64_t read_rdtsc() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#else
static inline uint64_t read_rdtsc() {
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}
#endif

HedgedVMA::HedgedVMA(uint32_t overflow_threshold)
    : m_overflow_threshold(overflow_threshold),
      m_hlc_counter(1000),
      m_scrubbed_count(0),
      m_compressor(std::make_unique<TelemetryCompressor>()) {}

HedgedVMA::~HedgedVMA() = default;

uint32_t HedgedVMA::compute_crc32c(const uint8_t* data, size_t len) const {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0x82F63B78 & (-(crc & 1)));
        }
    }
    return ~crc;
}

bool HedgedVMA::allocate_sparse_chunk(const std::string& guid_str, const uint8_t* data, size_t len,
                                       bool force_page_aligned, size_t granularity_multiplier) {
    size_t total_chunk_bytes = HVMA_PAGE_SIZE * granularity_multiplier;
    size_t chunk_limit = total_chunk_bytes - sizeof(HedgedPageHeader);
    size_t actual_payload_bytes = std::min(len, chunk_limit);

    std::vector<ChannelAllocation> allocations;

    // Allocate 2x redundant copies (Channel 0 on Core 0, Channel 1 on Core 2)
    for (int channel_id = 0; channel_id < 2; ++channel_id) {
        std::shared_ptr<SparsePageChunk> page;

        if (force_page_aligned) {
            void* raw_ptr = nullptr;
            if (posix_memalign(&raw_ptr, HVMA_PAGE_SIZE, sizeof(SparsePageChunk)) != 0) {
                std::cerr << "[-] Error: posix_memalign failed for 4KB page alignment.\n";
                return false;
            }
            page = std::shared_ptr<SparsePageChunk>(static_cast<SparsePageChunk*>(raw_ptr), free);
        } else {
            // Guarantee unaligned straddling by forcing a 64-byte offset from physical 4KB page boundary
            void* raw_ptr = nullptr;
            if (posix_memalign(&raw_ptr, HVMA_PAGE_SIZE, sizeof(SparsePageChunk) + 128) != 0) {
                std::cerr << "[-] Error: posix_memalign failed for unaligned allocation.\n";
                return false;
            }
            uint8_t* offset_ptr = static_cast<uint8_t*>(raw_ptr) + 64;
            page = std::shared_ptr<SparsePageChunk>(
                reinterpret_cast<SparsePageChunk*>(offset_ptr),
                [raw_ptr](SparsePageChunk*) { free(raw_ptr); }
            );
        }

        std::memset(page.get(), 0, sizeof(SparsePageChunk));

        // Populate header
        std::memcpy(page->header.chunk_guid, guid_str.c_str(), std::min(guid_str.size(), size_t(16)));
        page->header.monotonic_hlc = ++m_hlc_counter;
        page->header.channel_mask = static_cast<uint16_t>(1 << channel_id);
        page->header.win_count = 0;
        page->header.lose_count = 0;
        page->header.refresh_stalls = 0;
        page->header.boundary_sentinel = PAGE_BOUNDARY_SENTINEL_MAGIC;

        // Calculate virtual memory page alignment & straddle status for full chunk size
        uintptr_t virt_addr = reinterpret_cast<uintptr_t>(page.get());
        uint16_t offset = static_cast<uint16_t>(virt_addr & 0xFFF);

        page->header.virtual_address = static_cast<uint64_t>(virt_addr);
        page->header.page_offset = offset;
        page->header.pages_spanned = static_cast<uint16_t>(((offset + total_chunk_bytes - 1) / HVMA_PAGE_SIZE) + 1);

        // Page is straddled if offset > 0 or if spanned pages exceeds granularity multiplier
        page->header.page_straddled = (offset != 0 || page->header.pages_spanned > granularity_multiplier) ? 1 : 0;

        // Calculate exact byte offsets from block start (virt_addr) to physical 4KB page boundaries
        if (offset == 0) {
            page->header.bytes_to_first_page = 0;
            page->header.bytes_to_second_page = static_cast<uint16_t>(HVMA_PAGE_SIZE);
            page->header.straddle_start_byte = 0;
            page->header.straddle_end_byte = 0;
        } else {
            page->header.bytes_to_first_page = static_cast<uint16_t>(HVMA_PAGE_SIZE - offset);
            page->header.bytes_to_second_page = static_cast<uint16_t>((HVMA_PAGE_SIZE - offset) + HVMA_PAGE_SIZE);

            size_t dist_to_boundary = HVMA_PAGE_SIZE - offset;
            if (dist_to_boundary > sizeof(HedgedPageHeader)) {
                page->header.straddle_start_byte = static_cast<uint16_t>(dist_to_boundary - sizeof(HedgedPageHeader));
            } else {
                page->header.straddle_start_byte = 0;
            }
            page->header.straddle_end_byte = static_cast<uint16_t>(std::min(
                static_cast<size_t>(page->header.straddle_start_byte + HVMA_PAGE_SIZE),
                chunk_limit
            ));
        }

        // Populate payload & compute CRC32c
        std::memcpy(page->payload, data, actual_payload_bytes);

        // Stamp physical page boundary sentinel marker tag into payload if offset falls within payload area
        if (page->header.bytes_to_first_page >= sizeof(HedgedPageHeader)) {
            size_t marker_idx = page->header.bytes_to_first_page - sizeof(HedgedPageHeader);
            if (marker_idx + 4 <= sizeof(page->payload)) {
                uint32_t magic = PAGE_BOUNDARY_SENTINEL_MAGIC;
                std::memcpy(&page->payload[marker_idx], &magic, sizeof(magic));
            }
        }

        page->header.checksum_crc32c = compute_crc32c(page->payload, actual_payload_bytes);

        ChannelAllocation alloc;
        alloc.channel_id = channel_id;
        alloc.core_affinity = (channel_id == 0) ? 0 : 2;
        alloc.page_aligned = force_page_aligned;
        alloc.chunk_bytes = total_chunk_bytes;
        alloc.page_data = page;
        alloc.win_capture_count = 0;
        alloc.loss_capture_count = 0;

        allocations.push_back(alloc);

        // Record Page 0 initial vs discovered physical alignment mapping in KV registry
        if (channel_id == 0) {
            discover_page_alignment(guid_str, page->header.virtual_address);
        }
    }

    m_chunk_registry[guid_str] = allocations;
    return true;
}

PageAlignmentRecord HedgedVMA::discover_page_alignment(const std::string& guid_str, uint64_t raw_address) {
    PageAlignmentRecord rec;
    rec.initial_alloc = decode_hardware_coordinate(raw_address, 0);
    rec.offset_bytes = rec.initial_alloc.page_offset_bytes;

    if (rec.offset_bytes == 0) {
        rec.discovered_page = rec.initial_alloc;
        rec.is_straddled = 0;
    } else {
        // Compute first physical 4KB boundary within the 2x block capacity
        uint64_t aligned_addr = (raw_address + (HVMA_PAGE_SIZE - 1)) & ~(static_cast<uint64_t>(HVMA_PAGE_SIZE - 1));
        rec.discovered_page = decode_hardware_coordinate(aligned_addr, 0);
        rec.is_straddled = 1;
    }

    m_page_alignment_kv[guid_str] = rec;
    return rec;
}

bool HedgedVMA::get_page_alignment_record(const std::string& guid_str, PageAlignmentRecord& record) const {
    auto it = m_page_alignment_kv.find(guid_str);
    if (it != m_page_alignment_kv.end()) {
        record = it->second;
        return true;
    }
    return false;
}

void HedgedVMA::check_ftw_overflow(const std::string& guid_str, ChannelAllocation& ch_alloc) {
    if (ch_alloc.page_data->header.win_count >= m_overflow_threshold) {
        std::cout << "🚨 [FTW-overflow] GUID " << guid_str << " (Ch " << ch_alloc.channel_id
                  << ") reached " << m_overflow_threshold << " wins. Resetting counters.\n";
        ch_alloc.page_data->header.win_count = 0;
        ch_alloc.page_data->header.lose_count = 0;
    }
}

void HedgedVMA::check_and_flush_capture_watermark(const std::string& guid_str, ChannelAllocation& ch_alloc) {
    if (ch_alloc.win_capture_count >= RDTSC_WATERMARK_THRESHOLD ||
        ch_alloc.loss_capture_count >= RDTSC_WATERMARK_THRESHOLD) {
        
        std::vector<RDTSCCapture> batch;
        size_t win_n = std::min(ch_alloc.win_capture_count, RDTSC_CAPTURE_RING_SIZE);
        for (size_t i = 0; i < win_n; ++i) {
            batch.push_back(ch_alloc.win_captures[i]);
        }

        size_t loss_n = std::min(ch_alloc.loss_capture_count, RDTSC_CAPTURE_RING_SIZE);
        for (size_t i = 0; i < loss_n; ++i) {
            batch.push_back(ch_alloc.loss_captures[i]);
        }

        m_compressor->archive_capture_batch(
            guid_str,
            ch_alloc.channel_id,
            ch_alloc.page_data->header.page_straddled,
            ch_alloc.page_data->header.pages_spanned,
            batch
        );

        ch_alloc.win_capture_count = 0;
        ch_alloc.loss_capture_count = 0;
    }
}

bool HedgedVMA::read_chunk_hedged(const std::string& guid_str, uint8_t* out_buffer, size_t max_len,
                                 int& winning_channel) {
    auto it = m_chunk_registry.find(guid_str);
    if (it == m_chunk_registry.end() || it->second.size() < 2) {
        return false;
    }

    auto& ch0 = it->second[0];
    auto& ch1 = it->second[1];

    uint64_t rdtsc_start = read_rdtsc();

    // Thread-safe random stall generator simulating DRAM tRFC refresh spikes
    static thread_local std::mt19937 rng(1337);
    std::uniform_int_distribution<int> dist(1, 100);

    // Calculate refresh stall probability based on physical 4KB pages spanned (5% per physical page hit)
    int ch0_spanned = ch0.page_data->header.pages_spanned;
    int ch1_spanned = ch1.page_data->header.pages_spanned;

    bool ch0_stalled = false;
    for (int p = 0; p < ch0_spanned; ++p) {
        if (dist(rng) <= 5) ch0_stalled = true;
    }

    bool ch1_stalled = false;
    for (int p = 0; p < ch1_spanned; ++p) {
        if (dist(rng) <= 5) ch1_stalled = true;
    }

    if (ch0_stalled) ch0.page_data->header.refresh_stalls++;
    if (ch1_stalled) ch1.page_data->header.refresh_stalls++;

    int ch0_stall_ns = ch0_stalled ? 150000 : 300; // 150us stall vs 300ns normal
    int ch1_stall_ns = ch1_stalled ? 150000 : 300;

    double res0_lat = ch0_stall_ns / 1000.0; // us
    double res1_lat = ch1_stall_ns / 1000.0; // us

    // Determine winner based on lowest latency, using fair tie-breaker on equal latencies
    bool ch0_wins = false;
    if (res0_lat < res1_lat) {
        ch0_wins = true;
    } else if (res1_lat < res0_lat) {
        ch0_wins = false;
    } else {
        // Equal latency tie: alternate via round-robin flag to prevent bias
        static thread_local bool tie_flip = false;
        tie_flip = !tie_flip;
        ch0_wins = tie_flip;
    }

    uint64_t rdtsc_end = read_rdtsc();
    uint32_t ch0_cycles = static_cast<uint32_t>((ch0_stall_ns * 3) + (rdtsc_end - rdtsc_start));
    uint32_t ch1_cycles = static_cast<uint32_t>((ch1_stall_ns * 3) + (rdtsc_end - rdtsc_start));

    if (ch0_wins) {
        winning_channel = 0;
        ch0.page_data->header.win_count++;
        ch1.page_data->header.lose_count++;

        size_t w_idx = ch0.win_capture_count % RDTSC_CAPTURE_RING_SIZE;
        ch0.win_captures[w_idx] = {rdtsc_start, rdtsc_end, ch0_cycles, 1, static_cast<uint8_t>(ch0_stalled ? 1 : 0)};
        ch0.win_capture_count++;

        size_t l_idx = ch1.loss_capture_count % RDTSC_CAPTURE_RING_SIZE;
        ch1.loss_captures[l_idx] = {rdtsc_start, rdtsc_end, ch1_cycles, 0, static_cast<uint8_t>(ch1_stalled ? 1 : 0)};
        ch1.loss_capture_count++;

        std::memcpy(out_buffer, ch0.page_data->payload, std::min(max_len, sizeof(SparsePageChunk::payload)));
        check_ftw_overflow(guid_str, ch0);
    } else {
        winning_channel = 1;
        ch1.page_data->header.win_count++;
        ch0.page_data->header.lose_count++;

        size_t w_idx = ch1.win_capture_count % RDTSC_CAPTURE_RING_SIZE;
        ch1.win_captures[w_idx] = {rdtsc_start, rdtsc_end, ch1_cycles, 1, static_cast<uint8_t>(ch1_stalled ? 1 : 0)};
        ch1.win_capture_count++;

        size_t l_idx = ch0.loss_capture_count % RDTSC_CAPTURE_RING_SIZE;
        ch0.loss_captures[l_idx] = {rdtsc_start, rdtsc_end, ch0_cycles, 0, static_cast<uint8_t>(ch0_stalled ? 1 : 0)};
        ch0.loss_capture_count++;

        std::memcpy(out_buffer, ch1.page_data->payload, std::min(max_len, sizeof(SparsePageChunk::payload)));
        check_ftw_overflow(guid_str, ch1);
    }

    check_and_flush_capture_watermark(guid_str, ch0);
    check_and_flush_capture_watermark(guid_str, ch1);

    return true;
}

bool HedgedVMA::transparent_reallocate_chunk(const std::string& guid_str) {
    auto it = m_chunk_registry.find(guid_str);
    if (it == m_chunk_registry.end()) return false;

    bool reallocated = false;
    for (auto& alloc : it->second) {
        if (!alloc.page_aligned || alloc.page_data->header.page_straddled == 1) {
            uint64_t old_virt = alloc.page_data->header.virtual_address;

            void* raw_ptr = nullptr;
            if (posix_memalign(&raw_ptr, HVMA_PAGE_SIZE, sizeof(SparsePageChunk)) != 0) {
                std::cerr << "[-] Error: posix_memalign failed during transparent reallocation.\n";
                continue;
            }

            auto new_page = std::shared_ptr<SparsePageChunk>(static_cast<SparsePageChunk*>(raw_ptr), free);
            std::memset(new_page.get(), 0, sizeof(SparsePageChunk));

            new_page->header = alloc.page_data->header;
            std::memcpy(new_page->payload, alloc.page_data->payload, sizeof(SparsePageChunk::payload));

            uintptr_t new_virt = reinterpret_cast<uintptr_t>(new_page.get());
            new_page->header.virtual_address = static_cast<uint64_t>(new_virt);
            new_page->header.page_offset = 0;
            new_page->header.page_straddled = 0;
            new_page->header.pages_spanned = static_cast<uint16_t>(alloc.chunk_bytes / HVMA_PAGE_SIZE);
            new_page->header.straddle_start_byte = 0;
            new_page->header.straddle_end_byte = 0;
            new_page->header.bytes_to_first_page = 0;
            new_page->header.bytes_to_second_page = static_cast<uint16_t>(HVMA_PAGE_SIZE);
            new_page->header.boundary_sentinel = PAGE_BOUNDARY_SENTINEL_MAGIC;

            alloc.page_data = new_page;
            alloc.page_aligned = true;
            m_scrubbed_count++;
            reallocated = true;

            // Update Page 0 KV registry record for reallocated block
            m_page_alignment_kv[guid_str] = {
                decode_hardware_coordinate(old_virt, alloc.channel_id),
                decode_hardware_coordinate(static_cast<uint64_t>(new_virt), alloc.channel_id),
                0,                                          // offset_bytes is now 0
                0                                           // straddled flag cleared
            };

            std::cout << "🔄 [Transparent Reallocation] GUID " << guid_str << " (Ch " << alloc.channel_id
                      << "): Migrated 0x" << std::hex << old_virt << " -> 0x" << new_virt << std::dec
                      << " (Old straddled page safely freed & unaligned reference updated)\n";
        }
    }
    return reallocated;
}

size_t HedgedVMA::scrub_and_realign_straddled_chunks() {
    size_t scrubbed_this_pass = 0;

    for (auto& [guid, allocs] : m_chunk_registry) {
        for (auto& alloc : allocs) {
            if (!alloc.page_aligned || alloc.page_data->header.page_straddled == 1) {
                uint64_t old_virt = alloc.page_data->header.virtual_address;
                uint16_t b_first = alloc.page_data->header.bytes_to_first_page;

                // Allocate strictly page-aligned block
                void* raw_ptr = nullptr;
                if (posix_memalign(&raw_ptr, HVMA_PAGE_SIZE, sizeof(SparsePageChunk)) != 0) {
                    std::cerr << "[-] Error: posix_memalign failed during scrubbing.\n";
                    continue;
                }

                auto new_page = std::shared_ptr<SparsePageChunk>(static_cast<SparsePageChunk*>(raw_ptr), free);
                std::memset(new_page.get(), 0, sizeof(SparsePageChunk));

                // Copy header & payload to new aligned memory
                new_page->header = alloc.page_data->header;
                std::memcpy(new_page->payload, alloc.page_data->payload, sizeof(SparsePageChunk::payload));

                // Update alignment fields
                uintptr_t new_virt = reinterpret_cast<uintptr_t>(new_page.get());
                new_page->header.virtual_address = static_cast<uint64_t>(new_virt);
                new_page->header.page_offset = 0;
                new_page->header.page_straddled = 0;
                new_page->header.pages_spanned = static_cast<uint16_t>(alloc.chunk_bytes / HVMA_PAGE_SIZE);
                new_page->header.straddle_start_byte = 0;
                new_page->header.straddle_end_byte = 0;
                new_page->header.bytes_to_first_page = 0;
                new_page->header.bytes_to_second_page = static_cast<uint16_t>(HVMA_PAGE_SIZE);
                new_page->header.boundary_sentinel = PAGE_BOUNDARY_SENTINEL_MAGIC;

                // Replace allocation pointer and release old unaligned chunk
                alloc.page_data = new_page;
                alloc.page_aligned = true;
                scrubbed_this_pass++;
                m_scrubbed_count++;

                std::cout << "🧹 [Scrub-Realign] GUID " << guid << " (Ch " << alloc.channel_id
                          << "): Re-aligned 0x" << std::hex << old_virt << " -> 0x" << new_virt << std::dec
                          << " (1st Page Boundary marker was at +" << b_first << " B into block; scrubbed & freed)\n";
            }
        }
    }

    return scrubbed_this_pass;
}

void HedgedVMA::print_telemetry_report() const {
    std::cout << "\n====================================================================================================================\n";
    std::cout << "📊 H-VMA CXL PAGE STRIPING, PHYSICAL BOUNDARY MARKERS & REFRESH STALL TELEMETRY REPORT\n";
    std::cout << "====================================================================================================================\n";
    std::cout << "GUID          | Ch | Aligned | Virt Addr          | Offset | Straddled | 1st Page B | 2nd Page B | Sentinel Tag | Pages | Stalls\n";
    std::cout << "--------------------------------------------------------------------------------------------------------------------\n";
    for (const auto& [guid, allocs] : m_chunk_registry) {
        for (const auto& alloc : allocs) {
            const auto& h = alloc.page_data->header;
            printf("%-13s | %-2d | %-7s | 0x%012lx | %-6u | %-9s | +%-9u | +%-9u | 0x%08x     | %-5u | %-6u\n",
                   guid.c_str(),
                   alloc.channel_id,
                   alloc.page_aligned ? "YES" : "NO",
                   h.virtual_address,
                   h.page_offset,
                   h.page_straddled ? "STRADDLED" : "EXACT",
                   h.bytes_to_first_page,
                   h.bytes_to_second_page,
                   h.boundary_sentinel,
                   h.pages_spanned,
                   h.refresh_stalls);
        }
    }
    std::cout << "====================================================================================================================\n";
    std::cout << "   Total Memory Blocks Scrubbed & Re-aligned: " << m_scrubbed_count << "\n";

    std::cout << "\n====================================================================================================================\n";
    std::cout << "🗺️ PAGE 0 ALIGNMENT REGISTRY & DISCOVERED HARDWARE COORDINATE MAP\n";
    std::cout << "====================================================================================================================\n";
    std::cout << "GUID          | Allocated Raw Hex Addr | Discovered Physical Page Boundary | Hardware Coordinate [PFN|Row|BG|Bank|Ch|PageOff|CL|CLOff]\n";
    std::cout << "--------------------------------------------------------------------------------------------------------------------\n";
    for (const auto& [guid, rec] : m_page_alignment_kv) {
        printf("%-13s | %-22s | %-33s | %s\n",
               guid.c_str(),
               rec.initial_alloc.to_hex_string().c_str(),
               rec.discovered_page.to_hex_string().c_str(),
               rec.discovered_page.to_coordinate_string().c_str());
    }
    std::cout << "====================================================================================================================\n";

    m_compressor->print_archival_summary();
}

// end of file: labs/tailzlayer/src/hvma.cpp
