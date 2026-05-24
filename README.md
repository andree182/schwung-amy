# AMY for Schwung

Polyphonic synthesizer module based on the [AMY](https://github.com/shorepine/amy) synthesizer engine by Brian Whitman.

## Features

Main feature highlights:
* 16-voice polyphony
* Fast and low-latency fixed-point DSP engine
* Support for multiple synthesis algorithms:
  * Subtractive (Sine, Triangle, Saw, Pulse waveforms)
  * Frequency Modulation (FM synthesis)
  * Physical Modeling (Karplus-Strong string modeling)
  * PCM / Wave samples
  * Additive / Partials synthesis
* Integrates into Schwung UI

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
