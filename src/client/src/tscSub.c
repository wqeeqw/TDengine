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

#include "os.h"
#include "taos.h"
#include "trpc.h"
#include "tsclient.h"
#include "tsocket.h"
#include "ttime.h"
#include "ttimer.h"
#include "tutil.h"
#include "tscLog.h"
#include "tscUtil.h"
#include "tcache.h"
#include "tscProfile.h"

typedef struct SSubscriptionProgress {
  int64_t uid;
  TSKEY key;
} SSubscriptionProgress;

typedef struct SSub {
  void *                  signature;
  char                    topic[32];
  int64_t                 lastSyncTime;
  int64_t                 lastConsumeTime;
  TAOS *                  taos;
  void *                  pTimer;
  SSqlObj *               pSql;
  int                     interval;
  TAOS_SUBSCRIBE_CALLBACK fp;
  void *                  param;
  SArray* progress;
} SSub;


static int tscCompareSubscriptionProgress(const void* a, const void* b) {
  const SSubscriptionProgress* x = (const SSubscriptionProgress*)a;
  const SSubscriptionProgress* y = (const SSubscriptionProgress*)b;
  if (x->uid > y->uid) return 1;
  if (x->uid < y->uid) return -1;
  return 0;
}

TSKEY tscGetSubscriptionProgress(void* sub, int64_t uid, TSKEY dflt) {
  if (sub == NULL) {
    return dflt;
  }
  SSub* pSub = (SSub*)sub;

  SSubscriptionProgress target = {.uid = uid, .key = 0};
  SSubscriptionProgress* p = taosArraySearch(pSub->progress, &target, tscCompareSubscriptionProgress);
  if (p == NULL) {
    return dflt;
  }
  return p->key;
}

void tscUpdateSubscriptionProgress(void* sub, int64_t uid, TSKEY ts) {
  if( sub == NULL)
    return;
  SSub* pSub = (SSub*)sub;

  SSubscriptionProgress target = {.uid = uid, .key = ts};
  SSubscriptionProgress* p = taosArraySearch(pSub->progress, &target, tscCompareSubscriptionProgress);
  if (p != NULL) {
    p->key = ts;
  }
}


static void asyncCallback(void *param, TAOS_RES *tres, int code) {
  assert(param != NULL);
  SSqlObj *pSql = ((SSqlObj *)param);
  
  pSql->res.code = code;
  sem_post(&pSql->rspSem);
}


static SSub* tscCreateSubscription(STscObj* pObj, const char* topic, const char* sql) {
  SSub* pSub = NULL;

  TRY( 8 ) {
    SSqlObj* pSql = calloc_throw(1, sizeof(SSqlObj));
    CLEANUP_PUSH_FREE(true, pSql);
    SSqlCmd *pCmd = &pSql->cmd;
    SSqlRes *pRes = &pSql->res;

    if (tsem_init(&pSql->rspSem, 0, 0) == -1) {
      THROW(TAOS_SYSTEM_ERROR(errno));
    }
    CLEANUP_PUSH_INT_PTR(true, tsem_destroy, &pSql->rspSem);

    pSql->signature = pSql;
    pSql->param = pSql;
    pSql->pTscObj = pObj;
    pSql->maxRetry = TSDB_MAX_REPLICA_NUM;
    pSql->fp = asyncCallback;

    int code = tscAllocPayload(pCmd, TSDB_DEFAULT_PAYLOAD_SIZE);
    if (code != TSDB_CODE_SUCCESS) {
      THROW(code);
    }
    CLEANUP_PUSH_FREE(true, pCmd->payload);

    pRes->qhandle = 0;
    pRes->numOfRows = 1;

    pSql->sqlstr = strdup_throw(sql);
    CLEANUP_PUSH_FREE(true, pSql->sqlstr);
    strtolower(pSql->sqlstr, pSql->sqlstr);

    code = tsParseSql(pSql, false);
    if (code == TSDB_CODE_TSC_ACTION_IN_PROGRESS) {
      // wait for the callback function to post the semaphore
      sem_wait(&pSql->rspSem);
      code = pSql->res.code;
    }
    if (code != TSDB_CODE_SUCCESS) {
      tscError("failed to parse sql statement: %s, error: %s", pSub->topic, tstrerror(code));
      THROW( code );
    }

    if (pSql->cmd.command != TSDB_SQL_SELECT) {
      tscError("only 'select' statement is allowed in subscription: %s", pSub->topic);
      THROW( -1 );  // TODO
    }

    pSub = calloc_throw(1, sizeof(SSub));
    CLEANUP_PUSH_FREE(true, pSub);
    pSql->pSubscription = pSub;
    pSub->pSql = pSql;
    pSub->signature = pSub;
    strncpy(pSub->topic, topic, sizeof(pSub->topic));
    pSub->topic[sizeof(pSub->topic) - 1] = 0;
    pSub->progress = taosArrayInit(32, sizeof(SSubscriptionProgress));
    if (pSub->progress == NULL) {
      THROW(TSDB_CODE_TSC_OUT_OF_MEMORY);
    }

    CLEANUP_EXECUTE();

  } CATCH( code ) {
    tscError("failed to create subscription object: %s", tstrerror(code));
    CLEANUP_EXECUTE();
    pSub = NULL;

  } END_TRY

  return pSub;
}


