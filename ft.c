/*
 * ft.c - scp-style file copy over UDP with BBR congestion control.
 *
 *   ft [user@]host:path  local_path          download
 *   ft local_path  [user@]host:path          upload
 *
 * Needs only what scp needs: sshd reachable on the other machine (port 22)
 * and this program installed there. Nothing has to be running beforehand and
 * no firewall/router setting changes:
 *
 *   1. ssh starts "ft --serve ..." on the remote machine, on demand.
 *   2. Over that ssh session both ends exchange their UDP addresses (public
 *      address from STUN, plus the LAN address seen by sshd).
 *   3. Both ends then send UDP to each other at the same time. Outbound
 *      packets open the NAT / stateful firewall for the replies ("hole
 *      punching"), so no inbound port is ever opened.
 *   4. The file travels over that UDP path; ssh is only the control channel.
 *
 * Wire format: a 16-byte header {type, a, b, ts} + up to CHUNK bytes of data.
 *   DATA  a = chunk number, b = total chunks, ts = sender's clock
 *   ACK   a = chunk just received, b = cumulative ack (all chunks < b held),
 *         ts = echo of the DATA's ts (exact RTT, even for retransmissions)
 *   PUNCH opens NAT / firewall state and confirms the path
 *   FIN   sender -> receiver: everything was acknowledged, you can exit
 *
 * Sender: every chunk is ACKed individually; chunks are declared lost when a
 * chunk sent later has been ACKed (RACK-style) or on timeout, then resent.
 * BBR (Cardwell et al.) models the path as bottleneck bandwidth x min RTT
 * and paces packets at that rate instead of reacting to loss.
 *
 * Environment: FT_SSH    ssh command on this machine (default "ssh -4"), e.g. "ssh -p 2222"
 *              FT_REMOTE path to this machine's own ft binary. Read by the shell
 *                        on the machine ssh logs into (default: "ft" on its PATH).
 *              FT_STUN   STUN server host:port (default Google's); "" = don't use STUN
 *                        (then only the LAN address seen by sshd is offered)
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 1200              /* payload bytes; 1200+16+28 fits any IPv6 path */
#define WIN   (1u << 16)        /* max chunks between oldest unacked and newest sent */
#define QCAP  (1u << 17)        /* max transmissions tracked for loss detection */
#define HIGH_GAIN 2.885         /* 2/ln2 */

enum { PUNCH = 0x46540001, DATA, ACK, FIN };
struct hdr    { uint32_t type, a, b, ts; };
struct packet { struct hdr h; char data[CHUNK]; };

static struct sockaddr_in cand[4], peer;    /* candidate peer addresses; the one that answered */
static int ncand, have_peer, serving;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static void die(const char *msg) { perror(msg); exit(1); }
static void fail(const char *msg) { fprintf(stderr, "ft: %s\n", msg); exit(1); }

/* ---------------------------------------------------------------- network */

static int resolve(const char *s, struct sockaddr_in *out) {       /* "host:port" */
    char host[256], *c = strrchr(s, ':');
    if (!c || c == s || (size_t)(c - s) >= sizeof host) return -1;
    memcpy(host, s, c - s), host[c - s] = 0;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *ai;
    if (getaddrinfo(host, c + 1, &hints, &ai)) return -1;
    *out = *(struct sockaddr_in *)ai->ai_addr;
    freeaddrinfo(ai);
    return 0;
}

static void add_cand(const char *s) {                              /* silently ignores junk like "-" */
    struct sockaddr_in a;
    if (ncand < 4 && !resolve(s, &a)) cand[ncand++] = a;
}

static const char *addr_str(const struct sockaddr_in *a, char *buf) {   /* buf >= 32 bytes */
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof ip);
    snprintf(buf, 32, "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}

static int udp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) die("socket");
    int buf = 4 << 20;                                            /* absorb pacing bursts */
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    struct sockaddr_in a = { .sin_family = AF_INET };
    if (bind(fd, (struct sockaddr *)&a, sizeof a)) die("bind");
    return fd;
}

