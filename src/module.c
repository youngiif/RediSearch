#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>

#include "commands.h"
#include "document.h"
#include "tag_index.h"
#include "index.h"
#include "query.h"
#include "redis_index.h"
#include "redismodule.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"
#include "rmutil/args.h"
#include "spec.h"
#include "util/logging.h"
#include "config.h"
#include "aggregate/aggregate.h"
#include "aggregate/expr/attribute.h"
#include "rmalloc.h"
#include "cursor.h"
#include "version.h"
#include "debug_commads.h"
#include "spell_check.h"
#include "dictionary.h"
#include "suggest.h"
#include "numeric_index.h"
#include "redisearch_api.h"
#include "alias.h"
#include "module.h"
#include "info_command.h"
#include "result_processor.h"
#include "rules/rules.h"

pthread_rwlock_t RWLock = PTHREAD_RWLOCK_INITIALIZER;

#define LOAD_INDEX(ctx, srcname, write)                                                     \
  ({                                                                                        \
    IndexSpec *sptmp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(srcname, NULL), write); \
    if (sptmp == NULL) {                                                                    \
      return RedisModule_ReplyWithError(ctx, "Unknown index name");                         \
    }                                                                                       \
    sptmp;                                                                                  \
  })

/* FT.SETPAYLOAD {index} {docId} {payload} */
int SetPayloadCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {

  // nosave must be at place 4 and we must have at least 7 fields
  if (argc != 4) {
    return RedisModule_WrongArity(ctx);
  }
  RedisModule_ReplicateVerbatim(ctx);

  RedisModule_AutoMemory(ctx);

  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 1);
  if (sp == NULL) {
    RedisModule_ReplyWithError(ctx, "Unknown Index name");
    goto cleanup;
  }

  /* Find the document by its key */
  t_docId docId = DocTable_GetIdR(&sp->docs, argv[2]);
  if (docId == 0) {
    RedisModule_ReplyWithError(ctx, "Document not in index");
    goto cleanup;
  }

  size_t mdlen;
  const char *md = RedisModule_StringPtrLen(argv[3], &mdlen);

  if (DocTable_SetPayload(&sp->docs, docId, md, mdlen) == 0) {
    RedisModule_ReplyWithError(ctx, "Could not set payload ¯\\_(ツ)_/¯");
    goto cleanup;
  }

  RedisModule_ReplyWithSimpleString(ctx, "OK");
cleanup:

  return REDISMODULE_OK;
}

/* FT.MGET {index} {key} ...
 * Get document(s) by their id.
 * Currentlt it just performs HGETALL, but it's a future proof alternative allowing us to later on
 * replace the internal representation of the documents.
 *
 * If referred docs are missing or not HASH keys, we simply reply with Null, but the result will
 * be an array the same size of the ids list
 */
int GetDocumentsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }

  RedisSearchCtx *sctx = NewSearchCtx(ctx, argv[1], true);
  if (sctx == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }

  const DocTable *dt = &sctx->spec->docs;
  RedisModule_ReplyWithArray(ctx, argc - 2);
  for (size_t i = 2; i < argc; i++) {

    if (DocTable_GetIdR(dt, argv[i]) == 0) {
      // Document does not exist in index; even though it exists in keyspace
      RedisModule_ReplyWithNull(ctx);
      continue;
    }

    Document doc = {0};
    Document_Init(&doc, argv[i], 0, DEFAULT_LANGUAGE);
    if (Document_LoadAllFields(&doc, ctx) == REDISMODULE_ERR) {
      RedisModule_ReplyWithNull(ctx);
    } else {
      Document_ReplyFields(ctx, &doc);
      Document_Free(&doc);
    }
  }

  SearchCtx_Free(sctx);

  return REDISMODULE_OK;
}

/* FT.GET {index} {key} ...
 * Get a single document by their id.
 * Currentlt it just performs HGETALL, but it's a future proof alternative allowing us to later on
 * replace the internal representation of the documents.
 *
 * If referred docs are missing or not HASH keys, we simply reply with Null
 */
int GetSingleDocumentCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 3) {
    return RedisModule_WrongArity(ctx);
  }

  RedisSearchCtx *sctx = NewSearchCtx(ctx, argv[1], true);
  if (sctx == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }

  Document doc = {0};
  Document_Init(&doc, argv[2], 0, DEFAULT_LANGUAGE);

  if (DocTable_GetIdR(&sctx->spec->docs, argv[2]) == 0 ||
      Document_LoadAllFields(&doc, ctx) == REDISMODULE_ERR) {
    RedisModule_ReplyWithNull(ctx);
  } else {
    Document_ReplyFields(ctx, &doc);
    Document_Free(&doc);
  }
  SearchCtx_Free(sctx);
  return REDISMODULE_OK;
}

int SpellCheckCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
#define DICT_INITIAL_SIZE 5
#define DEFAULT_LEV_DISTANCE 1
#define MAX_LEV_DISTANCE 100
#define STRINGIFY(s) #s
  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }

  RedisModule_AutoMemory(ctx);
  RedisSearchCtx *sctx = NewSearchCtx(ctx, argv[1], true);
  if (sctx == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }

  QueryError status = {0};
  size_t len;
  const char *rawQuery = RedisModule_StringPtrLen(argv[2], &len);
  const char **includeDict = NULL, **excludeDict = NULL;
  RSSearchOptions opts = {0};
  QueryAST qast = {0};
  int rc = QAST_Parse(&qast, sctx, &opts, rawQuery, len, &status);

  if (rc != REDISMODULE_OK) {
    RedisModule_ReplyWithError(ctx, QueryError_GetError(&status));
    goto end;
  }

  includeDict = array_new(const char *, DICT_INITIAL_SIZE);
  excludeDict = array_new(const char *, DICT_INITIAL_SIZE);

  int distanceArgPos = 0;
  long long distance = DEFAULT_LEV_DISTANCE;
  if ((distanceArgPos = RMUtil_ArgExists("DISTANCE", argv, argc, 0))) {
    if (distanceArgPos + 1 >= argc) {
      RedisModule_ReplyWithError(ctx, "DISTANCE arg is given but no DISTANCE comes after");
      goto end;
    }
    if (RedisModule_StringToLongLong(argv[distanceArgPos + 1], &distance) != REDISMODULE_OK ||
        distance < 1 || distance > MAX_LEV_DISTANCE) {
      RedisModule_ReplyWithError(
          ctx, "bad distance given, distance must be a natural number between 1 to " STRINGIFY(
                   MAX_LEV_DISTANCE));
      goto end;
    }
  }  // LCOV_EXCL_LINE

  int nextPos = 0;
  while ((nextPos = RMUtil_ArgExists("TERMS", argv, argc, nextPos + 1))) {
    if (nextPos + 2 >= argc) {
      RedisModule_ReplyWithError(ctx, "TERM arg is given but no TERM params comes after");
      goto end;
    }
    const char *operation = RedisModule_StringPtrLen(argv[nextPos + 1], NULL);
    const char *dictName = RedisModule_StringPtrLen(argv[nextPos + 2], NULL);
    if (strcasecmp(operation, "INCLUDE") == 0) {
      includeDict = array_append(includeDict, (char *)dictName);
    } else if (strcasecmp(operation, "EXCLUDE") == 0) {
      excludeDict = array_append(excludeDict, (char *)dictName);
    } else {
      RedisModule_ReplyWithError(ctx, "bad format, exlude/include operation was not given");
      goto end;
    }
  }

  bool fullScoreInfo = false;
  if (RMUtil_ArgExists("FULLSCOREINFO", argv, argc, 0)) {
    fullScoreInfo = true;
  }

  SpellCheckCtx scCtx = {.sctx = sctx,
                         .includeDict = includeDict,
                         .excludeDict = excludeDict,
                         .distance = distance,
                         .fullScoreInfo = fullScoreInfo};

  SpellCheck_Reply(&scCtx, &qast);

end:
  QueryError_ClearError(&status);
  if (includeDict != NULL) {
    array_free(includeDict);
  }
  if (excludeDict != NULL) {
    array_free(excludeDict);
  }
  QAST_Destroy(&qast);
  SearchCtx_Free(sctx);
  return REDISMODULE_OK;
}

char *RS_GetExplainOutput(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                          QueryError *status);

static int queryExplainCommon(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                              int newlinesAsElements) {
  QueryError status = {0};
  char *explainRoot = RS_GetExplainOutput(ctx, argv, argc, &status);
  if (!explainRoot) {
    return QueryError_ReplyAndClear(ctx, &status);
  }
  if (newlinesAsElements) {
    size_t numElems = 0;
    RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
    char *explain = explainRoot;
    char *curLine = NULL;
    while ((curLine = strsep(&explain, "\n")) != NULL) {
      RedisModule_ReplyWithSimpleString(ctx, curLine);
      numElems++;
    }
    RedisModule_ReplySetArrayLength(ctx, numElems);
  } else {
    RedisModule_ReplyWithStringBuffer(ctx, explainRoot, strlen(explainRoot));
  }

  rm_free(explainRoot);
  return REDISMODULE_OK;
}

