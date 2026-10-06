// libx4vr_probe.so: Phase 0 socket probe (docs/LINUX_PORT_PLAN.md, 0.6).
//
// Preloaded by x4vr-probe-run into the whole Steam launch chain, it stays inert unless the process
// is X4. There it logs how X4 creates, configures, waits on and reads its OpenTrack UDP socket:
// which thread, how often, blocking or not, the buffer it reads into, and the code that calls.
// Every wrapper passes straight through to the real function; nothing is changed.
//
// Log: $X4VR_PROBE_DIR/socket-<pid>.log (x4vr-probe-run sets it). Overrides for tests:
// X4VR_PROBE_EXE (executable name, default X4), X4VR_PROBE_PORT (default 4242).
//
// Written in C so that a library loaded into every process on the way to X4 carries no C++
// runtime of its own, and kept to old glibc symbols (2.34, where dlsym moved into libc), so it
// loads into any process on that path whatever glibc it runs with.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <execinfo.h>

#define EXPORT __attribute__((visibility("default")))
#define DETAILED_CALLS 60    // tracked-socket calls logged one by one before summaries take over
#define SUMMARY_MS 2000
#define MAX_CALLERS 64
#define MAX_TIDS 16

// ---- state ------------------------------------------------------------------------------------
static atomic_bool active;
static int log_fd = -1;
static unsigned target_port = 4242;
static atomic_int tracked = -1;       // the OpenTrack socket, once bound to target_port
static atomic_int tracked_epoll = -1; // an epoll set that watches it
static _Atomic unsigned char udp[65536]; // fds known to be UDP sockets (for setup logging)
static struct timespec start;
static const void* self_base;

enum { F_READ, F_RECV, F_RECVFROM, F_RECVMSG, F_RECVMMSG, F_POLL, F_SELECT, F_EPOLL, F_COUNT };
static const char* const fn_names[F_COUNT] = {"read", "recv", "recvfrom", "recvmsg", "recvmmsg", "poll", "select", "epoll_wait"};
static struct {
    atomic_ulong calls[F_COUNT], packets, bytes, again, errors, wait_us, wait_max_us;
    atomic_long tids[MAX_TIDS];
} stats;
static atomic_ulong detailed, last_summary_ms;
static atomic_uintptr_t callers[MAX_CALLERS];

// ---- helpers ----------------------------------------------------------------------------------
static void* next_fn(const char* name, void* volatile* slot) {
    void* fn = *slot;
    if (!fn) *slot = fn = dlsym(RTLD_NEXT, name);
    return fn;
}
#define NEXT(type, name) ((type)next_fn(#name, &next_##name))
#define DECLARE_NEXT(name) static void* volatile next_##name

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec-start.tv_sec)*1000000u + (uint64_t)((t.tv_nsec-start.tv_nsec)/1000);
}
static long tid(void) { return syscall(SYS_gettid); }

__attribute__((format(printf, 1, 2))) static void log_line(const char* format, ...) {
    if (log_fd < 0) return;
    const int saved = errno;
    char line[4096], name[17] = {0};
    prctl(PR_GET_NAME, name);
    int n = snprintf(line, sizeof(line), "%10.3f tid=%ld [%s] ", (double)now_us()/1000, tid(), name);
    va_list args;
    va_start(args, format);
    n += vsnprintf(line+n, sizeof(line)-(size_t)n, format, args);
    va_end(args);
    if (n > (int)sizeof(line)-2) n = (int)sizeof(line)-2;
    line[n++] = '\n';
    if (write(log_fd, line, (size_t)n) < 0) {}
    errno = saved;
}

