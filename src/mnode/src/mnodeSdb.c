/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "hash.h"
#include "tutil.h"
#include "tbalance.h"
#include "tqueue.h"
#include "twal.h"
#include "tsync.h"
#include "ttimer.h"
#include "tglobal.h"
#include "dnode.h"
#include "mnode.h"
#include "mnodeDef.h"
#include "mnodeInt.h"
#include "mnodeMnode.h"
#include "mnodeDnode.h"
#include "mnodeCluster.h"
#include "mnodeSdb.h"

#define SDB_TABLE_LEN 12
#define SDB_SYNC_HACK 16

typedef enum {
  SDB_ACTION_INSERT = 0,
  SDB_ACTION_DELETE = 1,
  SDB_ACTION_UPDATE = 2
} ESdbAction;

typedef enum {
  SDB_STATUS_OFFLINE = 0,
  SDB_STATUS_SERVING = 1,
  SDB_STATUS_CLOSING = 2
} ESdbStatus;

char *actStr[] = {
  "insert",
  "delete",
  "update",
  "invalid"
};

typedef struct SSdbTable {
  char      tableName[SDB_TABLE_LEN];
  ESdbTable tableId;
  ESdbKey   keyType;
  int32_t   hashSessions;
  int32_t   maxRowSize;
  int32_t   refCountPos;
  int32_t   autoIndex;
  int64_t   numOfRows;
  void *    iHandle;
  int32_t (*fpInsert)(SSWriteMsg *pWrite);
  int32_t (*fpDelete)(SSWriteMsg *pWrite);
  int32_t (*fpUpdate)(SSWriteMsg *pWrite);
  int32_t (*fpDecode)(SSWriteMsg *pWrite);
  int32_t (*fpEncode)(SSWriteMsg *pWrite);
  int32_t (*fpDestroy)(SSWriteMsg *pWrite);
  int32_t (*fpRestored)();
  pthread_mutex_t mutex;
} SSdbTable;

typedef struct {
  ESyncRole  role;
  ESdbStatus status;
  uint64_t   version;
  int64_t    sync;
  void *     wal;
  SSyncCfg   cfg;
  int32_t    numOfTables;
  SSdbTable *tableList[SDB_TABLE_MAX];
  pthread_mutex_t mutex;
} SSdbMgmt;

typedef struct {
  pthread_t thread;
  int32_t   workerId;
} SSdbWorker;

typedef struct {
  int32_t num;
  SSdbWorker *worker;
} SSdbWorkerPool;

extern void *     tsMnodeTmr;
static void *     tsSdbTmr;
static SSdbMgmt   tsSdbMgmt = {0};
static taos_qset  tsSdbWQset;
static taos_qall  tsSdbWQall;
static taos_queue tsSdbWQueue;
static SSdbWorkerPool tsSdbPool;

static int32_t sdbWrite(void *pWrite, void *pHead, int32_t qtype, void *unused);
static int32_t sdbWriteToQueue(void *pWrite, void *pHead, int32_t qtype, void *unused);
static void *  sdbWorkerFp(void *pWorker);
static int32_t sdbInitWorker();
static void    sdbCleanupWorker();
static int32_t sdbAllocQueue();
static void    sdbFreeQueue();
extern int32_t sdbInsertRowImp(SSWriteMsg *pWrite);
static int32_t sdbUpdateRowImp(SSWriteMsg *pWrite);
static int32_t sdbDeleteRowImp(SSWriteMsg *pWrite);
static int32_t sdbInsertHash(SSdbTable *pTable, SSWriteMsg *pWrite);
static int32_t sdbUpdateHash(SSdbTable *pTable, SSWriteMsg *pWrite);
static int32_t sdbDeleteHash(SSdbTable *pTable, SSWriteMsg *pWrite);

int32_t sdbGetId(void *pTable) {
  return ((SSdbTable *)pTable)->autoIndex;
}

int64_t sdbGetNumOfRows(void *pTable) {
  return ((SSdbTable *)pTable)->numOfRows;
}

uint64_t sdbGetVersion() {
  return tsSdbMgmt.version;
}

bool sdbIsMaster() { 
  return tsSdbMgmt.role == TAOS_SYNC_ROLE_MASTER; 
}

bool sdbIsServing() {
  return tsSdbMgmt.status == SDB_STATUS_SERVING; 
}

static void *sdbGetObjKey(SSdbTable *pTable, void *key) {
  if (pTable->keyType == SDB_KEY_VAR_STRING) {
    return *(char **)key;
  }

  return key;
}

static char *sdbGetKeyStr(SSdbTable *pTable, void *key) {
  static char str[16];
  switch (pTable->keyType) {
    case SDB_KEY_STRING:
    case SDB_KEY_VAR_STRING:
      return (char *)key;
    case SDB_KEY_INT:
    case SDB_KEY_AUTO:
      sprintf(str, "%d", *(int32_t *)key);
      return str;
    default:
      return "invalid";
  }
}

static char *sdbGetRowStr(SSdbTable *pTable, void *key) {
  return sdbGetKeyStr(pTable, sdbGetObjKey(pTable, key));
}

static void *sdbGetTableFromId(int32_t tableId) {
  return tsSdbMgmt.tableList[tableId];
}

static int32_t sdbInitWal() {
  SWalCfg walCfg = {.vgId = 1, .walLevel = TAOS_WAL_FSYNC, .keep = TAOS_WAL_KEEP, .fsyncPeriod = 0};
  char    temp[TSDB_FILENAME_LEN] = {0};
  sprintf(temp, "%s/wal", tsMnodeDir);
  tsSdbMgmt.wal = walOpen(temp, &walCfg);
  if (tsSdbMgmt.wal == NULL) {
    sdbError("vgId:1, failed to open wal in %s", tsMnodeDir);
    return -1;
  }

  sdbInfo("vgId:1, open wal for restore");
  int code = walRestore(tsSdbMgmt.wal, NULL, sdbWrite);
  if (code != TSDB_CODE_SUCCESS) {
    sdbError("vgId:1, failed to open wal for restore since %s", tstrerror(code));
    return -1;
  }
  return 0;
}