static void tscProcessSubscriptionTimer(void *handle, void *tmrId) {
  SSub *pSub = (SSub *)handle;
  if (pSub == NULL || pSub->pTimer != tmrId) return;

  TAOS_RES* res = taos_consume(pSub);
  if (res != NULL) {
    pSub->fp(pSub, res, pSub->param, 0);
  }

  taosTmrReset(tscProcessSubscriptionTimer, pSub->interval, pSub, tscTmr, &pSub->pTimer);
}


static SArray* getTableList( SSqlObj* pSql ) {
  const char* p = strstr( pSql->sqlstr, " from " );
  char* sql = alloca(strlen(p) + 32);
  sprintf(sql, "select tbid(tbname)%s", p);
  
  SSqlObj* pNew = taos_query(pSql->pTscObj, sql);
  if (pNew == NULL) {
    tscError("failed to retrieve table id: cannot create new sql object.");
    return NULL;

  } else if (taos_errno(pNew) != TSDB_CODE_SUCCESS) {
    tscError("failed to retrieve table id: %s", tstrerror(taos_errno(pNew)));
    return NULL;
  }

  TAOS_ROW row;
  SArray* result = taosArrayInit( 128, sizeof(STidTags) );
  while ((row = taos_fetch_row(pNew))) {
    STidTags tags;
    memcpy(&tags, row[0], sizeof(tags));
    taosArrayPush(result, &tags);
  }

  taos_free_result(pNew);
  
  return result;
}


