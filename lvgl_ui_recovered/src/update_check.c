/*
 * Background update checker. Polls
 *   https://api.github.com/repos/ToonSoftwareCollective/freetoon-lvgl/releases/latest
 * every 6 hours, compares the returned tag_name against BUILD_VERSION,
 * sets g_update_state.* so the home tile can render a "v0.7.x available"
 * banner. Optional release notes (body field) are stored too for the
 * tap-to-show modal.
 */
#include "update_check.h"
#include "http.h"
#include "settings.h"
#include "notify.h"
#include "app_updates.h"
void ui_request_restart(void);   /* ui_stack.c (screens.h pulls in LVGL) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UPDATE_CHECK_INTERVAL_S (24 * 3600)  /* 24 h (once a day) between polls */
/* Update channels (settings.update_channel; 2 = own build, see below):
 *   1 = beta/dev (default): newest release INCLUDING prereleases. We fetch a
 *       page of recent releases and pick the HIGHEST SEMVER tag ourselves —
 *       NOT index 0 — because GitHub orders /releases by created_at, so a
 *       later edit/re-create of an older release (e.g. v0.9.8) could otherwise
 *       surface as "newest" and mask v0.9.13+.
 *   0 = stable/official only: /releases/latest, which GitHub returns only for
 *       non-prerelease, non-draft releases (404 → no update, banner stays off).
 * All freetoon releases are currently beta, so "stable" finds nothing until a
 * non-prerelease is published. */
/* per_page kept modest: the asset-metadata blob per release is large and the
 * shared http_fetch caps the transfer at a few seconds, so a 30-release page
 * could run past the timeout on the Toon's slow link and fail the whole check.
 * 10 newest is more than enough to find the highest semver even when GitHub's
 * created_at order is scrambled by a re-created older release. */
#define RELEASES_API_BETA   "https://api.github.com/repos/ToonzDev/freetoon-lvgl/releases?per_page=5"
#define RELEASES_API_STABLE "https://api.github.com/repos/ToonzDev/freetoon-lvgl/releases/latest"
/* 2 = own build ("Eigen bouw"): a privately built beta, pinned on the Toon by
 * scripts/beta_guard.sh. Only releases of the owner's fork are looked at, so
 * the banner never offers an official build that would replace it. The fork
 * has no releases until its owner publishes one: then this is simply quiet. */
#define OWN_REPO            "DanceM0nkey/freetoon-lvgl"
#define RELEASES_API_OWN    "https://api.github.com/repos/" OWN_REPO "/releases?per_page=5"
#define OFFICIAL_REPO       "ToonzDev/freetoon-lvgl"

/* Lightweight connectivity probe used to tell "the GitHub API itself is
 * unreachable / rate-limited / blocked" apart from "the Toon has no internet
 * at all". This is the SAME reliable host the working tiles can reach over
 * plain HTTP, so a green here means general internet is fine even when the
 * api.github.com fetch returned nothing (e.g. Pi-Hole/proxy quirks, the 60/hr
 * unauthenticated GitHub API rate-limit, or a transfer that ran past the
 * fetch timeout). When this succeeds we must NOT report "check internet". */
#define CONNECTIVITY_PROBE_URL "http://detectportal.firefox.com/success.txt"

update_state_t g_update_state = {0};

/* 1 if real internet is reachable right now, 0 otherwise. Cheap captive-portal
 * style probe (small body, plain HTTP, no TLS handshake to gate on). */
static int internet_reachable(void) {
    char tmp[64];
    int rc = http_fetch(CONNECTIVITY_PROBE_URL, tmp, sizeof tmp);
    return (rc == 0 && strstr(tmp, "success")) ? 1 : 0;
}

/* Pull "key":"value" out of a JSON blob. Brittle but jq-free. */
static int json_extract_str(const char * src, const char * key,
                            char * out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char * p = strstr(src, pat);
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\n' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < outsz) {
        /* Tolerate JSON-escaped chars in the body (we mostly care about
         * \n in release notes). Pass them through; the renderer can
         * handle them or strip. */
        if (*p == '\\' && p[1]) {
            if (p[1] == 'n' && n + 1 < outsz) {
                out[n++] = '\n';
                p += 2;
                continue;
            }
            if (p[1] == '\\' && n + 1 < outsz) {
                out[n++] = '\\';
                p += 2;
                continue;
            }
            if (p[1] == '"' && n + 1 < outsz) {
                out[n++] = '"';
                p += 2;
                continue;
            }
            p++;   /* skip unknown escape */
            continue;
        }
        out[n++] = *p++;
    }
    out[n] = 0;
    return 1;
}

