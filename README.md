# FUZIX on PicoCalc

This repo is a fork of the upstream FUZIX project.
It maintains a small patchset that enables FUZIX
to boot on a PicoCalc device.

It currently builds and publishes images for
the pico2 and pico_w.

The patchset was originally taken from https://github.com/clockworkpi/PicoCalc

## Downloads

[Download](https://github.com/wez/FUZIX/releases/tag/clockworkpi-continuous)

## Installation

* Attach a USB cable to the pico
* Hold down the bootsel button and power cycle the pico
* Copy the fuzix.uf2 to RP mount point

To update the filesystem image on your SD card, you can use dd to put it on
the second partition:

```console
$ dd if=filesystem.img of=/dev/sdb2 oflag=direct bs=8192 status=progress
```

Make sure to change the `of` to match your device and ensure that you
are writing it to the correct device!  You will typically need root
privileges to perform that dd command.

See [the PicoCalc repo for more information](https://github.com/clockworkpi/PicoCalc/tree/master/Bin/PicoCalc%20SD#flashing-the-fuzix-32mb-image)

