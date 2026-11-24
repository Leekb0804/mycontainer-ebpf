/* userns CapEff timeline experiment.
 * usage: exp <label> map <hostuid>|nomap
 *        exp <label> map <hostuid> <hostgid> setid   (variant a: gid_map uses <hostgid>,
 *              child calls setgid(0)/setuid(0) after mapping, extra stage 2b-postsetid)
 *        (internal) exp --stage3 <label>
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define BASE "/tmp/userns-exp"
static int p2c[2], c2p[2];
static char g_label[64];
static int g_domap; static long g_mapuid; static long g_mapgid = -1; static int g_setid;

static void status_of(const char *path, char *cap, char *uid) {
    FILE *f = fopen(path, "r"); char line[256];
    strcpy(cap, "?"); strcpy(uid, "?");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "CapEff:", 7)) strcpy(cap, line + 8);
        if (!strncmp(line, "Uid:", 4)) { char *s = line + 5; for (char *q = s; *q; q++) if (*q == '\t') *q = ','; strcpy(uid, s); }
    }
    fclose(f);
}
static void aa_of(const char *pid, char *out) {
    char p[64]; snprintf(p, sizeof p, "/proc/%s/attr/current", pid);
    FILE *f = fopen(p, "r"); strcpy(out, "?");
    if (f) { if (fgets(out, 128, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}
static const char *res(int r) {
    static char b[8][64]; static int i; i = (i + 1) % 8;
    if (r == 0) return "OK";
    snprintf(b[i], 64, "FAIL(errno=%d %s)", errno, strerrorname_np(errno));
    return b[i];
}
static void mount_tests(const char *stage, char *out, size_t n) {
    char d[256], lo[256], up[256], wk[256], mg[256], opts[1024];
    snprintf(d, sizeof d, BASE "/run_%s_%s", g_label, stage);
    snprintf(lo, sizeof lo, "%s/lower", d); snprintf(up, sizeof up, "%s/upper", d);
    snprintf(wk, sizeof wk, "%s/work", d);  snprintf(mg, sizeof mg, "%s/merged", d);
    char tm[300]; snprintf(tm, sizeof tm, "%s/tmpfs", d);
    /* dirs were pre-created by the parent (host uid) so they exist regardless of mapping */
    const char *rp = res(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL));
    const char *rt = res(mount("tmpfs", tm, "tmpfs", 0, "size=1m"));
    if (!strcmp(rt, "OK")) umount2(tm, MNT_DETACH);
    snprintf(opts, sizeof opts, "lowerdir=%s,upperdir=%s,workdir=%s", lo, up, wk);
    const char *ro = res(mount("overlay", mg, "overlay", 0, opts));
    if (!strcmp(ro, "OK")) umount2(mg, MNT_DETACH);
    snprintf(out, n, "make-private=%s tmpfs=%s overlay=%s", rp, rt, ro);
}
static void report(const char *stage, const char *who, const char *cap, const char *uid, const char *aa, const char *m) {
    printf("RESULT label=%s stage=%-11s by=%-6s CapEff=%s Uid(r,e,s,fs)=%s getuid=%d aa=\"%s\" %s\n",
           g_label, stage, who, cap, uid, (int)getuid(), aa, m ? m : "");
    fflush(stdout);
}
static void self_stage(const char *stage) {
    char cap[64], uid[128], aa[128], m[512];
    mount_tests(stage, m, sizeof m);           /* mount test first, status after (same creds) */
    status_of("/proc/self/status", cap, uid); aa_of("self", aa);
    report(stage, "child", cap, uid, aa, m);
}
static int child(void *arg) {
    char c;
    close(p2c[1]); close(c2p[0]);
    read(p2c[0], &c, 1);                         /* wait: parent inspects us (pre-map) */
    self_stage("1-premap");
    write(c2p[1], "x", 1);
    read(p2c[0], &c, 1);                         /* wait: parent writes maps (or not) */
    self_stage(g_domap ? "2-postmap" : "2-nomap");
    if (g_setid) {
        int rg = setgid(0); int eg = errno;
        int ru = setuid(0); int eu = errno;
        printf("  child: setgid(0) -> %s, setuid(0) -> %s\n",
               rg == 0 ? "OK" : strerrorname_np(eg), ru == 0 ? "OK" : strerrorname_np(eu));
        fflush(stdout);
        self_stage("2b-postsetid");
    }
    char *av[] = { "/proc/self/exe", "--stage3", g_label, NULL };
    execv("/proc/self/exe", av);
    perror("execv"); return 1;
}
static void wr(const char *path, const char *s) {
    int fd = open(path, O_WRONLY); int r = -1;
    if (fd >= 0) { r = write(fd, s, strlen(s)); close(fd); }
    printf("  parent: write %-28s '%s' -> %s\n", path, s, r >= 0 ? "OK" : strerror(errno));
}
static void mkdirs(const char *stage) {
    const char *sub[] = { "", "/lower", "/upper", "/work", "/merged", "/tmpfs" };
    char p[300];
    for (int i = 0; i < 6; i++) { snprintf(p, sizeof p, BASE "/run_%s_%s%s", g_label, stage, sub[i]); mkdir(p, 0777); chmod(p, 0777); }
    snprintf(p, sizeof p, BASE "/run_%s_%s/lower/hello", g_label, stage);
    FILE *f = fopen(p, "w"); if (f) { fputs("hi\n", f); fclose(f); }
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--stage3")) {
        snprintf(g_label, sizeof g_label, "%s", argv[2]);
        self_stage("3-postexec");
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: %s label map <hostuid>|nomap\n", argv[0]); return 2; }
    snprintf(g_label, sizeof g_label, "%s", argv[1]);
    g_domap = !strcmp(argv[2], "map");
    g_mapuid = g_domap ? strtol(argv[3], NULL, 10) : -1;
    if (g_domap && argc >= 6 && !strcmp(argv[5], "setid")) { g_mapgid = strtol(argv[4], NULL, 10); g_setid = 1; }
    const char *st[] = { "1-premap", "2-postmap", "2-nomap", "2b-postsetid", "3-postexec" };
    for (int i = 0; i < 5; i++) mkdirs(st[i]);

    char cap[64], uid[128], aa[128];
    status_of("/proc/self/status", cap, uid); aa_of("self", aa);
    report("0-parent", "parent", cap, uid, aa, NULL);

    pipe(p2c); pipe(c2p);
    static char stack[1 << 20];
    pid_t pid = clone(child, stack + sizeof stack, CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD, NULL);
    if (pid < 0) { perror("clone"); return 1; }
    close(p2c[0]); close(c2p[1]);

    char path[64], pidstr[16]; snprintf(pidstr, sizeof pidstr, "%d", pid);
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    status_of(path, cap, uid); aa_of(pidstr, aa);
    report("1-premap", "parent", cap, uid, aa, "(read from /proc/<pid>/status while child blocked)");

    char c; write(p2c[1], "x", 1); read(c2p[0], &c, 1);
    if (g_domap) {
        char buf[64], p[64];
        snprintf(buf, sizeof buf, "0 %ld 1", g_mapuid);
        snprintf(p, sizeof p, "/proc/%d/uid_map", pid);   wr(p, buf);
        snprintf(p, sizeof p, "/proc/%d/setgroups", pid); wr(p, "deny");
        snprintf(buf, sizeof buf, "0 %ld 1", g_mapgid >= 0 ? g_mapgid : (long)getgid());
        snprintf(p, sizeof p, "/proc/%d/gid_map", pid);   wr(p, buf);
    }
    write(p2c[1], "x", 1);
    int ws; waitpid(pid, &ws, 0);
    return 0;
}
