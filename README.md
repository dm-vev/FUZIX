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

## Iterating

I (@wez) am by no means an expert of FUZIX, but I've found the following things
useful when iterating on building stuff in here:

* You can use `picoctl flash` to reset into flash mode, which saves some hassle
  if you want to update the kernel image
* To clean and rebuild the kernel image, without cmake over-aggressively caching things, use:

```console
$ make -C Kernel/platform/platform-rpipico/ clean
$ make TARGET=rpipico SUBTARGET=pico2_w diskimage
```

## Customizing your filesystem image

* You probably want to `echo "stty erase '^?'" > .profile` to make
  delete/backspace work over the serial console
* If you add an additional partition to your SD card, you can enable swap on it; the maximum size that can be used is 2048kB; you can create a partition larger than that size, but you can only use 2048kB or 4096 blocks of it. The following command will use that size:

```console
$ swapon /dev/hda3 4096
```

You can put that command into `/etc/rc` using `levee` as an editor; it is
similar to `vi`.
