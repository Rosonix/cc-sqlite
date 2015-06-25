/*
** 2014 Jun 09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This is an SQLite module implementing full-text search.
*/

#if defined(SQLITE_ENABLE_FTS5)

#include "fts5Int.h"

/*
** This variable is set to false when running tests for which the on disk
** structures should not be corrupt. Otherwise, true. If it is false, extra
** assert() conditions in the fts5 code are activated - conditions that are
** only true if it is guaranteed that the fts5 database is not corrupt.
*/
int sqlite3_fts5_may_be_corrupt = 1;


typedef struct Fts5Table Fts5Table;
typedef struct Fts5Cursor Fts5Cursor;
typedef struct Fts5Global Fts5Global;
typedef struct Fts5Auxiliary Fts5Auxiliary;
typedef struct Fts5Auxdata Fts5Auxdata;

typedef struct Fts5TokenizerModule Fts5TokenizerModule;

/*
** NOTES ON TRANSACTIONS: 
**
** SQLite invokes the following virtual table methods as transactions are 
** opened and closed by the user:
**
**     xBegin():    Start of a new transaction.
**     xSync():     Initial part of two-phase commit.
**     xCommit():   Final part of two-phase commit.
**     xRollback(): Rollback the transaction.
**
** Anything that is required as part of a commit that may fail is performed
** in the xSync() callback. Current versions of SQLite ignore any errors 
** returned by xCommit().
**
** And as sub-transactions are opened/closed:
**
**     xSavepoint(int S):  Open savepoint S.
**     xRelease(int S):    Commit and close savepoint S.
**     xRollbackTo(int S): Rollback to start of savepoint S.
**
** During a write-transaction the fts5_index.c module may cache some data 
** in-memory. It is flushed to disk whenever xSync(), xRelease() or
** xSavepoint() is called. And discarded whenever xRollback() or xRollbackTo() 
** is called.
**
** Additionally, if SQLITE_DEBUG is defined, an instance of the following
** structure is used to record the current transaction state. This information
** is not required, but it is used in the assert() statements executed by
** function fts5CheckTransactionState() (see below).
*/
struct Fts5TransactionState {
  int eState;                     /* 0==closed, 1==open, 2==synced */
  int iSavepoint;                 /* Number of open savepoints (0 -> none) */
};

/*
** A single object of this type is allocated when the FTS5 module is 
** registered with a database handle. It is used to store pointers to
** all registered FTS5 extensions - tokenizers and auxiliary functions.
*/
struct Fts5Global {
  fts5_api api;                   /* User visible part of object (see fts5.h) */
  sqlite3 *db;                    /* Associated database connection */ 
  i64 iNextId;                    /* Used to allocate unique cursor ids */
  Fts5Auxiliary *pAux;            /* First in list of all aux. functions */
  Fts5TokenizerModule *pTok;      /* First in list of all tokenizer modules */
  Fts5TokenizerModule *pDfltTok;  /* Default tokenizer module */
  Fts5Cursor *pCsr;               /* First in list of all open cursors */
};

/*
** Each auxiliary function registered with the FTS5 module is represented
** by an object of the following type. All such objects are stored as part
** of the Fts5Global.pAux list.
*/
struct Fts5Auxiliary {
  Fts5Global *pGlobal;            /* Global context for this function */
  char *zFunc;                    /* Function name (nul-terminated) */
  void *pUserData;                /* User-data pointer */
  fts5_extension_function xFunc;  /* Callback function */
  void (*xDestroy)(void*);        /* Destructor function */
  Fts5Auxiliary *pNext;           /* Next registered auxiliary function */
};

/*
** Each tokenizer module registered with the FTS5 module is represented
** by an object of the following type. All such objects are stored as part
** of the Fts5Global.pTok list.
*/
struct Fts5TokenizerModule {
  char *zName;                    /* Name of tokenizer */
  void *pUserData;                /* User pointer passed to xCreate() */
  fts5_tokenizer x;               /* Tokenizer functions */
  void (*xDestroy)(void*);        /* Destructor function */
  Fts5TokenizerModule *pNext;     /* Next registered tokenizer module */
};

/*
** Virtual-table object.
*/
struct Fts5Table {
  sqlite3_vtab base;              /* Base class used by SQLite core */
  Fts5Config *pConfig;            /* Virtual table configuration */
  Fts5Index *pIndex;              /* Full-text index */
  Fts5Storage *pStorage;          /* Document store */
  Fts5Global *pGlobal;            /* Global (connection wide) data */
  Fts5Cursor *pSortCsr;           /* Sort data from this cursor */
#ifdef SQLITE_DEBUG
  struct Fts5TransactionState ts;
#endif
};

struct Fts5MatchPhrase {
  Fts5Buffer *pPoslist;           /* Pointer to current poslist */
  int nTerm;                      /* Size of phrase in terms */
};

/*
** pStmt:
**   SELECT rowid, <fts> FROM <fts> ORDER BY +rank;
**
** aIdx[]:
**   There is one entry in the aIdx[] array for each phrase in the query,
**   the value of which is the offset within aPoslist[] following the last 
**   byte of the position list for the corresponding phrase.
*/
struct Fts5Sorter {
  sqlite3_stmt *pStmt;
  i64 iRowid;                     /* Current rowid */
  const u8 *aPoslist;             /* Position lists for current row */
  int nIdx;                       /* Number of entries in aIdx[] */
  int aIdx[0];                    /* Offsets into aPoslist for current row */
};


/*
** Virtual-table cursor object.
**
** iSpecial:
**   If this is a 'special' query (refer to function fts5SpecialMatch()), 
**   then this variable contains the result of the query. 
**
** iFirstRowid, iLastRowid:
**   These variables are only used for FTS5_PLAN_MATCH cursors. Assuming the
**   cursor iterates in ascending order of rowids, iFirstRowid is the lower
**   limit of rowids to return, and iLastRowid the upper. In other words, the
**   WHERE clause in the user's query might have been:
**
**       <tbl> MATCH <expr> AND rowid BETWEEN $iFirstRowid AND $iLastRowid
**
**   If the cursor iterates in descending order of rowid, iFirstRowid
**   is the upper limit (i.e. the "first" rowid visited) and iLastRowid
**   the lower.
*/
struct Fts5Cursor {
  sqlite3_vtab_cursor base;       /* Base class used by SQLite core */
  int ePlan;                      /* FTS5_PLAN_XXX value */
  int bDesc;                      /* True for "ORDER BY rowid DESC" queries */
  i64 iFirstRowid;                /* Return no rowids earlier than this */
  i64 iLastRowid;                 /* Return no rowids later than this */
  sqlite3_stmt *pStmt;            /* Statement used to read %_content */
  Fts5Expr *pExpr;                /* Expression for MATCH queries */
  Fts5Sorter *pSorter;            /* Sorter for "ORDER BY rank" queries */
  int csrflags;                   /* Mask of cursor flags (see below) */
  Fts5Cursor *pNext;              /* Next cursor in Fts5Cursor.pCsr list */
  i64 iSpecial;                   /* Result of special query */

  /* "rank" function. Populated on demand from vtab.xColumn(). */
  char *zRank;                    /* Custom rank function */
  char *zRankArgs;                /* Custom rank function args */
  Fts5Auxiliary *pRank;           /* Rank callback (or NULL) */
  int nRankArg;                   /* Number of trailing arguments for rank() */
  sqlite3_value **apRankArg;      /* Array of trailing arguments */
  sqlite3_stmt *pRankArgStmt;     /* Origin of objects in apRankArg[] */

  /* Variables used by auxiliary functions */
  i64 iCsrId;                     /* Cursor id */
  Fts5Auxiliary *pAux;            /* Currently executing extension function */
  Fts5Auxdata *pAuxdata;          /* First in linked list of saved aux-data */
  int *aColumnSize;               /* Values for xColumnSize() */

  /* Cache used by auxiliary functions xInst() and xInstCount() */
  int nInstCount;                 /* Number of phrase instances */
  int *aInst;                     /* 3 integers per phrase instance */
};

/*
** Bits that make up the "idxNum" parameter passed indirectly by 
** xBestIndex() to xFilter().
*/
#define FTS5_BI_MATCH        0x0001         /* <tbl> MATCH ? */
#define FTS5_BI_RANK         0x0002         /* rank MATCH ? */
#define FTS5_BI_ROWID_EQ     0x0004         /* rowid == ? */
#define FTS5_BI_ROWID_LE     0x0008         /* rowid <= ? */
#define FTS5_BI_ROWID_GE     0x0010         /* rowid >= ? */

#define FTS5_BI_ORDER_RANK   0x0020
#define FTS5_BI_ORDER_ROWID  0x0040
#define FTS5_BI_ORDER_DESC   0x0080

/*
** Values for Fts5Cursor.csrflags
*/
#define FTS5CSR_REQUIRE_CONTENT   0x01
#define FTS5CSR_REQUIRE_DOCSIZE   0x02
#define FTS5CSR_EOF               0x04
#define FTS5CSR_FREE_ZRANK        0x08
#define FTS5CSR_REQUIRE_RESEEK    0x10

#define BitFlagAllTest(x,y) (((x) & (y))==(y))
#define BitFlagTest(x,y)    (((x) & (y))!=0)

/*
** Constants for the largest and smallest possible 64-bit signed integers.
** These are copied from sqliteInt.h.
*/
#ifndef SQLITE_AMALGAMATION
# define LARGEST_INT64  (0xffffffff|(((i64)0x7fffffff)<<32))
# define SMALLEST_INT64 (((i64)-1) - LARGEST_INT64)
#endif

/*
** Macros to Set(), Clear() and Test() cursor flags.
*/
#define CsrFlagSet(pCsr, flag)   ((pCsr)->csrflags |= (flag))
#define CsrFlagClear(pCsr, flag) ((pCsr)->csrflags &= ~(flag))
#define CsrFlagTest(pCsr, flag)  ((pCsr)->csrflags & (flag))

struct Fts5Auxdata {
  Fts5Auxiliary *pAux;            /* Extension to which this belongs */
  void *pPtr;                     /* Pointer value */
  void(*xDelete)(void*);          /* Destructor */
  Fts5Auxdata *pNext;             /* Next object in linked list */
};

