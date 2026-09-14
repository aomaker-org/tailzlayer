// Path: labs/tailzlayer/include/hvma.hpp
// Purpose: Hedged Virtual Memory Allocator (H-VMA) with RDTSC capture and scrubbing.
// Max Column: 80 Columns (comments/docs) / 120 Chars (code)

#ifndef HVMA_HPP
#define HVMA_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

constexpr size_t HVMA_PAGE_SIZE = 4096; // Standard 4KB page granule
constexpr uint32_t OVERFLOW_THRESHOLD_DEFAULT = 1000; // FTW threshold limit
constexpr size_t RDTSC_CAPTURE_RING_SIZE = 64; // Ring buffer size per channel
constexpr size_t RDTSC_WATERMARK_THRESHOLD = 50; // Archiving watermark trigger
constexpr uint32_t PAGE_BOUNDARY_SENTINEL_MAGIC = 0x50414745; // 'PAGE' magic sentinel

#pragma pack(push, 1)
struct HedgedPageHeader {
    uint8_t  chunk_guid[16];      // 128-bit UUID/GUID identifying memory block
    uint64_t monotonic_hlc;       // Monotonic Hybrid Logical Clock sequence timestamp
    uint16_t channel_mask;        // Bitmask of active physical memory channels
    uint32_t win_count;           // Incremented when this channel wins read race
    uint32_t lose_count;          // Incremented when this channel loses read race
    uint32_t checksum_crc32c;     // Payload integrity hash
    uint64_t virtual_address;     // Virtual base memory address of chunk
    uint16_t page_offset;         // Offset relative to 4KB page boundary (0 = page aligned)
    uint8_t  page_straddled;      // 1 if chunk straddles across 4KB page boundary, 0 if aligned
    uint16_t pages_spanned;       // Number of physical 4KB pages spanned by payload
    uint32_t refresh_stalls;      // Count of DRAM tRFC refresh stall penalties incurred
    uint16_t straddle_start_byte; // Payload byte index where physical straddle begins
    uint16_t straddle_end_byte;   // Payload byte index where physical straddle ends
    uint16_t bytes_to_first_page; // Bytes from block start (virt_addr) to 1st physical page
    uint16_t bytes_to_second_page;// Bytes from block start (virt_addr) to 2nd physical page
    uint32_t boundary_sentinel;   // Magic sentinel marker tag ('PAGE' 0x50414745)
};
#pragma pack(pop)

struct SparsePageChunk {
    HedgedPageHeader header;
    uint8_t payload[HVMA_PAGE_SIZE * 2 - sizeof(HedgedPageHeader)]; // Supports up to 2x (8KB) granularity
};

struct RDTSCCapture {
    uint64_t rdtsc_start = 0;   // Cycle counter at race start
    uint64_t rdtsc_end = 0;     // Cycle counter at race completion
    uint32_t elapsed_cycles = 0;// Cycle delta (end - start)
    uint8_t  is_win = 0;        // 1 if won, 0 if lost
    uint8_t  was_stalled = 0;   // 1 if DRAM tRFC refresh stall hit
};

struct ChannelAllocation {
    int channel_id = 0;
    int core_affinity = 0;
    bool page_aligned = true;
    size_t chunk_bytes = 4096;
    std::shared_ptr<SparsePageChunk> page_data = nullptr;
    RDTSCCapture win_captures[RDTSC_CAPTURE_RING_SIZE] = {};
    RDTSCCapture loss_captures[RDTSC_CAPTURE_RING_SIZE] = {};
    size_t win_capture_count = 0;
    size_t loss_capture_count = 0;
};

struct HardwareMemoryCoordinate {
    uint64_t virtual_address = 0;       // Virtual address in hex (e.g. 0x7fff10040)
    uint64_t physical_pfn = 0;          // Physical Page Frame Number (PFN)
    uint16_t dram_row = 0;              // Decoded DRAM Row address
    uint8_t  dram_bank_group = 0;       // Decoded DRAM Bank Group (0..3)
    uint8_t  dram_bank = 0;             // Decoded DRAM Bank (0..3)
    uint8_t  cxl_channel = 0;           // CXL Physical Channel (0 or 1)
    uint16_t page_offset_bytes = 0;     // Offset within 4KB physical page (0..4095)
    uint8_t  cacheline_index = 0;       // Cache line index within page (0..63)
    uint8_t  cacheline_offset = 0;      // Byte offset within 64-byte cache line (0..63)

    std::string to_hex_string() const;
    std::string to_coordinate_string() const;
};

struct PageAlignmentRecord {
    HardwareMemoryCoordinate initial_alloc;   // Raw allocation coordinate (hex + hardware breakdown)
    HardwareMemoryCoordinate discovered_page;// Discovered physical page coordinate
    uint16_t offset_bytes = 0;                // Starting byte offset from physical page boundary
    uint8_t  is_straddled = 0;                // 1 if initial allocation crossed physical page
};

HardwareMemoryCoordinate decode_hardware_coordinate(uint64_t address, uint8_t channel_id = 0);

struct GUIDHash {
    size_t operator()(const std::string& guid) const {
        std::hash<std::string> hasher;
        return hasher(guid);
    }
};

class TelemetryCompressor; // Forward declaration

class HedgedVMA {
public:
    explicit HedgedVMA(uint32_t overflow_threshold = OVERFLOW_THRESHOLD_DEFAULT);
    ~HedgedVMA();

    // Allocates sparse memory chunks (1x = 4KB, 2x = 8KB) with optional page alignment
    bool allocate_sparse_chunk(const std::string& guid_str, const uint8_t* data, size_t len,
                                bool force_page_aligned = true, size_t granularity_multiplier = 1);

    // Executes dual-channel hedged race read, measuring RDTSC cycles and boundary stalls
    bool read_chunk_hedged(const std::string& guid_str, uint8_t* out_buffer, size_t max_len, int& winning_channel);

    // Discovers physical alignment boundary within 2x chunk and records KV mapping
    PageAlignmentRecord discover_page_alignment(const std::string& guid_str, uint64_t raw_address);

    // Retrieves alignment record for specified chunk GUID
    bool get_page_alignment_record(const std::string& guid_str, PageAlignmentRecord& record) const;

    // Transparently re-allocates a straddled chunk to a page-aligned memory block without data loss
    bool transparent_reallocate_chunk(const std::string& guid_str);

    // Scrubs and re-aligns all straddled unaligned chunks in memory, releasing straddled pages
    size_t scrub_and_realign_straddled_chunks();

    // Prints concise telemetry report with RDTSC win/loss capture stats and scrubbing telemetry
    void print_telemetry_report() const;

private:
    uint32_t compute_crc32c(const uint8_t* data, size_t len) const;
    void check_ftw_overflow(const std::string& guid_str, ChannelAllocation& ch_alloc);
    void check_and_flush_capture_watermark(const std::string& guid_str, ChannelAllocation& ch_alloc);

    uint32_t m_overflow_threshold;
    uint64_t m_hlc_counter;
    size_t m_scrubbed_count;
    std::unordered_map<std::string, std::vector<ChannelAllocation>, GUIDHash> m_chunk_registry;
    std::unordered_map<std::string, PageAlignmentRecord, GUIDHash> m_page_alignment_kv;
    std::unique_ptr<TelemetryCompressor> m_compressor;
};

#endif // HVMA_HPP

// end of file: labs/tailzlayer/include/hvma.hpp
