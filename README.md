# DSi Save Sync

Homebrew `.nds` qui synchronise des sauvegardes DS/DSiWare entre deux
consoles via NiFi (Wi-Fi local sans borne), pensé pour tourner depuis
TWiLight Menu++ / Unlaunch. Pas de serveur.

Voir [`TUTORIAL.md`](TUTORIAL.md) pour l'installation de la chaîne de
compilation (BlocksDS), la compilation, les tests dans melonDS et le
déploiement sur console réelle.

## Structure

- [`source/crc32.c`](source/crc32.c) — CRC32 d'un buffer ou d'un fichier.
- [`source/savescan.c`](source/savescan.c) — scan des `.sav`/`.pub`/`.prv`
  sur la carte SD, gestion du fichier de métadonnées `.syncmeta` (compteur
  de version + CRC, pas de dépendance à l'horloge RTC).
- [`source/netsync.c`](source/netsync.c) — transport NiFi (DSWiFi
  multijoueur local) et logique de décision (qui pousse quoi, détection de
  conflit).
- [`source/main.c`](source/main.c) — menu (écran du bas) et boucle
  principale.

## État

Code testé et fonctionnel sur deux DSi classique (non XL).
