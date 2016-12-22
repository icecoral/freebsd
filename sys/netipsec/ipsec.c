/*	$FreeBSD$	*/
/*	$KAME: ipsec.c,v 1.103 2001/05/24 07:14:18 sakane Exp $	*/

/*-
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * IPsec controller part.
 */

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_ipsec.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/priv.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/errno.h>
#include <sys/hhook.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>
#include <sys/proc.h>

#include <net/if.h>
#include <net/if_enc.h>
#include <net/if_var.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/in_var.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <netinet/ip6.h>
#ifdef INET6
#include <netinet6/ip6_var.h>
#endif
#include <netinet/in_pcb.h>
#ifdef INET6
#include <netinet/icmp6.h>
#endif

#include <sys/types.h>
#include <netipsec/ipsec.h>
#ifdef INET6
#include <netipsec/ipsec6.h>
#endif
#include <netipsec/ah_var.h>
#include <netipsec/esp_var.h>
#include <netipsec/ipcomp.h>		/*XXX*/
#include <netipsec/ipcomp_var.h>
#include <netipsec/ipsec_support.h>

#include <netipsec/key.h>
#include <netipsec/keydb.h>
#include <netipsec/key_debug.h>

#include <netipsec/xform.h>

#include <machine/in_cksum.h>

#include <opencrypto/cryptodev.h>

#ifdef IPSEC_DEBUG
VNET_DEFINE(int, ipsec_debug) = 1;
#else
VNET_DEFINE(int, ipsec_debug) = 0;
#endif

/* NB: name changed so netstat doesn't use it. */
VNET_PCPUSTAT_DEFINE(struct ipsecstat, ipsec4stat);
VNET_PCPUSTAT_SYSINIT(ipsec4stat);

#ifdef VIMAGE
VNET_PCPUSTAT_SYSUNINIT(ipsec4stat);
#endif /* VIMAGE */

VNET_DEFINE(int, ip4_ah_offsetmask) = 0;	/* maybe IP_DF? */
/* DF bit on encap. 0: clear 1: set 2: copy */
VNET_DEFINE(int, ip4_ipsec_dfbit) = 0;
VNET_DEFINE(int, ip4_esp_trans_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip4_esp_net_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip4_ah_trans_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip4_ah_net_deflev) = IPSEC_LEVEL_USE;
/* ECN ignore(-1)/forbidden(0)/allowed(1) */
VNET_DEFINE(int, ip4_ipsec_ecn) = 0;
VNET_DEFINE(int, ip4_esp_randpad) = -1;

static VNET_DEFINE(int, ip4_filtertunnel) = 0;
#define	V_ip4_filtertunnel VNET(ip4_filtertunnel)
static VNET_DEFINE(int, check_policy_history) = 0;
#define	V_check_policy_history	VNET(check_policy_history)
static VNET_DEFINE(struct secpolicy, def_policy);
#define	V_def_policy	VNET(def_policy)
/*
 * Crypto support requirements:
 *
 *  1	require hardware support
 * -1	require software support
 *  0	take anything
 */
VNET_DEFINE(int, crypto_support) = CRYPTOCAP_F_HARDWARE | CRYPTOCAP_F_SOFTWARE;
/*
 * TCP/UDP checksum handling policy for transport mode NAT-T (RFC3948)
 *
 * 0 - incrementally recompute.
 * 1 - fully recompute TCP/UDP checksum.
 * 2 - for UDP reset checksum to zero; for TCP mark csum_flags as valid.
 */
VNET_DEFINE(int, natt_cksum_policy) = 0;

FEATURE(ipsec, "Internet Protocol Security (IPsec)");
FEATURE(ipsec_natt, "UDP Encapsulation of IPsec ESP Packets ('NAT-T')");

SYSCTL_DECL(_net_inet_ipsec);

/* net.inet.ipsec */
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DEF_POLICY, def_policy,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(def_policy).policy, 0,
	"IPsec default policy.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DEF_ESP_TRANSLEV, esp_trans_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_esp_trans_deflev), 0,
	"Default ESP transport mode level");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DEF_ESP_NETLEV, esp_net_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_esp_net_deflev), 0,
	"Default ESP tunnel mode level.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DEF_AH_TRANSLEV, ah_trans_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_ah_trans_deflev), 0,
	"AH transfer mode default level.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DEF_AH_NETLEV, ah_net_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_ah_net_deflev), 0,
	"AH tunnel mode default level.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_AH_CLEARTOS, ah_cleartos,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ah_cleartos), 0,
	"If set clear type-of-service field when doing AH computation.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_AH_OFFSETMASK, ah_offsetmask,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_ah_offsetmask), 0,
	"If not set clear offset field mask when doing AH computation.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DFBIT, dfbit,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_ipsec_dfbit), 0,
	"Do not fragment bit on encap.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_ECN, ecn,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_ipsec_ecn), 0,
	"Explicit Congestion Notification handling.");
SYSCTL_INT(_net_inet_ipsec, IPSECCTL_DEBUG, debug,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_debug), 0,
	"Enable IPsec debugging output when set.");
SYSCTL_INT(_net_inet_ipsec, OID_AUTO, crypto_support,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(crypto_support), 0,
	"Crypto driver selection.");
SYSCTL_INT(_net_inet_ipsec, OID_AUTO, check_policy_history,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(check_policy_history), 0,
	"Use strict check of inbound packets to security policy compliance.");
SYSCTL_INT(_net_inet_ipsec, OID_AUTO, natt_cksum_policy,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(natt_cksum_policy), 0,
	"Method to fix TCP/UDP checksum for transport mode IPsec after NAT.");
SYSCTL_INT(_net_inet_ipsec, OID_AUTO, filtertunnel,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip4_filtertunnel), 0,
	"If set filter packets from an IPsec tunnel.");
SYSCTL_VNET_PCPUSTAT(_net_inet_ipsec, OID_AUTO, ipsecstats, struct ipsecstat,
    ipsec4stat, "IPsec IPv4 statistics.");

#ifdef REGRESSION
/*
 * When set to 1, IPsec will send packets with the same sequence number.
 * This allows to verify if the other side has proper replay attacks detection.
 */
VNET_DEFINE(int, ipsec_replay) = 0;
SYSCTL_INT(_net_inet_ipsec, OID_AUTO, test_replay,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_replay), 0,
	"Emulate replay attack");
/*
 * When set 1, IPsec will send packets with corrupted HMAC.
 * This allows to verify if the other side properly detects modified packets.
 */
VNET_DEFINE(int, ipsec_integrity) = 0;
SYSCTL_INT(_net_inet_ipsec, OID_AUTO, test_integrity,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_integrity), 0,
	"Emulate man-in-the-middle attack");
#endif

#ifdef INET6 
VNET_PCPUSTAT_DEFINE(struct ipsecstat, ipsec6stat);
VNET_PCPUSTAT_SYSINIT(ipsec6stat);

#ifdef VIMAGE
VNET_PCPUSTAT_SYSUNINIT(ipsec6stat);
#endif /* VIMAGE */

