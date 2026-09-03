// SPDX-License-Identifier: MIT
//
// DSi Save Sync - synchronise des sauvegardes entre deux consoles en NiFi.
//
// Ordre des ecrans : on choisit son role, les deux consoles se connectent,
// PUIS l'hote choisit une ou plusieurs sauvegardes et les annonce au client,
// une par une. C'est ce qui garantit que les deux consoles parlent bien du
// meme fichier a chaque etape.
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

// Selection de l'hote : quelles sauvegardes partent dans la serie.
static bool g_selected[MAX_SAVE_ENTRIES];

// Resultat de chaque sauvegarde de la serie, pour le recapitulatif final.
typedef struct {
    char name[SAVE_NAME_MAX];
    SyncResult result;
} BatchResult;

static BatchResult g_results[MAX_SAVE_ENTRIES];
static int g_result_count = 0;

// Contexte affiche pendant la synchro (le callback de progression n'a pas
// d'autre moyen de connaitre ces informations).
static char g_sync_name[SAVE_NAME_MAX] = "";
static int g_batch_index = 0;
static int g_batch_total = 0;

// ---------------------------------------------------------------------------
// Habillage
// ---------------------------------------------------------------------------

#define UI_WIDTH 30

static void ui_rule(PrintConsole *c, ConsoleColor color)
{
    consoleSetColor(c, color);
    for (int i = 0; i < UI_WIDTH; i++)
        printf("-");
    printf("\n");
    consoleSetColor(c, CONSOLE_DEFAULT);
}

// Ecran du haut : identite de l'appli et contexte courant. Il ne recoit
// jamais d'interaction, ce qui laisse l'ecran du bas entierement au menu.
static void ui_top(const char *context, const char *detail)
{
    consoleSelect(&topScreen);
    consoleClear();

    consoleSetColor(&topScreen, CONSOLE_LIGHT_CYAN);
    printf("\n  D S i   S A V E   S Y N C\n");
    consoleSetColor(&topScreen, CONSOLE_DEFAULT);
    ui_rule(&topScreen, CONSOLE_CYAN);

    if (context != NULL)
    {
        consoleSetColor(&topScreen, CONSOLE_LIGHT_YELLOW);
        printf("\n %.28s\n", context);
        consoleSetColor(&topScreen, CONSOLE_DEFAULT);
    }
    if (detail != NULL)
        printf(" %.28s\n", detail);
}

static ConsoleColor result_color(SyncResult r)
{
    switch (r)
    {
        case SYNC_RESULT_OK_ALREADY_SYNCED:
        case SYNC_RESULT_OK_SENT:
        case SYNC_RESULT_OK_RECEIVED:   return CONSOLE_LIGHT_GREEN;
        case SYNC_RESULT_CONFLICT:      return CONSOLE_LIGHT_YELLOW;
        case SYNC_RESULT_CANCELLED:     return CONSOLE_LIGHT_GRAY;
        default:                        return CONSOLE_LIGHT_RED;
    }
}

static const char *result_title(SyncResult r)
{
    switch (r)
    {
        case SYNC_RESULT_OK_ALREADY_SYNCED: return "DEJA A JOUR";
        case SYNC_RESULT_OK_SENT:           return "ENVOYE";
        case SYNC_RESULT_OK_RECEIVED:       return "RECU";
        case SYNC_RESULT_CONFLICT:          return "CONFLIT";
        case SYNC_RESULT_CANCELLED:         return "ANNULE";
        case SYNC_RESULT_NO_PEER:           return "PAS DE CONSOLE";
        default:                            return "ERREUR";
    }
}

// Version courte pour tenir en bout de ligne dans le recapitulatif.
static const char *result_tag(SyncResult r)
{
    switch (r)
    {
        case SYNC_RESULT_OK_ALREADY_SYNCED: return "a jour";
        case SYNC_RESULT_OK_SENT:           return "envoye";
        case SYNC_RESULT_OK_RECEIVED:       return "recu";
        case SYNC_RESULT_CONFLICT:          return "conflit";
        case SYNC_RESULT_CANCELLED:         return "annule";
        case SYNC_RESULT_NO_PEER:           return "absent";
        default:                            return "erreur";
    }
}

static void wait_key(u16 key)
{
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & key)
            return;
    }
}