static unsigned local_port(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &l);
    return ntohs(a.sin_port);
}

/* Ask a STUN server which public ip:port our socket appears as (RFC 5389). */
static int stun(int fd, struct sockaddr_in *me) {
    struct sockaddr_in srv;
    uint8_t req[20] = { 0, 1, 0, 0, 0x21, 0x12, 0xA4, 0x42 }, rsp[512];
    const char *server = getenv("FT_STUN");                       /* "" disables STUN */
    if (!server) server = "stun.l.google.com:19302";
    if (!*server || resolve(server, &srv)) return -1;
    for (int i = 8; i < 20; i++) req[i] = rand();
    for (int try = 0; try < 3; try++) {
        sendto(fd, req, sizeof req, 0, (struct sockaddr *)&srv, sizeof srv);
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 500) <= 0) continue;
        ssize_t n = recv(fd, rsp, sizeof rsp, 0);
        if (n < 20 || rsp[0] != 1 || rsp[1] != 1 || memcmp(rsp + 4, req + 4, 16)) continue;
        for (ssize_t i = 20; i + 4 <= n; i += 4 + (((rsp[i + 2] << 8 | rsp[i + 3]) + 3) & ~3)) {
            int type = rsp[i] << 8 | rsp[i + 1], len = rsp[i + 2] << 8 | rsp[i + 3];
            if (type == 0x0020 && len >= 8 && i + 4 + len <= n && rsp[i + 5] == 1) {
                memset(me, 0, sizeof *me);                     /* XOR-MAPPED-ADDRESS */
                me->sin_family = AF_INET;
                me->sin_port = htons((rsp[i + 6] << 8 | rsp[i + 7]) ^ 0x2112);
                for (int k = 0; k < 4; k++) ((uint8_t *)&me->sin_addr)[k] = rsp[i + 8 + k] ^ req[4 + k];
                return 0;
            }
        }
    }
    return -1;
}

/* Until the peer has answered we do not know which candidate address works: try all. */
static void send_pkt(int fd, struct packet *p, uint32_t type, uint32_t a, uint32_t b,
                     uint32_t ts, size_t len) {
    p->h = (struct hdr){ htonl(type), htonl(a), htonl(b), ts };
    if (have_peer)
        sendto(fd, p, sizeof p->h + len, 0, (struct sockaddr *)&peer, sizeof peer);
    else
        for (int i = 0; i < ncand; i++)
            sendto(fd, p, sizeof p->h + len, 0, (struct sockaddr *)&cand[i], sizeof cand[i]);
}

static int same(const struct sockaddr_in *x, const struct sockaddr_in *y) {
    return x->sin_addr.s_addr == y->sin_addr.s_addr && x->sin_port == y->sin_port;
}

/* Non-blocking receive of one valid packet from the peer; returns payload size or -1.
 * The first packet from any candidate address fixes the peer. */
static ssize_t recv_pkt(int fd, struct packet *p) {
    for (;;) {
        struct sockaddr_in from;
        socklen_t sl = sizeof from;
        ssize_t n = recvfrom(fd, p, sizeof *p, MSG_DONTWAIT, (struct sockaddr *)&from, &sl);
        if (n < 0) return -1;
        p->h.type = ntohl(p->h.type), p->h.a = ntohl(p->h.a), p->h.b = ntohl(p->h.b);
        if (n < (ssize_t)sizeof p->h || p->h.type < PUNCH || p->h.type > FIN) continue;
        if (!have_peer) {
            int i = 0;
            while (i < ncand && !same(&cand[i], &from)) i++;
            if (i == ncand) continue;
            peer = from, have_peer = 1;
        } else if (!same(&from, &peer))
            continue;
        return n - sizeof p->h;
    }
}

/* --------------------------------------------------------------- receiver */

