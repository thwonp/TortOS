# Installing on an Anbernic H700 (RG SP)

plorpOS runs on top of **BaseOS**, a small system for Anbernic's H700
handhelds by Prashant Vaibhav (<https://github.com/pvaibhav/BaseOS>). BaseOS
goes on one microSD card and starts whatever frontend it finds on the other;
plorpOS is that frontend. plorpOS does not include or change BaseOS.

Tested with **BaseOS 1.3.0** on the **RG SP** (RG34XX SP). Other H700 models
come later.

You need the RG SP, two microSD cards and a computer.

## 1. BaseOS on the first card (TF1)

From [BaseOS's releases](https://github.com/pvaibhav/BaseOS/releases), download
the image for your model - for the RG SP, `baseos-rg34xxsp-<version>.img.zip` -
and write it to a card as BaseOS's own instructions say. Put it in the slot
marked **TF1** and turn the device on once: it grows its storage, then says
there is no frontend on the card. Turn it off.

## 2. plorpOS on the second card (TF2)

Download **`plorpOS-h700-v<version>.zip`** from
[Releases](https://github.com/thwonp/TortOS/releases/latest) and unzip it. Copy
everything in it to the root of a FAT32 or exFAT card:

```
System/       the frontend hook BaseOS looks for (launch_frontend.sh)
TortOS/       plorpOS itself
Roms/ Bios/ Saves/ Music/ Audiobooks/   empty, for your games and files
```

Put your games in `Roms/` (one folder per system, as `TortOS/systems.cfg`
names them) and BIOS files in `Bios/`.

## 3. Turn it on

Put the card in the slot marked **TF2** and turn the device on. BaseOS starts
plorpOS in a few seconds. Join Wi-Fi from **Menu > Wi-Fi Services**.

## Notes

- **Saved Wi-Fi networks** live on the plorpOS card, in
  `.userdata/rgsp/wpa_supplicant.conf`, with their passwords.
- **Brightness:** hold Menu and press Volume +/-.
- **BaseOS updates** (`.bosupd` files) go on the root of the plorpOS card;
  BaseOS installs them on the next start. plorpOS leaves unknown files there
  alone. A BaseOS version other than the tested one may work, but is untested.
- **Over USB** (for developers): BaseOS offers `adb`, but only with a
  USB-A to USB-C cable attached before the device is turned on. A USB-C to
  USB-C cable from a USB-C-only computer powers nothing and connects nothing.

## Removing plorpOS

Delete `System/` and `TortOS/` from the TF2 card. BaseOS then says there is no
frontend, as it did before.
