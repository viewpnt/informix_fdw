/*-------------------------------------------------------------------------
 *
 * ifx_fdw.c
 *		  foreign-data wrapper for IBM INFORMIX databases
 *
 * Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  informix_fdw/ifx_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "access/reloptions.h"
#include "catalog/indexing.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "mb/pg_wchar.h"
#include "optimizer/cost.h"
#include "storage/fd.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "ifx_fdw.h"
#include "ifx_conncache.h"

PG_MODULE_MAGIC;

/*
 * Object options using this wrapper module
 */
struct IfxFdwOption
{
	const char *optname;
	Oid         optcontext;
};

/*
 * Valid options for informix_fdw.
 */
static struct IfxFdwOption ifx_valid_options[] =
{
	{ "informixserver",   ForeignServerRelationId },
	{ "user",             UserMappingRelationId },
	{ "password",         UserMappingRelationId },
	{ "database",         ForeignTableRelationId },
	{ "query",            ForeignTableRelationId },
	{ "table",            ForeignTableRelationId },
	{ "estimated_rows",   ForeignTableRelationId },
	{ "connection_costs", ForeignTableRelationId },
	{ NULL,               ForeignTableRelationId }
};

/*
 * informix_fdw handler and validator function
 */
extern Datum ifx_fdw_handler(PG_FUNCTION_ARGS);
extern Datum ifx_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ifx_fdw_handler);
PG_FUNCTION_INFO_V1(ifx_fdw_validator);

/*******************************************************************************
 * FDW callback routines.
 */

static FdwPlan *ifxPlanForeignScan(Oid foreignTableOid,
								   PlannerInfo *planInfo,
								   RelOptInfo *baserel);

static void
ifxExplainForeignScan(ForeignScanState *node, ExplainState *es);

static void
ifxBeginForeignScan(ForeignScanState *node, int eflags);

static TupleTableSlot *ifxIterateForeignScan(ForeignScanState *node);

/*******************************************************************************
 * FDW helper functions.
 */
static StringInfoData *
ifxFdwOptionsToStringBuf(Oid context);

static bool
ifxIsValidOption(const char *option, Oid context);

static void
ifxGetOptions(Oid foreigntableOid, IfxConnectionInfo *coninfo);

static StringInfoData *
ifxGetDatabaseString(IfxConnectionInfo *coninfo);

static StringInfoData *
ifxGenerateConnName(IfxConnectionInfo *coninfo);

static void
ifxGetOptionDups(IfxConnectionInfo *coninfo, DefElem *def);

static void ifxConnInfoSetDefaults(IfxConnectionInfo *coninfo);

static IfxConnectionInfo *ifxMakeConnectionInfo(Oid foreignTableOid);

static char *ifxGenStatementName(IfxConnectionInfo *coninfo);

static char *ifxGenCursorName(IfxConnectionInfo *coninfo);

static void ifxPgColumnData(Oid foreignTableOid, IfxFdwExecutionState *festate);

static IfxFdwExecutionState *makeIfxFdwExecutionState();

static int ifxCatchExceptions(IfxStatementInfo *state);

/*******************************************************************************
 * Implementation starts here
 */

/*
 * Trap errors from the informix FDW API.
 *
 * This function checks exceptions from ESQL
 * and creates corresponding NOTICE, WARN or ERROR
 * messages.
 *
 */
static int ifxCatchExceptions(IfxStatementInfo *state)
{
	IfxSqlStateClass errclass;

	/*
	 * Set last error, if any
	 */
	errclass = ifxSetException(state);

	if (errclass != IFX_SUCCESS)
	{
		switch (errclass)
		{
			case IFX_RT_ERROR:
				/* log FATAL */
				break;
			case IFX_WARNING:
				/* log WARN */
				break;
			case IFX_ERROR:
				/* log ERROR */
				break;
			case IFX_NOT_FOUND:
			default:
				/* needs no log */
				break;
		}
	}

	return errclass;
}

/*
 * Returns a fully initialized pointer to
 * an IfxFdwExecutionState structure. All pointers
 * are initialized to NULL.
 */