static void receiver(int fd, int out) {
    struct packet p;
    uint8_t *got = NULL;
    uint32_t total = 0, count = 0, cum = 0;
    uint64_t t0 = 0, t1 = 0;
    int waits = 0;

    for (;;) {
        /* before the first chunk keep punching; after the last just linger for a lost final ACK */
        struct pollfd pf = { fd, POLLIN, 0 };
        int idle = !total ? 250 : count == total ? 3000 : 30000;
        if (poll(&pf, 1, idle) <= 0) {
            if (total && count == total) break;
            if (total) fail("recv: timed out");
            if (++waits > 240) fail("recv: the sender never showed up");
            send_pkt(fd, &p, PUNCH, 0, 0, 0, 0);
            continue;
        }
        ssize_t len;
        while ((len = recv_pkt(fd, &p)) >= 0) {
            if (p.h.type == PUNCH) { send_pkt(fd, &p, PUNCH, 0, 0, 0, 0); continue; }
            if (p.h.type == FIN) { if (total && count == total) goto done; continue; }
            if (p.h.type != DATA) continue;
            uint32_t seq = p.h.a, tot = p.h.b;
            if (!total && tot) { total = tot; got = calloc(total, 1); t0 = now_us(); }
            if (!got || tot != total || seq >= total || len > CHUNK ||
                (len < CHUNK && seq != total - 1)) continue;      /* malformed */
            if (!got[seq]) {
                if (pwrite(out, p.data, len, (off_t)seq * CHUNK) != len) die("write");
                got[seq] = 1, count++;
                while (cum < total && got[cum]) cum++;
                if (count == total) t1 = now_us();
            }
            send_pkt(fd, &p, ACK, seq, cum, p.h.ts, 0);          /* ack every copy */
        }
    }
done:
    close(out);
    if (!serving) fprintf(stderr, "received %u chunks in %.2f s\n", total, (t1 - t0) / 1e6);
}

/* ----------------------------------------------------------------- sender */

enum { UNSENT, INFLIGHT, LOST, ACKED };                          /* chunk states */
enum { STARTUP, DRAIN, PROBE_BW, PROBE_RTT };                    /* BBR modes */
static const char *mode_name[] = { "startup", "drain", "probe_bw", "probe_rtt" };
static const double CYCLE[8] = { 1.25, 0.75, 1, 1, 1, 1, 1, 1 };

typedef struct {            /* state of the latest transmission of a chunk */
    uint64_t sent, first_sent, delivered_at;                     /* for rate samples */
    uint32_t delivered;
    uint8_t  st;
} Tx;
typedef struct { uint64_t sent; uint32_t seq; } Rec;             /* send-order log */

static struct {
    Tx       tx[WIN];                                            /* indexed by seq % WIN */
    Rec      sq[QCAP];                                           /* transmissions in send order */
    uint32_t lq[WIN];                                            /* chunks waiting to be resent */
    uint32_t sq_h, sq_n, lq_h, lq_n;
    uint32_t una, next, total, inflight, retx;                   /* una: lowest unacked chunk */
    uint32_t delivered;  uint64_t delivered_at, first_sent, rack_sent;
    double   srtt, rttvar, next_send;
    /* BBR */
    int      mode, full, full_cnt, cycle;
    double   bw, bwwin[10], full_bw, pgain, cgain, cwnd;         /* bw in chunks/s */
    uint64_t min_rtt, min_rtt_at, cycle_at, probe_done;          /* microseconds */
    uint32_t round, next_round_delivered;
} S;
#define T(seq) S.tx[(seq) % WIN]

static uint64_t rto(void) {
    double r = S.srtt ? S.srtt + 4 * S.rttvar + 10000 : 1e6;
    return r > 2e6 ? 2e6 : r;
}

static void enter_probe_bw(uint64_t now) { S.mode = PROBE_BW, S.cycle = 2, S.cycle_at = now; }

