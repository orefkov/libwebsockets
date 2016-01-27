/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2015 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"

static int
lws_calllback_as_writeable(struct lws *wsi)
{
	int n;

	switch (wsi->mode) {
	case LWSCM_WS_CLIENT:
		n = LWS_CALLBACK_CLIENT_WRITEABLE;
		break;
	case LWSCM_WS_SERVING:
		n = LWS_CALLBACK_SERVER_WRITEABLE;
		break;
	default:
		n = LWS_CALLBACK_HTTP_WRITEABLE;
		break;
	}
	lwsl_debug("%s: %p (user=%p)\n", __func__, wsi, wsi->user_space);
	return user_callback_handle_rxflow(wsi->protocol->callback,
					   wsi, (enum lws_callback_reasons) n,
					   wsi->user_space, NULL, 0);
}

int
lws_handle_POLLOUT_event(struct lws *wsi, struct lws_pollfd *pollfd)
{
	int write_type = LWS_WRITE_PONG;
	struct lws_tokens eff_buf;
#ifdef LWS_USE_HTTP2
	struct lws *wsi2;
#endif
	int ret, m, n;

	/*
	 * user callback is lowest priority to get these notifications
	 * actually, since other pending things cannot be disordered
	 */

	/* Priority 1: pending truncated sends are incomplete ws fragments
	 *	       If anything else sent first the protocol would be
	 *	       corrupted.
	 */
	if (wsi->trunc_len) {
		if (lws_issue_raw(wsi, wsi->trunc_alloc + wsi->trunc_offset,
				  wsi->trunc_len) < 0) {
			lwsl_info("%s signalling to close\n", __func__);
			return -1;
		}
		/* leave POLLOUT active either way */
		return 0;
	} else
		if (wsi->state == LWSS_FLUSHING_STORED_SEND_BEFORE_CLOSE)
			return -1; /* retry closing now */


#ifdef LWS_USE_HTTP2
	/* Priority 2: protocol packets
	 */
	if (wsi->pps) {
		lwsl_info("servicing pps %d\n", wsi->pps);
		switch (wsi->pps) {
		case LWS_PPS_HTTP2_MY_SETTINGS:
		case LWS_PPS_HTTP2_ACK_SETTINGS:
			lws_http2_do_pps_send(lws_get_context(wsi), wsi);
			break;
		default:
			break;
		}
		wsi->pps = LWS_PPS_NONE;
		lws_rx_flow_control(wsi, 1);

		return 0; /* leave POLLOUT active */
	}
#endif
	/* Priority 3: pending control packets (pong or close)
	 */
	if ((wsi->state == LWSS_ESTABLISHED &&
	     wsi->u.ws.ping_pending_flag) ||
	    (wsi->state == LWSS_RETURNED_CLOSE_ALREADY &&
	     wsi->u.ws.payload_is_close)) {

		if (wsi->u.ws.payload_is_close)
			write_type = LWS_WRITE_CLOSE;

		n = lws_write(wsi, &wsi->u.ws.ping_payload_buf[LWS_PRE],
			      wsi->u.ws.ping_payload_len, write_type);
		if (n < 0)
			return -1;

		/* well he is sent, mark him done */
		wsi->u.ws.ping_pending_flag = 0;
		if (wsi->u.ws.payload_is_close)
			/* oh... a close frame was it... then we are done */
			return -1;

		/* otherwise for PING, leave POLLOUT active either way */
		return 0;
	}

	/* Priority 4: if we are closing, not allowed to send more data frags
	 *	       which means user callback or tx ext flush banned now
	 */
	if (wsi->state == LWSS_RETURNED_CLOSE_ALREADY)
		goto user_service;

	/* Priority 5: Tx path extension with more to send
	 *
	 *	       These are handled as new fragments each time around
	 *	       So while we must block new writeable callback to enforce
	 *	       payload ordering, but since they are always complete
	 *	       fragments control packets can interleave OK.
	 */
	if (wsi->state == LWSS_ESTABLISHED && wsi->u.ws.tx_draining_ext) {
		lwsl_ext("SERVICING TX EXT DRAINING\n");
		if (lws_write(wsi, NULL, 0, LWS_WRITE_CONTINUATION) < 0)
			return -1;
		/* leave POLLOUT active */
		return 0;
	}

	/* Priority 6: user can get the callback
	 */
	m = lws_ext_cb_active(wsi, LWS_EXT_CB_IS_WRITEABLE, NULL, 0);
#ifndef LWS_NO_EXTENSIONS
	if (!wsi->extension_data_pending)
		goto user_service;
#endif
	/*
	 * check in on the active extensions, see if they
	 * had pending stuff to spill... they need to get the
	 * first look-in otherwise sequence will be disordered
	 *
	 * NULL, zero-length eff_buf means just spill pending
	 */

	ret = 1;
	while (ret == 1) {

		/* default to nobody has more to spill */

		ret = 0;
		eff_buf.token = NULL;
		eff_buf.token_len = 0;

		/* give every extension a chance to spill */

		m = lws_ext_cb_active(wsi,
					LWS_EXT_CB_PACKET_TX_PRESEND,
					       &eff_buf, 0);
		if (m < 0) {
			lwsl_err("ext reports fatal error\n");
			return -1;
		}
		if (m)
			/*
			 * at least one extension told us he has more
			 * to spill, so we will go around again after
			 */
			ret = 1;

		/* assuming they gave us something to send, send it */

		if (eff_buf.token_len) {
			n = lws_issue_raw(wsi, (unsigned char *)eff_buf.token,
					  eff_buf.token_len);
			if (n < 0) {
				lwsl_info("closing from POLLOUT spill\n");
				return -1;
			}
			/*
			 * Keep amount spilled small to minimize chance of this
			 */
			if (n != eff_buf.token_len) {
				lwsl_err("Unable to spill ext %d vs %s\n",
							  eff_buf.token_len, n);
				return -1;
			}
		} else
			continue;

		/* no extension has more to spill */

		if (!ret)
			continue;

		/*
		 * There's more to spill from an extension, but we just sent
		 * something... did that leave the pipe choked?
		 */

		if (!lws_send_pipe_choked(wsi))
			/* no we could add more */
			continue;

		lwsl_info("choked in POLLOUT service\n");

		/*
		 * Yes, he's choked.  Leave the POLLOUT masked on so we will
		 * come back here when he is unchoked.  Don't call the user
		 * callback to enforce ordering of spilling, he'll get called
		 * when we come back here and there's nothing more to spill.
		 */

		return 0;
	}
#ifndef LWS_NO_EXTENSIONS
	wsi->extension_data_pending = 0;
#endif
user_service:
	/* one shot */

	if (pollfd) {
		if (lws_change_pollfd(wsi, LWS_POLLOUT, 0)) {
			lwsl_info("failed at set pollfd\n");
			return 1;
		}

		lws_libev_io(wsi, LWS_EV_STOP | LWS_EV_WRITE);
	}

#ifdef LWS_USE_HTTP2
	/*
	 * we are the 'network wsi' for potentially many muxed child wsi with
	 * no network connection of their own, who have to use us for all their
	 * network actions.  So we use a round-robin scheme to share out the
	 * POLLOUT notifications to our children.
	 *
	 * But because any child could exhaust the socket's ability to take
	 * writes, we can only let one child get notified each time.
	 *
	 * In addition children may be closed / deleted / added between POLLOUT
	 * notifications, so we can't hold pointers
	 */

	if (wsi->mode != LWSCM_HTTP2_SERVING) {
		lwsl_info("%s: non http2\n", __func__);
		goto notify;
	}

	wsi->u.http2.requested_POLLOUT = 0;
	if (!wsi->u.http2.initialized) {
		lwsl_info("pollout on uninitialized http2 conn\n");
		return 0;
	}

	lwsl_info("%s: doing children\n", __func__);

	wsi2 = wsi;
	do {
		wsi2 = wsi2->u.http2.next_child_wsi;
		lwsl_info("%s: child %p\n", __func__, wsi2);
		if (!wsi2)
			continue;
		if (!wsi2->u.http2.requested_POLLOUT)
			continue;
		wsi2->u.http2.requested_POLLOUT = 0;
		if (lws_calllback_as_writeable(wsi2)) {
			lwsl_debug("Closing POLLOUT child\n");
			lws_close_free_wsi(wsi2, LWS_CLOSE_STATUS_NOSTATUS);
		}
		wsi2 = wsi;
	} while (wsi2 != NULL && !lws_send_pipe_choked(wsi));

	lwsl_info("%s: completed\n", __func__);

	return 0;
notify:
#endif
	return lws_calllback_as_writeable(wsi);
}

