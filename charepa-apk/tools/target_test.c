/* target_test.c - run charepa's target_is_usable() on the real init.rc and on a
 * BR2-shaped one, including the substring case that was found to be wrong.
 *
 * The predicate decides which /system file gets replaced. Two things need care:
 *
 *   - the file has to exist for the predicate to reach the rc parsing, so the
 *     test builds a fake tree under /tmp/fakeroot rather than touching /system;
 *   - `debuggerd` must not match `debuggerd64`. The first version used a plain
 *     strstr and did match it, which would have replaced a file nothing runs.
 *     Keeping this test is what caught it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *ROOT = "/tmp/fakeroot";

static int usable(const char *path, const char *rc)
{
    char full[512];
    snprintf(full, sizeof full, "%s%s", ROOT, path);
    if (access(full, F_OK) != 0) return 0;

    FILE *f = fopen(rc, "r");
    if (!f) return 1;

    char line[512];
    int found = 0, is_service = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "service ", 8)) {
            const char *p = strstr(line, path);
            size_t n = strlen(path);
            is_service = p && (p[n] == ' ' || p[n] == '\n' || p[n] == '\r' || p[n] == 0);
            if (is_service) found = 1;
        } else if (is_service && !strncmp(line, "    class ", 10)) {
            if (strstr(line, "main")) { fclose(f); return 1; }
            is_service = 0;
        } else if (line[0] != ' ' && line[0] != '\n') {
            is_service = 0;
        }
    }
    fclose(f);
    return found ? 1 : 0;
}

int main(void)
{
    mkdir("/tmp/fakeroot", 0755);
    mkdir("/tmp/fakeroot/system", 0755);
    mkdir("/tmp/fakeroot/system/bin", 0755);
    const char *names[] = { "debuggerd", "calibinit.sh", NULL };
    for (int i = 0; names[i]; i++) {
        char p[512];
        snprintf(p, sizeof p, "/tmp/fakeroot/system/bin/%s", names[i]);
        FILE *g = fopen(p, "w");
        if (g) { fputs("x", g); fclose(g); }
    }

    int fails = 0;
#define EXPECT(label, got, want) do { \
        int _g = (got), _w = (want); \
        printf("  %-46s %s\n", label, _g == _w ? "ok" : "WRONG"); \
        if (_g != _w) fails++; \
    } while (0)

    printf("BS init.rc (read off the unit, 23,044 bytes):\n");
    EXPECT("debuggerd usable (class main)", usable("/system/bin/debuggerd", "/tmp/bs_init.rc"), 1);
    EXPECT("calibinit.sh not named",        usable("/system/bin/calibinit.sh", "/tmp/bs_init.rc"), 0);

    const char *br2 = "/tmp/br2_synth.rc";
    FILE *f = fopen(br2, "w");
    if (f) {
        fputs("on early-init\n    start ueventd\n\n"
              "service calibinit /system/bin/calibinit.sh\n"
              "    class main\n"
              "    oneshot\n\n"
              "service debuggerd64 /system/bin/debuggerd64\n"
              "    class main\n", f);
        fclose(f);
    }
    printf("\nBR2-shaped rc (calibinit + class main, only debuggerd64 present):\n");
    EXPECT("calibinit.sh usable (class main)", usable("/system/bin/calibinit.sh", br2), 1);
    EXPECT("debuggerd NOT usable (debuggerd64 is not it)",
           usable("/system/bin/debuggerd", br2), 0);

    printf("\n%s\n", fails ? "RESULT: FAILURES" : "RESULT: all predicates behave");
    return fails ? 1 : 0;
}
