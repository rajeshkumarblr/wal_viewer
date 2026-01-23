#include "wal_parser.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

// --- Local Definitions mimicking PostgreSQL structures ---
// This allows parsing without linking against sensitive system headers that
// conflict with C++

typedef uint32_t Oid;
typedef uint32_t RelFileNumber;
typedef uint32_t BlockNumber;
typedef uint32_t TransactionId;
typedef uint32_t TimeLineID;
typedef uint64_t XLogRecPtr;

#define XLOG_PAGE_MAGIC 0xD113
#define XLP_LONG_HEADER 0x0002
#define XLOG_BLCKSZ 8192
#define MAXALIGN(LEN) (((uint64_t)(LEN) + 7) & ~7)

// Resource Manager IDs defined in wal_parser.h

typedef struct XLogPageHeaderData {
  uint16_t xlp_magic;
  uint16_t xlp_info;
  TimeLineID xlp_tli;
  XLogRecPtr xlp_pageaddr;
  uint32_t xlp_rem_len;
} XLogPageHeaderData;

typedef struct XLogLongPageHeaderData {
  XLogPageHeaderData std;
  uint64_t xlp_sysid;
  uint32_t xlp_seg_size;
  uint32_t xlp_xlog_blcksz;
} XLogLongPageHeaderData;

typedef struct XLogRecord {
  uint32_t xl_tot_len;
  TransactionId xl_xid;
  XLogRecPtr xl_prev;
  uint8_t xl_info;
  uint8_t xl_rmid;
  uint8_t _pad[2];
  uint32_t xl_crc;
} XLogRecord; // Size = 24

#define SizeOfXLogRecord sizeof(XLogRecord)

typedef struct RelFileLocator {
  Oid spcOid;
  Oid dbOid;
  RelFileNumber relNumber;
} RelFileLocator;

// Block Header constants
#define XLR_MAX_BLOCK_ID 32
#define XLR_BLOCK_ID_DATA_SHORT 255
#define XLR_BLOCK_ID_DATA_LONG 254
#define XLR_BLOCK_ID_ORIGIN 253
#define XLR_BLOCK_ID_TOPLEVEL_XID 252

#define BKPBLOCK_HAS_IMAGE 0x10
#define BKPBLOCK_SAME_REL 0x80
#define BKPIMAGE_HAS_HOLE 0x01
#define BKPIMAGE_COMPRESS_PGLZ 0x04
#define BKPIMAGE_COMPRESS_LZ4 0x08
#define BKPIMAGE_COMPRESS_ZSTD 0x10
#define BKPIMAGE_COMPRESSED(info)                                              \
  ((info & (BKPIMAGE_COMPRESS_PGLZ | BKPIMAGE_COMPRESS_LZ4 |                   \
            BKPIMAGE_COMPRESS_ZSTD)) != 0)

#define SizeOfXLogRecordBlockImageHeader 5
#define SizeOfXLogRecordBlockCompressHeader 2

// Array of RMID names, indexed by RMID.
// This needs to be kept in sync with the RM_XXX_ID definitions in wal_parser.h
static const char *const rmid_names[] = {
    "XLOG",       "Transaction", "Storage",    "CLOG",     "Database",
    "Tablespace", "MultiXact",   "RelMap",     "Standby",  "Heap2",
    "Heap",       "Btree",       "Hash",       "Gin",      "Gist",
    "Seq",        "SPGist",      "BRIN",       "CommitTS", "ReplOrigin",
    "Generic",    "LogicalMsg",  "Unknown(22)" // Placeholder for 22, if needed
};

std::string WalParser::GetRmidName(uint8_t rmid) {
  if (rmid < sizeof(rmid_names) / sizeof(rmid_names[0])) {
    return rmid_names[rmid];
  }
  return "Unknown (" + std::to_string(rmid) + ")";
}

