/*-------------------------------------------------------------------------
 *
 * ifx_conv.c
 *		  Datatype conversion routines and helper functions.
 *
 * Be cautious about memory allocations outside
 * our memory context. Informix ESQL/C APIs allocate memory
 * under the hood of our PostgreSQL memory contexts. You *must*
 * call ifxRewindCallstack before re-throwing any PostgreSQL
 * elog's, otherwise you likely leak memory.
 *
 * Copyright (c) 2012, credativ GmbH
 *
 * IDENTIFICATION
 *		  informix_fdw/ifx_conv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif

#include "catalog/pg_cast.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "ifx_fdw.h"

/*******************************************************************************
 * Helper functions
 */
static regproc getTypeCastFunction(IfxFdwExecutionState *state,
								   Oid sourceOid, Oid targetOid);
static regproc getTypeInputFunction(IfxFdwExecutionState *state,
									Oid inputOid);
static void
deparse_predicate_node(Node *node, IfxPushdownOprContext *context,
					   IfxPushdownOprInfo *info);

#if PG_VERSION_NUM >= 90300
static regproc getTypeOutputFunction(IfxFdwExecutionState *state,
									 Oid inputOid);
#endif

/*******************************************************************************
 * Implementation starts here
 */

/*
 * convertIfxDateString()
 *
 * Converts an informix formatted date string into a PostgreSQL
 * DATE datum. Conversion is supported to
 *
 * DATE
 * TEXT
 * VARCHAR
 * BPCHAR
 */
Datum convertIfxDateString(IfxFdwExecutionState *state, int attnum)
{
	Datum result;
	Oid   inputOid;
	char *val;
	regproc typeinputfunc;

	/*
	 * Init...
	 */
	result   = PointerGetDatum(NULL);

	switch(PG_ATTRTYPE_P(state, attnum))
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case DATEOID:
			inputOid = PG_ATTRTYPE_P(state, attnum);
			break;
		default:
		{
			/* oops, unexpected datum conversion */
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return result;
		}
	}

	val = (char *)palloc0(IFX_DATE_BUFFER_LEN);
	if (ifxGetDateAsString(&(state->stmt_info),
						   PG_MAPPED_IFX_ATTNUM(state, attnum), val) == NULL)
	{
		/*
		 * Got a SQL null value or conversion error. Leave it up to
		 * the caller to look what's wrong (at least, we can't error
		 * out at this place, since the caller need's the chance to
		 * clean up itself).
		 */
		return result;
	}


	/*
	 * Catch errors from subsequent function calls...
	 */
	PG_TRY();
	{
		/*
		 * Try the conversion.
		 */
		typeinputfunc = getTypeInputFunction(state, inputOid);
		result = OidFunctionCall3(typeinputfunc,
								  CStringGetDatum(val),
								  ObjectIdGetDatum(InvalidOid),
								  Int32GetDatum(PG_ATTRTYPEMOD_P(state, attnum)));
	}
	PG_CATCH();
	{
		ifxRewindCallstack(&(state->stmt_info));
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
 }

/*
 * convertIfxTimestamp()
 *
 * Converts a given Informix DATETIME value into
 * a PostgreSQL timestamp.
 */
Datum convertIfxTimestampString(IfxFdwExecutionState *state, int attnum)
{
	Datum result;
	Oid   inputOid;
	char  *val;
	regproc typeinputfunc;

	/*
	 * Init ...
	 */
	result = PointerGetDatum(NULL);

	switch (PG_ATTRTYPE_P(state, attnum))
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			inputOid = PG_ATTRTYPE_P(state, attnum);
			break;
		default:
		{
			/* oops, unexpected datum conversion */
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return result;
		}
	}

	/*
	 * We get the Informix DTIME value as a ANSI SQL
	 * formatted character string. Prepare a buffer for it
	 * and call the appropiate conversion function from our
	 * Informix API...
	 */
	val = (char *) palloc0(IFX_DATETIME_BUFFER_LEN);

	if (ifxGetTimestampAsString(&(state->stmt_info),
								PG_MAPPED_IFX_ATTNUM(state, attnum), val) == NULL)
	{
		/*
		 * Got a SQL null value or conversion error. Leave it up to
		 * the caller to look what's wrong (at least, we can't error
		 * out at this place, since the caller need's the chance to
		 * clean up itself).
		 */
		return result;
	}

	PG_TRY();
	{
		/*
		 * Get the input function and try the conversion. We just pass
		 * the character string into the specific type input function.
		 */
		typeinputfunc = getTypeInputFunction(state, inputOid);
		result = OidFunctionCall3(typeinputfunc,
								  CStringGetDatum(val),
								  ObjectIdGetDatum(InvalidOid),
								  Int32GetDatum(PG_ATTRTYPEMOD_P(state, attnum)));
	}
	PG_CATCH();
	{
		ifxRewindCallstack(&(state->stmt_info));
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
 }

