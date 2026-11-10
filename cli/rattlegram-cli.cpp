/*
Desktop CLI around rattlegram's C++ core (encoder.hh / decoder.hh from
github.com/aicodix/rattlegram, 0BSD). Same wire format as the Android/iOS app.

  rattlegram-cli encode [-r RATE] [-c CALL] [-f HZ] [-n NOISE] [-F] OUT.wav "message"
  rattlegram-cli decode IN.wav
  rattlegram-cli sweep  [-r RATE] [-t TRIALS]   # AWGN decode-rate vs SNR, per mode
*/

// jni.h pulls these in on Android; the headers rely on it
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <new>
#include <cstring>
#define assert(expr) do {} while (0)
#include "encoder.hh"
#include "decoder.hh"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

static EncoderInterface *make_encoder(int rate) {
	switch (rate) {
		case 8000: return new Encoder<8000>();
		case 16000: return new Encoder<16000>();
		case 32000: return new Encoder<32000>();
		case 44100: return new Encoder<44100>();
		case 48000: return new Encoder<48000>();
	}
	return nullptr;
}

static DecoderInterface *make_decoder(int rate) {
	switch (rate) {
		case 8000: return new Decoder<8000>();
		case 16000: return new Decoder<16000>();
		case 32000: return new Decoder<32000>();
		case 44100: return new Decoder<44100>();
		case 48000: return new Decoder<48000>();
	}
	return nullptr;
}

static int extended_length(int rate) {
	int sym = (1280 * rate) / 8000;
	return sym + sym / 8;
}

static std::vector<int16_t> encode(int rate, const std::string &msg, const std::string &call,
	int freq, int noise, bool fancy) {
	EncoderInterface *enc = make_encoder(rate);
	uint8_t payload[170] = {0};
	memcpy(payload, msg.data(), std::min<size_t>(msg.size(), 170));
	int8_t cs[10] = {0};
	memcpy(cs, call.data(), std::min<size_t>(call.size(), 9));
	enc->configure(payload, cs, freq, noise, fancy);
	std::vector<int16_t> out, buf(extended_length(rate));
	while (enc->produce(buf.data(), 0))
		out.insert(out.end(), buf.begin(), buf.end());
	delete enc;
	return out;
}

struct Result {
	int status = -1;   // -1 nothing, 0 ok, 1 crc fail, 2 ping, 3 nope
	int mode = 0, flips = 0;
	float cfo = 0;
	std::string call, text;
};

static std::vector<Result> decode(int rate, const std::vector<int16_t> &audio, bool verbose) {
	DecoderInterface *dec = make_decoder(rate);
	std::vector<Result> results;
	int chunk = rate / 50;
	std::vector<int16_t> pad(audio);
	pad.insert(pad.end(), 3 * rate, 0);  // flush
	float cfo; int32_t mode; uint8_t call[10] = {0}, payload[170];
	for (size_t pos = 0; pos + chunk <= pad.size(); pos += chunk) {
		if (!dec->feed(pad.data() + pos, chunk, 0))
			continue;
		int st = dec->process();
		Result r;
		switch (st) {
			case STATUS_SYNC:
				dec->staged(&cfo, &mode, call);
				if (verbose)
					fprintf(stderr, "sync: call=%.9s mode=%d cfo=%.2f Hz\n", call, mode, cfo);
				break;
			case STATUS_FAIL:
				if (verbose) fprintf(stderr, "preamble fail\n");
				r.status = 4; results.push_back(r);
				break;
			case STATUS_PING:
			case STATUS_NOPE:
				dec->staged(&cfo, &mode, call);
				r.status = st == STATUS_PING ? 2 : 3;
				r.mode = mode; r.cfo = cfo; r.call = (char *)call;
				results.push_back(r);
				break;
			case STATUS_DONE: {
				int flips = dec->fetch(payload);
				r.mode = mode; r.cfo = cfo; r.call = (char *)call;
				if (flips < 0) {
					r.status = 1;
				} else {
					r.status = 0; r.flips = flips;
					r.text.assign((char *)payload, strnlen((char *)payload, 170));
				}
				results.push_back(r);
				break;
			}
		}
	}
	delete dec;
	for (auto &r : results)
		{ r.call.erase(0, r.call.find_first_not_of(' ')); while (!r.call.empty() && r.call.back() == ' ') r.call.pop_back(); }
	return results;
}