// One line per frame outside this library: absolute address (X4 isn't PIE), module and the
// nearest exported symbol, if any.
static void log_backtrace(const char* why) {
    void* frames[24];
    const int count = backtrace(frames, 24);
    log_line("  backtrace (%s):", why);
    for (int i = 0; i < count; ++i) {
        const uintptr_t ip = (uintptr_t)frames[i];
        Dl_info info;
        if (dladdr((void*)(ip-1), &info) && info.dli_fbase == self_base) continue;
        const char* module = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
        module = module ? module+1 : (info.dli_fname ? info.dli_fname : "?");
        if (info.dli_sname)
            log_line("    0x%lx %s (%s+0x%lx)", (unsigned long)ip, module, info.dli_sname, (unsigned long)(ip-(uintptr_t)info.dli_saddr));
        else
            log_line("    0x%lx %s (+0x%lx)", (unsigned long)ip, module, (unsigned long)(ip-(uintptr_t)info.dli_fbase));
    }
}
static bool new_caller(uintptr_t caller) {
    for (int i = 0; i < MAX_CALLERS; ++i) {
        uintptr_t seen = atomic_load(&callers[i]);
        if (seen == caller) return false;
        if (!seen && atomic_compare_exchange_strong(&callers[i], &seen, caller)) return true;
        if (seen == caller) return false;
    }
    return false;
}
static void note_tid(void) {
    const long me = tid();
    for (int i = 0; i < MAX_TIDS; ++i) {
        long seen = atomic_load(&stats.tids[i]);
        if (seen == me) return;
        if (!seen && atomic_compare_exchange_strong(&stats.tids[i], &seen, me)) return;
        if (seen == me) return;
    }
}
static void summary(bool final) {
    char counts[512], tids[256];
    int n = 0, m = 0;
    for (int f = 0; f < F_COUNT; ++f)
        n += snprintf(counts+n, sizeof(counts)-(size_t)n, " %s=%lu", fn_names[f], atomic_exchange(&stats.calls[f], 0));
    for (int i = 0; i < MAX_TIDS; ++i) {
        const long t = atomic_exchange(&stats.tids[i], 0);
        if (t) m += snprintf(tids+m, sizeof(tids)-(size_t)m, " %ld", t);
    }
    tids[m] = 0;
    log_line("%s %lus:%s packets=%lu bytes=%lu eagain=%lu errors=%lu wait_total_ms=%.1f wait_max_ms=%.1f threads:%s",
             final ? "FINAL" : "SUMMARY", (unsigned long)(SUMMARY_MS/1000), counts, atomic_exchange(&stats.packets, 0),
             atomic_exchange(&stats.bytes, 0), atomic_exchange(&stats.again, 0), atomic_exchange(&stats.errors, 0),
             (double)atomic_exchange(&stats.wait_us, 0)/1000, (double)atomic_exchange(&stats.wait_max_us, 0)/1000, tids);
}
static void maybe_summary(void) {
    const unsigned long now = (unsigned long)(now_us()/1000);
    unsigned long last = atomic_load(&last_summary_ms);
    if (now-last >= SUMMARY_MS && atomic_compare_exchange_strong(&last_summary_ms, &last, now)) summary(false);
}
static void add_wait(uint64_t us) {
    atomic_fetch_add(&stats.wait_us, us);
    unsigned long max = atomic_load(&stats.wait_max_us);
    while (us > max && !atomic_compare_exchange_weak(&stats.wait_max_us, &max, us)) {}
}
static const char* flag_text(int flags, char* out, size_t size) {
    snprintf(out, size, "0x%x%s%s%s", flags, flags & MSG_DONTWAIT ? " DONTWAIT" : "", flags & MSG_PEEK ? " PEEK" : "",
             flags & MSG_TRUNC ? " TRUNC" : "");
    return out;
}