// ---------------------------------------------------------------------------
// Configuration et scan
// ---------------------------------------------------------------------------

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
    memset(g_selected, 0, sizeof(g_selected));

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

// Nothing found is the one failure a user can't debug without being told
// where we actually looked.
static void print_where_we_looked(void)
{
    if (g_root_from_config)
    {
        printf("Dossier configure :\n  %.26s\n\n", g_root);
        printf("Corrige le chemin dans\n/_nds/savesync/config.txt\n");
        return;
    }

    printf("Dossiers essayes :\n");
    for (int i = 0; i < DEFAULT_ROOTS_COUNT; i++)
        printf("  %s\n", DEFAULT_ROOTS[i]);
    printf("\nPour en imposer un autre,\ncree /_nds/savesync/config.txt\n");
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

// ---------------------------------------------------------------------------
// Menu de selection multiple (cote hote)
// ---------------------------------------------------------------------------

static int selected_count(void)
{
    int n = 0;
    for (int i = 0; i < g_entry_count; i++)
        if (g_selected[i])
            n++;
    return n;
}

// Renvoie le nombre de sauvegardes cochees, ou -1 si l'utilisateur abandonne.
// `keep_link` maintient la session NiFi pendant la navigation.
static int menu_select_saves(bool keep_link)
{
    int sel = 0;
    const int rows = 9;

    while (1)
    {
        swiWaitForVBlank();

        if (keep_link)
            netsync_host_keepalive(NULL);

        consoleSelect(&bottomScreen);
        consoleClear();

        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_CYAN);
        printf("Sauvegardes a synchroniser\n");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
        ui_rule(&bottomScreen, CONSOLE_GRAY);

        if (g_entry_count == 0)
        {
            consoleSetColor(&bottomScreen, CONSOLE_LIGHT_RED);
            printf("Aucune sauvegarde trouvee.\n\n");
            consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
            print_where_we_looked();
            printf("\nB: retour\n");

            scanKeys();
            if (keysDown() & KEY_B)
                return -1;
            continue;
        }

        int first = sel - (rows / 2);
        if (first < 0)
            first = 0;
        if (first > g_entry_count - rows)
            first = g_entry_count - rows;
        if (first < 0)
            first = 0;

        int last = first + rows - 1;
        if (last >= g_entry_count)
            last = g_entry_count - 1;

        for (int i = first; i <= last; i++)
        {
            bool cursor = (i == sel);

            consoleSetColor(&bottomScreen,
                             cursor ? CONSOLE_LIGHT_CYAN : CONSOLE_DEFAULT);
            printf("%s", cursor ? ">" : " ");

            consoleSetColor(&bottomScreen,
                             g_selected[i] ? CONSOLE_LIGHT_GREEN : CONSOLE_GRAY);
            printf("[%c] ", g_selected[i] ? 'x' : ' ');

            consoleSetColor(&bottomScreen,
                             cursor ? CONSOLE_WHITE : CONSOLE_DEFAULT);
            printf("%.24s\n", g_entries[i].display_name);
        }
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

        if (g_entry_count > rows)
            printf(" ... %d/%d\n", sel + 1, g_entry_count);

        consoleSetCursor(&bottomScreen, 0, 19);
        ui_rule(&bottomScreen, CONSOLE_GRAY);

        int n = selected_count();
        consoleSetColor(&bottomScreen,
                         n > 0 ? CONSOLE_LIGHT_GREEN : CONSOLE_GRAY);
        printf(" %d selectionnee(s)\n", n);
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
        printf(" A: cocher   X: tout/rien\n");
        printf(" START: lancer   B: retour\n");

        scanKeys();
        u16 keys = keysDown();

        if (keys & KEY_UP)
            sel--;
        if (keys & KEY_DOWN)
            sel++;
        if (keys & KEY_LEFT)
            sel -= rows;
        if (keys & KEY_RIGHT)
            sel += rows;
        if (sel < 0)
            sel = 0;
        if (sel >= g_entry_count)
            sel = g_entry_count - 1;

        if (keys & KEY_A)
            g_selected[sel] = !g_selected[sel];

        if (keys & KEY_X)
        {
            bool all = (selected_count() == g_entry_count);
            for (int i = 0; i < g_entry_count; i++)
                g_selected[i] = !all;
        }

        if (keys & KEY_B)
            return -1;

        if ((keys & KEY_START) && selected_count() > 0)
            return selected_count();
    }
}

