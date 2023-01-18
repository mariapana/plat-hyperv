/*-
 * Copyright (c) 2016 Microsoft Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <uk/mutex.h>
#include <uk/wait.h>
#include <uk/plat/io.h>

#include <include/vmbus_xact.h>
#include <include/hyperv.h>

#include <hyperv/bsd_layer.h>


struct vmbus_xact {
	struct vmbus_xact_ctx		*x_ctx;
	void				*x_priv;

	void				*x_req;
	//struct hyperv_dma		x_req_dma;
	bus_addr_t	x_req_paddr;

	const void			*x_resp;
	size_t				x_resp_len;
	void				*x_resp0;
	struct uk_waitq         x_wq;
};

struct vmbus_xact_ctx {
	size_t				xc_req_size;
	size_t				xc_resp_size;
	size_t				xc_priv_size;

	struct uk_mutex			xc_lock;
	/*
	 * Protected by xc_lock.
	 */
	uint32_t			xc_flags;	/* VMBUS_XACT_CTXF_ */
	struct vmbus_xact		*xc_free;
	struct vmbus_xact		*xc_active;
	struct vmbus_xact		*xc_orphan;
};

#define VMBUS_XACT_CTXF_DESTROY		0x0001

// static struct vmbus_xact	*vmbus_xact_alloc(struct vmbus_xact_ctx *,
// 				    bus_dma_tag_t);
static struct vmbus_xact	*vmbus_xact_alloc(struct vmbus_xact_ctx *,
				    struct uk_alloc *);
// static void			vmbus_xact_free(struct vmbus_xact *);
static struct vmbus_xact	*vmbus_xact_get1(struct vmbus_xact_ctx *,
				    uint32_t);
static const void		*vmbus_xact_wait1(struct vmbus_xact *, size_t *,
				    bool);
static const void		*vmbus_xact_return(struct vmbus_xact *,
				    size_t *);
static void			vmbus_xact_save_resp(struct vmbus_xact *,
				    const void *, size_t);
// static void			vmbus_xact_ctx_free(struct vmbus_xact_ctx *);

// static struct vmbus_xact *
// vmbus_xact_alloc(struct vmbus_xact_ctx *ctx, bus_dma_tag_t parent_dtag)
static struct vmbus_xact *
vmbus_xact_alloc(struct vmbus_xact_ctx *ctx, struct uk_alloc *a)
{
	struct vmbus_xact *xact;

	//xact = malloc(sizeof(*xact), M_DEVBUF, M_WAITOK | M_ZERO);
	xact = uk_calloc(a, 1, sizeof(*xact));
	xact->x_ctx = ctx;

	/* XXX assume that page aligned is enough */
	// xact->x_req = hyperv_dmamem_alloc(parent_dtag, PAGE_SIZE, 0,
	//     ctx->xc_req_size, &xact->x_req_dma, BUS_DMA_WAITOK);
	// uk_pr_info("vmbus_xact_allock ctx->xc_req_size: %lu", ctx->xc_req_size);
	xact->x_req = hyperv_mem_alloc(a, ctx->xc_req_size);
	if (xact->x_req == NULL) {
		// free(xact, M_DEVBUF);
		uk_free(a, xact);
		return (NULL);
	}
	xact->x_req_paddr = ukplat_virt_to_phys(xact->x_req);

	if (ctx->xc_priv_size != 0)
		// xact->x_priv = malloc(ctx->xc_priv_size, M_DEVBUF, M_WAITOK);
		xact->x_priv = uk_malloc(a, ctx->xc_priv_size);
	// xact->x_resp0 = malloc(ctx->xc_resp_size, M_DEVBUF, M_WAITOK);
	xact->x_resp0 = uk_malloc(a, ctx->xc_resp_size);

	uk_waitq_init(&xact->x_wq);

	return (xact);
}

// static void
// vmbus_xact_free(struct vmbus_xact *xact)
// {

// 	hyperv_dmamem_free(&xact->x_req_dma, xact->x_req);
// 	free(xact->x_resp0, M_DEVBUF);
// 	if (xact->x_priv != NULL)
// 		free(xact->x_priv, M_DEVBUF);
// 	free(xact, M_DEVBUF);
// }