static bool write_wav(const char *path, int rate, const std::vector<int16_t> &s) {
	FILE *f = strcmp(path, "-") ? fopen(path, "wb") : stdout;
	if (!f) return false;
	uint32_t data = s.size() * 2, riff = 36 + data, fmt = 16, br = rate * 2;
	uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
	uint32_t r = rate;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&r, 4, 1, f);
	fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(s.data(), 2, s.size(), f);
	if (f != stdout) fclose(f);
	return true;
}

// Minimal PCM16 WAV reader; stereo is downmixed.
static bool read_wav(const char *path, int &rate, std::vector<int16_t> &out) {
	FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
	if (!f) return false;
	char id[4]; uint32_t sz; uint16_t fmt_tag = 0, ch = 0, bits = 0; uint32_t r = 0;
	if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4)) return false;
	fread(&sz, 4, 1, f);
	if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4)) return false;
	while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
		if (!memcmp(id, "fmt ", 4)) {
			std::vector<uint8_t> b(sz);
			fread(b.data(), 1, sz, f);
			memcpy(&fmt_tag, &b[0], 2); memcpy(&ch, &b[2], 2);
			memcpy(&r, &b[4], 4); memcpy(&bits, &b[14], 2);
		} else if (!memcmp(id, "data", 4)) {
			if (fmt_tag != 1 || bits != 16 || !ch) {
				fprintf(stderr, "need PCM 16-bit WAV (got fmt=%d bits=%d ch=%d)\n", fmt_tag, bits, ch);
				return false;
			}
			std::vector<int16_t> raw;
			int16_t buf[4096];
			size_t n;
			while ((n = fread(buf, 2, 4096, f)) > 0)
				raw.insert(raw.end(), buf, buf + n);
			for (size_t i = 0; i + ch <= raw.size(); i += ch) {
				int acc = 0;
				for (int c = 0; c < ch; ++c) acc += raw[i + c];
				out.push_back(acc / ch);
			}
			rate = r;
			return true;
		} else {
			fseek(f, sz + (sz & 1), SEEK_CUR);
		}
	}
	return false;
}

static const char *mode_payload(int mode) {
	return mode == 16 ? "85B" : mode == 15 ? "128B" : mode == 14 ? "170B" : "?";
}

// SNR is quoted in a 2500 Hz reference bandwidth, the convention FT8/VARA/etc use,
// so the threshold compares directly with other HF digital modes.
static void sweep(int rate, int trials) {
	std::mt19937 rng(12345);
	std::normal_distribution<float> gauss(0, 1);
	std::uniform_int_distribution<int> lead(0, rate);
	std::uniform_int_distribution<int> byte('a', 'z');
	const int lens[] = {85, 128, 170};
	printf("AWGN sweep, %d trials/point, rate %d Hz, SNR in 2500 Hz ref BW\n", trials, rate);
	printf("%8s", "SNR dB");
	for (int L : lens) printf("  mode%-2d(%4dB)", L == 85 ? 16 : L == 128 ? 15 : 14, L);
	printf("\n");
	for (int snr = atoi(getenv("LO") ? getenv("LO") : "-14"); snr <= atoi(getenv("HI") ? getenv("HI") : "4"); ++snr) {
		printf("%8d", snr);
		for (int L : lens) {
			int ok = 0, pf = 0, crcf = 0, none = 0;
			for (int t = 0; t < trials; ++t) {
				std::string msg(L, 'x');
				for (auto &c : msg) c = byte(rng);
				std::vector<int16_t> sig = encode(rate, msg, "N0CALL", 1500, 0, false);
				double p = 0; size_t active = 0;
				for (int16_t s : sig) if (s) { p += double(s) * s; ++active; }
				p /= std::max<size_t>(active, 1);
				// noise power in 2500 Hz = sigma^2 * 2500 / (rate/2)
				double sigma = std::sqrt(p / std::pow(10.0, snr / 10.0) * (rate / 2.0) / 2500.0);
				// scale down so signal+noise rarely clips
				double scale = 8000.0 / std::sqrt(p + sigma * sigma);
				std::vector<int16_t> rx(lead(rng) + sig.size() + rate, 0);
				size_t off = rx.size() - sig.size() - rate;
				for (size_t i = 0; i < rx.size(); ++i) {
					double s = (i >= off && i - off < sig.size()) ? sig[i - off] : 0;
					double v = scale * (s + sigma * gauss(rng));
					rx[i] = std::clamp(v, -32768.0, 32767.0);
				}
				auto res = decode(rate, rx, false);
				bool got = false, sawpf = false, sawcrc = false;
				for (auto &r : res) {
					if (r.status == 0 && r.text == msg) got = true;
					if (r.status == 4) sawpf = true;
					if (r.status == 1) sawcrc = true;
				}
				if (got) ++ok; else if (sawcrc) ++crcf; else if (sawpf) ++pf; else ++none;
			}
			if (getenv("DIAG")) printf("  ok%3d crc%3d pre%3d nosync%3d", ok, crcf, pf, none);
			else printf("  %12d%%", 100 * ok / trials);
		}
		printf("\n");
		fflush(stdout);
	}
}