#ifdef SQLITE_DEBUG
#define FTS5_BEGIN      1
#define FTS5_SYNC       2
#define FTS5_COMMIT     3
#define FTS5_ROLLBACK   4
#define FTS5_SAVEPOINT  5
#define FTS5_RELEASE    6
#define FTS5_ROLLBACKTO 7
static void fts5CheckTransactionState(Fts5Table *p, int op, int iSavepoint){
  switch( op ){
    case FTS5_BEGIN:
      assert( p->ts.eState==0 );
      p->ts.eState = 1;
      p->ts.iSavepoint = -1;
      break;

    case FTS5_SYNC:
      assert( p->ts.eState==1 );
      p->ts.eState = 2;
      break;

    case FTS5_COMMIT:
      assert( p->ts.eState==2 );
      p->ts.eState = 0;
      break;

    case FTS5_ROLLBACK:
      assert( p->ts.eState==1 || p->ts.eState==2 || p->ts.eState==0 );
      p->ts.eState = 0;
      break;

    case FTS5_SAVEPOINT:
      assert( p->ts.eState==1 );
      assert( iSavepoint>=0 );
      assert( iSavepoint>p->ts.iSavepoint );
      p->ts.iSavepoint = iSavepoint;
      break;
      
    case FTS5_RELEASE:
      assert( p->ts.eState==1 );
      assert( iSavepoint>=0 );
      assert( iSavepoint<=p->ts.iSavepoint );
      p->ts.iSavepoint = iSavepoint-1;
      break;

    case FTS5_ROLLBACKTO:
      assert( p->ts.eState==1 );
      assert( iSavepoint>=0 );
      assert( iSavepoint<=p->ts.iSavepoint );
      p->ts.iSavepoint = iSavepoint;
      break;
  }
}
#else
# define fts5CheckTransactionState(x,y,z)
#endif

/*
** Return true if pTab is a contentless table.
*/
static int fts5IsContentless(Fts5Table *pTab){
  return pTab->pConfig->eContent==FTS5_CONTENT_NONE;
}

/*
** Delete a virtual table handle allocated by fts5InitVtab(). 
*/
static void fts5FreeVtab(Fts5Table *pTab){
  if( pTab ){
    sqlite3Fts5IndexClose(pTab->pIndex);
    sqlite3Fts5StorageClose(pTab->pStorage);
    sqlite3Fts5ConfigFree(pTab->pConfig);
    sqlite3_free(pTab);
  }
}

/*
** The xDisconnect() virtual table method.
*/
static int fts5DisconnectMethod(sqlite3_vtab *pVtab){
  fts5FreeVtab((Fts5Table*)pVtab);
  return SQLITE_OK;
}

/*
** The xDestroy() virtual table method.
*/
static int fts5DestroyMethod(sqlite3_vtab *pVtab){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  int rc = sqlite3Fts5DropAll(pTab->pConfig);
  if( rc==SQLITE_OK ){
    fts5FreeVtab((Fts5Table*)pVtab);
  }
  return rc;
}

/*
** This function is the implementation of both the xConnect and xCreate
** methods of the FTS3 virtual table.
**
** The argv[] array contains the following:
**
**   argv[0]   -> module name  ("fts5")
**   argv[1]   -> database name
**   argv[2]   -> table name
**   argv[...] -> "column name" and other module argument fields.
*/
static int fts5InitVtab(
  int bCreate,                    /* True for xCreate, false for xConnect */
  sqlite3 *db,                    /* The SQLite database connection */
  void *pAux,                     /* Hash table containing tokenizers */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVTab,          /* Write the resulting vtab structure here */
  char **pzErr                    /* Write any error message here */
){
  Fts5Global *pGlobal = (Fts5Global*)pAux;
  const char **azConfig = (const char**)argv;
  int rc = SQLITE_OK;             /* Return code */
  Fts5Config *pConfig;            /* Results of parsing argc/argv */
  Fts5Table *pTab = 0;            /* New virtual table object */

  /* Allocate the new vtab object and parse the configuration */
  pTab = (Fts5Table*)sqlite3Fts5MallocZero(&rc, sizeof(Fts5Table));
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5ConfigParse(pGlobal, db, argc, azConfig, &pConfig, pzErr);
    assert( (rc==SQLITE_OK && *pzErr==0) || pConfig==0 );
  }
  if( rc==SQLITE_OK ){
    pTab->pConfig = pConfig;
    pTab->pGlobal = pGlobal;
  }

  /* Open the index sub-system */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5IndexOpen(pConfig, bCreate, &pTab->pIndex, pzErr);
  }

  /* Open the storage sub-system */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5StorageOpen(
        pConfig, pTab->pIndex, bCreate, &pTab->pStorage, pzErr
    );
  }

  /* Call sqlite3_declare_vtab() */
  if( rc==SQLITE_OK ){
    rc = sqlite3Fts5ConfigDeclareVtab(pConfig);
  }

  if( rc!=SQLITE_OK ){
    fts5FreeVtab(pTab);
    pTab = 0;
  }else if( bCreate ){
    fts5CheckTransactionState(pTab, FTS5_BEGIN, 0);
  }
  *ppVTab = (sqlite3_vtab*)pTab;
  return rc;
}

/*
** The xConnect() and xCreate() methods for the virtual table. All the
** work is done in function fts5InitVtab().
*/
static int fts5ConnectMethod(
  sqlite3 *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  return fts5InitVtab(0, db, pAux, argc, argv, ppVtab, pzErr);
}
static int fts5CreateMethod(
  sqlite3 *db,                    /* Database connection */
  void *pAux,                     /* Pointer to tokenizer hash table */
  int argc,                       /* Number of elements in argv array */
  const char * const *argv,       /* xCreate/xConnect argument array */
  sqlite3_vtab **ppVtab,          /* OUT: New sqlite3_vtab object */
  char **pzErr                    /* OUT: sqlite3_malloc'd error message */
){
  return fts5InitVtab(1, db, pAux, argc, argv, ppVtab, pzErr);
}

/*
** The different query plans.
*/
#define FTS5_PLAN_SCAN           1       /* No usable constraint */
#define FTS5_PLAN_MATCH          2       /* (<tbl> MATCH ?) */
#define FTS5_PLAN_SORTED_MATCH   3       /* (<tbl> MATCH ? ORDER BY rank) */
#define FTS5_PLAN_ROWID          4       /* (rowid = ?) */
#define FTS5_PLAN_SOURCE         5       /* A source cursor for SORTED_MATCH */
#define FTS5_PLAN_SPECIAL        6       /* An internal query */

/*
** Implementation of the xBestIndex method for FTS5 tables. Within the 
** WHERE constraint, it searches for the following:
**
**   1. A MATCH constraint against the special column.
**   2. A MATCH constraint against the "rank" column.
**   3. An == constraint against the rowid column.
**   4. A < or <= constraint against the rowid column.
**   5. A > or >= constraint against the rowid column.
**
** Within the ORDER BY, either:
**
**   5. ORDER BY rank [ASC|DESC]
**   6. ORDER BY rowid [ASC|DESC]
**
** Costs are assigned as follows:
**
**  a) If an unusable MATCH operator is present in the WHERE clause, the
**     cost is unconditionally set to 1e50 (a really big number).
**
**  a) If a MATCH operator is present, the cost depends on the other
**     constraints also present. As follows:
**
**       * No other constraints:         cost=1000.0
**       * One rowid range constraint:   cost=750.0
**       * Both rowid range constraints: cost=500.0
**       * An == rowid constraint:       cost=100.0
**
**  b) Otherwise, if there is no MATCH:
**
**       * No other constraints:         cost=1000000.0
**       * One rowid range constraint:   cost=750000.0
**       * Both rowid range constraints: cost=250000.0
**       * An == rowid constraint:       cost=10.0
**
** Costs are not modified by the ORDER BY clause.
*/
static int fts5BestIndexMethod(sqlite3_vtab *pVTab, sqlite3_index_info *pInfo){
  Fts5Table *pTab = (Fts5Table*)pVTab;
  Fts5Config *pConfig = pTab->pConfig;
  int idxFlags = 0;               /* Parameter passed through to xFilter() */
  int bHasMatch;
  int iNext;
  int i;

  struct Constraint {
    int op;                       /* Mask against sqlite3_index_constraint.op */
    int fts5op;                   /* FTS5 mask for idxFlags */
    int iCol;                     /* 0==rowid, 1==tbl, 2==rank */
    int omit;                     /* True to omit this if found */
    int iConsIndex;               /* Index in pInfo->aConstraint[] */
  } aConstraint[] = {
    {SQLITE_INDEX_CONSTRAINT_MATCH, FTS5_BI_MATCH,    1, 1, -1},
    {SQLITE_INDEX_CONSTRAINT_MATCH, FTS5_BI_RANK,     2, 1, -1},
    {SQLITE_INDEX_CONSTRAINT_EQ,    FTS5_BI_ROWID_EQ, 0, 0, -1},
    {SQLITE_INDEX_CONSTRAINT_LT|SQLITE_INDEX_CONSTRAINT_LE, 
                                    FTS5_BI_ROWID_LE, 0, 0, -1},
    {SQLITE_INDEX_CONSTRAINT_GT|SQLITE_INDEX_CONSTRAINT_GE, 
                                    FTS5_BI_ROWID_GE, 0, 0, -1},
  };

  int aColMap[3];
  aColMap[0] = -1;
  aColMap[1] = pConfig->nCol;
  aColMap[2] = pConfig->nCol+1;

  /* Set idxFlags flags for all WHERE clause terms that will be used. */
  for(i=0; i<pInfo->nConstraint; i++){
    struct sqlite3_index_constraint *p = &pInfo->aConstraint[i];
    int j;
    for(j=0; j<sizeof(aConstraint)/sizeof(aConstraint[0]); j++){
      struct Constraint *pC = &aConstraint[j];
      if( p->iColumn==aColMap[pC->iCol] && p->op & pC->op ){
        if( p->usable ){
          pC->iConsIndex = i;
          idxFlags |= pC->fts5op;
        }else if( j==0 ){
          /* As there exists an unusable MATCH constraint this is an 
          ** unusable plan. Set a prohibitively high cost. */
          pInfo->estimatedCost = 1e50;
          return SQLITE_OK;
        }
      }
    }
  }

  /* Set idxFlags flags for the ORDER BY clause */
  if( pInfo->nOrderBy==1 ){
    int iSort = pInfo->aOrderBy[0].iColumn;
    if( iSort==(pConfig->nCol+1) && BitFlagTest(idxFlags, FTS5_BI_MATCH) ){
      idxFlags |= FTS5_BI_ORDER_RANK;
    }else if( iSort==-1 ){
      idxFlags |= FTS5_BI_ORDER_ROWID;
    }
    if( BitFlagTest(idxFlags, FTS5_BI_ORDER_RANK|FTS5_BI_ORDER_ROWID) ){
      pInfo->orderByConsumed = 1;
      if( pInfo->aOrderBy[0].desc ){
        idxFlags |= FTS5_BI_ORDER_DESC;
      }
    }
  }

  /* Calculate the estimated cost based on the flags set in idxFlags. */
  bHasMatch = BitFlagTest(idxFlags, FTS5_BI_MATCH);
  if( BitFlagTest(idxFlags, FTS5_BI_ROWID_EQ) ){
    pInfo->estimatedCost = bHasMatch ? 100.0 : 10.0;
  }else if( BitFlagAllTest(idxFlags, FTS5_BI_ROWID_LE|FTS5_BI_ROWID_GE) ){
    pInfo->estimatedCost = bHasMatch ? 500.0 : 250000.0;
  }else if( BitFlagTest(idxFlags, FTS5_BI_ROWID_LE|FTS5_BI_ROWID_GE) ){
    pInfo->estimatedCost = bHasMatch ? 750.0 : 750000.0;
  }else{
    pInfo->estimatedCost = bHasMatch ? 1000.0 : 1000000.0;
  }

  /* Assign argvIndex values to each constraint in use. */
  iNext = 1;
  for(i=0; i<sizeof(aConstraint)/sizeof(aConstraint[0]); i++){
    struct Constraint *pC = &aConstraint[i];
    if( pC->iConsIndex>=0 ){
      pInfo->aConstraintUsage[pC->iConsIndex].argvIndex = iNext++;
      pInfo->aConstraintUsage[pC->iConsIndex].omit = pC->omit;
    }
  }

  pInfo->idxNum = idxFlags;
  return SQLITE_OK;
}

