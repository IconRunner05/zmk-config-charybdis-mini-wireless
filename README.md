# Charybdis Mini ZMK Configuration

This repository contains the ZMK firmware configuration for the Charybdis Mini Wireless keyboard.

It features a robust local build environment running the exact same Docker image as the GitHub Actions CI, ensuring bit-for-bit parity without requiring a local Zephyr SDK installation on your host system.

---

## Keymap

![Charybdis keymap](keymap-drawer/charybdis.svg)

> [!NOTE]
> This diagram is generated automatically by [keymap-drawer](https://github.com/caksoylar/keymap-drawer) on every push that changes the keymap (see `.github/workflows/draw-keymap.yml`). The physical layout comes from `config/info.json` — the same file the [keymap-editor](https://nickcoutsos.github.io/keymap-editor/) web GUI reads. This is the `charybdis-full` layout; the `master` branch renders its own diagram from its own keymap.

---

## Prerequisites

Before building locally, ensure you have the following installed on your host system:
* **Docker Desktop**: Running and active.
* **Make** & **Bash**: Standard on macOS and Linux.
* **rsync**: Used to safely sync the config files to the build staging directory.

---

## Local Build Instructions

Follow these commands to configure, build, and maintain your local firmware:

### 1. First-Time Initialization
Run the initialization target to set up the persistent Docker volume, pull the compilation image, and perform the initial `west init`, `west update`, and CMake package registration:
```bash
make init
```
> [!NOTE]
> The initial `west update` download is around 1–2 GB. This may take a few minutes depending on your network connection.

### 2. Build All Firmware
To compile the left half, right half (with ZMK Studio and RPC support), and the settings reset flasher in one command, run:
```bash
make build
```

### 3. Build Specific Targets
You can also compile individual targets to save time:
* **Left Half Only**:
  ```bash
  make left
  ```
* **Right Half Only** (Includes ZMK Studio support):
  ```bash
  make right
  ```
* **Settings Reset Flasher**:
  ```bash
  make reset
  ```

### 4. Cache & Clean Targets
* **Clear Build Caches**: Delete intermediate build caches within the Docker volume while leaving ZMK and Zephyr modules intact:
  ```bash
  make pristine
  ```
* **Full Teardown**: Remove the Docker volume, sentinel files, and all staging directories to start completely fresh:
  ```bash
  make clean
  ```

---

## Configuring Build Output Locations

The build script saves compiled `.uf2` firmware files to two locations. You can configure these target directories directly in the `Makefile` under the **Paths** section (around lines 48-54):

```makefile
# Staging dirs on the host: space-free, Docker bind-mount safe
CONFIG_STAGE   := $(HOME)/Docker/zmk-config
FIRMWARE_STAGE := $(HOME)/Docker/zmk-fw

# Final .uf2 output directory inside the repo (gitignored)
FIRMWARE_DIR   := $(REPO_ROOT)/firmware
```

### 1. Final Repository Output (`FIRMWARE_DIR`)
This is the directory in your local git repository where the final `.uf2` binaries are copied.
* **Default**: `$(REPO_ROOT)/firmware` (a gitignored folder named `firmware` at the root of this repository).
* **To change it**: Update the `FIRMWARE_DIR` variable to any absolute or relative path on your host machine.

### 2. Docker Staging Directory (`FIRMWARE_STAGE`)
Due to file I/O limitations and path spaces on macOS (especially inside iCloud/spaced directories like `Mobile Documents`), the Docker container writes outputs to a space-free staging directory inside your user's home directory.
* **Default**: `$(HOME)/Docker/zmk-fw` (resolves to `~/Docker/zmk-fw`).
* **To change it**: Update the `FIRMWARE_STAGE` variable to a space-free, Docker bind-mount safe directory on your local machine.

---

## Introspecting Firmware Binaries

To view the size and timestamp of the compiled `.uf2` binaries in your local output folder, run:
```bash
make firmware
```