/* BBR: called on every ACK after the bandwidth / RTT estimates were refreshed. */
static void bbr_update(uint64_t now, uint32_t acked, int round_start) {
    double bdp = S.bw * S.min_rtt / 1e6;

    if (round_start && S.mode == STARTUP) {                       /* pipe full when bw stops growing */
        if (S.bw >= S.full_bw * 1.25) S.full_bw = S.bw, S.full_cnt = 0;
        else if (++S.full_cnt >= 3) S.full = 1, S.mode = DRAIN;
    }
    if (S.mode == DRAIN && S.inflight <= bdp) enter_probe_bw(now);
    if (S.mode == PROBE_BW && now - S.cycle_at > S.min_rtt)       /* next gain phase every RTT */
        S.cycle = (S.cycle + 1) % 8, S.cycle_at = now;
    if (S.mode == PROBE_RTT) {                                    /* 200 ms with a near-empty pipe */
        if (!S.probe_done && S.inflight <= 4) S.probe_done = now + 200000;
        else if (S.probe_done && now >= S.probe_done) {
            S.min_rtt_at = now;
            if (S.full) enter_probe_bw(now); else S.mode = STARTUP;
        }
    }
    switch (S.mode) {
    case STARTUP:   S.pgain = S.cgain = HIGH_GAIN; break;
    case DRAIN:     S.pgain = 1 / HIGH_GAIN, S.cgain = HIGH_GAIN; break;
    case PROBE_BW:  S.pgain = CYCLE[S.cycle], S.cgain = 2; break;
    case PROBE_RTT: S.pgain = S.cgain = 1; break;
    }
    if (S.mode != PROBE_RTT) {                                    /* cwnd follows 2 x BDP */
        double target = S.cgain * bdp < 4 ? 4 : S.cgain * bdp;
        if (S.full) S.cwnd = S.cwnd + acked < target ? S.cwnd + acked : target;
        else if (S.cwnd < target || S.delivered < 10) S.cwnd += acked;
    }
}

static void on_ack(uint32_t seq, uint32_t cum, uint32_t ts, uint64_t now) {
    uint32_t d0 = S.delivered;
    int round_start = 0;
    if (cum > S.next) cum = S.next;

    if (seq >= S.una && seq < S.next && T(seq).st != ACKED) {
        Tx *t = &T(seq);
        if (t->st == INFLIGHT) S.inflight--;
        t->st = ACKED, S.delivered++, S.delivered_at = now;

        uint64_t rtt = (uint32_t)now - ts;                        /* exact: echoed timestamp */
        if (!rtt) rtt = 1;
        S.rttvar = S.srtt ? 0.75 * S.rttvar + 0.25 * (S.srtt > rtt ? S.srtt - rtt : rtt - S.srtt) : rtt / 2.0;
        S.srtt   = S.srtt ? 0.875 * S.srtt + 0.125 * rtt : rtt;
        int expired = now > S.min_rtt_at + 10000000;              /* min-RTT filter: 10 s window */
        if (!S.min_rtt || rtt <= S.min_rtt || expired) S.min_rtt = rtt, S.min_rtt_at = now;
        if (expired && S.mode != PROBE_RTT) S.mode = PROBE_RTT, S.probe_done = 0;

        if ((uint32_t)t->sent == ts) {                            /* ack of the latest copy */
            uint64_t a = now - t->delivered_at, s = t->sent - t->first_sent;
            uint64_t iv = a > s ? a : s;                          /* robust to ack compression */
            double rate = iv >= S.min_rtt && iv ? (S.delivered - t->delivered) * 1e6 / iv : 0;
            S.first_sent = t->sent;
            if (t->sent > S.rack_sent) S.rack_sent = t->sent;
            if (t->delivered >= S.next_round_delivered)
                S.next_round_delivered = S.delivered, S.round++, round_start = 1;
            double *w = &S.bwwin[S.round % 10];                   /* max over the last 10 rounds */
            if (round_start) *w = 0;
            if (rate > *w) *w = rate;
            S.bw = 0;
            for (int i = 0; i < 10; i++) if (S.bwwin[i] > S.bw) S.bw = S.bwwin[i];
        }
    }
    for (; S.una < cum; S.una++) {                                /* cumulative ack covers lost acks */
        Tx *t = &T(S.una);
        if (t->st == INFLIGHT) S.inflight--;
        if (t->st != ACKED) t->st = ACKED, S.delivered++;
    }
    while (S.una < S.next && T(S.una).st == ACKED) S.una++;
    bbr_update(now, S.delivered - d0, round_start);
}

