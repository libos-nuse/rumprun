/*-
 * Copyright (c) 2013 Antti Kantee.  All Rights Reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <mini-os/types.h>
#include <mini-os/console.h>

#include <xen/io/console.h>
#include <mini-os/xmalloc.h>
#include <mini-os/blkfront.h>
#include <mini-os/mm.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>

#include <bmk-common/errno.h>
#include <bmk-common/string.h>

#include "rumphyper.h"

struct rumpuser_hyperup rumpuser__hyp;

static struct rumpuser_mtx *bio_mtx;
static struct rumpuser_cv *bio_cv;
static int bio_outstanding_total;

#define RUMPHYPER_MYVERSION 17

int
rumpuser_init(int version, const struct rumpuser_hyperup *hyp)
{

	if (version != RUMPHYPER_MYVERSION) {
		minios_printk("Unsupported hypercall versions requested, %d vs %d\n",
		    version, RUMPHYPER_MYVERSION);
		return 1;
	}

	rumpuser__hyp = *hyp;

	rumpuser_mutex_init(&bio_mtx, RUMPUSER_MTX_SPIN);
	rumpuser_cv_init(&bio_cv);

	return 0;
}

void
rumpuser_putchar(int ch)
{
	char c = (char)ch;

	minios_console_print(NULL, &c, 1);
}

void
rumpuser_dprintf(const char *fmt, ...)
{
	char *buf;
	va_list va;

	buf = (void *)minios_alloc_pages(0);
	if (!buf)
		return;

	va_start(va, fmt);
	vsnprintf(buf, PAGE_SIZE, fmt, va);
	va_end(va);
	minios_console_print(NULL, buf, bmk_strlen(buf));

	minios_free_pages(buf, 0);
}

int
rumpuser_getparam(const char *name, void *buf, size_t buflen)
{
	int rv = 0;

	if (buflen <= 1)
		return BMK_EINVAL;

	if (bmk_strcmp(name, RUMPUSER_PARAM_NCPU) == 0
	    || bmk_strcmp(name, "RUMP_VERBOSE") == 0) {
		bmk_strncpy(buf, "1", buflen-1);

	} else if (bmk_strcmp(name, RUMPUSER_PARAM_HOSTNAME) == 0) {
		bmk_strncpy(buf, "rump-xen", buflen-1);

	/* for memlimit, we have to implement int -> string ... */
	} else if (bmk_strcmp(name, "RUMP_MEMLIMIT") == 0) {
		uint64_t memsize;
		char tmp[64];
		char *res = buf;
		unsigned i, j;

		/* use up to 50% memory for rump kernel */
		memsize = minios_get_memsize() / 2;
		if (memsize < (8 * 1024 * 1024)) {
			minios_printk("rumphyper: warning: low on physical "
				      "memory (%"PRIu64" bytes), "
				      "suggest increasing domU allocation\n",
				      memsize);
			memsize = 8 * 1024 * 1024;
		}

		for (i = 0; memsize > 0; i++) {
			if (i >= sizeof(tmp)-1)
				return BMK_E2BIG;
			tmp[i] = (memsize % 10) + '0';
			memsize = memsize / 10;
		}
		if (i >= buflen) {
			rv = 1;
		} else {
			res[i] = '\0';
			for (j = i; i > 0; i--) {
				res[j-i] = tmp[i-1];
			}
		}

	} else {
		rv = BMK_ENOENT;
	}

	return rv;
}

/* Use same values both for absolute and relative clock. */
int
rumpuser_clock_gettime(int which, int64_t *sec, long *nsec)
{
	uint32_t outsec;
	uint64_t outnsec;
	uint64_t time;

	switch (which) {
	case RUMPUSER_CLOCK_RELWALL:
		minios_clock_wall(&outsec, &outnsec);

		*sec = outsec;
		*nsec = outnsec;
		break;
	case RUMPUSER_CLOCK_ABSMONO:
		time = minios_clock_monotonic();

		*sec  = time / (1000*1000*1000ULL);
		*nsec = time % (1000*1000*1000ULL);
		break;
	}

	return 0;
}

