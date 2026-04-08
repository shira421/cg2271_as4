const express = require("express");
const cors = require("cors");
const helmet = require("helmet");
const morgan = require("morgan");
const dotenv = require("dotenv");
const {
  COMMAND_STATUS,
  enqueueCommandSchema,
  telemetrySchema,
  commandAckSchema
} = require("@petpal/shared");

dotenv.config({ path: require("path").resolve(process.cwd(), "../../.env") });
dotenv.config({ path: require("path").resolve(process.cwd(), "../../.env.local"), override: true });

const app = express();
const port = Number(process.env.PORT || 4000);

const dashboardApiKey = process.env.DASHBOARD_API_KEY || "dashboard_dev_key";
const deviceApiKey = process.env.DEVICE_API_KEY || "device_dev_key";
const allowedOrigin = process.env.ALLOWED_ORIGIN || "http://localhost:3000";
const petDistanceThresholdCm = Number(process.env.PET_DISTANCE_THRESHOLD_CM || 30);

app.use(helmet());
app.use(
  cors({
    origin: allowedOrigin,
    methods: ["GET", "POST"],
    allowedHeaders: ["Content-Type", "x-api-key"]
  })
);
app.use(express.json());
app.use(morgan("dev"));

const state = {
  commands: [],
  telemetry: null,
  commandCounter: 0,
  presence: {
    isAround: false,
    lastSeenAt: null,
    lastTriggerSensor: null,
    updatedAt: null
  },
  petEvents: []
};

function requireApiKey(expectedKey) {
  return (req, res, next) => {
    const provided = req.header("x-api-key");
    if (!provided || provided !== expectedKey) {
      return res.status(401).json({ ok: false, error: "Unauthorized" });
    }
    return next();
  };
}

function nowIso() {
  return new Date().toISOString();
}

function getLatestCommands(limit = 10) {
  return state.commands.slice(-limit).reverse();
}

function pushPetEvent(kind, sensor, message) {
  state.petEvents.push({
    id: `${Date.now()}-${Math.floor(Math.random() * 1000)}`,
    kind,
    sensor,
    message,
    ts: nowIso()
  });

  if (state.petEvents.length > 100) {
    state.petEvents = state.petEvents.slice(-100);
  }
}

app.get("/health", (_req, res) => {
  res.json({ ok: true, service: "petpal-api", ts: nowIso() });
});

app.post("/dashboard/commands", requireApiKey(dashboardApiKey), (req, res) => {
  const parsed = enqueueCommandSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ ok: false, error: parsed.error.flatten() });
  }

  const recent = state.commands
    .slice()
    .reverse()
    .find((cmd) => cmd.type === parsed.data.type && (Date.now() - new Date(cmd.createdAt).getTime()) < 2500);

  if (recent && recent.status !== COMMAND_STATUS.FAILED) {
    return res.status(409).json({ ok: false, error: "Duplicate command in debounce window", command: recent });
  }

  state.commandCounter += 1;
  const command = {
    id: String(state.commandCounter),
    type: parsed.data.type,
    source: parsed.data.source,
    status: COMMAND_STATUS.QUEUED,
    createdAt: nowIso(),
    fetchedAt: null,
    executedAt: null,
    failedAt: null,
    note: null
  };

  state.commands.push(command);
  return res.status(201).json({ ok: true, command });
});

app.get("/dashboard/state", requireApiKey(dashboardApiKey), (_req, res) => {
  const queuedCount = state.commands.filter((c) => c.status === COMMAND_STATUS.QUEUED).length;
  const latest = getLatestCommands(12);

  res.json({
    ok: true,
    telemetry: state.telemetry,
    presence: state.presence,
    petEvents: state.petEvents.slice(-30).reverse(),
    commands: latest,
    stats: {
      totalCommands: state.commands.length,
      queuedCount
    }
  });
});

app.get("/dashboard/history", requireApiKey(dashboardApiKey), (_req, res) => {
  res.json({
    ok: true,
    petEvents: state.petEvents.slice().reverse(),
    commands: state.commands.slice().reverse()
  });
});

app.get("/device/commands/next", requireApiKey(deviceApiKey), (_req, res) => {
  const next = state.commands.find((c) => c.status === COMMAND_STATUS.QUEUED);
  if (!next) {
    return res.status(204).send();
  }

  next.status = COMMAND_STATUS.FETCHED;
  next.fetchedAt = nowIso();

  return res.json({ ok: true, command: next });
});

app.post("/device/commands/:id/ack", requireApiKey(deviceApiKey), (req, res) => {
  const parsed = commandAckSchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ ok: false, error: parsed.error.flatten() });
  }

  const target = state.commands.find((c) => c.id === req.params.id);
  if (!target) {
    return res.status(404).json({ ok: false, error: "Command not found" });
  }

  target.status = parsed.data.status;
  target.note = parsed.data.note || null;

  if (parsed.data.status === COMMAND_STATUS.EXECUTED) {
    target.executedAt = nowIso();
  }
  if (parsed.data.status === COMMAND_STATUS.FAILED) {
    target.failedAt = nowIso();
  }

  return res.json({ ok: true, command: target });
});

app.post("/device/telemetry", requireApiKey(deviceApiKey), (req, res) => {
  const parsed = telemetrySchema.safeParse(req.body);
  if (!parsed.success) {
    return res.status(400).json({ ok: false, error: parsed.error.flatten() });
  }

  const ultrasonicDetected = parsed.data.distanceCm <= petDistanceThresholdCm;
  const shockDetected = parsed.data.shockDetected === true;
  const isAroundNow = ultrasonicDetected || shockDetected;
  const triggerSensor = ultrasonicDetected && shockDetected
    ? "ultrasonic+shock"
    : ultrasonicDetected
      ? "ultrasonic"
      : shockDetected
        ? "shock"
        : null;

  const wasAround = state.presence.isAround;
  const previousSensor = state.presence.lastTriggerSensor;

  if (isAroundNow && (!wasAround || previousSensor !== triggerSensor)) {
    pushPetEvent("pet_around", triggerSensor, `Pet is around (${triggerSensor} trigger)`);
  }

  if (!isAroundNow && wasAround) {
    pushPetEvent("pet_not_around", "none", "Pet not around");
  }

  state.presence = {
    isAround: isAroundNow,
    lastSeenAt: isAroundNow ? nowIso() : state.presence.lastSeenAt,
    lastTriggerSensor: isAroundNow ? triggerSensor : state.presence.lastTriggerSensor,
    updatedAt: nowIso()
  };

  state.telemetry = {
    ...parsed.data,
    receivedAt: nowIso()
  };

  return res.status(201).json({ ok: true });
});

app.listen(port, () => {
  console.log(`PetPal API listening on port ${port}`);
});
