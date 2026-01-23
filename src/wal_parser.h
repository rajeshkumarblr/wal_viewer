#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Define a standalone RelFileNode struct to avoid exposing PostgreSQL headers
// to the UI
struct WalRelFileNode {
  uint32_t spcNode;
  uint32_t dbNode;
  uint32_t relNode;
};

// Structures derived from PostgreSQL headers are handled in the implementation
// (cpp) file. We use standard types here for the interface.

// RMIDs from xlog_internal.h
#define RM_XLOG_ID 0
#define RM_XACT_ID 1
#define RM_SMGR_ID 2
#define RM_CLOG_ID 3
#define RM_DBASE_ID 4
#define RM_TBLSPC_ID 5
#define RM_MULTIXACT_ID 6
#define RM_RELMAP_ID 7
#define RM_STANDBY_ID 8
#define RM_HEAP2_ID 9
#define RM_HEAP_ID 10

// Info Flags / Masks
#define XLOG_HEAP_OPMASK 0x70
#define XLOG_HEAP_INSERT 0x00
#define XLOG_HEAP_DELETE 0x10
#define XLOG_HEAP_UPDATE 0x20
#define XLOG_HEAP_HOT_UPDATE 0x40

#define XLOG_HEAP2_CLEAN 0x00
#define XLOG_HEAP2_FREEZE_PAGE 0x10
#define XLOG_HEAP2_MULTI_INSERT 0x40

#define XLOG_XACT_COMMIT 0x00
#define XLOG_XACT_ABORT 0x10
#define XLOG_XACT_PREPARE 0x20

#define RM_BTREE_ID 11
#define RM_HASH_ID 12
#define RM_GIN_ID 13
#define RM_GIST_ID 14
#define RM_SEQ_ID 15
#define RM_SPGIST_ID 16
#define RM_BRIN_ID 17
#define RM_COMMIT_TS_ID 18
#define RM_REPLORIGIN_ID 19
#define RM_GENERIC_ID 20
#define RM_LOGICALMSG_ID 21

struct WalRecordInfo {

  size_t Offset;           /* Global offset in the loaded buffer */
  uint64_t LSN;            /* Log Sequence Number (approximate/derived) */
  uint32_t Length;         /* Total length */
  uint32_t XID;            /* Transaction ID */
  uint8_t RMID;            /* Resource Manager ID */
  uint8_t Info;            /* Info flags */
  std::string Description; /* Generated description */
  std::vector<WalRelFileNode> RelFileNodes; /* Affected relations */
};

class WalParser {
public:
  bool Parse(const uint8_t *data, size_t size,
             std::vector<WalRecordInfo> &out_records);
  std::string GetRmidName(uint8_t rmid);
  std::string GetOpDescription(uint8_t rmid, uint8_t info);
};