static IfxFdwExecutionState *makeIfxFdwExecutionState()
{
	IfxFdwExecutionState *state = palloc(sizeof(IfxFdwExecutionState));

	bzero(state->stmt_info.conname, IFX_CONNAME_LEN + 1);
	state->stmt_info.cursorUsage = IFX_DEFAULT_CURSOR;

	state->stmt_info.query        = NULL;
	state->stmt_info.cursor_name  = NULL;
	state->stmt_info.stmt_name    = NULL;
	state->stmt_info.ifxAttrCount = 0;
	state->stmt_info.ifxAttrDefs  = NULL;

	bzero(state->stmt_info.sqlstate, 6);
	state->stmt_info.exception_count = 0;

	state->pgAttrCount = 0;
	state->pgAttrDefs  = NULL;
	state->values = NULL;

	return state;
}

/*
 * Retrieve the local column definition of the
 * foreign table (attribute number, type and additional
 * options).
 */
static void ifxPgColumnData(Oid foreignTableOid, IfxFdwExecutionState *festate)
{
	HeapTuple         tuple;
	Relation          attrRel;
	SysScanDesc       scan;
	ScanKeyData       key[2];
	Form_pg_attribute attrTuple;
	Relation          foreignRel;
	int               attrIndex;

	attrIndex = 0;

	/* open foreign table, should be locked already */
	foreignRel = heap_open(foreignTableOid, NoLock);
	festate->pgAttrCount = RelationGetNumberOfAttributes(foreignRel);
	heap_close(foreignRel, NoLock);

	festate->pgAttrDefs = palloc(sizeof(PgAttrDef) * (festate->pgAttrCount + 1));
	festate->pgAttrDefs[festate->pgAttrCount + 1] = NULL;

	/*
	 * Get all attributes for the given foreign table.
	 */
	attrRel = heap_open(AttributeRelationId, AccessShareLock);
	ScanKeyInit(&key[0], Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(foreignTableOid));
	ScanKeyInit(&key[1], Anum_pg_attribute_attnum,
				BTGreaterStrategyNumber, F_INT2GT,
				Int16GetDatum((int2)0));
	scan = systable_beginscan(attrRel, AttributeRelidNumIndexId, true,
							  SnapshotNow, 2, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		/* don't rely on attnum directly */
		++attrIndex;
		attrTuple = (Form_pg_attribute) GETSTRUCT(tuple);

		/* Check for dropped columns. Any match is recorded
		 * by setting the corresponding column slot in pgAttrDefs
		 * to NULL.
		 */
		if (attrTuple->attisdropped)
		{
			festate->pgAttrDefs[attrIndex - 1] = NULL;
			continue;
		}

		/*
		 * Protect against corrupted numbers in pg_class.relnatts
		 * and number of attributes retrieved from pg_attribute.
		 */
		if (attrIndex >= festate->pgAttrCount)
		{
			systable_endscan(scan);
			heap_close(attrRel, AccessShareLock);
			elog(ERROR, "unexpected number of attributes in foreign table");
		}

		/*
		 * Save the attribute and all required properties for
		 * later usage.
		 */
		festate->pgAttrDefs[attrIndex - 1]->attnum = attrTuple->attnum;
		festate->pgAttrDefs[attrIndex - 1]->atttypid = attrTuple->atttypid;
		festate->pgAttrDefs[attrIndex - 1]->atttypmod = attrTuple->atttypmod;
		festate->pgAttrDefs[attrIndex - 1]->attname = pstrdup(NameStr(attrTuple->attname));
	}

	systable_endscan(scan);
	heap_close(foreignRel, AccessShareLock);
}

/*
 * Checks for duplicate and redundant options.
 *
 * Check for redundant options. Error out in case we've found
 * any duplicates or, in case it is an empty option, assign
 * it to the connection info.
 */