VNET_DEFINE(int, ip6_esp_trans_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip6_esp_net_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip6_ah_trans_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip6_ah_net_deflev) = IPSEC_LEVEL_USE;
VNET_DEFINE(int, ip6_ipsec_ecn) = 0;	/* ECN ignore(-1)/forbidden(0)/allowed(1) */

static VNET_DEFINE(int, ip6_filtertunnel) = 0;
#define	V_ip6_filtertunnel	VNET(ip6_filtertunnel)

SYSCTL_DECL(_net_inet6_ipsec6);

/* net.inet6.ipsec6 */
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_DEF_POLICY, def_policy,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(def_policy).policy, 0,
	"IPsec default policy.");
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_DEF_ESP_TRANSLEV, esp_trans_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip6_esp_trans_deflev), 0,
	"Default ESP transport mode level.");
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_DEF_ESP_NETLEV, esp_net_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip6_esp_net_deflev), 0,
	"Default ESP tunnel mode level.");
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_DEF_AH_TRANSLEV, ah_trans_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip6_ah_trans_deflev), 0,
	"AH transfer mode default level.");
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_DEF_AH_NETLEV, ah_net_deflev,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip6_ah_net_deflev), 0,
	"AH tunnel mode default level.");
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_ECN, ecn,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip6_ipsec_ecn), 0,
	"Explicit Congestion Notification handling.");
SYSCTL_INT(_net_inet6_ipsec6, IPSECCTL_DEBUG, debug,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ipsec_debug), 0,
	"Enable IPsec debugging output when set.");
SYSCTL_INT(_net_inet6_ipsec6, OID_AUTO, filtertunnel,
	CTLFLAG_VNET | CTLFLAG_RW, &VNET_NAME(ip6_filtertunnel),  0,
	"If set filter packets from an IPsec tunnel.");
SYSCTL_VNET_PCPUSTAT(_net_inet6_ipsec6, IPSECCTL_STATS, ipsecstats,
    struct ipsecstat, ipsec6stat, "IPsec IPv6 statistics.");
#endif /* INET6 */

static int ipsec_in_reject(struct secpolicy *, struct inpcb *,
    const struct mbuf *);
static void ipsec_setspidx_inpcb(struct inpcb *, struct secpolicyindex *,
    u_int);

static void ipsec4_get_ulp(const struct mbuf *, struct secpolicyindex *, int);
static void ipsec4_setsockaddrs(const struct mbuf *, union sockaddr_union *,
    union sockaddr_union *);
static void ipsec4_setspidx_ipaddr(const struct mbuf *,
    struct secpolicyindex *);
#ifdef INET6
static void ipsec6_get_ulp(const struct mbuf *m, struct secpolicyindex *, int);
static void ipsec6_setsockaddrs(const struct mbuf *, union sockaddr_union *,
    union sockaddr_union *);
static void ipsec6_setspidx_ipaddr(const struct mbuf *,
    struct secpolicyindex *);
#endif

/*
 * Return a held reference to the default SP.
 */
static struct secpolicy *
key_allocsp_default(void)
{
	struct secpolicy *sp;

	sp = &V_def_policy;
	if (sp->policy != IPSEC_POLICY_DISCARD &&
	    sp->policy != IPSEC_POLICY_NONE) {
		ipseclog((LOG_INFO, "fixed system default policy: %d->%d\n",
		    sp->policy, IPSEC_POLICY_NONE));
		sp->policy = IPSEC_POLICY_NONE;
	}
	key_addref(sp);
	return (sp);
}

static struct secpolicy *
ipsec_checkpolicy(struct secpolicy *sp, struct inpcb *inp, int *error)
{
	uint32_t genid;

	if (inp != NULL && inp->inp_sp != NULL &&
	    (inp->inp_sp->flags & INP_OUTBOUND_POLICY) == 0 &&
	    inp->inp_sp->sp_out == NULL) {
		/*
		 * Save found OUTBOUND policy into PCB SP cache.
		 */
		genid = key_getspgen();
		inp->inp_sp->sp_out = sp;
		if (genid != inp->inp_sp->genid) {
			/* Reset INBOUND cached policy if genid is changed */
			if ((inp->inp_sp->flags & INP_INBOUND_POLICY) == 0)
				inp->inp_sp->sp_in = NULL;
			inp->inp_sp->genid = genid;
		}
		KEYDBG(IPSEC_STAMP,
		    printf("%s: PCB(%p): cached SP(%p)\n",
		    __func__, inp, sp));
	}
	switch (sp->policy) {
	default:
		printf("%s: invalid policy %u\n", __func__, sp->policy);
		/* FALLTHROUGH */
	case IPSEC_POLICY_DISCARD:
		*error = -EINVAL;	/* Packet is discarded by caller. */
		/* FALLTHROUGH */
	case IPSEC_POLICY_BYPASS:
	case IPSEC_POLICY_NONE:
		key_freesp(&sp);
		sp = NULL;		/* NB: force NULL result. */
		break;
	case IPSEC_POLICY_IPSEC:
		/* XXXAE: handle LARVAL SP */
		break;
	}
	KEYDBG(IPSEC_DUMP,
	    printf("%s: get SP(%p), error %d\n", __func__, sp, *error));
	return (sp);
}

static struct secpolicy *
ipsec_getpcbpolicy(struct inpcb *inp, u_int dir)
{
	struct secpolicy *sp;
	int flags;

	if (inp == NULL || inp->inp_sp == NULL)
		return (NULL);

	flags = inp->inp_sp->flags;
	if (dir == IPSEC_DIR_OUTBOUND) {
		sp = inp->inp_sp->sp_out;
		flags &= INP_OUTBOUND_POLICY;
	} else {
		sp = inp->inp_sp->sp_in;
		flags &= INP_INBOUND_POLICY;
	}
	/*
	 * Check flags. If we have PCB SP, just return it.
	 * Otherwise we need to check that cached SP entry isn't stale.
	 */
	if (flags == 0) {
		if (sp == NULL)
			return (NULL);
		if (inp->inp_sp->genid != key_getspgen()) {
			/*
			 * Invalidate the cache.
			 * Do not touch policy if it was set by PCB.
			 */
			if ((inp->inp_sp->flags & INP_INBOUND_POLICY) == 0)
				inp->inp_sp->sp_in = NULL;
			if ((inp->inp_sp->flags & INP_OUTBOUND_POLICY) == 0)
				inp->inp_sp->sp_out = NULL;
			return (NULL);
		}
		KEYDBG(IPSEC_STAMP,
		    printf("%s: PCB(%p): cache hit SP(%p)\n",
		    __func__, inp, sp));
		/* Return referenced cached policy */
	}
	IPSEC_ASSERT(sp != NULL, ("null SP, but flags is 0x%04x", flags));
	key_addref(sp);
	return (sp);
}

