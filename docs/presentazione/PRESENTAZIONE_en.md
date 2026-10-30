# ESP32-S3-CAM_PDX DATV Beacon

**A complete DVB-S2 transmitter with camera, on a board that costs a few tens of euros**

*Luigi D'Arcangelo, IZ7PDX — open-source project, GPL-3.0 licence*

![Beacon received with Portsdown 4 and Minitiouner: MER 26 dB](../img/test_mer_20261030/05_m9.6dB_MER26.0.jpg)

A single **ESP32-S3 camera board** becomes a digital amateur television beacon. The chip's Wi-Fi transmitter is used as
an **I/Q modulator**; everything else — video, MPEG-TS multiplexer, **DVB-S2** coding to ETSI EN 302 307-1 and QPSK
modulation — is computed by the board itself. No SDR, PC or Raspberry Pi: just power, an antenna and a filter.

## What it does

- **DVB-S2 QPSK** at 333 kS/s, FEC 1/2 to 9/10: received with **MER 23–27 dB** on Portsdown 4 and Minitiouner.
- **2300–2450 MHz** directly; any other band with an **external mixer**: the final frequency is shown in the picture and
  in the service name.
- **Live camera** (H.264), camera **picture-in-picture** on the test card, standard H.265 test card and **custom test
  cards**: a photo from your phone or a video prepared on the PC (there is a Halloween one too!).
- **Stand-alone beacon**: test card / camera / pause cycle, automatic start at power-on, a **status LED** that "warms up"
  with the output level.
- **Set-up from your phone** in 11 languages, while not transmitting.
- Firmware, tools, documentation and test reports are **free** (GPL-3.0).

## What you need

- An ESP32-S3 N16R8 board with camera (tested: diymore ESP32-S3-CAM, Freenove layout).
- A 13 cm antenna and a **band-pass filter**; a linear amplifier for longer paths.
- A DVB-S2 receiver (Minitiouner, Portsdown, compatible satellite receivers).

## Let's work together!

The project has just been born and will grow with the amateur radio community. Every contribution is welcome:

- **on-air tests**: distance, MER, receiver and antennas used, photos of the reception;
- **bug reports** and **requests for improvements**;
- ideas and builds: amplifiers and filters, transverters for other bands, other ESP32-S3 boards, higher symbol rates;
- **review of the translations** of the web page and documents.

Repository, issues and discussions: **{{REPO}}** — contact: **iz7pdx@gmail.com**

*Warning: for licensed radio amateurs only, on the frequencies and at the power levels permitted by your licence, always
with a suitable filter on the output.*
