/* Reproduce mycontainer_run.c ordering and compare with/without re-exec.
 * usage: flow <label> <mapuid> <mapgid> <setid 0|1> <reexec 0|1>
 *  (internal) flow --phase2 <label>
 * Order (as in mycontainer_run.c):
 *   parent: clone(NEWUSER|NEWNS|NEWPID|NEWUTS) -> setgroups deny -> uid_map "0 U 1" -> gid_map "0 G 1" -> close pipe
 *   child : wait pipe EOF -> [setgid(0)/setuid(0)] -> [execv /proc/self/exe --phase2] -> phase2
 *   phase2: sethostname -> make-private -> overlay -> self-bind -> mkdir put_old
 *           -> procfs(new) on merged/proc  -> bind host /proc on merged/procbind   (both kept)
 *           -> pivot_root -> chdir / -> umount2 old root -> procfs(new) on /proc_post
 *           -> pid checks
 * Each step prints OK or errno; steps after an overlay failure are SKIP.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define BASE "/tmp/userns-exp"
static char L[64];
static int g_setid, g_reexec;
static int pfd[2];

static void capeff(char *cap, char *uid) {
    FILE *f = fopen("/proc/self/status", "r"); char line[256];
    strcpy(cap, "?"); strcpy(uid, "?");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "CapEff:", 7)) strcpy(cap, line + 8);
        if (!strncmp(line, "Uid:", 4)) { char *s = line + 5; for (char *q = s; *q; q++) if (*q == '\t') *q = ','; strcpy(uid, s); }
    }
    fclose(f);
}
static void state(const char *where) {
    char cap[64], uid[128]; capeff(cap, uid);
    printf("STATE label=%s at=%-14s getuid=%d getgid=%d Uid=%s CapEff=%s\n", L, where, (int)getuid(), (int)getgid(), uid, cap);
    fflush(stdout);
}
static int step(const char *name, int r) {
    int e = errno;
    if (r == 0) printf("STEP  label=%s step=%-20s OK\n", L, name);
    else printf("STEP  label=%s step=%-20s FAIL errno=%d %s\n", L, name, e, strerrorname_np(e));
    fflush(stdout);
    return r;
}
static void skip(const char *name) { printf("STEP  label=%s step=%-20s SKIP\n", L, name); fflush(stdout); }
static int count_pids(const char *dir) {
    DIR *d = opendir(dir); if (!d) return -1;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) if (isdigit((unsigned char)e->d_name[0])) n++;
    closedir(d); return n;
}
static void link_of(const char *p, char *out, size_t n) {
    ssize_t k = readlink(p, out, n - 1);
    if (k < 0) snprintf(out, n, "ERR(%s)", strerrorname_np(errno)); else out[k] = 0;
}

static void phase2(void) {
    char d[256], lo[300], up[300], wk[300], mg[300], opts[1200], p[400];
    snprintf(d, sizeof d, BASE "/flow_%s", L);
    snprintf(lo, sizeof lo, "%s/lower", d); snprintf(up, sizeof up, "%s/upper", d);
    snprintf(wk, sizeof wk, "%s/work", d);  snprintf(mg, sizeof mg, "%s/merged", d);
    state("phase2-entry");
    step("sethostname", sethostname("mycontainer", 11));
    step("make-private", mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL));
    snprintf(opts, sizeof opts, "lowerdir=%s,upperdir=%s,workdir=%s", lo, up, wk);
    if (step("overlay", mount("overlay", mg, "overlay", 0, opts)) != 0) {
        const char *rest[] = { "self-bind", "mkdir-put_old", "proc-new-prepivot", "proc-bind-prepivot",
                               "pivot_root", "chdir", "umount-oldroot", "proc-new-postpivot" };
        for (int i = 0; i < 8; i++) skip(rest[i]);
        printf("PID   label=%s getpid=%d (no rootfs; /proc checks skipped)\n", L, (int)getpid());
        return;
    }
    step("self-bind", mount(mg, mg, NULL, MS_BIND | MS_REC, NULL));
    snprintf(p, sizeof p, "%s/.old_root", mg);
    int r = mkdir(p, 0700); if (r != 0 && errno == EEXIST) r = 0;
    step("mkdir-put_old", r);
    snprintf(p, sizeof p, "%s/proc", mg);
    int proc_new_ok = step("proc-new-prepivot", mount("proc", p, "proc", 0, NULL)) == 0;
    snprintf(p, sizeof p, "%s/procbind", mg);
    int proc_bind_ok = step("proc-bind-prepivot", mount("/proc", p, NULL, MS_BIND | MS_REC, NULL)) == 0;
    snprintf(p, sizeof p, "%s/.old_root", mg);
    if (step("pivot_root", syscall(SYS_pivot_root, mg, p)) != 0) {
        skip("chdir"); skip("umount-oldroot"); skip("proc-new-postpivot");
        printf("PID   label=%s getpid=%d\n", L, (int)getpid());
        return;
    }
    step("chdir", chdir("/"));
    step("umount-oldroot", umount2("/.old_root", MNT_DETACH));
    rmdir("/.old_root");
    int proc_post_ok = step("proc-new-postpivot", mount("proc", "/proc_post", "proc", 0, NULL)) == 0;

    char s1[64] = "-", s2[64] = "-", s3[64] = "-";
    if (proc_new_ok)  link_of("/proc/self", s1, sizeof s1);
    if (proc_bind_ok) link_of("/procbind/self", s2, sizeof s2);
    if (proc_post_ok) link_of("/proc_post/self", s3, sizeof s3);
    printf("PID   label=%s getpid=%d | new-prepivot: self=%s npids=%d | bind-host: self=%s npids=%d | new-postpivot: self=%s npids=%d\n",
           L, (int)getpid(),
           s1, proc_new_ok ? count_pids("/proc") : -1,
           s2, proc_bind_ok ? count_pids("/procbind") : -1,
           s3, proc_post_ok ? count_pids("/proc_post") : -1);
    fflush(stdout);
}

static int child(void *arg) {
    char c;
    close(pfd[1]);
    while (read(pfd[0], &c, 1) > 0) ;          /* wait for EOF: parent finished maps */
    close(pfd[0]);
    state("postmap");
    if (g_setid) {
        step("setgid(0)", setgid(0));
        step("setuid(0)", setuid(0));
        state("postsetid");
    }
    if (g_reexec) {
        char *av[] = { "/proc/self/exe", "--phase2", L, NULL };
        execv("/proc/self/exe", av);
        step("reexec(execv)", -1);
        return 1;
    }
    phase2();
    return 0;
}
static void wr(pid_t pid, const char *f, const char *s) {
    char p[64]; snprintf(p, sizeof p, "/proc/%d/%s", pid, f);
    int fd = open(p, O_WRONLY), r = -1;
    if (fd >= 0) { r = write(fd, s, strlen(s)); close(fd); }
    printf("  parent: %-10s '%s' -> %s\n", f, s, r >= 0 ? "OK" : strerror(errno));
}
static void prep(uid_t u, gid_t g) {
    char d[256], p[400];
    snprintf(d, sizeof d, BASE "/flow_%s", L);
    const char *sub[] = { "", "/lower", "/lower/proc", "/lower/procbind", "/lower/proc_post", "/upper", "/work", "/merged" };
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof p, "%s%s", d, sub[i]); mkdir(p, 0755);
        if (geteuid() == 0) chown(p, u, g);     /* mirror mycontainer_run.c: chown to map uid/gid */
    }
    snprintf(p, sizeof p, "%s/lower/hello", d);
    FILE *f = fopen(p, "w"); if (f) { fputs("hi\n", f); fclose(f); }
    if (geteuid() == 0) chown(p, u, g);
}
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--phase2")) {
        snprintf(L, sizeof L, "%s", argv[2]);
        printf("  (re-exec'd into --phase2)\n");
        phase2();
        return 0;
    }
    if (argc < 6) { fprintf(stderr, "usage: %s label mapuid mapgid setid(0|1) reexec(0|1)\n", argv[0]); return 2; }
    snprintf(L, sizeof L, "%s", argv[1]);
    uid_t mu = atoi(argv[2]); gid_t mgid = atoi(argv[3]);
    g_setid = atoi(argv[4]); g_reexec = atoi(argv[5]);
    prep(mu, mgid);
    printf("### label=%s caller_uid=%d map='0 %u 1'/'0 %u 1' setid=%d reexec=%d restrict=", L, (int)getuid(), mu, mgid, g_setid, g_reexec);
    fflush(stdout); system("cat /proc/sys/kernel/apparmor_restrict_unprivileged_userns");

    pipe(pfd);
    static char stack[1 << 20];
    pid_t pid = clone(child, stack + sizeof stack, CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD, NULL);
    if (pid < 0) { perror("clone"); return 1; }
    close(pfd[0]);
    char buf[64];
    wr(pid, "setgroups", "deny");
    snprintf(buf, sizeof buf, "0 %u 1", mu);   wr(pid, "uid_map", buf);
    snprintf(buf, sizeof buf, "0 %u 1", mgid); wr(pid, "gid_map", buf);
    fflush(stdout);
    close(pfd[1]);
    int ws; waitpid(pid, &ws, 0);
    printf("  child exit=%d\n\n", WIFEXITED(ws) ? WEXITSTATUS(ws) : -1);
    return 0;
}
