/*
 * dbutils.c - Database connection/management functions
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 *
 */

#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "repmgr.h"


#include "catalog/pg_control.h"

static PGconn *_establish_db_connection(const char *conninfo,
										const bool exit_on_error,
										const bool log_notice,
										const bool verbose_only);

static bool _set_config(PGconn *conn, const char *config_param, const char *sqlquery);
static int _get_node_record(PGconn *conn, char *sqlquery, t_node_info *node_info);
static void _populate_node_record(PGresult *res, t_node_info *node_info, int row);

/* ==================== */
/* Connection functions */
/* ==================== */

/*
 * _establish_db_connection()
 *
 * Connect to a database using a conninfo string.
 *
 * NOTE: *do not* use this for replication connections; instead use:
 *	 establish_db_connection_by_params()
 */

static PGconn *
_establish_db_connection(const char *conninfo, const bool exit_on_error, const bool log_notice, const bool verbose_only)
{
	PGconn	   *conn = NULL;
	char		connection_string[MAXLEN];

	strncpy(connection_string, conninfo, MAXLEN);

	/* TODO: only set if not already present */
	strcat(connection_string, " fallback_application_name='repmgr'");

	log_debug(_("connecting to: '%s'"), connection_string);

	conn = PQconnectdb(connection_string);

	/* Check to see that the backend connection was successfully made */
	if ((PQstatus(conn) != CONNECTION_OK))
	{
		bool emit_log = true;

		if (verbose_only == true && verbose_logging == false)
			emit_log = false;

		if (emit_log)
		{
			if (log_notice)
			{
				log_notice(_("connection to database failed: %s"),
						   PQerrorMessage(conn));
			}
			else
			{
				log_error(_("connection to database failed: %s"),
						  PQerrorMessage(conn));
			}
		}

		if (exit_on_error)
		{
			PQfinish(conn);
			exit(ERR_DB_CON);
		}
	}

	/*
	 * set "synchronous_commit" to "local" in case synchronous replication is in use
	 *
	 * XXX set this explicitly before any write operations
	 */

	else if (set_config(conn, "synchronous_commit", "local") == false)
	{
		if (exit_on_error)
		{
			PQfinish(conn);
			exit(ERR_DB_CON);
		}
	}

	return conn;
}


/*
 * Establish a database connection, optionally exit on error
 */
PGconn *
establish_db_connection(const char *conninfo, const bool exit_on_error)
{
	return _establish_db_connection(conninfo, exit_on_error, false, false);
}


PGconn *
establish_db_connection_as_user(const char *conninfo,
								const char *user,
								const bool exit_on_error)
{
	PGconn	   *conn = NULL;
	t_conninfo_param_list conninfo_params;
	bool		parse_success;
	char	   *errmsg = NULL;

	initialize_conninfo_params(&conninfo_params, false);

	parse_success = parse_conninfo_string(conninfo, &conninfo_params, errmsg, true);

	if (parse_success == false)
	{
		log_error(_("unable to pass provided conninfo string:\n	 %s"), errmsg);
		return NULL;
	}

	param_set(&conninfo_params, "user", user);

	conn = establish_db_connection_by_params((const char**)conninfo_params.keywords,
											 (const char**)conninfo_params.values,
											 false);

	return conn;
}


PGconn *
establish_db_connection_by_params(const char *keywords[], const char *values[],
								  const bool exit_on_error)
{
	PGconn	   *conn;
	bool		replication_connection = false;
	int			i;

	/* Connect to the database using the provided parameters */
	conn = PQconnectdbParams(keywords, values, true);

	/* Check to see that the backend connection was successfully made */
	if ((PQstatus(conn) != CONNECTION_OK))
	{
		log_error(_("connection to database failed:\n	%s"),
				  PQerrorMessage(conn));
		if (exit_on_error)
		{
			PQfinish(conn);
			exit(ERR_DB_CON);
		}
	}
	else
	{
		/*
		 * set "synchronous_commit" to "local" in case synchronous replication is in
		 * use (provided this is not a replication connection)
		 */

		for (i = 0; keywords[i]; i++)
		{
			if (strcmp(keywords[i], "replication") == 0)
				replication_connection = true;
		}

		if (replication_connection == false && set_config(conn, "synchronous_commit", "local") == false)
		{
			if (exit_on_error)
			{
				PQfinish(conn);
				exit(ERR_DB_CON);
			}
		}
	}

	return conn;
}

