/* main.c — startup, the listening socket, launching apps, signals, and the frame clock.
 *
 *   pane [--backend drm|fbdev|headless] [--size WxH] [--theme FILE]
 *        [--terminal CMD] [--startup CMD]...
 *
 * The frame clock: when something changes we set srv.dirty. Before the event loop
 * sleeps, if a frame is dirty and the previous one has reached the screen, we draw a new
 * one and present it. The backend calls server_frame_done() once it's visible (vblank),
 * and that's when clients get their wl_surface.frame callbacks: "now is a good time to
 * draw your next frame". This is what keeps animations smooth and in step with the display. */
#include "server.h"
#include "backend.h"
#include "css.h"
#include "render.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/input-event-codes.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct server srv;

static const char default_theme[] =
#include "default_theme.inc"
    ;

static int tty_fd = -1;
static char socket_path[108], lock_path[120];
static int lock_fd = -1;
static char theme_path[4096];   /* PATH_MAX: realpath() needs that much */
static sigset_t blocked_signals;

/* ------------------------------------------------------------------- tty */

/* KD_GRAPHICS tells the kernel to stop drawing the text console on this VT. If we crash
 * without restoring KD_TEXT the console looks dead, so the crash handler restores it too. */
static void tty_restore(void) {
    if (tty_fd >= 0) ioctl(tty_fd, KDSETMODE, KD_TEXT);
}

static void tty_setup(void) {
    tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty_fd < 0) return;
    if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0) {   /* not a VT (e.g. over ssh): fine */
        close(tty_fd);
        tty_fd = -1;
    }
}

static void crash_handler(int sig) {
    tty_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ---------------------------------------------------------------- socket */

static void listen_event(int fd, uint32_t events, void *data) {
    (void)events; (void)data;
    client_accept(fd);
}

static int open_socket(void) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!dir || !*dir) {
        /* no login manager made us one (normal on LFS): make our own, private to us */
        snprintf(fallback, sizeof fallback, "/tmp/pane-runtime-%d", getuid());
        mkdir(fallback, 0700);
        chmod(fallback, 0700);
        setenv("XDG_RUNTIME_DIR", fallback, 1);
        dir = fallback;
    }
    for (int n = 0; n < 32; n++) {
        snprintf(srv.socket_name, sizeof srv.socket_name, "wayland-%d", n);
        snprintf(socket_path, sizeof socket_path, "%s/%s", dir, srv.socket_name);
        snprintf(lock_path, sizeof lock_path, "%s.lock", socket_path);
        /* the lock file says "a live server owns this name"; a stale socket can then go */
        lock_fd = open(lock_path, O_CREAT | O_CLOEXEC | O_RDWR, 0660);
        if (lock_fd < 0) continue;
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            close(lock_fd);
            lock_fd = -1;
            continue;
        }
        unlink(socket_path);
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s", socket_path);
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 64) < 0) {
            perror(socket_path);
            close(fd);
            return -1;
        }
        setenv("WAYLAND_DISPLAY", srv.socket_name, 1);
        return fd;
    }
    fprintf(stderr, "pane: no free wayland-N socket name\n");
    return -1;
}

/* ---------------------------------------------------------------- children */

void spawn(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        sigprocmask(SIG_UNBLOCK, &blocked_signals, NULL);   /* children shouldn't inherit our mask */
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) perror("fork");
}

static void signal_event(int fd, uint32_t events, void *data) {
    (void)events; (void)data;
    struct signalfd_siginfo si;
    while (read(fd, &si, sizeof si) == sizeof si) {
        if (si.ssi_signo == SIGCHLD) {
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
        } else {
            srv.running = false;
        }
    }
}

/* ------------------------------------------------------------------- theme */

static void theme_event(int fd, uint32_t events, void *data) {
    (void)events; (void)data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(fd, buf, sizeof buf);
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", theme_path);
    const char *base = basename(tmp);
    bool hit = false;
    for (char *p = buf; n > 0 && p < buf + n;) {
        struct inotify_event *ev = (struct inotify_event *)p;
        if (ev->len && !strcmp(ev->name, base)) hit = true;
        p += sizeof *ev + ev->len;
    }
    if (hit && css_load_file(theme_path) == 0) wm_theme_changed();
}