int
rumpuser_clock_sleep(int enum_rumpclock, int64_t sec, long nsec)
{
	enum rumpclock rclk = enum_rumpclock;
	struct thread *thread;
	uint32_t msec;
	int nlocks;

	rumpkern_unsched(&nlocks, NULL);
	switch (rclk) {
	case RUMPUSER_CLOCK_RELWALL:
		msec = sec * 1000 + nsec / (1000*1000UL);
		minios_msleep(msec);
		break;
	case RUMPUSER_CLOCK_ABSMONO:
		thread = get_current();
		thread->wakeup_time = sec * (1000*1000*1000ULL) + nsec;
		clear_runnable(thread);
		minios_schedule();
		break;
	}
	rumpkern_sched(nlocks, NULL);

	return 0;
}

int
rumpuser_malloc(size_t len, int alignment, void **retval)
{

	/*
	 * If we are allocating precisely a page-sized chunk
	 * (the common case), use the Mini-OS page allocator directly.
	 * This avoids the malloc header overhead for this very
	 * common allocation, leading to 50% better memory use.
	 * We can't easily use the page allocator for larger chucks
	 * of memory, since those allocations might have stricter
	 * alignment restrictions, and therefore it's just
	 * easier to use memalloc() in those rare cases; it's not
	 * as wasteful for larger chunks anyway.
	 *
	 * XXX: how to make sure that rump kernel's and our
	 * page sizes are the same?  Could be problematic especially
	 * for architectures which support multiple page sizes.
	 * Note that the code will continue to work, but the optimization
	 * will not trigger for the common case.
	 */
	if (len == PAGE_SIZE) {
		ASSERT(alignment <= PAGE_SIZE);
		*retval = (void *)minios_alloc_page();
	} else {
		*retval = memalloc(len, alignment);
	}
	if (*retval)
		return 0;
	else
		return BMK_ENOMEM;
}

void
rumpuser_free(void *buf, size_t buflen)
{

	if (buflen == PAGE_SIZE)
		minios_free_page(buf);
	else
		memfree(buf);
}

/* Not very random */
int
rumpuser_getrandom(void *buf, size_t buflen, int flags, size_t *retp)
{
	uint8_t *rndbuf;

	for (*retp = 0, rndbuf = buf; *retp < buflen; (*retp)++) {
		*rndbuf++ = minios_clock_monotonic() & 0xff;
	}

	return 0;
}

void
rumpuser_exit(int value)
{
	minios_stop_kernel();
	minios_do_halt(MINIOS_HALT_POWEROFF);
}

#define NBLKDEV 10
#define BLKFDOFF 64
static struct blkfront_dev *blkdevs[NBLKDEV];
static struct blkfront_info blkinfos[NBLKDEV];
static int blkopen[NBLKDEV];
static int blkdev_outstanding[NBLKDEV];

static int
devopen(int num)
{
	int devnum = 768 + (num<<6);
	char buf[32];
	int nlocks;

	if (blkopen[num]) {
		blkopen[num]++;
		return 1;
	}

	snprintf(buf, sizeof(buf), "device/vbd/%d", devnum);

	rumpkern_unsched(&nlocks, NULL);
	blkdevs[num] = blkfront_init(buf, &blkinfos[num]);
	rumpkern_sched(nlocks, NULL);

	if (blkdevs[num] != NULL) {
		blkopen[num] = 1;
		return 0;
	} else {
		return BMK_EIO; /* guess something */
	}
}

static int
devname2num(const char *name)
{
	const char *p;
	int num;

	/* we support only block devices */
	if (bmk_strncmp(name, "blk", 3) != 0 || bmk_strlen(name) != 4)
		return -1;

	p = name + bmk_strlen(name)-1;
	num = *p - '0';
	if (num < 0 || num >= NBLKDEV)
		return -1;

	return num;
}

int
rumpuser_open(const char *name, int mode, int *fdp)
{
	int acc, rv, num;

	if ((mode & RUMPUSER_OPEN_BIO) == 0 || (num = devname2num(name)) == -1)
		return BMK_ENXIO;

	if ((rv = devopen(num)) != 0)
		return rv;

	acc = mode & RUMPUSER_OPEN_ACCMODE;
	if (acc == RUMPUSER_OPEN_WRONLY || acc == RUMPUSER_OPEN_RDWR) {
		if (blkinfos[num].mode != O_RDWR) {
			/* XXX: unopen */
			return BMK_EROFS;
		}
	}

	*fdp = BLKFDOFF + num;
	return 0;
}

int
rumpuser_close(int fd)
{
	int rfd = fd - BLKFDOFF;

	if (rfd < 0 || rfd+1 > NBLKDEV)
		return BMK_EBADF;

	if (--blkopen[rfd] == 0) {
		struct blkfront_dev *toclose = blkdevs[rfd];
		
		/* not sure if this appropriately prevents races either ... */
		blkdevs[rfd] = NULL;
		blkfront_shutdown(toclose);
	}

	return 0;
}