/*
** Implementation of xOpen method.
*/
static int fts5OpenMethod(sqlite3_vtab *pVTab, sqlite3_vtab_cursor **ppCsr){
  Fts5Table *pTab = (Fts5Table*)pVTab;
  Fts5Config *pConfig = pTab->pConfig;
  Fts5Cursor *pCsr;               /* New cursor object */
  int nByte;                      /* Bytes of space to allocate */
  int rc = SQLITE_OK;             /* Return code */

  nByte = sizeof(Fts5Cursor) + pConfig->nCol * sizeof(int);
  pCsr = (Fts5Cursor*)sqlite3_malloc(nByte);
  if( pCsr ){
    Fts5Global *pGlobal = pTab->pGlobal;
    memset(pCsr, 0, nByte);
    pCsr->aColumnSize = (int*)&pCsr[1];
    pCsr->pNext = pGlobal->pCsr;
    pGlobal->pCsr = pCsr;
    pCsr->iCsrId = ++pGlobal->iNextId;
  }else{
    rc = SQLITE_NOMEM;
  }
  *ppCsr = (sqlite3_vtab_cursor*)pCsr;
  return rc;
}

static int fts5StmtType(Fts5Cursor *pCsr){
  if( pCsr->ePlan==FTS5_PLAN_SCAN ){
    return (pCsr->bDesc) ? FTS5_STMT_SCAN_DESC : FTS5_STMT_SCAN_ASC;
  }
  return FTS5_STMT_LOOKUP;
}

/*
** This function is called after the cursor passed as the only argument
** is moved to point at a different row. It clears all cached data 
** specific to the previous row stored by the cursor object.
*/
static void fts5CsrNewrow(Fts5Cursor *pCsr){
  CsrFlagSet(pCsr, FTS5CSR_REQUIRE_CONTENT | FTS5CSR_REQUIRE_DOCSIZE );
  sqlite3_free(pCsr->aInst);
  pCsr->aInst = 0;
  pCsr->nInstCount = 0;
}

/*
** Close the cursor.  For additional information see the documentation
** on the xClose method of the virtual table interface.
*/
static int fts5CloseMethod(sqlite3_vtab_cursor *pCursor){
  if( pCursor ){
    Fts5Table *pTab = (Fts5Table*)(pCursor->pVtab);
    Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
    Fts5Cursor **pp;
    Fts5Auxdata *pData;
    Fts5Auxdata *pNext;

    fts5CsrNewrow(pCsr);
    if( pCsr->pStmt ){
      int eStmt = fts5StmtType(pCsr);
      sqlite3Fts5StorageStmtRelease(pTab->pStorage, eStmt, pCsr->pStmt);
    }
    if( pCsr->pSorter ){
      Fts5Sorter *pSorter = pCsr->pSorter;
      sqlite3_finalize(pSorter->pStmt);
      sqlite3_free(pSorter);
    }

    if( pCsr->ePlan!=FTS5_PLAN_SOURCE ){
      sqlite3Fts5ExprFree(pCsr->pExpr);
    }

    for(pData=pCsr->pAuxdata; pData; pData=pNext){
      pNext = pData->pNext;
      if( pData->xDelete ) pData->xDelete(pData->pPtr);
      sqlite3_free(pData);
    }

    /* Remove the cursor from the Fts5Global.pCsr list */
    for(pp=&pTab->pGlobal->pCsr; (*pp)!=pCsr; pp=&(*pp)->pNext);
    *pp = pCsr->pNext;

    sqlite3_finalize(pCsr->pRankArgStmt);
    sqlite3_free(pCsr->apRankArg);

    if( CsrFlagTest(pCsr, FTS5CSR_FREE_ZRANK) ){
      sqlite3_free(pCsr->zRank);
      sqlite3_free(pCsr->zRankArgs);
    }
    sqlite3_free(pCsr);
  }
  return SQLITE_OK;
}

static int fts5SorterNext(Fts5Cursor *pCsr){
  Fts5Sorter *pSorter = pCsr->pSorter;
  int rc;

  rc = sqlite3_step(pSorter->pStmt);
  if( rc==SQLITE_DONE ){
    rc = SQLITE_OK;
    CsrFlagSet(pCsr, FTS5CSR_EOF);
  }else if( rc==SQLITE_ROW ){
    const u8 *a;
    const u8 *aBlob;
    int nBlob;
    int i;
    int iOff = 0;
    rc = SQLITE_OK;

    pSorter->iRowid = sqlite3_column_int64(pSorter->pStmt, 0);
    nBlob = sqlite3_column_bytes(pSorter->pStmt, 1);
    aBlob = a = sqlite3_column_blob(pSorter->pStmt, 1);

    for(i=0; i<(pSorter->nIdx-1); i++){
      int iVal;
      a += fts5GetVarint32(a, iVal);
      iOff += iVal;
      pSorter->aIdx[i] = iOff;
    }
    pSorter->aIdx[i] = &aBlob[nBlob] - a;

    pSorter->aPoslist = a;
    fts5CsrNewrow(pCsr);
  }

  return rc;
}


/*
** Set the FTS5CSR_REQUIRE_RESEEK flag on all FTS5_PLAN_MATCH cursors 
** open on table pTab.
*/
static void fts5TripCursors(Fts5Table *pTab){
  Fts5Cursor *pCsr;
  for(pCsr=pTab->pGlobal->pCsr; pCsr; pCsr=pCsr->pNext){
    if( pCsr->ePlan==FTS5_PLAN_MATCH
     && pCsr->base.pVtab==(sqlite3_vtab*)pTab 
    ){
      CsrFlagSet(pCsr, FTS5CSR_REQUIRE_RESEEK);
    }
  }
}

/*
** If the REQUIRE_RESEEK flag is set on the cursor passed as the first
** argument, close and reopen all Fts5IndexIter iterators that the cursor 
** is using. Then attempt to move the cursor to a rowid equal to or laster
** (in the cursors sort order - ASC or DESC) than the current rowid. 
**
** If the new rowid is not equal to the old, set output parameter *pbSkip
** to 1 before returning. Otherwise, leave it unchanged.
**
** Return SQLITE_OK if successful or if no reseek was required, or an 
** error code if an error occurred.
*/
static int fts5CursorReseek(Fts5Cursor *pCsr, int *pbSkip){
  int rc = SQLITE_OK;
  assert( *pbSkip==0 );
  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_RESEEK) ){
    Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
    int bDesc = pCsr->bDesc;
    i64 iRowid = sqlite3Fts5ExprRowid(pCsr->pExpr);

    rc = sqlite3Fts5ExprFirst(pCsr->pExpr, pTab->pIndex, iRowid, bDesc);
    if( rc==SQLITE_OK && iRowid!=sqlite3Fts5ExprRowid(pCsr->pExpr) ){
      *pbSkip = 1;
    }

    CsrFlagClear(pCsr, FTS5CSR_REQUIRE_RESEEK);
    fts5CsrNewrow(pCsr);
    if( sqlite3Fts5ExprEof(pCsr->pExpr) ){
      CsrFlagSet(pCsr, FTS5CSR_EOF);
    }
  }
  return rc;
}


/*
** Advance the cursor to the next row in the table that matches the 
** search criteria.
**
** Return SQLITE_OK if nothing goes wrong.  SQLITE_OK is returned
** even if we reach end-of-file.  The fts5EofMethod() will be called
** subsequently to determine whether or not an EOF was hit.
*/
static int fts5NextMethod(sqlite3_vtab_cursor *pCursor){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int ePlan = pCsr->ePlan;
  int bSkip = 0;
  int rc;

  if( (rc = fts5CursorReseek(pCsr, &bSkip)) || bSkip ) return rc;

  switch( ePlan ){
    case FTS5_PLAN_MATCH:
    case FTS5_PLAN_SOURCE:
      rc = sqlite3Fts5ExprNext(pCsr->pExpr, pCsr->iLastRowid);
      if( sqlite3Fts5ExprEof(pCsr->pExpr) ){
        CsrFlagSet(pCsr, FTS5CSR_EOF);
      }
      fts5CsrNewrow(pCsr);
      break;

    case FTS5_PLAN_SPECIAL: {
      CsrFlagSet(pCsr, FTS5CSR_EOF);
      break;
    }

    case FTS5_PLAN_SORTED_MATCH: {
      rc = fts5SorterNext(pCsr);
      break;
    }

    default:
      rc = sqlite3_step(pCsr->pStmt);
      if( rc!=SQLITE_ROW ){
        CsrFlagSet(pCsr, FTS5CSR_EOF);
        rc = sqlite3_reset(pCsr->pStmt);
      }else{
        rc = SQLITE_OK;
      }
      break;
  }
  
  return rc;
}

static int fts5CursorFirstSorted(Fts5Table *pTab, Fts5Cursor *pCsr, int bDesc){
  Fts5Config *pConfig = pTab->pConfig;
  Fts5Sorter *pSorter;
  int nPhrase;
  int nByte;
  int rc = SQLITE_OK;
  char *zSql;
  const char *zRank = pCsr->zRank;
  const char *zRankArgs = pCsr->zRankArgs;
  
  nPhrase = sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
  nByte = sizeof(Fts5Sorter) + sizeof(int) * nPhrase;
  pSorter = (Fts5Sorter*)sqlite3_malloc(nByte);
  if( pSorter==0 ) return SQLITE_NOMEM;
  memset(pSorter, 0, nByte);
  pSorter->nIdx = nPhrase;

  /* TODO: It would be better to have some system for reusing statement
  ** handles here, rather than preparing a new one for each query. But that
  ** is not possible as SQLite reference counts the virtual table objects.
  ** And since the statement required here reads from this very virtual 
  ** table, saving it creates a circular reference.
  **
  ** If SQLite a built-in statement cache, this wouldn't be a problem. */
  zSql = sqlite3Fts5Mprintf(&rc, 
      "SELECT rowid, rank FROM %Q.%Q ORDER BY %s(%s%s%s) %s",
      pConfig->zDb, pConfig->zName, zRank, pConfig->zName,
      (zRankArgs ? ", " : ""),
      (zRankArgs ? zRankArgs : ""),
      bDesc ? "DESC" : "ASC"
  );
  if( zSql ){
    rc = sqlite3_prepare_v2(pConfig->db, zSql, -1, &pSorter->pStmt, 0);
    sqlite3_free(zSql);
  }

  pCsr->pSorter = pSorter;
  if( rc==SQLITE_OK ){
    assert( pTab->pSortCsr==0 );
    pTab->pSortCsr = pCsr;
    rc = fts5SorterNext(pCsr);
    pTab->pSortCsr = 0;
  }

  if( rc!=SQLITE_OK ){
    sqlite3_finalize(pSorter->pStmt);
    sqlite3_free(pSorter);
    pCsr->pSorter = 0;
  }

  return rc;
}

