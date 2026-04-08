# PetPal Standalone Dashboard

This folder is a full, independent dashboard stack.

It does not require ESP32 or MCXC444 firmware to run. A built-in simulator acts as a virtual ESP32 device.

## Architecture

- `apps/web`: Next.js dashboard (deploy to Vercel)
- `apps/api`: Express API (deploy to Railway)
- `apps/simulator`: Virtual device worker (optional in production; required for hardware-free demo)
- `packages/shared`: Shared command enums and schema validation

## Command Scope

Two command types are supported:

- `feed_now`: models food dispenser servo + buzzer behavior
- `play_mode_toggle`: models laser emitter + play-servo movement behavior

## Local Quick Start

1. Copy env template:
   - PowerShell: `Copy-Item .env.example .env`
2. Install dependencies:
   - `npm install`
3. Start dashboard + API (real device mode, no simulator):
   - `npm run dev`
4. Open dashboard:
   - `http://localhost:3000`

Optional simulator mode:

- Start with fake device data enabled: `npm run dev:with-sim`
- Start simulator only: `npm run dev:sim`

## Environment Variables

See `.env.example` for all keys.

Required for local run:

- `DASHBOARD_API_KEY`
- `DEVICE_API_KEY`
- `RAILWAY_API_BASE_URL`
- `SERVER_DASHBOARD_API_KEY`

## API Endpoints

### Dashboard-facing
- `POST /dashboard/commands` with header `x-api-key: DASHBOARD_API_KEY`
- `GET /dashboard/state` with header `x-api-key: DASHBOARD_API_KEY`

### Device-facing
- `GET /device/commands/next` with header `x-api-key: DEVICE_API_KEY`
- `POST /device/commands/:id/ack` with header `x-api-key: DEVICE_API_KEY`
- `POST /device/telemetry` with header `x-api-key: DEVICE_API_KEY`

## Deploy

### Railway

Deploy `apps/api` as a Node service.
Set env:
- `PORT`
- `DASHBOARD_API_KEY`
- `DEVICE_API_KEY`
- `ALLOWED_ORIGIN` (your Vercel domain)

### Vercel

Deploy `apps/web`.
Set env:
- `RAILWAY_API_BASE_URL`
- `SERVER_DASHBOARD_API_KEY`
- `NEXT_PUBLIC_DASHBOARD_TITLE`

### Optional simulator hosting

You can run `apps/simulator` locally or as a separate Railway worker when hardware is unavailable.

## Hardware Cutover Later

When your real ESP32 is ready, point it to the same API endpoints and use `DEVICE_API_KEY`.
No dashboard code changes are required.

## ESP32 Local Test File

Use this sketch to test real device send/receive against your local dashboard stack:

- `esp32-test/dashboard_device_test.ino`

What it does:

- Connects ESP32 to WiFi
- Polls `GET /device/commands/next`
- Executes `feed_now` and `play_mode_toggle` logic
- Sends `POST /device/commands/:id/ack`
- Sends `POST /device/telemetry` every few seconds

Before flashing:

1. Start local stack in this folder with `npm run dev`
2. In the sketch, set:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `API_BASE_URL` to your computer LAN IP (example `http://192.168.1.25:4000`)
   - `DEVICE_API_KEY` to match your `.env`
3. Install Arduino library dependency:
   - `ArduinoJson` (by Benoit Blanchon)

Notes:

- Do not use `localhost` in `API_BASE_URL` on ESP32. It must be your computer IP on the same network.
- This test sketch is independent of your existing firmware under `ESP32/` and `MCXC444/`.