// ---------------------------------------------------------------------------
// Progression
// ---------------------------------------------------------------------------

static void draw_bar(u32 done, u32 total, ConsoleColor color)
{
    const int width = 22;
    int filled = 0;

    // done * width tient largement dans 32 bits pour des saves de quelques Mo.
    if (total > 0)
        filled = (int)((done * (u32)width) / total);
    if (filled > width)
        filled = width;

    printf(" ");
    consoleSetColor(&bottomScreen, color);
    printf("[");
    for (int i = 0; i < width; i++)
        printf(i < filled ? "#" : ".");
    printf("]\n");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

    u32 pct = (total > 0) ? (done * 100) / total : 0;
    printf("  %lu%%  %lu/%lu octets\n",
           (unsigned long)pct, (unsigned long)done, (unsigned long)total);
}

static void on_sync_progress(const SyncProgress *p)
{
    // Redessiner a 60 Hz coute du temps CPU au transfert pour rien : l'oeil
    // ne fait pas la difference a 10 images par seconde.
    static int throttle = 0;
    if (++throttle < 6)
        return;
    throttle = 0;

    consoleSelect(&bottomScreen);
    consoleClear();

    consoleSetColor(&bottomScreen, CONSOLE_LIGHT_CYAN);
    if (g_batch_total > 1)
        printf("Synchronisation  %d/%d\n", g_batch_index, g_batch_total);
    else
        printf("Synchronisation\n");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    ui_rule(&bottomScreen, CONSOLE_GRAY);

    printf(" %.28s\n\n", g_sync_name);

    if (p->phase == SYNC_PHASE_HANDSHAKE)
    {
        printf(" Comparaison des versions...\n");
    }
    else if (!p->sending && !p->receiving)
    {
        printf(" Verification...\n");
    }
    else
    {
        if (p->sending)
        {
            printf(" Envoi -->\n");
            draw_bar(p->sent_done, p->sent_total, CONSOLE_LIGHT_GREEN);
            printf("\n");
        }
        if (p->receiving)
        {
            printf(" Reception <--\n");
            draw_bar(p->recv_done, p->recv_total, CONSOLE_LIGHT_BLUE);
        }
    }

    // Au-dela d'une seconde sans paquet utile, afficher de quoi diagnostiquer
    // un blocage : ces deux chiffres disent si l'autre console parle d'une
    // autre sauvegarde (pair) et depuis combien de temps on n'entend rien.
    if (p->idle_frames > 60)
    {
        consoleSetCursor(&bottomScreen, 0, 19);
        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_RED);
        printf(" bloque depuis %ds\n", p->idle_frames / 60);
        printf(" pair sur sauvegarde n%d\n", p->peer_index);
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    }

    consoleSetCursor(&bottomScreen, 0, 22);
    consoleSetColor(&bottomScreen, CONSOLE_GRAY);
    printf(" B: annuler");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
}

// ---------------------------------------------------------------------------
// Ecrans de resultat
// ---------------------------------------------------------------------------

static void show_message(const char *title, const char *msg, ConsoleColor color)
{
    consoleSelect(&bottomScreen);
    consoleClear();

    consoleSetColor(&bottomScreen, color);
    printf("%s\n", title);
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    ui_rule(&bottomScreen, CONSOLE_GRAY);

    printf("\n%s\n", msg);

    consoleSetCursor(&bottomScreen, 0, 22);
    consoleSetColor(&bottomScreen, CONSOLE_GRAY);
    printf(" A: retour au menu");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

    wait_key(KEY_A);
}

static void record_result(const char *name, SyncResult r)
{
    if (g_result_count >= MAX_SAVE_ENTRIES)
        return;
    snprintf(g_results[g_result_count].name, SAVE_NAME_MAX, "%s", name);
    g_results[g_result_count].result = r;
    g_result_count++;
}