static void
ifxGetOptionDups(IfxConnectionInfo *coninfo, DefElem *def)
{
	Assert(coninfo != NULL);

	if (strcmp(def->defname, "servername") == 0)
	{
		if (coninfo->servername)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: servername(%s)",
								   defGetString(def))));

		coninfo->servername = defGetString(def);
	}

	if (strcmp(def->defname, "database") == 0)
	{
		if (coninfo->database)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: database(%s)",
								   defGetString(def))));

		coninfo->database = defGetString(def);
	}

	if (strcmp(def->defname, "username") == 0)
	{
		if (coninfo->database)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: username(%s)",
								   defGetString(def))));

		coninfo->username = defGetString(def);
	}

	if (strcmp(def->defname, "password") == 0)
	{
		if (coninfo->password)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: password(%s)",
								   defGetString(def))));

		coninfo->password = defGetString(def);
	}

	if (strcmp(def->defname, "query") == 0)
	{
		if (coninfo->tablename)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting options: query cannot be used with table")
						));

		if (coninfo->query)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("conflicting or redundant options: query (%s)", defGetString(def))
						));

		coninfo->tablename = defGetString(def);
	}

	if (strcmp(def->defname, "table") == 0)
	{
		if (coninfo->query)
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("conflicting options: query cannot be used with query")));

		if (coninfo->tablename)
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("conflicting or redundant options: table(%s)",
								   defGetString(def))));

		coninfo->tablename = defGetString(def);
	}

	/*
	 * Check wether cost parameters are already set.
	 */
	if (strcmp(def->defname, "estimated_rows") == 0)
	{
		/*
		 * Try to convert the cost value into a double value.
		 */
		char *endp;
		char *val;

		val = defGetString(def);
		coninfo->planData.estimated_rows = strtof(val, &endp);

		if (val == endp && coninfo->planData.estimated_rows < 0.0)
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("\"%s\" is not a valid number for parameter \"estimated_rows\"",
								   val)));

	}

	if (strcmp(def->defname, "connection_costs") == 0)
	{
		/*
		 * Try to convert the cost value into a double value
		 */
		char *endp;
		char *val;

		val = defGetString(def);
		coninfo->planData.connection_costs = strtof(val, &endp);

		if (val == endp && coninfo->planData.connection_costs < 0)
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("\"%s\" is not a valid number for parameter \"estimated_rows\"",
								   val)));
	}

}

/*
 * Returns the database connection string
 * as 'dbname@servername'
 */
static StringInfoData *
ifxGetDatabaseString(IfxConnectionInfo *coninfo)
{
	StringInfoData *buf;

	buf = makeStringInfo();
	initStringInfo(buf);
	appendStringInfo(buf, "%s@%s", coninfo->database, coninfo->servername);

	return buf;
}

/*
 * Create a unique name for the database connection.
 *
 * Currently the name is generated by concatenating the
 * database name, server name and user into a single string.
 */
static StringInfoData *
ifxGenerateConnName(IfxConnectionInfo *coninfo)
{
	StringInfoData *buf;

	buf = makeStringInfo();
	initStringInfo(buf);
	appendStringInfo(buf, "%s-%s-%s", coninfo->username, coninfo->database,
					 coninfo->servername);

	return buf;
}

Datum
ifx_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

	fdwRoutine->PlanForeignScan    = ifxPlanForeignScan;
	fdwRoutine->ExplainForeignScan = ifxExplainForeignScan;
	fdwRoutine->BeginForeignScan   = ifxBeginForeignScan;
	fdwRoutine->IterateForeignScan = ifxIterateForeignScan;
	fdwRoutine->ReScanForeignScan  = NULL;
	fdwRoutine->EndForeignScan     = NULL;

	PG_RETURN_POINTER(fdwRoutine);
}

/*
 * Validate options passed to the INFORMIX FDW (that are,
 * FOREIGN DATA WRAPPER, SERVER, USER MAPPING and FOREIGN TABLE)
 */
Datum
ifx_fdw_validator(PG_FUNCTION_ARGS)
{
	List     *ifx_options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid       catalogOid = PG_GETARG_OID(1);
	IfxConnectionInfo coninfo = {0};
	ListCell *cell;

	/*
	 * Check options passed to this FDW. Validate values and required
	 * arguments.
	 */
	foreach(cell, ifx_options_list)
	{
		DefElem *def = (DefElem *) lfirst(cell);

		/*
		 * Unknown option specified, print an error message
		 * and a hint message what's wrong.
		 */
		if (!ifxIsValidOption(def->defname, catalogOid))
		{
			StringInfoData *buf;

			buf = ifxFdwOptionsToStringBuf(catalogOid);

			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("Valid options in this context are: %s", buf->len ? buf->data : "<none>")
						));
		}

		ifxGetOptionDups(&coninfo, def);
	}

	PG_RETURN_VOID();
}