/*
 * convertIfxDecimal()
 *
 * Converts a decimal value into a PostgreSQL numeric datum.
 * Note that convertIfxDecimal() works by converting the
 * character representation of a dec_t value formerly retrieved
 * from an informix column. This we must be aware of any locale
 * settings here.
 *
 * Currently, we support conversion to numeric types only.
 */
Datum convertIfxDecimal(IfxFdwExecutionState *state, int attnum)
{
	Datum result;
	Oid inputOid;
	char *val;
	regproc typinputfunc;

	/*
	 * Init...
	 */
	result = PointerGetDatum(NULL);

	switch(PG_ATTRTYPE_P(state, attnum))
	{
		case NUMERICOID:
		{
			inputOid = PG_ATTRTYPE_P(state, attnum);
			break;
		}
		default:
		{
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return result;
		}
	}

	val = (char *) palloc0(IFX_DECIMAL_BUF_LEN + 1);

	/*
	 * Get the value from the informix column and check wether
	 * the character string is valid. Don't go further, if not...
	 */
	if (ifxGetDecimal(&state->stmt_info, PG_MAPPED_IFX_ATTNUM(state, attnum), val) == NULL)
	{
		/* caller should handle indicator */
		return result;
	}


	/*
	 * Type input function known and target column looks compatible,
	 * let's try the conversion...
	 */
	PG_TRY();
	{
		/*
		 * Get the type input function.
		 */
		typinputfunc = getTypeInputFunction(state, inputOid);

		/*
		 * Watch out for typemods
		 */
		result = OidFunctionCall3(typinputfunc,
								  CStringGetDatum(val),
								  ObjectIdGetDatum(InvalidOid),
								  Int32GetDatum(PG_ATTRTYPEMOD_P(state, attnum)));
	}
	PG_CATCH();
	{
		ifxRewindCallstack(&(state->stmt_info));
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}

/*
 * convertIfxInt()
 *
 * Converts either an 2-, 4-, or 8-byte informix integer value
 * into a corresponding PostgreSQL datum. The target type
 * range is checked and conversion refused if it doesn't
 * match. We also support conversion into either TEXT, VARCHAR
 * and bpchar.
 */
Datum convertIfxInt(IfxFdwExecutionState *state, int attnum)
{
	Datum result;
	PgAttrDef pg_def;

	/*
	 * Setup stuff...
	 */
	pg_def = state->pgAttrDefs[attnum];

	/*
	 * Do the conversion...
	 */
	switch(pg_def.atttypid)
	{
		case INT2OID:
		{
			int16 val;

			/* accepts int2 only */
			if (IFX_ATTRTYPE_P(state, attnum) != IFX_SMALLINT)
			{
				IFX_ATTR_SETNOTVALID_P(state, attnum);
				return PointerGetDatum(NULL);
			}

			val = ifxGetInt2(&(state->stmt_info),
							 PG_MAPPED_IFX_ATTNUM(state, attnum));
			result = Int16GetDatum(val);

			break;
		}
		case INT4OID:
		{
			int val;

			/* accepts int2 and int4/serial */
			if ((IFX_ATTRTYPE_P(state, attnum) != IFX_SMALLINT)
				&& (IFX_ATTRTYPE_P(state, attnum) != IFX_INTEGER)
				&& (IFX_ATTRTYPE_P(state, attnum) != IFX_SERIAL))
			{
				IFX_ATTR_SETNOTVALID_P(state, attnum);
				return PointerGetDatum(NULL);
			}

			val = ifxGetInt4(&(state->stmt_info),
							 PG_MAPPED_IFX_ATTNUM(state, attnum));
			result = Int32GetDatum(val);

			break;
		}
		case INT8OID:
			/*
			 * Note that the informix int8 value retrieved by
			 * ifxGetInt8() is converted into its *character*
			 * representation. We leave it up to the typinput
			 * routine to convert it back to a PostgreSQL BIGINT.
			 * So fall through and do the work below.
			 */
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		{
			/*
			 * Try the conversion...
			 */
			PG_TRY();
			{
				if ((IFX_ATTRTYPE_P(state, attnum) == IFX_INT8)
					|| (IFX_ATTRTYPE_P(state, attnum) == IFX_SERIAL8)
					|| (IFX_ATTRTYPE_P(state, attnum) == IFX_INFX_INT8))
				{
					char *buf;
					regproc typinputfunc;

					buf = (char *) palloc0(IFX_INT8_CHAR_LEN + 1);

					/*
					 * Extract the value from the sqlvar tuple.
					 *
					 * Take care for incompatible types BIGINT and INT8
					 */
					switch (IFX_ATTRTYPE_P(state, attnum))
					{
						case IFX_INT8:
						case IFX_SERIAL8:
							/* INT8 */
							buf = ifxGetInt8(&(state->stmt_info),
											 PG_MAPPED_IFX_ATTNUM(state, attnum), buf);
							break;
						case IFX_INFX_INT8:
							/* BIGINT */
							buf = ifxGetBigInt(&(state->stmt_info),
											   PG_MAPPED_IFX_ATTNUM(state, attnum), buf);
							break;
						default:
							pfree(buf);
							buf = NULL;
					}

					/*
					 * Check for null pointer in buf. This is not expected
					 * and means an error occured.
					 */
					if (buf == NULL)
					{
						ifxRewindCallstack(&(state->stmt_info));
						elog(ERROR,
							 "could not convert informix int8 value");
					}

					/*
					 * Finally call the type input function and we're
					 * done.
					 */
					typinputfunc = getTypeInputFunction(state, PG_ATTRTYPE_P(state, attnum));
					result = OidFunctionCall2(typinputfunc,
											  CStringGetDatum(pstrdup(buf)),
											  ObjectIdGetDatum(InvalidOid));
				}
				else
				{
					/*
					 * We have a compatible integer type here
					 * and a character target type. In this case
					 * we simply call the cast function of the designated
					 * target type and let it do the legwork...
					 */
					regproc typcastfunc;
					Oid     sourceOid;

					/*
					 * need the source type OID
					 *
					 * XXX: we might better do it earlier when
					 *      retrieving the IFX types ???
					 */
					if (IFX_ATTRTYPE_P(state, attnum) == IFX_INTEGER)
						sourceOid = INT4OID;
					else
						/* only INT2 left... */
						sourceOid = INT2OID;

					/*
					 * Execute the cast function and we're done...
					 */
					typcastfunc = getTypeCastFunction(state, sourceOid,
													  PG_ATTRTYPE_P(state, attnum));

					if (sourceOid == INT4OID)
						result = OidFunctionCall1(typcastfunc,
												  Int32GetDatum(ifxGetInt4(&(state->stmt_info),
																		   PG_MAPPED_IFX_ATTNUM(state, attnum))));
					else
						/* only INT2 left... */
						result = OidFunctionCall1(typcastfunc,
												  Int16GetDatum(ifxGetInt2(&(state->stmt_info),
																		   PG_MAPPED_IFX_ATTNUM(state, attnum))));
				}
			}
			PG_CATCH();
			{
				ifxRewindCallstack(&(state->stmt_info));
				PG_RE_THROW();
			}
			PG_END_TRY();

			break;
		}
		default:
		{
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return PointerGetDatum(NULL);
		}
	}

	return result;
}

#if PG_VERSION_NUM >= 90300

/*
 * Store the given string value into the specified
 * SQLDA handle, depending on wich target type we have.
 */
void setIfxText(IfxFdwExecutionState *state,
				TupleTableSlot *slot,
				int attnum)
{
	/*
	 * Sanity check, SQLDA available ?
	 */
	Assert(state->stmt_info.sqlda != NULL);

	/*
	 * In case we have a null value, set the indicator value accordingly.
	 */
	IFX_SET_INDICATOR_P(state, attnum,
						((slot->tts_isnull[attnum]) ? INDICATOR_NULL : INDICATOR_NOT_NULL));

	switch(IFX_ATTRTYPE_P(state, attnum))
	{
		case IFX_TEXT:
		case IFX_BYTES:
			break;
		default:
			break;
	}
}

void setIfxInteger(IfxFdwExecutionState *state,
				   TupleTableSlot *slot,
				   int attnum)
{
	/*
	 * Sanity check, SQLDA available ?
	 */
	Assert(state->stmt_info.sqlda != NULL);

	/*
	 * In case we have a null value, set the indiciator value accordingly.
	 */
	IFX_SET_INDICATOR_P(state, attnum,
						((slot->tts_isnull[attnum]) ? INDICATOR_NULL : INDICATOR_NOT_NULL));

	switch(IFX_ATTRTYPE_P(state, attnum))
	{
		case IFX_SMALLINT:
		{
			short val;

			val = DatumGetInt16(slot->tts_values[attnum]);

			/*
			 * Copy the value into the SQLDA structure.
			 */
			ifxSetInt2(&state->stmt_info,
					   PG_MAPPED_IFX_ATTNUM(state, attnum),
					   val);

			break;
		}
		case IFX_INTEGER:
			/* fall through to IFX_SERIAL */
		case IFX_SERIAL:
		{
			int val;

			/*
			 * Get the integer value.
			 */
			val = DatumGetInt32(slot->tts_values[attnum]);

			/*
			 * Copy the value into the SQLDA structure.
			 */
			ifxSetInteger(&state->stmt_info,
						  PG_MAPPED_IFX_ATTNUM(state, attnum),
						  val);
			break;
		}
		case IFX_INT8:
		case IFX_INFX_INT8:
			/* fall through to IFX_SERIAL8 */
		case IFX_SERIAL8:
		{
			char *strval;
			regproc typout;

			/*
			 * Convert the int8 value to its character representation.
			 */
			typout = getTypeOutputFunction(state, PG_ATTRTYPE_P(state, attnum));
			strval = DatumGetCString(OidFunctionCall1(typout, DatumGetInt64(slot->tts_values[attnum])));
			if (IFX_ATTRTYPE_P(state, attnum) == IFX_INT8)
			{
				/* INT8 (Informix ifx_int8_t) */
				ifxSetInt8(&state->stmt_info,
						   PG_MAPPED_IFX_ATTNUM(state, attnum),
						   strval);
			}
			else
			{
				/* BIGINT */
				ifxSetBigint(&state->stmt_info,
							 PG_MAPPED_IFX_ATTNUM(state, attnum),
							 strval);
			}

			/*
			 * Check wether conversion was successful
			 */
			break;
		}
		default:
			break;
	}
}

static regproc getTypeOutputFunction(IfxFdwExecutionState *state,
									 Oid inputOid)
{
	regproc result;
	HeapTuple type_tuple;

	/*
	 * Which output function to lookup?
	 */
	type_tuple = SearchSysCache1(TYPEOID, inputOid);

	if (!HeapTupleIsValid(type_tuple))
	{
		/*
		 * Oops, this is not expected...
		 *
		 * Don't throw an ERROR here immediately, but inform the caller
		 * that something went wrong. We need to give the caller time
		 * to cleanup itself...
		 */
		ifxRewindCallstack(&(state->stmt_info));
		elog(ERROR,
			 "cache lookup failed for output function for type %u",
			 inputOid);
	}

	ReleaseSysCache(type_tuple);

	result = ((Form_pg_type) GETSTRUCT(type_tuple))->typoutput;
	return result;
}

#endif

/*
 * Returns the type input function for the
 * specified type OID. Throws an error in case
 * no valid input function could be found.
 */
static regproc getTypeInputFunction(IfxFdwExecutionState *state,
								   Oid inputOid)
{
	regproc result;
	HeapTuple type_tuple;

	/*
	 * Get the type input function.
	 */
	type_tuple = SearchSysCache1(TYPEOID,
								 inputOid);
	if (!HeapTupleIsValid(type_tuple))
	{
		/*
		 * Oops, this is not expected...
		 *
		 * Don't throw an ERROR here immediately, but inform the caller
		 * that something went wrong. We need to give the caller time
		 * to cleanup itself...
		 */
		ifxRewindCallstack(&(state->stmt_info));
		elog(ERROR,
			 "cache lookup failed for input function for type %u",
			 inputOid);
	}

	ReleaseSysCache(type_tuple);

	result = ((Form_pg_type) GETSTRUCT(type_tuple))->typinput;
	return result;
}

/*
 * Returns the type case function for the specified
 * source and target OIDs. Throws an error in case
 * no cast function could be found.
 */
static regproc getTypeCastFunction(IfxFdwExecutionState *state,
								   Oid sourceOid, Oid targetOid)
{
	regproc result;
	HeapTuple cast_tuple;

	cast_tuple = SearchSysCache2(CASTSOURCETARGET,
								 sourceOid,
								 targetOid);

	if (!HeapTupleIsValid(cast_tuple))
	{
		ifxRewindCallstack(&(state->stmt_info));
		elog(ERROR,
			 "cache lookup failed for cast from %u to %u",
			 sourceOid, targetOid);
	}

	result = ((Form_pg_cast) GETSTRUCT(cast_tuple))->castfunc;
	ReleaseSysCache(cast_tuple);

	return result;
}


/*
 * convertIfxBoolean
 *
 * Converts the specified informix attribute
 * into a PostgreSQL boolean datum. If the target type
 * is a boolean, the function tries to convert the value
 * directly, otherwise the value is casted to the
 * requested target type, if possible.
 *
 * Supported target types are
 *
 * TEXTOID
 * VARCHAROID
 * CHAROID
 * BPCHAROID
 * BOOLOID
 *
 */
Datum convertIfxBoolean(IfxFdwExecutionState *state, int attnum)
{
	Datum result;
	Oid sourceOid;
	Oid targetOid;
	char val;

	/*
	 * Init variables...
	 */
	result = PointerGetDatum(NULL);
	val = ifxGetBool(&(state->stmt_info), attnum);
	sourceOid = InvalidOid;
	targetOid = InvalidOid;

	/*
	 * If the target type is not supposed to be compatible,
	 * reject any conversion attempts.
	 */
	switch(PG_ATTRTYPE_P(state, attnum))
	{
		case BOOLOID:
		case CHAROID:
		{
			result = BoolGetDatum(val);
			break;
		}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		{
			regproc typecastfunc;

			sourceOid = BOOLOID;
			targetOid = PG_ATTRTYPE_P(state, attnum);

			typecastfunc = getTypeCastFunction(state, sourceOid, targetOid);

			/*
			 * Execute the cast function.
			 */
			result = OidFunctionCall1(typecastfunc,
									  CharGetDatum(val));
			break;
		}
		default:
			/* not supported */
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			break;
	}

	return result;
}

/*
 * convertIfxSimpleLO
 *
 * Converts a simple large object into a corresponding
 * PostgreSQL datum. Currently supported are the following
 * conversions:
 *
 *  INFORMIX | POSTGRESQL
 * -----------------------
 *  TEXT     | TEXT
 *  TEXT     | VARCHAR
 *  TEXT     | BPCHAR
 *  TEXT     | BYTEA
 */
Datum convertIfxSimpleLO(IfxFdwExecutionState *state, int attnum)
{
	Datum  result;
	char  *val;
	Oid    inputOid;
	regproc typeinputfunc;
	long    buf_size;

	result = PointerGetDatum(NULL);

	/*
	 * Target type OID supported?
	 */
	switch (PG_ATTRTYPE_P(state, attnum))
	{
		case TEXTOID:
		case BPCHAROID:
		case VARCHAROID:
		case BYTEAOID:
			inputOid = PG_ATTRTYPE_P(state, attnum);
			break;
		default:
		{
			/* oops, unsupported datum conversion */
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return result;
		}
	}

	/*
	 * ifxGetTextFromLocator returns a pointer into
	 * the locator structure, just reuse it during the
	 * FETCH but don't try to deallocate it. This is done
	 * later by the ESQL/C API after the FETCH finishes...
	 */
	val = ifxGetTextFromLocator(&(state->stmt_info),
								PG_MAPPED_IFX_ATTNUM(state, attnum),
								&buf_size);

	/*
	 * Check indicator value. In case we got NULL,
	 * nothing more to do.
	 */
	if (IFX_ATTR_ISNULL_P(state, attnum)
		|| (! IFX_ATTR_IS_VALID_P(state, attnum)))
		return result;

	elog(DEBUG3, "blob size fetched: %ld", buf_size);

	PG_TRY();
	{
		/*
		 * If the target type is a varlena, go on. Take care for
		 * typemods however...
		 */
		typeinputfunc = getTypeInputFunction(state, inputOid);

		switch (inputOid)
		{
			case TEXTOID:
			case VARCHAROID:
			case BPCHAROID:
			{
				/*
				 * Take care for typmods...
				 */
				if (PG_ATTRTYPEMOD_P(state, attnum) != -1)
				{
					result = OidFunctionCall3(typeinputfunc,
											  CStringGetDatum(val),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(PG_ATTRTYPEMOD_P(state, attnum)));
				}
				else
				{
					result = OidFunctionCall2(typeinputfunc,
											  CStringGetDatum(val),
											  ObjectIdGetDatum(InvalidOid));
				}
			}
			case BYTEAOID:
			{
				bytea *binary_data;
				int    len;

				/*
				 * Allocate a bytea datum. Don't use strlen() for
				 * val, in case the source column is of type BYTE. Instead,
				 * rely on the loc_buffer size, Informix has returned to us.
				 */
				len = buf_size;
				binary_data = (bytea *) palloc0(VARHDRSZ + len);

				SET_VARSIZE(binary_data, len + VARHDRSZ);
				memcpy(VARDATA(binary_data), val, len);
				IFX_SETVAL_P(state, attnum, PointerGetDatum(binary_data));
				result = IFX_GETVAL_P(state, attnum);
			}
		}

	}
	PG_CATCH();
	{
		ifxRewindCallstack(&(state->stmt_info));
		PG_RE_THROW();
	}
	PG_END_TRY();

	return result;
}

/*
 * convertIfxCharacterString
 *
 * Converts a given character string formerly retrieved
 * from Informix into the given PostgreSQL destination type.
 *
 * Supported informix character types are:
 *
 * CHAR
 * VARCHAR
 * LVARCHAR
 * NVARCHAR
 *
 * The caller must have prepared the column definitions
 * before.
 *
 * Handled target types are
 *
 * BPCHAROID
 * VARCHAROID
 * TEXTOID
 *
 * The converted value is assigned to the execution state
 * context. Additionally, the converted value is returned to
 * caller directly. In case an error occured, a NULL datum
 * is returned.
 */

Datum convertIfxCharacterString(IfxFdwExecutionState *state, int attnum)
{
	Datum      result;
	PgAttrDef  pg_def;
	char      *val;

	/*
	 * Initialize stuff...
	 */
	pg_def = state->pgAttrDefs[attnum];

	/*
	 * Sanity check, fail in case we are called
	 * on incompatible data type mapping.
	 */
	switch (pg_def.atttypid)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case BYTEAOID:
			break;
		default:
		{
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return PointerGetDatum(NULL);
		}
	}

	/*
	 * Retrieve the character string from the informix result
	 * set. Caller must have checked for INDICATOR_NULL before...
	 */
	val = ifxGetText(&(state->stmt_info),
					 PG_MAPPED_IFX_ATTNUM(state, attnum));

	/*
	 * Check the state of the value. In case of a NULL value,
	 * nothing more to do.
	 */
	if (IFX_ATTR_ISNULL_P(state, attnum))
		return PointerGetDatum(NULL);

	/*
	 * If the target type is compatible with the source type,
	 * convert it directly. We do this with TEXT and VARCHAR only,
	 * since for the other types it might be necessary to apply typmods...
	 */
	if ((pg_def.atttypid == TEXTOID)
		|| (pg_def.atttypid == BYTEAOID)
		|| ((pg_def.atttypid == VARCHAROID) && (pg_def.atttypmod == -1)))
	{
		if (PG_ATTRTYPE_P(state, attnum) == TEXTOID)
		{
			text *text_val;

			/*
			 * Otherwise convert the character string into a text
			 * value. We must be cautious here, since the column length
			 * stored in the column definition struct only reports the *overall*
			 * length of the data buffer, *not* the value length itself.
			 *
			 */

			/*
			 * XXX: What about encoding conversion??
			 */

			text_val = cstring_to_text_with_len(val, strlen(val));
			IFX_SETVAL_P(state, attnum, PointerGetDatum(text_val));
			result = IFX_GETVAL_P(state, attnum);
		}
		else
		{
			/* binary BYTEA value */
			bytea *binary_data;
			int    len;

			/*
			 * Allocate a bytea datum. We can use strlen here, because
			 * we know that our source value must be a valid
			 * character string.
			 */
			len = strlen(val);
			binary_data = (bytea *) palloc0(VARHDRSZ + len);

			SET_VARSIZE(binary_data, len + VARHDRSZ);
			memcpy(VARDATA(binary_data), val, len);
			IFX_SETVAL_P(state, attnum, PointerGetDatum(binary_data));
			result = IFX_GETVAL_P(state, attnum);
		}
	}
	else
	{
		regproc typeinputfunc;
		HeapTuple  conv_tuple;

		conv_tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(pg_def.atttypid));

		if (!HeapTupleIsValid(conv_tuple))
		{
			/*
			 * Oops, this is not expected...
			 *
			 * Don't throw an ERROR here immediately, but inform the caller
			 * that something went wrong. We need to give the caller time
			 * to cleanup itself...
			 */
			IFX_ATTR_SETNOTVALID_P(state, attnum);
			return PointerGetDatum(NULL);
		}

		/*
		 * Catch any errors from the following function calls, or
		 * we likely leak memory allocated by the ESQL/C API...
		 */
		PG_TRY();
		{
			/*
			 * Try the conversion...
			 */
			typeinputfunc = ((Form_pg_type) GETSTRUCT(conv_tuple))->typinput;

			result = OidFunctionCall3(typeinputfunc,
									  CStringGetDatum(val),
									  ObjectIdGetDatum(InvalidOid),
									  Int32GetDatum(pg_def.atttypmod));
		}
		PG_CATCH();
		{
			ifxRewindCallstack(&(state->stmt_info));
			PG_RE_THROW();
		}
		PG_END_TRY();

		ReleaseSysCache(conv_tuple);
	}

	return result;
}

