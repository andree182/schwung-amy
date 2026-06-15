# AMY for Schwung

Polyphonic synthesizer module based on the [AMY](https://github.com/shorepine/amy) synthesizer engine.

## Features

Main feature highlights:
* Lightweight and polyphonic
* Fast and low-latency fixed-point DSP engine
* DX7- and Juno- based presets, as well as a piano simulation
* Support for multiple synthesis algorithms:
  * Subtractive (Sine, Triangle, Saw, Pulse waveforms)
  * Frequency Modulation (FM synthesis)
  * Physical Modeling (Karplus-Strong string modeling)
  * PCM / Wave samples
  * Additive / Partials synthesis
* Integrates into Schwung UI - web UI allows for programming using javascript API

## Prerequisites

- [Schwung](https://github.com/charlesvestal/schwung) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

```bash
git clone --recursive https://github.com/andree182/schwung-amy
cd schwung-amy
./scripts/build.sh
./scripts/install.sh
```

## Controls

| Control | Function |
|---------|----------|
| Jog wheel | Browse presets / navigate menus |
| Knobs 1-8 | Adjust parameters |

## License

MIT - See [LICENSE](LICENSE)