static void usage() {
	fprintf(stderr,
		"usage:\n"
		"  rattlegram-cli encode [-r RATE] [-c CALL] [-f HZ] [-n NOISE] [-F] OUT.wav \"message\"\n"
		"  rattlegram-cli decode IN.wav|-\n"
		"  rattlegram-cli sweep [-r RATE] [-t TRIALS]\n"
		"  RATE 8000|16000|32000|44100|48000 (48000)  CALL <=9 [A-Z0-9]  HZ carrier (1500)\n"
		"  NOISE leading-noise symbols for VOX 0|1|3|6 (0)  -F fancy header (callsign in waterfall)\n");
}

int main(int argc, char **argv) {
	if (argc < 2) { usage(); return 2; }
	std::string cmd = argv[1];
	optind = 2;
	int rate = 48000, freq = 1500, noise = 0, trials = 20, opt;
	bool fancy = false;
	std::string call = "N0CALL";
	while ((opt = getopt(argc, argv, "r:c:f:n:t:F")) != -1) {
		switch (opt) {
			case 'r': rate = atoi(optarg); break;
			case 'c': call = optarg; break;
			case 'f': freq = atoi(optarg); break;
			case 'n': noise = atoi(optarg); break;
			case 't': trials = atoi(optarg); break;
			case 'F': fancy = true; break;
			default: usage(); return 2;
		}
	}
	if (cmd == "encode") {
		if (argc - optind != 2) { usage(); return 2; }
		std::string msg = argv[optind + 1];
		if (msg.size() > 170) { fprintf(stderr, "message is %zu bytes, max 170\n", msg.size()); return 2; }
		if (!make_encoder(rate)) { fprintf(stderr, "bad rate\n"); return 2; }
		auto s = encode(rate, msg, call, freq, noise, fancy);
		int mode = msg.empty() ? 0 : msg.size() <= 85 ? 16 : msg.size() <= 128 ? 15 : 14;
		fprintf(stderr, "mode %d (%s), %.2f s audio\n", mode, mode_payload(mode), s.size() / double(rate));
		return write_wav(argv[optind], rate, s) ? 0 : 1;
	}
	if (cmd == "decode") {
		if (argc - optind != 1) { usage(); return 2; }
		std::vector<int16_t> audio;
		if (!read_wav(argv[optind], rate, audio)) { fprintf(stderr, "cannot read wav\n"); return 1; }
		if (!make_decoder(rate)) { fprintf(stderr, "unsupported rate %d\n", rate); return 1; }
		int got = 0;
		for (auto &r : decode(rate, audio, true)) {
			switch (r.status) {
				case 0: printf("[%s] mode %d, %d bits flipped, cfo %.1f Hz: %s\n",
					r.call.c_str(), r.mode, r.flips, r.cfo, r.text.c_str()); ++got; break;
				case 1: printf("[%s] mode %d: decoding failed (CRC)\n", r.call.c_str(), r.mode); break;
				case 2: printf("[%s] ping\n", r.call.c_str()); ++got; break;
				case 3: printf("[%s] unsupported mode %d\n", r.call.c_str(), r.mode); break;
			}
		}
		return got ? 0 : 1;
	}
	if (cmd == "sweep") { sweep(rate, trials); return 0; }
	usage();
	return 2;
}
