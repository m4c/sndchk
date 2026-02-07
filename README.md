# sndchk.sh

Real-time audio diagnostics tool for FreeBSD that monitors audio buffer xruns, USB transfer errors, and interrupt rate spikes, especially for USB DAC devices and music interfaces, to help identify causes of audio glitches and dropouts.

The script is an addendum to the article: [FreeBSD audio diagnostics and optimisation](https://m4c.pl/blog/freebsd-audio-diagnostics-and-optimization/).

## Features

- **xruns monitoring** - tracks buffer underruns (playback) and overruns (recording)
- **USB error tracking** - monitors UE_ISOCHRONOUS_FAIL, UE_CONTROL_FAIL, UE_BULK_FAIL, UE_INTERRUPT_FAIL
- **IRQ spike detection** - detects interrupt rate anomalies on USB controller with configurable threshold
- **Automatic device correlation** - maps pcm devices to USB devices (ugen) and controllers (xhci/ehci)
- **Works with any audio device** - USB, PCI, or built-in (USB features auto-disabled for non-USB devices)

## Requirements

- FreeBSD 14.0 or later
- `sndctl(8)` utility
- Root privileges (for USB stats and IRQ monitoring)

## Installation

```sh
# Clone repository
git clone https://github.com/m4c/sndchk.git
cd sndchk

# Make executable
chmod +x sndchk.sh

# Optional: install system-wide
sudo cp sndchk.sh /usr/local/bin/sndchk
```

## Usage

```
usage: sndchk.sh [-d device] [-p] [-xruns] [-usb] [-w] [-i interval] [-t threshold]

Options:
  -d N      Monitor device pcmN (default: system default)
  -p        Show only playback channels
  -xruns    Show only xruns (no USB errors, no IRQ monitoring)
  -usb      Show only USB errors and IRQ monitoring (no xruns)
  -w        Watch mode - start monitoring
  -i SEC    Interval in seconds (default: 1)
  -t N      IRQ spike threshold multiplier (default: 1.5)
  -h        Show this help
```

Running without `-w` displays available audio devices and help:

```sh
./sndchk.sh
```

```
Available audio devices:

  pcm0: <ATI R6xx (HDMI)> (play)
  pcm1: <ATI R6xx (HDMI)> (play)
  pcm2: <ATI R6xx (HDMI)> (play)
  pcm3: <ATI R6xx (HDMI)> (play)
  pcm4: <Realtek ALC236 (Analog)> (play/rec)
  pcm5: <Realtek ALC236 (Front Analog Headphones)> (play)
  pcm6 (default) [usb:0.4]: <Focusrite Scarlett Solo 4th Gen> (play/rec) default

usage: sndchk.sh [-d device] [-p] [-xruns] [-usb] [-w] [-i interval] [-t threshold]
...
```

## Examples

```sh
# List available devices
sndchk.sh

# Monitor default device (xruns + USB + IRQ)
sndchk.sh -w

# Monitor specific device
sndchk.sh -d 6 -w

# Monitor only playback xruns
sndchk.sh -p -xruns -w

# Monitor only USB errors and IRQ spikes
sndchk.sh -usb -w

# Set IRQ spike threshold to 2x baseline
sndchk.sh -t 2.0 -w

# Check every 2 seconds with 1.8x threshold
sndchk.sh -i 2 -t 1.8 -w
```

## Output

```
Monitoring pcm6: <Focusrite Scarlett Solo 4th Gen> (play/rec) default
USB device: ugen0.4
USB controller: xhci0 (irq64)
----------------------------------------
[10:22:06] Initial xruns: pcm6.play.0=0 pcm6.record.0=0
[10:22:06] Initial USB: CTRL=0 ISO=8 BULK=0 INT=0
[10:22:06] Initial IRQ: calibrating...
[10:22:16] xhci0 baseline: 7592/s
[10:23:47] xhci0: 7592 -> 15840/s (2.0x)
[10:23:47] UE_ISOCHRONOUS_FAIL: 8 -> 10 (+2)
[10:23:48] pcm6.play.0 xruns: 0 -> 2 (+2)
```

Output appears only when changes or problems are detected. Silence means everything is OK.

## What the metrics mean

### xruns
Buffer underrun (playback) or overrun (recording). Non-zero or increasing value indicates the system can't keep up with audio data flow. Common causes: CPU load, insufficient buffer size, wrong latency settings.

### UE_ISOCHRONOUS_FAIL
USB isochronous transfer failures. Critical for audio:  isochronous transfers are used for real-time audio streaming. Increasing values indicate USB communication problems. Note: small increments (+2, +4) without audible artifacts may occur during track changes, stream start/stop, or sample rate switching - this is normal behavior, not a problem.

### UE_CONTROL_FAIL / UE_BULK_FAIL / UE_INTERRUPT_FAIL
Other USB transfer failures. Less common for audio but may indicate general USB issues.

### IRQ spikes
Sudden increase in interrupt rate on the USB controller. Often correlates with audio artifacts when other USB devices compete for bandwidth or CPU is busy handling interrupts.

## Troubleshooting audio issues

If you see problems, try:

1. **Increase buffer size**
   ```sh
   sysctl hw.snd.latency=7
   ```

2. **Increase USB audio buffer**
   ```sh
   sysctl hw.usb.uaudio.buffer_ms=4
   ```

3. **Disable USB power saving** for audio device
   ```sh
   usbconfig -d 0.4 power_on
   ```

4. **Disable CPU C-states**
   ```sh
   sysctl hw.acpi.cpu.cx_lowest=C1
   ```

5. **Run audio player with realtime priority**
   ```sh
   rtprio 0 musicpd
   ```

## Related tools

- `sndctl(8)` — sound device control
- `usbconfig(8)` — USB device configuration
- `vmstat -i` — interrupt statistics
- `/dev/sndstat` — kernel sound status

## Further reading

- [FreeBSD audio setup: bitperfect, equalizer, realtime](https://m4c.pl/blog/freebsd-audio-setup-bitperfect-equalizer-realtime/) — practical guide to configuring audio on FreeBSD
- [Vox FreeBSD: How Sound Works](https://freebsdfoundation.org/our-work/journal/browser-based-edition/freebsd-15-0/vox-freebsd-how-sound-works/) — in-depth article about FreeBSD sound(4) internals by Christos Margiolis (author of sndctl)

## License

BSD-2-Clause