static void load_theme(const char *arg) {
    char cand[4][512];
    int n = 0;
    if (arg) snprintf(cand[n++], 512, "%s", arg);
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cand[n++], 512, "%s/pane/theme.css", xdg);
    if (home && *home) snprintf(cand[n++], 512, "%s/.config/pane/theme.css", home);
    snprintf(cand[n++], 512, "theme.css");
    for (int i = 0; i < n; i++) {
        if (css_load_file(cand[i]) == 0) {
            if (cand[i][0] == '/') snprintf(theme_path, sizeof theme_path, "%s", cand[i]);
            else if (!realpath(cand[i], theme_path)) snprintf(theme_path, sizeof theme_path, "%s", cand[i]);
            break;
        }
        if (arg && i == 0) fprintf(stderr, "pane: can't read theme %s\n", arg);
    }
    char tmp[4096];
    if (!theme_path[0]) {
        css_load_string(default_theme, "(built-in theme)");
        /* keep watching where the user's theme belongs, so creating it there
         * takes effect without a restart */
        int user = arg ? 1 : 0;
        if (user >= n - 1) {
            fprintf(stderr, "pane: using the built-in theme\n");
            return;
        }
        snprintf(theme_path, sizeof theme_path, "%s", cand[user]);
        snprintf(tmp, sizeof tmp, "%s", theme_path);
        for (char *s = strchr(tmp + 1, '/'); s; s = strchr(s + 1, '/')) {   /* mkdir -p */
            *s = '\0';
            mkdir(tmp, 0755);
            *s = '/';
        }
        fprintf(stderr, "pane: using the built-in theme; save one as %s to change it\n", theme_path);
    }
    /* watch the directory, not the file: editors save by writing a new file and
     * renaming, and the file may not exist yet */
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    snprintf(tmp, sizeof tmp, "%s", theme_path);
    if (fd >= 0 && inotify_add_watch(fd, dirname(tmp), IN_CLOSE_WRITE | IN_MOVED_TO) >= 0)
        watch_add(fd, EPOLLIN, theme_event, NULL);
}

/* --------------------------------------------------------- headless console */

/* With --backend headless, stdin takes commands so you can script input and grab
 * screenshots:  move X Y | rel DX DY | click [left|right|middle] | button NAME down|up
 *               key NAME|CODE [down|up] | scroll N | screenshot FILE | quit */