static int fts5CursorFirst(Fts5Table *pTab, Fts5Cursor *pCsr, int bDesc){
  int rc;
  Fts5Expr *pExpr = pCsr->pExpr;
  rc = sqlite3Fts5ExprFirst(pExpr, pTab->pIndex, pCsr->iFirstRowid, bDesc);
  if( sqlite3Fts5ExprEof(pExpr) ){
    CsrFlagSet(pCsr, FTS5CSR_EOF);
  }
  fts5CsrNewrow(pCsr);
  return rc;
}

/*
** Process a "special" query. A special query is identified as one with a
** MATCH expression that begins with a '*' character. The remainder of
** the text passed to the MATCH operator are used as  the special query
** parameters.
*/
static int fts5SpecialMatch(
  Fts5Table *pTab, 
  Fts5Cursor *pCsr, 
  const char *zQuery
){
  int rc = SQLITE_OK;             /* Return code */
  const char *z = zQuery;         /* Special query text */
  int n;                          /* Number of bytes in text at z */

  while( z[0]==' ' ) z++;
  for(n=0; z[n] && z[n]!=' '; n++);

  assert( pTab->base.zErrMsg==0 );
  pCsr->ePlan = FTS5_PLAN_SPECIAL;

  if( 0==sqlite3_strnicmp("reads", z, n) ){
    pCsr->iSpecial = sqlite3Fts5IndexReads(pTab->pIndex);
  }
  else if( 0==sqlite3_strnicmp("id", z, n) ){
    pCsr->iSpecial = pCsr->iCsrId;
  }
  else{
    /* An unrecognized directive. Return an error message. */
    pTab->base.zErrMsg = sqlite3_mprintf("unknown special query: %.*s", n, z);
    rc = SQLITE_ERROR;
  }

  return rc;
}

/*
** Search for an auxiliary function named zName that can be used with table
** pTab. If one is found, return a pointer to the corresponding Fts5Auxiliary
** structure. Otherwise, if no such function exists, return NULL.
*/
static Fts5Auxiliary *fts5FindAuxiliary(Fts5Table *pTab, const char *zName){
  Fts5Auxiliary *pAux;

  for(pAux=pTab->pGlobal->pAux; pAux; pAux=pAux->pNext){
    if( sqlite3_stricmp(zName, pAux->zFunc)==0 ) return pAux;
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}


static int fts5FindRankFunction(Fts5Cursor *pCsr){
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  Fts5Config *pConfig = pTab->pConfig;
  int rc = SQLITE_OK;
  Fts5Auxiliary *pAux = 0;
  const char *zRank = pCsr->zRank;
  const char *zRankArgs = pCsr->zRankArgs;

  if( zRankArgs ){
    char *zSql = sqlite3Fts5Mprintf(&rc, "SELECT %s", zRankArgs);
    if( zSql ){
      sqlite3_stmt *pStmt = 0;
      rc = sqlite3_prepare_v2(pConfig->db, zSql, -1, &pStmt, 0);
      sqlite3_free(zSql);
      assert( rc==SQLITE_OK || pCsr->pRankArgStmt==0 );
      if( rc==SQLITE_OK ){
        if( SQLITE_ROW==sqlite3_step(pStmt) ){
          int nByte;
          pCsr->nRankArg = sqlite3_column_count(pStmt);
          nByte = sizeof(sqlite3_value*)*pCsr->nRankArg;
          pCsr->apRankArg = (sqlite3_value**)sqlite3Fts5MallocZero(&rc, nByte);
          if( rc==SQLITE_OK ){
            int i;
            for(i=0; i<pCsr->nRankArg; i++){
              pCsr->apRankArg[i] = sqlite3_column_value(pStmt, i);
            }
          }
          pCsr->pRankArgStmt = pStmt;
        }else{
          rc = sqlite3_finalize(pStmt);
          assert( rc!=SQLITE_OK );
        }
      }
    }
  }

  if( rc==SQLITE_OK ){
    pAux = fts5FindAuxiliary(pTab, zRank);
    if( pAux==0 ){
      assert( pTab->base.zErrMsg==0 );
      pTab->base.zErrMsg = sqlite3_mprintf("no such function: %s", zRank);
      rc = SQLITE_ERROR;
    }
  }

  pCsr->pRank = pAux;
  return rc;
}


static int fts5CursorParseRank(
  Fts5Config *pConfig,
  Fts5Cursor *pCsr, 
  sqlite3_value *pRank
){
  int rc = SQLITE_OK;
  if( pRank ){
    const char *z = (const char*)sqlite3_value_text(pRank);
    char *zRank = 0;
    char *zRankArgs = 0;

    if( z==0 ){
      if( sqlite3_value_type(pRank)==SQLITE_NULL ) rc = SQLITE_ERROR;
    }else{
      rc = sqlite3Fts5ConfigParseRank(z, &zRank, &zRankArgs);
    }
    if( rc==SQLITE_OK ){
      pCsr->zRank = zRank;
      pCsr->zRankArgs = zRankArgs;
      CsrFlagSet(pCsr, FTS5CSR_FREE_ZRANK);
    }else if( rc==SQLITE_ERROR ){
      pCsr->base.pVtab->zErrMsg = sqlite3_mprintf(
          "parse error in rank function: %s", z
      );
    }
  }else{
    if( pConfig->zRank ){
      pCsr->zRank = (char*)pConfig->zRank;
      pCsr->zRankArgs = (char*)pConfig->zRankArgs;
    }else{
      pCsr->zRank = (char*)FTS5_DEFAULT_RANK;
      pCsr->zRankArgs = 0;
    }
  }
  return rc;
}

static i64 fts5GetRowidLimit(sqlite3_value *pVal, i64 iDefault){
  if( pVal ){
    int eType = sqlite3_value_numeric_type(pVal);
    if( eType==SQLITE_INTEGER ){
      return sqlite3_value_int64(pVal);
    }
  }
  return iDefault;
}

/*
** This is the xFilter interface for the virtual table.  See
** the virtual table xFilter method documentation for additional
** information.
** 
** There are three possible query strategies:
**
**   1. Full-text search using a MATCH operator.
**   2. A by-rowid lookup.
**   3. A full-table scan.
*/
static int fts5FilterMethod(
  sqlite3_vtab_cursor *pCursor,   /* The cursor used for this query */
  int idxNum,                     /* Strategy index */
  const char *idxStr,             /* Unused */
  int nVal,                       /* Number of elements in apVal */
  sqlite3_value **apVal           /* Arguments for the indexing scheme */
){
  Fts5Table *pTab = (Fts5Table*)(pCursor->pVtab);
  Fts5Config *pConfig = pTab->pConfig;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int rc = SQLITE_OK;             /* Error code */
  int iVal = 0;                   /* Counter for apVal[] */
  int bDesc;                      /* True if ORDER BY [rank|rowid] DESC */
  int bOrderByRank;               /* True if ORDER BY rank */
  sqlite3_value *pMatch = 0;      /* <tbl> MATCH ? expression (or NULL) */
  sqlite3_value *pRank = 0;       /* rank MATCH ? expression (or NULL) */
  sqlite3_value *pRowidEq = 0;    /* rowid = ? expression (or NULL) */
  sqlite3_value *pRowidLe = 0;    /* rowid <= ? expression (or NULL) */
  sqlite3_value *pRowidGe = 0;    /* rowid >= ? expression (or NULL) */
  char **pzErrmsg = pConfig->pzErrmsg;

  assert( pCsr->pStmt==0 );
  assert( pCsr->pExpr==0 );
  assert( pCsr->csrflags==0 );
  assert( pCsr->pRank==0 );
  assert( pCsr->zRank==0 );
  assert( pCsr->zRankArgs==0 );

  assert( pzErrmsg==0 || pzErrmsg==&pTab->base.zErrMsg );
  pConfig->pzErrmsg = &pTab->base.zErrMsg;

  /* Decode the arguments passed through to this function.
  **
  ** Note: The following set of if(...) statements must be in the same
  ** order as the corresponding entries in the struct at the top of
  ** fts5BestIndexMethod().  */
  if( BitFlagTest(idxNum, FTS5_BI_MATCH) ) pMatch = apVal[iVal++];
  if( BitFlagTest(idxNum, FTS5_BI_RANK) ) pRank = apVal[iVal++];
  if( BitFlagTest(idxNum, FTS5_BI_ROWID_EQ) ) pRowidEq = apVal[iVal++];
  if( BitFlagTest(idxNum, FTS5_BI_ROWID_LE) ) pRowidLe = apVal[iVal++];
  if( BitFlagTest(idxNum, FTS5_BI_ROWID_GE) ) pRowidGe = apVal[iVal++];
  assert( iVal==nVal );
  bOrderByRank = ((idxNum & FTS5_BI_ORDER_RANK) ? 1 : 0);
  pCsr->bDesc = bDesc = ((idxNum & FTS5_BI_ORDER_DESC) ? 1 : 0);

  /* Set the cursor upper and lower rowid limits. Only some strategies 
  ** actually use them. This is ok, as the xBestIndex() method leaves the
  ** sqlite3_index_constraint.omit flag clear for range constraints
  ** on the rowid field.  */
  if( pRowidEq ){
    pRowidLe = pRowidGe = pRowidEq;
  }
  if( bDesc ){
    pCsr->iFirstRowid = fts5GetRowidLimit(pRowidLe, LARGEST_INT64);
    pCsr->iLastRowid = fts5GetRowidLimit(pRowidGe, SMALLEST_INT64);
  }else{
    pCsr->iLastRowid = fts5GetRowidLimit(pRowidLe, LARGEST_INT64);
    pCsr->iFirstRowid = fts5GetRowidLimit(pRowidGe, SMALLEST_INT64);
  }

  if( pTab->pSortCsr ){
    /* If pSortCsr is non-NULL, then this call is being made as part of 
    ** processing for a "... MATCH <expr> ORDER BY rank" query (ePlan is
    ** set to FTS5_PLAN_SORTED_MATCH). pSortCsr is the cursor that will
    ** return results to the user for this query. The current cursor 
    ** (pCursor) is used to execute the query issued by function 
    ** fts5CursorFirstSorted() above.  */
    assert( pRowidEq==0 && pRowidLe==0 && pRowidGe==0 && pRank==0 );
    assert( nVal==0 && pMatch==0 && bOrderByRank==0 && bDesc==0 );
    assert( pCsr->iLastRowid==LARGEST_INT64 );
    assert( pCsr->iFirstRowid==SMALLEST_INT64 );
    pCsr->ePlan = FTS5_PLAN_SOURCE;
    pCsr->pExpr = pTab->pSortCsr->pExpr;
    rc = fts5CursorFirst(pTab, pCsr, bDesc);
  }else if( pMatch ){
    const char *zExpr = (const char*)sqlite3_value_text(apVal[0]);

    rc = fts5CursorParseRank(pConfig, pCsr, pRank);
    if( rc==SQLITE_OK ){
      if( zExpr[0]=='*' ){
        /* The user has issued a query of the form "MATCH '*...'". This
        ** indicates that the MATCH expression is not a full text query,
        ** but a request for an internal parameter.  */
        rc = fts5SpecialMatch(pTab, pCsr, &zExpr[1]);
      }else{
        char **pzErr = &pTab->base.zErrMsg;
        rc = sqlite3Fts5ExprNew(pConfig, zExpr, &pCsr->pExpr, pzErr);
        if( rc==SQLITE_OK ){
          if( bOrderByRank ){
            pCsr->ePlan = FTS5_PLAN_SORTED_MATCH;
            rc = fts5CursorFirstSorted(pTab, pCsr, bDesc);
          }else{
            pCsr->ePlan = FTS5_PLAN_MATCH;
            rc = fts5CursorFirst(pTab, pCsr, bDesc);
          }
        }
      }
    }
  }else if( pConfig->zContent==0 ){
    *pConfig->pzErrmsg = sqlite3_mprintf(
        "%s: table does not support scanning", pConfig->zName
    );
    rc = SQLITE_ERROR;
  }else{
    /* This is either a full-table scan (ePlan==FTS5_PLAN_SCAN) or a lookup
    ** by rowid (ePlan==FTS5_PLAN_ROWID).  */
    pCsr->ePlan = (pRowidEq ? FTS5_PLAN_ROWID : FTS5_PLAN_SCAN);
    rc = sqlite3Fts5StorageStmt(
        pTab->pStorage, fts5StmtType(pCsr), &pCsr->pStmt, &pTab->base.zErrMsg
    );
    if( rc==SQLITE_OK ){
      if( pCsr->ePlan==FTS5_PLAN_ROWID ){
        sqlite3_bind_value(pCsr->pStmt, 1, apVal[0]);
      }else{
        sqlite3_bind_int64(pCsr->pStmt, 1, pCsr->iFirstRowid);
        sqlite3_bind_int64(pCsr->pStmt, 2, pCsr->iLastRowid);
      }
      rc = fts5NextMethod(pCursor);
    }
  }

  pConfig->pzErrmsg = pzErrmsg;
  return rc;
}

/* 
** This is the xEof method of the virtual table. SQLite calls this 
** routine to find out if it has reached the end of a result set.
*/
static int fts5EofMethod(sqlite3_vtab_cursor *pCursor){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  return (CsrFlagTest(pCsr, FTS5CSR_EOF) ? 1 : 0);
}

/*
** Return the rowid that the cursor currently points to.
*/
static i64 fts5CursorRowid(Fts5Cursor *pCsr){
  assert( pCsr->ePlan==FTS5_PLAN_MATCH 
       || pCsr->ePlan==FTS5_PLAN_SORTED_MATCH 
       || pCsr->ePlan==FTS5_PLAN_SOURCE 
  );
  if( pCsr->pSorter ){
    return pCsr->pSorter->iRowid;
  }else{
    return sqlite3Fts5ExprRowid(pCsr->pExpr);
  }
}

/* 
** This is the xRowid method. The SQLite core calls this routine to
** retrieve the rowid for the current row of the result set. fts5
** exposes %_content.docid as the rowid for the virtual table. The
** rowid should be written to *pRowid.
*/
static int fts5RowidMethod(sqlite3_vtab_cursor *pCursor, sqlite_int64 *pRowid){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int ePlan = pCsr->ePlan;
  
  assert( CsrFlagTest(pCsr, FTS5CSR_EOF)==0 );
  switch( ePlan ){
    case FTS5_PLAN_SPECIAL:
      *pRowid = 0;
      break;

    case FTS5_PLAN_SOURCE:
    case FTS5_PLAN_MATCH:
    case FTS5_PLAN_SORTED_MATCH:
      *pRowid = fts5CursorRowid(pCsr);
      break;

    default:
      *pRowid = sqlite3_column_int64(pCsr->pStmt, 0);
      break;
  }

  return SQLITE_OK;
}

/*
** If the cursor requires seeking (bSeekRequired flag is set), seek it.
** Return SQLITE_OK if no error occurs, or an SQLite error code otherwise.
**
** If argument bErrormsg is true and an error occurs, an error message may
** be left in sqlite3_vtab.zErrMsg.
*/
static int fts5SeekCursor(Fts5Cursor *pCsr, int bErrormsg){
  int rc = SQLITE_OK;

  /* If the cursor does not yet have a statement handle, obtain one now. */ 
  if( pCsr->pStmt==0 ){
    Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
    int eStmt = fts5StmtType(pCsr);
    rc = sqlite3Fts5StorageStmt(
        pTab->pStorage, eStmt, &pCsr->pStmt, (bErrormsg?&pTab->base.zErrMsg:0)
    );
    assert( rc!=SQLITE_OK || pTab->base.zErrMsg==0 );
    assert( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_CONTENT) );
  }

  if( rc==SQLITE_OK && CsrFlagTest(pCsr, FTS5CSR_REQUIRE_CONTENT) ){
    assert( pCsr->pExpr );
    sqlite3_reset(pCsr->pStmt);
    sqlite3_bind_int64(pCsr->pStmt, 1, fts5CursorRowid(pCsr));
    rc = sqlite3_step(pCsr->pStmt);
    if( rc==SQLITE_ROW ){
      rc = SQLITE_OK;
      CsrFlagClear(pCsr, FTS5CSR_REQUIRE_CONTENT);
    }else{
      rc = sqlite3_reset(pCsr->pStmt);
      if( rc==SQLITE_OK ){
        rc = FTS5_CORRUPT;
      }
    }
  }
  return rc;
}

