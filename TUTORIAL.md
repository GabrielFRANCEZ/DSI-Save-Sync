# DSi Save Sync — tutoriel

Homebrew `.nds` qui synchronise les sauvegardes entre deux DSi via NiFi
(Wi-Fi local sans borne), lancé depuis TWiLight Menu++ / Unlaunch. Pas de
serveur, pas d'infra : les deux consoles se parlent directement.

**État.** Le ROM `savesync.nds` est compilé (BlocksDS 1.23.0, sans
avertissement) et démarre correctement sous melonDS : il lit la carte SD,
trouve les sauvegardes et affiche ses menus. Ce qui reste à valider : la
découverte NiFi entre deux consoles, et surtout le transfert lui-même, qui
n'a encore jamais tourné. **Ne le laisse pas toucher une vraie sauvegarde
avant d'avoir fait un transfert réussi sur des fichiers factices.**

## 1. Ce que fait l'appli

- Scanne un dossier de la carte SD à la recherche des `.sav` (jeux DS) et des
  paires `.pub`/`.prv` (DSiWare).
- Garde à côté de chaque sauvegarde un fichier `.syncmeta` : un compteur de
  version incrémenté dès que le CRC32 du fichier change, plus l'ID de la
  console. Aucune décision ne dépend de l'horloge RTC, qui dérive entre
  consoles ([`source/savescan.c`](source/savescan.c)).
- Tu choisis un jeu, puis "Héberger" sur une console et "Rejoindre" sur
  l'autre. L'appli compare les versions :
  - **versions différentes** → la plus récente écrase l'ancienne, l'ancienne
    étant d'abord copiée en `.bak` ;
  - **versions égales, contenus différents** (vrai conflit) → rien n'est
    écrasé : la copie de l'autre console arrive sous
    `NomDuJeu.sav.conflict-XXXXXXXX`, à toi de trancher ;
  - **identiques** → rien à faire.
- Transfert par blocs de 200 octets avec accusé de réception, et vérification
  CRC32 de bout en bout avant tout remplacement
  ([`source/netsync.c`](source/netsync.c)).

## 2. Compiler (déjà fait, pour rebuild après modification)

La chaîne de compilation est **déjà installée** sur ce PC. Pour recompiler,
ouvre "Wonderful Toolchain Shell" depuis le menu Démarrer et :

```bash
cd /c/Users/gabfr/projects/dsi-save-sync
make
```

Depuis un terminal quelconque (Git Bash, terminal VS Code) où l'environnement
Wonderful n'est pas configuré, utilise plutôt :

```bash
bash /c/Users/gabfr/projects/dsi-save-sync/build.sh
```

### Si tu dois réinstaller la chaîne un jour

Trois pièges rencontrés lors de l'installation, non documentés en amont :

1. **`wf-config` n'existe pas** dans la version actuelle du Wonderful
   Toolchain, alors que la doc BlocksDS dit de lancer
   `wf-config repo enable blocksds`. Il faut enregistrer le dépôt à la main
   en créant `/opt/wonderful/etc/pacman.d/99-final.conf` (soit
   `C:\msys64\opt\wonderful\etc\pacman.d\99-final.conf`) contenant :
   ```
   [blocksds]
   Server = https://blocksds.skylyrac.net/packages/rolling/windows/$arch/
   ```
   Attention à `windows` : le README du dépôt blocksds/packages indique
   `linux`, ce qui installe les binaires Linux et fait échouer l'édition du
   ROM avec `ndstool: cannot execute binary file`.
