const dotenv = require("dotenv");
const path = require("path");
const { COMMAND_TYPES, COMMAND_STATUS } = require("@petpal/shared");

dotenv.config({ path: path.resolve(process.cwd(), "../../.env") });
dotenv.config({ path: path.resolve(process.cwd(), "../../.env.local"), override: true });

const baseUrl = process.env.API_BASE_URL || "http://localhost:4000";
const deviceKey = process.env.DEVICE_API_KEY || "device_dev_key";
const deviceId = process.env.SIMULATOR_DEVICE_ID || "sim-esp32s2-01";
const profile = process.env.SIMULATOR_PROFILE || "normal";
const intervalMs = Number(process.env.SIMULATOR_INTERVAL_MS || 2500);

const sim = {
  mode: "idle",
  uptimeSec: 0,
  temperatureC: 26.8,
  humidityPct: 62,
  distanceCm: 42,
  shockDetected: false,
  waterLevelRaw: 380,
  laserOn: false,
  playServoMoving: false,
  feederTriggered: false,
  buzzerOn: false,
  lastEvent: "sim_boot",
  lastFeedTs: null,
  lastPlayTs: null
};

function randomIn(min, max) {
  return Math.random() * (max - min) + min;
}

function mutateTelemetry() {
  sim.uptimeSec += Math.floor(intervalMs / 1000);
  sim.temperatureC = Math.max(20, Math.min(33, sim.temperatureC + randomIn(-0.35, 0.35)));
  sim.humidityPct = Math.max(35, Math.min(85, sim.humidityPct + randomIn(-1.2, 1.2)));
  sim.distanceCm = Math.max(10, Math.min(80, sim.distanceCm + randomIn(-2.5, 2.5)));
  sim.waterLevelRaw = Math.max(40, Math.min(450, Math.round(sim.waterLevelRaw + randomIn(-4, 2))));
  sim.shockDetected = Math.random() < 0.08;
}

async function api(pathname, options = {}) {
  const res = await fetch(`${baseUrl}${pathname}`, {
    ...options,
    headers: {
      "Content-Type": "application/json",
      "x-api-key": deviceKey,
      ...(options.headers || {})
    }
  });
  return res;
}

async function postTelemetry() {
  mutateTelemetry();
  const payload = {
    deviceId,
    mode: sim.mode,
    uptimeSec: sim.uptimeSec,
    temperatureC: Number(sim.temperatureC.toFixed(1)),
    humidityPct: Number(sim.humidityPct.toFixed(1)),
    distanceCm: Number(sim.distanceCm.toFixed(1)),
    shockDetected: sim.shockDetected,
    waterLevelRaw: sim.waterLevelRaw,
    laserOn: sim.laserOn,
    playServoMoving: sim.playServoMoving,
    feederTriggered: sim.feederTriggered,
    buzzerOn: sim.buzzerOn,
    lastEvent: sim.lastEvent,
    lastFeedTs: sim.lastFeedTs,
    lastPlayTs: sim.lastPlayTs
  };

  await api("/device/telemetry", {
    method: "POST",
    body: JSON.stringify(payload)
  });
}

async function ack(id, status, note) {
  await api(`/device/commands/${id}/ack`, {
    method: "POST",
    body: JSON.stringify({ status, note })
  });
}

async function processCommand(command) {
  if (!command) return;

  if (profile === "slow-network") {
    await new Promise((r) => setTimeout(r, 1200));
  }

  if (command.type === COMMAND_TYPES.FEED_NOW) {
    sim.mode = "feeding";
    sim.feederTriggered = true;
    sim.buzzerOn = true;
    sim.lastEvent = "feed_started";
    sim.lastFeedTs = new Date().toISOString();

    await ack(command.id, COMMAND_STATUS.EXECUTED, "Feed servo and buzzer simulated");

    setTimeout(() => {
      sim.mode = "idle";
      sim.feederTriggered = false;
      sim.buzzerOn = false;
      sim.lastEvent = "feed_completed";
    }, 1500);
    return;
  }

  if (command.type === COMMAND_TYPES.PLAY_MODE_TOGGLE) {
    const active = !(sim.mode === "playing");
    sim.mode = active ? "playing" : "idle";
    sim.laserOn = active;
    sim.playServoMoving = active;
    sim.lastEvent = active ? "play_started" : "play_stopped";
    sim.lastPlayTs = new Date().toISOString();

    await ack(command.id, COMMAND_STATUS.EXECUTED, "Laser and play-servo simulated");
    return;
  }

  await ack(command.id, COMMAND_STATUS.FAILED, "Unsupported command");
}

async function pollCommand() {
  if (profile === "offline-flap" && Math.random() < 0.22) {
    return;
  }

  const res = await api("/device/commands/next", { method: "GET" });
  if (res.status === 204) return;
  if (!res.ok) return;

  const json = await res.json();
  await processCommand(json.command);
}

async function tick() {
  try {
    await postTelemetry();
    await pollCommand();
  } catch (err) {
    console.error("Simulator tick failed:", err.message);
  }
}

console.log(`Simulator started for ${deviceId} (${profile}) -> ${baseUrl}`);
setInterval(tick, intervalMs);
tick();
