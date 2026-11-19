/* SPDX-License-Identifier: Apache-2.0
 *
 * When to ask Cumulocity for a new token (s/uat). Pure, so the cases that
 * matter (no answer to a request, a token that is about to expire) are
 * unit-tested rather than reasoned about.
 */

#include "tedge_internal.h"

bool tedge_jwt_request_due(int64_t now_ms, int64_t got_at_ms,
			   int64_t asked_at_ms, int64_t refresh_ms,
			   int64_t retry_ms)
{
	/* A request is out: give its answer time to arrive. */
	if (asked_at_ms != 0 && now_ms - asked_at_ms < retry_ms) {
		return false;
	}
	/* No token yet, or the request for one went unanswered. */
	if (got_at_ms == 0) {
		return true;
	}
	return now_ms - got_at_ms >= refresh_ms;
}
