#!/bin/bash
# Build script for Ghost Tool
#
# Requirements:
#   - ps2dev toolchain at /c/ps2dev_real
#   - sfx.wav in the project root (../sfx.wav relative to this script)
#
# Default builds are USB-only. Set NETWORK=1 to include unfinished online
# leaderboard loading plus dev9/netman/smap/ps2ip IRX blobs from ps2sdk.
#
# Network UI iteration (stub leaderboard, no TCP/IP - see net.c):
#   Bash/Git Bash:  NETWORK=1 DEBUG_UI=1 bash build.sh
#   PowerShell:      $env:NETWORK='1'; $env:DEBUG_UI='1'; bash ./build.sh
#   cmd.exe:         set NETWORK=1 && set DEBUG_UI=1 && bash build.sh
# Release USB-only - clears sticky DEBUG_UI from a prior session:
#   bash build.sh     or   RELEASE=1 bash build.sh
#   build_release.bat  (unsets NETWORK / DEBUG_UI / GHOST_LOADER_DEBUG_UI)

export PS2DEV=/c/ps2dev_real
export PS2SDK=$PS2DEV/ps2sdk
export PATH=$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2SDK/bin:$PATH

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SRCDIR"

DISTDIR="$SRCDIR/dist"
OUTPUT_ELF="$DISTDIR/ghost_tool.elf"
SFX_WAV="$SRCDIR/../sfx.wav"

# NETWORK=1 enables unfinished online leaderboard loading. It is off by default
# for public USB-only builds.
EE_FEATURE_FLAGS=""
if [ "${RELEASE:-}" = "1" ]; then
    unset DEBUG_UI GHOST_LOADER_DEBUG_UI
fi
if [ "${NETWORK:-}" = "1" ]; then
    EE_FEATURE_FLAGS="$EE_FEATURE_FLAGS -DNETWORK"
    echo ">>> NETWORK: online leaderboard code enabled"
fi
if [ "${GHOST_LOADER_DEBUG_UI:-}" = "1" ] || [ "${DEBUG_UI:-}" = "1" ]; then
    if [ "${NETWORK:-}" = "1" ]; then
        EE_FEATURE_FLAGS="$EE_FEATURE_FLAGS -DGHOST_LOADER_DEBUG_UI"
        echo ">>> DEBUG UI: network stack not used; stub leaderboard (edit net.c LbLoadDebugLeaderboardStub)"
    else
        echo ">>> DEBUG UI ignored: set NETWORK=1 to build the leaderboard UI stub"
    fi
    echo ""
fi

echo "=== Ghost Loader Build ==="
echo ""

# Clean
mkdir -p "$DISTDIR"
rm -f ghost_tool.elf ghost_loader.elf "$OUTPUT_ELF" \
      ghostwr.irx ghostwr.o ghostrd.irx ghostrd.o \
      main.o net.o ui_tex.o ui_tex_table.o ui_sky_preview.o skybox_table.o \
      ghostwr_irx.o ghostwr_irx.s ghostrd_irx.o ghostrd_irx.s \
      iomanX_irx.o  iomanX_irx.s  fileXio_irx.o fileXio_irx.s \
      usbd_irx.o    usbd_irx.s    usbhdfsd_irx.o usbhdfsd_irx.s \
      audsrv_irx.o  audsrv_irx.s  sfx_wav.o sfx_wav.s \
      ps2dev9_irx.o ps2dev9_irx.s \
      netman_irx.o  netman_irx.s \
      smap_irx.o    smap_irx.s \
      ps2ip_irx.o   ps2ip_irx.s
rm -rf gen
mkdir -p gen assets
SKYBOX_DIR="$SRCDIR/../ghost-server/skybox"
rm -f "$SRCDIR"/gen/sky_*.s "$SRCDIR"/gen/sky_*.o "$SRCDIR"/gen/skybox_table.c 2>/dev/null || true

# ===================================================
# Step 1: Build the custom IOP module (ghostwr.irx)
# ===================================================
echo "[1/7] Compiling ghostwr.c (IOP module)..."
iop-gcc -D_IOP -O2 -G0 -Wall \
    -I"$PS2SDK/iop/include" \
    -I"$PS2SDK/common/include" \
    -c ghostwr.c -o ghostwr.o || exit 1

echo "[2/7] Linking ghostwr.irx..."
iop-gcc -nostdlib -o ghostwr.irx ghostwr.o -lgcc || exit 1

echo "[2b/7] Compiling ghostrd.c (IOP read module)..."
iop-gcc -D_IOP -O2 -G0 -Wall \
    -I"$PS2SDK/iop/include" \
    -I"$PS2SDK/common/include" \
    -c ghostrd.c -o ghostrd.o || exit 1

echo "[2c/7] Linking ghostrd.irx..."
iop-gcc -nostdlib -o ghostrd.irx ghostrd.o -lgcc || exit 1

# ===================================================
# Step 2: Convert all IRX modules + assets to assembly
# ===================================================
echo "[3/7] Converting IRX modules to linkable objects..."