/*
 * Retrieves options for ifx_fdw foreign data wrapper.
 */
 static void
ifxGetOptions(Oid foreigntableOid, IfxConnectionInfo *coninfo)
{
	ForeignTable  *foreignTable;
	ForeignServer *foreignServer;
	UserMapping   *userMap;
	List          *options;
	ListCell      *elem;

	Assert(coninfo != NULL);

	foreignTable  = GetForeignTable(foreigntableOid);
	foreignServer = GetForeignServer(foreignTable->serverid);
	userMap       = GetUserMapping(GetUserId(), foreignTable->serverid);

	options = NIL;
	options = list_concat(options, foreignTable->options);
	options = list_concat(options, foreignServer->options);
	options = list_concat(options, userMap->options);

	/*
	 * Retrieve required arguments.
	 */
	foreach(elem, options)
	{
		DefElem *def = (DefElem *) lfirst(elem);

		elog(DEBUG1, "ifx_fdw set param %s=%s",
			 def->defname, defGetString(def));

		/*
		 * "servername" defines the INFORMIXSERVER to connect to
		 */
		if (strcmp(def->defname, "informixserver") == 0)
		{
			coninfo->servername = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "database") == 0)
		{
			coninfo->database = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "username") == 0)
		{
			coninfo->username = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "password") == 0)
		{
			coninfo->password = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "table") == 0)
		{
			coninfo->tablename = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "query") == 0)
		{
			coninfo->query = pstrdup(defGetString(def));
		}

		if (strcmp(def->defname, "estimated_rows") == 0)
		{
			char *val;

			val = defGetString(def);
			coninfo->planData.estimated_rows = strtof(val, NULL);
		}

		if (strcmp(def->defname, "connection_costs") == 0)
		{
			char *val;

			val = defGetString(def);
			coninfo->planData.connection_costs = strtof(val, NULL);
		}
	}

}

/*
 * Generate a unique statement identifier to create
 * on the target database. Informix requires us to build
 * a unique name among all concurrent connections.
 *
 * Returns a palloc'ed string containing a statement identifier
 * suitable to pass to an Informix database.
 */
static char *ifxGenStatementName(IfxConnectionInfo *coninfo)
{
	char *stmt_name;
	size_t stmt_name_len;

	stmt_name_len = strlen(coninfo->conname) + 10;
	stmt_name = (char *) palloc(stmt_name_len + 1);
	bzero(stmt_name, stmt_name_len + 1);

	snprintf("%s_%d", stmt_name_len, coninfo->conname, MyBackendId);

	return stmt_name;
}

/*
 * Generate a unique cursor identifier
 */
static char *ifxGenCursorName(IfxConnectionInfo *coninfo)
{
	char *cursor_name;
	size_t len;

	len = strlen(coninfo->conname) + 10;
	cursor_name = (char *) palloc(len + 1);
	bzero(cursor_name, len + 1);

	snprintf("%s_%d_cur", len, coninfo->conname, MyBackendId);
	return cursor_name;
}