static int tscUpdateSubscription(STscObj* pObj, SSub* pSub) {
  SSqlObj* pSql = pSub->pSql;

  SSqlCmd* pCmd = &pSql->cmd;

  pSub->lastSyncTime = taosGetTimestampMs();

  STableMetaInfo *pTableMetaInfo = tscGetTableMetaInfoFromCmd(pCmd, pCmd->clauseIndex, 0);
  if (UTIL_TABLE_IS_NORMAL_TABLE(pTableMetaInfo)) {
    STableMeta * pTableMeta = pTableMetaInfo->pTableMeta;
    SSubscriptionProgress target = {.uid = pTableMeta->uid, .key = 0};
    SSubscriptionProgress* p = taosArraySearch(pSub->progress, &target, tscCompareSubscriptionProgress);
    if (p == NULL) {
      taosArrayClear(pSub->progress);
      taosArrayPush(pSub->progress, &target);
    }
    return 1;
  }

  SArray* tables = getTableList(pSql);
  if (tables == NULL) {
    return 0;
  }
  size_t numOfTables = taosArrayGetSize(tables);

  SArray* progress = taosArrayInit(numOfTables, sizeof(SSubscriptionProgress));
  for( size_t i = 0; i < numOfTables; i++ ) {
    STidTags* tt = taosArrayGet( tables, i );
    SSubscriptionProgress p = { .uid = tt->uid };
    p.key = tscGetSubscriptionProgress(pSub, tt->uid, INT64_MIN);
    taosArrayPush(progress, &p);
  }
  taosArraySort(progress, tscCompareSubscriptionProgress);

  taosArrayDestroy(pSub->progress);
  pSub->progress = progress;

  if (UTIL_TABLE_IS_SUPER_TABLE(pTableMetaInfo)) {
    taosArraySort( tables, tscCompareTidTags );
    tscBuildVgroupTableInfo(pSql, pTableMetaInfo, tables);
  }
  taosArrayDestroy(tables);

  TSDB_QUERY_SET_TYPE(tscGetQueryInfoDetail(pCmd, 0)->type, TSDB_QUERY_TYPE_MULTITABLE_QUERY);
  return 1;
}


static int tscLoadSubscriptionProgress(SSub* pSub) {
  char buf[TSDB_MAX_SQL_LEN];
  sprintf(buf, "%s/subscribe/%s", tsDataDir, pSub->topic);

  FILE* fp = fopen(buf, "r");
  if (fp == NULL) {
    tscTrace("subscription progress file does not exist: %s", pSub->topic);
    return 1;
  }

  if (fgets(buf, sizeof(buf), fp) == NULL) {
    tscTrace("invalid subscription progress file: %s", pSub->topic);
    fclose(fp);
    return 0;
  }

  for (int i = 0; i < sizeof(buf); i++) {
    if (buf[i] == 0)
      break;
    if (buf[i] == '\r' || buf[i] == '\n') {
      buf[i] = 0;
      break;
    }
  }
  if (strcmp(buf, pSub->pSql->sqlstr) != 0) {
    tscTrace("subscription sql statement mismatch: %s", pSub->topic);
    fclose(fp);
    return 0;
  }

  SArray* progress = pSub->progress;
  taosArrayClear(progress);
  while( 1 ) {
    if (fgets(buf, sizeof(buf), fp) == NULL) {
      fclose(fp);
      return 0;
    }
    SSubscriptionProgress p;
    sscanf(buf, "%" SCNd64 ":%" SCNd64, &p.uid, &p.key);
    taosArrayPush(progress, &p);
  }

  fclose(fp);

  taosArraySort(progress, tscCompareSubscriptionProgress);
  tscTrace("subscription progress loaded, %zu tables: %s", taosArrayGetSize(progress), pSub->topic);
  return 1;
}

void tscSaveSubscriptionProgress(void* sub) {
  SSub* pSub = (SSub*)sub;

  char path[256];
  sprintf(path, "%s/subscribe", tsDataDir);
  if (tmkdir(path, 0777) != 0) {
    tscError("failed to create subscribe dir: %s", path);
  }

  sprintf(path, "%s/subscribe/%s", tsDataDir, pSub->topic);
  FILE* fp = fopen(path, "w+");
  if (fp == NULL) {
    tscError("failed to create progress file for subscription: %s", pSub->topic);
    return;
  }

  fputs(pSub->pSql->sqlstr, fp);
  fprintf(fp, "\n");
  for(size_t i = 0; i < taosArrayGetSize(pSub->progress); i++) {
    SSubscriptionProgress* p = taosArrayGet(pSub->progress, i);
    fprintf(fp, "%" PRId64 ":%" PRId64 "\n", p->uid, p->key);
  }

  fclose(fp);
}