// Everything that happens after a read-type call on the tracked socket.
static void after_read(int fn, const void* buffer, size_t length, int flags, ssize_t result, int error, uint64_t us, uintptr_t caller) {
    atomic_fetch_add(&stats.calls[fn], 1);
    note_tid();
    add_wait(us);
    if (result > 0) { atomic_fetch_add(&stats.packets, 1); atomic_fetch_add(&stats.bytes, (unsigned long)result); }
    else if (result < 0 && (error == EAGAIN || error == EWOULDBLOCK)) atomic_fetch_add(&stats.again, 1);
    else if (result < 0) atomic_fetch_add(&stats.errors, 1);
    const bool fresh = new_caller(caller);
    if (atomic_fetch_add(&detailed, 1) < DETAILED_CALLS || fresh) {
        char flag_buf[64];
        log_line("%s fd=%d buffer=%p length=%zu flags=%s -> %zd%s%s took_us=%lu caller=0x%lx", fn_names[fn], atomic_load(&tracked),
                 buffer, length, flag_text(flags, flag_buf, sizeof(flag_buf)), result, result < 0 ? " errno=" : "",
                 result < 0 ? strerror(error) : "", (unsigned long)us, (unsigned long)caller);
        if (result == 48 && buffer) {
            double v[6];
            memcpy(v, buffer, sizeof(v));
            log_line("  packet x=%.3f y=%.3f z=%.3f yaw=%.3f pitch=%.3f roll=%.3f", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        if (fresh) log_backtrace("new caller");
    }
    maybe_summary();
}
static bool is_tracked(int fd) { return atomic_load_explicit(&active, memory_order_relaxed) && fd >= 0 && fd == atomic_load_explicit(&tracked, memory_order_relaxed); }
static bool is_udp(int fd) { return atomic_load_explicit(&active, memory_order_relaxed) && fd >= 0 && fd < 65536 && atomic_load(&udp[fd]); }

static void make_dirs(char* path) {
    for (char* p = path+1; *p; ++p) if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    mkdir(path, 0755);
}

__attribute__((constructor)) static void probe_start(void) {
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n <= 0) return;
    exe[n] = 0;
    const char* base = strrchr(exe, '/');
    base = base ? base+1 : exe;
    const char* want = getenv("X4VR_PROBE_EXE");
    if (strcmp(base, want && *want ? want : "X4") != 0) return; // not the game: stay inert

    if (getenv("X4VR_PROBE_PORT")) { // by hand: glibc 2.38 headers turn strtol/atoi into a 2.38-only symbol
        target_port = 0;
        for (const char* c = getenv("X4VR_PROBE_PORT"); *c >= '0' && *c <= '9'; ++c) target_port = target_port*10+(unsigned)(*c-'0');
    }
    char dir[PATH_MAX];
    if (getenv("X4VR_PROBE_DIR") && *getenv("X4VR_PROBE_DIR")) snprintf(dir, sizeof(dir), "%s", getenv("X4VR_PROBE_DIR"));
    else if (getenv("XDG_STATE_HOME") && *getenv("XDG_STATE_HOME")) snprintf(dir, sizeof(dir), "%s/x4vr/probe", getenv("XDG_STATE_HOME"));
    else snprintf(dir, sizeof(dir), "%s/.local/state/x4vr/probe", getenv("HOME") ? getenv("HOME") : "/tmp");
    make_dirs(dir);
    char path[PATH_MAX+64];
    snprintf(path, sizeof(path), "%s/socket-%d.log", dir, (int)getpid());
    log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) return;

    clock_gettime(CLOCK_MONOTONIC, &start);
    Dl_info info;
    if (dladdr((void*)&probe_start, &info)) self_base = info.dli_fbase;
    time_t wall = time(NULL);
    char when[64];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&wall));
    log_line("x4vr socket probe: %s pid=%d (main thread tid=pid) watching UDP port %u, started %s", exe, (int)getpid(), target_port, when);
    log_line("LD_PRELOAD=%s", getenv("LD_PRELOAD") ? getenv("LD_PRELOAD") : "");
    void* warm[4];
    backtrace(warm, 4); // glibc loads the unwinder on first use: do it now, not on a game thread
    atomic_store(&active, true);
}
__attribute__((destructor)) static void probe_stop(void) {
    if (!atomic_load(&active)) return;
    if (atomic_load(&tracked) >= 0) summary(true); // otherwise close() already wrote it
    log_line("x4vr socket probe: process exiting");
}

// ---- socket setup -----------------------------------------------------------------------------
DECLARE_NEXT(socket); DECLARE_NEXT(bind); DECLARE_NEXT(setsockopt); DECLARE_NEXT(close);
DECLARE_NEXT(dup2); DECLARE_NEXT(dup3); DECLARE_NEXT(fcntl); DECLARE_NEXT(fcntl64); DECLARE_NEXT(ioctl);
DECLARE_NEXT(epoll_ctl);

