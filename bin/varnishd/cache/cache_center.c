/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file contains the central state machine for pushing requests.
 *
 * We cannot just use direct calls because it is possible to kick a
 * request back to the lookup stage (usually after a rewrite).  The
 * state engine also allows us to break the processing up into some
 * logical chunks which improves readability a little bit.
 *
 * Since the states are rather nasty in detail, I have decided to embedd
 * a dot(1) graph in the source code comments.  So to see the big picture,
 * extract the DOT lines and run though dot(1), for instance with the
 * command:
 *	sed -n '/^DOT/s///p' cache/cache_center.c | dot -Tps > /tmp/_.ps
 */

/*
DOT digraph vcl_center {
xDOT	page="8.2,11.5"
DOT	size="7.2,10.5"
DOT	margin="0.5"
DOT	center="1"
DOT acceptor [
DOT	shape=hexagon
DOT	label="Request received"
DOT ]
DOT ERROR [shape=plaintext]
DOT RESTART [shape=plaintext]
DOT acceptor -> first [style=bold,color=green]
 */

#include "config.h"

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>

#include "cache.h"

#include "common/heritage.h"

#include "hash/hash_slinger.h"
#include "vcl.h"
#include "vcli_priv.h"
#include "vsha256.h"
#include "vtcp.h"
#include "vtim.h"

#ifndef HAVE_SRANDOMDEV
#include "compat/srandomdev.h"
#endif

static unsigned xids;

/*--------------------------------------------------------------------
 * WAIT
 * Wait (briefly) until we have a full request in our htc.
 *
DOT subgraph xcluster_wait {
DOT	wait [
DOT		shape=box
DOT		label="cnt_wait:\nwait for\nrequest"
DOT	]
DOT	herding [shape=hexagon]
DOT	wait -> start [label="got req"]
DOT	wait -> "SES_Delete()" [label="errors"]
DOT	wait -> herding [label="timeout_linger"]
DOT	herding -> wait [label="fd read_ready"]
DOT }
 */

static int
cnt_wait(struct sess *sp, struct worker *wrk, struct req *req)
{
	int i, j, tmo;
	struct pollfd pfd[1];
	double now, when;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	if (req == NULL) {
		SES_GetReq(sp);
		req = sp->req;
		CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
		HTC_Init(req->htc, req->ws, sp->fd, sp->vsl_id,
		    cache_param->http_req_size,
		    cache_param->http_req_hdr_len);
	}

	AZ(req->vcl);
	AZ(req->obj);
	AZ(req->esi_level);
	assert(req->xid == 0);
	req->t_resp = NAN;

	assert(!isnan(sp->t_req));
	tmo = (int)(1e3 * cache_param->timeout_linger);
	while (1) {
		pfd[0].fd = sp->fd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		j = poll(pfd, 1, tmo);
		assert(j >= 0);
		now = VTIM_real();
		if (j != 0)
			i = HTC_Rx(req->htc);
		else
			i = HTC_Complete(req->htc);
		if (i == 1) {
			/* Got it, run with it */
			sp->t_req = now;
			break;
		}
		if (i == -1) {
			SES_Charge(sp);
			SES_Delete(sp, "EOF", now);
			return (1);
		}
		if (i == -2) {
			SES_Charge(sp);
			SES_Delete(sp, "overflow", now);
			return (1);
		}
		if (i == -3) {
			/* Nothing but whitespace */
			when = sp->t_idle + cache_param->timeout_idle;
			if (when < now) {
				SES_Charge(sp);
				SES_Delete(sp, "timeout", now);
				return (1);
			}
			when = sp->t_idle + cache_param->timeout_linger;
			tmo = (int)(1e3 * (when - now));
			if (when < now || tmo == 0) {
				sp->t_req = NAN;
				wrk->stats.sess_herd++;
				SES_Charge(sp);
				SES_ReleaseReq(sp);
				WAIT_Enter(sp);
				return (1);
			}
		} else {
			/* Working on it */
			when = sp->t_req + cache_param->timeout_req;
			tmo = (int)(1e3 * (when - now));
			if (when < now || tmo == 0) {
				SES_Charge(sp);
				SES_Delete(sp, "req timeout", now);
				return (1);
			}
		}
	}
	sp->step = STP_START;
	return (0);
}

/*--------------------------------------------------------------------
 * We have a refcounted object on the session, now deliver it.
 *
DOT subgraph xcluster_prepresp {
DOT	prepresp [
DOT		shape=ellipse
DOT		label="Filter obj.->resp."
DOT	]
DOT	vcl_deliver [
DOT		shape=record
DOT		label="vcl_deliver()|resp."
DOT	]
DOT	prepresp -> vcl_deliver [style=bold,color=green]
DOT	prepresp -> vcl_deliver [style=bold,color=cyan]
DOT	prepresp -> vcl_deliver [style=bold,color=red]
DOT	prepresp -> vcl_deliver [style=bold,color=blue,]
DOT	vcl_deliver -> deliver [style=bold,color=green,label=deliver]
DOT	vcl_deliver -> deliver [style=bold,color=red]
DOT	vcl_deliver -> deliver [style=bold,color=blue]
DOT     vcl_deliver -> errdeliver [label="error"]
DOT     errdeliver [label="ERROR",shape=plaintext]
DOT     vcl_deliver -> rstdeliver [label="restart",color=purple]
DOT     rstdeliver [label="RESTART",shape=plaintext]
DOT     vcl_deliver -> streambody [style=bold,color=cyan,label="deliver"]
DOT }
 *
 */

