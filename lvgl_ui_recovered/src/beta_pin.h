#ifndef TOON_BETA_PIN_H
#define TOON_BETA_PIN_H

/* Toonui side of the beta guard (scripts/beta_guard.sh, installed in
 * /mnt/data/beta-pin). The guard keeps a pinned own build of toonui and
 * quby_bridge in place: when an official install replaces them, it puts the
 * pinned copies back. This module only
 *   - switches the guard on/off (the ENABLED file), from the update channel,
 *   - shows the guard's notice in the inbox after it restored something.
 * Everything is a no-op on a Toon without /mnt/data/beta-pin. */

int  beta_pin_installed(void);   /* guard folder + script present */
int  beta_pin_enabled(void);     /* installed and switched on */
void beta_pin_set(int on);       /* on: ENABLED file; off: remove it */
void beta_pin_start(void);       /* background notice poster */

#endif