/* =============================== */
/* conninfo manipulation functions */
/* =============================== */


void
initialize_conninfo_params(t_conninfo_param_list *param_list, bool set_defaults)
{
	PQconninfoOption *defs = NULL;
	PQconninfoOption *def;
	int c;

	defs = PQconndefaults();
	param_list->size = 0;

	/* Count maximum number of parameters */
	for (def = defs; def->keyword; def++)
		param_list->size ++;

	/* Initialize our internal parameter list */
	param_list->keywords = pg_malloc0(sizeof(char *) * (param_list->size + 1));
	param_list->values = pg_malloc0(sizeof(char *) * (param_list->size + 1));

	for (c = 0; c < param_list->size; c++)
	{
		param_list->keywords[c] = NULL;
		param_list->values[c] = NULL;
	}

	if (set_defaults == true)
	{
		/* Pre-set any defaults */

		for (def = defs; def->keyword; def++)
		{
			if (def->val != NULL && def->val[0] != '\0')
			{
				param_set(param_list, def->keyword, def->val);
			}
		}
	}
}


void
copy_conninfo_params(t_conninfo_param_list *dest_list, t_conninfo_param_list *source_list)
{
	int c;
	for (c = 0; c < source_list->size && source_list->keywords[c] != NULL; c++)
	{
		if (source_list->values[c] != NULL && source_list->values[c][0] != '\0')
		{
			param_set(dest_list, source_list->keywords[c], source_list->values[c]);
		}
	}
}

void
param_set(t_conninfo_param_list *param_list, const char *param, const char *value)
{
	int c;
	int value_len = strlen(value) + 1;

	/*
	 * Scan array to see if the parameter is already set - if not, replace it
	 */
	for (c = 0; c < param_list->size && param_list->keywords[c] != NULL; c++)
	{
		if (strcmp(param_list->keywords[c], param) == 0)
		{
			if (param_list->values[c] != NULL)
				pfree(param_list->values[c]);

			param_list->values[c] = pg_malloc0(value_len);
			strncpy(param_list->values[c], value, value_len);

			return;
		}
	}

	/*
	 * Parameter not in array - add it and its associated value
	 */
	if (c < param_list->size)
	{
		int param_len = strlen(param) + 1;
		param_list->keywords[c] = pg_malloc0(param_len);
		param_list->values[c] = pg_malloc0(value_len);

		strncpy(param_list->keywords[c], param, param_len);
		strncpy(param_list->values[c], value, value_len);
	}

	/*
	 * It's theoretically possible a parameter couldn't be added as
	 * the array is full, but it's highly improbable so we won't
	 * handle it at the moment.
	 */
}


char *
param_get(t_conninfo_param_list *param_list, const char *param)
{
	int c;

	for (c = 0; c < param_list->size && param_list->keywords[c] != NULL; c++)
	{
		if (strcmp(param_list->keywords[c], param) == 0)
		{
			if (param_list->values[c] != NULL && param_list->values[c][0] != '\0')
				return param_list->values[c];
			else
				return NULL;
		}
	}

	return NULL;
}


/*
 * Parse a conninfo string into a t_conninfo_param_list
 *
 * See conn_to_param_list() to do the same for a PQconn
 */
bool
parse_conninfo_string(const char *conninfo_str, t_conninfo_param_list *param_list, char *errmsg, bool ignore_application_name)
{
	PQconninfoOption *connOptions;
	PQconninfoOption *option;

	connOptions = PQconninfoParse(conninfo_str, &errmsg);

	if (connOptions == NULL)
		return false;

	for (option = connOptions; option && option->keyword; option++)
	{
		/* Ignore non-set or blank parameter values*/
		if ((option->val == NULL) ||
		   (option->val != NULL && option->val[0] == '\0'))
			continue;

		/* Ignore application_name */
		if (ignore_application_name == true && strcmp(option->keyword, "application_name") == 0)
			continue;

		param_set(param_list, option->keyword, option->val);
	}

	return true;
}

