#!/bin/bash
# Affiche en clair un fichier .syncmeta (40 octets, binaire).
#
#   bash tools/readsyncmeta.sh "D:/roms/nds/saves/Mon Jeu.syncmeta"
#
# Structure (SyncMeta, source/savescan.h), 10 mots de 32 bits little-endian :
#   magic | console_id | version | crc32[3] | len[3] | present[3]

set -e

f="$1"
if [ -z "$f" ]; then
    echo "usage: bash $0 <fichier.syncmeta>" >&2
    exit 1
fi
if [ ! -f "$f" ]; then
    echo "introuvable : $f" >&2
    exit 1
fi

# -v pour ne pas replier les lignes identiques, sinon des champs disparaissent.
# L'affectation de tableau decoupe sur tous les blancs, y compris les sauts de
# ligne : od produit plusieurs lignes de 4 mots.
w=($(od -A n -t x4 -v "$f"))

if [ "${#w[@]}" -lt 10 ]; then
    echo "fichier trop court (${#w[@]} mots, 10 attendus) : pas un .syncmeta valide" >&2
    exit 1
fi

hex2dec() { printf '%d' "0x$1"; }

magic="${w[0]}"
if [ "$magic" != "53535931" ]; then
    echo "magic inattendu ($magic au lieu de 53535931) : pas un .syncmeta valide" >&2
    exit 1
fi

present=$(hex2dec "${w[9]}")
p_sav=$(( present & 0xFF ))
p_pub=$(( (present >> 8) & 0xFF ))
p_prv=$(( (present >> 16) & 0xFF ))

oui_non() { [ "$1" -ne 0 ] && echo "oui" || echo "non"; }

echo "Fichier    : $f"
echo "Magic      : SSY1 (ok)"
echo "ID console : 0x${w[1]}"
echo "Version    : $(hex2dec "${w[2]}")"
echo
printf '%-8s %-6s %-12s %s\n' "Sous-fic" "Prsnt" "CRC32" "Taille"
printf '%-8s %-6s %-12s %s\n' ".sav" "$(oui_non $p_sav)" "0x${w[3]}" "$(hex2dec "${w[6]}") octets"
printf '%-8s %-6s %-12s %s\n' ".pub" "$(oui_non $p_pub)" "0x${w[4]}" "$(hex2dec "${w[7]}") octets"
printf '%-8s %-6s %-12s %s\n' ".prv" "$(oui_non $p_prv)" "0x${w[5]}" "$(hex2dec "${w[8]}") octets"
echo
echo "Astuce : si l'ID console ci-dessus differe du contenu de"
echo "         /_nds/savesync/console_id.dat de la meme carte, c'est que"
echo "         cette sauvegarde a ete RECUE de l'autre console."
