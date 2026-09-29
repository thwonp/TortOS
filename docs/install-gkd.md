# Installing on the GKD 350H Ultra

plorpOS runs on top of the GKD's own system, ROCKNIX, and changes nothing in
it. It needs two things: its files on the microSD card, and three small service
files on the GKD's internal storage that tell ROCKNIX to start plorpOS instead
of EmulationStation. Take the card out and the GKD starts EmulationStation as
before.

You need the GKD with its stock ROCKNIX, a microSD card, a computer on the same
Wi-Fi network, and a plorpOS build for the GKD. The build has two folders:

```
TortOS/     the launcher, emulator, cores and assets    -> the card
system.d/   the three service folders                   -> the GKD's storage
```

## 1. Connect the GKD to Wi-Fi

In EmulationStation, press START and open **Network Settings**. Turn Wi-Fi on,
choose your network and enter its password. Note the **IP address** it shows,
for example `192.168.0.55`.

ROCKNIX shares its storage on the network out of the box (Samba), with no
password. Leave the card out for now.

## 2. Put the services on the GKD

Do this before the card goes in. One of the three services stops ROCKNIX from
filling the card's `Roms/` with a hundred-odd empty system folders, and it can
only do that for a card it hasn't seen yet.

1. On the computer, connect to the GKD's **`config`** share:
   - Windows: type `\\192.168.0.55\config` in File Explorer's address bar.
   - Mac: Finder, *Go > Connect to Server*, `smb://192.168.0.55/config`,
     and connect as **Guest**.
   - Linux: `smb://192.168.0.55/config` in the file manager.
2. Open the **`system.d`** folder in that share. Create it if it isn't there.
3. Copy the three folders from the build's `system.d/` into it:

```
essway.service.d/plorpos.conf              starts plorpOS in place of EmulationStation
input.service.d/plorpos.conf               keeps ROCKNIX's hotkeys off while plorpOS runs
rocknix-automount.service.d/plorpos.conf   no empty system folders on the card
```

Copy the folders themselves, not just the `.conf` files in them: each file only
works inside its folder.

<details>
<summary>Over SSH instead</summary>

SSH is also on out of the box, as `root`. The password is in EmulationStation's
settings.

```sh
ssh root@192.168.0.55 mkdir -p /storage/.config/system.d
scp -r system.d/* root@192.168.0.55:/storage/.config/system.d/
```

</details>

## 3. Prepare the card

1. **Format it as exFAT.**
2. **Copy the build's `TortOS/` folder to the root of the card,** and create
   `Roms/`, `Bios/` and `Saves/` beside it.
3. **Add games** to `Roms/`, one folder per console, named as in
   [Supported systems](../README.md#supported-systems). BIOS files go loose in
   `Bios/`.

```
TortOS/    tortos.elf  diatom  launch.sh  systems.cfg  menu.ttf  cacert.pem
           cards/  cores/  res/
Roms/      NES/  SNES/  Game Boy/ ...
Bios/
Saves/
```

You can also fill the card in the GKD: put it in, and copy to the
**`games-external`** share the same way as in step 2.

## 4. Restart

Put the card in, and restart the GKD from EmulationStation (START, **Quit**,
**Restart System**). It comes up in plorpOS, and does from now on.

## If it starts EmulationStation instead

- **The card isn't in, or isn't readable.** ROCKNIX mounts it at
  `/storage/games-external`; check it's exFAT and that `TortOS/launch.sh` is on
  it.
- **The services aren't in place.** The `config` share should hold
  `system.d/essway.service.d/plorpos.conf` and the other two, each in its own
  folder.
- **plorpOS failed to start.** After five quick failures in a row it hands the
  GKD to EmulationStation until the next restart, so you're never left with a
  black screen. Its log is `.userdata/gkd/logs/tortos.log` on the card.

## What's on the GKD itself

Only the three `plorpos.conf` files in `/storage/.config/system.d/`. They
replace nothing: each adds to one of ROCKNIX's services. The first two do
nothing when plorpOS isn't on the card; the third keeps ROCKNIX from creating
empty system folders on any card. plorpOS's Wi-Fi, SSH and Samba switches change
ROCKNIX's own settings, the ones EmulationStation shows.

## Removing plorpOS

Take the card out: the GKD starts EmulationStation. To remove it for good,
delete the three folders (`essway.service.d`, `input.service.d`,
`rocknix-automount.service.d`) from the `config` share's `system.d`, and
restart.