static void sdbRestoreTables() {
  int32_t totalRows = 0;
  int32_t numOfTables = 0;
  for (int32_t tableId = 0; tableId < SDB_TABLE_MAX; ++tableId) {
    SSdbTable *pTable = sdbGetTableFromId(tableId);
    if (pTable == NULL) continue;
    if (pTable->fpRestored) {
      (*pTable->fpRestored)();
    }

    totalRows += pTable->numOfRows;
    numOfTables++;
    sdbDebug("vgId:1, sdb:%s is restored, rows:%" PRId64, pTable->tableName, pTable->numOfRows);
  }

  sdbInfo("vgId:1, sdb is restored, mver:%" PRIu64 " rows:%d tables:%d", tsSdbMgmt.version, totalRows, numOfTables);
}

void sdbUpdateMnodeRoles() {
  if (tsSdbMgmt.sync <= 0) return;

  SNodesRole roles = {0};
  syncGetNodesRole(tsSdbMgmt.sync, &roles);

  sdbInfo("vgId:1, update mnodes role, replica:%d", tsSdbMgmt.cfg.replica);
  for (int32_t i = 0; i < tsSdbMgmt.cfg.replica; ++i) {
    SMnodeObj *pMnode = mnodeGetMnode(roles.nodeId[i]);
    if (pMnode != NULL) {
      pMnode->role = roles.role[i];
      sdbInfo("vgId:1, mnode:%d, role:%s", pMnode->mnodeId, syncRole[pMnode->role]);
      if (pMnode->mnodeId == dnodeGetDnodeId()) tsSdbMgmt.role = pMnode->role;
      mnodeDecMnodeRef(pMnode);
    }
  }

  mnodeUpdateClusterId();
  mnodeUpdateMnodeEpSet();
}

static uint32_t sdbGetFileInfo(void *ahandle, char *name, uint32_t *index, uint32_t eindex, int64_t *size, uint64_t *fversion) {
  sdbUpdateMnodeRoles();
  return 0;
}

static int32_t sdbGetWalInfo(void *ahandle, char *fileName, int64_t *fileId) {
  return walGetWalFile(tsSdbMgmt.wal, fileName, fileId);
}

static void sdbNotifyRole(void *ahandle, int8_t role) {
  sdbInfo("vgId:1, mnode role changed from %s to %s", syncRole[tsSdbMgmt.role], syncRole[role]);

  if (role == TAOS_SYNC_ROLE_MASTER && tsSdbMgmt.role != TAOS_SYNC_ROLE_MASTER) {
    balanceReset();
  }
  tsSdbMgmt.role = role;

  sdbUpdateMnodeRoles();
}

FORCE_INLINE
static void sdbConfirmForward(void *ahandle, void *wparam, int32_t code) {
  if (wparam == NULL) return;
  SSWriteMsg *pWrite = wparam;
  SMnodeMsg * pMsg = pWrite->pMsg;

  if (code <= 0) pWrite->code = code;
  int32_t count = atomic_add_fetch_32(&pWrite->processedCount, 1);
  if (count <= 1) {
    if (pMsg != NULL) sdbTrace("vgId:1, msg:%p waiting for confirm, count:%d code:%x", pMsg, count, code);
    return;
  } else {
    if (pMsg != NULL) sdbTrace("vgId:1, msg:%p is confirmed, code:%x", pMsg, code);
  }

  // failed to forward, need revert insert
  if (pWrite->code != TSDB_CODE_SUCCESS) {
    SWalHead *pHead = (SWalHead *)((char *)pWrite + sizeof(SSWriteMsg) + SDB_SYNC_HACK);
    int32_t   action = pHead->msgType % 10;
    sdbError("vgId:1, row:%p:%s hver:%" PRIu64 " action:%s, failed to foward since %s", pWrite->pRow,
             sdbGetKeyStr(pWrite->pTable, pHead->cont), pHead->version, actStr[action], tstrerror(pWrite->code));
    if (action == SDB_ACTION_INSERT) {
      // It's better to create a table in two stages, create it first and then set it success
      SSWriteMsg wmsg = {
        .type   = SDB_OPER_GLOBAL,
        .pTable = pWrite->pTable,
        .pRow   = pWrite->pRow
      };
      sdbDeleteRow(&wmsg);
    }
  }

  if (pWrite->fpRsp != NULL) {
    pWrite->code = (*pWrite->fpRsp)(pMsg, pWrite->code);
  }
  dnodeSendRpcMWriteRsp(pMsg, pWrite->code);

  // if ahandle, means this func is called by sdb write
  if (ahandle == NULL) {
    sdbDecRef(pWrite->pTable, pWrite->pRow);
  }

  taosFreeQitem(pWrite);
}

static void sdbUpdateSyncTmrFp(void *param, void *tmrId) { sdbUpdateSync(NULL); }

void sdbUpdateAsync() {
  taosTmrReset(sdbUpdateSyncTmrFp, 200, NULL, tsMnodeTmr, &tsSdbTmr);
}