/* Strip a trailing /Z-suffix etc. — empty/whitespace strings get treated
 * as "missing". Used so notes containing nothing don't get rendered as
 * "release notes:" with an empty body. */
static int nonempty(const char * s) {
    if (!s) return 0;
    while (*s) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') return 1;
        s++;
    }
    return 0;
}

/* Return 1 if `tag` represents a newer release than the running build.
 * Both strings are expected to be of the form "v0.7.2" or similar — we
 * just check string-inequality after trimming the leading 'v', so the
 * comparison treats "v0.7.10" > "v0.7.2" the same way semver does
 * lexically (good enough — we don't expect to ship >9 minors). */
static void parse_ver(const char * s, int * a, int * b, int * c) {
    if (*s == 'v' || *s == 'V') s++;
    *a = *b = *c = 0;
    /* sscanf stops at the first non-digit/dot, so any `-dirty` / `-3-gabc`
     * git-describe suffix on the running build is ignored. */
    sscanf(s, "%d.%d.%d", a, b, c);
}

/* Own-build label: "1.0.12-beta.lourens.3" -> 3. Returns -1 when the version
 * carries no "-beta." part (an official release, or a git-describe dev
 * build such as "1.0.12-32-gc588be6", which keeps comparing as before). */
static int beta_build_no(const char * s) {
    const char * p = strstr(s, "-beta.");
    if (!p) return -1;
    const char * dot = strrchr(p, '.');
    int n = 0;
    if (dot && dot[1] >= '0' && dot[1] <= '9') n = atoi(dot + 1);
    return n;
}

/* >0 if a newer than b, <0 older, 0 equal. major.minor.patch first; on a tie
 * a beta build counts as newer than one without a beta label (it was built
 * on top of that release), and two beta builds compare by their number. */
static int ver_cmp(const char * a, const char * b) {
    int aa, ab, ac, ba, bb, bc;
    parse_ver(a, &aa, &ab, &ac);
    parse_ver(b, &ba, &bb, &bc);
    if (aa != ba) return aa - ba;
    if (ab != bb) return ab - bb;
    if (ac != bc) return ac - bc;
    int na = beta_build_no(a), nb = beta_build_no(b);
    if (na < 0 && nb < 0) return 0;
    if (na < 0) return -1;
    if (nb < 0) return 1;
    return na - nb;
}

/* Return 1 ONLY when `tag` is a strictly NEWER release than the running build.
 * Equal or older -> 0, so we never offer a downgrade. */
static int is_newer_than_build(const char * tag) {
    if (!nonempty(tag)) return 0;
    return ver_cmp(tag, BUILD_VERSION) > 0;
}

