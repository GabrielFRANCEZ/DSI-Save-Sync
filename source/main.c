// SPDX-License-Identifier: MIT
//
// DSi Save Sync - synchronise des sauvegardes entre deux consoles en NiFi.
//
// Ordre des ecrans : on choisit son role, les deux consoles se connectent,
// PUIS l'hote choisit la sauvegarde et l'annonce au client. C'est ce qui
// garantit que les deux consoles parlent bien du meme jeu.
//
// Voir ../TUTORIAL.md.

#include <fat.h>
#include <dswifi9.h>
#include <nds.h>

#include <stdio.h>
#include <string.h>

#include "savescan.h"
#include "netsync.h"

static PrintConsole topScreen;
static PrintConsole bottomScreen;

// Tried in order when there is no config file. TWiLight Menu++ keeps DS saves
// in /roms/nds/saves by default; the others cover the layouts people end up
// with. Scanning is recursive, so the broader entries at the end also catch
// saves sitting in sub-folders.
static const char *const DEFAULT_ROOTS[] = {
    "/roms/nds/saves",
    "/saves",
    "/roms/nds",
    "/roms",
};
#define DEFAULT_ROOTS_COUNT ((int)(sizeof(DEFAULT_ROOTS) / sizeof(DEFAULT_ROOTS[0])))

// Bounded so that root + '/' + a save name always fits in a SaveEntry's
// base_path, otherwise a long root would silently truncate a path and we'd
// end up reading or overwriting the wrong file.
#define ROOT_PATH_MAX (SAVE_BASE_MAX - SAVE_NAME_MAX - 2)

static char g_root[ROOT_PATH_MAX] = "";
static bool g_root_from_config = false;
static SaveEntry g_entries[MAX_SAVE_ENTRIES];
static int g_entry_count = 0;

static void load_config(void)
{
    FILE *f = fopen("/_nds/savesync/config.txt", "r");
    if (f == NULL)
        return;

    char line[ROOT_PATH_MAX];
    if (fgets(line, sizeof(line), f) != NULL)
    {
        char *start = line;

        // A file saved from Notepad starts with a UTF-8 BOM, which would
        // otherwise become part of the path and make it unopenable.
        if ((u8)start[0] == 0xEF && (u8)start[1] == 0xBB && (u8)start[2] == 0xBF)
            start += 3;

        while (*start == ' ' || *start == '\t')
            start++;

        size_t len = strlen(start);
        while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' ||
                           start[len - 1] == ' '  || start[len - 1] == '\t'))
            start[--len] = '\0';

        if (len > 0)
        {
            snprintf(g_root, sizeof(g_root), "%s", start);
            g_root_from_config = true;
        }
    }
    fclose(f);
}

static void rescan(void)
{
    if (g_root_from_config)
    {
        g_entry_count = scan_saves(g_root, g_entries, MAX_SAVE_ENTRIES);
        return;
    }

    // No config file: try the usual places and keep the first one that
    // actually holds saves, so a fresh card works with no setup at all.
    for (int i = 0; i < DEFAULT_ROOTS_COUNT; i++)
    {
        g_entry_count = scan_saves(DEFAULT_ROOTS[i], g_entries, MAX_SAVE_ENTRIES);
        if (g_entry_count > 0)
        {
            snprintf(g_root, sizeof(g_root), "%s", DEFAULT_ROOTS[i]);
            return;
        }
    }

    snprintf(g_root, sizeof(g_root), "%s", DEFAULT_ROOTS[0]);
    g_entry_count = 0;
}

// Finds the entry the host announced. If this console doesn't have that game
// at all, builds an entry pointing at the scan root so the save can simply be
// copied over as a new file.
static void resolve_entry(const char *name, SaveKind kind, SaveEntry *out, bool *is_new)
{
    for (int i = 0; i < g_entry_count; i++)
    {
        if (strcmp(g_entries[i].display_name, name) == 0)
        {
            *out = g_entries[i];
            *is_new = false;
            return;
        }
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->base_path, SAVE_BASE_MAX, "%s/%s", g_root, name);
    snprintf(out->display_name, SAVE_NAME_MAX, "%s", name);
    out->kind = kind;
    *is_new = true;
}