static const struct { const char *name; int code; } key_names[] = {
    { "esc", KEY_ESC }, { "enter", KEY_ENTER }, { "tab", KEY_TAB }, { "space", KEY_SPACE },
    { "backspace", KEY_BACKSPACE }, { "ctrl", KEY_LEFTCTRL }, { "shift", KEY_LEFTSHIFT },
    { "alt", KEY_LEFTALT }, { "super", KEY_LEFTMETA }, { "up", KEY_UP }, { "down", KEY_DOWN },
    { "left", KEY_LEFT }, { "right", KEY_RIGHT }, { "f4", KEY_F4 }, { "f10", KEY_F10 },
    { "a", KEY_A }, { "b", KEY_B }, { "c", KEY_C }, { "d", KEY_D }, { "e", KEY_E }, { "f", KEY_F },
    { "g", KEY_G }, { "h", KEY_H }, { "i", KEY_I }, { "j", KEY_J }, { "k", KEY_K }, { "l", KEY_L },
    { "m", KEY_M }, { "n", KEY_N }, { "o", KEY_O }, { "p", KEY_P }, { "q", KEY_Q }, { "r", KEY_R },
    { "s", KEY_S }, { "t", KEY_T }, { "u", KEY_U }, { "v", KEY_V }, { "w", KEY_W }, { "x", KEY_X },
    { "y", KEY_Y }, { "z", KEY_Z }, { "0", KEY_0 }, { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 },
    { "4", KEY_4 }, { "5", KEY_5 }, { "6", KEY_6 }, { "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 },
};

static int parse_key(const char *s) {
    for (size_t i = 0; i < ARRAY_LEN(key_names); i++)
        if (!strcmp(s, key_names[i].name)) return key_names[i].code;
    return atoi(s);
}

static int parse_button(const char *s) {
    if (!s || !strcmp(s, "left")) return BTN_LEFT;
    if (!strcmp(s, "right")) return BTN_RIGHT;
    if (!strcmp(s, "middle")) return BTN_MIDDLE;
    return BTN_LEFT;
}

static void console_command(char *line) {
    char *argv[4] = { 0 };
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < 4; tok = strtok(NULL, " \t")) argv[argc++] = tok;
    if (argc == 0) return;
    const char *cmd = argv[0];
    if (!strcmp(cmd, "move") && argc >= 3) wm_pointer_warp(atof(argv[1]), atof(argv[2]));
    else if (!strcmp(cmd, "rel") && argc >= 3) wm_pointer_motion(atof(argv[1]), atof(argv[2]));
    else if (!strcmp(cmd, "click")) {
        int b = parse_button(argv[1]);
        wm_pointer_button(b, true);
        wm_pointer_button(b, false);
    } else if (!strcmp(cmd, "button") && argc >= 3) wm_pointer_button(parse_button(argv[1]), !strcmp(argv[2], "down"));
    else if (!strcmp(cmd, "key") && argc >= 2) {
        int code = parse_key(argv[1]);
        if (argc >= 3) wm_key(code, !strcmp(argv[2], "down"));
        else { wm_key(code, true); wm_key(code, false); }
    } else if (!strcmp(cmd, "scroll") && argc >= 2) {
        int n = atoi(argv[1]);
        wm_pointer_axis(0, 15.0 * n, n);
    } else if (!strcmp(cmd, "screenshot") && argc >= 2) {
        if (srv.backend->screenshot) srv.backend->screenshot(srv.backend, argv[1]);
    } else if (!strcmp(cmd, "quit")) srv.running = false;
    else { fprintf(stderr, "console: unknown command '%s'\n", cmd); return; }
    printf("ok\n");
    fflush(stdout);
}

static void console_event(int fd, uint32_t events, void *data) {
    (void)data;
    static char buf[4096];
    static size_t len;
    ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
    if (n <= 0) {
        if (n == 0 || (events & EPOLLHUP)) watch_remove(*(struct watch **)data);
        return;
    }
    len += n;
    char *nl;
    while ((nl = memchr(buf, '\n', len))) {
        *nl = '\0';
        console_command(buf);
        size_t used = nl + 1 - buf;
        memmove(buf, nl + 1, len - used);
        len -= used;
    }
    if (len == sizeof buf - 1) len = 0;
}

/* -------------------------------------------------------------- frame clock */

static void render_frame(void) {
    struct backend *b = srv.backend;
    struct canvas c = { b->shadow, b->width, b->height, b->width, { 0, 0, b->width, b->height } };
    srv.dirty = false;
    wm_render(&c);
    if (anim_frame_done()) srv.dirty = true;   /* a transition is running: draw the next frame too */
    frame_callbacks_collect();
    srv.frame_pending = true;
    b->present(b);
}

void server_frame_done(void) {
    srv.frame_pending = false;
    frame_callbacks_done(now_ms());
}

static void before_wait(void) {
    for (int i = 0; i < 3; i++) {
        clients_flush_and_reap();
        if (!srv.dirty || srv.frame_pending) break;
        render_frame();
    }
}

/* ------------------------------------------------------------------- main */

static void usage(void) {
    fprintf(stderr,
            "usage: pane [options]\n"
            "  -b, --backend drm|fbdev|headless   (default: drm, falling back to fbdev)\n"
            "  -d, --device PATH                  DRM or fbdev device node\n"
            "  -S, --size WxH                     headless screen size (default 1280x800)\n"
            "  -c, --theme FILE                   stylesheet (default ~/.config/pane/theme.css)\n"
            "  -t, --terminal CMD                 command for Super+Enter / Ctrl+Alt+T (default foot)\n"
            "  -s, --startup CMD                  run CMD once the server is up (repeatable)\n"
            "\n"
            "keys: Ctrl+Alt+Backspace quit, Alt+Tab cycle, Alt+F4 close, Super+Up maximize,\n"
            "      Alt+drag move, Alt+right-drag resize. Set PANE_DEBUG=1 to trace the protocol.\n");
}