EXPORT int socket(int domain, int type, int protocol) {
    const int fd = NEXT(int (*)(int, int, int), socket)(domain, type, protocol);
    if (fd >= 0 && atomic_load(&active) && (domain == AF_INET || domain == AF_INET6)) {
        const int saved = errno;
        const bool dgram = (type & 0xf) == SOCK_DGRAM;
        if (dgram && fd < 65536) atomic_store(&udp[fd], 1);
        log_line("socket(%s, %s%s%s) -> fd %d", domain == AF_INET ? "AF_INET" : "AF_INET6", dgram ? "SOCK_DGRAM" : "stream/other",
                 type & SOCK_NONBLOCK ? " | SOCK_NONBLOCK" : "", type & SOCK_CLOEXEC ? " | SOCK_CLOEXEC" : "", fd);
        if (dgram) log_backtrace("UDP socket created");
        errno = saved;
    }
    return fd;
}
EXPORT int bind(int fd, __CONST_SOCKADDR_ARG address, socklen_t length) {
    const int result = NEXT(int (*)(int, __CONST_SOCKADDR_ARG, socklen_t), bind)(fd, address, length);
    const struct sockaddr* a = address.__sockaddr__;
    if (atomic_load(&active) && a && (a->sa_family == AF_INET || a->sa_family == AF_INET6)) {
        const int saved = errno;
        unsigned port = a->sa_family == AF_INET ? ntohs(((const struct sockaddr_in*)a)->sin_port) : ntohs(((const struct sockaddr_in6*)a)->sin6_port);
        int type = 0;
        socklen_t size = sizeof(type);
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size);
        log_line("bind(fd %d, %s port %u, %s) -> %d", fd, a->sa_family == AF_INET ? "IPv4" : "IPv6", port, type == SOCK_DGRAM ? "UDP" : "not UDP", result);
        if (result == 0 && type == SOCK_DGRAM && port == target_port) {
            atomic_store(&tracked, fd);
            log_line("TRACKING fd %d as X4's OpenTrack socket", fd);
            log_backtrace("OpenTrack socket bound");
        }
        errno = saved;
    }
    return result;
}
EXPORT int setsockopt(int fd, int level, int name, const void* value, socklen_t length) {
    const int result = NEXT(int (*)(int, int, int, const void*, socklen_t), setsockopt)(fd, level, name, value, length);
    if (is_udp(fd) || is_tracked(fd)) {
        if (level == SOL_SOCKET && name == SO_RCVTIMEO && value && length >= sizeof(struct timeval)) {
            const struct timeval* tv = value;
            log_line("setsockopt(fd %d, SO_RCVTIMEO %ld.%06ld s) -> %d", fd, (long)tv->tv_sec, (long)tv->tv_usec, result);
        } else if (value && length == sizeof(int)) {
            log_line("setsockopt(fd %d, level %d, option %d, value %d) -> %d", fd, level, name, *(const int*)value, result);
        } else {
            log_line("setsockopt(fd %d, level %d, option %d, %u bytes) -> %d", fd, level, name, (unsigned)length, result);
        }
    }
    return result;
}
static void forget(int fd, const char* why) {
    if (fd < 0 || !atomic_load(&active)) return;
    if (fd < 65536) atomic_store(&udp[fd], 0);
    int expected = fd;
    if (atomic_compare_exchange_strong(&tracked, &expected, -1)) { log_line("%s: fd %d is no longer tracked", why, fd); summary(true); }
}
EXPORT int close(int fd) {
    forget(fd, "close");
    return NEXT(int (*)(int), close)(fd);
}
EXPORT int dup2(int from, int to) {
    if (from != to) forget(to, "dup2 over");
    return NEXT(int (*)(int, int), dup2)(from, to);
}
EXPORT int dup3(int from, int to, int flags) {
    forget(to, "dup3 over");
    return NEXT(int (*)(int, int, int), dup3)(from, to, flags);
}
static void log_fcntl(const char* fn, int fd, int command, unsigned long arg, int result) {
    if (!(is_udp(fd) || is_tracked(fd))) return;
    if (command == F_SETFL) log_line("%s(fd %d, F_SETFL 0x%lx%s) -> %d", fn, fd, arg, arg & O_NONBLOCK ? " O_NONBLOCK" : "", result);
    else if (command == F_GETFL) log_line("%s(fd %d, F_GETFL) -> 0x%x", fn, fd, result);
}
EXPORT int fcntl(int fd, int command, ...) {
    va_list args;
    va_start(args, command);
    const unsigned long arg = va_arg(args, unsigned long);
    va_end(args);
    const int result = NEXT(int (*)(int, int, ...), fcntl)(fd, command, arg);
    const int saved = errno;
    log_fcntl("fcntl", fd, command, arg, result);
    errno = saved;
    return result;
}
EXPORT int fcntl64(int fd, int command, ...) {
    va_list args;
    va_start(args, command);
    const unsigned long arg = va_arg(args, unsigned long);
    va_end(args);
    const int result = NEXT(int (*)(int, int, ...), fcntl64)(fd, command, arg);
    const int saved = errno;
    log_fcntl("fcntl64", fd, command, arg, result);
    errno = saved;
    return result;
}
EXPORT int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void* arg = va_arg(args, void*);
    va_end(args);
    const int result = NEXT(int (*)(int, unsigned long, ...), ioctl)(fd, request, arg);
    if (request == FIONBIO && (is_udp(fd) || is_tracked(fd))) {
        const int saved = errno;
        log_line("ioctl(fd %d, FIONBIO %d) -> %d", fd, arg ? *(const int*)arg : -1, result);
        errno = saved;
    }
    return result;
}
EXPORT int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    const int result = NEXT(int (*)(int, int, int, struct epoll_event*), epoll_ctl)(epfd, op, fd, event);
    if (is_tracked(fd)) {
        const int saved = errno;
        if (op != EPOLL_CTL_DEL) atomic_store(&tracked_epoll, epfd);
        log_line("epoll_ctl(epoll %d, %s, fd %d, events 0x%x) -> %d", epfd, op == EPOLL_CTL_ADD ? "ADD" : op == EPOLL_CTL_MOD ? "MOD" : "DEL",
                 fd, event ? event->events : 0u, result);
        errno = saved;
    }
    return result;
}