int
lws_service_timeout_check(struct lws *wsi, unsigned int sec)
{
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	struct lws **pwsi;

	/*
	 * if extensions want in on it (eg, we are a mux parent)
	 * give them a chance to service child timeouts
	 */
	if (lws_ext_cb_active(wsi, LWS_EXT_CB_1HZ, NULL, sec) < 0)
		return 0;

	if (!wsi->pending_timeout)
		return 0;

	/*
	 * if we went beyond the allowed time, kill the
	 * connection
	 */
	if ((time_t)sec > wsi->pending_timeout_limit) {
#if LWS_POSIX
		lwsl_notice("wsi %p: TIMEDOUT WAITING on %d (did hdr %d, ah %p, wl %d, pfd events %d)\n",
			    (void *)wsi, wsi->pending_timeout,
			    wsi->hdr_parsing_completed, wsi->u.hdr.ah,
			    pt->ah_wait_list_length,
			    pt->fds[wsi->sock].events);
#endif
		lws_pt_lock(pt);

		pwsi = &pt->ah_wait_list;
		while (*pwsi) {
			if (*pwsi == wsi)
				break;
			pwsi = &(*pwsi)->u.hdr.ah_wait_list;
		}
		lws_pt_unlock(pt);

		if (!*pwsi)
			lwsl_err("*** not on ah wait list ***\n");
		/*
		 * Since he failed a timeout, he already had a chance to do
		 * something and was unable to... that includes situations like
		 * half closed connections.  So process this "failed timeout"
		 * close as a violent death and don't try to do protocol
		 * cleanup like flush partials.
		 */
		wsi->socket_is_permanently_unusable = 1;
		lws_close_free_wsi(wsi, LWS_CLOSE_STATUS_NOSTATUS);

		return 1;
	}

	return 0;
}