# USB stack
bin2s ghostwr.irx                         ghostwr_irx.s  ghostwr_irx  || exit 1
bin2s ghostrd.irx                         ghostrd_irx.s  ghostrd_irx  || exit 1
bin2s "$PS2SDK/iop/irx/iomanX.irx"       iomanX_irx.s   iomanX_irx   || exit 1
bin2s "$PS2SDK/iop/irx/fileXio.irx"      fileXio_irx.s  fileXio_irx  || exit 1
bin2s "$PS2SDK/iop/irx/usbd.irx"         usbd_irx.s     usbd_irx     || exit 1
bin2s "$PS2SDK/iop/irx/usbhdfsd.irx"     usbhdfsd_irx.s usbhdfsd_irx || exit 1
bin2s "$PS2SDK/iop/irx/audsrv.irx"       audsrv_irx.s   audsrv_irx   || exit 1

if [ "${NETWORK:-}" = "1" ]; then
    # Network stack (ps2dev9 -> netman -> smap -> ps2ip, order matters at load time)
    bin2s "$PS2SDK/iop/irx/ps2dev9.irx"  ps2dev9_irx.s  ps2dev9_irx  || exit 1
    bin2s "$PS2SDK/iop/irx/netman.irx"   netman_irx.s   netman_irx   || exit 1
    bin2s "$PS2SDK/iop/irx/smap.irx"     smap_irx.s     smap_irx     || exit 1
    bin2s "$PS2SDK/iop/irx/ps2ip.irx"    ps2ip_irx.s    ps2ip_irx    || exit 1
fi

# Audio SFX
if [ ! -f "$SFX_WAV" ]; then
    echo "ERROR: sfx.wav not found at $SFX_WAV"
    echo "Place sfx.wav in the project root directory."
    exit 1
fi
bin2s "$SFX_WAV" sfx_wav.s sfx_wav || exit 1

# ===================================================
# UI textures: PNG -> RAW (Pillow) + bin2s symbols
# ===================================================
echo "[3b/7] Generating UI textures (gen/)..."
if command -v python3 >/dev/null 2>&1; then
    python3 "$SRCDIR/scripts/png_to_gs_rgba32.py" --assets "$SRCDIR/assets" --out "$SRCDIR/gen" || exit 1
elif command -v python >/dev/null 2>&1; then
    python "$SRCDIR/scripts/png_to_gs_rgba32.py" --assets "$SRCDIR/assets" --out "$SRCDIR/gen" || exit 1
elif command -v py >/dev/null 2>&1; then
    py -3 "$SRCDIR/scripts/png_to_gs_rgba32.py" --assets "$SRCDIR/assets" --out "$SRCDIR/gen" || exit 1
else
    echo "ERROR: No Python found (python3 / python / py). Install Python 3 and Pillow:  pip install pillow"
    exit 1
fi

