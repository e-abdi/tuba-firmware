#ifndef APP_LIMITS_H
#define APP_LIMITS_H

#define STARTUP_TIMEOUT_SEC   600
#define UI_TICK_HZ            1

/* Avoid clashing with libc's LINE_MAX */
#define APP_LINE_MAX          48

/* Input guard for test durations */
#define TEST_MIN_SEC   (-10)
#define TEST_MAX_SEC   (10)

#endif /* APP_LIMITS_H */