static int
cnt_prepresp(struct sess *sp, struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	CHECK_OBJ_NOTNULL(req->obj, OBJECT_MAGIC);
	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);

	if (wrk->busyobj != NULL) {
		CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
		AN(wrk->busyobj->do_stream);
		AssertObjCorePassOrBusy(req->obj->objcore);
	}

	wrk->res_mode = 0;

	if (wrk->busyobj == NULL)
		wrk->res_mode |= RES_LEN;

	if (wrk->busyobj != NULL &&
	    (wrk->busyobj->h_content_length != NULL ||
	    !wrk->busyobj->do_stream) &&
	    !wrk->busyobj->do_gzip && !wrk->busyobj->do_gunzip)
		wrk->res_mode |= RES_LEN;

	if (!req->disable_esi && req->obj->esidata != NULL) {
		/* In ESI mode, we don't know the aggregate length */
		wrk->res_mode &= ~RES_LEN;
		wrk->res_mode |= RES_ESI;
	}

	if (req->esi_level > 0) {
		wrk->res_mode &= ~RES_LEN;
		wrk->res_mode |= RES_ESI_CHILD;
	}

	if (cache_param->http_gzip_support && req->obj->gziped &&
	    !RFC2616_Req_Gzip(sp)) {
		/*
		 * We don't know what it uncompresses to
		 * XXX: we could cache that
		 */
		wrk->res_mode &= ~RES_LEN;
		wrk->res_mode |= RES_GUNZIP;
	}

	if (!(wrk->res_mode & (RES_LEN|RES_CHUNKED|RES_EOF))) {
		if (req->obj->len == 0 &&
		    (wrk->busyobj == NULL || !wrk->busyobj->do_stream))
			/*
			 * If the object is empty, neither ESI nor GUNZIP
			 * can make it any different size
			 */
			wrk->res_mode |= RES_LEN;
		else if (!req->wantbody) {
			/* Nothing */
		} else if (req->http->protover >= 11) {
			wrk->res_mode |= RES_CHUNKED;
		} else {
			wrk->res_mode |= RES_EOF;
			req->doclose = "EOF mode";
		}
	}

	req->t_resp = W_TIM_real(wrk);
	if (req->obj->objcore != NULL) {
		if ((req->t_resp - req->obj->last_lru) >
		    cache_param->lru_timeout &&
		    EXP_Touch(req->obj->objcore))
			req->obj->last_lru = req->t_resp;
		req->obj->last_use = req->t_resp;	/* XXX: locking ? */
	}
	http_Setup(req->resp, req->ws);
	RES_BuildHttp(sp);
	VCL_deliver_method(sp);
	switch (req->handling) {
	case VCL_RET_DELIVER:
		break;
	case VCL_RET_RESTART:
		if (req->restarts >= cache_param->max_restarts)
			break;
		if (wrk->busyobj != NULL) {
			AN(wrk->busyobj->do_stream);
			VDI_CloseFd(wrk, &wrk->busyobj->vbc);
			HSH_Drop(wrk);
			VBO_DerefBusyObj(wrk, &wrk->busyobj);
		} else {
			(void)HSH_Deref(wrk, NULL, &req->obj);
		}
		AZ(req->obj);
		req->restarts++;
		req->director = NULL;
		http_Setup(req->resp, NULL);
		sp->step = STP_RECV;
		return (0);
	default:
		WRONG("Illegal action in vcl_deliver{}");
	}
	if (wrk->busyobj != NULL && wrk->busyobj->do_stream) {
		AssertObjCorePassOrBusy(req->obj->objcore);
		sp->step = STP_STREAMBODY;
	} else {
		sp->step = STP_DELIVER;
	}
	return (0);
}

/*--------------------------------------------------------------------
 * Deliver an already stored object
 *
DOT subgraph xcluster_deliver {
DOT	deliver [
DOT		shape=ellipse
DOT		label="Send body"
DOT	]
DOT }
DOT deliver -> DONE [style=bold,color=green]
DOT deliver -> DONE [style=bold,color=red]
DOT deliver -> DONE [style=bold,color=blue]
 *
 */

static int
cnt_deliver(struct sess *sp, struct worker *wrk, struct req *req)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	AZ(sp->wrk->busyobj);
	req->director = NULL;
	req->restarts = 0;

	RES_WriteObj(sp);

	assert(WRW_IsReleased(wrk));
	assert(wrk->wrw.ciov == wrk->wrw.siov);
	(void)HSH_Deref(wrk, NULL, &req->obj);
	http_Setup(req->resp, NULL);
	sp->step = STP_DONE;
	return (0);
}

/*--------------------------------------------------------------------
 * This is the final state, figure out if we should close or recycle
 * the client connection
 *
DOT	DONE [
DOT		shape=hexagon
DOT		label="cnt_done:\nRequest completed"
DOT	]
DOT	ESI_RESP [ shape=hexagon ]
DOT	DONE -> start [label="full pipeline"]
DOT	DONE -> wait
DOT	DONE -> ESI_RESP
 */

static int
cnt_done(struct sess *sp, struct worker *wrk, struct req *req)
{
	double dh, dp, da;
	int i;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_ORNULL(req->vcl, VCL_CONF_MAGIC);

	AZ(req->obj);
	AZ(wrk->busyobj);
	req->director = NULL;
	req->restarts = 0;

	wrk->busyobj = NULL;

	SES_Charge(sp);

	/* If we did an ESI include, don't mess up our state */
	if (req->esi_level > 0)
		return (1);

	if (req->vcl != NULL) {
		if (wrk->vcl != NULL)
			VCL_Rel(&wrk->vcl);
		wrk->vcl = req->vcl;
		req->vcl = NULL;
	}


	sp->t_idle = W_TIM_real(wrk);
	if (req->xid == 0) {
		req->t_resp = sp->t_idle;
	} else {
		dp = req->t_resp - sp->t_req;
		da = sp->t_idle - req->t_resp;
		dh = sp->t_req - sp->t_open;
		/* XXX: Add StatReq == StatSess */
		/* XXX: Workaround for pipe */
		if (sp->fd >= 0) {
			WSP(sp, SLT_Length, "%ju",
			    (uintmax_t)req->req_bodybytes);
		}
		WSP(sp, SLT_ReqEnd, "%u %.9f %.9f %.9f %.9f %.9f",
		    req->xid, sp->t_req, sp->t_idle, dh, dp, da);
	}
	req->xid = 0;
	WSL_Flush(wrk, 0);

	sp->t_req = NAN;
	req->t_resp = NAN;

	req->req_bodybytes = 0;

	req->hash_always_miss = 0;
	req->hash_ignore_busy = 0;

	if (sp->fd >= 0 && req->doclose != NULL) {
		/*
		 * This is an orderly close of the connection; ditch nolinger
		 * before we close, to get queued data transmitted.
		 */
		// XXX: not yet (void)VTCP_linger(sp->fd, 0);
		SES_Close(sp, req->doclose);
	}

	if (sp->fd < 0) {
		wrk->stats.sess_closed++;
		SES_Delete(sp, NULL, NAN);
		return (1);
	}

	if (wrk->stats.client_req >= cache_param->wthread_stats_rate)
		WRK_SumStat(wrk);
	/* Reset the workspace to the session-watermark */
	WS_Reset(req->ws, NULL);
	WS_Reset(wrk->ws, NULL);

	i = HTC_Reinit(req->htc);
	if (i == 1) {
		wrk->stats.sess_pipeline++;
		sp->t_req = sp->t_idle;
		sp->step = STP_START;
		return (0);
	}
	if (Tlen(req->htc->rxbuf))
		wrk->stats.sess_readahead++;
	sp->step = STP_WAIT;
	sp->t_req = sp->t_idle;
	return (0);
}

/*--------------------------------------------------------------------
 * Emit an error
 *
DOT subgraph xcluster_error {
DOT	vcl_error [
DOT		shape=record
DOT		label="vcl_error()|resp."
DOT	]
DOT	ERROR -> vcl_error
DOT	vcl_error-> prepresp [label=deliver]
DOT }
DOT vcl_error-> rsterr [label="restart",color=purple]
DOT rsterr [label="RESTART",shape=plaintext]
 */