SKY_OBJS=""
if [ "${NETWORK:-}" = "1" ]; then
    echo "[3c/7] Skybox embed (ghost-server/skybox -> ELF)..."
    if command -v python3 >/dev/null 2>&1; then
        python3 "$SRCDIR/scripts/skybox_png_to_raw.py" || true
    elif command -v python >/dev/null 2>&1; then
        python "$SRCDIR/scripts/skybox_png_to_raw.py" || true
    elif command -v py >/dev/null 2>&1; then
        py -3 "$SRCDIR/scripts/skybox_png_to_raw.py" || true
    fi
    if command -v python3 >/dev/null 2>&1; then
        python3 "$SRCDIR/scripts/gen_skybox_table.py" || exit 1
    elif command -v python >/dev/null 2>&1; then
        python "$SRCDIR/scripts/gen_skybox_table.py" || exit 1
    elif command -v py >/dev/null 2>&1; then
        py -3 "$SRCDIR/scripts/gen_skybox_table.py" || exit 1
    else
        exit 1
    fi

    if [ -d "$SKYBOX_DIR" ]; then
        for raw in "$SKYBOX_DIR"/*.raw; do
            [ -f "$raw" ] || continue
            base=$(basename "$raw" .raw)
            sym="sky_${base}"
            # Paths relative to $SRCDIR (cwd) so spaces in the repo path do not break the linker argv.
            bin2s "$raw" "gen/sky_${base}.s" "$sym" || exit 1
            ee-as -G0 "gen/sky_${base}.s" -o "gen/sky_${base}.o" || exit 1
            SKY_OBJS="$SKY_OBJS gen/sky_${base}.o"
        done
    fi
else
    echo "[3c/7] Skybox embed skipped (NETWORK=0)"
fi

TEXTURE_OBJS=""
for raw in "$SRCDIR"/gen/tex_*.raw; do
    [ -f "$raw" ] || continue
    base=$(basename "$raw" .raw)
    sym="$base"
    ( cd "$SRCDIR/gen" && bin2s "$base.raw" "$base.s" "$sym" ) || exit 1
    ee-as -G0 "$SRCDIR/gen/$base.s" -o "$SRCDIR/gen/$base.o" || exit 1
    TEXTURE_OBJS="$TEXTURE_OBJS $SRCDIR/gen/$base.o"
done

# ===================================================
# Step 3: Assemble all objects
# ===================================================
echo "[4/7] Assembling IRX + asset objects..."
ee-as -G0 ghostwr_irx.s  -o ghostwr_irx.o  || exit 1
ee-as -G0 ghostrd_irx.s  -o ghostrd_irx.o  || exit 1
ee-as -G0 iomanX_irx.s   -o iomanX_irx.o   || exit 1
ee-as -G0 fileXio_irx.s  -o fileXio_irx.o  || exit 1
ee-as -G0 usbd_irx.s     -o usbd_irx.o     || exit 1
ee-as -G0 usbhdfsd_irx.s -o usbhdfsd_irx.o || exit 1
ee-as -G0 audsrv_irx.s   -o audsrv_irx.o   || exit 1
NETWORK_OBJS=""
NETWORK_LIBS=""
if [ "${NETWORK:-}" = "1" ]; then
    ee-as -G0 ps2dev9_irx.s  -o ps2dev9_irx.o  || exit 1
    ee-as -G0 netman_irx.s   -o netman_irx.o   || exit 1
    ee-as -G0 smap_irx.s     -o smap_irx.o     || exit 1
    ee-as -G0 ps2ip_irx.s    -o ps2ip_irx.o    || exit 1
    NETWORK_OBJS="net.o ui_sky_preview.o skybox_table.o ps2dev9_irx.o netman_irx.o smap_irx.o ps2ip_irx.o"
    NETWORK_LIBS="-lps2ip -lnetman"
fi
ee-as -G0 sfx_wav.s      -o sfx_wav.o      || exit 1

# ===================================================
# Step 4: Compile EE main.c
# ===================================================
echo "[5/7] Compiling main.c (EE)..."
ee-gcc -D_EE $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -I"$PS2SDK/ee/include" \
    -I"$PS2SDK/common/include" \
    -I"$SRCDIR" \
    -c main.c -o main.o || exit 1

# ===================================================
# Step 5: Compile net.c
# ===================================================
echo "[6/7] Compiling net.c + ui textures (EE)..."
# PS2IP_DNS: enable lwip_gethostbyname in resolve_host_ipv4 (ghosts.waffler.uk etc.); without this,
# only dotted IPv4 works — hostnames always fail to resolve.
if [ "${NETWORK:-}" = "1" ]; then
ee-gcc -D_EE -DPS2IP_DNS $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -I"$PS2SDK/ee/include" \
    -I"$PS2SDK/common/include" \
    -I"$SRCDIR" \
    -c net.c -o net.o || exit 1
fi

ee-gcc -D_EE $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -I"$PS2SDK/ee/include" \
    -I"$PS2SDK/common/include" \
    -I"$SRCDIR" \
    -c "$SRCDIR/ui_tex.c" -o ui_tex.o || exit 1

ee-gcc -D_EE $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -I"$PS2SDK/ee/include" \
    -I"$PS2SDK/common/include" \
    -I"$SRCDIR" \
    -c "$SRCDIR/gen/ui_tex_table.c" -o ui_tex_table.o || exit 1

if [ "${NETWORK:-}" = "1" ]; then
ee-gcc -D_EE $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -I"$PS2SDK/ee/include" \
    -I"$PS2SDK/common/include" \
    -I"$SRCDIR" \
    -c "$SRCDIR/ui_sky_preview.c" -o ui_sky_preview.o || exit 1

ee-gcc -D_EE $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -I"$PS2SDK/ee/include" \
    -I"$PS2SDK/common/include" \
    -I"$SRCDIR" \
    -c "$SRCDIR/gen/skybox_table.c" -o skybox_table.o || exit 1
fi

# ===================================================
# Step 6: Link final ELF
# ===================================================
echo "[7/7] Linking dist/ghost_tool.elf..."
ee-gcc -mno-crt0 -T"$PS2SDK/ee/startup/linkfile" \
    -D_EE $EE_FEATURE_FLAGS -O2 -G0 -Wall \
    -o "$OUTPUT_ELF" \
    "$PS2SDK/ee/startup/crt0.o" \
    main.o ui_tex.o ui_tex_table.o $NETWORK_OBJS \
    ghostwr_irx.o ghostrd_irx.o iomanX_irx.o fileXio_irx.o \
    usbd_irx.o usbhdfsd_irx.o audsrv_irx.o sfx_wav.o \
    $TEXTURE_OBJS $SKY_OBJS \
    -L"$PS2SDK/ee/lib" \
    -lfileXio -lpad -lpatches -ldebug \
    -lfont -ldraw -lgraph -lpacket -ldma \
    $NETWORK_LIBS -laudsrv -lc -lkernel || exit 1

echo ""
echo "=== BUILD SUCCESS ==="
ls -la "$OUTPUT_ELF" ghostwr.irx ghostrd.irx
echo ""
echo "dist/ghost_tool.elf built successfully :)"
echo ""