std::string WalParser::GetOpDescription(uint8_t rmid, uint8_t info) {
  if (rmid == RM_HEAP_ID) {
    uint8_t op = info & XLOG_HEAP_OPMASK;
    switch (op) {
    case XLOG_HEAP_INSERT:
      return "INSERT";
    case XLOG_HEAP_DELETE:
      return "DELETE";
    case XLOG_HEAP_UPDATE:
      return "UPDATE";
    case XLOG_HEAP_HOT_UPDATE:
      return "HOT_UPDATE";
    default:
      return "";
    }
  } else if (rmid == RM_HEAP2_ID) {
    uint8_t op = info & 0x70; // Using same mask?
    // Heap2 ops are slightly different bits sometimes, but let's check basic
    // For now using literal values from manual checking if needed, or macro
    if ((info & 0x70) == XLOG_HEAP2_CLEAN)
      return "CLEAN";
    if ((info & 0x70) == XLOG_HEAP2_FREEZE_PAGE)
      return "FREEZE_PAGE";
    if ((info & 0x70) == XLOG_HEAP2_MULTI_INSERT)
      return "MULTI_INSERT";
    return "";
  } else if (rmid == RM_XACT_ID) {
    // Transaction info bits are top 4 bits for some, but low bits for others?
    // Let's check typical values.
    // XLOG_XACT_COMMIT is 0x00
    // But need to be careful masking.
    if ((info & 0xF0) == 0x00)
      return "COMMIT"; // Simplified check
    if ((info & 0xF0) == 0x10)
      return "ABORT";
    if ((info & 0xF0) == 0x20)
      return "PREPARE";
    return "XACT";
  }
  return "";
}

// Helper to parse the payload headers for RelFileLocator
static void ParseXLogRecordPayload(const uint8_t *payload, uint32_t len,
                                   WalRecordInfo &info) {
  uint32_t offset = 0;
  RelFileLocator lastLocator = {0, 0, 0};

  while (offset < len) {
    // Ensure we have at least the block-id byte
    if (offset + 1 > len)
      break;

    uint8_t id = *(const uint8_t *)(payload + offset);

    if (id <= XLR_MAX_BLOCK_ID) {
      // XLogRecordBlockHeader
      // defined as: id(1), fork_flags(1), data_length(2) = 4 bytes
      if (offset + 4 > len)
        break;

      // Read fields
      uint8_t fork_flags = *(const uint8_t *)(payload + offset + 1);
      // uint16_t data_length; memcpy(&data_length, payload + offset + 2, 2);

      offset += 4; // Advance past fixed header

      // If BKPBLOCK_HAS_IMAGE, an XLogRecordBlockImageHeader struct follows
      if (fork_flags & BKPBLOCK_HAS_IMAGE) {
        // SizeOfXLogRecordBlockImageHeader is 5 bytes
        if (offset + SizeOfXLogRecordBlockImageHeader > len)
          break;

        uint8_t bimg_info = *(const uint8_t *)(payload + offset + 4);
        offset += SizeOfXLogRecordBlockImageHeader;

        // If HAS_HOLE and COMPRESSED, XLogRecordBlockCompressHeader follows
        if ((bimg_info & BKPIMAGE_HAS_HOLE) && BKPIMAGE_COMPRESSED(bimg_info)) {
          // SizeOfXLogRecordBlockCompressHeader is 2 bytes
          if (offset + SizeOfXLogRecordBlockCompressHeader > len)
            break;
          offset += SizeOfXLogRecordBlockCompressHeader;
        }
      }

      // If BKPBLOCK_SAME_REL is not set, a RelFileLocator follows
      if (!(fork_flags & BKPBLOCK_SAME_REL)) {
        if (offset + sizeof(RelFileLocator) > len)
          break;
        memcpy(&lastLocator, payload + offset, sizeof(RelFileLocator));
        offset += sizeof(RelFileLocator);
      }

      // Add to info (copy)
      WalRelFileNode node;
      node.spcNode = lastLocator.spcOid;
      node.dbNode = lastLocator.dbOid;
      node.relNode = lastLocator.relNumber;
      info.RelFileNodes.push_back(node);

      // BlockNumber follows
      if (offset + sizeof(BlockNumber) > len)
        break;
      offset += sizeof(BlockNumber);

    } else if (id == XLR_BLOCK_ID_DATA_SHORT) {
      // End of block headers
      break;
    } else if (id == XLR_BLOCK_ID_DATA_LONG) {
      // End of block headers
      break;
    } else if (id == XLR_BLOCK_ID_ORIGIN) {
      // RepOriginId (uint16) follows id
      if (offset + 1 + 2 > len)
        break;
      offset += 3;
    } else if (id == XLR_BLOCK_ID_TOPLEVEL_XID) {
      // TransactionId (uint32) follows id
      if (offset + 1 + 4 > len)
        break;
      offset += 5;
    } else {
      // Unknown or other ID, stop parsing headers
      break;
    }
  }
}