void sdbUpdateSync(void *pMnodes) {
  SMnodeInfos *mnodes = pMnodes;
  if (!mnodeIsRunning()) {
    mDebug("vgId:1, mnode not start yet, update sync config later");
    return;
  }

  mDebug("vgId:1, update sync config in sync module, mnodes:%p", pMnodes);

  SSyncCfg syncCfg = {0};
  int32_t  index = 0;

  if (mnodes == NULL) {
    void *pIter = NULL;
    while (1) {
      SMnodeObj *pMnode = NULL;
      pIter = mnodeGetNextMnode(pIter, &pMnode);
      if (pMnode == NULL) break;

      syncCfg.nodeInfo[index].nodeId = pMnode->mnodeId;

      SDnodeObj *pDnode = mnodeGetDnode(pMnode->mnodeId);
      if (pDnode != NULL) {
        syncCfg.nodeInfo[index].nodePort = pDnode->dnodePort + TSDB_PORT_SYNC;
        tstrncpy(syncCfg.nodeInfo[index].nodeFqdn, pDnode->dnodeFqdn, TSDB_FQDN_LEN);
        index++;
      }

      mnodeDecDnodeRef(pDnode);
      mnodeDecMnodeRef(pMnode);
    }
    sdbFreeIter(pIter);
    syncCfg.replica = index;
    mDebug("vgId:1, mnodes info not input, use infos in sdb, numOfMnodes:%d", syncCfg.replica);
  } else {
    for (index = 0; index < mnodes->mnodeNum; ++index) {
      SMnodeInfo *node = &mnodes->mnodeInfos[index];
      syncCfg.nodeInfo[index].nodeId = node->mnodeId;
      taosGetFqdnPortFromEp(node->mnodeEp, syncCfg.nodeInfo[index].nodeFqdn, &syncCfg.nodeInfo[index].nodePort);
      syncCfg.nodeInfo[index].nodePort += TSDB_PORT_SYNC;
    }
    syncCfg.replica = index;
    mDebug("vgId:1, mnodes info input, numOfMnodes:%d", syncCfg.replica);
  }

  syncCfg.quorum = (syncCfg.replica == 1) ? 1 : 2;

  bool hasThisDnode = false;
  for (int32_t i = 0; i < syncCfg.replica; ++i) {
    if (syncCfg.nodeInfo[i].nodeId == dnodeGetDnodeId()) {
      hasThisDnode = true;
      break;
    }
  }

  if (!hasThisDnode) {
    sdbDebug("vgId:1, update sync config, this dnode not exist");
    return;
  }

  if (memcmp(&syncCfg, &tsSdbMgmt.cfg, sizeof(SSyncCfg)) == 0) {
    sdbDebug("vgId:1, update sync config, info not changed");
    return;
  }

  sdbInfo("vgId:1, work as mnode, replica:%d", syncCfg.replica);
  for (int32_t i = 0; i < syncCfg.replica; ++i) {
    sdbInfo("vgId:1, mnode:%d, %s:%d", syncCfg.nodeInfo[i].nodeId, syncCfg.nodeInfo[i].nodeFqdn,
            syncCfg.nodeInfo[i].nodePort);
  }

  SSyncInfo syncInfo = {0};
  syncInfo.vgId = 1;
  syncInfo.version = sdbGetVersion();
  syncInfo.syncCfg = syncCfg;
  sprintf(syncInfo.path, "%s", tsMnodeDir);
  syncInfo.ahandle = NULL;
  syncInfo.getWalInfo = sdbGetWalInfo;
  syncInfo.getFileInfo = sdbGetFileInfo;
  syncInfo.writeToCache = sdbWriteToQueue;
  syncInfo.confirmForward = sdbConfirmForward;
  syncInfo.notifyRole = sdbNotifyRole;
  tsSdbMgmt.cfg = syncCfg;

  if (tsSdbMgmt.sync) {
    syncReconfig(tsSdbMgmt.sync, &syncCfg);
  } else {
    tsSdbMgmt.sync = syncStart(&syncInfo);
  }
  sdbUpdateMnodeRoles();
}

int32_t sdbInit() {
  pthread_mutex_init(&tsSdbMgmt.mutex, NULL);

  if (sdbInitWorker() != 0) {
    return -1;
  }

  if (sdbInitWal() != 0) {
    return -1;
  }

  sdbRestoreTables();

  if (mnodeGetMnodesNum() == 1) {
    tsSdbMgmt.role = TAOS_SYNC_ROLE_MASTER;
  }

  tsSdbMgmt.status = SDB_STATUS_SERVING;
  return TSDB_CODE_SUCCESS;
}

void sdbCleanUp() {
  if (tsSdbMgmt.status != SDB_STATUS_SERVING) return;

  tsSdbMgmt.status = SDB_STATUS_CLOSING;

  sdbCleanupWorker();
  sdbDebug("vgId:1, sdb will be closed, mver:%" PRIu64, tsSdbMgmt.version);

  if (tsSdbMgmt.sync) {
    syncStop(tsSdbMgmt.sync);
    tsSdbMgmt.sync = -1;
  }

  if (tsSdbMgmt.wal) {
    walClose(tsSdbMgmt.wal);
    tsSdbMgmt.wal = NULL;
  }
  
  pthread_mutex_destroy(&tsSdbMgmt.mutex);
}

void sdbIncRef(void *tparam, void *pRow) {
  if (pRow == NULL || tparam == NULL) return;

  SSdbTable *pTable = tparam;
  int32_t *  pRefCount = (int32_t *)(pRow + pTable->refCountPos);
  int32_t    refCount = atomic_add_fetch_32(pRefCount, 1);
  sdbTrace("vgId:1, sdb:%s, inc ref to row:%p:%s:%d", pTable->tableName, pRow, sdbGetRowStr(pTable, pRow), refCount);
}

void sdbDecRef(void *tparam, void *pRow) {
  if (pRow == NULL || tparam == NULL) return;

  SSdbTable *pTable = tparam;
  int32_t *  pRefCount = (int32_t *)(pRow + pTable->refCountPos);
  int32_t    refCount = atomic_sub_fetch_32(pRefCount, 1);
  sdbTrace("vgId:1, sdb:%s, dec ref to row:%p:%s:%d", pTable->tableName, pRow, sdbGetRowStr(pTable, pRow), refCount);

  int32_t *updateEnd = pRow + pTable->refCountPos - 4;
  if (refCount <= 0 && *updateEnd) {
    sdbTrace("vgId:1, sdb:%s, row:%p:%s:%d destroyed", pTable->tableName, pRow, sdbGetRowStr(pTable, pRow), refCount);
    SSWriteMsg wmsg = {.pRow = pRow};
    (*pTable->fpDestroy)(&wmsg);
  }
}