static void
ifxBeginForeignScan(ForeignScanState *node, int eflags)
{
	IfxConnectionInfo    *coninfo;
	IfxFdwExecutionState *festate;
	IfxCachedConnection  *cached;
	Oid                   foreignTableOid;
	bool                  conn_cached;

	foreignTableOid = RelationGetRelid(node->ss.ss_currentRelation);
	Assert((foreignTableOid != InvalidOid));
	coninfo = ifxMakeConnectionInfo(foreignTableOid);

	/*
	 * XXX: ifxPlanForeignScan() already should have added a cached
	 * connection entry for the requested table. If we don't
	 * find any entry in the connection cache, we treat this as an error
	 * for now. Maybe I need to revert this, but for the initial
	 * coding it seems the best option.
	 */
	cached = ifxConnCache_add(foreignTableOid, coninfo->conname,
							  &conn_cached);

	Assert(conn_cached);

	festate = makeIfxFdwExecutionState();

	if (coninfo->query)
		festate->stmt_info.query = coninfo->query;
	else
	{
		size_t len = strlen(coninfo->tablename) + 15;

		festate->stmt_info.query = (char *) palloc(len);
		snprintf(festate->stmt_info.query, len, "SELECT * FROM %s", coninfo->tablename);
	}

	/*
	 * Get the definition of the local foreign table attributes.
	 */
	ifxPgColumnData(foreignTableOid, festate);

	/*
	 * Save the connection identifier.
	 */
	StrNCpy(festate->stmt_info.conname, coninfo->conname, IFX_CONNAME_LEN);

	/*
	 * Generate a statement identifier. Required to uniquely
	 * identify the prepared statement within Informix.
	 */
	festate->stmt_info.stmt_name = ifxGenStatementName(coninfo);

	/*
	 * Cursor name
	 */
	festate->stmt_info.cursor_name = ifxGenCursorName(coninfo);

	/*
	 * Prepare the query. We need to generate a unique
	 * statement name for it, first.
	 */
	ifxPrepareQuery(&festate->stmt_info);

	if (ifxSetException(&(festate->stmt_info)) == IFX_RT_ERROR)
	{
		elog(ERROR, "could not prepare informix query %s",
			 festate->stmt_info.query);
	}

	/*
	 * Declare the cursor for the prepared
	 * statement.
	 */
	ifxDeclareCursorForPrepared(&festate->stmt_info);

	if (ifxSetException(&(festate->stmt_info)) == IFX_RT_ERROR)
	{
		elog(ERROR, "could not declare informix cursor");
	}

	/*
	 * Create a descriptor handle for the prepared
	 * query, so we can obtain information about returned
	 * columns. We cheat a little bit and just reuse the
	 * statement id.
	 */
	ifxAllocateDescriptor(festate->stmt_info.stmt_name);

	if (ifxSetException(&(festate->stmt_info)) == IFX_RT_ERROR)
	{
		elog(ERROR, "could not allocate informix descriptor area");
	}

	/*
	 * Open the cursor
	 */
	ifxOpenCursorForPrepared(&festate->stmt_info);

	if (ifxSetException(&(festate->stmt_info)) == IFX_RT_ERROR)
	{
		elog(ERROR, "could not open informix cursor");
	}

	/*
	 * Populate the DESCRIPTOR area.
	 */
	ifxDescribeAllocatorByName(festate->stmt_info.stmt_name,
							   festate->stmt_info.stmt_name);

	if (ifxSetException(&(festate->stmt_info)) == IFX_RT_ERROR)
	{
		elog(ERROR, "could not describe informix result set");
	}

	/*
	 * Get the number of columns.
	 */
	festate->stmt_info.ifxAttrCount = ifxDescriptorColumnCount(festate->stmt_info.stmt_name);
	festate->stmt_info.ifxAttrDefs = palloc((festate->stmt_info.ifxAttrCount + 1)
											* sizeof(IfxAttrDef));
	festate->stmt_info.ifxAttrDefs[festate->stmt_info.ifxAttrCount] = NULL;

	/*
	 * Populate result set column info array.
	 */
	ifxGetColumnAttributes(&festate->stmt_info);

	node->fdw_state = (void *) festate;
}

static void ifxColumnValueByAttNum(IfxFdwExecutionState *state, int attnum)
{
	Assert(state != NULL);
	Assert(attnum >= 0);

	switch (state->stmt_info.ifxAttrDefs[attnum - 1]->type)
	{
		case IFX_INTEGER:
		case IFX_SERIAL:
		{
			state->values[attnum - 1]->val = Int32GetDatum(ifxGetInt(&(state->stmt_info),
																	 attnum));
			state->values[attnum - 1]->def = state->stmt_info.ifxAttrDefs[attnum - 1];
			break;
		}
		default:
			elog(ERROR, "\"%d\" is not a known informix type id",
				state->stmt_info.ifxAttrDefs[attnum - 1]->type);
			break;
	}
}