/* FT.EXPLAIN {index_name} {query} */
int QueryExplainCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return queryExplainCommon(ctx, argv, argc, 0);
}
int QueryExplainCLICommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  return queryExplainCommon(ctx, argv, argc, 1);
}

int RSAggregateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RSSearchCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
int RSCursorCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc);

/* FT.DEL {index} {doc_id}
 *  Delete a document from the index. Returns 1 if the document was in the index, or 0 if not.
 *
 *  **NOTE**: This does not actually delete the document from the index, just marks it as deleted
 * If DD (Delete Document) is set, we also delete the document.
 */
int DeleteCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RedisModule_AutoMemory(ctx);

  if (argc < 3 || argc > 4) return RedisModule_WrongArity(ctx);
  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 1);
  if (sp == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }
  if (sp->flags & Index_UseRules) {
    return RedisModule_ReplyWithError(
        ctx, "Cannot manually remove documents from index declared using `WITHRULES`");
  }

  int delDoc = 0;
  if (argc == 4 && RMUtil_StringEqualsCaseC(argv[3], "DD")) {
    delDoc = 1;
  }

  RedisSearchCtx sctx = SEARCH_CTX_STATIC(ctx, sp);
  RedisModuleString *docKey = argv[2];

  // Get the doc ID
  t_docId id = DocTable_GetIdR(&sp->docs, docKey);
  if (id == 0) {
    return RedisModule_ReplyWithLongLong(ctx, 0);
    // ID does not exist.
  }

  for (size_t i = 0; i < sp->numFields; ++i) {
    FieldSpec *fs = sp->fields + i;
    if (!FIELD_IS(fs, INDEXFLD_T_GEO)) {
      continue;
    }
    GeoIndex *gi = IDX_LoadGeo(sp, fs, REDISMODULE_WRITE);
    GeoIndex_RemoveEntries(gi, sctx.spec, id);
  }

  int rc = DocTable_DeleteR(&sp->docs, docKey);
  if (rc) {
    sp->stats.numDocuments--;

    // If needed - delete the actual doc
    if (delDoc) {

      RedisModuleKey *dk = RedisModule_OpenKey(ctx, argv[2], REDISMODULE_WRITE);
      if (dk && RedisModule_KeyType(dk) == REDISMODULE_KEYTYPE_HASH) {
        RedisModule_DeleteKey(dk);
      } else {
        RedisModule_Log(ctx, "warning", "Document %s doesn't exist",
                        RedisModule_StringPtrLen(argv[2], NULL));
      }
    }

    // Increment the index's garbage collector's scanning frequency after document deletions
    if (sp->gc) {
      GCContext_OnDelete(sp->gc);
    }
    if (!delDoc) {
      RedisModule_Replicate(ctx, RS_DEL_CMD, "cs", sp->name, argv[2]);
    } else {
      RedisModule_Replicate(ctx, RS_DEL_CMD, "csc", sp->name, argv[2], "dd");
    }
  }
  return RedisModule_ReplyWithLongLong(ctx, rc);
}

/* FT.TAGVALS {idx} {field}
 * Return all the values of a tag field.
 * There is no sorting or paging, so be careful with high-cradinality tag fields */
int TagValsCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  // at least one field, and number of field/text args must be even
  if (argc != 3) {
    return RedisModule_WrongArity(ctx);
  }

  RedisModule_AutoMemory(ctx);
  RedisSearchCtx *sctx = NewSearchCtx(ctx, argv[1], true);
  if (sctx == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }

  size_t len;
  const char *field = RedisModule_StringPtrLen(argv[2], &len);
  const FieldSpec *sp = IndexSpec_GetField(sctx->spec, field, len);
  if (!sp) {
    RedisModule_ReplyWithError(ctx, "No such field");
    goto cleanup;
  }
  if (!FIELD_IS(sp, INDEXFLD_T_TAG)) {
    RedisModule_ReplyWithError(ctx, "Not a tag field");
    goto cleanup;
  }
  TagIndex *idx = IDX_LoadTags(sctx->spec, sp, REDISMODULE_READ);
  if (!idx) {
    RedisModule_ReplyWithArray(ctx, 0);
    goto cleanup;
  }

  TagIndex_SerializeValues(idx, ctx);