static int
cnt_error(struct sess *sp, struct worker *wrk, struct req *req)
{
	struct http *h;
	char date[40];

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	if (req->obj == NULL) {
		HSH_Prealloc(sp);
		AZ(wrk->busyobj);
		wrk->busyobj = VBO_GetBusyObj(wrk);
		req->obj = STV_NewObject(wrk, NULL, cache_param->http_resp_size,
		     (uint16_t)cache_param->http_max_hdr);
		if (req->obj == NULL)
			req->obj = STV_NewObject(wrk, TRANSIENT_STORAGE,
			    cache_param->http_resp_size,
			    (uint16_t)cache_param->http_max_hdr);
		if (req->obj == NULL) {
			req->doclose = "Out of objects";
			req->director = NULL;
			http_Setup(wrk->busyobj->beresp, NULL);
			http_Setup(wrk->busyobj->bereq, NULL);
			sp->step = STP_DONE;
			return(0);
		}
		AN(req->obj);
		req->obj->xid = req->xid;
		req->obj->exp.entered = sp->t_req;
	} else {
		CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
		/* XXX: Null the headers ? */
	}
	CHECK_OBJ_NOTNULL(req->obj, OBJECT_MAGIC);
	h = req->obj->http;

	if (req->err_code < 100 || req->err_code > 999)
		req->err_code = 501;

	http_PutProtocol(wrk, sp->vsl_id, h, "HTTP/1.1");
	http_PutStatus(h, req->err_code);
	VTIM_format(W_TIM_real(wrk), date);
	http_PrintfHeader(wrk, sp->vsl_id, h, "Date: %s", date);
	http_SetHeader(wrk, sp->vsl_id, h, "Server: Varnish");

	if (req->err_reason != NULL)
		http_PutResponse(wrk, sp->vsl_id, h, req->err_reason);
	else
		http_PutResponse(wrk, sp->vsl_id, h,
		    http_StatusMessage(req->err_code));
	VCL_error_method(sp);

	if (req->handling == VCL_RET_RESTART &&
	    req->restarts <  cache_param->max_restarts) {
		HSH_Drop(wrk);
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		req->director = NULL;
		req->restarts++;
		sp->step = STP_RECV;
		return (0);
	} else if (req->handling == VCL_RET_RESTART)
		req->handling = VCL_RET_DELIVER;


	/* We always close when we take this path */
	req->doclose = "error";
	req->wantbody = 1;

	assert(req->handling == VCL_RET_DELIVER);
	req->err_code = 0;
	req->err_reason = NULL;
	http_Setup(wrk->busyobj->bereq, NULL);
	VBO_DerefBusyObj(wrk, &wrk->busyobj);
	sp->step = STP_PREPRESP;
	return (0);
}

/*--------------------------------------------------------------------
 * Fetch response headers from the backend
 *
DOT subgraph xcluster_fetch {
DOT	fetch [
DOT		shape=ellipse
DOT		label="fetch hdr\nfrom backend\n(find obj.ttl)"
DOT	]
DOT	vcl_fetch [
DOT		shape=record
DOT		label="vcl_fetch()|req.\nbereq.\nberesp."
DOT	]
DOT	fetch -> vcl_fetch [style=bold,color=blue]
DOT	fetch -> vcl_fetch [style=bold,color=red]
DOT	fetch_pass [
DOT		shape=ellipse
DOT		label="obj.f.pass=true"
DOT	]
DOT	vcl_fetch -> fetch_pass [label="hit_for_pass",style=bold,color=red]
DOT }
DOT fetch_pass -> fetchbody [style=bold,color=red]
DOT vcl_fetch -> fetchbody [label="deliver",style=bold,color=blue]
DOT vcl_fetch -> rstfetch [label="restart",color=purple]
DOT rstfetch [label="RESTART",shape=plaintext]
DOT fetch -> errfetch
DOT vcl_fetch -> errfetch [label="error"]
DOT errfetch [label="ERROR",shape=plaintext]
 */

static int
cnt_fetch(struct sess *sp, struct worker *wrk, struct req *req)
{
	int i, need_host_hdr;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);

	AN(req->director);
	AZ(wrk->busyobj->vbc);
	AZ(wrk->busyobj->should_close);
	AZ(req->storage_hint);

	http_Setup(wrk->busyobj->beresp, wrk->ws);

	need_host_hdr = !http_GetHdr(wrk->busyobj->bereq, H_Host, NULL);

	i = FetchHdr(sp, need_host_hdr);
	/*
	 * If we recycle a backend connection, there is a finite chance
	 * that the backend closed it before we get a request to it.
	 * Do a single retry in that case.
	 */
	if (i == 1) {
		VSC_C_main->backend_retry++;
		i = FetchHdr(sp, need_host_hdr);
	}

	if (i) {
		req->handling = VCL_RET_ERROR;
		req->err_code = 503;
	} else {
		/*
		 * These two headers can be spread over multiple actual headers
		 * and we rely on their content outside of VCL, so collect them
		 * into one line here.
		 */
		http_CollectHdr(wrk->busyobj->beresp, H_Cache_Control);
		http_CollectHdr(wrk->busyobj->beresp, H_Vary);

		/*
		 * Figure out how the fetch is supposed to happen, before the
		 * headers are adultered by VCL
		 * NB: Also sets other wrk variables
		 */
		wrk->busyobj->body_status = RFC2616_Body(sp);

		req->err_code = http_GetStatus(wrk->busyobj->beresp);

		/*
		 * What does RFC2616 think about TTL ?
		 */
		EXP_Clr(&wrk->busyobj->exp);
		wrk->busyobj->exp.entered = W_TIM_real(wrk);
		RFC2616_Ttl(sp);

		/* pass from vclrecv{} has negative TTL */
		if (req->objcore == NULL)
			wrk->busyobj->exp.ttl = -1.;

		AZ(wrk->busyobj->do_esi);

		VCL_fetch_method(sp);

		switch (req->handling) {
		case VCL_RET_HIT_FOR_PASS:
			if (req->objcore != NULL)
				req->objcore->flags |= OC_F_PASS;
			sp->step = STP_FETCHBODY;
			return (0);
		case VCL_RET_DELIVER:
			AssertObjCorePassOrBusy(req->objcore);
			sp->step = STP_FETCHBODY;
			return (0);
		default:
			break;
		}

		/* We are not going to fetch the body, Close the connection */
		VDI_CloseFd(wrk, &wrk->busyobj->vbc);
	}

	/* Clean up partial fetch */
	AZ(wrk->busyobj->vbc);

	if (req->objcore != NULL) {
		CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);
		AZ(HSH_Deref(wrk, req->objcore, NULL));
		req->objcore = NULL;
	}
	VBO_DerefBusyObj(wrk, &wrk->busyobj);
	req->director = NULL;
	req->storage_hint = NULL;

	switch (req->handling) {
	case VCL_RET_RESTART:
		req->restarts++;
		sp->step = STP_RECV;
		return (0);
	case VCL_RET_ERROR:
		sp->step = STP_ERROR;
		return (0);
	default:
		WRONG("Illegal action in vcl_fetch{}");
	}
}