/*
 * Parse a PQconn into a t_conninfo_param_list
 *
 * See parse_conninfo_string() to do the same for a conninfo string
 */
void
conn_to_param_list(PGconn *conn, t_conninfo_param_list *param_list)
{
	PQconninfoOption *connOptions;
	PQconninfoOption *option;

	connOptions = PQconninfo(conn);
	for (option = connOptions; option && option->keyword; option++)
	{
		/* Ignore non-set or blank parameter values*/
		if ((option->val == NULL) ||
		   (option->val != NULL && option->val[0] == '\0'))
			continue;

		param_set(param_list, option->keyword, option->val);
	}
}


/* ===================== */
/* transaction functions */
/* ===================== */

bool
begin_transaction(PGconn *conn)
{
	PGresult   *res;

	log_verbose(LOG_DEBUG, "begin_transaction()");

	res = PQexec(conn, "BEGIN");

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_error(_("Unable to begin transaction:\n	 %s"),
				  PQerrorMessage(conn));

		PQclear(res);
		return false;
	}

	PQclear(res);

	return true;
}


bool
commit_transaction(PGconn *conn)
{
	PGresult   *res;

	log_verbose(LOG_DEBUG, "commit_transaction()");

	res = PQexec(conn, "COMMIT");

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_error(_("Unable to commit transaction:\n  %s"),
				  PQerrorMessage(conn));
		PQclear(res);

		return false;
	}

	PQclear(res);

	return true;
}


bool
rollback_transaction(PGconn *conn)
{
	PGresult   *res;

	log_verbose(LOG_DEBUG, "rollback_transaction()");

	res = PQexec(conn, "ROLLBACK");

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_error(_("Unable to rollback transaction:\n	%s"),
				  PQerrorMessage(conn));
		PQclear(res);

		return false;
	}

	PQclear(res);

	return true;
}


/* ========================== */
/* GUC manipulation functions */
/* ========================== */

static bool
_set_config(PGconn *conn, const char *config_param, const char *sqlquery)
{
	PGresult   *res;

	res = PQexec(conn, sqlquery);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_error("unable to set '%s': %s", config_param, PQerrorMessage(conn));
		PQclear(res);
		return false;
	}

	PQclear(res);

	return true;
}

bool
set_config(PGconn *conn, const char *config_param,	const char *config_value)
{
	char		sqlquery[MAX_QUERY_LEN];

	sqlquery_snprintf(sqlquery,
					  "SET %s TO '%s'",
					  config_param,
					  config_value);

	log_verbose(LOG_DEBUG, "set_config():\n%s", sqlquery);

	return _set_config(conn, config_param, sqlquery);
}

bool
set_config_bool(PGconn *conn, const char *config_param, bool state)
{
	char		sqlquery[MAX_QUERY_LEN];

	sqlquery_snprintf(sqlquery,
					  "SET %s TO %s",
					  config_param,
					  state ? "TRUE" : "FALSE");

	log_verbose(LOG_DEBUG, "set_config_bool():\n%s\n", sqlquery);

	return _set_config(conn, config_param, sqlquery);
}

/* ============================ */
/* Server information functions */
/* ============================ */

/*
 * Return the server version number for the connection provided
 */
int
get_server_version(PGconn *conn, char *server_version)
{
	PGresult   *res;
	res = PQexec(conn,
				 "SELECT pg_catalog.current_setting('server_version_num'), "
				 "       pg_catalog.current_setting('server_version')");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_error(_("unable to determine server version number:\n%s"),
				  PQerrorMessage(conn));
		PQclear(res);
		return -1;
	}

	if (server_version != NULL)
		strcpy(server_version, PQgetvalue(res, 0, 0));

	return atoi(PQgetvalue(res, 0, 0));
}

int
is_standby(PGconn *conn)
{
	PGresult   *res;
	int			result = 0;
	char	   *sqlquery = "SELECT pg_catalog.pg_is_in_recovery()";

	log_verbose(LOG_DEBUG, "is_standby(): %s", sqlquery);

	res = PQexec(conn, sqlquery);

	if (res == NULL || PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_error(_("unable to determine if server is in recovery:\n  %s"),
				PQerrorMessage(conn));
		result = -1;
	}
	else if (PQntuples(res) == 1 && strcmp(PQgetvalue(res, 0, 0), "t") == 0)
	{
		result = 1;
	}

	PQclear(res);
	return result;
}