static struct vmbus_xact *
vmbus_xact_get1(struct vmbus_xact_ctx *ctx, uint32_t dtor_flag)
{
	struct vmbus_xact *xact;

	mtx_lock(&ctx->xc_lock);

	while ((ctx->xc_flags & dtor_flag) == 0 && ctx->xc_free == NULL)
		// mtx_sleep(&ctx->xc_free, &ctx->xc_lock, 0, "gxact", 0);
		mtx_sleep(&ctx->xc_free->x_wq, (ctx->xc_flags & dtor_flag) == 0 && ctx->xc_free == NULL, &ctx->xc_lock, 0, "gxact", 0);
	if (ctx->xc_flags & dtor_flag) {
		/* Being destroyed */
		xact = NULL;
	} else {
		xact = ctx->xc_free;
		KASSERT(xact != NULL, ("no free xact"));
		KASSERT(xact->x_resp == NULL, ("xact has pending response"));
		ctx->xc_free = NULL;
	}

	mtx_unlock(&ctx->xc_lock);

	return (xact);
}

// struct vmbus_xact_ctx *
// vmbus_xact_ctx_create(bus_dma_tag_t dtag, size_t req_size, size_t resp_size,
//     size_t priv_size)
struct vmbus_xact_ctx *
vmbus_xact_ctx_create(struct uk_alloc *a, size_t req_size, size_t resp_size,
    size_t priv_size)
{
	struct vmbus_xact_ctx *ctx;

	KASSERT(req_size > 0, ("request size is 0"));
	KASSERT(resp_size > 0, ("response size is 0"));

// 	ctx = malloc(sizeof(*ctx), M_DEVBUF, M_WAITOK | M_ZERO);
	ctx = uk_calloc(a, 1, sizeof(*ctx));
	ctx->xc_req_size = req_size;
	ctx->xc_resp_size = resp_size;
	ctx->xc_priv_size = priv_size;

	ctx->xc_free = vmbus_xact_alloc(ctx, a);
	if (ctx->xc_free == NULL) {
		// free(ctx, M_DEVBUF);
		uk_free(a, ctx);
		return (NULL);
	}

	mtx_init(&ctx->xc_lock, "vmbus xact", NULL, MTX_DEF);

	return (ctx);
}

bool
vmbus_xact_ctx_orphan(struct vmbus_xact_ctx *ctx)
{
	uk_pr_info("[vmbus_xact_ctx_orphan] ctx: %p, start\n", ctx);

	mtx_lock(&ctx->xc_lock);
	if (ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY) {
		mtx_unlock(&ctx->xc_lock);
		uk_pr_info("[vmbus_xact_ctx_orphan] ctx: %p, end (return false)\n", ctx);
		return (false);
	}
	ctx->xc_flags |= VMBUS_XACT_CTXF_DESTROY;
	mtx_unlock(&ctx->xc_lock);

	wakeup(&ctx->xc_free->x_wq);
	wakeup(&ctx->xc_active->x_wq);

	ctx->xc_orphan = vmbus_xact_get1(ctx, 0);
	if (ctx->xc_orphan == NULL)
		panic("can't get xact");

	uk_pr_info("[vmbus_xact_ctx_orphan] ctx: %p, end (return true)\n", ctx);

	return (true);
}

// static void
// vmbus_xact_ctx_free(struct vmbus_xact_ctx *ctx)
// {
// 	KASSERT(ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY,
// 	    ("xact ctx was not orphaned"));
// 	KASSERT(ctx->xc_orphan != NULL, ("no orphaned xact"));

// 	vmbus_xact_free(ctx->xc_orphan);
// 	mtx_destroy(&ctx->xc_lock);
// 	free(ctx, M_DEVBUF);
// }

// void
// vmbus_xact_ctx_destroy(struct vmbus_xact_ctx *ctx)
// {

// 	vmbus_xact_ctx_orphan(ctx);
// 	vmbus_xact_ctx_free(ctx);
// }

struct vmbus_xact *
vmbus_xact_get(struct vmbus_xact_ctx *ctx, size_t req_len)
{
	struct vmbus_xact *xact;

	if (req_len > ctx->xc_req_size)
		panic("invalid request size %zu", req_len);

	xact = vmbus_xact_get1(ctx, VMBUS_XACT_CTXF_DESTROY);
	if (xact == NULL)
		return (NULL);

	memset(xact->x_req, 0, req_len);
	return (xact);
}

void
vmbus_xact_put(struct vmbus_xact *xact)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;

	uk_pr_info("[vmbus_xact_put] ctx: %p, start\n", ctx);

	KASSERT(ctx->xc_active == NULL, ("pending active xact"));
	xact->x_resp = NULL;

	//mtx_lock(&ctx->xc_lock);
	uk_mutex_lock(&ctx->xc_lock);
	KASSERT(ctx->xc_free == NULL, ("has free xact"));
	ctx->xc_free = xact;
	// mtx_unlock(&ctx->xc_lock);
	uk_mutex_unlock(&ctx->xc_lock);
	wakeup(&ctx->xc_free->x_wq);

	uk_pr_info("[vmbus_xact_put] ctx: %p, end\n", ctx);
}

