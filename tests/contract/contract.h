/* Lumen contract tests: the case runner. */

#ifndef lumen__contract_contract_h
#define lumen__contract_contract_h

#include "driver.h"

typedef void (*ct_case_fn)(void);

typedef struct {
    const char *name;
    ct_case_fn fn;
    /* NULL for the default configuration (data/rc.xml), or the name of the
       change run.sh makes to it (configs/NAME.sed) before the case runs */
    const char *config;
} ct_case;

extern const ct_case ct_cases[];

/* Fail the running case when expr is false. */
#define CT_CHECK(expr) \
    ct__check((expr), #expr, __FILE__, __LINE__)

/* Fail the running case when expr has not become true within a while; the
   window manager answers asynchronously. */
#define CT_EVENTUALLY(expr) \
    ct__check(ct__eventually_begin() && ({ bool ct__ok;                \
        while (!(ct__ok = (expr)) && ct__eventually_again()) ;         \
        ct__ok; }), "eventually " #expr, __FILE__, __LINE__)

/* Skip the running case when the driver cannot do this. */
#define CT_REQUIRE(caps) \
    do { if ((ct_driver_caps() & (caps)) != (caps)) { \
        ct__skip(#caps); return; } } while (0)

/* Wait for the window manager to settle. */
void ct_settle(void);

bool ct_rect_eq(ct_rect a, ct_rect b);
bool ct_rect_contains(ct_rect outer, ct_rect inner);

void ct__check(bool ok, const char *what, const char *file, int line);
void ct__skip(const char *why);
bool ct__eventually_begin(void);
bool ct__eventually_again(void);

#endif
