/* PC-Fassung der DSP-Nachrechnung: dsp_replay <stream.log> [neu.log]  → Bericht auf stdout, Exit 0 = bitgleich.
 * Übersetzen (macht host/a121_esp_ultimate.py --mode replay-cpp):
 *   g++ -O2 -std=c++17 -ffp-contract=off -I host/dsp_replay -I firmware/esp32s3/src host/dsp_replay/dsp_replay_main.cpp
 */
#include "dsp_replay.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Aufruf: %s <stream.log> [neu.log]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) {
        perror(argv[1]);
        return 2;
    }
    FILE *out = argc > 2 ? fopen(argv[2], "w") : nullptr;
    static a121_dsp::UltimateEspDsp dsp;
    dsp_replay::Replayer r(dsp, out, stdout);
    r.runFile(in);
    fclose(in);
    const bool same = r.finish();
    if (out) fclose(out);
    return same ? 0 : 1;
}