/*--------------------------------------------------------------------
 * Fetch response body from the backend
 *
DOT subgraph xcluster_body {
DOT	fetchbody [
DOT		shape=diamond
DOT		label="stream ?"
DOT	]
DOT	fetchbody2 [
DOT		shape=ellipse
DOT		label="fetch body\nfrom backend\n"
DOT	]
DOT }
DOT fetchbody -> fetchbody2 [label=no,style=bold,color=red]
DOT fetchbody -> fetchbody2 [style=bold,color=blue]
DOT fetchbody -> prepresp [label=yes,style=bold,color=cyan]
DOT fetchbody2 -> prepresp [style=bold,color=red]
DOT fetchbody2 -> prepresp [style=bold,color=blue]
 */


static int
cnt_fetchbody(struct sess *sp, struct worker *wrk, struct req *req)
{
	int i;
	struct http *hp, *hp2;
	char *b;
	uint16_t nhttp;
	unsigned l;
	struct vsb *vary = NULL;
	int varyl = 0, pass;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);

	assert(req->handling == VCL_RET_HIT_FOR_PASS ||
	    req->handling == VCL_RET_DELIVER);

	if (req->objcore == NULL) {
		/* This is a pass from vcl_recv */
		pass = 1;
		/* VCL may have fiddled this, but that doesn't help */
		wrk->busyobj->exp.ttl = -1.;
	} else if (req->handling == VCL_RET_HIT_FOR_PASS) {
		/* pass from vcl_fetch{} -> hit-for-pass */
		/* XXX: the bereq was not filtered pass... */
		pass = 1;
	} else {
		/* regular object */
		pass = 0;
	}

	/*
	 * The VCL variables beresp.do_g[un]zip tells us how we want the
	 * object processed before it is stored.
	 *
	 * The backend Content-Encoding header tells us what we are going
	 * to receive, which we classify in the following three classes:
	 *
	 *	"Content-Encoding: gzip"	--> object is gzip'ed.
	 *	no Content-Encoding		--> object is not gzip'ed.
	 *	anything else			--> do nothing wrt gzip
	 *
	 */

	/* We do nothing unless the param is set */
	if (!cache_param->http_gzip_support)
		wrk->busyobj->do_gzip = wrk->busyobj->do_gunzip = 0;

	wrk->busyobj->is_gzip =
	    http_HdrIs(wrk->busyobj->beresp, H_Content_Encoding, "gzip");

	wrk->busyobj->is_gunzip =
	    !http_GetHdr(wrk->busyobj->beresp, H_Content_Encoding, NULL);

	/* It can't be both */
	assert(wrk->busyobj->is_gzip == 0 || wrk->busyobj->is_gunzip == 0);

	/* We won't gunzip unless it is gzip'ed */
	if (wrk->busyobj->do_gunzip && !wrk->busyobj->is_gzip)
		wrk->busyobj->do_gunzip = 0;

	/* If we do gunzip, remove the C-E header */
	if (wrk->busyobj->do_gunzip)
		http_Unset(wrk->busyobj->beresp, H_Content_Encoding);

	/* We wont gzip unless it is ungziped */
	if (wrk->busyobj->do_gzip && !wrk->busyobj->is_gunzip)
		wrk->busyobj->do_gzip = 0;

	/* If we do gzip, add the C-E header */
	if (wrk->busyobj->do_gzip)
		http_SetHeader(wrk, sp->vsl_id, wrk->busyobj->beresp,
		    "Content-Encoding: gzip");

	/* But we can't do both at the same time */
	assert(wrk->busyobj->do_gzip == 0 || wrk->busyobj->do_gunzip == 0);

	/* ESI takes precedence and handles gzip/gunzip itself */
	if (wrk->busyobj->do_esi)
		wrk->busyobj->vfp = &vfp_esi;
	else if (wrk->busyobj->do_gunzip)
		wrk->busyobj->vfp = &vfp_gunzip;
	else if (wrk->busyobj->do_gzip)
		wrk->busyobj->vfp = &vfp_gzip;
	else if (wrk->busyobj->is_gzip)
		wrk->busyobj->vfp = &vfp_testgzip;

	if (wrk->busyobj->do_esi || req->esi_level > 0)
		wrk->busyobj->do_stream = 0;
	if (!req->wantbody)
		wrk->busyobj->do_stream = 0;

	l = http_EstimateWS(wrk->busyobj->beresp,
	    pass ? HTTPH_R_PASS : HTTPH_A_INS, &nhttp);

	/* Create Vary instructions */
	if (req->objcore != NULL) {
		CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);
		vary = VRY_Create(sp, wrk->busyobj->beresp);
		if (vary != NULL) {
			varyl = VSB_len(vary);
			assert(varyl > 0);
			l += varyl;
		}
	}

	/*
	 * Space for producing a Content-Length: header including padding
	 * A billion gigabytes is enough for anybody.
	 */
	l += strlen("Content-Length: XxxXxxXxxXxxXxxXxx") + sizeof(void *);

	if (wrk->busyobj->exp.ttl < cache_param->shortlived ||
	    req->objcore == NULL)
		req->storage_hint = TRANSIENT_STORAGE;

	req->obj = STV_NewObject(wrk, req->storage_hint, l, nhttp);
	if (req->obj == NULL) {
		/*
		 * Try to salvage the transaction by allocating a
		 * shortlived object on Transient storage.
		 */
		req->obj = STV_NewObject(wrk, TRANSIENT_STORAGE, l, nhttp);
		if (wrk->busyobj->exp.ttl > cache_param->shortlived)
			wrk->busyobj->exp.ttl = cache_param->shortlived;
		wrk->busyobj->exp.grace = 0.0;
		wrk->busyobj->exp.keep = 0.0;
	}
	if (req->obj == NULL) {
		req->err_code = 503;
		sp->step = STP_ERROR;
		VDI_CloseFd(wrk, &wrk->busyobj->vbc);
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		return (0);
	}
	CHECK_OBJ_NOTNULL(req->obj, OBJECT_MAGIC);

	req->storage_hint = NULL;

	if (wrk->busyobj->do_gzip ||
	    (wrk->busyobj->is_gzip && !wrk->busyobj->do_gunzip))
		req->obj->gziped = 1;

	if (vary != NULL) {
		req->obj->vary = (void *)WS_Alloc(req->obj->http->ws, varyl);
		AN(req->obj->vary);
		memcpy(req->obj->vary, VSB_data(vary), varyl);
		VRY_Validate(req->obj->vary);
		VSB_delete(vary);
	}

	req->obj->xid = req->xid;
	req->obj->response = req->err_code;
	WS_Assert(req->obj->ws_o);

	/* Filter into object */
	hp = wrk->busyobj->beresp;
	hp2 = req->obj->http;

	hp2->logtag = HTTP_Obj;
	http_FilterResp(sp, hp, hp2, pass ? HTTPH_R_PASS : HTTPH_A_INS);
	http_CopyHome(wrk, sp->vsl_id, hp2);

	if (http_GetHdr(hp, H_Last_Modified, &b))
		req->obj->last_modified = VTIM_parse(b);
	else
		req->obj->last_modified = floor(wrk->busyobj->exp.entered);

	assert(WRW_IsReleased(wrk));

	/*
	 * If we can deliver a 304 reply, we don't bother streaming.
	 * Notice that vcl_deliver{} could still nuke the headers
	 * that allow the 304, in which case we return 200 non-stream.
	 */
	if (req->obj->response == 200 &&
	    req->http->conds &&
	    RFC2616_Do_Cond(sp))
		wrk->busyobj->do_stream = 0;

	AssertObjCorePassOrBusy(req->obj->objcore);

	if (wrk->busyobj->do_stream) {
		sp->step = STP_PREPRESP;
		return (0);
	}

	/* Use unmodified headers*/
	i = FetchBody(wrk, req->obj);

	http_Setup(wrk->busyobj->bereq, NULL);
	http_Setup(wrk->busyobj->beresp, NULL);
	wrk->busyobj->vfp = NULL;
	assert(WRW_IsReleased(wrk));
	AZ(wrk->busyobj->vbc);
	AN(req->director);

	if (i) {
		HSH_Drop(wrk);
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		AZ(req->obj);
		req->err_code = 503;
		sp->step = STP_ERROR;
		return (0);
	}

	if (req->obj->objcore != NULL) {
		EXP_Insert(req->obj);
		AN(req->obj->objcore);
		AN(req->obj->objcore->ban);
		HSH_Unbusy(wrk, 1);
	}
	VBO_DerefBusyObj(wrk, &wrk->busyobj);
	wrk->acct_tmp.fetch++;
	sp->step = STP_PREPRESP;
	return (0);
}

