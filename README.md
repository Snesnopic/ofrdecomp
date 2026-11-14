# ofrdecomp

A C++ reimplementation of the [OptimFROG](http://losslessaudio.org/) audio codecs, reverse-engineered
from the closed-source v5.100 binaries.

The goal is a decoder that matches the official one bit for bit and an encoder whose files the
official decoder reads back losslessly, on any platform and from a library. Both are checked against
the official binaries rather than assumed from a format description.

## Status

| Format | Decoder | Encoder |
|---|---|---|
| Lossless (`.ofr`), mono and stereo, 8/16/24/32-bit | yes | yes |
| IEEE Float (`.off`) | yes | yes |
| DualStream (`.ofs`, lossy) | yes | no |

[doc/status.md](doc/status.md) lists what each part was verified against, including what still
waits for a run against the official binaries.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

Floating point has to stay exact for the output to match, so the build sets `-fno-fast-math
-ffp-contract=off`, or `/fp:precise` on MSVC. 32-bit x86 also needs SSE2 math, which the build
turns on.

This produces:

- `ofr`, a command line tool modelled on the original:
  ```sh
  ofr --encode [options] src [--output dst] ...
  ofr --decode [options] src [--output dst] ...
  ofr --info src ...
  ofr --verify src ...
  ofr --check src ...
  ```
  Options include `--preset {0-10,max}`, `--threads n`, `--raw` with
  `--channelconfig`/`--sampletype`/`--rate`, `--output`, `--overwrite`, `--silent` and `--help`.
  WAV files are restored byte for byte, including chunks around the audio.
- `ofrdecomp` and `ofrenc`, minimal tools for scripts.
- `ofrdecomp_lib`, the library behind them.

## Using the library

`include/ofrdecomp.hpp` is the C++ interface:

```cpp
#include "ofrdecomp.hpp"

ofrdecomp::Decoder decoder("track.ofr");            // also from memory, or a custom Stream
std::vector<int32_t> samples = decoder.readAll();

ofrdecomp::EncodeOptions options;
options.threads = 4;
std::vector<uint8_t> file = ofrdecomp::Encoder::encode(samples, 2, 44100, 16, options);
```

`include/OptimFROG.h` offers the C interface of the official library, for programs written against
it.

## Testing

```sh
ctest --test-dir build                              # no external files needed
bash tests/conformance/run.sh                       # lossless, against the official binaries
bash tests/conformance/run_float.sh                 # IEEE Float, against the official binaries
```

The conformance scripts check both directions: files from the official encoder decode identically
here, and files from this encoder decode losslessly on the official decoder. They need the official
tools, from the [OptimFROG site](http://losslessaudio.org/); see the top of each script for the
paths and options.

To fuzz the decoder, with a clang that ships libFuzzer:

```sh
cmake -B build-fuzz -DOFRDECOMP_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-fuzz
mkdir corpus && build-fuzz/make_corpus corpus
build-fuzz/fuzz_decoder corpus -max_len=32768
```

## Documentation

[doc/README.md](doc/README.md) indexes the format description and the notes on each algorithm.

## License

MIT, see [LICENSE](LICENSE). This covers the code and text in this repository. It grants nothing
over the OptimFROG name or the official software, which stays under its own terms. The official
binaries are not part of this repository: CI downloads them at run time, only to compare against.

This project was built by analysing the official binaries, statically and at run time, for
interoperability. It is not a clean-room implementation, and it is not affiliated with or endorsed
by the author of OptimFROG.
