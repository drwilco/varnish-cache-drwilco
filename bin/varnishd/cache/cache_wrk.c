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
 * Worker thread stuff unrelated to the worker thread pools.
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>

#include "cache.h"

#include "hash/hash_slinger.h"

static struct lock		wstat_mtx;

/*--------------------------------------------------------------------*/

static void
wrk_sumstat(struct worker *w)
{

	Lck_AssertHeld(&wstat_mtx);
#define L0(n)
#define L1(n) (VSC_C_main->n += w->stats.n)
#define VSC_F(n, t, l, f, d, e) L##l(n);
#include "tbl/vsc_f_main.h"
#undef VSC_F
#undef L0
#undef L1
	memset(&w->stats, 0, sizeof w->stats);
}

void
WRK_SumStat(struct worker *w)
{

	Lck_Lock(&wstat_mtx);
	wrk_sumstat(w);
	Lck_Unlock(&wstat_mtx);
}

int
WRK_TrySumStat(struct worker *w)
{
	if (Lck_Trylock(&wstat_mtx))
		return (0);
	wrk_sumstat(w);
	Lck_Unlock(&wstat_mtx);
	return (1);
}

/*--------------------------------------------------------------------
 * Create and starte a back-ground thread which as its own worker and
 * session data structures;
 */

struct bgthread {
	unsigned	magic;
#define BGTHREAD_MAGIC	0x23b5152b
	const char	*name;
	bgthread_t	*func;
	void		*priv;
};

static void *
wrk_bgthread(void *arg)
{
	struct bgthread *bt;
	struct worker wrk;
	struct sess *sp;
	uint32_t logbuf[1024];	/* XXX:  size ? */

	CAST_OBJ_NOTNULL(bt, arg, BGTHREAD_MAGIC);
	THR_SetName(bt->name);
	sp = SES_Alloc();
	XXXAN(sp);
	memset(&wrk, 0, sizeof wrk);
	sp->wrk = &wrk;
	wrk.magic = WORKER_MAGIC;
	wrk.wlp = wrk.wlb = logbuf;
	wrk.wle = logbuf + (sizeof logbuf) / 4;

	(void)bt->func(sp, bt->priv);

	WRONG("BgThread terminated");

	NEEDLESS_RETURN(NULL);
}

void
WRK_BgThread(pthread_t *thr, const char *name, bgthread_t *func, void *priv)
{
	struct bgthread *bt;

	ALLOC_OBJ(bt, BGTHREAD_MAGIC);
	AN(bt);

	bt->name = name;
	bt->func = func;
	bt->priv = priv;
	AZ(pthread_create(thr, NULL, wrk_bgthread, bt));
}

/*--------------------------------------------------------------------*/

static void *
wrk_thread_real(void *priv, unsigned shm_workspace,
    unsigned wthread_workspace, unsigned siov)
{
	struct worker *w, ww;
	uint32_t wlog[shm_workspace / 4];
	/* XXX: can we trust these to be properly aligned ? */
	unsigned char ws[wthread_workspace];
	struct iovec iov[siov];

	THR_SetName("cache-worker");
	w = &ww;
	memset(w, 0, sizeof *w);
	w->magic = WORKER_MAGIC;
	w->lastused = NAN;
	w->wlb = w->wlp = wlog;
	w->wle = wlog + (sizeof wlog) / 4;
	w->wrw.iov = iov;
	w->wrw.siov = siov;
	w->wrw.ciov = siov;
	AZ(pthread_cond_init(&w->cond, NULL));

	WS_Init(w->ws, "wrk", ws, wthread_workspace);

	VSL(SLT_WorkThread, 0, "%p start", w);

	Pool_Work_Thread(priv, w);
	AZ(w->pool);

	VSL(SLT_WorkThread, 0, "%p end", w);
	if (w->vcl != NULL)
		VCL_Rel(&w->vcl);
	AZ(pthread_cond_destroy(&w->cond));
	HSH_Cleanup(w);
	WRK_SumStat(w);
	return (NULL);
}

void *
WRK_thread(void *priv)
{
	uint16_t nhttp;
	unsigned siov;

	assert(cache_param->http_max_hdr <= 65535);
	/* We need to snapshot these two for consistency */
	nhttp = (uint16_t)cache_param->http_max_hdr;
	siov = nhttp * 2;
	if (siov > IOV_MAX)
		siov = IOV_MAX;
	return (wrk_thread_real(priv,
	    cache_param->shm_workspace,
	    cache_param->wthread_workspace, siov));
}

void
WRK_Init(void)
{
	Lck_New(&wstat_mtx, lck_wstat);
}
