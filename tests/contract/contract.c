/* Lumen contract tests: runs every case against a driver and prints TAP.

     contract-x11 [--config NAME] [CASE...]

   Runs the cases written for the default configuration, or with --config,
   those written for that one (the window manager must already run with it).
   With names, runs only those cases. Exits non-zero when a case fails. */

#include "contract.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CT_TIMEOUT_MS 2000
#define CT_STEP_MS 20

static bool failed, skipped;
static long deadline;

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&t, NULL);
}

void ct__check(bool ok, const char *what, const char *file, int line)
{
    if (!ok) {
        printf("#   failed: %s (%s:%d)\n", what, file, line);
        failed = true;
    }
}

void ct__skip(const char *why)
{
    printf("#   needs %s\n", why);
    skipped = true;
}

bool ct__eventually_begin(void)
{
    ct_pump();
    deadline = now_ms() + CT_TIMEOUT_MS;
    return true;
}

bool ct__eventually_again(void)
{
    if (now_ms() > deadline) return false;
    sleep_ms(CT_STEP_MS);
    ct_pump();
    return true;
}

void ct_settle(void)
{
    long end = now_ms() + 150;
    while (now_ms() < end) {
        ct_pump();
        sleep_ms(CT_STEP_MS);
    }
}

bool ct_rect_eq(ct_rect a, ct_rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool ct_rect_contains(ct_rect o, ct_rect i)
{
    return i.x >= o.x && i.y >= o.y &&
        i.x + i.w <= o.x + o.w && i.y + i.h <= o.y + o.h;
}

static const char *config;

static bool wanted(const ct_case *c, int argc, char **argv)
{
    if (!c->config != !config || (config && strcmp(c->config, config)))
        return false;
    if (argc < 2) return true;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], c->name)) return true;
    return false;
}

int main(int argc, char **argv)
{
    int n = 0, bad = 0;

    if (argc > 2 && !strcmp(argv[1], "--config")) {
        config = argv[2];
        argv[2] = argv[0];
        argc -= 2;
        argv += 2;
    }

    if (!ct_driver_open()) {
        printf("Bail out! no window manager to test on the %s driver\n",
               ct_driver_name());
        return 2;
    }

    for (const ct_case *c = ct_cases; c->name; ++c)
        if (wanted(c, argc, argv)) ++n;
    printf("TAP version 13\n1..%d\n# driver: %s, configuration: %s\n", n,
           ct_driver_name(), config ? config : "default");

    n = 0;
    for (const ct_case *c = ct_cases; c->name; ++c) {
        if (!wanted(c, argc, argv)) continue;
        failed = skipped = false;
        c->fn();
        ct_settle();
        ++n;
        if (failed) ++bad;
        printf("%s %d - %s%s\n", failed ? "not ok" : "ok", n, c->name,
               skipped && !failed ? " # SKIP" : "");
        fflush(stdout);
    }

    ct_driver_close();
    return bad ? 1 : 0;
}