static void fts5SetVtabError(Fts5Table *p, const char *zFormat, ...){
  va_list ap;                     /* ... printf arguments */
  va_start(ap, zFormat);
  assert( p->base.zErrMsg==0 );
  p->base.zErrMsg = sqlite3_vmprintf(zFormat, ap);
  va_end(ap);
}

/*
** This function is called to handle an FTS INSERT command. In other words,
** an INSERT statement of the form:
**
**     INSERT INTO fts(fts) VALUES($pCmd)
**     INSERT INTO fts(fts, rank) VALUES($pCmd, $pVal)
**
** Argument pVal is the value assigned to column "fts" by the INSERT 
** statement. This function returns SQLITE_OK if successful, or an SQLite
** error code if an error occurs.
**
** The commands implemented by this function are documented in the "Special
** INSERT Directives" section of the documentation. It should be updated if
** more commands are added to this function.
*/
static int fts5SpecialInsert(
  Fts5Table *pTab,                /* Fts5 table object */
  sqlite3_value *pCmd,            /* Value inserted into special column */
  sqlite3_value *pVal             /* Value inserted into rank column */
){
  Fts5Config *pConfig = pTab->pConfig;
  const char *z = (const char*)sqlite3_value_text(pCmd);
  int rc = SQLITE_OK;
  int bError = 0;

  if( 0==sqlite3_stricmp("delete-all", z) ){
    if( pConfig->eContent==FTS5_CONTENT_NORMAL ){
      fts5SetVtabError(pTab, 
          "'delete-all' may only be used with a "
          "contentless or external content fts5 table"
      );
      rc = SQLITE_ERROR;
    }else{
      rc = sqlite3Fts5StorageDeleteAll(pTab->pStorage);
    }
  }else if( 0==sqlite3_stricmp("rebuild", z) ){
    if( pConfig->eContent==FTS5_CONTENT_NONE ){
      fts5SetVtabError(pTab, 
          "'rebuild' may not be used with a contentless fts5 table"
      );
      rc = SQLITE_ERROR;
    }else{
      rc = sqlite3Fts5StorageRebuild(pTab->pStorage);
    }
  }else if( 0==sqlite3_stricmp("optimize", z) ){
    rc = sqlite3Fts5StorageOptimize(pTab->pStorage);
  }else if( 0==sqlite3_stricmp("merge", z) ){
    int nMerge = sqlite3_value_int(pVal);
    rc = sqlite3Fts5StorageMerge(pTab->pStorage, nMerge);
  }else if( 0==sqlite3_stricmp("integrity-check", z) ){
    rc = sqlite3Fts5StorageIntegrity(pTab->pStorage);
  }else{
    rc = sqlite3Fts5IndexLoadConfig(pTab->pIndex);
    if( rc==SQLITE_OK ){
      rc = sqlite3Fts5ConfigSetValue(pTab->pConfig, z, pVal, &bError);
    }
    if( rc==SQLITE_OK ){
      if( bError ){
        rc = SQLITE_ERROR;
      }else{
        rc = sqlite3Fts5StorageConfigValue(pTab->pStorage, z, pVal, 0);
      }
    }
  }
  return rc;
}

static int fts5SpecialDelete(
  Fts5Table *pTab, 
  sqlite3_value **apVal, 
  sqlite3_int64 *piRowid
){
  int rc = SQLITE_OK;
  int eType1 = sqlite3_value_type(apVal[1]);
  if( eType1==SQLITE_INTEGER ){
    sqlite3_int64 iDel = sqlite3_value_int64(apVal[1]);
    rc = sqlite3Fts5StorageSpecialDelete(pTab->pStorage, iDel, &apVal[2]);
  }
  return rc;
}