static void show_summary(void)
{
    consoleSelect(&bottomScreen);
    consoleClear();

    consoleSetColor(&bottomScreen, CONSOLE_LIGHT_CYAN);
    printf("Recapitulatif\n");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    ui_rule(&bottomScreen, CONSOLE_GRAY);

    if (g_result_count == 0)
    {
        printf("\n Rien n'a ete synchronise.\n");
    }
    else if (g_result_count == 1)
    {
        // Une seule sauvegarde : autant afficher le verdict en entier plutot
        // qu'une ligne de tableau.
        printf("\n %.28s\n\n", g_results[0].name);
        consoleSetColor(&bottomScreen, result_color(g_results[0].result));
        printf(" %s\n", result_title(g_results[0].result));
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    }

    int shown = (g_result_count == 1) ? 0 : g_result_count;
    if (shown > 16)
        shown = 16;

    for (int i = 0; i < shown; i++)
    {
        printf("%-18.18s ", g_results[i].name);
        consoleSetColor(&bottomScreen, result_color(g_results[i].result));
        printf("%s\n", result_tag(g_results[i].result));
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    }

    if (g_result_count > shown)
        printf("... et %d autre(s)\n", g_result_count - shown);

    // Un conflit demande une action manuelle : le rappeler explicitement.
    bool any_conflict = false;
    for (int i = 0; i < g_result_count; i++)
        if (g_results[i].result == SYNC_RESULT_CONFLICT)
            any_conflict = true;

    if (any_conflict)
    {
        printf("\n");
        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_YELLOW);
        printf("Conflit : voir les fichiers\n.conflict-* sur la carte SD\n");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    }

    consoleSetCursor(&bottomScreen, 0, 22);
    consoleSetColor(&bottomScreen, CONSOLE_GRAY);
    printf(" A: retour au menu");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

    wait_key(KEY_A);
}

// ---------------------------------------------------------------------------
// Parcours hote / client
// ---------------------------------------------------------------------------

static void run_as_host(void)
{
    char msg[160];

    ui_top("Mode HEBERGER", "En attente de l'autre console");
    consoleSelect(&bottomScreen);
    consoleClear();
    printf("\n L'autre console doit\n choisir REJOINDRE.\n\n");
    consoleSetColor(&bottomScreen, CONSOLE_GRAY);
    printf(" B: annuler\n");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

    if (!netsync_host_connect(msg, sizeof(msg)))
    {
        netsync_end();
        show_message("HEBERGEMENT ARRETE", msg, CONSOLE_LIGHT_RED);
        return;
    }

    ui_top("Connecte", "Choisis les sauvegardes");

    if (menu_select_saves(true) < 0)
    {
        netsync_host_batch_finished();
        netsync_end();
        return;
    }

    g_result_count = 0;
    g_batch_total = selected_count();
    g_batch_index = 0;

    for (int i = 0; i < g_entry_count; i++)
    {
        if (!g_selected[i])
            continue;

        // Ne pas annoncer la sauvegarde suivante tant que l'autre console
        // n'a pas fini la precedente, sinon les deux se parlent de fichiers
        // differents et plus rien n'avance.
        if (g_batch_index > 0)
        {
            ui_top("Synchronisation", "Attente de l'autre console");
            consoleSelect(&bottomScreen);
            consoleClear();
            printf("\n L'autre console termine la\n sauvegarde precedente...\n");

            if (!netsync_host_wait_peer_idle(msg, sizeof(msg)))
            {
                record_result("(suite)", SYNC_RESULT_NO_PEER);
                break;
            }
        }

        SaveEntry *entry = &g_entries[i];
        g_batch_index++;
        snprintf(g_sync_name, sizeof(g_sync_name), "%s", entry->display_name);

        char ctx[40];
        snprintf(ctx, sizeof(ctx), "Envoi %d/%d", g_batch_index, g_batch_total);
        ui_top(ctx, entry->display_name);

        SyncMeta meta;
        refresh_local_meta(entry, &meta);

        SyncResult r = netsync_sync(true, entry, &meta, g_batch_index, g_batch_total,
                                     on_sync_progress, msg, sizeof(msg));
        record_result(entry->display_name, r);

        // Plus de pair : inutile d'enchainer sur les suivantes.
        if (r == SYNC_RESULT_NO_PEER || r == SYNC_RESULT_CANCELLED)
            break;
    }

    netsync_host_batch_finished();
    netsync_end();

    ui_top("Termine", NULL);
    show_summary();
}

