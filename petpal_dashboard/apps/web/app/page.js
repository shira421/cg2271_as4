"use client";

import { useEffect, useMemo, useState } from "react";
import TopTabs from "./components/TopTabs";

const refreshMs = 1800;
const heartbeatGraceMs = 7000;

function fmtTs(ts) {
  if (!ts) return "-";
  const d = new Date(ts);
  if (Number.isNaN(d.getTime())) return "-";
  return d.toLocaleTimeString();
}

function prettySensor(sensor) {
  if (!sensor) return "-";
  if (sensor === "ultrasonic+shock") return "Ultrasonic + Shock";
  if (sensor === "ultrasonic") return "Ultrasonic";
  if (sensor === "shock") return "Shock";
  return sensor;
}

function numberOrDash(value, digits = 0) {
  if (value === null || value === undefined || Number.isNaN(Number(value))) {
    return "-";
  }
  return Number(value).toFixed(digits);
}

function waterLevelLabel(telemetry) {
  const normalized = String(telemetry?.waterLevel || "").toUpperCase();
  if (normalized === "EMPTY") return "EMPTY";
  if (normalized === "LOW") return "LOW";
  if (normalized === "OK" || normalized === "FULL") return "OK";

  const raw = Number(telemetry?.waterLevelRaw);
  if (!Number.isFinite(raw)) return "-";
  if (raw <= 50) return "EMPTY";
  if (raw <= 500) return "LOW";
  return "OK";
}

export default function HomePage() {
  const [state, setState] = useState(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [playOptimistic, setPlayOptimistic] = useState(null);

  async function loadState() {
    try {
      const res = await fetch("/api/state", { cache: "no-store" });
      const json = await res.json();
      if (!res.ok) throw new Error(json.error || "State fetch failed");
      setState(json);
      setError("");
    } catch (err) {
      setError(err.message || "Unknown error");
    }
  }

  useEffect(() => {
    loadState();
    const id = setInterval(loadState, refreshMs);
    return () => clearInterval(id);
  }, []);

  async function send(type) {
    setBusy(true);
    setError("");
    try {
      const res = await fetch("/api/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ type })
      });
      const json = await res.json();
      if (!res.ok) throw new Error(json.error || "Command failed");
      await loadState();
    } catch (err) {
      setError(err.message || "Unknown error");
    } finally {
      setBusy(false);
    }
  }

  const telemetry = state?.telemetry;
  const presence = state?.presence;
  const playFromTelemetry = telemetry?.playServoMoving === true || telemetry?.mode === "playing";
  const playIsOn = playOptimistic === null ? playFromTelemetry : playOptimistic;

  useEffect(() => {
      setPlayOptimistic(null);
  }, [telemetry?.mode, telemetry?.playServoMoving]);

  const heartbeat = useMemo(() => {
    if (!telemetry?.receivedAt) {
      return {
        connected: false,
        label: "PetPal: Disconnected",
        detail: "Waiting for heartbeat from ESP32",
        ageMs: null
      };
    }

    const ageMs = Date.now() - new Date(telemetry.receivedAt).getTime();
    const connected = ageMs <= heartbeatGraceMs;

    if (!connected) {
      return {
        connected: false,
        label: "PetPal: Disconnected",
        detail: "Heartbeat stale. Not ready for live device control.",
        ageMs
      };
    }

    return {
      connected: true,
      label: "PetPal: Connected",
      detail: "Ready to send and receive instructions and telemetry.",
      ageMs
    };
  }, [telemetry?.receivedAt]);

  const statusPill = useMemo(() => {
    if (!telemetry?.receivedAt) return "No device data yet";
    const ageMs = Date.now() - new Date(telemetry.receivedAt).getTime();
    if (ageMs < 6000) return "Device online";
    return "Device stale";
  }, [telemetry?.receivedAt]);

  const petAroundLabel = presence?.isAround ? "Pet is around!" : "Pet not around";

  return (
    <main>
      <h1>{process.env.NEXT_PUBLIC_DASHBOARD_TITLE || "PetPal Control Center"}</h1>
      <TopTabs />
      <p className="subtitle">Smart pet insights from DHT, ultrasonic, and shock sensors.</p>

      <section className="hero-layout">
        <aside className="weather-left">
          <div className="temp-display">{numberOrDash(telemetry?.temperatureC, 1)}<span className="unit">C</span></div>
          <div className="humidity-display">Humidity {numberOrDash(telemetry?.humidityPct, 1)}%</div>
          <div className="left-footnote">Updated {fmtTs(telemetry?.receivedAt)}</div>
        </aside>

        <section className="today-right">
          <h2>Today</h2>
          <article className={`heartbeat-card ${heartbeat.connected ? "is-connected" : "is-disconnected"}`}>
            <div className="heartbeat-row">
              <span className="dot" aria-hidden="true" />
              <strong>{heartbeat.label}</strong>
            </div>
            <div className="heartbeat-detail">{heartbeat.detail}</div>
          </article>

          <div className="today-actions">
            <button className="btn-feed action-button" onClick={() => send("feed_now")} disabled={busy}>
              Dispense Treat
            </button>
            <button
              className={`btn-play action-button ${playIsOn ? "is-on" : "is-off"}`}
              onClick={async () => {
                setPlayOptimistic(!playIsOn);
                await send("play_mode_toggle");
              }}
              disabled={busy}
            >
              {playIsOn ? "Play Mode: ON" : "Play Mode: OFF"}
            </button>
          </div>
        </section>
      </section>

      {error ? <div className="card" style={{ borderColor: "#d98c6f" }}>Error: {error}</div> : null}

      <h2 className="highlights-title">Highlights</h2>
      <div className="highlights-grid">
        <section className={`card pet-card ${presence?.isAround ? "pet-around" : "pet-away"}`}>
          <h3>{petAroundLabel}</h3>
          <p>Trigger Sensor: {prettySensor(presence?.lastTriggerSensor)}</p>
          <p>Distance: {telemetry?.distanceCm ?? "-"} cm</p>
          <p>Shock Sensor: {telemetry?.shockDetected ? "Triggered" : "Idle"}</p>
          <p>Updated: {fmtTs(presence?.updatedAt)}</p>
        </section>

        <section className="card">
          <h3>Water Level</h3>
          <p className="metric-big">{waterLevelLabel(telemetry)}</p>
          <p>Device Status: {statusPill}</p>
          <p>Uptime: {telemetry?.uptimeSec ?? "-"} sec</p>
        </section>

        <section className="card">
          <h3>Care Summary</h3>
          <p>Last Time Around: {fmtTs(presence?.lastSeenAt)}</p>
          <p>Last Feed: {fmtTs(telemetry?.lastFeedTs)}</p>
          <p>Last Play: {fmtTs(telemetry?.lastPlayTs)}</p>
        </section>
      </div>

    </main>
  );
}
