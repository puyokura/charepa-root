/* target_test.c - exercise charepa's hook-target predicate without needing
 * anything off the unit
 *
 * The predicate decides which /system file gets replaced, and it is the one
 * model-specific choice in the chain. Three things it has to get right:
 *
 *   1. a file listed as `service debuggerd /system/bin/debuggerd` with
 *      `class main` is usable (this is what the BS init.rc has);
 *   2. a file that exists but that init.rc never names is not usable;
 *   3. `debuggerd` must not match `debuggerd64` by substring.
 *
 * The first version of this file read /tmp/bs_init.rc, which exists only on the
 * machine where the tablet was attached, so on any other host the test looked
 * for a missing file and every predicate returned "not usable" — passing for
 * the wrong reason. It now writes both rc files itself, from the measured
 * stanzas, and touches nothing outside /tmp/fakeroot.
 *
 * It also checks the fallback: an unreadable init.rc makes the predicate answer
 * by existence, because the read happens only after root is obtained.
 *
 * Build and run:
 *   cc -O2 -D_GNU_SOURCE -o target_test target_test.c && ./target_test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *ROOT = "/tmp/fakeroot";
static int fails = 0;

/* charepa.c's target_is_usable(), with the path root injectable */
static int usable(const char *path, const char *rc)
{
    char full[512];
    snprintf(full, sizeof full, "%s%s", ROOT, path);
    if (access(full, F_OK) != 0) return 0;

    FILE *f = fopen(rc, "r");
    if (!f) return 1;              /* unreadable: answer by existence */

    char line[512];
    int is_service = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "service ", 8)) {
            const char *p = strstr(line, path);
            size_t n = strlen(path);
            is_service = p && (p[n] == ' ' || p[n] == '\n' || p[n] == '\r' || p[n] == 0);

        } else if (is_service && !strncmp(line, "    class ", 10)) {
            if (strstr(line, "main")) { fclose(f); return 1; }
            is_service = 0;
        } else if (line[0] != ' ' && line[0] != '\n') {
            is_service = 0;
        }
    }
    fclose(f);
    return 0;
}

static void touch(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/system/bin/%s", ROOT, name);
    FILE *g = fopen(p, "w");
    if (g) { fputs("x", g); fclose(g); }
}

static int write_rc(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fputs(body, f);
    fclose(f);
    return 1;
}

#define EXPECT(label, got, want) do { \
        int _g = (got), _w = (want); \
        printf("  %-48s %s\n", label, _g == _w ? "ok" : "WRONG"); \
        if (_g != _w) fails++; \
    } while (0)

int main(void)
{
    mkdir(ROOT, 0755);
    char d[512];
    snprintf(d, sizeof d, "%s/system", ROOT); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/system/bin", ROOT); mkdir(d, 0755);
    touch("debuggerd");
    touch("calibinit.sh");

    /* the BS shape: debuggerd with class main, no calibinit at all */
    const char *bs = "/tmp/tt_bs.rc";
    write_rc(bs,
        "on early-init\n    start ueventd\n\n"
        "service debuggerd /system/bin/debuggerd\n"
        "    class main\n\n"
        "service debuggerd64 /system/bin/debuggerd64\n"
        "    class main\n");
    printf("BS-shaped rc (debuggerd + class main, no calibinit):\n");
    EXPECT("debuggerd usable",              usable("/system/bin/debuggerd", bs), 1);
    EXPECT("calibinit.sh not named -> unusable", usable("/system/bin/calibinit.sh", bs), 0);

    /* the BR2 shape from the record: calibinit with class main, debuggerd64 only */
    const char *br2 = "/tmp/tt_br2.rc";
    write_rc(br2,
        "on early-init\n    start ueventd\n\n"
        "service calibinit /system/bin/calibinit.sh\n"
        "    class main\n"
        "    oneshot\n\n"
        "service debuggerd64 /system/bin/debuggerd64\n"
        "    class main\n");
    printf("BR2-shaped rc (calibinit + class main, only debuggerd64):\n");
    EXPECT("calibinit.sh usable",           usable("/system/bin/calibinit.sh", br2), 1);
    EXPECT("debuggerd NOT usable (debuggerd64 is a different path)",
           usable("/system/bin/debuggerd", br2), 0);

    /* a service that exists but is not class main */
    const char *odd = "/tmp/tt_odd.rc";
    write_rc(odd,
        "service slowthing /system/bin/calibinit.sh\n"
        "    class late_start\n"
        "    oneshot\n");
    printf("rc with calibinit under class late_start:\n");
    EXPECT("calibinit.sh NOT usable (class is not main)",
           usable("/system/bin/calibinit.sh", odd), 0);

    /* unreadable rc: the predicate falls back to existence */
    printf("unreadable init.rc:\n");
    EXPECT("debuggerd usable by existence only",
           usable("/system/bin/debuggerd", "/tmp/tt_does_not_exist.rc"), 1);

    printf("\n%s\n", fails ? "RESULT: FAILURES" : "RESULT: all predicates behave");
    return fails ? 1 : 0;
}