static void run_as_client(void)
{
    char msg[160];

    ui_top("Mode REJOINDRE", "Recherche de l'autre console");
    consoleSelect(&bottomScreen);
    consoleClear();
    printf("\n Recherche...\n\n");
    consoleSetColor(&bottomScreen, CONSOLE_GRAY);
    printf(" B: annuler\n");
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

    if (!netsync_client_connect(msg, sizeof(msg)))
    {
        netsync_end();
        show_message("CONNEXION ECHOUEE", msg, CONSOLE_LIGHT_RED);
        return;
    }

    g_result_count = 0;
    g_batch_index = 0;
    g_batch_total = 0;

    int last_index = 0;

    while (1)
    {
        ui_top("Connecte", "En attente de l'autre console");
        consoleSelect(&bottomScreen);
        consoleClear();
        printf("\n L'autre console choisit\n les sauvegardes...\n\n");
        consoleSetColor(&bottomScreen, CONSOLE_GRAY);
        printf(" B: annuler\n");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

        char name[SAVE_NAME_MAX];
        SaveKind kind;
        int index = 0, total = 0;
        bool batch_done = false;

        if (!netsync_client_wait_selection(name, sizeof(name), &kind, &index,
                                            &total, &batch_done, msg, sizeof(msg)))
        {
            netsync_end();
            if (g_result_count > 0)
            {
                ui_top("Interrompu", NULL);
                show_summary();
            }
            else
            {
                show_message("ARRETE", msg, CONSOLE_LIGHT_RED);
            }
            return;
        }

        if (batch_done)
            break;

        // L'hote re-annonce la sauvegarde courante a chaque image : on ignore
        // celles qu'on a deja traitees.
        if (index <= last_index)
            continue;
        last_index = index;

        g_batch_index = index;
        g_batch_total = total;

        SaveEntry entry;
        bool is_new;
        resolve_entry(name, kind, &entry, &is_new);
        snprintf(g_sync_name, sizeof(g_sync_name), "%s", entry.display_name);

        char ctx[40];
        snprintf(ctx, sizeof(ctx), "Reception %d/%d", index, total);
        ui_top(ctx, is_new ? "Absent ici, sera copie" : entry.display_name);

        SyncMeta meta;
        refresh_local_meta(&entry, &meta);

        SyncResult r = netsync_sync(false, &entry, &meta, index, total,
                                     on_sync_progress, msg, sizeof(msg));
        record_result(entry.display_name, r);

        if (r == SYNC_RESULT_NO_PEER || r == SYNC_RESULT_CANCELLED)
            break;
    }

    netsync_end();
    ui_top("Termine", NULL);
    show_summary();
}

// ---------------------------------------------------------------------------

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

    ui_top("Demarrage", NULL);

    if (!fatInitDefault())
    {
        consoleSelect(&bottomScreen);
        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_RED);
        printf("\n Erreur : carte SD illisible\n");
        goto halt;
    }

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY))
    {
        consoleSelect(&bottomScreen);
        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_RED);
        printf("\n Erreur : Wi-Fi indisponible\n");
        goto halt;
    }

    load_config();
    rescan();

    while (1)
    {
        char detail[40];
        snprintf(detail, sizeof(detail), "%d sauvegarde(s)", g_entry_count);
        ui_top(g_root, detail);

        consoleSelect(&bottomScreen);
        consoleClear();

        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_CYAN);
        printf("Que veux-tu faire ?\n");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
        ui_rule(&bottomScreen, CONSOLE_GRAY);
        printf("\n");

        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_GREEN);
        printf(" X ");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
        printf("HEBERGER\n");
        printf("   Je choisis les sauvegardes\n   et je les envoie.\n\n");

        consoleSetColor(&bottomScreen, CONSOLE_LIGHT_BLUE);
        printf(" Y ");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
        printf("REJOINDRE\n");
        printf("   Je suis l'autre console.\n\n");

        if (g_entry_count == 0)
        {
            consoleSetColor(&bottomScreen, CONSOLE_LIGHT_RED);
            printf(" Aucune sauvegarde trouvee\n");
            consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
            printf(" (tu peux quand meme REJOINDRE\n  pour en recevoir)\n");
        }

        consoleSetCursor(&bottomScreen, 0, 21);
        ui_rule(&bottomScreen, CONSOLE_GRAY);
        consoleSetColor(&bottomScreen, CONSOLE_GRAY);
        printf(" R: re-scanner  START: quitter");
        consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);

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
    consoleSetColor(&bottomScreen, CONSOLE_DEFAULT);
    consoleSetCursor(&bottomScreen, 0, 22);
    printf(" START: quitter");
    wait_key(KEY_START);

    return 0;
}