static void
ipsec_setsockaddrs_inpcb(struct inpcb *inp, union sockaddr_union *src,
    union sockaddr_union *dst, u_int dir)
{

#ifdef INET6
	if (inp->inp_vflag & INP_IPV6) {
		struct sockaddr_in6 *sin6;

		bzero(&src->sin6, sizeof(src->sin6));
		bzero(&dst->sin6, sizeof(dst->sin6));
		src->sin6.sin6_family = AF_INET6;
		src->sin6.sin6_len = sizeof(struct sockaddr_in6);
		dst->sin6.sin6_family = AF_INET6;
		dst->sin6.sin6_len = sizeof(struct sockaddr_in6);

		if (dir == IPSEC_DIR_OUTBOUND)
			sin6 = &src->sin6;
		else
			sin6 = &dst->sin6;
		sin6->sin6_addr = inp->in6p_laddr;
		sin6->sin6_port = inp->inp_lport;
		if (IN6_IS_SCOPE_LINKLOCAL(&inp->in6p_laddr)) {
			/* XXXAE: use in6p_zoneid */
			sin6->sin6_addr.s6_addr16[1] = 0;
			sin6->sin6_scope_id = ntohs(
			    inp->in6p_laddr.s6_addr16[1]);
		}

		if (dir == IPSEC_DIR_OUTBOUND)
			sin6 = &dst->sin6;
		else
			sin6 = &src->sin6;
		sin6->sin6_addr = inp->in6p_faddr;
		sin6->sin6_port = inp->inp_fport;
		if (IN6_IS_SCOPE_LINKLOCAL(&inp->in6p_faddr)) {
			/* XXXAE: use in6p_zoneid */
			sin6->sin6_addr.s6_addr16[1] = 0;
			sin6->sin6_scope_id = ntohs(
			    inp->in6p_faddr.s6_addr16[1]);
		}
	}
#endif
#ifdef INET
	if (inp->inp_vflag & INP_IPV4) {
		struct sockaddr_in *sin;

		bzero(&src->sin, sizeof(src->sin));
		bzero(&dst->sin, sizeof(dst->sin));
		src->sin.sin_family = AF_INET;
		src->sin.sin_len = sizeof(struct sockaddr_in);
		dst->sin.sin_family = AF_INET;
		dst->sin.sin_len = sizeof(struct sockaddr_in);

		if (dir == IPSEC_DIR_OUTBOUND)
			sin = &src->sin;
		else
			sin = &dst->sin;
		sin->sin_addr = inp->inp_laddr;
		sin->sin_port = inp->inp_lport;

		if (dir == IPSEC_DIR_OUTBOUND)
			sin = &dst->sin;
		else
			sin = &src->sin;
		sin->sin_addr = inp->inp_faddr;
		sin->sin_port = inp->inp_fport;
	}
#endif
}

static void
ipsec_setspidx_inpcb(struct inpcb *inp, struct secpolicyindex *spidx,
    u_int dir)
{

	ipsec_setsockaddrs_inpcb(inp, &spidx->src, &spidx->dst, dir);
#ifdef INET6
	if (inp->inp_vflag & INP_IPV6) {
		spidx->prefs = sizeof(struct in6_addr) << 3;
		spidx->prefd = sizeof(struct in6_addr) << 3;
	}
#endif
#ifdef INET
	if (inp->inp_vflag & INP_IPV4) {
		spidx->prefs = sizeof(struct in_addr) << 3;
		spidx->prefd = sizeof(struct in_addr) << 3;
	}
#endif
	spidx->ul_proto = inp->inp_ip_p;
	spidx->dir = dir;
	KEYDBG(IPSEC_DUMP,
	    printf("%s: ", __func__); kdebug_secpolicyindex(spidx, NULL));
}

void
ipsec_setsockaddrs(const struct mbuf *m, union sockaddr_union *src,
    union sockaddr_union *dst)
{
	struct ip *ip;

	IPSEC_ASSERT(m->m_len >= sizeof(*ip), ("unexpected mbuf len"));

	ip = mtod(m, struct ip *);
	switch (ip->ip_v) {
#ifdef INET
	case IPVERSION:
		ipsec4_setsockaddrs(m, src, dst);
		break;
#endif
#ifdef INET6
	case (IPV6_VERSION >> 4):
		ipsec6_setsockaddrs(m, src, dst);
		break;
#endif
	default:
		bzero(src, sizeof(*src));
		bzero(dst, sizeof(*dst));
	}
}

#ifdef INET
static void
ipsec4_setsockaddrs(const struct mbuf *m, union sockaddr_union *src,
    union sockaddr_union *dst)
{
	static const struct sockaddr_in template = {
		sizeof (struct sockaddr_in),
		AF_INET,
		0, { 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }
	};

	src->sin = template;
	dst->sin = template;

	if (m->m_len < sizeof (struct ip)) {
		m_copydata(m, offsetof(struct ip, ip_src),
			   sizeof (struct  in_addr),
			   (caddr_t) &src->sin.sin_addr);
		m_copydata(m, offsetof(struct ip, ip_dst),
			   sizeof (struct  in_addr),
			   (caddr_t) &dst->sin.sin_addr);
	} else {
		const struct ip *ip = mtod(m, const struct ip *);
		src->sin.sin_addr = ip->ip_src;
		dst->sin.sin_addr = ip->ip_dst;
	}
}

static void
ipsec4_get_ulp(const struct mbuf *m, struct secpolicyindex *spidx,
    int needport)
{
	uint8_t nxt;
	int off;

	/* Sanity check. */
	IPSEC_ASSERT(m->m_pkthdr.len >= sizeof(struct ip),
	    ("packet too short"));

	if (m->m_len >= sizeof (struct ip)) {
		const struct ip *ip = mtod(m, const struct ip *);
		if (ip->ip_off & htons(IP_MF | IP_OFFMASK))
			goto done;
		off = ip->ip_hl << 2;
		nxt = ip->ip_p;
	} else {
		struct ip ih;

		m_copydata(m, 0, sizeof (struct ip), (caddr_t) &ih);
		if (ih.ip_off & htons(IP_MF | IP_OFFMASK))
			goto done;
		off = ih.ip_hl << 2;
		nxt = ih.ip_p;
	}

	while (off < m->m_pkthdr.len) {
		struct ip6_ext ip6e;
		struct tcphdr th;
		struct udphdr uh;

		switch (nxt) {
		case IPPROTO_TCP:
			spidx->ul_proto = nxt;
			if (!needport)
				goto done_proto;
			if (off + sizeof(struct tcphdr) > m->m_pkthdr.len)
				goto done;
			m_copydata(m, off, sizeof (th), (caddr_t) &th);
			spidx->src.sin.sin_port = th.th_sport;
			spidx->dst.sin.sin_port = th.th_dport;
			return;
		case IPPROTO_UDP:
			spidx->ul_proto = nxt;
			if (!needport)
				goto done_proto;
			if (off + sizeof(struct udphdr) > m->m_pkthdr.len)
				goto done;
			m_copydata(m, off, sizeof (uh), (caddr_t) &uh);
			spidx->src.sin.sin_port = uh.uh_sport;
			spidx->dst.sin.sin_port = uh.uh_dport;
			return;
		case IPPROTO_AH:
			if (off + sizeof(ip6e) > m->m_pkthdr.len)
				goto done;
			/* XXX Sigh, this works but is totally bogus. */
			m_copydata(m, off, sizeof(ip6e), (caddr_t) &ip6e);
			off += (ip6e.ip6e_len + 2) << 2;
			nxt = ip6e.ip6e_nxt;
			break;
		case IPPROTO_ICMP:
		default:
			/* XXX Intermediate headers??? */
			spidx->ul_proto = nxt;
			goto done_proto;
		}
	}
done:
	spidx->ul_proto = IPSEC_ULPROTO_ANY;
done_proto:
	spidx->src.sin.sin_port = IPSEC_PORT_ANY;
	spidx->dst.sin.sin_port = IPSEC_PORT_ANY;
	KEYDBG(IPSEC_DUMP,
	    printf("%s: ", __func__); kdebug_secpolicyindex(spidx, NULL));
}