cleanup:
  SearchCtx_Free(sctx);
  return REDISMODULE_OK;
}
/*
## FT.CREATE {index} [NOOFFSETS] [NOFIELDS]
    SCHEMA {field} [TEXT [NOSTEM] [WEIGHT {weight}]] | [NUMERIC] ...

Creates an index with the given spec. The index name will be used in all the
key
names
so keep it short!

### Parameters:

    - index: the index name to create. If it exists the old spec will be
overwritten

    - NOOFFSETS: If set, we do not store term offsets for documents (saves memory, does not allow
      exact searches)

    - NOFIELDS: If set, we do not store field bits for each term. Saves memory, does not allow
      filtering by specific fields.

    - SCHEMA: After the SCHEMA keyword we define the index fields. They can be either numeric or
      textual.
      For textual fields we optionally specify a weight. The default weight is 1.0
      The weight is a double, but does not need to be normalized.

### Returns:

    OK or an error
*/
int CreateIndexCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  // at least one field, the SCHEMA keyword, and number of field/text args must be even
  if (argc < 5) {
    return RedisModule_WrongArity(ctx);
  }

  if (RedisModule_GetSelectedDb(ctx) != 0) {
    return RedisModule_ReplyWithError(ctx, "Cannot create index on db != 0");
  }

  QueryError status = {0};
  IndexCreateOptions opts = {0};
  ArgsCursor ac = {0};
  ArgsCursor_InitRString(&ac, argv + 2, argc - 2);
  const char *name = RedisModule_StringPtrLen(argv[1], NULL);
  IndexSpec *sp = IndexSpec_ParseArgs(name, &ac, &opts, &status);
  if (sp == NULL) {
    return QueryError_ReplyAndClear(ctx, &status);
  }
  if (IndexSpec_Register(sp, &opts, &status) != REDISMODULE_OK) {
    IndexSpec_Free(sp);
    return QueryError_ReplyAndClear(ctx, &status);
  }

  RedisModule_Replicate(ctx, RS_CREATE_CMD, "ccv", sp->name, "REPLACE", argv + 2, (size_t)argc - 2);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* FT.OPTIMIZE <index>
 *  After the index is built (and doesn't need to be updated again withuot a
 * complete rebuild)
 *  we can optimize memory consumption by trimming all index buffers to their
 * actual size.
 *
 *  Warning 1: This will delete score indexes for small words (n < 5000), so
 * updating the index
 * after
 *  optimizing it might lead to screwed up results (TODO: rebuild score indexes
 * if needed).
 *  The simple solution to that is to call optimize again after adding
 * documents
 * to the index.
 *
 *  Warning 2: This blocks redis for a long time. Do not run it on production
 * instances
 *
 */
int OptimizeIndexCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  // at least one field, and number of field/text args must be even
  if (argc != 2) {
    return RedisModule_WrongArity(ctx);
  }

  RedisModule_AutoMemory(ctx);

  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 0);
  if (sp == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }

  // DEPRECATED - we now don't do anything.  The GC optimizes the index in the background
  return RedisModule_ReplyWithLongLong(ctx, 0);
}

/*
 * FT.DROP <index> [KEEPDOCS]
 * Deletes all the keys associated with the index.
 * If no other data is on the redis instance, this is equivalent to FLUSHDB,
 * apart from the fact that the index specification is not deleted.
 *
 * If KEEPDOCS exists, we do not delete the actual docs
 */
int DropIndexCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  // at least one field, and number of field/text args must be even
  if (argc < 2 || argc > 3) {
    return RedisModule_WrongArity(ctx);
  }
  RedisModule_ReplicateVerbatim(ctx);

  RedisModule_AutoMemory(ctx);
  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 0);
  if (sp == NULL) {
    return RedisModule_ReplyWithError(ctx, "Unknown Index name");
  }

  // Optional KEEPDOCS
  int options = IDXFREE_F_DELDOCS;
  if (argc == 3 && RMUtil_StringEqualsCaseC(argv[2], "KEEPDOCS")) {
    options = 0;
  }

  IndexSpec_FreeEx(sp, options);
  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/**
 * FT.SYNADD <index> <term1> <term2> ...
 *
 * Add a synonym group to the given index. The synonym data structure is compose of synonyms
 * groups. Each Synonym group has a unique id. The SYNADD command creates a new synonym group with
 * the given terms and return its id.
 */
int SynAddCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 3) return RedisModule_WrongArity(ctx);

  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 0);
  if (!sp) {
    RedisModule_ReplyWithError(ctx, "Unknown index name");
    return REDISMODULE_OK;
  }

  RedisModule_ReplicateVerbatim(ctx);

  IndexSpec_InitializeSynonym(sp);

  uint32_t id = SynonymMap_AddRedisStr(sp->smap, argv + 2, argc - 2);

  RedisModule_ReplyWithLongLong(ctx, id);

  return REDISMODULE_OK;
}

