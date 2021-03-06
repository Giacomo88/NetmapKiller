#include "everything.h"
#include "sender.h"

void *receiver_body(void *data);

/* very crude code to print a number in normalized form.
 * Caller has to make sure that the buffer is large enough.
 */
static const char *
norm(char *buf, double val)
{
	char *units[] = { "", "K", "M", "G", "T" };
	u_int i;

	for (i = 0; val >=1000 && i < sizeof(units)/sizeof(char *) - 1; i++)
		val /= 1000;
	sprintf(buf, "%.2f %s", val, units[i]);
	return buf;
}

static void
tx_output(uint64_t sent, int size, double delta)
{
	double bw, raw_bw, pps;
	char b1[40], b2[80], b3[80];

	printf("Sent %llu packets, %d bytes each, in %.2f seconds.\n",
			(unsigned long long)sent, size, delta);
	if (delta == 0)
		delta = 1e-6;
	if (size < 60)		/* correct for min packet size */
		size = 60;
	pps = sent / delta;
	bw = (8.0 * size * sent) / delta;
	/* raw packets have4 bytes crc + 20 bytes framing */
	raw_bw = (8.0 * (size + 24) * sent) / delta;

	printf("Speed: %spps Bandwidth: %sbps (raw %sbps)\n",
			norm(b1, pps), norm(b2, bw), norm(b3, raw_bw) );
}

static void
rx_output(uint64_t received, double delta)
{
	double pps;
	char b1[40];

	printf("Received %llu packets, in %.2f seconds.\n",
			(unsigned long long) received, delta);

	if (delta == 0)
		delta = 1e-6;
	pps = received / delta;
	printf("Speed: %spps\n", norm(b1, pps));
}

static __inline int
timespec_ge(const struct timespec *a, const struct timespec *b)
{

	if (a->tv_sec > b->tv_sec)
		return (1);
	if (a->tv_sec < b->tv_sec)
		return (0);
	if (a->tv_nsec >= b->tv_nsec)
		return (1);
	return (0);
}

static __inline struct timespec
timeval2spec(const struct timeval *a)
{
	struct timespec ts = {
			.tv_sec = a->tv_sec,
			.tv_nsec = a->tv_usec * 1000
	};
	return ts;
}

static __inline struct timeval
timespec2val(const struct timespec *a)
{
	struct timeval tv = {
			.tv_sec = a->tv_sec,
			.tv_usec = a->tv_nsec / 1000
	};
	return tv;
}

void
main_thread(struct glob_arg *g, struct targ *targs)
{
	int i;
	int done;
	uint64_t pps, usec, my_count, npkts;
	uint64_t prev = 0;
	uint64_t count = 0;
	double delta_t;
	struct timeval tic, toc;

	gettimeofday(&toc, NULL);
	for (;;) {
		struct timeval now, delta;
		done = 0;

		delta.tv_sec = g->report_interval/1000;
		delta.tv_usec = (g->report_interval%1000)*1000;
		select(0, NULL, NULL, NULL, &delta);
		gettimeofday(&now, NULL);
		timersub(&now, &toc, &toc);
		my_count = 0;
		for (i = 0; i < g->nthreads; i++) {
			my_count += targs[i].count;
			if (targs[i].used == 0)
				done++;
		}
		usec = toc.tv_sec* 1000000 + toc.tv_usec;
		if (usec < 10000)
			continue;
		npkts = my_count - prev;
		pps = (npkts*1000000 + usec/2) / usec;
		D("%llu pps (%llu pkts in %llu usec)",
				(unsigned long long)pps,
				(unsigned long long)npkts,
				(unsigned long long)usec);
		prev = my_count;
		toc = now;
		if (done == g->nthreads)
			break;
	}

	timerclear(&tic);
	timerclear(&toc);
	for (i = 0; i < g->nthreads; i++) {
		struct timespec t_tic, t_toc;
		/*
		 * Join active threads, unregister interfaces and close
		 * file descriptors.
		 */
		if (targs[i].used)
			pthread_join(targs[i].thread, NULL);
		close(targs[i].fd);

		if (targs[i].completed == 0)
			D("ouch, thread %d exited with error", i);

		/*
		 * Collect threads output and extract information about
		 * how long it took to send all the packets.
		 */
		count += targs[i].count;
		t_tic = timeval2spec(&tic);
		t_toc = timeval2spec(&toc);
		if (!timerisset(&tic) || timespec_ge(&targs[i].tic, &t_tic))
			tic = timespec2val(&targs[i].tic);
		if (!timerisset(&toc) || timespec_ge(&targs[i].toc, &t_toc))
			toc = timespec2val(&targs[i].toc);
	}

	/* print output. */
	timersub(&toc, &tic, &toc);
	delta_t = toc.tv_sec + 1e-6* toc.tv_usec;
	if (g->td_body == sender_body)
		tx_output(count, g->pkt_size, delta_t);
	else
		rx_output(count, delta_t);

	if (g->dev_type == DEV_NETMAP) {
		munmap(g->nmd->mem, g->nmd->req.nr_memsize);
		close(g->main_fd);
	}
}