/*--------------------------------------------------------------------
 * Stream the body as we fetch it
DOT subgraph xstreambody {
DOT	streambody [
DOT		shape=ellipse
DOT		label="streaming\nfetch/deliver"
DOT	]
DOT }
DOT streambody -> DONE [style=bold,color=cyan]
 */

static int
cnt_streambody(struct sess *sp, struct worker *wrk, struct req *req)
{
	int i;
	struct stream_ctx sctx;
	uint8_t obuf[sp->wrk->res_mode & RES_GUNZIP ?
	    cache_param->gzip_stack_buffer : 1];

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
	memset(&sctx, 0, sizeof sctx);
	sctx.magic = STREAM_CTX_MAGIC;
	AZ(wrk->sctx);
	wrk->sctx = &sctx;

	if (wrk->res_mode & RES_GUNZIP) {
		sctx.vgz = VGZ_NewUngzip(wrk, "U S -");
		sctx.obuf = obuf;
		sctx.obuf_len = sizeof (obuf);
	}

	RES_StreamStart(sp);

	AssertObjCorePassOrBusy(req->obj->objcore);

	i = FetchBody(wrk, req->obj);

	http_Setup(wrk->busyobj->bereq, NULL);
	http_Setup(wrk->busyobj->beresp, NULL);
	wrk->busyobj->vfp = NULL;
	AZ(wrk->busyobj->vbc);
	AN(req->director);

	if (!i && req->obj->objcore != NULL) {
		EXP_Insert(req->obj);
		AN(req->obj->objcore);
		AN(req->obj->objcore->ban);
		HSH_Unbusy(wrk, 1);
	} else {
		req->doclose = "Stream error";
	}
	wrk->acct_tmp.fetch++;
	req->director = NULL;
	req->restarts = 0;

	RES_StreamEnd(sp);
	if (wrk->res_mode & RES_GUNZIP)
		(void)VGZ_Destroy(&sctx.vgz, sp->vsl_id);

	wrk->sctx = NULL;
	assert(WRW_IsReleased(wrk));
	assert(wrk->wrw.ciov == wrk->wrw.siov);
	(void)HSH_Deref(wrk, NULL, &req->obj);
	VBO_DerefBusyObj(wrk, &wrk->busyobj);
	http_Setup(req->resp, NULL);
	sp->step = STP_DONE;
	return (0);
}

/*--------------------------------------------------------------------
 * A freshly accepted socket
 *
DOT subgraph xcluster_first {
DOT	first [
DOT		shape=box
DOT		label="cnt_first:\nSockaddr's"
DOT	]
DOT }
DOT first -> wait
 */

static int
cnt_first(struct sess *sp, struct worker *wrk)
{
	char laddr[ADDR_BUFSIZE];
	char lport[PORT_BUFSIZE];

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);

	VTCP_name(&sp->sockaddr, sp->sockaddrlen,
	    sp->addr, sizeof sp->addr, sp->port, sizeof sp->port);
	if (cache_param->log_local_addr) {
		AZ(getsockname(sp->fd, (void*)&sp->mysockaddr,
		    &sp->mysockaddrlen));
		VTCP_name(&sp->mysockaddr, sp->mysockaddrlen,
		    laddr, sizeof laddr, lport, sizeof lport);
		WSP(sp, SLT_SessionOpen, "%s %s %s %s",
		    sp->addr, sp->port, laddr, lport);
	} else {
		WSP(sp, SLT_SessionOpen, "%s %s %s",
		    sp->addr, sp->port, sp->mylsock->name);
	}

	wrk->acct_tmp.sess++;

	sp->step = STP_WAIT;
	return (0);
}

/*--------------------------------------------------------------------
 * HIT
 * We had a cache hit.  Ask VCL, then march off as instructed.
 *
DOT subgraph xcluster_hit {
DOT	hit [
DOT		shape=record
DOT		label="vcl_hit()|req.\nobj."
DOT	]
DOT }
DOT hit -> err_hit [label="error"]
DOT err_hit [label="ERROR",shape=plaintext]
DOT hit -> rst_hit [label="restart",color=purple]
DOT rst_hit [label="RESTART",shape=plaintext]
DOT hit -> pass [label=pass,style=bold,color=red]
DOT hit -> prepresp [label="deliver",style=bold,color=green]
 */

static int
cnt_hit(struct sess *sp, struct worker *wrk, struct req *req)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	CHECK_OBJ_NOTNULL(req->obj, OBJECT_MAGIC);
	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);
	AZ(wrk->busyobj);

	assert(!(req->obj->objcore->flags & OC_F_PASS));

	VCL_hit_method(sp);

	if (req->handling == VCL_RET_DELIVER) {
		/* Dispose of any body part of the request */
		(void)FetchReqBody(sp);
		//AZ(wrk->busyobj->bereq->ws);
		//AZ(wrk->busyobj->beresp->ws);
		sp->step = STP_PREPRESP;
		return (0);
	}

	/* Drop our object, we won't need it */
	(void)HSH_Deref(wrk, NULL, &req->obj);
	req->objcore = NULL;

	switch(req->handling) {
	case VCL_RET_PASS:
		sp->step = STP_PASS;
		return (0);
	case VCL_RET_ERROR:
		sp->step = STP_ERROR;
		return (0);
	case VCL_RET_RESTART:
		req->director = NULL;
		req->restarts++;
		sp->step = STP_RECV;
		return (0);
	default:
		WRONG("Illegal action in vcl_hit{}");
	}
}