/*
 * Read the node list from the provided connection and attempt to connect to each node
 * in turn to definitely establish if it's the cluster primary.
 *
 * The node list is returned in the order which makes it likely that the
 * current primary will be returned first, reducing the number of speculative
 * connections which need to be made to other nodes.
 *
 * If master_conninfo_out points to allocated memory of MAXCONNINFO in length,
 * the primary server's conninfo string will be copied there.
 */

PGconn *
get_master_connection(PGconn *conn,
					  int *master_id, char *master_conninfo_out)
{
	PQExpBufferData	  query;

	PGconn	   *remote_conn = NULL;
	PGresult   *res;

	char		remote_conninfo_stack[MAXCONNINFO];
	char	   *remote_conninfo = &*remote_conninfo_stack;

	int			i,
				node_id;

	/*
	 * If the caller wanted to get a copy of the connection info string, sub
	 * out the local stack pointer for the pointer passed by the caller.
	 */
	if (master_conninfo_out != NULL)
		remote_conninfo = master_conninfo_out;

	if (master_id != NULL)
	{
		*master_id = NODE_NOT_FOUND;
	}

	/* find all registered nodes  */
	log_info(_("retrieving node list"));
	initPQExpBuffer(&query);
	appendPQExpBuffer(&query,
					  "  SELECT node_id, conninfo, "
					  "         CASE WHEN type = 'master' THEN 1 ELSE 2 END AS type_priority"
					  "	   FROM repmgr.nodes "
					  "   WHERE type != 'witness' "
					  "ORDER BY active DESC, type_priority, priority, node_id");

	log_verbose(LOG_DEBUG, "get_master_connection():\n%s", query.data);

	res = PQexec(conn, query.data);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_error(_("unable to retrieve node records:\n	 %s"),
				  PQerrorMessage(conn));
		PQclear(res);
		return NULL;
	}

	termPQExpBuffer(&query);

	for (i = 0; i < PQntuples(res); i++)
	{
		int is_node_standby;

		/* initialize with the values of the current node being processed */
		node_id = atoi(PQgetvalue(res, i, 0));
		strncpy(remote_conninfo, PQgetvalue(res, i, 1), MAXCONNINFO);
		log_verbose(LOG_INFO,
					_("checking role of cluster node '%i'"),
					node_id);
		remote_conn = establish_db_connection(remote_conninfo, false);

		if (PQstatus(remote_conn) != CONNECTION_OK)
			continue;

		is_node_standby = is_standby(remote_conn);

		if (is_node_standby == -1)
		{
			log_error(_("unable to retrieve recovery state from node %i:\n	%s"),
					  node_id,
					  PQerrorMessage(remote_conn));
			PQfinish(remote_conn);
			continue;
		}

		/* if is_standby() returns 0, queried node is the master */
		if (is_node_standby == 0)
		{
			PQclear(res);
			log_debug(_("get_master_connection(): current master node is %i"), node_id);

			if (master_id != NULL)
			{
				*master_id = node_id;
			}

			return remote_conn;
		}

		PQfinish(remote_conn);
	}

	PQclear(res);
	return NULL;
}



/*
 * Return the id of the active master node, or NODE_NOT_FOUND if no
 * record available.
 *
 * This reports the value stored in the database only and
 * does not verify whether the node is actually available
 */
int
get_master_node_id(PGconn *conn)
{
	PQExpBufferData	  query;
	PGresult   *res;
	int			retval;

	initPQExpBuffer(&query);
	appendPQExpBuffer(&query,
					  "SELECT node_id		  "
					  "	 FROM repmgr.nodes	  "
					  " WHERE type = 'master' "
					  "   AND active IS TRUE  ");

	log_verbose(LOG_DEBUG, "get_master_node_id():\n%s", query.data);

	res = PQexec(conn, query.data);

	termPQExpBuffer(&query);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_error(_("get_master_node_id(): query failed\n  %s"),
				  PQerrorMessage(conn));
		retval = NODE_NOT_FOUND;
	}
	else if (PQntuples(res) == 0)
	{
		log_verbose(LOG_WARNING, _("get_master_node_id(): no active primary found\n"));
		retval = NODE_NOT_FOUND;
	}
	else
	{
		retval = atoi(PQgetvalue(res, 0, 0));
	}
	PQclear(res);

	return retval;
}