/* Oldest in-flight chunks are lost if a later-sent chunk was acked, or on timeout. */
static void detect_losses(uint64_t now) {
    uint64_t reo = S.min_rtt / 4;
    while (S.sq_n) {
        Rec *r = &S.sq[S.sq_h % QCAP];
        Tx *t = &T(r->seq);
        if (r->seq >= S.una && t->st == INFLIGHT && t->sent == r->sent) {
            if (r->sent + reo > S.rack_sent && now < r->sent + rto()) break;
            t->st = LOST, S.inflight--;
            S.lq[(S.lq_h + S.lq_n++) % WIN] = r->seq;
        }
        S.sq_h++, S.sq_n--;
    }
}

static uint32_t cwnd(void) { return S.mode == PROBE_RTT ? 4 : (uint32_t)S.cwnd; }

static int pick(uint32_t *seq) {                                  /* retransmissions first */
    while (S.lq_n) {
        uint32_t s = S.lq[S.lq_h++ % WIN];
        S.lq_n--;
        if (s >= S.una && T(s).st == LOST) return *seq = s, 1;
    }
    return S.next < S.total && S.next - S.una < WIN ? (*seq = S.next++, 1) : 0;
}

static void transmit(int fd, int file, uint32_t seq, uint64_t now) {
    Tx *t = &T(seq);
    struct packet p;
    ssize_t n = pread(file, p.data, CHUNK, (off_t)seq * CHUNK);
    if (n < 0) die("read");
    if (t->st == LOST) S.retx++;
    if (!S.inflight) S.first_sent = S.delivered_at = now;         /* restart rate sampling */
    *t = (Tx){ now, S.first_sent, S.delivered_at, S.delivered, INFLIGHT };
    S.sq[(S.sq_h + S.sq_n++) % QCAP] = (Rec){ now, seq };
    S.inflight++;
    send_pkt(fd, &p, DATA, seq, S.total, (uint32_t)now, n);
    /* pace: one chunk every 1/(pacing_gain x btl_bw); allow <= 2 ms of catch-up burst */
    double rate = S.pgain * S.bw;
    if (S.next_send < now - 2000.0) S.next_send = now - 2000.0;
    if (rate > 0) S.next_send += 1e6 / rate;
}

static void sender(int fd, int file) {
    struct stat st;
    if (fstat(file, &st)) die("fstat");
    S.total = st.st_size ? (st.st_size + CHUNK - 1) / CHUNK : 1;
    S.mode = STARTUP, S.pgain = S.cgain = HIGH_GAIN, S.cwnd = 10;

    /* punch through the NATs / firewalls: repeat until the receiver answers */
    struct packet p;
    for (int tries = 0;; tries++) {
        if (tries == 240) fail("send: no answer from peer (UDP blocked, or a symmetric NAT?)");
        send_pkt(fd, &p, PUNCH, 0, 0, 0, 0);
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 250) > 0 && recv_pkt(fd, &p) >= 0) break;
    }

    uint64_t t0 = now_us(), last_ack = t0, last_print = t0;
    uint32_t last_delivered = 0;
    for (;;) {
        uint64_t now = now_us();
        detect_losses(now);
        if (S.una == S.total) break;
        if (now - last_ack > 20000000) { fprintf(stderr, "\n"); fail("send: peer stopped answering"); }

        uint32_t seq;
        while (S.inflight < cwnd() && now >= S.next_send && S.sq_n < QCAP && pick(&seq))
            transmit(fd, file, seq, now);

        /* sleep until the next pacing slot or timeout, whichever is first, or an ACK arrives */
        int64_t wait = 100000;
        if (S.inflight < cwnd() && (S.lq_n || (S.next < S.total && S.next - S.una < WIN)))
            wait = S.next_send - now;
        if (S.sq_n) {
            int64_t w = S.sq[S.sq_h % QCAP].sent + rto() - now;
            if (w < wait) wait = w;
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, wait > 0 ? (wait + 999) / 1000 : 0) > 0) {
            while (recv_pkt(fd, &p) >= 0)
                if (p.h.type == ACK) last_ack = now_us(), on_ack(p.h.a, p.h.b, p.h.ts, last_ack);
        }

        now = now_us();
        if (now - last_print >= 1000000) {
            fprintf(stderr, "\r%5.1f%%  goodput %7.2f Mbit/s  btl_bw %7.2f Mbit/s  min_rtt %6.1f ms  %-9s",
                    100.0 * S.una / S.total,
                    (S.delivered - last_delivered) * CHUNK * 8.0 / (now - last_print),
                    S.bw * CHUNK * 8 / 1e6, S.min_rtt / 1e3, mode_name[S.mode]);
            last_print = now, last_delivered = S.delivered;
        }
    }
    for (int i = 0; i < 3; i++) send_pkt(fd, &p, FIN, 0, 0, 0, 0);  /* let the receiver exit now */
    double s = (now_us() - t0) / 1e6;
    fprintf(stderr, "\rsent %lld bytes in %.2f s (%.2f Mbit/s), %u of %u chunks retransmitted%20s\n",
            (long long)st.st_size, s, st.st_size * 8 / 1e6 / s, S.retx, S.total, "");
}