void update_check_now(void) {
    if (!settings.update_check_enabled) return;
    /* GitHub's /releases/latest payload runs ~8-12 KB once asset metadata
     * is in there; bump the buffer to 32 KB so curl doesn't EPIPE when we
     * close the read pipe before it's finished writing. */
    /* 512 KB: a 10-release listing with full asset metadata runs ~30 KB each,
     * so ~300 KB — this keeps comfortable headroom so the changelog isn't
     * truncated mid-release. http_fetch caps at the buffer and treats a full
     * buffer as success, so even an over-long payload keeps the newest releases
     * (they're first in the JSON). Longer timeout for the bigger transfer. */
    static char body[524288];
    body[0] = 0;
    const char * api = settings.update_channel == 0 ? RELEASES_API_STABLE
                     : settings.update_channel == 2 ? RELEASES_API_OWN
                     : RELEASES_API_BETA;
    const char * repo = settings.update_channel == 2 ? OWN_REPO : OFFICIAL_REPO;
    /* TLS-verified: this decides which release auto-update installs. */
    int rc = http_fetch_secure(api, body, sizeof body, 20);
    g_update_state.last_check_epoch = (long)time(NULL);
    /* http_fetch returns 0 on success (not byte count). Treat anything
     * non-zero as failure. body[] is also empty after a failure since
     * we cleared it pre-call. */
    if (rc != 0 || body[0] == 0) {
        /* The api.github.com fetch failed — but that is NOT proof the Toon is
         * offline. The GitHub *API* host is the flaky one: unauthenticated
         * requests are rate-limited (60/hr → 403/empty), a Pi-Hole/proxy can
         * single it out, and the larger /releases payload can run past the
         * fetch timeout on a slow link. Meanwhile buienradar/news/waste/
         * domoticz all fetch fine. So only report "check internet" when a real
         * connectivity probe ALSO fails; otherwise treat it as "couldn't reach
         * the update server right now" without the false offline banner. */
        g_update_state.last_check_ok = internet_reachable();
        fprintf(stderr, "[update] github fetch failed (rc=%d), internet=%d\n",
                rc, g_update_state.last_check_ok);
        return;
    }
    g_update_state.last_check_ok = 1;

    char tag[UPDATE_VERSION_MAX] = {0};
    char url[UPDATE_URL_MAX]     = {0};
    char notes[UPDATE_NOTES_MAX] = {0};

    if (settings.update_channel == 0) {
        /* /releases/latest → a single release object. */
        json_extract_str(body, "tag_name", tag,   sizeof tag);
        json_extract_str(body, "html_url", url,   sizeof url);
        json_extract_str(body, "body",     notes, sizeof notes);
        snprintf(g_update_state.changelog, sizeof g_update_state.changelog,
                 "%s\n%s\n", tag, nonempty(notes) ? notes : "(no notes)");
    } else {
        /* Beta and own-build channels: body is an array of releases. Walk every "tag_name",
         * keep the highest semver for the banner (don't trust GitHub's date
         * order), AND append each release's notes to the changelog (in the
         * order returned — newest first) for the About modal's history view. */
        const char * best_at = NULL;
        const char * pos = body;
        char cur[UPDATE_VERSION_MAX];
        char rbody[1600];
        const char * t;
        char * cl = g_update_state.changelog;
        size_t cl_cap = sizeof g_update_state.changelog, cl_len = 0;
        int cl_full = 0;
        cl[0] = 0;
        while ((t = strstr(pos, "\"tag_name\"")) != NULL) {
            if (json_extract_str(t, "tag_name", cur, sizeof cur) && cur[0]) {
                if (!tag[0] || ver_cmp(cur, tag) > 0) {
                    snprintf(tag, sizeof tag, "%s", cur);
                    best_at = t;
                }
                if (!cl_full) {
                    rbody[0] = 0;
                    json_extract_str(t, "body", rbody, sizeof rbody);
                    int rem = (int)(cl_cap - cl_len);
                    int w = snprintf(cl + cl_len, (size_t)rem, "%s%s\n%s\n",
                                     cl_len ? "\n" : "", cur,
                                     nonempty(rbody) ? rbody : "(no notes)");
                    if (w > 0 && w < rem) cl_len += (size_t)w;
                    else { cl[cl_len] = 0; cl_full = 1; }   /* stop at first that won't fit */
                }
            }
            pos = t + 10;
        }
        if (tag[0]) {
            /* html_url is deterministic — build it instead of parsing the
             * matching release object out of the array (brittle with nested
             * author/assets objects). */
            snprintf(url, sizeof url,
                "https://github.com/%s/releases/tag/%s", repo, tag);
            /* notes = the "body" field that follows this release's tag_name. */
            if (best_at) json_extract_str(best_at, "body", notes, sizeof notes);
        }
    }

    if (is_newer_than_build(tag)) {
        snprintf(g_update_state.latest_version,
                 sizeof g_update_state.latest_version, "%s", tag);
        snprintf(g_update_state.release_url,
                 sizeof g_update_state.release_url, "%s", url);
        snprintf(g_update_state.release_notes,
                 sizeof g_update_state.release_notes, "%s", notes);
        if (!g_update_state.available) {
            fprintf(stderr, "[update] new version available: %s (running %s)\n",
                    tag, BUILD_VERSION);
        }
        g_update_state.available = 1;
    } else {
        if (g_update_state.available)
            fprintf(stderr, "[update] caught up to %s\n", BUILD_VERSION);
        g_update_state.available = 0;
    }
}

/* Kick off the on-device self-installer (detached) + a Toon banner. Used by
 * the nightly auto-update and the "Update now" button. */