int lws_rxflow_cache(struct lws *wsi, unsigned char *buf, int n, int len)
{
	/* his RX is flowcontrolled, don't send remaining now */
	if (wsi->rxflow_buffer) {
		/* rxflow while we were spilling prev rxflow */
		lwsl_info("stalling in existing rxflow buf\n");
		return 1;
	}

	/* a new rxflow, buffer it and warn caller */
	lwsl_info("new rxflow input buffer len %d\n", len - n);
	wsi->rxflow_buffer = lws_malloc(len - n);
	wsi->rxflow_len = len - n;
	wsi->rxflow_pos = 0;
	memcpy(wsi->rxflow_buffer, buf + n, len - n);

	return 0;
}

/**
 * lws_service_fd() - Service polled socket with something waiting
 * @context:	Websocket context
 * @pollfd:	The pollfd entry describing the socket fd and which events
 *		happened.
 *
 *	This function takes a pollfd that has POLLIN or POLLOUT activity and
 *	services it according to the state of the associated
 *	struct lws.
 *
 *	The one call deals with all "service" that might happen on a socket
 *	including listen accepts, http files as well as websocket protocol.
 *
 *	If a pollfd says it has something, you can just pass it to
 *	lws_service_fd() whether it is a socket handled by lws or not.
 *	If it sees it is a lws socket, the traffic will be handled and
 *	pollfd->revents will be zeroed now.
 *
 *	If the socket is foreign to lws, it leaves revents alone.  So you can
 *	see if you should service yourself by checking the pollfd revents
 *	after letting lws try to service it.
 */