/* 
** This function is the implementation of the xUpdate callback used by 
** FTS3 virtual tables. It is invoked by SQLite each time a row is to be
** inserted, updated or deleted.
*/
static int fts5UpdateMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  int nArg,                       /* Size of argument array */
  sqlite3_value **apVal,          /* Array of arguments */
  sqlite_int64 *pRowid            /* OUT: The affected (or effected) rowid */
){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  Fts5Config *pConfig = pTab->pConfig;
  int eType0;                     /* value_type() of apVal[0] */
  int eConflict;                  /* ON CONFLICT for this DML */
  int rc = SQLITE_OK;             /* Return code */

  /* A transaction must be open when this is called. */
  assert( pTab->ts.eState==1 );

  assert( pTab->pConfig->pzErrmsg==0 );
  pTab->pConfig->pzErrmsg = &pTab->base.zErrMsg;

  /* A delete specifies a single argument - the rowid of the row to remove.
  ** Update and insert operations pass:
  **
  **   1. The "old" rowid, or NULL.
  **   2. The "new" rowid.
  **   3. Values for each of the nCol matchable columns.
  **   4. Values for the two hidden columns (<tablename> and "rank").
  */

  eType0 = sqlite3_value_type(apVal[0]);
  eConflict = sqlite3_vtab_on_conflict(pConfig->db);

  assert( eType0==SQLITE_INTEGER || eType0==SQLITE_NULL );
  assert( pVtab->zErrMsg==0 );
  assert( (nArg==1 && eType0==SQLITE_INTEGER) || nArg==(2+pConfig->nCol+2) );

  fts5TripCursors(pTab);
  if( eType0==SQLITE_INTEGER ){
    if( fts5IsContentless(pTab) ){
      pTab->base.zErrMsg = sqlite3_mprintf(
          "cannot %s contentless fts5 table: %s", 
          (nArg>1 ? "UPDATE" : "DELETE from"), pConfig->zName
      );
      rc = SQLITE_ERROR;
    }else{
      i64 iDel = sqlite3_value_int64(apVal[0]);  /* Rowid to delete */
      rc = sqlite3Fts5StorageDelete(pTab->pStorage, iDel);
    }
  }else{
    assert( nArg>1 );
    sqlite3_value *pCmd = apVal[2 + pConfig->nCol];
    if( SQLITE_NULL!=sqlite3_value_type(pCmd) ){
      const char *z = (const char*)sqlite3_value_text(pCmd);
      if( pConfig->eContent!=FTS5_CONTENT_NORMAL 
       && 0==sqlite3_stricmp("delete", z) 
      ){
        rc = fts5SpecialDelete(pTab, apVal, pRowid);
      }else{
        rc = fts5SpecialInsert(pTab, pCmd, apVal[2 + pConfig->nCol + 1]);
      }
      goto update_method_out;
    }
  }


  if( rc==SQLITE_OK && nArg>1 ){
    rc = sqlite3Fts5StorageInsert(pTab->pStorage, apVal, eConflict, pRowid);
  }

 update_method_out:
  pTab->pConfig->pzErrmsg = 0;
  return rc;
}

/*
** Implementation of xSync() method. 
*/
static int fts5SyncMethod(sqlite3_vtab *pVtab){
  int rc;
  Fts5Table *pTab = (Fts5Table*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_SYNC, 0);
  pTab->pConfig->pzErrmsg = &pTab->base.zErrMsg;
  fts5TripCursors(pTab);
  rc = sqlite3Fts5StorageSync(pTab->pStorage, 1);
  pTab->pConfig->pzErrmsg = 0;
  return rc;
}

/*
** Implementation of xBegin() method. 
*/
static int fts5BeginMethod(sqlite3_vtab *pVtab){
  fts5CheckTransactionState((Fts5Table*)pVtab, FTS5_BEGIN, 0);
  return SQLITE_OK;
}

/*
** Implementation of xCommit() method. This is a no-op. The contents of
** the pending-terms hash-table have already been flushed into the database
** by fts5SyncMethod().
*/
static int fts5CommitMethod(sqlite3_vtab *pVtab){
  fts5CheckTransactionState((Fts5Table*)pVtab, FTS5_COMMIT, 0);
  return SQLITE_OK;
}

/*
** Implementation of xRollback(). Discard the contents of the pending-terms
** hash-table. Any changes made to the database are reverted by SQLite.
*/
static int fts5RollbackMethod(sqlite3_vtab *pVtab){
  int rc;
  Fts5Table *pTab = (Fts5Table*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_ROLLBACK, 0);
  rc = sqlite3Fts5StorageRollback(pTab->pStorage);
  return rc;
}

static void *fts5ApiUserData(Fts5Context *pCtx){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return pCsr->pAux->pUserData;
}

static int fts5ApiColumnCount(Fts5Context *pCtx){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return ((Fts5Table*)(pCsr->base.pVtab))->pConfig->nCol;
}

static int fts5ApiColumnTotalSize(
  Fts5Context *pCtx, 
  int iCol, 
  sqlite3_int64 *pnToken
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  return sqlite3Fts5StorageSize(pTab->pStorage, iCol, pnToken);
}

static int fts5ApiRowCount(Fts5Context *pCtx, i64 *pnRow){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  return sqlite3Fts5StorageRowCount(pTab->pStorage, pnRow);
}

static int fts5ApiTokenize(
  Fts5Context *pCtx, 
  const char *pText, int nText, 
  void *pUserData,
  int (*xToken)(void*, const char*, int, int, int)
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  return sqlite3Fts5Tokenize(pTab->pConfig, pText, nText, pUserData, xToken);
}

static int fts5ApiPhraseCount(Fts5Context *pCtx){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
}

static int fts5ApiPhraseSize(Fts5Context *pCtx, int iPhrase){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  return sqlite3Fts5ExprPhraseSize(pCsr->pExpr, iPhrase);
}

static int fts5CsrPoslist(Fts5Cursor *pCsr, int iPhrase, const u8 **pa){
  int n;
  if( pCsr->pSorter ){
    Fts5Sorter *pSorter = pCsr->pSorter;
    int i1 = (iPhrase==0 ? 0 : pSorter->aIdx[iPhrase-1]);
    n = pSorter->aIdx[iPhrase] - i1;
    *pa = &pSorter->aPoslist[i1];
  }else{
    n = sqlite3Fts5ExprPoslist(pCsr->pExpr, iPhrase, pa);
  }
  return n;
}

/*
** Ensure that the Fts5Cursor.nInstCount and aInst[] variables are populated
** correctly for the current view. Return SQLITE_OK if successful, or an
** SQLite error code otherwise.
*/
static int fts5CacheInstArray(Fts5Cursor *pCsr){
  int rc = SQLITE_OK;
  if( pCsr->aInst==0 ){
    Fts5PoslistReader *aIter;     /* One iterator for each phrase */
    int nIter;                    /* Number of iterators/phrases */
    int nByte;
    
    nIter = sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
    nByte = sizeof(Fts5PoslistReader) * nIter;
    aIter = (Fts5PoslistReader*)sqlite3Fts5MallocZero(&rc, nByte);
    if( aIter ){
      Fts5Buffer buf = {0, 0, 0}; /* Build up aInst[] here */
      int nInst = 0;              /* Number instances seen so far */
      int i;

      /* Initialize all iterators */
      for(i=0; i<nIter; i++){
        const u8 *a;
        int n = fts5CsrPoslist(pCsr, i, &a);
        sqlite3Fts5PoslistReaderInit(-1, a, n, &aIter[i]);
      }

      while( 1 ){
        int *aInst;
        int iBest = -1;
        for(i=0; i<nIter; i++){
          if( (aIter[i].bEof==0) 
           && (iBest<0 || aIter[i].iPos<aIter[iBest].iPos) 
          ){
            iBest = i;
          }
        }

        if( iBest<0 ) break;
        nInst++;
        if( sqlite3Fts5BufferGrow(&rc, &buf, nInst * sizeof(int) * 3) ) break;

        aInst = &((int*)buf.p)[3 * (nInst-1)];
        aInst[0] = iBest;
        aInst[1] = FTS5_POS2COLUMN(aIter[iBest].iPos);
        aInst[2] = FTS5_POS2OFFSET(aIter[iBest].iPos);
        sqlite3Fts5PoslistReaderNext(&aIter[iBest]);
      }

      pCsr->aInst = (int*)buf.p;
      pCsr->nInstCount = nInst;
      sqlite3_free(aIter);
    }
  }
  return rc;
}

static int fts5ApiInstCount(Fts5Context *pCtx, int *pnInst){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  int rc;
  if( SQLITE_OK==(rc = fts5CacheInstArray(pCsr)) ){
    *pnInst = pCsr->nInstCount;
  }
  return rc;
}

static int fts5ApiInst(
  Fts5Context *pCtx, 
  int iIdx, 
  int *piPhrase, 
  int *piCol, 
  int *piOff
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  int rc;
  if( SQLITE_OK==(rc = fts5CacheInstArray(pCsr)) ){
    if( iIdx<0 || iIdx>=pCsr->nInstCount ){
      rc = SQLITE_RANGE;
    }else{
      *piPhrase = pCsr->aInst[iIdx*3];
      *piCol = pCsr->aInst[iIdx*3 + 1];
      *piOff = pCsr->aInst[iIdx*3 + 2];
    }
  }
  return rc;
}

static sqlite3_int64 fts5ApiRowid(Fts5Context *pCtx){
  return fts5CursorRowid((Fts5Cursor*)pCtx);
}

static int fts5ApiColumnText(
  Fts5Context *pCtx, 
  int iCol, 
  const char **pz, 
  int *pn
){
  int rc = SQLITE_OK;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  if( fts5IsContentless((Fts5Table*)(pCsr->base.pVtab)) ){
    *pz = 0;
    *pn = 0;
  }else{
    rc = fts5SeekCursor(pCsr, 0);
    if( rc==SQLITE_OK ){
      *pz = (const char*)sqlite3_column_text(pCsr->pStmt, iCol+1);
      *pn = sqlite3_column_bytes(pCsr->pStmt, iCol+1);
    }
  }
  return rc;
}

static int fts5ColumnSizeCb(
  void *pContext,                 /* Pointer to int */
  const char *pToken,             /* Buffer containing token */
  int nToken,                     /* Size of token in bytes */
  int iStart,                     /* Start offset of token */
  int iEnd                        /* End offset of token */
){
  int *pCnt = (int*)pContext;
  *pCnt = *pCnt + 1;
  return SQLITE_OK;
}

static int fts5ApiColumnSize(Fts5Context *pCtx, int iCol, int *pnToken){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  Fts5Config *pConfig = pTab->pConfig;
  int rc = SQLITE_OK;

  if( CsrFlagTest(pCsr, FTS5CSR_REQUIRE_DOCSIZE) ){
    if( pConfig->bColumnsize ){
      i64 iRowid = fts5CursorRowid(pCsr);
      rc = sqlite3Fts5StorageDocsize(pTab->pStorage, iRowid, pCsr->aColumnSize);
    }else if( pConfig->zContent==0 ){
      int i;
      for(i=0; i<pConfig->nCol; i++){
        if( pConfig->abUnindexed[i]==0 ){
          pCsr->aColumnSize[i] = -1;
        }
      }
    }else{
      int i;
      for(i=0; rc==SQLITE_OK && i<pConfig->nCol; i++){
        if( pConfig->abUnindexed[i]==0 ){
          const char *z; int n;
          void *p = (void*)(&pCsr->aColumnSize[i]);
          pCsr->aColumnSize[i] = 0;
          rc = fts5ApiColumnText(pCtx, i, &z, &n);
          if( rc==SQLITE_OK ){
            rc = sqlite3Fts5Tokenize(pConfig, z, n, p, fts5ColumnSizeCb);
          }
        }
      }
    }
    CsrFlagClear(pCsr, FTS5CSR_REQUIRE_DOCSIZE);
  }
  if( iCol<0 ){
    int i;
    *pnToken = 0;
    for(i=0; i<pConfig->nCol; i++){
      *pnToken += pCsr->aColumnSize[i];
    }
  }else if( iCol<pConfig->nCol ){
    *pnToken = pCsr->aColumnSize[iCol];
  }else{
    *pnToken = 0;
    rc = SQLITE_RANGE;
  }
  return rc;
}