static void *sdbGetRowMeta(SSdbTable *pTable, void *key) {
  if (pTable == NULL) return NULL;

  int32_t keySize = sizeof(int32_t);
  if (pTable->keyType == SDB_KEY_STRING || pTable->keyType == SDB_KEY_VAR_STRING) {
    keySize = strlen((char *)key);
  }

  void **ppRow = (void **)taosHashGet(pTable->iHandle, key, keySize);
  if (ppRow != NULL) return *ppRow;

  return NULL;
}

static void *sdbGetRowMetaFromObj(SSdbTable *pTable, void *key) {
  return sdbGetRowMeta(pTable, sdbGetObjKey(pTable, key));
}

void *sdbGetRow(void *tparam, void *key) {
  SSdbTable *pTable = tparam;

  pthread_mutex_lock(&pTable->mutex);
  void *pRow = sdbGetRowMeta(pTable, key);
  if (pRow) sdbIncRef(pTable, pRow);
  pthread_mutex_unlock(&pTable->mutex);

  return pRow;
}

static void *sdbGetRowFromObj(SSdbTable *pTable, void *key) {
  return sdbGetRow(pTable, sdbGetObjKey(pTable, key));
}

static int32_t sdbInsertHash(SSdbTable *pTable, SSWriteMsg *pWrite) {
  void *  key = sdbGetObjKey(pTable, pWrite->pRow);
  int32_t keySize = sizeof(int32_t);

  if (pTable->keyType == SDB_KEY_STRING || pTable->keyType == SDB_KEY_VAR_STRING) {
    keySize = strlen((char *)key);
  }

  pthread_mutex_lock(&pTable->mutex);
  taosHashPut(pTable->iHandle, key, keySize, &pWrite->pRow, sizeof(int64_t));
  pthread_mutex_unlock(&pTable->mutex);

  sdbIncRef(pTable, pWrite->pRow);
  atomic_add_fetch_32(&pTable->numOfRows, 1);

  if (pTable->keyType == SDB_KEY_AUTO) {
    pTable->autoIndex = MAX(pTable->autoIndex, *((uint32_t *)pWrite->pRow));
  } else {
    atomic_add_fetch_32(&pTable->autoIndex, 1);
  }

  sdbDebug("vgId:1, sdb:%s, insert key:%s to hash, rowSize:%d rows:%" PRId64 ", msg:%p", pTable->tableName,
           sdbGetRowStr(pTable, pWrite->pRow), pWrite->rowSize, pTable->numOfRows, pWrite->pMsg);

  int32_t code = (*pTable->fpInsert)(pWrite);
  if (code != TSDB_CODE_SUCCESS) {
    sdbError("vgId:1, sdb:%s, failed to insert key:%s to hash, remove it", pTable->tableName,
             sdbGetRowStr(pTable, pWrite->pRow));
    sdbDeleteHash(pTable, pWrite);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t sdbDeleteHash(SSdbTable *pTable, SSWriteMsg *pWrite) {
  int32_t *updateEnd = pWrite->pRow + pTable->refCountPos - 4;
  bool set = atomic_val_compare_exchange_32(updateEnd, 0, 1) == 0;
  if (!set) {
    sdbError("vgId:1, sdb:%s, failed to delete key:%s from hash, for it already removed", pTable->tableName,
             sdbGetRowStr(pTable, pWrite->pRow));
    return TSDB_CODE_MND_SDB_OBJ_NOT_THERE;
  }

  (*pTable->fpDelete)(pWrite);
  
  void *  key = sdbGetObjKey(pTable, pWrite->pRow);
  int32_t keySize = sizeof(int32_t);
  if (pTable->keyType == SDB_KEY_STRING || pTable->keyType == SDB_KEY_VAR_STRING) {
    keySize = strlen((char *)key);
  }

  pthread_mutex_lock(&pTable->mutex);
  taosHashRemove(pTable->iHandle, key, keySize);
  pthread_mutex_unlock(&pTable->mutex);

  atomic_sub_fetch_32(&pTable->numOfRows, 1);

  sdbDebug("vgId:1, sdb:%s, delete key:%s from hash, numOfRows:%" PRId64 ", msg:%p", pTable->tableName,
           sdbGetRowStr(pTable, pWrite->pRow), pTable->numOfRows, pWrite->pMsg);

  sdbDecRef(pTable, pWrite->pRow);

  return TSDB_CODE_SUCCESS;
}

static int32_t sdbUpdateHash(SSdbTable *pTable, SSWriteMsg *pWrite) {
  sdbDebug("vgId:1, sdb:%s, update key:%s in hash, numOfRows:%" PRId64 ", msg:%p", pTable->tableName,
           sdbGetRowStr(pTable, pWrite->pRow), pTable->numOfRows, pWrite->pMsg);

  (*pTable->fpUpdate)(pWrite);
  return TSDB_CODE_SUCCESS;
}

static int sdbWrite(void *wparam, void *hparam, int32_t qtype, void *unused) {
  SSWriteMsg *pWrite = wparam;
  SWalHead *pHead = hparam;
  int32_t tableId = pHead->msgType / 10;
  int32_t action = pHead->msgType % 10;

  SSdbTable *pTable = sdbGetTableFromId(tableId);
  assert(pTable != NULL);

  pthread_mutex_lock(&tsSdbMgmt.mutex);
  
  if (pHead->version == 0) {
    // assign version
    tsSdbMgmt.version++;
    pHead->version = tsSdbMgmt.version;
  } else {
    // for data from WAL or forward, version may be smaller
    if (pHead->version <= tsSdbMgmt.version) {
      pthread_mutex_unlock(&tsSdbMgmt.mutex);
      sdbDebug("vgId:1, sdb:%s, failed to restore %s key:%s from source(%d), hver:%" PRIu64 " too large, mver:%" PRIu64,
               pTable->tableName, actStr[action], sdbGetKeyStr(pTable, pHead->cont), qtype, pHead->version, tsSdbMgmt.version);
      return TSDB_CODE_SUCCESS;
    } else if (pHead->version != tsSdbMgmt.version + 1) {
      pthread_mutex_unlock(&tsSdbMgmt.mutex);
      sdbError("vgId:1, sdb:%s, failed to restore %s key:%s from source(%d), hver:%" PRIu64 " too large, mver:%" PRIu64,
               pTable->tableName, actStr[action], sdbGetKeyStr(pTable, pHead->cont), qtype, pHead->version, tsSdbMgmt.version);
      return TSDB_CODE_SYN_INVALID_VERSION;
    } else {
      tsSdbMgmt.version = pHead->version;
    }
  }

  int32_t code = walWrite(tsSdbMgmt.wal, pHead);
  if (code < 0) {
    pthread_mutex_unlock(&tsSdbMgmt.mutex);
    return code;
  }

  pthread_mutex_unlock(&tsSdbMgmt.mutex);

  // from app, wmsg is created
  if (pWrite != NULL) {
    // forward to peers
    pWrite->processedCount = 0;
    int32_t syncCode = syncForwardToPeer(tsSdbMgmt.sync, pHead, pWrite, TAOS_QTYPE_RPC);
    if (syncCode <= 0) pWrite->processedCount = 1;

    if (syncCode < 0) {
      sdbError("vgId:1, sdb:%s, failed to forward req since %s action:%s key:%s hver:%" PRIu64 ", msg:%p", pTable->tableName,
               tstrerror(syncCode), actStr[action], sdbGetKeyStr(pTable, pHead->cont), pHead->version, pWrite->pMsg);
    } else if (syncCode > 0) {
      sdbDebug("vgId:1, sdb:%s, forward req is sent, action:%s key:%s hver:%" PRIu64 ", msg:%p", pTable->tableName,
               actStr[action], sdbGetKeyStr(pTable, pHead->cont), pHead->version, pWrite->pMsg);
    } else {
      sdbTrace("vgId:1, sdb:%s, no need to send fwd req, action:%s key:%s hver:%" PRIu64 ", msg:%p", pTable->tableName,
               actStr[action], sdbGetKeyStr(pTable, pHead->cont), pHead->version, pWrite->pMsg);
    }
    return syncCode;
  }

  sdbDebug("vgId:1, sdb:%s, record from wal/fwd is disposed, action:%s key:%s hver:%" PRIu64, pTable->tableName,
           actStr[action], sdbGetKeyStr(pTable, pHead->cont), pHead->version);

  // even it is WAL/FWD, it shall be called to update version in sync
  syncForwardToPeer(tsSdbMgmt.sync, pHead, pWrite, TAOS_QTYPE_RPC);

  // from wal or forward msg, wmsg not created, should add into hash
  if (action == SDB_ACTION_INSERT) {
    SSWriteMsg wmsg = {.rowSize = pHead->len, .rowData = pHead->cont, .pTable = pTable};
    code = (*pTable->fpDecode)(&wmsg);
    return sdbInsertHash(pTable, &wmsg);
  } else if (action == SDB_ACTION_DELETE) {
    void *pRow = sdbGetRowMeta(pTable, pHead->cont);
    if (pRow == NULL) {
      sdbDebug("vgId:1, sdb:%s, object:%s not exist in hash, ignore delete action", pTable->tableName,
               sdbGetKeyStr(pTable, pHead->cont));
      return TSDB_CODE_SUCCESS;
    }
    SSWriteMsg wmsg = {.pTable = pTable, .pRow = pRow};
    return sdbDeleteHash(pTable, &wmsg);
  } else if (action == SDB_ACTION_UPDATE) {
    void *pRow = sdbGetRowMeta(pTable, pHead->cont);
    if (pRow == NULL) {
      sdbDebug("vgId:1, sdb:%s, object:%s not exist in hash, ignore update action", pTable->tableName,
               sdbGetKeyStr(pTable, pHead->cont));
      return TSDB_CODE_SUCCESS;
    }
    SSWriteMsg wmsg = {.rowSize = pHead->len, .rowData = pHead->cont, .pTable = pTable};
    code = (*pTable->fpDecode)(&wmsg);
    return sdbUpdateHash(pTable, &wmsg);
  } else {
    return TSDB_CODE_MND_INVALID_MSG_TYPE;
  }
}

int32_t sdbInsertRow(SSWriteMsg *pWrite) {
  SSdbTable *pTable = pWrite->pTable;
  if (pTable == NULL) return TSDB_CODE_MND_SDB_INVALID_TABLE_TYPE;

  if (sdbGetRowFromObj(pTable, pWrite->pRow)) {
    sdbError("vgId:1, sdb:%s, failed to insert key:%s, already exist", pTable->tableName,
             sdbGetRowStr(pTable, pWrite->pRow));
    sdbDecRef(pTable, pWrite->pRow);
    return TSDB_CODE_MND_SDB_OBJ_ALREADY_THERE;
  }

  if (pTable->keyType == SDB_KEY_AUTO) {
    *((uint32_t *)pWrite->pRow) = atomic_add_fetch_32(&pTable->autoIndex, 1);

    // let vgId increase from 2
    if (pTable->autoIndex == 1 && strcmp(pTable->tableName, "vgroups") == 0) {
      *((uint32_t *)pWrite->pRow) = atomic_add_fetch_32(&pTable->autoIndex, 1);
    }
  }

  int32_t code = sdbInsertHash(pTable, pWrite);
  if (code != TSDB_CODE_SUCCESS) {
    sdbError("vgId:1, sdb:%s, failed to insert into hash", pTable->tableName);
    return code;
  }

  // just insert data into memory
  if (pWrite->type != SDB_OPER_GLOBAL) {
    return TSDB_CODE_SUCCESS;
  }

  if (pWrite->fpReq) {
    return (*pWrite->fpReq)(pWrite->pMsg);
  } else {
    return sdbInsertRowImp(pWrite);
  }
}

int32_t sdbInsertRowImp(SSWriteMsg *pWrite) {
  SSdbTable *pTable = pWrite->pTable;
  if (pTable == NULL) return TSDB_CODE_MND_SDB_INVALID_TABLE_TYPE;

  int32_t size = sizeof(SSWriteMsg) + sizeof(SWalHead) + pTable->maxRowSize + SDB_SYNC_HACK;
  SSWriteMsg *pNewWrite = taosAllocateQitem(size);

  SWalHead *pHead = (SWalHead *)((char *)pNewWrite + sizeof(SSWriteMsg) + SDB_SYNC_HACK);
  pHead->version = 0;
  pHead->len = pWrite->rowSize;
  pHead->msgType = pTable->tableId * 10 + SDB_ACTION_INSERT;

  pWrite->rowData = pHead->cont;
  (*pTable->fpEncode)(pWrite);
  pHead->len = pWrite->rowSize;

  memcpy(pNewWrite, pWrite, sizeof(SSWriteMsg));

  if (pNewWrite->pMsg != NULL) {
    sdbDebug("vgId:1, ahandle:%p msg:%p, sdb:%s row:%p:%s, insert action is add to sdb queue", pNewWrite->pMsg->rpcMsg.ahandle,
             pNewWrite->pMsg, pTable->tableName, pWrite->pRow, sdbGetRowStr(pTable, pWrite->pRow));
  }

  sdbIncRef(pNewWrite->pTable, pNewWrite->pRow);
  taosWriteQitem(tsSdbWQueue, TAOS_QTYPE_RPC, pNewWrite);

  return TSDB_CODE_MND_ACTION_IN_PROGRESS;
}

bool sdbCheckRowDeleted(void *tparam, void *pRow) {
  SSdbTable *pTable = tparam;
  if (pTable == NULL) return false;

  int32_t *updateEnd = pRow + pTable->refCountPos - 4;
  return atomic_val_compare_exchange_32(updateEnd, 1, 1) == 1;
}

int32_t sdbDeleteRow(SSWriteMsg *pWrite) {
  SSdbTable *pTable = pWrite->pTable;
  if (pTable == NULL) return TSDB_CODE_MND_SDB_INVALID_TABLE_TYPE;

  void *pRow = sdbGetRowMetaFromObj(pTable, pWrite->pRow);
  if (pRow == NULL) {
    sdbDebug("vgId:1, sdb:%s, record is not there, delete failed", pTable->tableName);
    return TSDB_CODE_MND_SDB_OBJ_NOT_THERE;
  }

  sdbIncRef(pTable, pWrite->pRow);

  int32_t code = sdbDeleteHash(pTable, pWrite);
  if (code != TSDB_CODE_SUCCESS) {
    sdbError("vgId:1, sdb:%s, failed to delete from hash", pTable->tableName);
    sdbDecRef(pTable, pWrite->pRow);
    return code;
  }

  // just delete data from memory
  if (pWrite->type != SDB_OPER_GLOBAL) {
    sdbDecRef(pTable, pWrite->pRow);
    return TSDB_CODE_SUCCESS;
  }

  if (pWrite->fpReq) {
    return (*pWrite->fpReq)(pWrite->pMsg);
  } else {
    return sdbDeleteRowImp(pWrite);
  }
}

int32_t sdbDeleteRowImp(SSWriteMsg *pWrite) {
  SSdbTable *pTable = pWrite->pTable;
  if (pTable == NULL) return TSDB_CODE_MND_SDB_INVALID_TABLE_TYPE;

  int32_t size = sizeof(SSWriteMsg) + sizeof(SWalHead) + pTable->maxRowSize + SDB_SYNC_HACK;
  SSWriteMsg *pNewWrite = taosAllocateQitem(size);

  SWalHead *pHead = (SWalHead *)((void *)pNewWrite + sizeof(SSWriteMsg) + SDB_SYNC_HACK);
  pHead->version = 0;
  pHead->msgType = pTable->tableId * 10 + SDB_ACTION_DELETE;

  pWrite->rowData = pHead->cont;
  (*pTable->fpEncode)(pWrite);
  pHead->len = pWrite->rowSize;

  memcpy(pNewWrite, pWrite, sizeof(SSWriteMsg));

  if (pNewWrite->pMsg != NULL) {
    sdbDebug("vgId:1, ahandle:%p msg:%p, sdb:%s row:%p:%s, delete action is add to sdb queue", pNewWrite->pMsg->rpcMsg.ahandle,
             pNewWrite->pMsg, pTable->tableName, pWrite->pRow, sdbGetRowStr(pTable, pWrite->pRow));
  }

  taosWriteQitem(tsSdbWQueue, TAOS_QTYPE_RPC, pNewWrite);

  return TSDB_CODE_MND_ACTION_IN_PROGRESS;
}

int32_t sdbUpdateRow(SSWriteMsg *pWrite) {
  SSdbTable *pTable = pWrite->pTable;
  if (pTable == NULL) return TSDB_CODE_MND_SDB_INVALID_TABLE_TYPE;

  void *pRow = sdbGetRowMetaFromObj(pTable, pWrite->pRow);
  if (pRow == NULL) {
    sdbDebug("vgId:1, sdb:%s, record is not there, update failed", pTable->tableName);
    return TSDB_CODE_MND_SDB_OBJ_NOT_THERE;
  }

  int32_t code = sdbUpdateHash(pTable, pWrite);
  if (code != TSDB_CODE_SUCCESS) {
    sdbError("vgId:1, sdb:%s, failed to update hash", pTable->tableName);
    return code;
  }

  // just update data in memory
  if (pWrite->type != SDB_OPER_GLOBAL) {
    return TSDB_CODE_SUCCESS;
  }

  if (pWrite->fpReq) {
    return (*pWrite->fpReq)(pWrite->pMsg);
  } else {
    return sdbUpdateRowImp(pWrite);
  }
}

int32_t sdbUpdateRowImp(SSWriteMsg *pWrite) {
  SSdbTable *pTable = pWrite->pTable;
  if (pTable == NULL) return TSDB_CODE_MND_SDB_INVALID_TABLE_TYPE;

  int32_t size = sizeof(SSWriteMsg) + sizeof(SWalHead) + pTable->maxRowSize + SDB_SYNC_HACK;
  SSWriteMsg *pNewWrite = taosAllocateQitem(size);

  SWalHead *pHead = (SWalHead *)((void *)pNewWrite + sizeof(SSWriteMsg) + SDB_SYNC_HACK);
  pHead->version = 0;
  pHead->msgType = pTable->tableId * 10 + SDB_ACTION_UPDATE;

  pWrite->rowData = pHead->cont;
  (*pTable->fpEncode)(pWrite);
  pHead->len = pWrite->rowSize;

  memcpy(pNewWrite, pWrite, sizeof(SSWriteMsg));

  if (pNewWrite->pMsg != NULL) {
    sdbDebug("vgId:1, ahandle:%p msg:%p, sdb:%s row:%p:%s, update action is add to sdb queue", pNewWrite->pMsg->rpcMsg.ahandle,
             pNewWrite->pMsg, pTable->tableName, pWrite->pRow, sdbGetRowStr(pTable, pWrite->pRow));
  }

  sdbIncRef(pNewWrite->pTable, pNewWrite->pRow);
  taosWriteQitem(tsSdbWQueue, TAOS_QTYPE_RPC, pNewWrite);

  return TSDB_CODE_MND_ACTION_IN_PROGRESS;
}

void *sdbFetchRow(void *tparam, void *pNode, void **ppRow) {
  SSdbTable *pTable = tparam;
  *ppRow = NULL;
  if (pTable == NULL) return NULL;

  SHashMutableIterator *pIter = pNode;
  if (pIter == NULL) {
    pIter = taosHashCreateIter(pTable->iHandle);
  }

  if (!taosHashIterNext(pIter)) {
    taosHashDestroyIter(pIter);
    return NULL;
  }

  void **ppMetaRow = taosHashIterGet(pIter);
  if (ppMetaRow == NULL) {
    taosHashDestroyIter(pIter);
    return NULL;
  }

  *ppRow = *ppMetaRow;
  sdbIncRef(pTable, *ppMetaRow);

  return pIter;
}

void sdbFreeIter(void *pIter) {
  if (pIter != NULL) {
    taosHashDestroyIter(pIter);
  }
}

void *sdbOpenTable(SSdbTableDesc *pDesc) {
  SSdbTable *pTable = (SSdbTable *)calloc(1, sizeof(SSdbTable));
  
  if (pTable == NULL) return NULL;

  pthread_mutex_init(&pTable->mutex, NULL);
  tstrncpy(pTable->tableName, pDesc->tableName, SDB_TABLE_LEN);
  pTable->keyType      = pDesc->keyType;
  pTable->tableId      = pDesc->tableId;
  pTable->hashSessions = pDesc->hashSessions;
  pTable->maxRowSize   = pDesc->maxRowSize;
  pTable->refCountPos  = pDesc->refCountPos;
  pTable->fpInsert     = pDesc->fpInsert;
  pTable->fpDelete     = pDesc->fpDelete;
  pTable->fpUpdate     = pDesc->fpUpdate;
  pTable->fpEncode     = pDesc->fpEncode;
  pTable->fpDecode     = pDesc->fpDecode;
  pTable->fpDestroy    = pDesc->fpDestroy;
  pTable->fpRestored   = pDesc->fpRestored;

  _hash_fn_t hashFp = taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT);
  if (pTable->keyType == SDB_KEY_STRING || pTable->keyType == SDB_KEY_VAR_STRING) {
    hashFp = taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY);
  }
  pTable->iHandle = taosHashInit(pTable->hashSessions, hashFp, true, true);

  tsSdbMgmt.numOfTables++;
  tsSdbMgmt.tableList[pTable->tableId] = pTable;
  return pTable;
}