// ---- reads ------------------------------------------------------------------------------------
DECLARE_NEXT(read); DECLARE_NEXT(__read_chk); DECLARE_NEXT(recv); DECLARE_NEXT(__recv_chk);
DECLARE_NEXT(recvfrom); DECLARE_NEXT(__recvfrom_chk); DECLARE_NEXT(recvmsg); DECLARE_NEXT(recvmmsg);

#define TIMED_READ(fn, buffer, length, flags, call)                                              \
    do {                                                                                          \
        const uintptr_t caller = (uintptr_t)__builtin_return_address(0);                         \
        const uint64_t t = now_us();                                                              \
        const ssize_t result = (call);                                                            \
        const int error = errno;                                                                  \
        after_read(fn, buffer, length, flags, result, error, now_us()-t, caller);                 \
        errno = error;                                                                            \
        return result;                                                                            \
    } while (0)

EXPORT ssize_t read(int fd, void* buffer, size_t length) {
    typedef ssize_t (*fn)(int, void*, size_t);
    if (!is_tracked(fd)) return NEXT(fn, read)(fd, buffer, length);
    TIMED_READ(F_READ, buffer, length, 0, NEXT(fn, read)(fd, buffer, length));
}
EXPORT ssize_t __read_chk(int fd, void* buffer, size_t length, size_t size) {
    typedef ssize_t (*fn)(int, void*, size_t, size_t);
    if (!is_tracked(fd)) return NEXT(fn, __read_chk)(fd, buffer, length, size);
    TIMED_READ(F_READ, buffer, length, 0, NEXT(fn, __read_chk)(fd, buffer, length, size));
}
EXPORT ssize_t recv(int fd, void* buffer, size_t length, int flags) {
    typedef ssize_t (*fn)(int, void*, size_t, int);
    if (!is_tracked(fd)) return NEXT(fn, recv)(fd, buffer, length, flags);
    TIMED_READ(F_RECV, buffer, length, flags, NEXT(fn, recv)(fd, buffer, length, flags));
}
EXPORT ssize_t __recv_chk(int fd, void* buffer, size_t length, size_t size, int flags) {
    typedef ssize_t (*fn)(int, void*, size_t, size_t, int);
    if (!is_tracked(fd)) return NEXT(fn, __recv_chk)(fd, buffer, length, size, flags);
    TIMED_READ(F_RECV, buffer, length, flags, NEXT(fn, __recv_chk)(fd, buffer, length, size, flags));
}
EXPORT ssize_t recvfrom(int fd, void* buffer, size_t length, int flags, __SOCKADDR_ARG from, socklen_t* from_length) {
    typedef ssize_t (*fn)(int, void*, size_t, int, __SOCKADDR_ARG, socklen_t*);
    if (!is_tracked(fd)) return NEXT(fn, recvfrom)(fd, buffer, length, flags, from, from_length);
    TIMED_READ(F_RECVFROM, buffer, length, flags, NEXT(fn, recvfrom)(fd, buffer, length, flags, from, from_length));
}
EXPORT ssize_t __recvfrom_chk(int fd, void* buffer, size_t length, size_t size, int flags, __SOCKADDR_ARG from, socklen_t* from_length) {
    typedef ssize_t (*fn)(int, void*, size_t, size_t, int, __SOCKADDR_ARG, socklen_t*);
    if (!is_tracked(fd)) return NEXT(fn, __recvfrom_chk)(fd, buffer, length, size, flags, from, from_length);
    TIMED_READ(F_RECVFROM, buffer, length, flags, NEXT(fn, __recvfrom_chk)(fd, buffer, length, size, flags, from, from_length));
}
EXPORT ssize_t recvmsg(int fd, struct msghdr* message, int flags) {
    typedef ssize_t (*fn)(int, struct msghdr*, int);
    if (!is_tracked(fd)) return NEXT(fn, recvmsg)(fd, message, flags);
    void* buffer = message && message->msg_iovlen ? message->msg_iov[0].iov_base : NULL;
    const size_t length = message && message->msg_iovlen ? message->msg_iov[0].iov_len : 0;
    TIMED_READ(F_RECVMSG, buffer, length, flags, NEXT(fn, recvmsg)(fd, message, flags));
}
EXPORT int recvmmsg(int fd, struct mmsghdr* messages, unsigned int count, int flags, struct timespec* timeout) {
    typedef int (*fn)(int, struct mmsghdr*, unsigned int, int, struct timespec*);
    if (!is_tracked(fd)) return NEXT(fn, recvmmsg)(fd, messages, count, flags, timeout);
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    const uint64_t t = now_us();
    const int result = NEXT(fn, recvmmsg)(fd, messages, count, flags, timeout);
    const int error = errno;
    const bool first = messages && count && messages[0].msg_hdr.msg_iovlen;
    after_read(F_RECVMMSG, first ? messages[0].msg_hdr.msg_iov[0].iov_base : NULL, first ? messages[0].msg_hdr.msg_iov[0].iov_len : 0,
               flags, result > 0 ? (ssize_t)messages[0].msg_len : result, error, now_us()-t, caller);
    if (result > 1) log_line("recvmmsg returned %d messages at once (count limit %u)", result, count);
    errno = error;
    return result;
}