IfxOprType mapPushdownOperator(Oid oprid, IfxPushdownOprInfo *pushdownInfo)
{
	char *oprname;
	HeapTuple oprtuple;
	Form_pg_operator oprForm;

	Assert(oprid != InvalidOid);
	Assert(pushdownInfo != NULL);

	oprtuple = SearchSysCache1(OPEROID, oprid);

	if (!HeapTupleIsValid(oprtuple))
		elog(ERROR, "cache lookup failed for operator %u", oprid);

	oprForm  = (Form_pg_operator)GETSTRUCT(oprtuple);
	oprname  = pstrdup(NameStr(oprForm->oprname));

	ReleaseSysCache(oprtuple);

	/*
	 * Currently we support Postgresql internal
	 * operators only. Ignore all operators living
	 * in other schemas than pg_catalog.
	 *
	 * We might relax this some time, since we push
	 * down the operator names based on string
	 * comparisons.
	 */
	if (oprForm->oprnamespace != PG_CATALOG_NAMESPACE)
		return IFX_OPR_NOT_SUPPORTED;

	if (strcmp(oprname, ">=") == 0)
	{
		pushdownInfo->type = IFX_OPR_GE;
		return IFX_OPR_GE;
	}
	else if (strcmp(oprname, "<=") == 0)
	{
		pushdownInfo->type = IFX_OPR_LE;
		return IFX_OPR_LE;
	}
	else if (strcmp(oprname, "<") == 0)
	{
		pushdownInfo->type = IFX_OPR_LT;
		return IFX_OPR_LT;
	}
	else if (strcmp(oprname, ">") == 0)
	{
		pushdownInfo->type = IFX_OPR_GT;
		return IFX_OPR_GT;
	}
	else if (strcmp(oprname, "=") == 0)
	{
		pushdownInfo->type = IFX_OPR_EQUAL;
		return IFX_OPR_EQUAL;
	}
	else if (strcmp(oprname, "<>") == 0)
	{
		pushdownInfo->type = IFX_OPR_NEQUAL;
		return IFX_OPR_NEQUAL;
	}
	else if (strcmp(oprname, "~~") == 0)
	{
		pushdownInfo->type = IFX_OPR_LIKE;
		return IFX_OPR_LIKE;
	}
	else
	{
		pushdownInfo->type = IFX_OPR_NOT_SUPPORTED;
		return IFX_OPR_NOT_SUPPORTED;
	}

	/* currently never reached */
	return IFX_OPR_UNKNOWN;
}