static void
ipsec4_setspidx_ipaddr(const struct mbuf *m, struct secpolicyindex *spidx)
{

	ipsec4_setsockaddrs(m, &spidx->src, &spidx->dst);
	spidx->prefs = sizeof(struct in_addr) << 3;
	spidx->prefd = sizeof(struct in_addr) << 3;
}

static struct secpolicy *
ipsec4_getpolicy(const struct mbuf *m, struct inpcb *inp, u_int dir)
{
	struct secpolicyindex spidx;
	struct secpolicy *sp;

	sp = ipsec_getpcbpolicy(inp, dir);
	if (sp == NULL && key_havesp(dir)) {
		/* Make an index to look for a policy. */
		ipsec4_setspidx_ipaddr(m, &spidx);
		/* Fill ports in spidx if we have inpcb. */
		ipsec4_get_ulp(m, &spidx, inp != NULL);
		spidx.dir = dir;
		sp = key_allocsp(&spidx, dir);
	}
	if (sp == NULL)		/* No SP found, use system default. */
		sp = key_allocsp_default();
	return (sp);
}

/*
 * Check security policy for *OUTBOUND* IPv4 packet.
 */
struct secpolicy *
ipsec4_checkpolicy(const struct mbuf *m, struct inpcb *inp, int *error)
{
	struct secpolicy *sp;

	*error = 0;
	sp = ipsec4_getpolicy(m, inp, IPSEC_DIR_OUTBOUND);
	if (sp != NULL)
		sp = ipsec_checkpolicy(sp, inp, error);
	if (sp == NULL) {
		switch (*error) {
		case 0: /* No IPsec required: BYPASS or NONE */
			break;
		case -EINVAL:
			IPSECSTAT_INC(ips_out_polvio);
			break;
		default:
			IPSECSTAT_INC(ips_out_inval);
		}
	}
	KEYDBG(IPSEC_STAMP,
	    printf("%s: using SP(%p), error %d\n", __func__, sp, *error));
	if (sp != NULL)
		KEYDBG(IPSEC_DATA, kdebug_secpolicy(sp));
	return (sp);
}

/*
 * Check IPv4 packet against *INBOUND* security policy.
 * This function is called from tcp_input(), udp_input(),
 * rip_input() and sctp_input().
 */
int
ipsec4_in_reject(const struct mbuf *m, struct inpcb *inp)
{
	struct secpolicy *sp;
	int result;

	sp = ipsec4_getpolicy(m, inp, IPSEC_DIR_INBOUND);
	result = ipsec_in_reject(sp, inp, m);
	key_freesp(&sp);
	if (result != 0)
		IPSECSTAT_INC(ips_in_polvio);
	return (result);
}

/*
 * IPSEC_CAP() method implementation for IPv4.
 */
int
ipsec4_capability(struct mbuf *m, u_int cap)
{

	switch (cap) {
	case IPSEC_CAP_BYPASS_FILTER:
		/*
		 * Bypass packet filtering for packets previously handled
		 * by IPsec.
		 */
		if (!V_ip4_filtertunnel &&
		    m_tag_find(m, PACKET_TAG_IPSEC_IN_DONE, NULL) != NULL)
			return (1);
		return (0);
	case IPSEC_CAP_OPERABLE:
		/* Do we have active security policies? */
		if (key_havesp(IPSEC_DIR_INBOUND) != 0 ||
		    key_havesp(IPSEC_DIR_OUTBOUND) != 0)
			return (1);
		return (0);
	};
	return (EOPNOTSUPP);
}

#endif /* INET */

#ifdef INET6
static void
ipsec6_setsockaddrs(const struct mbuf *m, union sockaddr_union *src,
    union sockaddr_union *dst)
{
	struct ip6_hdr ip6buf;
	const struct ip6_hdr *ip6;

	if (m->m_len >= sizeof(*ip6))
		ip6 = mtod(m, const struct ip6_hdr *);
	else {
		m_copydata(m, 0, sizeof(ip6buf), (caddr_t)&ip6buf);
		ip6 = &ip6buf;
	}

	bzero(&src->sin6, sizeof(struct sockaddr_in6));
	src->sin6.sin6_family = AF_INET6;
	src->sin6.sin6_len = sizeof(struct sockaddr_in6);
	bcopy(&ip6->ip6_src, &src->sin6.sin6_addr, sizeof(ip6->ip6_src));
	if (IN6_IS_SCOPE_LINKLOCAL(&ip6->ip6_src)) {
		src->sin6.sin6_addr.s6_addr16[1] = 0;
		src->sin6.sin6_scope_id = ntohs(ip6->ip6_src.s6_addr16[1]);
	}

	bzero(&dst->sin6, sizeof(struct sockaddr_in6));
	dst->sin6.sin6_family = AF_INET6;
	dst->sin6.sin6_len = sizeof(struct sockaddr_in6);
	bcopy(&ip6->ip6_dst, &dst->sin6.sin6_addr, sizeof(ip6->ip6_dst));
	if (IN6_IS_SCOPE_LINKLOCAL(&ip6->ip6_dst)) {
		dst->sin6.sin6_addr.s6_addr16[1] = 0;
		dst->sin6.sin6_scope_id = ntohs(ip6->ip6_dst.s6_addr16[1]);
	}
}