/*--------------------------------------------------------------------
 * LOOKUP
 * Hash things together and look object up in hash-table.
 *
 * LOOKUP consists of two substates so that we can reenter if we
 * encounter a busy object.
 *
DOT subgraph xcluster_lookup {
DOT	hash [
DOT		shape=record
DOT		label="vcl_hash()|req."
DOT	]
DOT	lookup [
DOT		shape=diamond
DOT		label="obj in cache ?\ncreate if not"
DOT	]
DOT	lookup2 [
DOT		shape=diamond
DOT		label="obj.f.pass ?"
DOT	]
DOT	hash -> lookup [label="hash",style=bold,color=green]
DOT	lookup -> lookup2 [label="yes",style=bold,color=green]
DOT }
DOT lookup2 -> hit [label="no", style=bold,color=green]
DOT lookup2 -> pass [label="yes",style=bold,color=red]
DOT lookup -> miss [label="no",style=bold,color=blue]
 */

static int
cnt_lookup(struct sess *sp, struct worker *wrk, struct req *req)
{
	struct objcore *oc;
	struct object *o;
	struct objhead *oh;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);
	AZ(wrk->busyobj);

	if (req->hash_objhead == NULL) {
		/* Not a waiting list return */
		AZ(req->vary_b);
		AZ(req->vary_l);
		AZ(req->vary_e);
		(void)WS_Reserve(req->ws, 0);
	} else {
		AN(req->ws->r);
	}
	req->vary_b = (void*)req->ws->f;
	req->vary_e = (void*)req->ws->r;
	req->vary_b[2] = '\0';

	oc = HSH_Lookup(sp, &oh);

	if (oc == NULL) {
		/*
		 * We lost the session to a busy object, disembark the
		 * worker thread.   The hash code to restart the session,
		 * still in STP_LOOKUP, later when the busy object isn't.
		 * NB:  Do not access sp any more !
		 */
		return (1);
	}


	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	CHECK_OBJ_NOTNULL(oh, OBJHEAD_MAGIC);

	/* If we inserted a new object it's a miss */
	if (oc->flags & OC_F_BUSY) {
		wrk->stats.cache_miss++;

		if (req->vary_l != NULL) {
			assert(oc->busyobj->vary == req->vary_b);
			VRY_Validate(oc->busyobj->vary);
			WS_ReleaseP(req->ws, (void*)req->vary_l);
		} else {
			AZ(oc->busyobj->vary);
			WS_Release(req->ws, 0);
		}
		req->vary_b = NULL;
		req->vary_l = NULL;
		req->vary_e = NULL;

		req->objcore = oc;
		CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
		sp->step = STP_MISS;
		return (0);
	}

	o = oc_getobj(wrk, oc);
	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	req->obj = o;

	WS_Release(req->ws, 0);
	req->vary_b = NULL;
	req->vary_l = NULL;
	req->vary_e = NULL;

	if (oc->flags & OC_F_PASS) {
		wrk->stats.cache_hitpass++;
		WSP(sp, SLT_HitPass, "%u", req->obj->xid);
		(void)HSH_Deref(wrk, NULL, &req->obj);
		req->objcore = NULL;
		sp->step = STP_PASS;
		return (0);
	}

	wrk->stats.cache_hit++;
	WSP(sp, SLT_Hit, "%u", req->obj->xid);
	sp->step = STP_HIT;
	return (0);
}

/*--------------------------------------------------------------------
 * We had a miss, ask VCL, proceed as instructed
 *
DOT subgraph xcluster_miss {
DOT	miss [
DOT		shape=ellipse
DOT		label="filter req.->bereq."
DOT	]
DOT	vcl_miss [
DOT		shape=record
DOT		label="vcl_miss()|req.\nbereq."
DOT	]
DOT	miss -> vcl_miss [style=bold,color=blue]
DOT }
DOT vcl_miss -> rst_miss [label="restart",color=purple]
DOT rst_miss [label="RESTART",shape=plaintext]
DOT vcl_miss -> err_miss [label="error"]
DOT err_miss [label="ERROR",shape=plaintext]
DOT vcl_miss -> fetch [label="fetch",style=bold,color=blue]
DOT vcl_miss -> pass [label="pass",style=bold,color=red]
DOT
 */

static int
cnt_miss(struct sess *sp, struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);

	AZ(req->obj);
	AN(req->objcore);
	CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
	WS_Reset(wrk->ws, NULL);
	wrk->busyobj = VBO_GetBusyObj(wrk);
	http_Setup(wrk->busyobj->bereq, wrk->ws);
	http_FilterReq(sp, HTTPH_R_FETCH);
	http_ForceGet(wrk->busyobj->bereq);
	if (cache_param->http_gzip_support) {
		/*
		 * We always ask the backend for gzip, even if the
		 * client doesn't grok it.  We will uncompress for
		 * the minority of clients which don't.
		 */
		http_Unset(wrk->busyobj->bereq, H_Accept_Encoding);
		http_SetHeader(wrk, sp->vsl_id, wrk->busyobj->bereq,
		    "Accept-Encoding: gzip");
	}
	wrk->connect_timeout = 0;
	wrk->first_byte_timeout = 0;
	wrk->between_bytes_timeout = 0;

	VCL_miss_method(sp);

	switch(req->handling) {
	case VCL_RET_ERROR:
		AZ(HSH_Deref(wrk, req->objcore, NULL));
		req->objcore = NULL;
		http_Setup(wrk->busyobj->bereq, NULL);
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		sp->step = STP_ERROR;
		return (0);
	case VCL_RET_PASS:
		AZ(HSH_Deref(wrk, req->objcore, NULL));
		req->objcore = NULL;
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		sp->step = STP_PASS;
		return (0);
	case VCL_RET_FETCH:
		CHECK_OBJ_NOTNULL(wrk->busyobj, BUSYOBJ_MAGIC);
		sp->step = STP_FETCH;
		return (0);
	case VCL_RET_RESTART:
		AZ(HSH_Deref(wrk, req->objcore, NULL));
		req->objcore = NULL;
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		INCOMPL();
	default:
		WRONG("Illegal action in vcl_miss{}");
	}
}