int SynUpdateCommandInternal(RedisModuleCtx *ctx, RedisModuleString *indexName, long long id,
                             RedisModuleString **synonyms, size_t size, bool checkIdSanity) {
  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(indexName, NULL), 0);
  if (!sp) {
    RedisModule_ReplyWithError(ctx, "Unknown index name");
    return REDISMODULE_OK;
  }

  if (checkIdSanity && (!sp->smap || id >= SynonymMap_GetMaxId(sp->smap))) {
    RedisModule_ReplyWithError(ctx, "given id does not exists");
    return REDISMODULE_OK;
  }

  IndexSpec_InitializeSynonym(sp);

  SynonymMap_UpdateRedisStr(sp->smap, synonyms, size, id);

  RedisModule_ReplyWithSimpleString(ctx, "OK");

  return REDISMODULE_OK;
}

/**
 * FT.SYNUPDATE <index> <id> <term1> <term2> ...
 *
 * Update an already existing synonym group with the given terms.
 * Its only to add new terms to a synonym group.
 * return true on success.
 */
int SynUpdateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);

  long long id;
  if (RedisModule_StringToLongLong(argv[2], &id) != REDISMODULE_OK) {
    RedisModule_ReplyWithError(ctx, "wrong parameters, id is not an integer");
    return REDISMODULE_OK;
  }

  if (id < 0 || id > UINT32_MAX) {
    RedisModule_ReplyWithError(ctx, "wrong parameters, id out of range");
    return REDISMODULE_OK;
  }

  RedisModule_ReplicateVerbatim(ctx);

  return SynUpdateCommandInternal(ctx, argv[1], id, argv + 3, argc - 3, true);
}

int SynForceUpdateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 4) return RedisModule_WrongArity(ctx);

  long long id;
  if (RedisModule_StringToLongLong(argv[2], &id) != REDISMODULE_OK) {
    RedisModule_ReplyWithError(ctx, "wrong parameters, id is not an integer");
    return REDISMODULE_OK;
  }

  if (id < 0 || id > UINT32_MAX) {
    RedisModule_ReplyWithError(ctx, "wrong parameters, id out of range");
    return REDISMODULE_OK;
  }

  RedisModule_ReplicateVerbatim(ctx);

  return SynUpdateCommandInternal(ctx, argv[1], id, argv + 3, argc - 3, false);
}

/**
 * FT.SYNDUMP <index>
 *
 * Dump the synonym data structure in the following format:
 *    - term1
 *        - id1
 *        - id2
 *    - term2
 *        - id3
 *    - term3
 *        - id4
 */
int SynDumpCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc < 2) return RedisModule_WrongArity(ctx);

  IndexSpec *sp = IndexSpec_Load(ctx, RedisModule_StringPtrLen(argv[1], NULL), 0);
  if (!sp) {
    RedisModule_ReplyWithError(ctx, "Unknown index name");
    return REDISMODULE_OK;
  }

  if (!sp->smap) {
    RedisModule_ReplyWithArray(ctx, 0);
    return REDISMODULE_OK;
  }

  size_t size;
  TermData **terms_data = SynonymMap_DumpAllTerms(sp->smap, &size);

  RedisModule_ReplyWithArray(ctx, size * 2);

  for (int i = 0; i < size; ++i) {
    TermData *t_data = terms_data[i];
    RedisModule_ReplyWithStringBuffer(ctx, t_data->term, strlen(t_data->term));
    RedisModule_ReplyWithArray(ctx, array_len(t_data->ids));
    for (size_t j = 0; j < array_len(t_data->ids); ++j) {
      RedisModule_ReplyWithLongLong(ctx, t_data->ids[j]);
    }
  }

  rm_free(terms_data);

  return REDISMODULE_OK;
}

int AlterIndexCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  ArgsCursor ac = {0};
  ArgsCursor_InitRString(&ac, argv + 1, argc - 1);

  // Need at least <cmd> <index> <subcommand> <args...>
  RedisModule_AutoMemory(ctx);

  if (argc < 5) {
    return RedisModule_WrongArity(ctx);
  }
  QueryError status = {0};

  const char *ixname = AC_GetStringNC(&ac, NULL);
  IndexSpec *sp = IndexSpec_Load(ctx, ixname, 1);
  if (!sp) {
    return RedisModule_ReplyWithError(ctx, "Unknown index name");
  }

  if (AC_AdvanceIfMatch(&ac, "SCHEMA")) {
    if (!AC_AdvanceIfMatch(&ac, "ADD")) {
      return RedisModule_ReplyWithError(ctx, "Unknown action passed to ALTER SCHEMA");
    }
    if (!AC_NumRemaining(&ac)) {
      return RedisModule_ReplyWithError(ctx, "No fields provided");
    }
    IndexSpec_AddFields(sp, &ac, &status);
  }

  if (QueryError_HasError(&status)) {
    return QueryError_ReplyAndClear(ctx, &status);
  } else {
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
}