// ---- readiness waits that include the tracked socket ------------------------------------------
DECLARE_NEXT(poll); DECLARE_NEXT(__poll_chk); DECLARE_NEXT(ppoll); DECLARE_NEXT(select); DECLARE_NEXT(pselect);
DECLARE_NEXT(epoll_wait); DECLARE_NEXT(epoll_pwait);

static void note_wait(int fn, const char* what, long timeout_ms, int result, uint64_t us, uintptr_t caller) {
    atomic_fetch_add(&stats.calls[fn], 1);
    note_tid();
    const bool fresh = new_caller(caller);
    if (atomic_fetch_add(&detailed, 1) < DETAILED_CALLS || fresh)
        log_line("%s including the OpenTrack socket, timeout_ms=%ld -> %d took_us=%lu caller=0x%lx", what, timeout_ms, result, (unsigned long)us, (unsigned long)caller);
    if (fresh) log_backtrace("new caller");
    maybe_summary();
}
static bool polls_tracked(const struct pollfd* fds, nfds_t count) {
    if (!atomic_load_explicit(&active, memory_order_relaxed) || atomic_load(&tracked) < 0) return false;
    for (nfds_t i = 0; i < count; ++i) if (fds[i].fd == atomic_load(&tracked)) return true;
    return false;
}
static long ts_ms(const struct timespec* t) { return t ? (long)(t->tv_sec*1000+t->tv_nsec/1000000) : -1; }

#define TIMED_WAIT(fn, what, timeout, call)                                                       \
    do {                                                                                           \
        const uintptr_t caller = (uintptr_t)__builtin_return_address(0);                          \
        const uint64_t t = now_us();                                                               \
        const int result = (call);                                                                 \
        const int error = errno;                                                                   \
        note_wait(fn, what, timeout, result, now_us()-t, caller);                                  \
        errno = error;                                                                             \
        return result;                                                                             \
    } while (0)

