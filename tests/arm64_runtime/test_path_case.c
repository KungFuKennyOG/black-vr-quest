/* Case-insensitive guest path resolution (kernel_path.c) on a case-sensitive
 * host filesystem, as Android external storage can be. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "kernel.h"

static int failures;
static void touch(const char *p) { int fd = open(p, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd); }

int main(void)
{
    char base[256], p[512], out[512], want[512];
    snprintf(base, sizeof(base), "/tmp/arm64rt_ci_%d", (int)getpid());
    mkdir(base, 0755);
    snprintf(p, sizeof(p), "%s/Sound", base); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Sound/Banks", base); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/Sound/Banks/FEAudio.awd", base); touch(p);
    snprintf(p, sizeof(p), "%s/default.xbe", base); touch(p);

    static const struct { const char *rel, *expect; int found; } q[] = {
        {"sound/banks/feaudio.awd", "/Sound/Banks/FEAudio.awd", 1},
        {"SOUND/BANKS/FEAUDIO.AWD", "/Sound/Banks/FEAudio.awd", 1},
        {"Sound/Banks/FEAudio.awd", "/Sound/Banks/FEAudio.awd", 1},  /* exact */
        {"DEFAULT.XBE", "/default.xbe", 1},
        {"sound/banks/new.sav", "/Sound/Banks/new.sav", 0},         /* create keeps name */
        {"nope/x", "/nope/x", 0},
        {"sound/banks/feaudio.awd", "/Sound/Banks/FEAudio.awd", 1},  /* cached */
    };
    for (unsigned i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        int r = xbox_path_resolve_case(base, q[i].rel, out, sizeof(out));
        snprintf(want, sizeof(want), "%s%s", base, q[i].expect);
        int ok = !strcmp(out, want) && r == q[i].found;
        printf("  %s %-26s -> %s (%d)\n", ok ? "PASS" : "FAIL", q[i].rel, out + strlen(base), r);
        failures += !ok;
    }

    /* Through the real translation: a d:\ path into the game dir. */
    xbox_path_init(base, NULL);
    {
        char host[512];
        int ok = xbox_translate_path("D:\\SOUND\\banks\\FEAUDIO.AWD", host, sizeof(host));
        snprintf(want, sizeof(want), "%s/Sound/Banks/FEAudio.awd", base);
        ok = ok && !strcmp(host, want);
        printf("  %s translate D:\\SOUND\\banks\\FEAUDIO.AWD -> %s\n", ok ? "PASS" : "FAIL", host);
        failures += !ok;
    }
    printf("path case: %d failures\n", failures);
    return failures != 0;
}