/*--------------------------------------------------------------------
 * Start pass processing by getting headers from backend, then
 * continue in passbody.
 *
DOT subgraph xcluster_pass {
DOT	pass [
DOT		shape=ellipse
DOT		label="deref obj."
DOT	]
DOT	pass2 [
DOT		shape=ellipse
DOT		label="filter req.->bereq."
DOT	]
DOT	vcl_pass [
DOT		shape=record
DOT		label="vcl_pass()|req.\nbereq."
DOT	]
DOT	pass_do [
DOT		shape=ellipse
DOT		label="create anon object\n"
DOT	]
DOT	pass -> pass2 [style=bold, color=red]
DOT	pass2 -> vcl_pass [style=bold, color=red]
DOT	vcl_pass -> pass_do [label="pass"] [style=bold, color=red]
DOT }
DOT pass_do -> fetch [style=bold, color=red]
DOT vcl_pass -> rst_pass [label="restart",color=purple]
DOT rst_pass [label="RESTART",shape=plaintext]
DOT vcl_pass -> err_pass [label="error"]
DOT err_pass [label="ERROR",shape=plaintext]
 */

static int
cnt_pass(struct sess *sp, struct worker *wrk, struct req *req)
{
	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);
	AZ(req->obj);
	AZ(wrk->busyobj);

	wrk->busyobj = VBO_GetBusyObj(wrk);
	WS_Reset(wrk->ws, NULL);
	wrk->busyobj = VBO_GetBusyObj(wrk);
	http_Setup(wrk->busyobj->bereq, wrk->ws);
	http_FilterReq(sp, HTTPH_R_PASS);

	wrk->connect_timeout = 0;
	wrk->first_byte_timeout = 0;
	wrk->between_bytes_timeout = 0;
	VCL_pass_method(sp);
	if (req->handling == VCL_RET_ERROR) {
		http_Setup(wrk->busyobj->bereq, NULL);
		VBO_DerefBusyObj(wrk, &wrk->busyobj);
		sp->step = STP_ERROR;
		return (0);
	}
	assert(req->handling == VCL_RET_PASS);
	wrk->acct_tmp.pass++;
	req->sendbody = 1;
	sp->step = STP_FETCH;
	return (0);
}

/*--------------------------------------------------------------------
 * Ship the request header to the backend unchanged, then pipe
 * until one of the ends close the connection.
 *
DOT subgraph xcluster_pipe {
DOT	pipe [
DOT		shape=ellipse
DOT		label="Filter req.->bereq."
DOT	]
DOT	vcl_pipe [
DOT		shape=record
DOT		label="vcl_pipe()|req.\nbereq\."
DOT	]
DOT	pipe_do [
DOT		shape=ellipse
DOT		label="send bereq.\npipe until close"
DOT	]
DOT	vcl_pipe -> pipe_do [label="pipe",style=bold,color=orange]
DOT	pipe -> vcl_pipe [style=bold,color=orange]
DOT }
DOT pipe_do -> DONE [style=bold,color=orange]
DOT vcl_pipe -> err_pipe [label="error"]
DOT err_pipe [label="ERROR",shape=plaintext]
 */

static int
cnt_pipe(struct sess *sp, struct worker *wrk, const struct req *req)
{

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);
	AZ(wrk->busyobj);

	wrk->acct_tmp.pipe++;
	wrk->busyobj = VBO_GetBusyObj(wrk);
	WS_Reset(wrk->ws, NULL);
	wrk->busyobj = VBO_GetBusyObj(wrk);
	http_Setup(wrk->busyobj->bereq, wrk->ws);
	http_FilterReq(sp, 0);

	VCL_pipe_method(sp);

	if (req->handling == VCL_RET_ERROR)
		INCOMPL();
	assert(req->handling == VCL_RET_PIPE);

	PipeSession(sp);
	assert(WRW_IsReleased(wrk));
	http_Setup(wrk->busyobj->bereq, NULL);
	VBO_DerefBusyObj(wrk, &wrk->busyobj);
	sp->step = STP_DONE;
	return (0);
}

/*--------------------------------------------------------------------
 * RECV
 * We have a complete request, set everything up and start it.
 *
DOT subgraph xcluster_recv {
DOT	recv [
DOT		shape=record
DOT		label="vcl_recv()|req."
DOT	]
DOT }
DOT ESI_REQ [ shape=hexagon ]
DOT RESTART -> recv
DOT ESI_REQ -> recv
DOT recv -> pipe [label="pipe",style=bold,color=orange]
DOT recv -> pass2 [label="pass",style=bold,color=red]
DOT recv -> err_recv [label="error"]
DOT err_recv [label="ERROR",shape=plaintext]
DOT recv -> hash [label="lookup",style=bold,color=green]
 */

static int
cnt_recv(struct sess *sp, struct worker *wrk, struct req *req)
{
	unsigned recv_handling;
	struct SHA256Context sha256ctx;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->vcl, VCL_CONF_MAGIC);
	AZ(req->obj);
	AZ(wrk->busyobj);
	assert(wrk->wrw.ciov == wrk->wrw.siov);

	/* By default we use the first backend */
	AZ(req->director);
	req->director = req->vcl->director[0];
	AN(req->director);

	req->disable_esi = 0;
	req->hash_always_miss = 0;
	req->hash_ignore_busy = 0;
	req->client_identity = NULL;

	http_CollectHdr(req->http, H_Cache_Control);

	VCL_recv_method(sp);
	recv_handling = req->handling;

	if (req->restarts >= cache_param->max_restarts) {
		if (req->err_code == 0)
			req->err_code = 503;
		sp->step = STP_ERROR;
		return (0);
	}

	if (cache_param->http_gzip_support &&
	     (recv_handling != VCL_RET_PIPE) &&
	     (recv_handling != VCL_RET_PASS)) {
		if (RFC2616_Req_Gzip(sp)) {
			http_Unset(req->http, H_Accept_Encoding);
			http_SetHeader(wrk, sp->vsl_id, req->http,
			    "Accept-Encoding: gzip");
		} else {
			http_Unset(req->http, H_Accept_Encoding);
		}
	}

	req->sha256ctx = &sha256ctx;
	SHA256_Init(req->sha256ctx);
	VCL_hash_method(sp);
	assert(req->handling == VCL_RET_HASH);
	SHA256_Final(req->digest, req->sha256ctx);
	req->sha256ctx = NULL;

	if (!strcmp(req->http->hd[HTTP_HDR_REQ].b, "HEAD"))
		req->wantbody = 0;
	else
		req->wantbody = 1;

	req->sendbody = 0;
	switch(recv_handling) {
	case VCL_RET_LOOKUP:
		/* XXX: discard req body, if any */
		sp->step = STP_LOOKUP;
		return (0);
	case VCL_RET_PIPE:
		if (req->esi_level > 0) {
			/* XXX: VSL something */
			INCOMPL();
			/* sp->step = STP_DONE; */
			return (1);
		}
		sp->step = STP_PIPE;
		return (0);
	case VCL_RET_PASS:
		sp->step = STP_PASS;
		return (0);
	case VCL_RET_ERROR:
		/* XXX: discard req body, if any */
		sp->step = STP_ERROR;
		return (0);
	default:
		WRONG("Illegal action in vcl_recv{}");
	}
}

