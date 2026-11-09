/* SPDX-License-Identifier: Apache-2.0
 *
 * The public calls of the features this image was built without. tedge.h
 * declares every call whatever the build, so an application compiles
 * either way; here the absent ones answer -ENOTSUP.
 */

#include "tedge_internal.h"
#include "tedge_util.h"

#include <errno.h>

#if !defined(TEDGE_TELEMETRY)
int tedge_publish_measurement(const char *type,
			      const struct tedge_measurement_value *values,
			      size_t count, int64_t timestamp_ms)
{
	TEDGE_UNUSED(type);
	TEDGE_UNUSED(values);
	TEDGE_UNUSED(count);
	TEDGE_UNUSED(timestamp_ms);
	return -ENOTSUP;
}

int tedge_publish_event(const char *type, const char *text,
			int64_t timestamp_ms)
{
	TEDGE_UNUSED(type);
	TEDGE_UNUSED(text);
	TEDGE_UNUSED(timestamp_ms);
	return -ENOTSUP;
}

int tedge_raise_alarm(const char *type, enum tedge_alarm_severity severity,
		      const char *text)
{
	TEDGE_UNUSED(type);
	TEDGE_UNUSED(severity);
	TEDGE_UNUSED(text);
	return -ENOTSUP;
}

int tedge_clear_alarm(const char *type)
{
	TEDGE_UNUSED(type);
	return -ENOTSUP;
}

uint32_t tedge_telemetry_dropped(void)
{
	return 0;
}
#endif /* !TEDGE_TELEMETRY */

#if !defined(TEDGE_LOG_UPLOAD)
int tedge_register_log_type(const char *type, tedge_log_reader_t reader,
			    void *user_data)
{
	TEDGE_UNUSED(type);
	TEDGE_UNUSED(reader);
	TEDGE_UNUSED(user_data);
	return -ENOTSUP;
}
#endif /* !TEDGE_LOG_UPLOAD */

#if !defined(TEDGE_PARAMETERS)
int tedge_declare_parameters(const char *set,
			     const struct tedge_parameter *params,
			     size_t count, tedge_parameters_changed_t on_change,
			     void *user_data)
{
	TEDGE_UNUSED(set);
	TEDGE_UNUSED(params);
	TEDGE_UNUSED(count);
	TEDGE_UNUSED(on_change);
	TEDGE_UNUSED(user_data);
	return -ENOTSUP;
}

int tedge_parameter_get_bool(const char *set, const char *name, bool *out)
{
	TEDGE_UNUSED(set);
	TEDGE_UNUSED(name);
	TEDGE_UNUSED(out);
	return -ENOTSUP;
}

int tedge_parameter_get_int(const char *set, const char *name, int32_t *out)
{
	TEDGE_UNUSED(set);
	TEDGE_UNUSED(name);
	TEDGE_UNUSED(out);
	return -ENOTSUP;
}

int tedge_parameter_get_string(const char *set, const char *name, char *out,
			       size_t len)
{
	TEDGE_UNUSED(set);
	TEDGE_UNUSED(name);
	TEDGE_UNUSED(out);
	TEDGE_UNUSED(len);
	return -ENOTSUP;
}
#endif /* !TEDGE_PARAMETERS */