static void
ipsec6_get_ulp(const struct mbuf *m, struct secpolicyindex *spidx,
    int needport)
{
	struct tcphdr th;
	struct udphdr uh;
	struct icmp6_hdr ih;
	int off, nxt;

	IPSEC_ASSERT(m->m_pkthdr.len >= sizeof(struct ip6_hdr),
	    ("packet too short"));

	/* Set default. */
	spidx->ul_proto = IPSEC_ULPROTO_ANY;
	spidx->src.sin6.sin6_port = IPSEC_PORT_ANY;
	spidx->dst.sin6.sin6_port = IPSEC_PORT_ANY;

	nxt = -1;
	off = ip6_lasthdr(m, 0, IPPROTO_IPV6, &nxt);
	if (off < 0 || m->m_pkthdr.len < off)
		return;

	switch (nxt) {
	case IPPROTO_TCP:
		spidx->ul_proto = nxt;
		if (!needport)
			break;
		if (off + sizeof(struct tcphdr) > m->m_pkthdr.len)
			break;
		m_copydata(m, off, sizeof(th), (caddr_t)&th);
		spidx->src.sin6.sin6_port = th.th_sport;
		spidx->dst.sin6.sin6_port = th.th_dport;
		break;
	case IPPROTO_UDP:
		spidx->ul_proto = nxt;
		if (!needport)
			break;
		if (off + sizeof(struct udphdr) > m->m_pkthdr.len)
			break;
		m_copydata(m, off, sizeof(uh), (caddr_t)&uh);
		spidx->src.sin6.sin6_port = uh.uh_sport;
		spidx->dst.sin6.sin6_port = uh.uh_dport;
		break;
	case IPPROTO_ICMPV6:
		spidx->ul_proto = nxt;
		if (off + sizeof(struct icmp6_hdr) > m->m_pkthdr.len)
			break;
		m_copydata(m, off, sizeof(ih), (caddr_t)&ih);
		spidx->src.sin6.sin6_port = htons((uint16_t)ih.icmp6_type);
		spidx->dst.sin6.sin6_port = htons((uint16_t)ih.icmp6_code);
		break;
	default:
		/* XXX Intermediate headers??? */
		spidx->ul_proto = nxt;
		break;
	}
	KEYDBG(IPSEC_DUMP,
	    printf("%s: ", __func__); kdebug_secpolicyindex(spidx, NULL));
}

static void
ipsec6_setspidx_ipaddr(const struct mbuf *m, struct secpolicyindex *spidx)
{

	ipsec6_setsockaddrs(m, &spidx->src, &spidx->dst);
	spidx->prefs = sizeof(struct in6_addr) << 3;
	spidx->prefd = sizeof(struct in6_addr) << 3;
}

static struct secpolicy *
ipsec6_getpolicy(const struct mbuf *m, struct inpcb *inp, u_int dir)
{
	struct secpolicyindex spidx;
	struct secpolicy *sp;

	sp = ipsec_getpcbpolicy(inp, dir);
	if (sp == NULL && key_havesp(dir)) {
		/* Make an index to look for a policy. */
		ipsec6_setspidx_ipaddr(m, &spidx);
		/* Fill ports in spidx if we have inpcb. */
		ipsec6_get_ulp(m, &spidx, inp != NULL);
		spidx.dir = dir;
		sp = key_allocsp(&spidx, dir);
	}
	if (sp == NULL)		/* No SP found, use system default. */
		sp = key_allocsp_default();
	return (sp);
}

/*
 * Check security policy for *OUTBOUND* IPv6 packet.
 */
struct secpolicy *
ipsec6_checkpolicy(const struct mbuf *m, struct inpcb *inp, int *error)
{
	struct secpolicy *sp;

	*error = 0;
	sp = ipsec6_getpolicy(m, inp, IPSEC_DIR_OUTBOUND);
	if (sp != NULL)
		sp = ipsec_checkpolicy(sp, inp, error);
	if (sp == NULL) {
		switch (*error) {
		case 0: /* No IPsec required: BYPASS or NONE */
			break;
		case -EINVAL:
			IPSEC6STAT_INC(ips_out_polvio);
			break;
		default:
			IPSEC6STAT_INC(ips_out_inval);
		}
	}
	KEYDBG(IPSEC_STAMP,
	    printf("%s: using SP(%p), error %d\n", __func__, sp, *error));
	if (sp != NULL)
		KEYDBG(IPSEC_DATA, kdebug_secpolicy(sp));
	return (sp);
}

/*
 * Check IPv6 packet against inbound security policy.
 * This function is called from tcp6_input(), udp6_input(),
 * rip6_input() and sctp_input().
 */
int
ipsec6_in_reject(const struct mbuf *m, struct inpcb *inp)
{
	struct secpolicy *sp;
	int result;

	sp = ipsec6_getpolicy(m, inp, IPSEC_DIR_INBOUND);
	result = ipsec_in_reject(sp, inp, m);
	key_freesp(&sp);
	if (result)
		IPSEC6STAT_INC(ips_in_polvio);
	return (result);
}

/*
 * IPSEC_CAP() method implementation for IPv6.
 */
int
ipsec6_capability(struct mbuf *m, u_int cap)
{

	switch (cap) {
	case IPSEC_CAP_BYPASS_FILTER:
		/*
		 * Bypass packet filtering for packets previously handled
		 * by IPsec.
		 */
		if (!V_ip6_filtertunnel &&
		    m_tag_find(m, PACKET_TAG_IPSEC_IN_DONE, NULL) != NULL)
			return (1);
		return (0);
	case IPSEC_CAP_OPERABLE:
		/* Do we have active security policies? */
		if (key_havesp(IPSEC_DIR_INBOUND) != 0 ||
		    key_havesp(IPSEC_DIR_OUTBOUND) != 0)
			return (1);
		return (0);
	};
	return (EOPNOTSUPP);
}
#endif /* INET6 */

int
ipsec_run_hhooks(struct ipsec_ctx_data *ctx, int type)
{
	int idx;

	switch (ctx->af) {
#ifdef INET
	case AF_INET:
		idx = HHOOK_IPSEC_INET;
		break;
#endif
#ifdef INET6
	case AF_INET6:
		idx = HHOOK_IPSEC_INET6;
		break;
#endif
	default:
		return (EPFNOSUPPORT);
	}
	if (type == HHOOK_TYPE_IPSEC_IN)
		HHOOKS_RUN_IF(V_ipsec_hhh_in[idx], ctx, NULL);
	else
		HHOOKS_RUN_IF(V_ipsec_hhh_out[idx], ctx, NULL);
	if (*ctx->mp == NULL)
		return (EACCES);
	return (0);
}

struct ipsecrequest *
ipsec_newisr(void)
{

	return (malloc(sizeof(struct ipsecrequest), M_IPSEC_SR,
	    M_NOWAIT | M_ZERO));
}

void
ipsec_delisr(struct ipsecrequest *p)
{

	free(p, M_IPSEC_SR);
}

/*
 * Return current level.
 * Either IPSEC_LEVEL_USE or IPSEC_LEVEL_REQUIRE are always returned.
 */