int
rumpuser_getfileinfo(const char *name, uint64_t *size, int *type)
{
	int rv, num;

	if ((num = devname2num(name)) == -1)
		return BMK_ENXIO;
	if ((rv = devopen(num)) != 0)
		return rv;

	*size = blkinfos[num].sectors * blkinfos[num].sector_size;
	*type = RUMPUSER_FT_BLK;

	rumpuser_close(num + BLKFDOFF);

	return 0;
}

struct biocb {
	struct blkfront_aiocb bio_aiocb;
	int bio_num;
	rump_biodone_fn bio_done;
	void *bio_arg;
};

static void
biocomp(struct blkfront_aiocb *aiocb, int ret)
{
	struct biocb *bio = aiocb->data;
	int dummy, num;

	rumpkern_sched(0, NULL);
	if (ret)
		bio->bio_done(bio->bio_arg, 0, BMK_EIO);
	else
		bio->bio_done(bio->bio_arg, bio->bio_aiocb.aio_nbytes, 0);
	rumpkern_unsched(&dummy, NULL);
	num = bio->bio_num;
	xfree(bio);

	rumpuser_mutex_enter_nowrap(bio_mtx);
	bio_outstanding_total--;
	blkdev_outstanding[num]--;
	rumpuser_mutex_exit(bio_mtx);
}

static void
biothread(void *arg)
{
	DEFINE_WAIT(w);
	int i, flags, did;

	/* for the bio callback */
	rumpuser__hyp.hyp_schedule();
	rumpuser__hyp.hyp_lwproc_newlwp(0);
	rumpuser__hyp.hyp_unschedule();

	for (;;) {
		rumpuser_mutex_enter_nowrap(bio_mtx);
		while (bio_outstanding_total == 0) {
			rumpuser_cv_wait_nowrap(bio_cv, bio_mtx);
		}
		rumpuser_mutex_exit(bio_mtx);

		/*
		 * if we made any progress, recheck.  could be batched,
		 * but since currently locks are free here ... meh
		 */
		local_irq_save(flags);
		for (did = 0;;) {
			for (i = 0; i < NBLKDEV; i++) {
				if (blkdev_outstanding[i])
					did += blkfront_aio_poll(blkdevs[i]);
			}
			if (did)
				break;
			minios_add_waiter(w, blkfront_queue);
			local_irq_restore(flags);
			minios_schedule();
			local_irq_save(flags);
		}
		local_irq_restore(flags);
	}
}

void
rumpuser_bio(int fd, int op, void *data, size_t dlen, int64_t off,
	rump_biodone_fn biodone, void *donearg)
{
	static int bio_inited;
	struct biocb *bio = memalloc(sizeof(*bio), 0);
	struct blkfront_aiocb *aiocb = &bio->bio_aiocb;
	int nlocks;
	int num = fd - BLKFDOFF;

	rumpkern_unsched(&nlocks, NULL);

	if (!bio_inited) {
		rumpuser_mutex_enter_nowrap(bio_mtx);
		if (!bio_inited) {
			bio_inited = 1;
			rumpuser_mutex_exit(bio_mtx);
			minios_create_thread("biopoll", NULL,
			    biothread, NULL, NULL);
		} else {
			rumpuser_mutex_exit(bio_mtx);
		}
	}

	bio->bio_done = biodone;
	bio->bio_arg = donearg;
	bio->bio_num = num;

	aiocb->aio_dev = blkdevs[num];
	aiocb->aio_buf = data;
	aiocb->aio_nbytes = dlen;
	aiocb->aio_offset = off;
	aiocb->aio_cb = biocomp;
	aiocb->data  = bio;

	if (op & RUMPUSER_BIO_READ)
		blkfront_aio_read(aiocb);
	else
		blkfront_aio_write(aiocb);

	rumpuser_mutex_enter(bio_mtx);
	bio_outstanding_total++;
	blkdev_outstanding[num]++;
	rumpuser_cv_signal(bio_cv);
	rumpuser_mutex_exit(bio_mtx);

	rumpkern_sched(nlocks, NULL);
}

/* XXX FIXME */
#include <errno.h>

void
rumpuser_seterrno(int err)
{

	errno = err;
}