/*--------------------------------------------------------------------
 * START
 * Handle a request.
 *
DOT start [
DOT	shape=box
DOT	label="cnt_start:\nDissect request\nHandle expect"
DOT ]
DOT start -> recv [style=bold,color=green]
DOT start -> DONE [label=errors]
 */

static int
cnt_start(struct sess *sp, struct worker *wrk, struct req *req)
{
	uint16_t done;
	char *p;
	const char *r = "HTTP/1.1 100 Continue\r\n\r\n";

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AZ(req->restarts);
	AZ(req->obj);
	AZ(req->vcl);
	EXP_Clr(&req->exp);
	AZ(req->esi_level);

	/* Update stats of various sorts */
	wrk->stats.client_req++;
	assert(!isnan(sp->t_req));
	wrk->acct_tmp.req++;

	/* Assign XID and log */
	req->xid = ++xids;				/* XXX not locked */
	WSP(sp, SLT_ReqStart, "%s %s %u", sp->addr, sp->port,  req->xid);

	/* Borrow VCL reference from worker thread */
	VCL_Refresh(&wrk->vcl);
	req->vcl = wrk->vcl;
	wrk->vcl = NULL;

	http_Setup(req->http, req->ws);
	done = http_DissectRequest(sp);

	/* If we could not even parse the request, just close */
	if (done == 400) {
		sp->step = STP_DONE;
		SES_Close(sp, "junk");
		return (0);
	}

	/* Catch request snapshot */
	req->ws_req = WS_Snapshot(req->ws);

	/* Catch original request, before modification */
	HTTP_Copy(req->http0, req->http);

	if (done != 0) {
		req->err_code = done;
		sp->step = STP_ERROR;
		return (0);
	}

	req->doclose = http_DoConnection(req->http);

	/* XXX: Handle TRACE & OPTIONS of Max-Forwards = 0 */

	/*
	 * Handle Expect headers
	 */
	if (http_GetHdr(req->http, H_Expect, &p)) {
		if (strcasecmp(p, "100-continue")) {
			req->err_code = 417;
			sp->step = STP_ERROR;
			return (0);
		}

		/* XXX: Don't bother with write failures for now */
		(void)write(sp->fd, r, strlen(r));
		/* XXX: When we do ESI includes, this is not removed
		 * XXX: because we use http0 as our basis.  Believed
		 * XXX: safe, but potentially confusing.
		 */
		http_Unset(req->http, H_Expect);
	}

	sp->step = STP_RECV;
	return (0);
}

/*--------------------------------------------------------------------
 * Central state engine dispatcher.
 *
 * Kick the session around until it has had enough.
 *
 */

static void
cnt_diag(struct sess *sp, const char *state)
{
	void *vcl;
	void *obj;

	if (sp->req == NULL) {
		vcl = NULL;
		obj = NULL;
	} else {
		vcl = sp->req->vcl;
		obj = sp->req->obj;
	}

	if (sp->wrk != NULL) {
		WSP(sp, SLT_Debug, "vsl_id %u STP_%s sp %p obj %p vcl %p",
		    sp->vsl_id, state, sp, obj, vcl);
		WSL_Flush(sp->wrk, 0);
	} else {
		VSL(SLT_Debug, sp->vsl_id,
		    "vsl_id %u STP_%s sp %p obj %p vcl %p",
		    sp->vsl_id, state, sp, obj, vcl);
	}
}

void
CNT_Session(struct sess *sp)
{
	int done;
	struct worker *wrk;

	CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
#if 0
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	MPL_AssertSane(req);
#endif
	wrk = sp->wrk;
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);

	/*
	 * Possible entrance states
	 */
	assert(
	    sp->step == STP_FIRST ||
	    sp->step == STP_WAIT ||
	    sp->step == STP_LOOKUP ||
	    sp->step == STP_RECV);

	/*
	 * Whenever we come in from the acceptor or waiter, we need to set
	 * blocking mode, but there is no point in setting it when we come from
	 * ESI or when a parked sessions returns.
	 * It would be simpler to do this in the acceptor or waiter, but we'd
	 * rather do the syscall in the worker thread.
	 * On systems which return errors for ioctl, we close early
	 */
	if ((sp->step == STP_FIRST || sp->step == STP_START) &&
	    VTCP_blocking(sp->fd)) {
		if (errno == ECONNRESET)
			SES_Close(sp, "remote closed");
		else
			SES_Close(sp, "error");
		sp->step = STP_DONE;
	}

	/*
	 * NB: Once done is set, we can no longer touch sp!
	 */
	for (done = 0; !done; ) {
		assert(sp->wrk == wrk);
#if 0
		CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
		MPL_AssertSane(req);
#endif
		/*
		 * This is a good place to be paranoid about the various
		 * pointers still pointing to the things we expect.
		 */
		CHECK_OBJ_NOTNULL(sp, SESS_MAGIC);
		CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
		CHECK_OBJ_ORNULL(wrk->nobjhead, OBJHEAD_MAGIC);
		WS_Assert(wrk->ws);

		switch (sp->step) {
#define STEP(l,u,arg) \
		    case STP_##u: \
			if (cache_param->diag_bitmap & 0x01) \
				cnt_diag(sp, #u); \
			done = cnt_##l arg; \
		        break;
#include "tbl/steps.h"
#undef STEP
		default:
			WRONG("State engine misfire");
		}
		WS_Assert(wrk->ws);
		CHECK_OBJ_ORNULL(wrk->nobjhead, OBJHEAD_MAGIC);
	}
	WSL_Flush(wrk, 0);
#define ACCT(foo)	AZ(wrk->acct_tmp.foo);
#include "tbl/acct_fields.h"
#undef ACCT
	assert(WRW_IsReleased(wrk));
}

/*
DOT }
*/

/*--------------------------------------------------------------------
 * Debugging aids
 */

static void
cli_debug_xid(struct cli *cli, const char * const *av, void *priv)
{
	(void)priv;
	if (av[2] != NULL)
		xids = strtoul(av[2], NULL, 0);
	VCLI_Out(cli, "XID is %u", xids);
}

/*
 * Default to seed=1, this is the only seed value POSIXl guarantees will
 * result in a reproducible random number sequence.
 */
static void
cli_debug_srandom(struct cli *cli, const char * const *av, void *priv)
{
	(void)priv;
	unsigned seed = 1;

	if (av[2] != NULL)
		seed = strtoul(av[2], NULL, 0);
	srandom(seed);
	srand48(random());
	VCLI_Out(cli, "Random(3) seeded with %u", seed);
}

static struct cli_proto debug_cmds[] = {
	{ "debug.xid", "debug.xid",
		"\tExamine or set XID\n", 0, 1, "d", cli_debug_xid },
	{ "debug.srandom", "debug.srandom",
		"\tSeed the random(3) function\n", 0, 1, "d",
		cli_debug_srandom },
	{ NULL }
};

/*--------------------------------------------------------------------
 *
 */

void
CNT_Init(void)
{

	srandomdev();
	srand48(random());
	xids = random();
	CLI_AddFuncs(debug_cmds);
}


