/* See beta_pin.h. */
#include "beta_pin.h"
#include "notify.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef SIM
#define PIN_DIR "/tmp/beta-pin"
#else
#define PIN_DIR "/mnt/data/beta-pin"
#endif
#define PIN_SCRIPT  PIN_DIR "/beta_guard.sh"
#define PIN_ENABLED PIN_DIR "/ENABLED"
#define PIN_NOTICE  PIN_DIR "/notice"

int beta_pin_installed(void) {
    return access(PIN_SCRIPT, X_OK) == 0;
}

int beta_pin_enabled(void) {
    return beta_pin_installed() && access(PIN_ENABLED, F_OK) == 0;
}

void beta_pin_set(int on) {
    if (!beta_pin_installed()) return;
    if (on) {
        FILE * f = fopen(PIN_ENABLED, "w");
        if (f) { fputs("on (toonui: update channel own build)\n", f); fclose(f); }
    } else {
        unlink(PIN_ENABLED);
    }
    sync();
    fprintf(stderr, "[beta-pin] guard %s\n", on ? "on" : "off");
}

/* The guard writes one line to `notice` when it put the beta back; the toonui
 * it restored posts it to the inbox (happ_usermsg) once BoxTalk is up. */
static void * notice_thread(void * arg) {
    (void)arg;
    sleep(45);
    for (;;) {
        FILE * f = fopen(PIN_NOTICE, "r");
        if (f) {
            char line[200] = {0};
            if (!fgets(line, sizeof line, f)) line[0] = 0;
            fclose(f);
            line[strcspn(line, "\r\n")] = 0;
            /* One message, not one per restore: drop the previous first. */
            if (line[0]) notify_clear("system", "beta_pin");
            if (line[0] && notify_show("system", "beta_pin", line) == 0)
                unlink(PIN_NOTICE);
        }
        sleep(60);
    }
    return NULL;
}

void beta_pin_start(void) {
    if (!beta_pin_installed()) return;
    pthread_t t;
    if (pthread_create(&t, NULL, notice_thread, NULL) == 0) pthread_detach(t);
}