static TupleTableSlot *ifxIterateForeignScan(ForeignScanState *node)
{
	TupleTableSlot       *tupleSlot = node->ss.ss_ScanTupleSlot;
	IfxFdwExecutionState *state;
	IfxSqlStateClass      errclass;
	int i;

	state = (IfxFdwExecutionState *) node->fdw_state;

	tupleSlot->tts_mintuple   = NULL;
	tupleSlot->tts_buffer     = InvalidBuffer;
	tupleSlot->tts_tuple      = NULL;
	tupleSlot->tts_shouldFree = false;

	/*
	 * Fetch tuple from cursor
	 */
	ifxFetchRowFromCursor(&state->stmt_info);

	/*
	 * No more rows?
	 */
	if ((errclass = ifxSetException(&(state->stmt_info))) == IFX_NOT_FOUND)
	{
		/*
		 * Create an empty tuple slot and we're done.
		 */
		elog(DEBUG2, "informix fdw scan end");

		tupleSlot->tts_isempty = true;
		tupleSlot->tts_nvalid  = 0;
		return tupleSlot;
	}

	/*
	 * Allocate slots for column value data.
	 */
	state->values = palloc(sizeof(IfxValue)
						   * (state->stmt_info.ifxAttrCount + 1));
	state->values[state->stmt_info.ifxAttrCount] = NULL;

	tupleSlot->tts_isempty = false;
	tupleSlot->tts_nvalid = state->stmt_info.ifxAttrCount;
	tupleSlot->tts_values = (Datum *) palloc(sizeof(Datum)
											 * tupleSlot->tts_nvalid);
	tupleSlot->tts_isnull = (bool *) palloc(sizeof(bool)
											* tupleSlot->tts_nvalid);

	/*
	 * The cursor should now be positioned at the current row
	 * we want to retrieve. Loop through the columns and retrieve
	 * their values. Note: No conversion into a PostgreSQL specific
	 * datatype is done so far.
	 */
	for (i = 0; i < state->stmt_info.ifxAttrCount - 1; i++)
	{
		elog(DEBUG2, "get column %d", i);

		/*
		 * Retrieve a converted datum from the current
		 * column and store it within state context.
		 */
		ifxColumnValueByAttNum(state, i);

		/*
		 * It might happen that the FDW table has dropped
		 * columns...check for them and insert a NULL value instead..
		 */
		if (state->pgAttrDefs[i] == NULL)
		{
			tupleSlot->tts_isnull[i] = true;
			tupleSlot->tts_values[i] = PointerGetDatum(NULL);
			continue;
		}

		/*
		 * Same for retrieved NULL values...
		 */
		if (state->stmt_info.ifxAttrDefs[i]->indicator == INDICATOR_NULL)
		{
			tupleSlot->tts_isnull[i] = true;
			tupleSlot->tts_values[i] = PointerGetDatum(NULL);
			continue;
		}

		/*
		 * ifxColumnValueByAttnum() has already converted the current
		 * column value into a datum. We just need to assign it to the
		 * tupleSlot and we're done.
		 */
		tupleSlot->tts_isnull[i] = false;
		tupleSlot->tts_values[i] = state->values[i]->val;
	}

	return tupleSlot;
}

/*
 * Returns a new allocated pointer
 * to IfxConnectionInfo.
 */
static IfxConnectionInfo *ifxMakeConnectionInfo(Oid foreignTableOid)
{
	IfxConnectionInfo *coninfo;
	StringInfoData    *buf;

	coninfo = (IfxConnectionInfo *) palloc(sizeof(IfxConnectionInfo));
	bzero(coninfo->conname, IFX_CONNAME_LEN + 1);
	ifxGetOptions(foreignTableOid, coninfo);

	buf = ifxGenerateConnName(coninfo);
	StrNCpy(coninfo->conname, buf->data, IFX_CONNAME_LEN);

	ifxConnInfoSetDefaults(coninfo);

	return coninfo;
}

static bytea *
ifxFdwPlanDataAsBytea(IfxConnectionInfo *coninfo)
{
	bytea *data;
	int    len;

	data = (bytea *) palloc(len + VARHDRSZ);
	memcpy(VARDATA(data), &(coninfo->planData), sizeof(IfxPlanData));
	return data;
}