/* ================ */
/* result functions */
/* ================ */

bool atobool(const char *value)
{
	return (strcmp(value, "t") == 0)
		? true
		: false;
}

/* ===================== */
/* Node record functions */
/* ===================== */


static int
_get_node_record(PGconn *conn, char *sqlquery, t_node_info *node_info)
{
	int         ntuples;
	PGresult   *res;

	res = PQexec(conn, sqlquery);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		return NODE_RECORD_QUERY_ERROR;
	}

	ntuples = PQntuples(res);

	if (ntuples == 0)
	{
		PQclear(res);
		return NODE_RECORD_NOT_FOUND;
	}

	_populate_node_record(res, node_info, 0);

	PQclear(res);

	return ntuples;
}


static void
_populate_node_record(PGresult *res, t_node_info *node_info, int row)
{
	node_info->node_id = atoi(PQgetvalue(res, row, 0));
	node_info->type = parse_node_type(PQgetvalue(res, row, 1));

	if (PQgetisnull(res, row, 2))
	{
		node_info->upstream_node_id = NO_UPSTREAM_NODE;
	}
	else
	{
		node_info->upstream_node_id = atoi(PQgetvalue(res, row, 2));
	}

	strncpy(node_info->node_name, PQgetvalue(res, row, 3), MAXLEN);
	strncpy(node_info->conninfo, PQgetvalue(res, row, 4), MAXLEN);
	strncpy(node_info->slot_name, PQgetvalue(res, row, 5), MAXLEN);
	node_info->priority = atoi(PQgetvalue(res, row, 6));
	node_info->active = atobool(PQgetvalue(res, row, 7));

	/* Set remaining struct fields with default values */
	node_info->is_ready = false;
	node_info->is_visible = false;
	node_info->xlog_location = InvalidXLogRecPtr;
}


t_server_type
parse_node_type(const char *type)
{
	if (strcmp(type, "master") == 0)
	{
		return MASTER;
	}
	else if (strcmp(type, "standby") == 0)
	{
		return STANDBY;
	}
	else if (strcmp(type, "witness") == 0)
	{
		return WITNESS;
	}
	else if (strcmp(type, "bdr") == 0)
	{
		return BDR;
	}

	return UNKNOWN;
}

const char *
get_node_type_string(t_server_type type)
{
	switch(type)
	{
		case MASTER:
			return "master";
		case STANDBY:
			return "standby";
		case WITNESS:
			return "witness";
		case BDR:
			return "bdr";
		/* this should never happen */
		case UNKNOWN:
		default:
			log_error(_("unknown node type %i"), type);
			return "unknown";
	}
}


int
get_node_record(PGconn *conn, int node_id, t_node_info *node_info)
{
	PQExpBufferData	  query;
	int		    result;

	initPQExpBuffer(&query);
	appendPQExpBuffer(&query,
					  "SELECT node_id, type, upstream_node_id, node_name, conninfo, slot_name, priority, active"
					  "  FROM repmgr.nodes "
					  " WHERE node_id = %i",
					  node_id);

	log_verbose(LOG_DEBUG, "get_node_record():\n%s", query.data);

	result = _get_node_record(conn, query.data, node_info);
	termPQExpBuffer(&query);

	if (result == NODE_RECORD_NOT_FOUND)
	{
		log_verbose(LOG_DEBUG, "get_node_record(): no record found for node %i", node_id);
	}


	return result;
}