// Nothing found is the one failure a user can't debug without being told
// where we actually looked.
static void print_where_we_looked(void)
{
    if (g_root_from_config)
    {
        printf("Dossier configure :\n  %s\n\n", g_root);
        printf("Corrige le chemin dans\n/_nds/savesync/config.txt\n");
        return;
    }

    printf("Dossiers essayes :\n");
    for (int i = 0; i < DEFAULT_ROOTS_COUNT; i++)
        printf("  %s\n", DEFAULT_ROOTS[i]);
    printf("\nPour en imposer un autre,\ncree /_nds/savesync/config.txt\navec le chemin dedans.\n");
}

// Menu de choix du jeu, cote hote. `keep_link` maintient la session NiFi
// pendant que l'utilisateur navigue. Renvoie -1 si B est presse.
static int menu_pick_entry(bool keep_link)
{
    int sel = 0;

    while (1)
    {
        swiWaitForVBlank();

        if (keep_link)
            netsync_host_keepalive(NULL);

        consoleSelect(&bottomScreen);
        consoleClear();

        printf("Choisis la sauvegarde\n");
        printf("a synchroniser :\n\n");

        if (g_entry_count == 0)
        {
            printf("Aucune sauvegarde trouvee.\n\n");
            print_where_we_looked();
            printf("\nB: retour\n");
        }

        int first = sel - 3;
        if (first < 0)
            first = 0;
        int last = first + 7;
        if (last >= g_entry_count)
            last = g_entry_count - 1;

        for (int i = first; i <= last; i++)
        {
            printf("%s %s %.20s\n", i == sel ? "->" : "  ",
                   g_entries[i].kind == SAVE_KIND_DSIWARE ? "[DSi]" : "[DS] ",
                   g_entries[i].display_name);
        }

        if (g_entry_count > 0)
            printf("\nA: valider   B: retour\n");

        scanKeys();
        u16 keys = keysDown();

        if (keys & KEY_UP)
            sel--;
        if (keys & KEY_DOWN)
            sel++;
        if (sel < 0)
            sel = 0;
        if (g_entry_count > 0 && sel >= g_entry_count)
            sel = g_entry_count - 1;

        if (keys & KEY_B)
            return -1;

        if ((keys & KEY_A) && g_entry_count > 0)
            return sel;
    }
}

static void show_message(const char *title, const char *msg)
{
    consoleSelect(&bottomScreen);
    consoleClear();

    printf("== %s ==\n\n%s\n\n", title, msg);
    printf("A: retour au menu\n");

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_A)
            return;
    }
}

static void show_result(SyncResult r, const char *msg)
{
    consoleSelect(&bottomScreen);
    consoleClear();

    const char *title;
    switch (r)
    {
        case SYNC_RESULT_OK_ALREADY_SYNCED: title = "DEJA A JOUR";    break;
        case SYNC_RESULT_OK_SENT:           title = "ENVOYE";         break;
        case SYNC_RESULT_OK_RECEIVED:       title = "RECU";           break;
        case SYNC_RESULT_CONFLICT:          title = "CONFLIT";        break;
        case SYNC_RESULT_CANCELLED:         title = "ANNULE";         break;
        case SYNC_RESULT_NO_PEER:           title = "PAS DE CONSOLE"; break;
        default:                            title = "ERREUR";         break;
    }

    printf("== %s ==\n\n%s\n\n", title, msg);
    printf("A: retour au menu\n");

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_A)
            return;
    }
}

static void status(const char *line)
{
    consoleSelect(&bottomScreen);
    consoleClear();
    printf("%s\n", line);
}

