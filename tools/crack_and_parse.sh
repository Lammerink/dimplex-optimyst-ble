#!/bin/sh
# usage: ./crack_and_parse.sh "Dimplex v5.pcapng"
# Cracks the legacy-pairing passkey, decrypts, and prints the ATT command writes.
set -e
cd "$(dirname "$0")"
IN="$1"
OUT="${IN%.pcapng}_decrypted.pcap"
TSHARK="/Applications/Wireshark.app/Contents/MacOS/tshark"

echo "############ crackle (crack passkey + decrypt) ############"
./crackle/crackle -i "$IN" -o "$OUT"

echo "\n############ access address of the connection ############"
AA=$("$TSHARK" -r "$IN" -Y "btle.data_header" -T fields -e btle.access_address 2>/dev/null \
     | grep -v '^$' | sort | uniq -c | sort -rn | head -1 | awk '{print $2}' | sed 's/0x//')
echo "AA = 0x$AA"

echo "\n############ decrypted ATT trace ############"
python3 parse_att.py "$OUT" "$AA"
