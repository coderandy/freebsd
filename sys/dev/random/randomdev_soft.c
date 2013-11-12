/*-
 * Copyright (c) 2000-2013 Mark R V Murray
 * Copyright (c) 2004 Robert N. M. Watson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
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
 *
 */

/*
 * This is the loadable infrastructure base file for software CSPRNG
 * drivers such as Yarrow or Fortuna.
 *
 * It is anticipated that one instance of this file will be used
 * for _each_ invocation of a CSPRNG, but with different #defines
 * set. See below.
 *
 */

#include "opt_random.h"

#if !defined(RANDOM_YARROW) && !defined(RANDOM_FORTUNA)
#define RANDOM_YARROW
#elif defined(RANDOM_YARROW) && defined(RANDOM_FORTUNA)
#error "Must define either RANDOM_YARROW or RANDOM_FORTUNA"
#endif
#if defined(RANDOM_FORTUNA)
#error "Fortuna is not yet implemented"
#endif

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/poll.h>
#include <sys/random.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/unistd.h>

#include <dev/random/randomdev.h>
#include <dev/random/randomdev_soft.h>
#include <dev/random/random_harvestq.h>
#include <dev/random/random_adaptors.h>
#if defined(RANDOM_YARROW)
#include <dev/random/yarrow.h>
#endif
#if defined(RANDOM_FORTUNA)
#include <dev/random/fortuna.h>
#endif

static int randomdev_poll(int event, struct thread *td);
static int randomdev_block(int flag);
static void randomdev_flush_reseed(void);

#if defined(RANDOM_YARROW)
static struct random_adaptor random_soft_processor = {
	.ra_ident = "Software, Yarrow",
	.ra_init = randomdev_init,
	.ra_deinit = randomdev_deinit,
	.ra_block = randomdev_block,
	.ra_read = random_yarrow_read,
	.ra_poll = randomdev_poll,
	.ra_reseed = randomdev_flush_reseed,
	.ra_seeded = 0, /* This will be seeded during entropy processing */
	.ra_priority = 90, /* High priority, so top of the list. Fortuna may still win. */
};
#define RANDOM_MODULE_NAME	yarrow
#define RANDOM_CSPRNG_NAME	"yarrow"
#endif

#if defined(RANDOM_FORTUNA)
static struct random_adaptor random_soft_processor = {
	.ra_ident = "Software, Fortuna",
	.ra_init = randomdev_init,
	.ra_deinit = randomdev_deinit,
	.ra_block = randomdev_block,
	.ra_read = random_fortuna_read,
	.ra_poll = randomdev_poll,
	.ra_reseed = randomdev_flush_reseed,
	.ra_seeded = 0, /* This will be excplicitly seeded at startup when secured */
	.ra_priority = 100, /* High priority, so top of the list. Beat Yarrow. */
};
#define RANDOM_MODULE_NAME	fortuna
#define RANDOM_CSPRNG_NAME	"fortuna"
#endif

TUNABLE_INT("kern.random.sys.seeded", &random_soft_processor.ra_seeded);

/* List for the dynamic sysctls */
static struct sysctl_ctx_list random_clist;

static struct selinfo rsel;

/* ARGSUSED */
static int
random_check_boolean(SYSCTL_HANDLER_ARGS)
{
	if (oidp->oid_arg1 != NULL && *(u_int *)(oidp->oid_arg1) != 0)
		*(u_int *)(oidp->oid_arg1) = 1;
	return (sysctl_handle_int(oidp, oidp->oid_arg1, oidp->oid_arg2, req));
}

/* ARGSUSED */
RANDOM_CHECK_UINT(harvestmask, 0, ((1<<RANDOM_ENVIRONMENTAL_END) - 1));

/* ARGSUSED */
static int
random_print_harvestmask(SYSCTL_HANDLER_ARGS)
{
	struct sbuf sbuf;
	int error, i;

	error = sysctl_wire_old_buffer(req, 0);
	if (error != 0)
		return (error);

	sbuf_new_for_sysctl(&sbuf, NULL, 128, req);

	for (i = 31; i >= 0; i--)
		sbuf_cat(&sbuf, (randomdev_harvest_source_mask & (1<<i)) ? "1" : "0");

	error = sbuf_finish(&sbuf);
	sbuf_delete(&sbuf);

	return (error);
}

