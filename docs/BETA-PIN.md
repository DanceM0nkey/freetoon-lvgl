# Eigen bèta vastzetten

Met het updatekanaal **Eigen bouw** en de bèta-bewaker op de Toon kan een officiële
FreeToon-update een eigen bèta niet meer ongemerkt vervangen.

## Onderdelen

- **Updatekanaal "Eigen bouw"** (Update-venster → Kanaal). De updatecheck kijkt dan
  alleen naar de releases van de eigen fork (`DanceM0nkey/freetoon-lvgl`), nooit naar
  die van ToonzDev. De banner biedt dus geen officiële versie meer aan. Heeft de fork
  geen release, dan blijft de banner weg.
- **Versielabel** `1.0.12-beta.lourens.N`. Bouw met
  `make TARGET=toon2 BUILD_VERSION=1.0.12-beta.lourens.N`. De vergelijking telt eerst
  major.minor.patch en daarna het bètanummer. Een bèta gaat vóór de officiële versie
  met hetzelfde nummer.
- **Bewaker** `scripts/beta_guard.sh`, op de Toon in `/mnt/data/beta-pin/`, samen met
  kopieën van de vastgezette `toonui` en `quby_bridge`. Hij draait vanuit een eigen
  inittab-rij `btpn`. `install.sh` raakt alleen de rijen `toon`, `qbri`, `p1br` en
  `vncs` aan, dus de rij en de map blijven staan bij een officiële installatie.
  - Ziet hij dat `toonui` of `quby_bridge` vervangen is, dan wacht hij tot de installer
    klaar is (`/mnt/data/.freetoon_dl.*` weg, geen `install.sh` meer in `ps`). Hij
    zet de bèta terug, herstart de UI en de bridge, logt dat in `guard.log` en laat een
    inboxbericht achter.
  - Een nieuwe `toonui` met het label `-beta.lourens.` wordt overgenomen
    (vastgezet) en niet teruggedraaid.
  - Na een herstart doet hij dezelfde controle (binnen ongeveer 40 s).

## Bewust updaten

**Naar een nieuwe officiële versie:** kies in het Update-venster het kanaal *Stabiel* of
*Beta / dev*. Dat zet de bewaker uit. Druk daarna op Update. Op de Toon kan het ook zo:

```sh
/mnt/data/beta-pin/beta_guard.sh off
```

**Naar een nieuwe eigen bèta:**

1. Bouw met een hoger nummer: `BUILD_VERSION=1.0.12-beta.lourens.3`.
2. Zet de bèta op de Toon. Er zijn twee manieren:
   - Met de hand: kopieer `toonui` naar `/mnt/data/toonui` en herstart. De bewaker ziet
     het label en zet hem binnen ongeveer 40 s vast.
   - Via de updateknop: publiceer een release met dat label als tag op de fork, met
     dezelfde assets als een officiële release. Het kanaal *Eigen bouw* biedt hem dan
     aan en installeert hem met `install.sh` uit die tag.
3. Een nieuwe `quby_bridge` zonder nieuwe toonui zet je vast met
   `/mnt/data/beta-pin/beta_guard.sh pin`.

**Terug naar de bèta-bewaking:** kies het kanaal *Eigen bouw*, of op de Toon:
`/mnt/data/beta-pin/beta_guard.sh on`. Staat er inmiddels een officiële versie, dan
zet de bewaker de vastgezette bèta terug.

## Commando's

| Commando | Wat |
|---|---|
| `beta_guard.sh status` | aan/uit, inittab-rij, draaiende en vastgezette versie, laatste logregels |
| `beta_guard.sh off` / `on` | bewaking uit / aan (bestand `ENABLED`) |
| `beta_guard.sh pin` | zet vast wat nu in `/mnt/data` staat |
| `beta_guard.sh setup` | eerste installatie: vastzetten, script en inittab-rij |
| `beta_guard.sh remove` | uit en inittab-rij weg (de kopieën blijven) |
