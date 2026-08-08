# Bambuddy Display for ESP32-4848S040CI

Touch display for Bambuddy on the **Sunton ESP32-4848S040CI / ESP32-4848S040CIY1**.

This project brings Bambuddy directly to a 4.0" ESP32 touch display: printer status, AMS, queue, archive, camera snapshot, and smart plug controls on a compact panel next to your printer.

**Powered by Bambuddy**

<p>
  <img src="docs/1000020755.jpg" alt="Bambuddy display next to Bambu Studio showing printer status" width="100%">
</p>

## What is this?

`bambuddy-display` is a native LVGL interface for an ESP32-S3 display board. The device connects to your Bambuddy instance and shows key printer information directly on the touchscreen.

It is built to work with [Bambuddy](https://github.com/maziggy/bambuddy), a self-hosted command center for Bambu Lab printers, and uses Bambuddy's REST API for device data and actions. Bambuddy also provides API reference documentation in its docs and repository.

Current features:

- Live printer status for Bambu printers
- AMS view
- Queue view with start actions
- Archive view with reprint and delete
- Printer camera snapshot
- Smart plug controls
- Wi-Fi setup directly on the device
- Bambuddy configuration for HTTP or MQTT

## Target Hardware

This project is built for the following board:

- **Sunton ESP32-4848S040CI**
- PlatformIO board ID: `esp32-4848S040CIY1`
- ESP32-S3
- 480x480 touch display
- 4.0 inch
- USB-C for power and flashing

<p>
  <img src="docs/aliexpress_4848S040CI.png" alt="Sunton ESP32-4848S040CI product image" width="560">
</p>

If you mean that square ESP32 touchscreen board with USB-C: yes, this is the one.

## Gallery

### Bambuddy in action

<table>
  <tr>
    <td align="center" width="50%">
      <img src="docs/1000020759.jpg" alt="Queue view on the Bambuddy display" width="100%" height="320" style="object-fit: cover; object-position: center;">
    </td>
    <td align="center" width="50%">
      <img src="docs/1000020763.jpg" alt="Smart plug controls on the Bambuddy display" width="100%" height="320" style="object-fit: cover; object-position: center;">
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <img src="docs/1000020761.jpg" alt="Bambuddy display during printing" width="100%" height="320" style="object-fit: cover; object-position: center;">
    </td>
    <td align="center" width="50%">
      <img src="docs/1000020762.jpg" alt="Close-up of the Bambuddy display interface" width="100%" height="320" style="object-fit: cover; object-position: center;">
    </td>
  </tr>
  <tr>
    <td align="center" colspan="2">
      <img src="docs/1000020757.jpg" alt="Main printer status view on the Bambuddy display" width="49%" height="320" style="object-fit: cover; object-position: center;">
    </td>
  </tr>
</table>

More photos are available in [`docs/`](docs/).

## Get Started

### 1. Get the hardware

You need:

- a **Sunton ESP32-4848S040CI** display board
- a **USB-C data cable**
- a computer with PlatformIO
- a running **Bambuddy** instance

### 2. Clone the repo

```bash
git clone <your-repo-url>
cd bambuddy-display
```

### 3. Prepare your config

`include/secrets.example.h` is the template for your initial configuration.

```bash
cp include/secrets.example.h include/secrets.h
```

Then fill in `include/secrets.h` with:

- `BAMBUDDY_DEFAULT_URL`
- `BAMBUDDY_DEFAULT_API_KEY`
- `BAMBUDDY_DEFAULT_PRINTER_ID`
- optional camera token
- optional MQTT credentials

Important: these values are only the **initial defaults for the first boot**. After that, the display stores its own settings in NVS.

### 4. Build the firmware

```bash
pio run
```

### 5. Flash the board

Connect the board via **USB-C**, then run:

```bash
pio run --target upload
```

Open the serial monitor:

```bash
pio device monitor
```

## First Boot

After flashing:

1. Power up the display over USB-C
2. Configure Wi-Fi on the device
3. Verify the Bambuddy URL and API access
4. Set the printer ID
5. Optionally switch to MQTT

At that point, the display should start showing live printer status.

## UI Layout

The interface is built as a horizontal tile view:

- **AMS**
- **Status**
- **Queue**
- **Archive**
- **System**

Inside the system area you will find:

- Wi-Fi
- Settings
- Smart Plugs
- Jog controls

## Connection to Bambuddy

The display supports two data sources:

- **HTTP polling**
- **MQTT**

MQTT is a good choice if you want status changes to appear on the display as quickly as possible. HTTP is the simpler option to get started.

This project primarily talks to Bambuddy through its REST API, with optional MQTT support for faster status updates.

## Development

### Stack

- PlatformIO
- Arduino framework
- ESP32-S3
- LVGL 9.2.2
- `esp32-smartdisplay`

### Build

```bash
pio run
```

### Upload

```bash
pio run --target upload
```

### Monitor

```bash
pio device monitor
```

## Tested With

Tested with **Bambuddy v0.2.4.4**.
Tested on a **Bambu Lab P1S**.

As of **August 8, 2026**, this appears to be the latest stable Bambuddy release on GitHub. If you are on a newer release, small API or behavior differences may exist.

## Project Status

This is no longer a generic display demo. It is a dedicated **Bambuddy companion display** for Bambu printers.

A sensible next step would be a small web flasher page so the firmware can be installed directly from the browser, similar to WLED or Tasmota.

## Thanks

Thanks to the [Bambuddy project](https://github.com/maziggy/bambuddy) and its REST API.