static int aliasAddCommon(RedisModuleCtx *ctx, RedisModuleString **argv, int argc,
                          QueryError *error) {
  ArgsCursor ac = {0};
  ArgsCursor_InitRString(&ac, argv + 1, argc - 1);
  IndexLoadOptions loadOpts = {.name = {.rstring = argv[2]},
                               .flags = INDEXSPEC_LOAD_NOALIAS | INDEXSPEC_LOAD_KEY_RSTRING};
  IndexSpec *sptmp = IndexSpec_LoadEx(ctx, &loadOpts);
  if (!sptmp) {
    QueryError_SetError(error, QUERY_ENOINDEX, "Unknown index name (or name is an alias itself)");
    return REDISMODULE_ERR;
  }
  return IndexAlias_Add(RedisModule_StringPtrLen(argv[1], NULL), sptmp, 0, error);
}

// FT.ALIASADD <NAME> <TARGET>
static int AliasAddCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 3) {
    return RedisModule_WrongArity(ctx);
  }
  QueryError e = {0};
  if (aliasAddCommon(ctx, argv, argc, &e) != REDISMODULE_OK) {
    return QueryError_ReplyAndClear(ctx, &e);
  } else {
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
}

static int AliasDelCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 2) {
    return RedisModule_WrongArity(ctx);
  }
  IndexLoadOptions lOpts = {.name = {.rstring = argv[1]}, .flags = INDEXSPEC_LOAD_KEY_RSTRING};
  IndexSpec *sp = IndexSpec_LoadEx(ctx, &lOpts);
  if (!sp) {
    return RedisModule_ReplyWithError(ctx, "Alias does not exist");
  }
  QueryError status = {0};
  if (IndexAlias_Del(RedisModule_StringPtrLen(argv[1], NULL), sp, 0, &status) != REDISMODULE_OK) {
    return QueryError_ReplyAndClear(ctx, &status);
  } else {
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
}

static int AliasUpdateCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  if (argc != 3) {
    return RedisModule_WrongArity(ctx);
  }

  QueryError status = {0};
  IndexLoadOptions lOpts = {.name = {.rstring = argv[1]}, .flags = INDEXSPEC_LOAD_KEY_RSTRING};
  IndexSpec *spOrig = IndexSpec_LoadEx(ctx, &lOpts);
  if (spOrig) {
    if (IndexAlias_Del(RedisModule_StringPtrLen(argv[1], NULL), spOrig, 0, &status) !=
        REDISMODULE_OK) {
      return QueryError_ReplyAndClear(ctx, &status);
    }
  }
  if (aliasAddCommon(ctx, argv, argc, &status) != REDISMODULE_OK) {
    // Add back the previous index.. this shouldn't fail
    if (spOrig) {
      QueryError e2 = {0};
      const char *alias = RedisModule_StringPtrLen(argv[1], NULL);
      IndexAlias_Add(alias, spOrig, 0, &e2);
      QueryError_ClearError(&e2);
    }
    return QueryError_ReplyAndClear(ctx, &status);
  } else {
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
}

static int RuleAddCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  ArgsCursor ac = {0};
  ArgsCursor_InitRString(&ac, argv + 1, argc - 1);
  // INDEX RULENAME MATCHTYPE MATCHEXPR [ACTION ACTION-PARAMS]
  if (AC_NumRemaining(&ac) < 4) {
    return RedisModule_WrongArity(ctx);
  }
  const char *idxstr = AC_GetStringNC(&ac, NULL);
  const char *name = AC_GetStringNC(&ac, NULL);
  QueryError err = {0};
  int rc = SchemaRules_AddArgs(idxstr, name, &ac, &err);
  if (rc != REDISMODULE_OK) {
    RedisModule_ReplyWithError(ctx, QueryError_GetError(&err));
    QueryError_ClearError(&err);
  } else {
    RedisModule_ReplyWithSimpleString(ctx, "OK");
  }
  return REDISMODULE_OK;
}

static int RulesSetCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  ArgsCursor ac = {0};
  ArgsCursor_InitRString(&ac, argv + 1, argc - 1);
  QueryError status = {0};
  int rc = SchemaRules_SetArgs(&ac, &status);
  if (rc != REDISMODULE_OK) {
    QueryError_ReplyAndClear(ctx, &status);
  }
  return REDISMODULE_OK;
}

int ConfigCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  // Not bound to a specific index, so...
  RedisModule_AutoMemory(ctx);
  QueryError status = {0};

  // CONFIG <GET|SET> <NAME> [value]
  if (argc < 3) {
    return RedisModule_WrongArity(ctx);
  }
  const char *action = RedisModule_StringPtrLen(argv[1], NULL);
  const char *name = RedisModule_StringPtrLen(argv[2], NULL);
  if (!strcasecmp(action, "GET")) {
    RSConfig_DumpProto(&RSGlobalConfig, &RSGlobalConfigOptions, name, ctx, 0);
  } else if (!strcasecmp(action, "HELP")) {
    RSConfig_DumpProto(&RSGlobalConfig, &RSGlobalConfigOptions, name, ctx, 1);
  } else if (!strcasecmp(action, "SET")) {
    size_t offset = 3;  // Might be == argc. SetOption deals with it.
    if (RSConfig_SetOption(&RSGlobalConfig, &RSGlobalConfigOptions, name, argv, argc, &offset,
                           &status) == REDISMODULE_ERR) {
      RedisModule_ReplyWithSimpleString(ctx, QueryError_GetError(&status));
      return REDISMODULE_OK;
    }
    if (offset != argc) {
      RedisModule_ReplyWithSimpleString(ctx, "EXCESSARGS");
    } else {
      RedisModule_Log(ctx, "notice", "Successfully changed configuration for `%s`", name);
      RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    return REDISMODULE_OK;
  } else {
    RedisModule_ReplyWithSimpleString(ctx, "No such configuration action");
    return REDISMODULE_OK;
  }

  return REDISMODULE_OK;
}

void ShardingEvent(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent, void *data) {
  /**
   * On sharding event we need to do couple of things depends on the subevent given:
   *
   * 1. REDISMODULE_SUBEVENT_SHARDING_SLOT_RANGE_CHANGED
   *    On this event we know that the slot range changed and we might have data
   *    which are no longer belong to this shard, we must ignore it on searches
   *
   * 2. REDISMODULE_SUBEVENT_SHARDING_TRIMMING_STARTED
   *    This event tells us that the trimming process has started and keys will start to be
   *    deleted, we do not need to do anything on this event
   *
   * 3. REDISMODULE_SUBEVENT_SHARDING_TRIMMING_ENDED
   *    This event tells us that the trimming process has finished, we are not longer
   *    have data that are not belong to us and its safe to stop checking this on searches.
   */
  if (eid.id != REDISMODULE_EVENT_SHARDING) {
    RedisModule_Log(RSDummyContext, "warning", "Bad event given, ignored.");
    return;
  }

  switch (subevent) {
    case REDISMODULE_SUBEVENT_SHARDING_SLOT_RANGE_CHANGED:
      RedisModule_Log(NULL, "notice", "Got slot changed event");
      verifyDocumentSlotRange = true;
      break;
    case REDISMODULE_SUBEVENT_SHARDING_TRIMMING_STARTED:
      RedisModule_Log(NULL, "notice", "Got trim started event");
      break;
    case REDISMODULE_SUBEVENT_SHARDING_TRIMMING_ENDED:
      RedisModule_Log(NULL, "notice", "Got trim ended event");
      verifyDocumentSlotRange = false;
      break;
    default:
      RedisModule_Log(RSDummyContext, "warning", "Bad subevent given, ignored.");
  }
}