/*
 * Create a list with a RTE for the given foreign table.
 */
static List *make_deparse_context(Oid foreignRelid)
{
	/*
	 * Get the relation name for the given foreign table.
	 *
	 * XXX: This currently works only, because we rely on the fact that
	 *      a foreign table is equally named like on the foreign server.
	 *      Bad style (tm), but i'm not sure how to make this transparently
	 *      without using additional FDW options.
	 */;
	return deparse_context_for(get_rel_name(foreignRelid), foreignRelid);
}

#define IFX_MARK_PREDICATE_NOT_SUPPORTED(a, b) \
	(a)->type = IFX_OPR_NOT_SUPPORTED;

#define IFX_MARK_PREDICATE_ELEM(a, b) \
	(b)->predicates = lappend((b)->predicates, (a)); \
	(b)->count++;

#define IFX_MARK_PREDICATE_BOOL(a, b) \
	(b)->predicates = lappend((b)->predicates, (a)); \
	(b)->count++;

/*
 * ifx_predicate_tree_walker()
 *
 * Examine the expression node. We expect a OPEXPR
 * here always in the form
 *
 * FDW col = CONST
 * FDW col != CONST
 * FDW col >(=) CONST
 * FDW col <(=) CONST
 *
 * Only CONST and VAR expressions are currently supported.
 */