void sdbCloseTable(void *handle) {
  SSdbTable *pTable = (SSdbTable *)handle;
  if (pTable == NULL) return;
  
  tsSdbMgmt.numOfTables--;
  tsSdbMgmt.tableList[pTable->tableId] = NULL;

  SHashMutableIterator *pIter = taosHashCreateIter(pTable->iHandle);
  while (taosHashIterNext(pIter)) {
    void **ppRow = taosHashIterGet(pIter);
    if (ppRow == NULL) continue;

    SSWriteMsg wmsg = {
      .pRow = *ppRow,
      .pTable = pTable,
    };

    (*pTable->fpDestroy)(&wmsg);
  }

  taosHashDestroyIter(pIter);
  taosHashCleanup(pTable->iHandle);
  pthread_mutex_destroy(&pTable->mutex);

  sdbDebug("vgId:1, sdb:%s, is closed, numOfTables:%d", pTable->tableName, tsSdbMgmt.numOfTables);
  free(pTable);
}

int32_t sdbInitWorker() {
  tsSdbPool.num = 1;
  tsSdbPool.worker = calloc(sizeof(SSdbWorker), tsSdbPool.num);

  if (tsSdbPool.worker == NULL) return -1;
  for (int32_t i = 0; i < tsSdbPool.num; ++i) {
    SSdbWorker *pWorker = tsSdbPool.worker + i;
    pWorker->workerId = i;
  }

  sdbAllocQueue();
  
  mInfo("vgId:1, sdb write is opened");
  return 0;
}