/*
** Implementation of the xSetAuxdata() method.
*/
static int fts5ApiSetAuxdata(
  Fts5Context *pCtx,              /* Fts5 context */
  void *pPtr,                     /* Pointer to save as auxdata */
  void(*xDelete)(void*)           /* Destructor for pPtr (or NULL) */
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Auxdata *pData;

  /* Search through the cursors list of Fts5Auxdata objects for one that
  ** corresponds to the currently executing auxiliary function.  */
  for(pData=pCsr->pAuxdata; pData; pData=pData->pNext){
    if( pData->pAux==pCsr->pAux ) break;
  }

  if( pData ){
    if( pData->xDelete ){
      pData->xDelete(pData->pPtr);
    }
  }else{
    int rc = SQLITE_OK;
    pData = (Fts5Auxdata*)sqlite3Fts5MallocZero(&rc, sizeof(Fts5Auxdata));
    if( pData==0 ){
      if( xDelete ) xDelete(pPtr);
      return rc;
    }
    pData->pAux = pCsr->pAux;
    pData->pNext = pCsr->pAuxdata;
    pCsr->pAuxdata = pData;
  }

  pData->xDelete = xDelete;
  pData->pPtr = pPtr;
  return SQLITE_OK;
}

static void *fts5ApiGetAuxdata(Fts5Context *pCtx, int bClear){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Auxdata *pData;
  void *pRet = 0;

  for(pData=pCsr->pAuxdata; pData; pData=pData->pNext){
    if( pData->pAux==pCsr->pAux ) break;
  }

  if( pData ){
    pRet = pData->pPtr;
    if( bClear ){
      pData->pPtr = 0;
      pData->xDelete = 0;
    }
  }

  return pRet;
}

static int fts5ApiQueryPhrase(Fts5Context*, int, void*, 
    int(*)(const Fts5ExtensionApi*, Fts5Context*, void*)
);

static const Fts5ExtensionApi sFts5Api = {
  1,                            /* iVersion */
  fts5ApiUserData,
  fts5ApiColumnCount,
  fts5ApiRowCount,
  fts5ApiColumnTotalSize,
  fts5ApiTokenize,
  fts5ApiPhraseCount,
  fts5ApiPhraseSize,
  fts5ApiInstCount,
  fts5ApiInst,
  fts5ApiRowid,
  fts5ApiColumnText,
  fts5ApiColumnSize,
  fts5ApiQueryPhrase,
  fts5ApiSetAuxdata,
  fts5ApiGetAuxdata,
};


/*
** Implementation of API function xQueryPhrase().
*/
static int fts5ApiQueryPhrase(
  Fts5Context *pCtx, 
  int iPhrase, 
  void *pUserData,
  int(*xCallback)(const Fts5ExtensionApi*, Fts5Context*, void*)
){
  Fts5Cursor *pCsr = (Fts5Cursor*)pCtx;
  Fts5Table *pTab = (Fts5Table*)(pCsr->base.pVtab);
  int rc;
  Fts5Cursor *pNew = 0;

  rc = fts5OpenMethod(pCsr->base.pVtab, (sqlite3_vtab_cursor**)&pNew);
  if( rc==SQLITE_OK ){
    Fts5Config *pConf = pTab->pConfig;
    pNew->ePlan = FTS5_PLAN_MATCH;
    pNew->iFirstRowid = SMALLEST_INT64;
    pNew->iLastRowid = LARGEST_INT64;
    pNew->base.pVtab = (sqlite3_vtab*)pTab;
    rc = sqlite3Fts5ExprPhraseExpr(pConf, pCsr->pExpr, iPhrase, &pNew->pExpr);
  }

  if( rc==SQLITE_OK ){
    for(rc = fts5CursorFirst(pTab, pNew, 0);
        rc==SQLITE_OK && CsrFlagTest(pNew, FTS5CSR_EOF)==0;
        rc = fts5NextMethod((sqlite3_vtab_cursor*)pNew)
    ){
      rc = xCallback(&sFts5Api, (Fts5Context*)pNew, pUserData);
      if( rc!=SQLITE_OK ){
        if( rc==SQLITE_DONE ) rc = SQLITE_OK;
        break;
      }
    }
  }

  fts5CloseMethod((sqlite3_vtab_cursor*)pNew);
  return rc;
}

static void fts5ApiInvoke(
  Fts5Auxiliary *pAux,
  Fts5Cursor *pCsr,
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){
  assert( pCsr->pAux==0 );
  pCsr->pAux = pAux;
  pAux->xFunc(&sFts5Api, (Fts5Context*)pCsr, context, argc, argv);
  pCsr->pAux = 0;
}

static Fts5Cursor *fts5CursorFromCsrid(Fts5Global *pGlobal, i64 iCsrId){
  Fts5Cursor *pCsr;
  for(pCsr=pGlobal->pCsr; pCsr; pCsr=pCsr->pNext){
    if( pCsr->iCsrId==iCsrId ) break;
  }
  return pCsr;
}

static void fts5ApiCallback(
  sqlite3_context *context,
  int argc,
  sqlite3_value **argv
){

  Fts5Auxiliary *pAux;
  Fts5Cursor *pCsr;
  i64 iCsrId;

  assert( argc>=1 );
  pAux = (Fts5Auxiliary*)sqlite3_user_data(context);
  iCsrId = sqlite3_value_int64(argv[0]);

  pCsr = fts5CursorFromCsrid(pAux->pGlobal, iCsrId);
  if( pCsr==0 ){
    char *zErr = sqlite3_mprintf("no such cursor: %lld", iCsrId);
    sqlite3_result_error(context, zErr, -1);
    sqlite3_free(zErr);
  }else{
    fts5ApiInvoke(pAux, pCsr, context, argc-1, &argv[1]);
  }
}


/*
** Given cursor id iId, return a pointer to the corresponding Fts5Index 
** object. Or NULL If the cursor id does not exist.
**
** If successful, set *pnCol to the number of indexed columns in the
** table before returning.
*/
Fts5Index *sqlite3Fts5IndexFromCsrid(
  Fts5Global *pGlobal, 
  i64 iCsrId, 
  int *pnCol
){
  Fts5Cursor *pCsr;
  Fts5Table *pTab;

  pCsr = fts5CursorFromCsrid(pGlobal, iCsrId);
  pTab = (Fts5Table*)pCsr->base.pVtab;
  *pnCol = pTab->pConfig->nCol;

  return pTab->pIndex;
}

/*
** Return a "position-list blob" corresponding to the current position of
** cursor pCsr via sqlite3_result_blob(). A position-list blob contains
** the current position-list for each phrase in the query associated with
** cursor pCsr.
**
** A position-list blob begins with (nPhrase-1) varints, where nPhrase is
** the number of phrases in the query. Following the varints are the
** concatenated position lists for each phrase, in order.
**
** The first varint (if it exists) contains the size of the position list
** for phrase 0. The second (same disclaimer) contains the size of position
** list 1. And so on. There is no size field for the final position list,
** as it can be derived from the total size of the blob.
*/
static int fts5PoslistBlob(sqlite3_context *pCtx, Fts5Cursor *pCsr){
  int i;
  int rc = SQLITE_OK;
  int nPhrase = sqlite3Fts5ExprPhraseCount(pCsr->pExpr);
  Fts5Buffer val;

  memset(&val, 0, sizeof(Fts5Buffer));

  /* Append the varints */
  for(i=0; i<(nPhrase-1); i++){
    const u8 *dummy;
    int nByte = sqlite3Fts5ExprPoslist(pCsr->pExpr, i, &dummy);
    sqlite3Fts5BufferAppendVarint(&rc, &val, nByte);
  }

  /* Append the position lists */
  for(i=0; i<nPhrase; i++){
    const u8 *pPoslist;
    int nPoslist;
    nPoslist = sqlite3Fts5ExprPoslist(pCsr->pExpr, i, &pPoslist);
    sqlite3Fts5BufferAppendBlob(&rc, &val, nPoslist, pPoslist);
  }

  sqlite3_result_blob(pCtx, val.p, val.n, sqlite3_free);
  return rc;
}

/* 
** This is the xColumn method, called by SQLite to request a value from
** the row that the supplied cursor currently points to.
*/
static int fts5ColumnMethod(
  sqlite3_vtab_cursor *pCursor,   /* Cursor to retrieve value from */
  sqlite3_context *pCtx,          /* Context for sqlite3_result_xxx() calls */
  int iCol                        /* Index of column to read value from */
){
  Fts5Table *pTab = (Fts5Table*)(pCursor->pVtab);
  Fts5Config *pConfig = pTab->pConfig;
  Fts5Cursor *pCsr = (Fts5Cursor*)pCursor;
  int rc = SQLITE_OK;
  
  assert( CsrFlagTest(pCsr, FTS5CSR_EOF)==0 );

  if( pCsr->ePlan==FTS5_PLAN_SPECIAL ){
    if( iCol==pConfig->nCol ){
      sqlite3_result_int64(pCtx, pCsr->iSpecial);
    }
  }else

  if( iCol==pConfig->nCol ){
    /* User is requesting the value of the special column with the same name
    ** as the table. Return the cursor integer id number. This value is only
    ** useful in that it may be passed as the first argument to an FTS5
    ** auxiliary function.  */
    sqlite3_result_int64(pCtx, pCsr->iCsrId);
  }else if( iCol==pConfig->nCol+1 ){

    /* The value of the "rank" column. */
    if( pCsr->ePlan==FTS5_PLAN_SOURCE ){
      fts5PoslistBlob(pCtx, pCsr);
    }else if( 
        pCsr->ePlan==FTS5_PLAN_MATCH
     || pCsr->ePlan==FTS5_PLAN_SORTED_MATCH
    ){
      if( pCsr->pRank || SQLITE_OK==(rc = fts5FindRankFunction(pCsr)) ){
        fts5ApiInvoke(pCsr->pRank, pCsr, pCtx, pCsr->nRankArg, pCsr->apRankArg);
      }
    }
  }else if( !fts5IsContentless(pTab) ){
    rc = fts5SeekCursor(pCsr, 1);
    if( rc==SQLITE_OK ){
      sqlite3_result_value(pCtx, sqlite3_column_value(pCsr->pStmt, iCol+1));
    }
  }
  return rc;
}


