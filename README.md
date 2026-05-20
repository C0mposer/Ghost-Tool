# Donwload Ghosts

To find ghosts made by top level players in the Spyro community: [See Here](https://docs.google.com/spreadsheets/d/1FRsIFruvudBQzKBPcCEm27SErnol9FkRNUDLPE_SKMI/edit?gid=0#gid=0)

# Ghost Tool
This tool allows you to to save & load ghosts from the Spyro 1 Practice Rom on PS2's 75k-90k.   
  
*(note: if you are looking for the tool to save & load ghosts on Duckstation, see here: **TODO**)*

# Usage

## Saving Ghosts
After getting a ghost you're happy with, ***soft reset*** your PS2 (tap power button).   
  
You **cannot** hard reset your PS2, or the data will go away.  
  
After soft resetting your PS2, open up ULaunchElf, and launch **ghost_tool.elf** from your USB drive (mass:).

Once the app has launched, simply click "SAVE GHOST" on the menu. It will show you the data from your last ghost. If it looks correct, simply press X, and it will save it to your USB drive!

## Loading Ghosts
Before launching the Spyro 1 Practice Rom, open up ULaunchElf, and launch **ghost_tool.elf** from your USB drive (mass:).

Once the app has launched, simply click "LOAD GHOST" on the menu. It will show you all files on your USB drive that start with the text "ghost_", so make sure any file you download starts with that text.  
  
Choose any ghost you'd like to load, and then hit X. After you load it, you will be prompted to enter your Spyro 1 Practice Rom disc, and press X again.  
  
After doing this you will be brought to the PS2 browser while your disc reads. Once it reads, you can boot up the game, and you should be teleported to the level with the ghost you loaded, and it will be playing back.

# Network
Network download support is currently being worked on but not yet ready. Check back here soon for updates!

# Compiling & Building
I have currently only tested building this on windows. You should be able to on linux, but it will probably require some tweaks to build.sh.

### Windows
To build this yourself, you must have the [ps2dev toolchain](https://github.com/ps2dev/ps2toolchain) downloaded somewhere on your machine.  
  
In build.sh, change these 2 lines:
```sh
export PS2DEV= 
export PS2SDK= 
```
to point to your local toolchain.

Then, run build_release.bat. After it builds, you will find ghost_tool.elf in the *dist/* directory.