/*
WASM/C ABI around rattlegram's encoder.hh / decoder.hh (0BSD, github.com/aicodix/rattlegram).
Same wire format as the Android/iOS app. Built for the browser (rspace-online) and the PC viewer.
*/
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>
#define assert(expr) do {} while (0)
#include "encoder.hh"
#include "decoder.hh"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define API extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define API extern "C"
#endif

static EncoderInterface *mk_enc(int r){switch(r){case 8000:return new Encoder<8000>();case 16000:return new Encoder<16000>();case 32000:return new Encoder<32000>();case 44100:return new Encoder<44100>();case 48000:return new Encoder<48000>();}return nullptr;}
static DecoderInterface *mk_dec(int r){switch(r){case 8000:return new Decoder<8000>();case 16000:return new Decoder<16000>();case 32000:return new Decoder<32000>();case 44100:return new Decoder<44100>();case 48000:return new Decoder<48000>();}return nullptr;}
static int ext_len(int r){int s=(1280*r)/8000;return s+s/8;}

// ---- encode: write int16 PCM into `out` (cap samples). returns sample count, or -1. ----
API int rg_encode(const char *msg, int msg_len, const char *call,
                  int carrier, int noise, int fancy, int rate,
                  int16_t *out, int out_cap) {
    EncoderInterface *e = mk_enc(rate);
    if (!e) return -1;
    uint8_t payload[170] = {0};
    int n = std::min(msg_len, 170);
    memcpy(payload, msg, n);
    int8_t cs[10] = {0};
    for (int i = 0; i < 9 && call && call[i]; ++i) cs[i] = call[i];
    e->configure(payload, cs, carrier, noise, fancy != 0);
    int el = ext_len(rate), total = 0;
    int16_t *buf = (int16_t*)malloc(el * sizeof(int16_t));
    while (buf && e->produce(buf, 0)) {
        if (total + el > out_cap) break;
        memcpy(out + total, buf, el * sizeof(int16_t));
        total += el;
    }
    free(buf);
    delete e;
    return total;
}

// ---- streaming decoder ----
struct Dec {
    DecoderInterface *d;
    int rate, chunk;
    uint8_t payload[170];
    char call[10];
    int mode, flips;
    float cfo;
    int ready;   // 0 none, 1 message, 2 ping
};

API void *rg_dec_create(int rate) {
    DecoderInterface *d = mk_dec(rate);
    if (!d) return nullptr;
    Dec *s = new Dec{};
    s->d = d; s->rate = rate; s->chunk = rate / 50;
    return s;
}
API void rg_dec_destroy(void *h) { if (!h) return; Dec *s=(Dec*)h; delete s->d; delete s; }

// push PCM int16 samples; drives the state machine in rate/50 chunks.
// returns: 0 nothing, 1 message decoded, 2 ping, -1 preamble/CRC fail seen,
//          3 preamble synced (a burst is arriving; its carrier is rg_dec_fetch's cfo).
//          1, 2 and -1 outrank 3 within one push.
API int rg_dec_push(void *h, const int16_t *s, int count) {
    if (!h) return 0;
    Dec *d = (Dec*)h;
    int outcome = 0;
    float cfo; int32_t mode; uint8_t call[10] = {0};
    for (int off = 0; off + d->chunk <= count; off += d->chunk) {
        if (!d->d->feed(s + off, d->chunk, 0)) continue;
        int st = d->d->process();
        if (st == STATUS_SYNC || st == STATUS_PING || st == STATUS_NOPE)
            d->d->staged(&cfo, &mode, call);
        if (st == STATUS_SYNC) { d->cfo = cfo; if (outcome == 0) outcome = 3; }
        if (st == STATUS_FAIL) outcome = -1;
        else if (st == STATUS_PING) {
            d->d->staged(&cfo, &mode, call);
            memcpy(d->call, call, 10); d->call[9]=0; d->mode=mode; d->cfo=cfo;
            d->ready = 2; outcome = 2;
        } else if (st == STATUS_DONE) {
            d->d->staged(&cfo, &mode, call);
            int f = d->d->fetch(d->payload);
            memcpy(d->call, call, 10); d->call[9]=0; d->mode=mode; d->cfo=cfo; d->flips=f;
            if (f < 0) { outcome = -1; }
            else { d->ready = 1; outcome = 1; }
        }
    }
    return outcome;
}

// read the last decoded result. copies up to 170 payload bytes into `pay`,
// callsign (trimmed) into `call9`. returns payload length, sets *mode,*flips,*cfo.
API int rg_dec_fetch(void *h, uint8_t *pay, char *call9, int *mode, int *flips, float *cfo) {
    if (!h) return -1;
    Dec *d = (Dec*)h;
    int len = 0; while (len < 170 && d->payload[len]) ++len;
    memcpy(pay, d->payload, 170);
    // trim leading/trailing spaces from base37 callsign
    int a = 0; while (d->call[a] == ' ') ++a;
    int b = 9; while (b > a && (d->call[b-1] == ' ' || d->call[b-1] == 0)) --b;
    int j = 0; for (int i = a; i < b; ++i) call9[j++] = d->call[i]; call9[j] = 0;
    if (mode) *mode = d->mode;
    if (flips) *flips = d->flips;
    if (cfo) *cfo = d->cfo;
    return len;
}

// expose the occupied band for a given carrier (bandwidth is fixed 1600 Hz)
API int rg_bandwidth() { return 1600; }