void
start_threads(struct glob_arg *g, struct targ *targs)
{
	int i, idx;
	uint64_t nmd_flags;
	 
	/*
	 * Now create the desired number of threads, each one
	 * using a single descriptor.
	 */
	for (i = 0; i < g->nthreads; i++) {
		struct targ *t = &targs[i];

		bzero(t, sizeof(*t));
		t->fd = -1; /* default, with pcap */
		t->g = g;

		if (g->dev_type == DEV_NETMAP) {
			struct nm_desc nmd = *g->nmd; /* copy, we overwrite ringid */
			nmd_flags = 0;
			nmd.self = &nmd;

			if (g->nthreads > 1) {
				if (nmd.req.nr_flags != NR_REG_ALL_NIC) {
					D("invalid nthreads mode %d", nmd.req.nr_flags);
					continue;
				}
				nmd.req.nr_flags = NR_REG_ONE_NIC;
				nmd.req.nr_ringid = i;
			}
			/* Only touch one of the rings (rx is already ok) */
			if (g->td_body == receiver_body)
				nmd_flags |= NETMAP_NO_TX_POLL;

			/* register interface. Override ifname and ringid etc. */
			if (g->options & OPT_MONITOR_TX)
				nmd.req.nr_flags |= NR_MONITOR_TX;
			if (g->options & OPT_MONITOR_RX)
				nmd.req.nr_flags |= NR_MONITOR_RX;

			t->nmd = nm_open(t->g->ifname, NULL, nmd_flags |
					NM_OPEN_IFNAME | NM_OPEN_NO_MMAP, &nmd);
			if (t->nmd == NULL) {
				D("Unable to open %s: %s",
						t->g->ifname, strerror(errno));
				continue;
			}
			t->fd = t->nmd->fd;

		} else {
			targs[i].fd = g->main_fd;
		}
		t->used = 1;
		t->me = i;
		if (g->affinity >= 0) {
			if (g->affinity < g->cpus)
				t->affinity = g->affinity;
			else
				t->affinity = i % g->cpus;
		} else {
			t->affinity = -1;
		}

		idx = 0;
		while (g->pkt_map[idx].key != NULL) {
			if (strcmp(g->pkt_map[idx].key, g->mode) == 0) break;
			idx++;
		}

		int (*ptrf) (struct targ *targs);
		ptrf = g->pkt_map[idx].f_init;

		if (ptrf(t) < 0) D("initialize failure");
		else {
			if (pthread_create(&t->thread, NULL, g->td_body, t) == -1) {
				D("Unable to create thread %d: %s", i, strerror(errno));
				t->used = 0;
			}
		}
	}
	
	/* start main_thread */
	main_thread(g, targs);
}