/* ----------------------------------------------------- files and the remote end */

static int open_src(const char *path) {
    struct stat st;
    int f = open(path, O_RDONLY);
    if (f < 0) die(path);
    if (fstat(f, &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "ft: %s: not a regular file\n", path); exit(1); }
    return f;
}

/* Like scp: if the destination is a directory (or empty, meaning "home"), keep the source's name. */
static int open_dest(const char *path, const char *name) {
    char dest[4096];
    struct stat st;
    if (!*path) snprintf(dest, sizeof dest, "%s", name);
    else if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(dest, sizeof dest, "%s/%s", path, name);
    else snprintf(dest, sizeof dest, "%s", path);
    int f = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f < 0) die(dest);
    return f;
}

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Read lines from f until one is "FTADDR a b" (ignores any noise a login shell prints). */
static int read_addrs(FILE *f, char *a, char *b) {
    char line[256];
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "FTADDR %63s %63s", a, b) == 2) return 0;
    return -1;
}

/* Remote end, started by ssh:  ft --serve send PATH   |   ft --serve recv PATH NAME
 * stdout/stdin are the ssh control channel; this process lives only for one transfer. */
static int serve(const char *mode, const char *path, const char *name) {
    int sending = !strcmp(mode, "send");
    serving = 1;
    int file = sending ? open_src(path) : open_dest(path, name);   /* fail early, before any UDP */
    int fd = udp_socket();
    unsigned port = local_port(fd);

    struct sockaddr_in me;
    char pub[32] = "-", lan[64] = "-", cip[64] = "", sip[64] = "", a[64], b[64], t[128];
    if (stun(fd, &me) == 0) addr_str(&me, pub);
    const char *sc = getenv("SSH_CONNECTION");                    /* "client_ip cport server_ip sport" */
    if (sc && sscanf(sc, "%63s %*s %63s", cip, sip) == 2)
        snprintf(lan, sizeof lan, "%s:%u", sip, port);
    printf("FTADDR %s %s\n", pub, lan);
    fflush(stdout);

    if (read_addrs(stdin, a, b)) fail("lost the control connection");
    add_cand(a);                                                  /* client's public address */
    snprintf(t, sizeof t, "%s:%s", cip, b);
    add_cand(t);                                                  /* client's address as seen by sshd */
    if (!ncand) fail("no usable address for the client");

    if (sending) sender(fd, file); else receiver(fd, file);
    return 0;
}