static void run_as_host(void)
{
    char msg[160];

    status("Mode HEBERGER\n\nEn attente de l'autre\nconsole...\n\n(elle doit choisir\nREJOINDRE)\n\nB: annuler");

    if (!netsync_host_connect(msg, sizeof(msg)))
    {
        netsync_end();
        show_message("HEBERGEMENT ARRETE", msg);
        return;
    }

    int idx = menu_pick_entry(true);
    if (idx < 0)
    {
        netsync_end();
        return;
    }

    SaveEntry *entry = &g_entries[idx];

    status("Lecture de la sauvegarde...");
    SyncMeta meta;
    refresh_local_meta(entry, &meta);

    status("Synchronisation...");
    SyncResult r = netsync_sync(true, entry, &meta, msg, sizeof(msg));
    netsync_end();

    show_result(r, msg);
}

static void run_as_client(void)
{
    char msg[160];

    status("Mode REJOINDRE\n\nRecherche de l'autre\nconsole...\n\nB: annuler");

    if (!netsync_client_connect(msg, sizeof(msg)))
    {
        netsync_end();
        show_message("CONNEXION ECHOUEE", msg);
        return;
    }

    status("Connecte !\n\nEn attente du choix de\nl'autre console...\n\nB: annuler");

    char name[SAVE_NAME_MAX];
    SaveKind kind;
    if (!netsync_client_wait_selection(name, sizeof(name), &kind, msg, sizeof(msg)))
    {
        netsync_end();
        show_message("ARRETE", msg);
        return;
    }

    SaveEntry entry;
    bool is_new;
    resolve_entry(name, kind, &entry, &is_new);

    consoleSelect(&bottomScreen);
    consoleClear();
    printf("Jeu choisi par l'autre\nconsole :\n\n  %.24s\n\n", name);
    if (is_new)
        printf("(absent ici, il sera copie)\n\n");
    printf("Synchronisation...\n");

    SyncMeta meta;
    refresh_local_meta(&entry, &meta);

    SyncResult r = netsync_sync(false, &entry, &meta, msg, sizeof(msg));
    netsync_end();

    show_result(r, msg);
}

int main(int argc, char *argv[])
{
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);

    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&topScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottomScreen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 3, false, true);

    consoleSelect(&topScreen);
    consoleArm7Setup(&topScreen, 1024);

    printf("DSi Save Sync\n\n");

    if (!fatInitDefault())
    {
        printf("Erreur: carte SD illisible\n");
        goto halt;
    }

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
    {
        printf("Erreur: Wi-Fi indisponible\n");
        goto halt;
    }

    load_config();
    rescan();

    printf("Dossier : %s%s\n", g_root, g_root_from_config ? " (config)" : "");
    printf("%d sauvegarde(s) trouvee(s)\n", g_entry_count);
    printf("Paquet NiFi : %d octets\n", netsync_packet_size());

    if (g_entry_count == 0)
    {
        printf("\n");
        print_where_we_looked();
    }

    while (1)
    {
        consoleSelect(&bottomScreen);
        consoleClear();
        printf("DSi Save Sync\n\n");
        printf("Sur UNE console :\n");
        printf("  X: HEBERGER\n\n");
        printf("Sur l'AUTRE :\n");
        printf("  Y: REJOINDRE\n\n");
        printf("L'hebergeur choisira le jeu\nune fois les deux consoles\nconnectees.\n\n");
        printf("R: re-scanner les saves\n");
        printf("START: quitter\n");

        swiWaitForVBlank();
        scanKeys();
        u16 keys = keysDown();

        if (keys & KEY_X)
            run_as_host();
        else if (keys & KEY_Y)
            run_as_client();
        else if (keys & KEY_R)
            rescan();
        else if (keys & KEY_START)
            break;
    }

halt:
    consoleSelect(&bottomScreen);
    consoleClear();
    printf("Fin. START pour quitter.\n");

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            break;
    }

    return 0;
}