/*
** This routine implements the xFindFunction method for the FTS3
** virtual table.
*/
static int fts5FindFunctionMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  int nArg,                       /* Number of SQL function arguments */
  const char *zName,              /* Name of SQL function */
  void (**pxFunc)(sqlite3_context*,int,sqlite3_value**), /* OUT: Result */
  void **ppArg                    /* OUT: User data for *pxFunc */
){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  Fts5Auxiliary *pAux;

  pAux = fts5FindAuxiliary(pTab, zName);
  if( pAux ){
    *pxFunc = fts5ApiCallback;
    *ppArg = (void*)pAux;
    return 1;
  }

  /* No function of the specified name was found. Return 0. */
  return 0;
}

/*
** Implementation of FTS5 xRename method. Rename an fts5 table.
*/
static int fts5RenameMethod(
  sqlite3_vtab *pVtab,            /* Virtual table handle */
  const char *zName               /* New name of table */
){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  return sqlite3Fts5StorageRename(pTab->pStorage, zName);
}

/*
** The xSavepoint() method.
**
** Flush the contents of the pending-terms table to disk.
*/
static int fts5SavepointMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_SAVEPOINT, iSavepoint);
  fts5TripCursors(pTab);
  return sqlite3Fts5StorageSync(pTab->pStorage, 0);
}

/*
** The xRelease() method.
**
** This is a no-op.
*/
static int fts5ReleaseMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_RELEASE, iSavepoint);
  fts5TripCursors(pTab);
  return sqlite3Fts5StorageSync(pTab->pStorage, 0);
}

/*
** The xRollbackTo() method.
**
** Discard the contents of the pending terms table.
*/
static int fts5RollbackToMethod(sqlite3_vtab *pVtab, int iSavepoint){
  Fts5Table *pTab = (Fts5Table*)pVtab;
  fts5CheckTransactionState(pTab, FTS5_ROLLBACKTO, iSavepoint);
  fts5TripCursors(pTab);
  return sqlite3Fts5StorageRollback(pTab->pStorage);
}

/*
** Register a new auxiliary function with global context pGlobal.
*/
static int fts5CreateAux(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void *pUserData,                /* User data for aux. function */
  fts5_extension_function xFunc,  /* Aux. function implementation */
  void(*xDestroy)(void*)          /* Destructor for pUserData */
){
  Fts5Global *pGlobal = (Fts5Global*)pApi;
  int rc = sqlite3_overload_function(pGlobal->db, zName, -1);
  if( rc==SQLITE_OK ){
    Fts5Auxiliary *pAux;
    int nByte;                      /* Bytes of space to allocate */

    nByte = sizeof(Fts5Auxiliary) + strlen(zName) + 1;
    pAux = (Fts5Auxiliary*)sqlite3_malloc(nByte);
    if( pAux ){
      memset(pAux, 0, nByte);
      pAux->zFunc = (char*)&pAux[1];
      strcpy(pAux->zFunc, zName);
      pAux->pGlobal = pGlobal;
      pAux->pUserData = pUserData;
      pAux->xFunc = xFunc;
      pAux->xDestroy = xDestroy;
      pAux->pNext = pGlobal->pAux;
      pGlobal->pAux = pAux;
    }else{
      rc = SQLITE_NOMEM;
    }
  }

  return rc;
}

/*
** Register a new tokenizer. This is the implementation of the 
** fts5_api.xCreateTokenizer() method.
*/
static int fts5CreateTokenizer(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void *pUserData,                /* User data for aux. function */
  fts5_tokenizer *pTokenizer,     /* Tokenizer implementation */
  void(*xDestroy)(void*)          /* Destructor for pUserData */
){
  Fts5Global *pGlobal = (Fts5Global*)pApi;
  Fts5TokenizerModule *pNew;
  int nByte;                      /* Bytes of space to allocate */
  int rc = SQLITE_OK;

  nByte = sizeof(Fts5TokenizerModule) + strlen(zName) + 1;
  pNew = (Fts5TokenizerModule*)sqlite3_malloc(nByte);
  if( pNew ){
    memset(pNew, 0, nByte);
    pNew->zName = (char*)&pNew[1];
    strcpy(pNew->zName, zName);
    pNew->pUserData = pUserData;
    pNew->x = *pTokenizer;
    pNew->xDestroy = xDestroy;
    pNew->pNext = pGlobal->pTok;
    pGlobal->pTok = pNew;
    if( pNew->pNext==0 ){
      pGlobal->pDfltTok = pNew;
    }
  }else{
    rc = SQLITE_NOMEM;
  }

  return rc;
}

static Fts5TokenizerModule *fts5LocateTokenizer(
  Fts5Global *pGlobal, 
  const char *zName
){
  Fts5TokenizerModule *pMod = 0;

  if( zName==0 ){
    pMod = pGlobal->pDfltTok;
  }else{
    for(pMod=pGlobal->pTok; pMod; pMod=pMod->pNext){
      if( sqlite3_stricmp(zName, pMod->zName)==0 ) break;
    }
  }

  return pMod;
}

/*
** Find a tokenizer. This is the implementation of the 
** fts5_api.xFindTokenizer() method.
*/
static int fts5FindTokenizer(
  fts5_api *pApi,                 /* Global context (one per db handle) */
  const char *zName,              /* Name of new function */
  void **ppUserData,
  fts5_tokenizer *pTokenizer      /* Populate this object */
){
  int rc = SQLITE_OK;
  Fts5TokenizerModule *pMod;

  pMod = fts5LocateTokenizer((Fts5Global*)pApi, zName);
  if( pMod ){
    *pTokenizer = pMod->x;
    *ppUserData = pMod->pUserData;
  }else{
    memset(pTokenizer, 0, sizeof(fts5_tokenizer));
    rc = SQLITE_ERROR;
  }

  return rc;
}

int sqlite3Fts5GetTokenizer(
  Fts5Global *pGlobal, 
  const char **azArg,
  int nArg,
  Fts5Tokenizer **ppTok,
  fts5_tokenizer **ppTokApi,
  char **pzErr
){
  Fts5TokenizerModule *pMod;
  int rc = SQLITE_OK;

  pMod = fts5LocateTokenizer(pGlobal, nArg==0 ? 0 : azArg[0]);
  if( pMod==0 ){
    assert( nArg>0 );
    rc = SQLITE_ERROR;
    *pzErr = sqlite3_mprintf("no such tokenizer: %s", azArg[0]);
  }else{
    rc = pMod->x.xCreate(pMod->pUserData, &azArg[1], (nArg?nArg-1:0), ppTok);
    *ppTokApi = &pMod->x;
    if( rc!=SQLITE_OK && pzErr ){
      *pzErr = sqlite3_mprintf("error in tokenizer constructor");
    }
  }

  if( rc!=SQLITE_OK ){
    *ppTokApi = 0;
    *ppTok = 0;
  }

  return rc;
}

static void fts5ModuleDestroy(void *pCtx){
  Fts5TokenizerModule *pTok, *pNextTok;
  Fts5Auxiliary *pAux, *pNextAux;
  Fts5Global *pGlobal = (Fts5Global*)pCtx;

  for(pAux=pGlobal->pAux; pAux; pAux=pNextAux){
    pNextAux = pAux->pNext;
    if( pAux->xDestroy ) pAux->xDestroy(pAux->pUserData);
    sqlite3_free(pAux);
  }

  for(pTok=pGlobal->pTok; pTok; pTok=pNextTok){
    pNextTok = pTok->pNext;
    if( pTok->xDestroy ) pTok->xDestroy(pTok->pUserData);
    sqlite3_free(pTok);
  }

  sqlite3_free(pGlobal);
}

static void fts5Fts5Func(
  sqlite3_context *pCtx,          /* Function call context */
  int nArg,                       /* Number of args */
  sqlite3_value **apVal           /* Function arguments */
){
  Fts5Global *pGlobal = (Fts5Global*)sqlite3_user_data(pCtx);
  char buf[8];
  assert( nArg==0 );
  assert( sizeof(buf)>=sizeof(pGlobal) );
  memcpy(buf, (void*)&pGlobal, sizeof(pGlobal));
  sqlite3_result_blob(pCtx, buf, sizeof(pGlobal), SQLITE_TRANSIENT);
}

#ifdef _WIN32_
__declspec(dllexport)
#endif
int sqlite3_fts5_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
){
  static const sqlite3_module fts5Mod = {
    /* iVersion      */ 2,
    /* xCreate       */ fts5CreateMethod,
    /* xConnect      */ fts5ConnectMethod,
    /* xBestIndex    */ fts5BestIndexMethod,
    /* xDisconnect   */ fts5DisconnectMethod,
    /* xDestroy      */ fts5DestroyMethod,
    /* xOpen         */ fts5OpenMethod,
    /* xClose        */ fts5CloseMethod,
    /* xFilter       */ fts5FilterMethod,
    /* xNext         */ fts5NextMethod,
    /* xEof          */ fts5EofMethod,
    /* xColumn       */ fts5ColumnMethod,
    /* xRowid        */ fts5RowidMethod,
    /* xUpdate       */ fts5UpdateMethod,
    /* xBegin        */ fts5BeginMethod,
    /* xSync         */ fts5SyncMethod,
    /* xCommit       */ fts5CommitMethod,
    /* xRollback     */ fts5RollbackMethod,
    /* xFindFunction */ fts5FindFunctionMethod,
    /* xRename       */ fts5RenameMethod,
    /* xSavepoint    */ fts5SavepointMethod,
    /* xRelease      */ fts5ReleaseMethod,
    /* xRollbackTo   */ fts5RollbackToMethod,
  };

  int rc;
  Fts5Global *pGlobal = 0;

  SQLITE_EXTENSION_INIT2(pApi);

  pGlobal = (Fts5Global*)sqlite3_malloc(sizeof(Fts5Global));
  if( pGlobal==0 ){
    rc = SQLITE_NOMEM;
  }else{
    void *p = (void*)pGlobal;
    memset(pGlobal, 0, sizeof(Fts5Global));
    pGlobal->db = db;
    pGlobal->api.iVersion = 1;
    pGlobal->api.xCreateFunction = fts5CreateAux;
    pGlobal->api.xCreateTokenizer = fts5CreateTokenizer;
    pGlobal->api.xFindTokenizer = fts5FindTokenizer;
    rc = sqlite3_create_module_v2(db, "fts5", &fts5Mod, p, fts5ModuleDestroy);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5IndexInit(db);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5ExprInit(pGlobal, db);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5AuxInit(&pGlobal->api);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5TokenizerInit(&pGlobal->api);
    if( rc==SQLITE_OK ) rc = sqlite3Fts5VocabInit(pGlobal, db);
    if( rc==SQLITE_OK ){
      rc = sqlite3_create_function(
          db, "fts5", 0, SQLITE_UTF8, p, fts5Fts5Func, 0, 0
      );
    }
  }
  return rc;
}
#endif /* defined(SQLITE_ENABLE_FTS5) */