u_int
ipsec_get_reqlevel(struct secpolicy *sp, u_int idx)
{
	struct ipsecrequest *isr;
	u_int esp_trans_deflev, esp_net_deflev;
	u_int ah_trans_deflev, ah_net_deflev;
	u_int level = 0;

	IPSEC_ASSERT(idx < sp->tcount, ("Wrong IPsec request index %d", idx));
/* XXX Note that we have ipseclog() expanded here - code sync issue. */
#define IPSEC_CHECK_DEFAULT(lev) \
	(((lev) != IPSEC_LEVEL_USE && (lev) != IPSEC_LEVEL_REQUIRE &&	\
	  (lev) != IPSEC_LEVEL_UNIQUE)					\
		? (V_ipsec_debug  ?					\
		log(LOG_INFO, "fixed system default level " #lev ":%d->%d\n",\
		(lev), IPSEC_LEVEL_REQUIRE) : 0),			\
		(lev) = IPSEC_LEVEL_REQUIRE, (lev) : (lev))

	/*
	 * IPsec VTI uses unique security policy with fake spidx filled
	 * with zeroes. Just return IPSEC_LEVEL_REQUIRE instead of doing
	 * full level lookup for such policies.
	 */
	if (sp->state == IPSEC_SPSTATE_IFNET) {
		IPSEC_ASSERT(sp->req[idx]->level == IPSEC_LEVEL_UNIQUE,
		    ("Wrong IPsec request level %d", sp->req[idx]->level));
		return (IPSEC_LEVEL_REQUIRE);
	}

	/* Set default level. */
	switch (sp->spidx.src.sa.sa_family) {
#ifdef INET
	case AF_INET:
		esp_trans_deflev = IPSEC_CHECK_DEFAULT(V_ip4_esp_trans_deflev);
		esp_net_deflev = IPSEC_CHECK_DEFAULT(V_ip4_esp_net_deflev);
		ah_trans_deflev = IPSEC_CHECK_DEFAULT(V_ip4_ah_trans_deflev);
		ah_net_deflev = IPSEC_CHECK_DEFAULT(V_ip4_ah_net_deflev);
		break;
#endif
#ifdef INET6
	case AF_INET6:
		esp_trans_deflev = IPSEC_CHECK_DEFAULT(V_ip6_esp_trans_deflev);
		esp_net_deflev = IPSEC_CHECK_DEFAULT(V_ip6_esp_net_deflev);
		ah_trans_deflev = IPSEC_CHECK_DEFAULT(V_ip6_ah_trans_deflev);
		ah_net_deflev = IPSEC_CHECK_DEFAULT(V_ip6_ah_net_deflev);
		break;
#endif /* INET6 */
	default:
		panic("%s: unknown af %u",
			__func__, sp->spidx.src.sa.sa_family);
	}

#undef IPSEC_CHECK_DEFAULT

	isr = sp->req[idx];
	/* Set level. */
	switch (isr->level) {
	case IPSEC_LEVEL_DEFAULT:
		switch (isr->saidx.proto) {
		case IPPROTO_ESP:
			if (isr->saidx.mode == IPSEC_MODE_TUNNEL)
				level = esp_net_deflev;
			else
				level = esp_trans_deflev;
			break;
		case IPPROTO_AH:
			if (isr->saidx.mode == IPSEC_MODE_TUNNEL)
				level = ah_net_deflev;
			else
				level = ah_trans_deflev;
			break;
		case IPPROTO_IPCOMP:
			/*
			 * We don't really care, as IPcomp document says that
			 * we shouldn't compress small packets.
			 */
			level = IPSEC_LEVEL_USE;
			break;
		default:
			panic("%s: Illegal protocol defined %u\n", __func__,
				isr->saidx.proto);
		}
		break;

	case IPSEC_LEVEL_USE:
	case IPSEC_LEVEL_REQUIRE:
		level = isr->level;
		break;
	case IPSEC_LEVEL_UNIQUE:
		level = IPSEC_LEVEL_REQUIRE;
		break;

	default:
		panic("%s: Illegal IPsec level %u\n", __func__, isr->level);
	}

	return (level);
}

static int
ipsec_check_history(const struct mbuf *m, struct secpolicy *sp, u_int idx)
{
	struct xform_history *xh;
	struct m_tag *mtag;

	mtag = NULL;
	while ((mtag = m_tag_find(__DECONST(struct mbuf *, m),
	    PACKET_TAG_IPSEC_IN_DONE, mtag)) != NULL) {
		xh = (struct xform_history *)(mtag + 1);
		KEYDBG(IPSEC_DATA,
		    char buf[IPSEC_ADDRSTRLEN];
		    printf("%s: mode %s proto %u dst %s\n", __func__,
			kdebug_secasindex_mode(xh->mode), xh->proto,
			ipsec_address(&xh->dst, buf, sizeof(buf))));
		if (xh->proto != sp->req[idx]->saidx.proto)
			continue;
		/* If SA had IPSEC_MODE_ANY, consider this as match. */
		if (xh->mode != sp->req[idx]->saidx.mode &&
		    xh->mode != IPSEC_MODE_ANY)
			continue;
		/*
		 * For transport mode IPsec request doesn't contain
		 * addresses. We need to use address from spidx.
		 */
		if (sp->req[idx]->saidx.mode == IPSEC_MODE_TRANSPORT) {
			if (key_sockaddrcmp_withmask(&xh->dst.sa,
			    &sp->spidx.dst.sa, sp->spidx.prefd) != 0)
				continue;
		} else {
			if (key_sockaddrcmp(&xh->dst.sa,
			    &sp->req[idx]->saidx.dst.sa, 0) != 0)
				continue;
		}
		return (0); /* matched */
	}
	return (1);
}

/*
 * Check security policy requirements against the actual
 * packet contents.  Return one if the packet should be
 * reject as "invalid"; otherwiser return zero to have the
 * packet treated as "valid".
 *
 * OUT:
 *	0: valid
 *	1: invalid
 */