void sdbCleanupWorker() {
  for (int32_t i = 0; i < tsSdbPool.num; ++i) {
    SSdbWorker *pWorker = tsSdbPool.worker + i;
    if (pWorker->thread) {
      taosQsetThreadResume(tsSdbWQset);
    }
  }

  for (int32_t i = 0; i < tsSdbPool.num; ++i) {
    SSdbWorker *pWorker = tsSdbPool.worker + i;
    if (pWorker->thread) {
      pthread_join(pWorker->thread, NULL);
    }
  }

  sdbFreeQueue();
  tfree(tsSdbPool.worker);

  mInfo("vgId:1, sdb write is closed");
}

int32_t sdbAllocQueue() {
  tsSdbWQueue = taosOpenQueue();
  if (tsSdbWQueue == NULL) return TSDB_CODE_MND_OUT_OF_MEMORY;

  tsSdbWQset = taosOpenQset();
  if (tsSdbWQset == NULL) {
    taosCloseQueue(tsSdbWQueue);
    return TSDB_CODE_MND_OUT_OF_MEMORY;
  }
  taosAddIntoQset(tsSdbWQset, tsSdbWQueue, NULL);

  tsSdbWQall = taosAllocateQall();
  if (tsSdbWQall == NULL) {
    taosCloseQset(tsSdbWQset);
    taosCloseQueue(tsSdbWQueue);
    return TSDB_CODE_MND_OUT_OF_MEMORY;
  }
  
  for (int32_t i = 0; i < tsSdbPool.num; ++i) {
    SSdbWorker *pWorker = tsSdbPool.worker + i;
    pWorker->workerId = i;

    pthread_attr_t thAttr;
    pthread_attr_init(&thAttr);
    pthread_attr_setdetachstate(&thAttr, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&pWorker->thread, &thAttr, sdbWorkerFp, pWorker) != 0) {
      mError("failed to create thread to process sdb write queue, reason:%s", strerror(errno));
      taosFreeQall(tsSdbWQall);
      taosCloseQset(tsSdbWQset);
      taosCloseQueue(tsSdbWQueue);
      return TSDB_CODE_MND_OUT_OF_MEMORY;
    }

    pthread_attr_destroy(&thAttr);
    mDebug("sdb write worker:%d is launched, total:%d", pWorker->workerId, tsSdbPool.num);
  }

  mDebug("sdb write queue:%p is allocated", tsSdbWQueue);
  return TSDB_CODE_SUCCESS;
}