TAOS_SUB *taos_subscribe(TAOS *taos, int restart, const char* topic, const char *sql, TAOS_SUBSCRIBE_CALLBACK fp, void *param, int interval) {
  STscObj* pObj = (STscObj*)taos;
  if (pObj == NULL || pObj->signature != pObj) {
    terrno = TSDB_CODE_TSC_DISCONNECTED;
    tscError("connection disconnected");
    return NULL;
  }

  SSub* pSub = tscCreateSubscription(pObj, topic, sql);
  if (pSub == NULL) {
    return NULL;
  }
  pSub->taos = taos;

  if (restart) {
    tscTrace("restart subscription: %s", topic);
  } else {
    tscLoadSubscriptionProgress(pSub);
  }

  if (!tscUpdateSubscription(pObj, pSub)) {
    taos_unsubscribe(pSub, 1);
    return NULL;
  }

  pSub->interval = interval;
  if (fp != NULL) {
    tscTrace("asynchronize subscription, create new timer: %s", topic);
    pSub->fp = fp;
    pSub->param = param;
    taosTmrReset(tscProcessSubscriptionTimer, interval, pSub, tscTmr, &pSub->pTimer);
  }

  return pSub;
}

void taos_free_result_imp(SSqlObj* pSql, int keepCmd);

TAOS_RES *taos_consume(TAOS_SUB *tsub) {
  SSub *pSub = (SSub *)tsub;
  if (pSub == NULL) return NULL;

  tscSaveSubscriptionProgress(pSub);

  SSqlObj* pSql = pSub->pSql;
  SSqlRes *pRes = &pSql->res;

  if (pSub->pTimer == NULL) {
    int64_t duration = taosGetTimestampMs() - pSub->lastConsumeTime;
    if (duration < (int64_t)(pSub->interval)) {
      tscTrace("subscription consume too frequently, blocking...");
      taosMsleep(pSub->interval - (int32_t)duration);
    }
  }

  for (int retry = 0; retry < 3; retry++) {
    tscRemoveFromSqlList(pSql);

    if (taosGetTimestampMs() - pSub->lastSyncTime > 10 * 60 * 1000) {
      tscTrace("begin table synchronization");
      if (!tscUpdateSubscription(pSub->taos, pSub)) return NULL;
      tscTrace("table synchronization completed");
    }

    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
    
    uint32_t type = pQueryInfo->type;
    tscFreeSqlResult(pSql);
    pRes->numOfRows = 1;
    pRes->qhandle = 0;
    pSql->cmd.command = TSDB_SQL_SELECT;
    pQueryInfo->type = type;

    tscGetTableMetaInfoFromCmd(&pSql->cmd, 0, 0)->vgroupIndex = 0;

    pSql->fp = asyncCallback;
    pSql->param = pSql;
    tscDoQuery(pSql);
    sem_wait(&pSql->rspSem);

    if (pRes->code != TSDB_CODE_SUCCESS) {
      continue;
    }
    // meter was removed, make sync time zero, so that next retry will
    // do synchronization first
    pSub->lastSyncTime = 0;
    break;
  }

  if (pRes->code != TSDB_CODE_SUCCESS) {
    tscError("failed to query data: %s", tstrerror(pRes->code));
    tscRemoveFromSqlList(pSql);
    return NULL;
  }

  pSub->lastConsumeTime = taosGetTimestampMs();
  return pSql;
}

void taos_unsubscribe(TAOS_SUB *tsub, int keepProgress) {
  SSub *pSub = (SSub *)tsub;
  if (pSub == NULL || pSub->signature != pSub) return;

  if (pSub->pTimer != NULL) {
    taosTmrStop(pSub->pTimer);
  }

  if (keepProgress) {
    tscSaveSubscriptionProgress(pSub);
  } else {
    char path[256];
    sprintf(path, "%s/subscribe/%s", tsDataDir, pSub->topic);
    if (remove(path) != 0) {
      tscError("failed to remove progress file, topic = %s, error = %s", pSub->topic, strerror(errno));
    }
  }

  tscFreeSqlObj(pSub->pSql);
  taosArrayDestroy(pSub->progress);
  memset(pSub, 0, sizeof(*pSub));
  free(pSub);
}