void *
vmbus_xact_req_data(const struct vmbus_xact *xact)
{
	// uk_pr_info("vmbus_xact_req_data\n");
	return (xact->x_req);
}

bus_addr_t
vmbus_xact_req_paddr(const struct vmbus_xact *xact)
{
	// uk_pr_info("vmbus_xact_req_paddr\n");
	//return (xact->x_req_dma.hv_paddr);
	return (xact->x_req_paddr);

}

void *
vmbus_xact_priv(const struct vmbus_xact *xact, size_t priv_len)
{

	if (priv_len > xact->x_ctx->xc_priv_size)
		panic("invalid priv size %zu", priv_len);
	return (xact->x_priv);
}

void
vmbus_xact_activate(struct vmbus_xact *xact)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;

	// uk_pr_info("vmbus_xact_activate start\n");

	KASSERT(xact->x_resp == NULL, ("xact has pending response"));

	mtx_lock(&ctx->xc_lock);
	KASSERT(ctx->xc_active == NULL, ("pending active xact"));
	ctx->xc_active = xact;
	mtx_unlock(&ctx->xc_lock);

	// uk_pr_info("[vmbus_xact_activate end\n");
}

void
vmbus_xact_deactivate(struct vmbus_xact *xact)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;

	// uk_pr_info("vmbus_xact_deactivate start\n");

	mtx_lock(&ctx->xc_lock);
	KASSERT(ctx->xc_active == xact, ("xact mismatch"));
	ctx->xc_active = NULL;
	mtx_unlock(&ctx->xc_lock);

	// uk_pr_info("vmbus_xact_deactivate end\n");
}

static const void *
vmbus_xact_return(struct vmbus_xact *xact, size_t *resp_len)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;
	const void *resp;

	uk_pr_info("[vmbus_xact_return] ctx: %p start\n", ctx);

	//mtx_assert(&ctx->xc_lock, MA_OWNED);
	KASSERT(ctx->xc_active == xact, ("xact trashed"));

	if ((ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY) && xact->x_resp == NULL) {
		uint8_t b = 0;

		/*
		 * Orphaned and no response was received yet; fake up
		 * an one byte response.
		 */
		printf("vmbus: xact ctx was orphaned w/ pending xact\n");
		vmbus_xact_save_resp(ctx->xc_active, &b, sizeof(b));
	}
	KASSERT(xact->x_resp != NULL, ("no response"));

	ctx->xc_active = NULL;

	resp = xact->x_resp;
	*resp_len = xact->x_resp_len;

	uk_pr_info("[vmbus_xact_return] ctx: %p end\n", ctx);

	return (resp);
}

static const void *
vmbus_xact_wait1(struct vmbus_xact *xact, size_t *resp_len,
    bool can_sleep)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;
	const void *resp;

	uk_pr_info("[vmbus_xact_wait1] ctx: %p, start\n", ctx);

	mtx_lock(&ctx->xc_lock);

	uk_pr_info("[vmbus_xact_wait1] ctx: %p, ctx->xc_flags: %u\n", ctx, ctx->xc_flags);
	uk_pr_info("[vmbus_xact_wait1] ctx->xc_active: %p, ctx->xc_active->x_wq: %p\n", ctx->xc_active, &ctx->xc_active->x_wq);
	uk_pr_info("[vmbus_xact_wait1] thread: %p\n", uk_thread_current());
	KASSERT(ctx->xc_active == xact, ("xact mismatch"));
	while (xact->x_resp == NULL &&
	    (ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY) == 0) {
		if (can_sleep) {
			uk_pr_info("[vmbus_xact_wait1] ctx: %p, mtx_sleep xact->x_resp: %p, ctx->xc_flags: %u\n", ctx, xact->x_resp, ctx->xc_flags);
// 			mtx_sleep(&ctx->xc_active, &ctx->xc_lock, 0,
// 			    "wxact", 0);
			mtx_sleep(&ctx->xc_active->x_wq, !(xact->x_resp == NULL &&
	    		(ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY) == 0), &ctx->xc_lock, 0,
			    "wxact", 1000);
			uk_pr_info("[vmbus_xact_wait1] ctx: %p woke up\n");
		} else {
			uk_pr_info("[vmbus_xact_wait1] ctx: %p, delay\n", ctx);
			mtx_unlock(&ctx->xc_lock);
			DELAY(1000);
			mtx_lock(&ctx->xc_lock);
		}
	}
	resp = vmbus_xact_return(xact, resp_len);

	mtx_unlock(&ctx->xc_lock);

	uk_pr_info("[vmbus_xact_wait1] ctx: %p, end\n", ctx);

	return (resp);
}