bool
create_node_record(PGconn *conn, char *action, t_node_info *node_info)
{
	PQExpBufferData	  query;
	char			upstream_node_id[MAXLEN];
	char		slot_name_buf[MAXLEN];
	PGresult   *res;

	if (node_info->upstream_node_id == NO_UPSTREAM_NODE)
	{
		/*
		 * No explicit upstream node id provided for standby - attempt to
		 * get primary node id
		 */
		if (node_info->type == STANDBY)
		{
			int primary_node_id = get_master_node_id(conn);
			maxlen_snprintf(upstream_node_id, "%i", primary_node_id);
		}
		else
		{
			maxlen_snprintf(upstream_node_id, "%s", "NULL");
		}
	}
	else
	{
		maxlen_snprintf(upstream_node_id, "%i", node_info->upstream_node_id);
	}

	if (node_info->slot_name[0])
	{
		maxlen_snprintf(slot_name_buf, "'%s'", node_info->slot_name);
	}
	else
	{
		maxlen_snprintf(slot_name_buf, "%s", "NULL");
	}

	/* XXX convert to placeholder query */

	initPQExpBuffer(&query);

	appendPQExpBuffer(&query,
					  "INSERT INTO repmgr.nodes "
					  "       (node_id, type, upstream_node_id, "
					  "        node_name, conninfo, slot_name, "
					  "        priority, active) "
					  "VALUES (%i, '%s', %s, '%s', '%s', %s, %i, %s) ",
					  node_info->node_id,
					  get_node_type_string(node_info->type),
					  upstream_node_id,
					  node_info->node_name,
					  node_info->conninfo,
					  slot_name_buf,
					  node_info->priority,
					  node_info->active == true ? "TRUE" : "FALSE");

	log_verbose(LOG_DEBUG, "create_node_record(): %s", query.data);

	if (action != NULL)
	{
		log_verbose(LOG_DEBUG, "create_node_record(): action is \"%s\"", action);
	}

	res = PQexec(conn, query.data);

	termPQExpBuffer(&query);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_error(_("unable to create node record:\n  %s"),
				  PQerrorMessage(conn));
		PQclear(res);
		return false;
	}

	PQclear(res);

	return true;
}


bool
update_node_record(PGconn *conn, char *action, t_node_info *node_info)
{
	PQExpBufferData	query;
	char			upstream_node_id[MAXLEN];
	char			slot_name_buf[MAXLEN];
	PGresult   	   *res;

	/* XXX this segment copied from create_node_record() */
	if (node_info->upstream_node_id == NO_UPSTREAM_NODE)
	{
		/*
		 * No explicit upstream node id provided for standby - attempt to
		 * get primary node id
		 */
		if (node_info->type == STANDBY)
		{
			int primary_node_id = get_master_node_id(conn);
			maxlen_snprintf(upstream_node_id, "%i", primary_node_id);
		}
		else
		{
			maxlen_snprintf(upstream_node_id, "%s", "NULL");
		}
	}
	else
	{
		maxlen_snprintf(upstream_node_id, "%i", node_info->upstream_node_id);
	}

	if (node_info->slot_name[0])
	{
		maxlen_snprintf(slot_name_buf, "'%s'", node_info->slot_name);
	}
	else
	{
		maxlen_snprintf(slot_name_buf, "%s", "NULL");
	}

	initPQExpBuffer(&query);

	/* XXX convert to placeholder query */

	appendPQExpBuffer(&query,
					  "UPDATE repmgr.nodes SET "
					  "       type = '%s', "
					  "       upstream_node_id = %s, "
					  "       node_name = '%s', "
					  "       conninfo = '%s', "
					  "       slot_name = %s, "
					  "       priority = %i, "
					  "       active = %s "
					  " WHERE node_id = %i ",
					  get_node_type_string(node_info->type),
					  upstream_node_id,
					  node_info->node_name,
					  node_info->conninfo,
					  slot_name_buf,
					  node_info->priority,
					  node_info->active == true ? "TRUE" : "FALSE",
					  node_info->node_id);

	log_verbose(LOG_DEBUG, "update_node_record(): %s", query.data);

	if (action != NULL)
	{
		log_verbose(LOG_DEBUG, "update_node_record(): action is \"%s\"", action);
	}

	res = PQexec(conn, query.data);

	termPQExpBuffer(&query);

	if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		log_error(_("unable to update node record:\n  %s"),
				  PQerrorMessage(conn));
		PQclear(res);
		return false;
	}

	PQclear(res);

	return true;
}