LWS_VISIBLE int
lws_service_fd_tsi(struct lws_context *context, struct lws_pollfd *pollfd, int tsi)
{
	struct lws_context_per_thread *pt = &context->pt[tsi];
	lws_sockfd_type our_fd = 0, tmp_fd;
	struct lws_tokens eff_buf;
	unsigned int pending = 0;
	struct lws *wsi, *wsi1;
	char draining_flow = 0;
	int timed_out = 0;
	time_t now;
	int n, m;
	int more;

	/*
	 * you can call us with pollfd = NULL to just allow the once-per-second
	 * global timeout checks; if less than a second since the last check
	 * it returns immediately then.
	 */

	time(&now);

	/* TODO: if using libev, we should probably use timeout watchers... */
	if (context->last_timeout_check_s != now) {
		context->last_timeout_check_s = now;

		lws_plat_service_periodic(context);

		/* global timeout check once per second */

		if (pollfd)
			our_fd = pollfd->fd;

		wsi = context->pt[tsi].timeout_list;
		while (wsi) {
			/* we have to take copies, because he may be deleted */
			wsi1 = wsi->timeout_list;
			tmp_fd = wsi->sock;
			if (lws_service_timeout_check(wsi, (unsigned int)now)) {
				/* he did time out... */
				if (tmp_fd == our_fd)
					/* it was the guy we came to service! */
					timed_out = 1;
					/* he's gone, no need to mark as handled */
			}
			wsi = wsi1;
		}
#if 0
		{
			char s[300], *p = s;

			for (n = 0; n < context->count_threads; n++)
				p += sprintf(p, " %7lu (%5d), ",
					     context->pt[n].count_conns,
					     context->pt[n].fds_count);

			lwsl_notice("load: %s\n", s);
		}
#endif
	}

	/* the socket we came to service timed out, nothing to do */
	if (timed_out)
		return 0;

	/* just here for timeout management? */
	if (!pollfd)
		return 0;

	/* no, here to service a socket descriptor */
	wsi = wsi_from_fd(context, pollfd->fd);
	if (!wsi)
		/* not lws connection ... leave revents alone and return */
		return 0;

	/*
	 * so that caller can tell we handled, past here we need to
	 * zero down pollfd->revents after handling
	 */

#if LWS_POSIX

	/* handle session socket closed */

	if ((!(pollfd->revents & pollfd->events & LWS_POLLIN)) &&
	    (pollfd->revents & LWS_POLLHUP)) {
		wsi->socket_is_permanently_unusable = 1;
		lwsl_debug("Session Socket %p (fd=%d) dead\n",
						       (void *)wsi, pollfd->fd);

		goto close_and_handled;
	}

#ifdef _WIN32
	if (pollfd->revents & LWS_POLLOUT)
		wsi->sock_send_blocking = FALSE;
#endif

#endif

	/* okay, what we came here to do... */

	switch (wsi->mode) {
	case LWSCM_HTTP_SERVING:
	case LWSCM_HTTP_SERVING_ACCEPTED:
	case LWSCM_SERVER_LISTENER:
	case LWSCM_SSL_ACK_PENDING:
		n = lws_server_socket_service(context, wsi, pollfd);
		if (n) /* closed by above */
			return 1;
		pending = lws_ssl_pending(wsi);
		if (pending)
			goto handle_pending;
		goto handled;

	case LWSCM_WS_SERVING:
	case LWSCM_WS_CLIENT:
	case LWSCM_HTTP2_SERVING:

		/* 1: something requested a callback when it was OK to write */

		if ((pollfd->revents & LWS_POLLOUT) &&
		    (wsi->state == LWSS_ESTABLISHED ||
		     wsi->state == LWSS_HTTP2_ESTABLISHED ||
		     wsi->state == LWSS_HTTP2_ESTABLISHED_PRE_SETTINGS ||
		     wsi->state == LWSS_RETURNED_CLOSE_ALREADY ||
		     wsi->state == LWSS_FLUSHING_STORED_SEND_BEFORE_CLOSE) &&
		    lws_handle_POLLOUT_event(wsi, pollfd)) {
			if (wsi->state == LWSS_RETURNED_CLOSE_ALREADY)
				wsi->state = LWSS_FLUSHING_STORED_SEND_BEFORE_CLOSE;
			lwsl_info("lws_service_fd: closing\n");
			goto close_and_handled;
		}
#if 1
		if (wsi->state == LWSS_RETURNED_CLOSE_ALREADY ||
		    wsi->state == LWSS_AWAITING_CLOSE_ACK) {
			/*
			 * we stopped caring about anything except control
			 * packets.  Force flow control off, defeat tx
			 * draining.
			 */
			lws_rx_flow_control(wsi, 1);
			wsi->u.ws.tx_draining_ext = 0;
		}
#endif
		if (wsi->u.ws.tx_draining_ext) {
			/* we cannot deal with new RX until the TX ext
			 * path has been drained.  It's because new
			 * rx will, eg, crap on the wsi rx buf that
			 * may be needed to retain state.
			 *
			 * TX ext drain path MUST go through event loop
			 * to avoid blocking.
			 */
			break;
		}

		if (!(wsi->rxflow_change_to & LWS_RXFLOW_ALLOW))
			/* We cannot deal with any kind of new RX
			 * because we are RX-flowcontrolled.
			 */
			break;

		/* 2: RX Extension needs to be drained
		 */

		if (wsi->state == LWSS_ESTABLISHED &&
		    wsi->u.ws.rx_draining_ext) {

			lwsl_ext("%s: RX EXT DRAINING: Service\n", __func__);
#ifndef LWS_NO_CLIENT
			if (wsi->mode == LWSCM_WS_CLIENT) {
				n = lws_client_rx_sm(wsi, 0);
				if (n < 0)
					/* we closed wsi */
					n = 0;
			} else
#endif
				n = lws_rx_sm(wsi, 0);

			goto handled;
		}

		if (wsi->u.ws.rx_draining_ext)
			/*
			 * We have RX EXT content to drain, but can't do it
			 * right now.  That means we cannot do anything lower
			 * priority either.
			 */
			break;

		/* 3: RX Flowcontrol buffer needs to be drained
		 */

		if (wsi->rxflow_buffer) {
			lwsl_info("draining rxflow (len %d)\n",
				wsi->rxflow_len - wsi->rxflow_pos
			);
			/* well, drain it */
			eff_buf.token = (char *)wsi->rxflow_buffer +
						wsi->rxflow_pos;
			eff_buf.token_len = wsi->rxflow_len - wsi->rxflow_pos;
			draining_flow = 1;
			goto drain;
		}

		/* 4: any incoming data ready?
		 * notice if rx flow going off raced poll(), rx flow wins
		 */
		if (!(pollfd->revents & pollfd->events & LWS_POLLIN))
			break;
read:
		eff_buf.token_len = lws_ssl_capable_read(wsi, pt->serv_buf,
					pending ? pending : LWS_MAX_SOCKET_IO_BUF);
		switch (eff_buf.token_len) {
		case 0:
			lwsl_info("service_fd: closing due to 0 length read\n");
			goto close_and_handled;
		case LWS_SSL_CAPABLE_MORE_SERVICE:
			lwsl_info("SSL Capable more service\n");
			n = 0;
			goto handled;
		case LWS_SSL_CAPABLE_ERROR:
			lwsl_info("Closing when error\n");
			goto close_and_handled;
		}

		/*
		 * give any active extensions a chance to munge the buffer
		 * before parse.  We pass in a pointer to an lws_tokens struct
		 * prepared with the default buffer and content length that's in
		 * there.  Rather than rewrite the default buffer, extensions
		 * that expect to grow the buffer can adapt .token to
		 * point to their own per-connection buffer in the extension
		 * user allocation.  By default with no extensions or no
		 * extension callback handling, just the normal input buffer is
		 * used then so it is efficient.
		 */

		eff_buf.token = (char *)pt->serv_buf;
drain:

		do {
			more = 0;

			m = lws_ext_cb_active(wsi,
				LWS_EXT_CB_PACKET_RX_PREPARSE, &eff_buf, 0);
			if (m < 0)
				goto close_and_handled;
			if (m)
				more = 1;

			/* service incoming data */

			if (eff_buf.token_len) {
				/*
				 * if draining from rxflow buffer, not
				 * critical to track what was used since at the
				 * use it bumps wsi->rxflow_pos.  If we come
				 * around again it will pick up from where it
				 * left off.
				 */
				n = lws_read(wsi, (unsigned char *)eff_buf.token,
					     eff_buf.token_len);
				if (n < 0) {
					/* we closed wsi */
					n = 0;
					goto handled;
				}
			}

			eff_buf.token = NULL;
			eff_buf.token_len = 0;
		} while (more);

		pending = lws_ssl_pending(wsi);
		if (pending) {
handle_pending:
			pending = pending > LWS_MAX_SOCKET_IO_BUF ?
					LWS_MAX_SOCKET_IO_BUF : pending;
			goto read;
		}

		if (draining_flow && wsi->rxflow_buffer &&
				 wsi->rxflow_pos == wsi->rxflow_len) {
			lwsl_info("flow buffer: drained\n");
			lws_free_set_NULL(wsi->rxflow_buffer);
			/* having drained the rxflow buffer, can rearm POLLIN */
#ifdef LWS_NO_SERVER
			n =
#endif
			_lws_rx_flow_control(wsi);
			/* n ignored, needed for NO_SERVER case */
		}

		break;

	default:
#ifdef LWS_NO_CLIENT
		break;
#else
		n = lws_client_socket_service(context, wsi, pollfd);
		if (n)
			return 1;
		goto handled;
#endif
	}

	n = 0;
	goto handled;

close_and_handled:
	lwsl_debug("Close and handled\n");
	lws_close_free_wsi(wsi, LWS_CLOSE_STATUS_NOSTATUS);
	/*
	 * pollfd may point to something else after the close
	 * due to pollfd swapping scheme on delete on some platforms
	 * we can't clear revents now because it'd be the wrong guy's revents
	 */
	return 1;

handled:
	pollfd->revents = 0;
	return n;
}

