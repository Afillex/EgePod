#!/system/bin/sh
# Configure the MT6359 ALSA mixer for headphone output.
# Requires tinymix (from tinyalsa) to be in PATH.
# Run once before starting egepod_audiod (or let alsa_out.c handle it).

set -e
CARD=0

mx() { tinymix -D $CARD "$1" "$2" && echo "  set '$1' = $2"; }

echo "[alsa-configure] Configuring MT6359 for headphone output…"

mx "Headphone Switch"     1
mx "Audio_Amp_R_Switch"   1
mx "Audio_Amp_L_Switch"   1
mx "HPL Mux"              "Audio Playback"
mx "HPR Mux"              "Audio Playback"
mx "DAC R2 Switch"        1
mx "DAC L2 Switch"        1
mx "HPOUT_L_SEL"          1
mx "HPOUT_R_SEL"          1
mx "Headphone Volume"     14
mx "Speaker Switch"       0
mx "Sidetone Switch"      0

echo "[alsa-configure] Done. Verify with: tinymix -D $CARD"
