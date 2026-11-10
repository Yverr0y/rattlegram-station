# Rattlegram Station

Text-over-sound on the desktop and in the browser, wire-compatible with the
[aicodix Rattlegram](https://github.com/aicodix/rattlegram) phone app. Encode a
message to ~1 s of audio, decode it back from a mic or a recording, and
experiment with making the signal **coexist with music**.

Built on the 0BSD aicodix OFDM modem (vendored in `src/aicodix/`): 160 ms OFDM
symbols, 6.25 Hz subcarriers, differential QPSK, CRC32C-aided polar SCL FEC.

## What's here

| Path | What |
|---|---|
| `cli/rattlegram-cli.cpp` | native CLI: `encode` / `decode` / `sweep` |
| `src/wasm-glue.cpp` | C ABI over the modem for WebAssembly |
| `web/` | browser app (receive / send / hide-in-audio / encrypt) + prebuilt `rattlegram.wasm` |
| `src/aicodix/` | vendored modem headers (0BSD) |

## Run the web app (no toolchain needed — WASM is prebuilt)

```bash
./serve.sh            # http://localhost:8787
```
Open it in a browser. `localhost` is a secure context, so the mic works there.
- **Receive** — listen on the mic, decode live.
- **Send** — compose, transmit from the speaker, or download a `.wav`.
- **Hide in audio** — drop a message into the quietest band of a music track
  (valley-placement or notch), then self-decode to verify it survived.
- **Encrypt** — AES-GCM from a shared passphrase (base64'd, since the modem
  truncates payloads at the first NUL byte).

## Build the CLI

```bash
g++ -std=c++17 -O2 -march=native -I src/aicodix cli/rattlegram-cli.cpp -o rattlegram-cli
./rattlegram-cli encode -c JEFF -F out.wav "hello over the air"
./rattlegram-cli decode out.wav
./rattlegram-cli sweep            # AWGN decode-rate vs SNR, per mode
```

## Rebuild the WASM (needs emscripten)

```bash
./build.sh            # writes web/rattlegram.{js,wasm}
```

## Honest limits

- **Capacity:** ≤170 bytes/burst (emoji = 4 B each).
- **Band:** 1600 Hz wide, centred on the carrier (default 1500 → 700–2300 Hz);
  tunable 1000–3000 Hz, or up to ~20 kHz near-ultrasonic at 48 kHz.
- **Sensitivity (measured, AWGN, 2500 Hz ref):** ≥90% decode at ~+3 dB SNR for
  ≤85 B, ~+5 dB for ≤128 B, ~+7 dB for ≤170 B. It needs a *positive* SNR — you
  cannot bury it under louder audio and still decode it, hence coexistence
  (valley/notch) rather than concealment.
- **Privacy:** the payload scrambler uses a fixed public seed. A plain
  rattlegram is readable by anyone with the app — hiding in music is obscurity,
  not secrecy. Use Encrypt for confidentiality.
- **Law:** on amateur bands, encrypting to obscure meaning is prohibited in many
  countries (US FCC Part 97 §97.113). Keep encryption to local / line-level /
  unlicensed audio. Many licence-free services (PMR446/FRS/CB) restrict data —
  check local rules.

## Credits

Modem and app wire format: aicodix GmbH (Ahmet Inan), 0BSD. This repo adds the
CLI, the WASM ABI, and the browser app.
