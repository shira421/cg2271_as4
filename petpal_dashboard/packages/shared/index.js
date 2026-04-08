const { z } = require("zod");

const COMMAND_TYPES = {
  FEED_NOW: "feed_now",
  PLAY_MODE_TOGGLE: "play_mode_toggle"
};

const COMMAND_STATUS = {
  QUEUED: "queued",
  FETCHED: "fetched",
  EXECUTED: "executed",
  FAILED: "failed"
};

const commandTypeSchema = z.enum([COMMAND_TYPES.FEED_NOW, COMMAND_TYPES.PLAY_MODE_TOGGLE]);
const commandStatusSchema = z.enum([
  COMMAND_STATUS.QUEUED,
  COMMAND_STATUS.FETCHED,
  COMMAND_STATUS.EXECUTED,
  COMMAND_STATUS.FAILED
]);

const enqueueCommandSchema = z.object({
  type: commandTypeSchema,
  source: z.string().min(1).max(64).default("dashboard")
});

const telemetrySchema = z.object({
  deviceId: z.string().min(1).max(64),
  mode: z.string().min(1).max(32),
  uptimeSec: z.number().int().nonnegative(),
  temperatureC: z.number().optional().nullable(),
  humidityPct: z.number().optional().nullable(),
  distanceCm: z.number().nonnegative(),
  shockDetected: z.boolean().optional().default(false),
  waterLevelRaw: z.number().int().nonnegative(),
  laserOn: z.boolean(),
  playServoMoving: z.boolean(),
  feederTriggered: z.boolean(),
  buzzerOn: z.boolean(),
  lastEvent: z.string().min(1).max(128),
  lastFeedTs: z.string().datetime().nullable(),
  lastPlayTs: z.string().datetime().nullable()
});

const commandAckSchema = z.object({
  status: z.enum([COMMAND_STATUS.FETCHED, COMMAND_STATUS.EXECUTED, COMMAND_STATUS.FAILED]),
  note: z.string().max(256).optional()
});

module.exports = {
  COMMAND_TYPES,
  COMMAND_STATUS,
  enqueueCommandSchema,
  telemetrySchema,
  commandAckSchema
};