bool WalParser::Parse(const uint8_t *data, size_t size,
                      std::vector<WalRecordInfo> &out_records) {
  out_records.clear();

  if (size < sizeof(XLogPageHeaderData)) {
    return false;
  }

  size_t offset = 0;
  while (offset < size) {
    // Ensure we have enough data for a page header
    if (offset + sizeof(XLogPageHeaderData) > size)
      break;

    const XLogPageHeaderData *header =
        (const XLogPageHeaderData *)(data + offset);

    // Basic verification
    if (header->xlp_magic != XLOG_PAGE_MAGIC) {
      break;
    }

    size_t pageHeaderSize = sizeof(XLogPageHeaderData);
    if (header->xlp_info & XLP_LONG_HEADER) {
      pageHeaderSize = sizeof(XLogLongPageHeaderData);
    }

    // Pointer to the first record on the page
    size_t currentPos = offset + pageHeaderSize;

    if (header->xlp_rem_len > 0) {
      // The data for the previous record continues here.
      currentPos += header->xlp_rem_len;
    }

    // Alignment using PostgreSQL's MAXALIGN
    currentPos = MAXALIGN(currentPos);

    while (currentPos < offset + XLOG_BLCKSZ) {
      // Check end of data
      if (currentPos + sizeof(XLogRecord) > size)
        break;

      // Check end of page
      if (currentPos + sizeof(XLogRecord) > offset + XLOG_BLCKSZ)
        break;

      const XLogRecord *rec = (const XLogRecord *)(data + currentPos);

      if (rec->xl_tot_len == 0) {
        break;
      }

      WalRecordInfo info;
      info.Offset = currentPos;
      info.Length = rec->xl_tot_len;
      info.XID = rec->xl_xid;
      info.RMID = rec->xl_rmid;
      info.Info = rec->xl_info;
      info.Description = GetRmidName(rec->xl_rmid);

      // Append Info details
      std::string opDesc = GetOpDescription(rec->xl_rmid, rec->xl_info);
      if (!opDesc.empty()) {
        info.Description += ": " + opDesc;
      }
      // approximate LSN
      info.LSN = header->xlp_pageaddr + (currentPos - offset);

      // Parse Payload for RelFileNodes
      uint32_t recordHeaderLen = SizeOfXLogRecord;
      if (rec->xl_tot_len > recordHeaderLen) {
        // Ensure we don't go out of bounds of the file
        size_t payloadMax = rec->xl_tot_len - recordHeaderLen;
        if (currentPos + rec->xl_tot_len <= size) {
          ParseXLogRecordPayload(data + currentPos + recordHeaderLen,
                                 payloadMax, info);
        }
      }

      out_records.push_back(info);

      // Move to next record
      currentPos += rec->xl_tot_len;
      currentPos = MAXALIGN(currentPos);
    }

    // Move to next page
    offset += XLOG_BLCKSZ;
  }

  return !out_records.empty();
}