void sdbFreeQueue() {
  taosCloseQueue(tsSdbWQueue);
  taosFreeQall(tsSdbWQall);
  taosCloseQset(tsSdbWQset);
  tsSdbWQall = NULL;
  tsSdbWQset = NULL;
  tsSdbWQueue = NULL;
}

int32_t sdbWriteToQueue(void *wparam, void *hparam, int32_t qtype, void *unsed) {
  SWalHead *pHead = hparam;
  int32_t   size = sizeof(SWalHead) + pHead->len;
  SWalHead *pWal = taosAllocateQitem(size);
  memcpy(pWal, pHead, size);

  taosWriteQitem(tsSdbWQueue, qtype, pWal);
  return 0;
}

static void *sdbWorkerFp(void *pWorker) {
  SWalHead *pHead;
  SSWriteMsg *pWrite;
  int32_t   qtype;
  int32_t   numOfMsgs;
  void *    item;
  void *    unUsed;

  while (1) {
    numOfMsgs = taosReadAllQitemsFromQset(tsSdbWQset, tsSdbWQall, &unUsed);
    if (numOfMsgs == 0) {
      sdbDebug("qset:%p, sdb got no message from qset, exiting", tsSdbWQset);
      break;
    }

    for (int32_t i = 0; i < numOfMsgs; ++i) {
      taosGetQitem(tsSdbWQall, &qtype, &item);
      if (qtype == TAOS_QTYPE_RPC) {
        pWrite = (SSWriteMsg *)item;
        pWrite->processedCount = 1;
        pHead = (void *)pWrite + sizeof(SSWriteMsg) + SDB_SYNC_HACK;
        if (pWrite->pMsg != NULL) {
          sdbDebug("vgId:1, ahandle:%p msg:%p, sdb:%s row:%p:%s hver:%" PRIu64 ", will be processed in sdb queue",
                   pWrite->pMsg->rpcMsg.ahandle, pWrite->pMsg, pWrite->pTable->tableName, pWrite->pRow,
                   sdbGetKeyStr(pWrite->pTable, pHead->cont), pHead->version);
        }
      } else {
        pHead = (SWalHead *)item;
        pWrite = NULL;
      }

      int32_t code = sdbWrite(pWrite, pHead, qtype, NULL);
      if (code > 0) code = 0;
      if (pWrite) {
        pWrite->code = code;
      } else {
        pHead->len = code;  // hackway
      }
    }

    walFsync(tsSdbMgmt.wal, true);

    // browse all items, and process them one by one
    taosResetQitems(tsSdbWQall);
    for (int32_t i = 0; i < numOfMsgs; ++i) {
      taosGetQitem(tsSdbWQall, &qtype, &item);

      if (qtype == TAOS_QTYPE_RPC) {
        pWrite = (SSWriteMsg *)item;
        sdbConfirmForward(NULL, pWrite, pWrite->code);
      } else if (qtype == TAOS_QTYPE_FWD) {
        pHead = (SWalHead *)item;
        syncConfirmForward(tsSdbMgmt.sync, pHead->version, pHead->len);
        taosFreeQitem(item);
      } else {
        taosFreeQitem(item);
      }
    }
  }

  return NULL;
}
