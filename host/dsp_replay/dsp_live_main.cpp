/* PC-Fassung des Live-Pfads (V5.2): dsp_live <stream.log> → die Handy-Zeilen auf stdout, Rechenzeit auf stderr.
 * Liest die Datei Zeile für Zeile wie die App den Empfang (host/tests/test_dsp_live.py). Übersetzen:
 *   g++ -O2 -std=c++17 -ffp-contract=off [-DA121_DSP_HEAVY] -I host/dsp_replay -I firmware/esp32s3/src \
 *       host/dsp_replay/dsp_live_main.cpp
 */
#include "dsp_live.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Aufruf: %s <stream.log>\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) {
        perror(argv[1]);
        return 2;
    }
    static a121_dsp::UltimateEspDsp dsp;
    dsp_live::LiveDsp live(dsp);
    char *buf = nullptr;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&buf, &cap, in)) >= 0) {
        const std::string &out = live.line(buf, (size_t)n);
        if (!out.empty()) printf("%s\n", out.c_str());
    }
    free(buf);
    fclose(in);
    const double ms = live.cpuNs() / 1e6, s = live.frames() / (double)a121_dsp::kFsHz;
    fprintf(stderr, "Rechenzeit %.1f ms für %.1f s Daten (%.2f %% eines Kerns)\n", ms, s,
            s > 0 ? ms / (10.0 * s) : 0.0);
    return 0;
}
