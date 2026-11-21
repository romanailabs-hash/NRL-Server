#!/usr/bin/env bash
# init_rootfs.sh — Build a minimal BusyBox rootfs for sandbox testing
set -euo pipefail

ROOTFS_DIR="$(cd "$(dirname "$0")/.." && pwd)/build/rootfs/base"

if [ -d "$ROOTFS_DIR" ] && [ -f "$ROOTFS_DIR/bin/busybox" ]; then
    echo "Rootfs already exists at $ROOTFS_DIR"
    exit 0
fi

echo "Creating minimal rootfs at: $ROOTFS_DIR"
mkdir -p "$ROOTFS_DIR"/{bin,sbin,etc,dev,proc,sys,tmp,usr/bin,usr/sbin,lib,lib64}

# Check for busybox static binary
if command -v busybox &>/dev/null; then
    BUSYBOX=$(command -v busybox)
elif [ -f /usr/bin/busybox ]; then
    BUSYBOX=/usr/bin/busybox
else
    echo "BusyBox not found. Downloading static busybox binary..."
    BUSYBOX="$ROOTFS_DIR/bin/busybox"
    # Try to download a static busybox
    if command -v wget &>/dev/null; then
        wget -q -O "$BUSYBOX" "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox" || true
    elif command -v curl &>/dev/null; then
        curl -sL -o "$BUSYBOX" "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox" || true
    fi
    if [ ! -f "$BUSYBOX" ] || [ ! -s "$BUSYBOX" ]; then
        echo "Could not download BusyBox. Create a minimal test binary instead."
        # Create a minimal /bin/echo so sandbox tests still work
        cat > "$ROOTFS_DIR/bin/echo" << 'EOF'
#!/bin/sh
echo "$@"
EOF
        chmod +x "$ROOTFS_DIR/bin/echo"
        echo "Created minimal /bin/echo stub"
        exit 0
    fi
fi

if [ -f "$BUSYBOX" ] && [ -x "$BUSYBOX" ]; then
    cp "$BUSYBOX" "$ROOTFS_DIR/bin/busybox"
    chmod +x "$ROOTFS_DIR/bin/busybox"
    
    # Install busybox applets
    cd "$ROOTFS_DIR" && "$ROOTFS_DIR/bin/busybox" --install -s 2>/dev/null || true
    
    echo "BusyBox rootfs created successfully"
else
    echo "Creating stub rootfs..."
    cat > "$ROOTFS_DIR/bin/echo" << 'EOF'
#!/bin/sh
echo "$@"
EOF
    chmod +x "$ROOTFS_DIR/bin/echo"
fi

# Create essential device nodes
if [ -e /dev/null ]; then mknod -m 666 "$ROOTFS_DIR/dev/null" c 1 3 2>/dev/null || true; fi
if [ -e /dev/zero ]; then mknod -m 666 "$ROOTFS_DIR/dev/zero" c 1 5 2>/dev/null || true; fi
if [ -e /dev/urandom ]; then mknod -m 666 "$ROOTFS_DIR/dev/urandom" c 1 9 2>/dev/null || true; fi

echo "Rootfs initialization complete: $ROOTFS_DIR"
