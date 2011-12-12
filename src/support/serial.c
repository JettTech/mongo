/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * Serialization: serialization support allows scheduling operations requiring
 * serialized access to a piece of memory, normally by a different thread of
 * control.  This includes updating and evicting pages from trees.
 *
 * __wt_session_serialize_func --
 *	Schedule a serialization request, and block or spin until it completes.
 */
int
__wt_session_serialize_func(WT_SESSION_IMPL *session,
    wq_state_t op, void (*func)(WT_SESSION_IMPL *), void *args)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/*
	 * Threads serializing access to data using a function:
	 *	call the function while holding a spinlock
	 *	update the session sleeping state, and
	 *	if necessary, block until an async action completes.
	 */
	session->wq_args = args;
	session->wq_sleeping = (op == WT_SERIAL_EVICT);

	/* Functions are serialized by holding a spinlock. */
	if (op != WT_SERIAL_REENTER)
		__wt_spin_lock(session, &conn->serial_lock);

	func(session);

	if (op != WT_SERIAL_REENTER)
		__wt_spin_unlock(session, &conn->serial_lock);

	switch (op) {
	case WT_SERIAL_EVICT:
		__wt_evict_server_wake(session);
		break;
	default:
		break;
	}

	/*
	 * If we are waiting on a server thread, block on the session condition
	 * variable: when the operation is complete, this will be notified and
	 * we can continue.
	 */
	if (session->wq_sleeping)
		__wt_cond_wait(session, session->cond);
	return (session->wq_ret);
}

/*
 * __wt_session_serialize_wrapup --
 *	Server function cleanup.
 */
void
__wt_session_serialize_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page, int ret)
{
	if (ret == 0 && page != NULL) {
		/*
		 * If passed a page and the return value is OK, we modified the
		 * page.  Wake the eviction server as necessary if the page
		 * has become too large.
		 */
		ret = __wt_page_set_modified(session, page);
		(void)__wt_eviction_page_check(session, page);
	}

	/* Set the return value. */
	session->wq_ret = ret;

	/*
	 * Publish: there must be a barrier to ensure the return value is set
	 * before the calling thread can see its results, and the page's new
	 * write generation makes it to memory.  The latter isn't a correctness
	 * issue, the write generation just needs to be updated so that readers
	 * get credit for reading the right version of the page, otherwise,
	 * they will have to retry their update for reading an old version of
	 * the page.
	 */
	WT_PUBLISH(session->wq_state, WT_SERIAL_NONE);

	/* If the calling thread is sleeping, wake it up. */
	if (session->wq_sleeping)
		__wt_cond_signal(session, session->cond);
}