static FdwPlan *
ifxPlanForeignScan(Oid foreignTableOid, PlannerInfo *planInfo, RelOptInfo *baserel)
{

	IfxConnectionInfo *coninfo;
	StringInfoData    *buf;
	bool               conn_cached;
	FdwPlan           *plan;
	bytea             *plan_data;
	IfxSqlStateClass   err;

	/*
	 * Prepare a generic plan structure
	 */
	plan = makeNode(FdwPlan);

	/*
	 * If not already done, initialize cache data structures.
	 */
	InformixCacheInit();

	/*
	 * Initialize connection structures and retrieve FDW options
	 */

	coninfo = ifxMakeConnectionInfo(foreignTableOid);

	/*
	 * Lookup the connection name in the connection cache.
	 */
	ifxConnCache_add(foreignTableOid, coninfo->conname, &conn_cached);

	/*
	 * Establish a new INFORMIX connection with transactions,
	 * in case a new one needs to be created. Otherwise make
	 * the requested connection current.
	 */
	if (!conn_cached)
	{
		ifxCreateConnectionXact(coninfo);

		/*
		 * A new connection probably has less cache affinity on the
		 * server than a cached one. So if this is a fresh connection,
		 * reflect it in the startup cost.
		 */
		plan->startup_cost = 500;
	}
	else
	{
		/*
		 * Make the requested connection current.
		 */
		ifxSetConnection(coninfo);

		plan->startup_cost = 100;
	}

	/*
	 * Check connection status. This should happen directly
	 * after connection establishing, otherwise we might get confused by
	 * other ESQL API calls in the meantime.
	 */
	if ((err = ifxConnectionStatus()) != IFX_CONNECTION_OK)
	{
		if (err == IFX_CONNECTION_WARN)
			elog(WARNING, "opened informix connection with warnings");

		if (err == IFX_CONNECTION_ERROR)
			elog(ERROR, "could not open connection to informix server");
	}

	return plan;
}

/*
 * ifxExplainForeignScan
 *		Produce extra output for EXPLAIN
 */
static void
ifxExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	IfxConnectionInfo coninfo;
	IfxFdwExecutionState *festate;

	festate = (IfxFdwExecutionState *) node->fdw_state;

	/*
	 * XXX: We need to get the info from the cached connection!
	 */

	/* Fetch options  */
	ifxGetOptions(RelationGetRelid(node->ss.ss_currentRelation),
				  &coninfo);

	/* Give some possibly useful info about startup costs */
	if (es->costs)
	{
		ExplainPropertyFloat("Remote server startup cost",
							 coninfo.planData.connection_costs, 4, es);
		ExplainPropertyFloat("Remote table row estimate",
							 coninfo.planData.estimated_rows, 4, es);
		ExplainPropertyText("Informix query", festate->stmt_info.query, es);
	}
}


static void ifxConnInfoSetDefaults(IfxConnectionInfo *coninfo)
{
	Assert(coninfo != NULL);

	if (coninfo == NULL)
		return;

	coninfo->planData.estimated_rows = 100.0;
	coninfo->planData.connection_costs = 100.0;
}

static StringInfoData *
ifxFdwOptionsToStringBuf(Oid context)
{
	StringInfoData      *buf;
	struct IfxFdwOption *ifxopt;

	buf = makeStringInfo();
	initStringInfo(buf);

	for (ifxopt = ifx_valid_options; ifxopt->optname; ifxopt++)
	{
		if (context == ifxopt->optcontext)
		{
			appendStringInfo(buf, "%s%s", (buf->len > 0) ? "," : "",
							 ifxopt->optname);
		}
	}

	return buf;
}

/*
 * Check if specified option is actually known
 * to the Informix FDW.
 */
static bool
ifxIsValidOption(const char *option, Oid context)
{
	struct IfxFdwOption *ifxopt;

	for (ifxopt = ifx_valid_options; ifxopt->optname; ifxopt++)
	{
		if (context == ifxopt->optcontext
			&& strcmp(ifxopt->optname, ifxopt->optname) == 0)
		{
			return true;
		}
	}
	/*
	 * Only reached in case of mismatch
	 */
	return false;
}