const void *
vmbus_xact_wait(struct vmbus_xact *xact, size_t *resp_len)
{

	return (vmbus_xact_wait1(xact, resp_len, true /* can sleep */));
}

const void *
vmbus_xact_busywait(struct vmbus_xact *xact, size_t *resp_len)
{

	return (vmbus_xact_wait1(xact, resp_len, false /* can't sleep */));
}

const void *
vmbus_xact_poll(struct vmbus_xact *xact, size_t *resp_len)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;
	const void *resp;

	uk_pr_info("[vmbus_xact_poll] ctx: %p start\n", ctx);

	mtx_lock(&ctx->xc_lock);

	KASSERT(ctx->xc_active == xact, ("xact mismatch"));
	if (xact->x_resp == NULL &&
	    (ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY) == 0) {
		mtx_unlock(&ctx->xc_lock);
		*resp_len = 0;
		return (NULL);
	}
	resp = vmbus_xact_return(xact, resp_len);

	mtx_unlock(&ctx->xc_lock);

	uk_pr_info("[vmbus_xact_poll] ctx: %p end\n", ctx);

	return (resp);
}

static void
vmbus_xact_save_resp(struct vmbus_xact *xact, const void *data, size_t dlen)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;
	size_t cplen = dlen;

	uk_pr_info("[vmbus_xact_save_resp] ctx: %p, start\n", ctx);

	// mtx_assert(&ctx->xc_lock, MA_OWNED);

	if (cplen > ctx->xc_resp_size) {
		printf("vmbus: xact response truncated %zu -> %zu\n",
		    cplen, ctx->xc_resp_size);
		cplen = ctx->xc_resp_size;
	}

	KASSERT(ctx->xc_active == xact, ("xact mismatch"));
	memcpy(xact->x_resp0, data, cplen);
	xact->x_resp_len = cplen;
	xact->x_resp = xact->x_resp0;

	uk_pr_info("[vmbus_xact_save_resp] ctx: %p, end\n", ctx);
}

void
vmbus_xact_wakeup(struct vmbus_xact *xact, const void *data, size_t dlen)
{
	struct vmbus_xact_ctx *ctx = xact->x_ctx;
	int do_wakeup = 0;

	uk_pr_info("[vmbus_xact_wakeup] ctx: %p, start\n", ctx);

	mtx_lock(&ctx->xc_lock);
	/*
	 * NOTE:
	 * xc_active could be NULL, if the ctx has been orphaned.
	 */
	if (ctx->xc_active != NULL) {
		vmbus_xact_save_resp(xact, data, dlen);
		do_wakeup = 1;
	} else {
		KASSERT(ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY,
		    ("no active xact pending"));
		printf("vmbus: drop xact response\n");
	}
	mtx_unlock(&ctx->xc_lock);

	if (do_wakeup)
		wakeup(&ctx->xc_active);
	
	uk_pr_info("[vmbus_xact_wakeup] ctx: %p, end\n", ctx);
}

void
vmbus_xact_ctx_wakeup(struct vmbus_xact_ctx *ctx, const void *data, size_t dlen)
{
	int do_wakeup = 0;

	uk_pr_info("[vmbus_xact_ctx_wakeup] ctx: %p, start\n", ctx);

	mtx_lock(&ctx->xc_lock);
	/*
	 * NOTE:
	 * xc_active could be NULL, if the ctx has been orphaned.
	 */
	if (ctx->xc_active != NULL) {
		vmbus_xact_save_resp(ctx->xc_active, data, dlen);
		do_wakeup = 1;
	} else {
		KASSERT(ctx->xc_flags & VMBUS_XACT_CTXF_DESTROY,
		    ("no active xact pending"));
		printf("vmbus: drop xact response\n");
	}
	mtx_unlock(&ctx->xc_lock);

	if (do_wakeup) {
		uk_pr_info("[vmbus_xact_ctx_wakeup] wakeup ctx->xc_active: %p, ctx->xc_active->x_wq: %p\n", ctx->xc_active, &ctx->xc_active->x_wq);
		wakeup(&ctx->xc_active->x_wq);
	}

	uk_pr_info("[vmbus_xact_ctx_wakeup] ctx: %p, end\n", ctx);
}