void update_install_now(void) {
    char msg[96];
    snprintf(msg, sizeof msg, "Updaten naar %s...",
             g_update_state.latest_version[0] ? g_update_state.latest_version : "nieuwste versie");
    notify_show("update", "freetoon", msg);
    fprintf(stderr, "[update] installing now: %s\n", g_update_state.latest_version);
    if (settings.update_channel == 2) {
        /* Own build: install the found fork release with the installer from
         * that same tag, never the official one from ToonzDev main. The
         * beta guard recognises the "-beta." label and pins the new build. */
        const char * tag = g_update_state.latest_version;
        for (const char * c = tag; *c; c++) {
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
                  (*c >= 'A' && *c <= 'Z') || *c == '.' || *c == '-' || *c == '_')) {
                fprintf(stderr, "[update] refusing odd tag name\n");
                return;
            }
        }
        if (!tag[0]) return;
        char cmd[640];
        snprintf(cmd, sizeof cmd,
            "nohup sh -c ': > /var/volatile/tmp/selfinstall.log; sleep 2; curl -fsSL -4 "
            "https://raw.githubusercontent.com/" OWN_REPO "/%s/install.sh "
            "| env FORCE_DOWNLOAD=1 REPO=" OWN_REPO " sh -s -- --update' "
            ">> /var/volatile/tmp/selfinstall.log 2>&1 &", tag);
        system(cmd);
        return;
    }
    system("nohup sh -c ': > /var/volatile/tmp/selfinstall.log; sleep 2; curl -fsSL -4 "
           "https://raw.githubusercontent.com/ToonzDev/freetoon-lvgl/main/install.sh "
           "| env FORCE_DOWNLOAD=1 sh -s -- --update' >> /var/volatile/tmp/selfinstall.log 2>&1 &");
}

static void * update_thread(void * arg) {
    (void)arg;
    /* Stagger the first probe by 30 s so we don't fight with boot-time
     * network setup on the Toon (wifi association can take a moment). */
    sleep(30);
    update_check_now();
    app_updates_check_now();      /* marketplace apps, same daily cadence */
    long last_check = (long)time(NULL);
    int  last_auto_yday = -1;     /* so a manual reboot at the hour doesn't double-fire */
    while (1) {
        sleep(60);                /* wake every minute to service the auto-update window */
        long now = (long)time(NULL);
        if (now - last_check >= UPDATE_CHECK_INTERVAL_S) {
            update_check_now();
            app_updates_check_now();
            last_check = now;
        }
        /* Nightly auto-update: once per day, at the configured hour. Apps
         * first (the helper runs synchronously), then FreeToon itself — its
         * installer is detached and restarts the UI on its own. A new plugin
         * .so is only loaded at start-up, so restart when only apps changed. */
        int want_ft   = settings.auto_update_enabled && g_update_state.available;
        int want_apps = settings.auto_update_apps && app_updates_get(NULL, 0) > 0;
        if (want_ft || want_apps) {
            time_t t = time(NULL); struct tm tm;
            localtime_r(&t, &tm);
            if (tm.tm_hour == settings.auto_update_hour && tm.tm_yday != last_auto_yday) {
                last_auto_yday = tm.tm_yday;
                int apps_done = want_apps ? app_updates_install_all() : 0;
                if (want_ft) {
                    update_install_now();
                } else if (apps_done > 0) {
                    char msg[64];
                    snprintf(msg, sizeof msg, "%d app-update(s) geinstalleerd", apps_done);
                    notify_show("update", "freetoon", msg);
                    fprintf(stderr, "[apps] %d app(s) updated — restarting UI\n", apps_done);
                    sleep(3);
                    ui_request_restart();
                }
            }
        }
    }
    return NULL;
}

int update_check_start(void) {
    static volatile int started = 0;
    if (!settings.update_check_enabled) {
        fprintf(stderr, "[update] checks disabled in settings\n");
        return 0;
    }
    /* Once: at boot, or from the Update modal when an auto-update switch
     * turns checks back on (update_check_enable()). */
    if (!__sync_bool_compare_and_swap(&started, 0, 1)) return 0;
    pthread_t t;
    if (pthread_create(&t, NULL, update_thread, NULL) != 0) { started = 0; return -1; }
    pthread_detach(t);
    fprintf(stderr, "[update] checker started (running %s, every %d s)\n",
            BUILD_VERSION, UPDATE_CHECK_INTERVAL_S);
    return 0;
}