int main(int argc, char **argv) {
    const char *backend_name = NULL, *device = NULL, *theme = NULL;
    const char *startup[16];
    int nstartup = 0, hw = 1280, hh = 800;
    srv.terminal = "foot";

    static const struct option opts[] = {
        { "backend", required_argument, 0, 'b' }, { "device", required_argument, 0, 'd' },
        { "size", required_argument, 0, 'S' },    { "theme", required_argument, 0, 'c' },
        { "terminal", required_argument, 0, 't' }, { "startup", required_argument, 0, 's' },
        { "help", no_argument, 0, 'h' },           { 0, 0, 0, 0 },
    };
    int o;
    while ((o = getopt_long(argc, argv, "b:d:S:c:t:s:h", opts, NULL)) != -1) {
        switch (o) {
        case 'b': backend_name = optarg; break;
        case 'd': device = optarg; break;
        case 'S': if (sscanf(optarg, "%dx%d", &hw, &hh) != 2 || hw < 64 || hh < 64) { usage(); return 1; } break;
        case 'c': theme = optarg; break;
        case 't': srv.terminal = optarg; break;
        case 's': if (nstartup < 16) startup[nstartup++] = optarg; break;
        default: usage(); return o == 'h' ? 0 : 1;
        }
    }

    list_init(&srv.clients);
    list_init(&srv.surfaces);
    loop_init();
    bool headless = backend_name && !strcmp(backend_name, "headless");

    if (headless) srv.backend = backend_headless_create(hw, hh);
    else if (backend_name && !strcmp(backend_name, "fbdev")) srv.backend = backend_fbdev_create(device);
    else {
        srv.backend = backend_drm_create(device);
        if (!srv.backend && !backend_name) {
            fprintf(stderr, "pane: DRM failed, trying fbdev\n");
            srv.backend = backend_fbdev_create(NULL);
        }
    }
    if (!srv.backend) {
        fprintf(stderr, "pane: no usable display backend\n");
        return 1;
    }
    srv.width = srv.backend->width;
    srv.height = srv.backend->height;

    /* restore the console if we crash */
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGILL, crash_handler);
    signal(SIGPIPE, SIG_IGN);
    install_sigbus_handler();
    if (!headless) tty_setup();

    sigemptyset(&blocked_signals);
    sigaddset(&blocked_signals, SIGINT);
    sigaddset(&blocked_signals, SIGTERM);
    sigaddset(&blocked_signals, SIGHUP);
    sigaddset(&blocked_signals, SIGCHLD);
    sigprocmask(SIG_BLOCK, &blocked_signals, NULL);
    int sfd = signalfd(-1, &blocked_signals, SFD_CLOEXEC | SFD_NONBLOCK);
    watch_add(sfd, EPOLLIN, signal_event, NULL);

    core_init();
    surface_init_globals();
    xdg_init_globals();
    seat_init();
    wm_init();
    load_theme(theme);
    if (!headless) input_init();

    int lfd = open_socket();
    if (lfd < 0) {
        tty_restore();
        return 1;
    }
    watch_add(lfd, EPOLLIN, listen_event, NULL);

    /* only our children see these: steer toolkits to Wayland */
    unsetenv("DISPLAY");
    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("QT_QPA_PLATFORM", "wayland", 0);
    setenv("GDK_BACKEND", "wayland", 0);
    setenv("SDL_VIDEODRIVER", "wayland", 0);
    setenv("MOZ_ENABLE_WAYLAND", "1", 0);

    fprintf(stderr, "pane: %dx%d via %s, WAYLAND_DISPLAY=%s\n", srv.width, srv.height,
            srv.backend->name, srv.socket_name);

    static struct watch *console_watch;
    if (headless) console_watch = watch_add(STDIN_FILENO, EPOLLIN, console_event, &console_watch);

    for (int i = 0; i < nstartup; i++) spawn(startup[i]);

    srv.running = true;
    srv.dirty = true;
    loop_run(&srv.running, before_wait);

    fprintf(stderr, "pane: shutting down\n");
    clients_destroy_all();
    input_finish();
    seat_finish();
    srv.backend->destroy(srv.backend);
    tty_restore();
    close(lfd);
    unlink(socket_path);
    unlink(lock_path);
    return 0;
}