static int
ipsec_in_reject(struct secpolicy *sp, struct inpcb *inp, const struct mbuf *m)
{
	uint32_t genid;
	int i;

	KEYDBG(IPSEC_STAMP,
	    printf("%s: PCB(%p): using SP(%p)\n", __func__, inp, sp));
	KEYDBG(IPSEC_DATA, kdebug_secpolicy(sp));

	if (inp != NULL &&
	    (inp->inp_sp->flags & INP_INBOUND_POLICY) == 0 &&
	    inp->inp_sp->sp_in == NULL &&
	    inp->inp_laddr.s_addr != INADDR_ANY) {
		/*
		 * Save found INBOUND policy into PCB SP cache.
		 * NOTE: We do this only if local address isn't INADDR_ANY,
		 * because a cached policy for listen socket, that bound to
		 * ANY address, may prevent to establish another connection.
		 * We don't check address family, since both INADDR_ANY and
		 * UNSPECIFIED IPv6 address contains all zeroes.
		 */
		genid = key_getspgen();
		inp->inp_sp->sp_in = sp;
		if (genid != inp->inp_sp->genid) {
			/* Reset OUTBOUND cached policy if genid is changed */
			if ((inp->inp_sp->flags & INP_OUTBOUND_POLICY) == 0)
				inp->inp_sp->sp_out = NULL;
			inp->inp_sp->genid = genid;
		}
		KEYDBG(IPSEC_STAMP,
		    printf("%s: PCB(%p): cached SP(%p)\n",
		    __func__, inp, sp));
	}
	/* Check policy. */
	switch (sp->policy) {
	case IPSEC_POLICY_DISCARD:
		return (1);
	case IPSEC_POLICY_BYPASS:
	case IPSEC_POLICY_NONE:
		return (0);
	}

	IPSEC_ASSERT(sp->policy == IPSEC_POLICY_IPSEC,
		("invalid policy %u", sp->policy));

	/*
	 * ipsec[46]_common_input_cb after each transform adds
	 * PACKET_TAG_IPSEC_IN_DONE mbuf tag. It contains SPI, proto, mode
	 * and destination address from saidx. We can compare info from
	 * these tags with requirements in SP.
	 */
	for (i = 0; i < sp->tcount; i++) {
		/*
		 * Do not check IPcomp, since IPcomp document
		 * says that we shouldn't compress small packets.
		 * IPComp policy should always be treated as being
		 * in "use" level.
		 */
		if (sp->req[i]->saidx.proto == IPPROTO_IPCOMP ||
		    ipsec_get_reqlevel(sp, i) != IPSEC_LEVEL_REQUIRE)
			continue;
		if (V_check_policy_history != 0 &&
		    ipsec_check_history(m, sp, i) != 0)
			return (1);
		else switch (sp->req[i]->saidx.proto) {
		case IPPROTO_ESP:
			if ((m->m_flags & M_DECRYPTED) == 0) {
				KEYDBG(IPSEC_DUMP,
				    printf("%s: ESP m_flags:%x\n", __func__,
					    m->m_flags));
				return (1);
			}
			break;
		case IPPROTO_AH:
			if ((m->m_flags & M_AUTHIPHDR) == 0) {
				KEYDBG(IPSEC_DUMP,
				    printf("%s: AH m_flags:%x\n", __func__,
					    m->m_flags));
				return (1);
			}
			break;
		}
	}
	return (0);		/* Valid. */
}

/*
 * Compute the byte size to be occupied by IPsec header.
 * In case it is tunnelled, it includes the size of outer IP header.
 */
static size_t
ipsec_hdrsiz_internal(struct secpolicy *sp)
{
	size_t size;
	int i;

	KEYDBG(IPSEC_STAMP, printf("%s: using SP(%p)\n", __func__, sp));
	KEYDBG(IPSEC_DATA, kdebug_secpolicy(sp));

	switch (sp->policy) {
	case IPSEC_POLICY_DISCARD:
	case IPSEC_POLICY_BYPASS:
	case IPSEC_POLICY_NONE:
		return (0);
	}

	IPSEC_ASSERT(sp->policy == IPSEC_POLICY_IPSEC,
		("invalid policy %u", sp->policy));

	/*
	 * XXX: for each transform we need to lookup suitable SA
	 * and use info from SA to calculate headers size.
	 * XXX: for NAT-T we need to cosider UDP header size.
	 */
	size = 0;
	for (i = 0; i < sp->tcount; i++) {
		switch (sp->req[i]->saidx.proto) {
		case IPPROTO_ESP:
			size += esp_hdrsiz(NULL);
			break;
		case IPPROTO_AH:
			size += ah_hdrsiz(NULL);
			break;
		case IPPROTO_IPCOMP:
			size += sizeof(struct ipcomp);
			break;
		}

		if (sp->req[i]->saidx.mode == IPSEC_MODE_TUNNEL) {
			switch (sp->req[i]->saidx.dst.sa.sa_family) {
#ifdef INET
			case AF_INET:
				size += sizeof(struct ip);
				break;
#endif
#ifdef INET6
			case AF_INET6:
				size += sizeof(struct ip6_hdr);
				break;
#endif
			default:
				ipseclog((LOG_ERR, "%s: unknown AF %d in "
				    "IPsec tunnel SA\n", __func__,
				    sp->req[i]->saidx.dst.sa.sa_family));
				break;
			}
		}
	}
	return (size);
}

/*
 * Compute ESP/AH header size for protocols with PCB, including
 * outer IP header. Currently only tcp_output() uses it.
 */
size_t
ipsec_hdrsiz_inpcb(struct inpcb *inp)
{
	struct secpolicyindex spidx;
	struct secpolicy *sp;
	size_t sz;

	sp = ipsec_getpcbpolicy(inp, IPSEC_DIR_OUTBOUND);
	if (sp == NULL && key_havesp(IPSEC_DIR_OUTBOUND)) {
		ipsec_setspidx_inpcb(inp, &spidx, IPSEC_DIR_OUTBOUND);
		sp = key_allocsp(&spidx, IPSEC_DIR_OUTBOUND);
	}
	if (sp == NULL)
		sp = key_allocsp_default();
	sz = ipsec_hdrsiz_internal(sp);
	key_freesp(&sp);
	return (sz);
}

/*
 * Check the variable replay window.
 * ipsec_chkreplay() performs replay check before ICV verification.
 * ipsec_updatereplay() updates replay bitmap.  This must be called after
 * ICV verification (it also performs replay check, which is usually done
 * beforehand).
 * 0 (zero) is returned if packet disallowed, 1 if packet permitted.
 *
 * Based on RFC 6479. Blocks are 32 bits unsigned integers
 */

#define IPSEC_BITMAP_INDEX_MASK(w)	(w - 1)
#define IPSEC_REDUNDANT_BIT_SHIFTS	5
#define IPSEC_REDUNDANT_BITS		(1 << IPSEC_REDUNDANT_BIT_SHIFTS)
#define IPSEC_BITMAP_LOC_MASK		(IPSEC_REDUNDANT_BITS - 1)

int
ipsec_chkreplay(uint32_t seq, struct secasvar *sav)
{
	const struct secreplay *replay;
	uint32_t wsizeb;		/* Constant: window size. */
	int index, bit_location;

	IPSEC_ASSERT(sav != NULL, ("Null SA"));
	IPSEC_ASSERT(sav->replay != NULL, ("Null replay state"));

	replay = sav->replay;

	/* No need to check replay if disabled. */
	if (replay->wsize == 0)
		return (1);

	/* Constant. */
	wsizeb = replay->wsize << 3;

	/* Sequence number of 0 is invalid. */
	if (seq == 0)
		return (0);

	/* First time is always okay. */
	if (replay->count == 0)
		return (1);

	/* Larger sequences are okay. */
	if (seq > replay->lastseq)
		return (1);

	/* Over range to check, i.e. too old or wrapped. */
	if (replay->lastseq - seq >= wsizeb)
		return (0);

	/* The sequence is inside the sliding window
	 * now check the bit in the bitmap
	 * bit location only depends on the sequence number
	 */
	bit_location = seq & IPSEC_BITMAP_LOC_MASK;
	index = (seq >> IPSEC_REDUNDANT_BIT_SHIFTS)
		& IPSEC_BITMAP_INDEX_MASK(replay->bitmap_size);

	/* This packet already seen? */
	if ((replay->bitmap)[index] & (1 << bit_location))
		return (0);
	return (1);
}