/* Quote s for the remote shell; a leading "~/" stays unquoted so the remote shell expands it. */
static void quote(char *d, size_t n, const char *s) {
    size_t l = strlen(d);
    d[l++] = ' ';
    if (!strncmp(s, "~/", 2)) d[l++] = '~', d[l++] = '/', s += 2;
    d[l++] = '\'';
    for (; *s && l + 6 < n; s++) {
        if (*s == '\'') memcpy(d + l, "'\\''", 4), l += 4;
        else d[l++] = *s;
    }
    d[l++] = '\'';
    d[l] = 0;
}

static pid_t ssh_pid;
static void kill_ssh(void) { if (ssh_pid > 0) kill(ssh_pid, SIGTERM); }

/* "user@host:path" -> NUL-terminates the host part and returns the path; NULL if local.
 * Like scp, a colon after a slash belongs to a local file name. */
static char *remote_split(char *arg) {
    char *c = strchr(arg, ':'), *sl = strchr(arg, '/');
    if (!c || (sl && sl < c)) return NULL;
    *c = 0;
    return c + 1;
}

int main(int argc, char **argv) {
    srand(getpid() ^ time(NULL));
    if (argc >= 4 && !strcmp(argv[1], "--serve")) return serve(argv[2], argv[3], argc > 4 ? argv[4] : "");
    char *rsrc = argc == 3 ? remote_split(argv[1]) : NULL, *rdst = argc == 3 ? remote_split(argv[2]) : NULL;
    if (argc != 3 || (!rsrc && !rdst)) {
        fprintf(stderr, "usage: %s SRC DST     (exactly one of them is [user@]host:path)\n"
                        "  upload:   %s file.bin user@host:dir/\n"
                        "  download: %s user@host:dir/file.bin .\n", argv[0], argv[0], argv[0]);
        return 2;
    }
    if (rsrc && rdst) fail("copying between two remote hosts is not supported");
    int upload = rdst != NULL;
    char *host = upload ? argv[2] : argv[1], *rpath = upload ? rdst : rsrc;
    const char *name = base_name(upload ? argv[1] : rpath);
    if (strlen(rpath) + strlen(name) > 1500) fail("path too long");
    int file = upload ? open_src(argv[1]) : -1;

    /* start ft on the remote machine through ssh (this is the only thing that needs port 22).
     * The remote shell expands FT_REMOTE, so each machine names its own binary. */
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "\"${FT_REMOTE:-ft}\" --serve %s", upload ? "recv" : "send");
    quote(cmd, sizeof cmd, rpath);
    if (upload) quote(cmd, sizeof cmd, name);
    int to[2], from[2];
    signal(SIGPIPE, SIG_IGN);
    if (pipe(to) || pipe(from)) die("pipe");
    if ((ssh_pid = fork()) < 0) die("fork");
    if (!ssh_pid) {
        dup2(to[0], 0), dup2(from[1], 1);
        close(to[0]), close(to[1]), close(from[0]), close(from[1]);
        execlp("sh", "sh", "-c", "exec ${FT_SSH:-ssh -4} -- \"$@\"", "sh", host, cmd, (char *)NULL);
        die("exec");
    }
    close(to[0]), close(from[1]);
    atexit(kill_ssh);

    /* exchange UDP addresses over ssh (while ssh is still logging in we learn ours) */
    int fd = udp_socket();
    struct sockaddr_in me;
    char pub[32] = "-", a[64], b[64];
    if (stun(fd, &me) == 0) addr_str(&me, pub);
    dprintf(to[1], "FTADDR %s %u\n", pub, local_port(fd));
    FILE *rf = fdopen(from[0], "r");
    if (read_addrs(rf, a, b)) {
        int status = 0;
        waitpid(ssh_pid, &status, 0), ssh_pid = 0;
        fprintf(stderr, "ft: could not start the transfer on %s (is ft installed there? see FT_REMOTE)\n", host);
        return 1;
    }
    add_cand(a);                                                  /* server's public address */
    add_cand(b);                                                  /* server's address as seen by sshd */
    if (!ncand) fail("the server offered no usable address");

    if (upload) sender(fd, file); else receiver(fd, open_dest(argv[2], name));

    close(to[1]);
    int status = 0;
    waitpid(ssh_pid, &status, 0), ssh_pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
