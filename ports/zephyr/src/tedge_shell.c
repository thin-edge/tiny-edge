/* SPDX-License-Identifier: Apache-2.0
 *
 * The client's console commands, on Zephyr's shell.
 */

#include "tedge_internal.h"

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <errno.h>

/* ------------------------------------------------------------------------ */
/* The "tedge" shell root                                                    */
/*                                                                           */
/* One root command for the whole client. Everything else, the module's own  */
/* features and the application's test aids alike, hangs subcommands off it  */
/* with SHELL_SUBCMD_ADD((tedge), ...), so a device ends up with one command */
/* tree instead of two roots of the same name, only one of which the shell   */
/* would ever reach.                                                         */
/* ------------------------------------------------------------------------ */

SHELL_SUBCMD_SET_CREATE(tedge_shell_cmds, (tedge));
SHELL_CMD_REGISTER(tedge, &tedge_shell_cmds, "thin-edge.io client", NULL);

#if defined(CONFIG_TEDGE_PARAMETERS_SCHEMA)
/* ------------------------------------------------------------------------ */
/* "tedge params schema"                                                     */
/*                                                                           */
/* Whoever registers a set in the tenant needs its schema, and the device is */
/* the only thing that knows what the running image actually declares. A     */
/* console command is the shortest path from one to the other.               */
/* ------------------------------------------------------------------------ */

/* Big enough for a set of a dozen parameters with their descriptions; the
 * generator says so rather than truncating when it is not. */
#define SCHEMA_BUF 2048

static int print_schema(const struct shell *sh, const char *set_name)
{
	char *buf = tedge_alloc(SCHEMA_BUF);
	int rc;

	if (buf == NULL) {
		shell_error(sh, "out of client heap");
		return -ENOMEM;
	}
	rc = tedge_params_schema(set_name, buf, SCHEMA_BUF);
	if (rc == -ENOENT) {
		shell_error(sh, "no parameter set '%s' is declared", set_name);
	} else if (rc == -ENOMEM) {
		shell_error(sh, "'%s' needs more than %d bytes of schema",
			    set_name, SCHEMA_BUF);
	} else if (rc < 0) {
		shell_error(sh, "could not produce the schema (%d)", rc);
	} else {
		shell_print(sh, "%s", buf);
	}
	tedge_free(buf);
	return rc < 0 ? rc : 0;
}

static int cmd_params_schema(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		return print_schema(sh, argv[1]);
	}
	if (tedge_params_set_count() == 0) {
		shell_warn(sh, "this image declares no parameter set");
		return 0;
	}
	for (size_t i = 0; i < tedge_params_set_count(); i++) {
		int rc = print_schema(sh, tedge_params_set_name(i, NULL));

		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int cmd_params_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (tedge_params_set_count() == 0) {
		shell_warn(sh, "this image declares no parameter set");
		return 0;
	}
	for (size_t i = 0; i < tedge_params_set_count(); i++) {
		size_t count;
		const char *name = tedge_params_set_name(i, &count);

		shell_print(sh, "%s (%zu parameters)", name, count);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	params_cmds,
	SHELL_CMD_ARG(schema, NULL,
		      "Print the registration body for a set, or for every "
		      "set.\nUsage: tedge params schema [<set>]",
		      cmd_params_schema, 1, 1),
	SHELL_CMD(list, NULL, "List the declared parameter sets",
		  cmd_params_list),
	SHELL_SUBCMD_SET_END);

SHELL_SUBCMD_ADD((tedge), params, &params_cmds,
		 "Parameters the cloud may change", NULL, 1, 0);
#endif /* CONFIG_TEDGE_PARAMETERS_SCHEMA */