LWS_VISIBLE int
lws_service_fd(struct lws_context *context, struct lws_pollfd *pollfd)
{
	return lws_service_fd_tsi(context, pollfd, 0);
}

/**
 * lws_service() - Service any pending websocket activity
 * @context:	Websocket context
 * @timeout_ms:	Timeout for poll; 0 means return immediately if nothing needed
 *		service otherwise block and service immediately, returning
 *		after the timeout if nothing needed service.
 *
 *	This function deals with any pending websocket traffic, for three
 *	kinds of event.  It handles these events on both server and client
 *	types of connection the same.
 *
 *	1) Accept new connections to our context's server
 *
 *	2) Call the receive callback for incoming frame data received by
 *	    server or client connections.
 *
 *	You need to call this service function periodically to all the above
 *	functions to happen; if your application is single-threaded you can
 *	just call it in your main event loop.
 *
 *	Alternatively you can fork a new process that asynchronously handles
 *	calling this service in a loop.  In that case you are happy if this
 *	call blocks your thread until it needs to take care of something and
 *	would call it with a large nonzero timeout.  Your loop then takes no
 *	CPU while there is nothing happening.
 *
 *	If you are calling it in a single-threaded app, you don't want it to
 *	wait around blocking other things in your loop from happening, so you
 *	would call it with a timeout_ms of 0, so it returns immediately if
 *	nothing is pending, or as soon as it services whatever was pending.
 */

LWS_VISIBLE int
lws_service(struct lws_context *context, int timeout_ms)
{
	return lws_plat_service(context, timeout_ms);
}

LWS_VISIBLE int
lws_service_tsi(struct lws_context *context, int timeout_ms, int tsi)
{
	return lws_plat_service_tsi(context, timeout_ms, tsi);
}