/*
 * Check replay counter whether to update or not.
 * OUT:	0:	OK
 *	1:	NG
 */
int
ipsec_updatereplay(uint32_t seq, struct secasvar *sav)
{
	char buf[128];
	struct secreplay *replay;
	uint32_t wsizeb;		/* Constant: window size. */
	int diff, index, bit_location;

	IPSEC_ASSERT(sav != NULL, ("Null SA"));
	IPSEC_ASSERT(sav->replay != NULL, ("Null replay state"));

	replay = sav->replay;

	if (replay->wsize == 0)
		goto ok;	/* No need to check replay. */

	/* Constant. */
	wsizeb = replay->wsize << 3;

	/* Sequence number of 0 is invalid. */
	if (seq == 0)
		return (1);

	/* The packet is too old, no need to update */
	if (wsizeb + seq < replay->lastseq)
		goto ok;

	/* Now update the bit */
	index = (seq >> IPSEC_REDUNDANT_BIT_SHIFTS);

	/* First check if the sequence number is in the range */
	if (seq > replay->lastseq) {
		int id;
		int index_cur = replay->lastseq >> IPSEC_REDUNDANT_BIT_SHIFTS;

		diff = index - index_cur;
		if (diff > replay->bitmap_size) {
			/* something unusual in this case */
			diff = replay->bitmap_size;
		}

		for (id = 0; id < diff; ++id) {
			replay->bitmap[(id + index_cur + 1)
			& IPSEC_BITMAP_INDEX_MASK(replay->bitmap_size)] = 0;
		}

		replay->lastseq = seq;
	}

	index &= IPSEC_BITMAP_INDEX_MASK(replay->bitmap_size);
	bit_location = seq & IPSEC_BITMAP_LOC_MASK;

	/* this packet has already been received */
	if (replay->bitmap[index] & (1 << bit_location))
		return (1);

	replay->bitmap[index] |= (1 << bit_location);

ok:
	if (replay->count == ~0) {

		/* Set overflow flag. */
		replay->overflow++;

		/* Don't increment, no more packets accepted. */
		if ((sav->flags & SADB_X_EXT_CYCSEQ) == 0) {
			if (sav->sah->saidx.proto == IPPROTO_AH)
				AHSTAT_INC(ahs_wrap);
			else if (sav->sah->saidx.proto == IPPROTO_ESP)
				ESPSTAT_INC(esps_wrap);
			return (1);
		}

		ipseclog((LOG_WARNING, "%s: replay counter made %d cycle. %s\n",
		    __func__, replay->overflow,
		    ipsec_logsastr(sav, buf, sizeof(buf))));
	}
	return (0);
}

int
ipsec_updateid(struct secasvar *sav, uint64_t *new, uint64_t *old)
{
	uint64_t tmp;

	/*
	 * tdb_cryptoid is initialized by xform_init().
	 * Then it can be changed only when some crypto error occurred or
	 * when SA is deleted. We stored used cryptoid in the xform_data
	 * structure. In case when crypto error occurred and crypto
	 * subsystem has reinited the session, it returns new cryptoid
	 * and EAGAIN error code.
	 *
	 * This function will be called when we got EAGAIN from crypto
	 * subsystem.
	 * *new is cryptoid that was returned by crypto subsystem in
	 * the crp_sid.
	 * *old is the original cryptoid that we stored in xform_data.
	 *
	 * For first failed request *old == sav->tdb_cryptoid, then
	 * we update sav->tdb_cryptoid and redo crypto_dispatch().
	 * For next failed request *old != sav->tdb_cryptoid, then
	 * we store cryptoid from first request into the *new variable
	 * and crp_sid from this second session will be returned via
	 * *old pointer, so caller can release second session.
	 *
	 * XXXAE: check this more carefully.
	 */
	KEYDBG(IPSEC_STAMP,
	    printf("%s: SA(%p) moves cryptoid %jd -> %jd\n",
		__func__, sav, (uintmax_t)(*old), (uintmax_t)(*new)));
	KEYDBG(IPSEC_DATA, kdebug_secasv(sav));
	SECASVAR_LOCK(sav);
	if (sav->tdb_cryptoid != *old) {
		/* cryptoid was already updated */
		tmp = *new;
		*new = sav->tdb_cryptoid;
		*old = tmp;
		SECASVAR_UNLOCK(sav);
		return (1);
	}
	sav->tdb_cryptoid = *new;
	SECASVAR_UNLOCK(sav);
	return (0);
}

/* Return a printable string for the address. */
char*
ipsec_address(const union sockaddr_union* sa, char *buf, socklen_t size)
{

	switch (sa->sa.sa_family) {
#ifdef INET
	case AF_INET:
		return (inet_ntop(AF_INET, &sa->sin.sin_addr, buf, size));
#endif /* INET */
#ifdef INET6
	case AF_INET6:
		if (IN6_IS_SCOPE_LINKLOCAL(&sa->sin6.sin6_addr)) {
			snprintf(buf, size, "%s%%%u", inet_ntop(AF_INET6,
			    &sa->sin6.sin6_addr, buf, size),
			    sa->sin6.sin6_scope_id);
			return (buf);
		} else
			return (inet_ntop(AF_INET6, &sa->sin6.sin6_addr,
			    buf, size));
#endif /* INET6 */
	case 0:
		return ("*");
	default:
		return ("(unknown address family)");
	}
}

char *
ipsec_logsastr(struct secasvar *sav, char *buf, size_t size)
{
	char sbuf[IPSEC_ADDRSTRLEN], dbuf[IPSEC_ADDRSTRLEN];

	IPSEC_ASSERT(sav->sah->saidx.src.sa.sa_family ==
	    sav->sah->saidx.dst.sa.sa_family, ("address family mismatch"));

	snprintf(buf, size, "SA(SPI=%08lx src=%s dst=%s)",
	    (u_long)ntohl(sav->spi),
	    ipsec_address(&sav->sah->saidx.src, sbuf, sizeof(sbuf)),
	    ipsec_address(&sav->sah->saidx.dst, dbuf, sizeof(dbuf)));
	return (buf);
}

void
ipsec_dumpmbuf(const struct mbuf *m)
{
	const u_char *p;
	int totlen;
	int i;

	totlen = 0;
	printf("---\n");
	while (m) {
		p = mtod(m, const u_char *);
		for (i = 0; i < m->m_len; i++) {
			printf("%02x ", p[i]);
			totlen++;
			if (totlen % 16 == 0)
				printf("\n");
		}
		m = m->m_next;
	}
	if (totlen % 16 != 0)
		printf("\n");
	printf("---\n");
}

static void
def_policy_init(const void *unused __unused)
{

	bzero(&V_def_policy, sizeof(struct secpolicy));
	V_def_policy.policy = IPSEC_POLICY_NONE;
	V_def_policy.refcnt = 1;
}
VNET_SYSINIT(def_policy_init, SI_SUB_PROTO_DOMAIN, SI_ORDER_FIRST,
    def_policy_init, NULL);