2. **`blocksds-toolchain` déclare une dépendance `runtime-musl` inexistante
   côté Windows** (bug de packaging en amont). Contournement : installer en
   ignorant la résolution de dépendances, puis installer le compilateur ARM
   séparément (lui s'installe normalement) :
   ```bash
   wf-pacman -S blocksds-toolchain -dd
   wf-pacman -S toolchain-gcc-arm-none-eabi
   ```
3. **`arm-none-eabi-gcc` échoue avec `Cannot create temporary file in
   C:\WINDOWS\`** si les variables `TMP`/`TEMP` ne lui parviennent pas. C'est
   ce que gère [`build.sh`](build.sh) ; dans le Wonderful Toolchain Shell le
   problème ne se pose pas.

## 3. Tester dans melonDS

melonDS permet de valider la partie réseau (découverte, connexion, échange
de métadonnées) sans toucher à une vraie console. Il ne permet **pas** de
tester un transfert complet — voir la limite ci-dessous.

### 3.1 Lancer deux instances (la bonne méthode)

**Ne lance pas deux `melonDS.exe`.** Depuis melonDS 1.0, le multijoueur local
(`LocalMP`) passe par des mutex et sémaphores internes au processus, sans
mémoire partagée : deux processus séparés ne se voient jamais. Le conseil
« faire deux copies du dossier melonDS » qu'on trouve sur les forums date de
melonDS 0.9.

La bonne méthode :

1. Lance **un seul** melonDS et charge `savesync.nds`.
2. Menu **System → Multiplayer → Launch new instance** : une deuxième fenêtre
   apparaît.
3. Charge `savesync.nds` dans cette deuxième fenêtre aussi.

Les deux instances ont des adresses MAC distinctes, donc elles se voient bien
comme deux consoles différentes côté Wi-Fi.

### 3.2 Limite : les deux instances partagent une seule carte SD

Les réglages DLDI de melonDS sont globaux au processus, pas par instance
(`getSDCardArgs("DLDI")` lit la configuration globale, sans suffixe
d'instance, contrairement au firmware ou à l'adresse MAC). Les deux consoles
émulées voient donc **les mêmes fichiers**, le même `.syncmeta` et le même
identifiant de console.

Conséquence : dans melonDS, le résultat attendu est `DEJA A JOUR`, et c'est
un succès. Ça valide le scan de la carte SD, le calcul des CRC, la
découverte NiFi, la connexion, l'annonce du jeu par l'hôte et la comparaison
des métadonnées — c'est-à-dire tout sauf le transfert lui-même.

Mets l'image DLDI en lecture seule (`ReadOnly = true`) pour ce test : deux
pilotes FAT émulés qui écrivent en même temps dans le même fichier image
finiraient par le corrompre.

Pour tester un vrai transfert et un conflit, il faut deux cartes SD
distinctes, donc soit les deux vraies consoles (voir §4, sans risque avec des
fichiers factices), soit le mode LAN de melonDS entre deux processus séparés
(chacun avec sa config et son dossier), non vérifié ici.

### 3.3 Ce qu'on vérifie dans melonDS

| # | Manip | Résultat attendu |
|---|---|---|
| 1 | Charger la ROM | L'écran du haut affiche le dossier scanné et le nombre de sauvegardes trouvées |
| 2 | X sur une instance | `Mode HEBERGER / En attente de l'autre console` |
| 3 | Y sur l'autre | Les deux passent en connecté en quelques secondes |
| 4 | L'hôte choisit un jeu | Le client affiche le nom choisi, puis les deux affichent `DEJA A JOUR` |
| 5 | Y sans hôte en face | Au bout d'environ 20 s : `Aucune console trouvee` |

Si l'étape 3 ne se fait pas, c'est la découverte NiFi qui est en cause :
vérifie que les deux fenêtres viennent bien du **même** processus melonDS
(§3.1) et que la ROM tourne dans les deux.

## 4. Déployer sur les deux DSi

1. Copie `savesync.nds` dans le dossier de homebrews de TWiLight Menu++ (par
   ex. `/roms/nds/`) sur **les deux** cartes SD.
2. Crée `/_nds/savesync/config.txt` sur les deux cartes, contenant le dossier
   à scanner, par exemple :
   ```
   /roms
   ```
   Ajuste selon l'emplacement réel de tes sauvegardes (réglage des chemins
   dans TWiLight Menu++).
3. Sur chaque DSi : Unlaunch → TWiLight Menu++ → "DSi Save Sync".

**Avant le premier essai sur du vrai matériel, fais une copie de la carte SD**
(ou au moins du dossier de sauvegardes) sur ton PC. L'appli fait des `.bak`,
mais une sauvegarde hors console coûte deux minutes et couvre le cas où un
bug la contourne.

## 5. Utiliser

1. HAUT/BAS pour naviguer, A pour choisir **le même jeu sur les deux
   consoles**.
2. **X** sur une console (héberge), **Y** sur l'autre (rejoint).
3. Résultat affiché : `DEJA A JOUR`, `ENVOYE`, `RECU`, `CONFLIT`,
   `PAS DE CONSOLE` ou `ERREUR`. B annule à tout moment sans rien écrire.

En cas de `CONFLIT`, les fichiers `.conflict-XXXXXXXX` se comparent depuis un
PC avec un lecteur de carte SD ; remplace toi-même le `.sav` si tu veux garder
la version de l'autre console.

## 6. Limites connues

- Un jeu à la fois, synchronisation lancée à la main (pas d'intégration
  automatique au lancement d'un jeu depuis TWiLight Menu++, ce qui
  demanderait de forker TWiLight Menu++ en C++).
- Les deux consoles doivent être allumées en même temps (c'est inhérent au
  NiFi ; l'alternative serait un serveur Wi-Fi).
- Un seul niveau de `.bak` : une deuxième synchro écrase la sauvegarde de
  secours précédente.