void
randomdev_init(void)
{
	struct sysctl_oid *random_sys_o;

#if defined(RANDOM_YARROW)
	random_yarrow_init_alg(&random_clist);
#endif
#if defined(RANDOM_FORTUNA)
	random_fortuna_init_alg(&random_clist);
#endif

	random_sys_o = SYSCTL_ADD_NODE(&random_clist,
	    SYSCTL_STATIC_CHILDREN(_kern_random),
	    OID_AUTO, "sys", CTLFLAG_RW, 0,
	    "Entropy Device Parameters");

	SYSCTL_ADD_PROC(&random_clist,
	    SYSCTL_CHILDREN(random_sys_o),
	    OID_AUTO, "seeded", CTLTYPE_INT | CTLFLAG_RW,
	    &random_soft_processor.ra_seeded, 0, random_check_boolean, "I",
	    "Seeded State");

	SYSCTL_ADD_PROC(&random_clist,
	    SYSCTL_CHILDREN(random_sys_o),
	    OID_AUTO, "source_mask", CTLTYPE_UINT | CTLFLAG_RW,
	    &randomdev_harvest_source_mask, ((1<<RANDOM_ENVIRONMENTAL_END) - 1),
	    random_check_uint_harvestmask, "IU",
	    "Entropy harvesting mask");

	SYSCTL_ADD_PROC(&random_clist,
	    SYSCTL_CHILDREN(random_sys_o),
	    OID_AUTO, "source_mask_bin", CTLTYPE_STRING | CTLFLAG_RD,
	    NULL, 0, random_print_harvestmask, "A", "Entropy harvesting mask (printable)");

	/* Register the randomness processing routine */
#if defined(RANDOM_YARROW)
	random_harvestq_init(random_yarrow_process_event);
#endif
#if defined(RANDOM_FORTUNA)
	random_harvestq_init(random_fortuna_process_event);
#endif

	/* Register the randomness harvesting routine */
	randomdev_init_harvester(random_harvestq_internal,
	    random_soft_processor.ra_read);
}

void
randomdev_deinit(void)
{
	/* Deregister the randomness harvesting routine */
	randomdev_deinit_harvester();

	/*
	 * Command the hash/reseed thread to end and wait for it to finish
	 */
	random_kthread_control = -1;
	tsleep((void *)&random_kthread_control, 0, "term", 0);

#if defined(RANDOM_YARROW)
	random_yarrow_deinit_alg();
#endif
#if defined(RANDOM_FORTUNA)
	random_fortuna_deinit_alg();
#endif

	sysctl_ctx_free(&random_clist);
}

void
randomdev_unblock(void)
{
	if (!random_soft_processor.ra_seeded) {
		selwakeuppri(&rsel, PUSER);
		wakeup(&random_soft_processor);
                printf("random: unblocking device.\n");
		random_soft_processor.ra_seeded = 1;
	}
	/* Do arc4random(9) a favour while we are about it. */
	(void)atomic_cmpset_int(&arc4rand_iniseed_state, ARC4_ENTR_NONE,
	    ARC4_ENTR_HAVE);
}

static int
randomdev_poll(int events, struct thread *td)
{
	int revents = 0;

	mtx_lock(&random_reseed_mtx);

	if (random_soft_processor.ra_seeded)
		revents = events & (POLLIN | POLLRDNORM);
	else
		selrecord(td, &rsel);

	mtx_unlock(&random_reseed_mtx);
	return (revents);
}

static int
randomdev_block(int flag)
{
	int error = 0;

	mtx_lock(&random_reseed_mtx);

	/* Blocking logic */
	while (!random_soft_processor.ra_seeded && !error) {
		if (flag & O_NONBLOCK)
			error = EWOULDBLOCK;
		else {
			printf("random: blocking on read.\n");
			error = msleep(&random_soft_processor,
			    &random_reseed_mtx,
			    PUSER | PCATCH, "block", 0);
		}
	}
	mtx_unlock(&random_reseed_mtx);

	return (error);
}

/* Helper routine to perform explicit reseeds */
static void
randomdev_flush_reseed(void)
{
	/* Command a entropy queue flush and wait for it to finish */
	random_kthread_control = 1;
	while (random_kthread_control)
		pause("-", hz/10);

#if defined(RANDOM_YARROW)
	/* This ultimately calls randomdev_unblock() */
	random_yarrow_reseed();
#endif
#if defined(RANDOM_FORTUNA)
	/* This ultimately calls randomdev_unblock() */
	random_fortuna_reseed();
#endif
}

/* ARGSUSED */
static int
randomdev_soft_modevent(module_t mod __unused, int type, void *unused __unused)
{
	int error = 0;

	switch (type) {
	case MOD_LOAD:
		random_adaptor_register(RANDOM_CSPRNG_NAME, &random_soft_processor);
		break;

	case MOD_UNLOAD:
		random_adaptor_deregister(RANDOM_CSPRNG_NAME);
		break;

	case MOD_SHUTDOWN:
		break;

	default:
		error = EOPNOTSUPP;
		break;

	}
	return (error);
}

DEV_MODULE(RANDOM_MODULE_NAME, randomdev_soft_modevent, NULL);
MODULE_VERSION(RANDOM_MODULE_NAME, 1);
MODULE_DEPEND(RANDOM_MODULE_NAME, randomdev, 1, 1, 1);