#define RM_TRY(f, ...)                                                         \
  if (f(__VA_ARGS__) == REDISMODULE_ERR) {                                     \
    RedisModule_Log(ctx, "warning", "Could not run " #f "(" #__VA_ARGS__ ")"); \
    return REDISMODULE_ERR;                                                    \
  } else {                                                                     \
    RedisModule_Log(ctx, "verbose", "Successfully executed " #f);              \
  }

int RediSearch_InitModuleInternal(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  char *err;
  if (ReadConfig(argv, argc, &err) == REDISMODULE_ERR) {
    RedisModule_Log(ctx, "warning", "Invalid Configurations: %s", err);
    rm_free(err);
    return REDISMODULE_ERR;
  }
  if (RediSearch_Init(ctx, REDISEARCH_INIT_MODULE) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  if (RedisModule_SubscribeToServerEvent) {
    // we have server events support, lets subscribe to relevan events.
    RedisModule_Log(NULL, "notice", "Subscribing to shards server events");
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Sharding, ShardingEvent);
  }

#ifndef RS_COORDINATOR
#define FIRST_KEY 1
#define LAST_KEY 1
#define STEPS 1
#else
#define FIRST_KEY 0
#define LAST_KEY 0
#define STEPS -1
#endif

  // register trie type
  RM_TRY(TrieType_Register, ctx);

  RM_TRY(IndexSpec_RegisterType, ctx);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_ADD_CMD, RSAddDocumentCommand, "write deny-oom", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SAFEADD_CMD, RSSafeAddDocumentCommand, "write deny-oom",
          FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SETPAYLOAD_CMD, SetPayloadCommand, "write deny-oom", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_ADDHASH_CMD, RSAddHashCommand, "write deny-oom", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SAFEADDHASH_CMD, RSSafeAddHashCommand, "write deny-oom",
          FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_DEL_CMD, DeleteCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SEARCH_CMD, RSSearchCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);
  RM_TRY(RedisModule_CreateCommand, ctx, RS_AGGREGATE_CMD, RSAggregateCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_GET_CMD, GetSingleDocumentCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_MGET_CMD, GetDocumentsCommand, "readonly", 0, 0, -1);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_CREATE_CMD, CreateIndexCommand, "write deny-oom", FIRST_KEY, LAST_KEY, STEPS);
  RM_TRY(RedisModule_CreateCommand, ctx, RS_CMD_PREFIX ".OPTIMIZE", OptimizeIndexCommand,
         "write deny-oom", FIRST_KEY, LAST_KEY, STEPS);
  RM_TRY(RedisModule_CreateCommand, ctx, RS_DROP_CMD, DropIndexCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_INFO_CMD, IndexInfoCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_TAGVALS_CMD, TagValsCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_EXPLAIN_CMD, QueryExplainCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);
  RM_TRY(RedisModule_CreateCommand, ctx, RS_EXPLAINCLI_CMD, QueryExplainCLICommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SUGADD_CMD, RSSuggestAddCommand, "write deny-oom", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SUGDEL_CMD, RSSuggestDelCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SUGLEN_CMD, RSSuggestLenCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SUGGET_CMD, RSSuggestGetCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

#ifndef RS_COORDINATOR
  RM_TRY(RedisModule_CreateCommand, ctx, RS_CURSOR_CMD, RSCursorCommand, "readonly", 2, 2, 1);
#else
  RM_TRY(RedisModule_CreateCommand, ctx, RS_CURSOR_CMD, RSCursorCommand, "readonly", 0, 0, -1);
#endif

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SYNADD_CMD, SynAddCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SYNUPDATE_CMD, SynUpdateCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SYNFORCEUPDATE_CMD, SynForceUpdateCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SYNDUMP_CMD, SynDumpCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALTER_CMD, AlterIndexCommand, "write", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_DEBUG, DebugCommand, "readonly", 0, 0, 0);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_SPELL_CHECK, SpellCheckCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_DICT_ADD, DictAddCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_DICT_DEL, DictDelCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_DICT_DUMP, DictDumpCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_CONFIG, ConfigCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_RULEADD, RuleAddCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_RULESET, RulesSetCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

#ifndef RS_COORDINATOR
  // we are running in a normal mode so we should raise cross slot error on alias commands
  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALIASADD, AliasAddCommand, "readonly", 1, 2, 1);
  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALIASUPDATE, AliasUpdateCommand, "readonly", 1, 2, 1);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALIASDEL, AliasDelCommand, "readonly", 1, 1, 1);
#else
  // Cluster is manage outside of module lets trust it and not raise cross slot error.
  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALIASADD, AliasAddCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);
  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALIASUPDATE, AliasUpdateCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);

  RM_TRY(RedisModule_CreateCommand, ctx, RS_ALIASDEL, AliasDelCommand, "readonly", FIRST_KEY, LAST_KEY, STEPS);
#endif
  return REDISMODULE_OK;
}

void __attribute__((destructor)) RediSearch_CleanupModule(void) {
  static int invoked = 0;
  if (!getenv("RS_GLOBAL_DTORS") || invoked || !RS_Initialized) {
    return;
  }
  invoked = 1;
  IndexSpec_CleanAll();
  SchemaRules_ShutdownGlobal();
  CursorList_Destroy(&RSCursors);
  Extensions_Free();
  StopWordList_FreeGlobals();
  FunctionRegistry_Free();
  mempool_free_global();
  IndexAlias_DestroyGlobal();
  Expr_AttributesDestroy();
  RedisModule_FreeThreadSafeContext(RSDummyContext);
}