// glibc declares poll's fds as write-only (access attribute), so newer GCC calls reading them
// "maybe uninitialized"; the caller's pollfds are always initialised.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
EXPORT int poll(struct pollfd* fds, nfds_t count, int timeout) {
    typedef int (*fn)(struct pollfd*, nfds_t, int);
    if (!polls_tracked(fds, count)) return NEXT(fn, poll)(fds, count, timeout);
    TIMED_WAIT(F_POLL, "poll", timeout, NEXT(fn, poll)(fds, count, timeout));
}
EXPORT int __poll_chk(struct pollfd* fds, nfds_t count, int timeout, size_t size) {
    typedef int (*fn)(struct pollfd*, nfds_t, int, size_t);
    if (!polls_tracked(fds, count)) return NEXT(fn, __poll_chk)(fds, count, timeout, size);
    TIMED_WAIT(F_POLL, "poll", timeout, NEXT(fn, __poll_chk)(fds, count, timeout, size));
}
EXPORT int ppoll(struct pollfd* fds, nfds_t count, const struct timespec* timeout, const sigset_t* mask) {
    typedef int (*fn)(struct pollfd*, nfds_t, const struct timespec*, const sigset_t*);
    if (!polls_tracked(fds, count)) return NEXT(fn, ppoll)(fds, count, timeout, mask);
    TIMED_WAIT(F_POLL, "ppoll", ts_ms(timeout), NEXT(fn, ppoll)(fds, count, timeout, mask));
}
#pragma GCC diagnostic pop
static bool selects_tracked(int count, fd_set* readable) {
    const int fd = atomic_load(&tracked);
    return atomic_load_explicit(&active, memory_order_relaxed) && fd >= 0 && fd < count && fd < FD_SETSIZE && readable && FD_ISSET(fd, readable);
}
EXPORT int select(int count, fd_set* readable, fd_set* writable, fd_set* failed, struct timeval* timeout) {
    typedef int (*fn)(int, fd_set*, fd_set*, fd_set*, struct timeval*);
    if (!selects_tracked(count, readable)) return NEXT(fn, select)(count, readable, writable, failed, timeout);
    const long ms = timeout ? (long)(timeout->tv_sec*1000+timeout->tv_usec/1000) : -1;
    TIMED_WAIT(F_SELECT, "select", ms, NEXT(fn, select)(count, readable, writable, failed, timeout));
}
EXPORT int pselect(int count, fd_set* readable, fd_set* writable, fd_set* failed, const struct timespec* timeout, const sigset_t* mask) {
    typedef int (*fn)(int, fd_set*, fd_set*, fd_set*, const struct timespec*, const sigset_t*);
    if (!selects_tracked(count, readable)) return NEXT(fn, pselect)(count, readable, writable, failed, timeout, mask);
    TIMED_WAIT(F_SELECT, "pselect", ts_ms(timeout), NEXT(fn, pselect)(count, readable, writable, failed, timeout, mask));
}
static bool epoll_tracked(int epfd) { return atomic_load_explicit(&active, memory_order_relaxed) && epfd >= 0 && epfd == atomic_load(&tracked_epoll); }
EXPORT int epoll_wait(int epfd, struct epoll_event* events, int count, int timeout) {
    typedef int (*fn)(int, struct epoll_event*, int, int);
    if (!epoll_tracked(epfd)) return NEXT(fn, epoll_wait)(epfd, events, count, timeout);
    TIMED_WAIT(F_EPOLL, "epoll_wait", timeout, NEXT(fn, epoll_wait)(epfd, events, count, timeout));
}
EXPORT int epoll_pwait(int epfd, struct epoll_event* events, int count, int timeout, const sigset_t* mask) {
    typedef int (*fn)(int, struct epoll_event*, int, int, const sigset_t*);
    if (!epoll_tracked(epfd)) return NEXT(fn, epoll_pwait)(epfd, events, count, timeout, mask);
    TIMED_WAIT(F_EPOLL, "epoll_pwait", timeout, NEXT(fn, epoll_pwait)(epfd, events, count, timeout, mask));
}