bool ifx_predicate_tree_walker(Node *node, struct IfxPushdownOprContext *context)
{
	IfxPushdownOprInfo *info;

	if (node == NULL)
		return false;

	/*
	 * Handle BoolExpr. Recurse into its arguments
	 * and try to decode its OpExpr accordingly.
	 */
	if (IsA(node, BoolExpr))
	{
		BoolExpr *boolexpr;
		ListCell *cell;

		boolexpr = (BoolExpr *) node;

		/*
		 * Decode the arguments to the BoolExpr
		 * directly, don't leave it to expression_tree_walker()
		 *
		 * By going down this route, we are able to push the
		 * IfxPushdownOprInfo into our context list in the right
		 * order.
		 */
		foreach(cell, boolexpr->args)
		{
			Node *bool_arg = (Node *) lfirst(cell);

			ifx_predicate_tree_walker(bool_arg, context);

			/*
			 * End of arguments?
			 * If true, don't add additional pushdown info
			 */
			if (lnext(cell) != NULL)
			{
				/*
				 * Save boolean expression type.
				 */
				info = palloc(sizeof(IfxPushdownOprInfo));
				info->expr = NULL;

				switch (boolexpr->boolop)
				{
					case AND_EXPR:
						info->type        = IFX_OPR_AND;
						info->expr_string = cstring_to_text("AND");
						break;
					case OR_EXPR:
						info->type        = IFX_OPR_OR;
						info->expr_string = cstring_to_text("OR");
						break;
					case NOT_EXPR:
						info->type        = IFX_OPR_NOT;
						info->expr_string = cstring_to_text("NOT");
						break;
					default:
						elog(ERROR, "unsupported boolean expression type");
				}

				/* Push to the predicates list, but don't mark it
				 * as a pushdown expression */
				IFX_MARK_PREDICATE_BOOL(info, context);
			}
		}

		/* done */
		return true;
	}

	/*
	 * Check for <var> IS NULL or <var> IS NOT NULL
	 */
	else if (IsA(node, NullTest))
	{
		NullTest *ntest;
		bool      operand_supported;

		info = palloc(sizeof(IfxPushdownOprInfo));
		info->expr = (Expr *)node;
		info->expr_string = NULL;
		ntest = (NullTest *)node;

		/*
		 * NullTest on composite types can be thrown away immediately.
		 */
		if (ntest->argisrow)
			return true;

		/*
		 * [NOT] NULL ...
		 */
		switch (ntest->nulltesttype)
		{
			case IS_NULL:
				info->type = IFX_IS_NULL;
				break;
			case IS_NOT_NULL:
				info->type = IFX_IS_NOT_NULL;
				break;
		}

		/*
		 * Assume this expression to be able to be pushed
		 * down...
		 */
		operand_supported = true;

		/*
		 * ...else check wether the expression is passed suitable
		 * for pushdown.
		 */
		switch (((Expr *)(ntest->arg))->type)
		{
			case T_Var:
			{
				Var *var = (Var *) ntest->arg;

				elog(DEBUG2, "varno %d, bogus_varno %d, varlevelsup %d",
					 var->varno, context->foreign_rtid, var->varlevelsup);

				if (var->varno != context->foreign_rtid ||
					var->varlevelsup != 0)
					operand_supported = false;
				break;
			}
			default:
				operand_supported = false;
		}

		/*
		 * If no supported expression is found, return.
		 */
		if (!operand_supported)
			return true;

		/*
		 * Mark this expression for pushdown.
		 */
		IFX_MARK_PREDICATE_ELEM(info, context);

		/*
		 * Deparse the expression node...
		 */
		deparse_predicate_node(node, context, info);
	}

	/*
	 * Check wether this is an OpExpr. If true,
	 * recurse into it...
	 */
	else if (IsA(node, OpExpr))
	{
		OpExpr *opr;

		info = palloc(sizeof(IfxPushdownOprInfo));
		info->expr = (Expr *)node;
		opr  = (OpExpr *)node;
		info->expr_string = NULL;

		if (mapPushdownOperator(opr->opno, info) != IFX_OPR_NOT_SUPPORTED)
		{
			ListCell *cell;
			bool      operand_supported;

			/*
			 * Examine the operands of this operator expression. Please
			 * note that we don't get further here, we stop at the first
			 * layer even when there are more nested expressions.
			 */
			operand_supported = true;
			foreach(cell, opr->args)
			{
				Node *oprarg = lfirst(cell);

				switch(oprarg->type)
				{
					case T_Var:
					{
						Var *var = (Var *) oprarg;

						elog(DEBUG2, "varno %d, bogus_varno %d, varlevelsup %d",
							 var->varno, context->foreign_rtid, var->varlevelsup);

						if (var->varno != context->foreign_rtid ||
							var->varlevelsup != 0)
							operand_supported = false;

						break;
					}
					case T_Const:
						break;
					default:
						operand_supported = false;
						break;
				}

				/* exit immediately */
				if(!operand_supported)
					break;
			}

			/*
			 * If any operand not supported is found, don't
			 * bother adding this operator expression to the pushdown
			 * predicate list.
			 */
			if (!operand_supported)
				return true; /* done */

			/*
			 * Mark this predicate for pushdown.
			 */
			IFX_MARK_PREDICATE_ELEM(info, context);

			/*
			 * Deparse the expression node...
			 */
			deparse_predicate_node(node, context, info);
		}
		else
		{
			/* Operator not supported */
			return true;
		}
	}

	/* reached in case no further nodes to be examined */
	return false;
}

/*
 * Deparse the given Node into a string assigned
 * to the specified IfxPushdownOprInfo pointer.
 */
static void
deparse_predicate_node(Node *node, IfxPushdownOprContext *context,
					   IfxPushdownOprInfo *info)
{
	List *dpc; /* deparse context */
	Node *copy_obj;

	/*
	 * Copy the expression node. We don't want to allow
	 * ChangeVarNodes() to fiddle directly on the baserestrictinfo
	 * nodes.
	 */
	copy_obj = copyObject(node);

	/*
	 * Adjust varno. The RTE currently present aren't adjusted according
	 * to the FDW entries we get for the ForeignScan node. We call
	 * ChangeVarNodes() to adjust them to make them usable by deparsing
	 * later.
	 */
	ChangeVarNodes(copy_obj, context->foreign_rtid, 1, 0);
	dpc = make_deparse_context(context->foreign_relid);
	info->expr_string = cstring_to_text(deparse_expression(copy_obj, dpc, false, false));

	elog(DEBUG1, "deparsed pushdown predicate %d, %s",
		 context->count - 1,
		 text_to_cstring(info->expr_string));
}
