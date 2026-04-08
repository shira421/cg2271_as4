"use client";

import { useEffect, useMemo, useState } from "react";
import TopTabs from "../components/TopTabs";

const periods = {
  "1h": 60 * 60 * 1000,
  "24h": 24 * 60 * 60 * 1000,
  "7d": 7 * 24 * 60 * 60 * 1000,
  "30d": 30 * 24 * 60 * 60 * 1000,
  all: null
};

function fmtTs(ts) {
  if (!ts) return "-";
  const d = new Date(ts);
  if (Number.isNaN(d.getTime())) return "-";
  return `${d.toLocaleDateString()} ${d.toLocaleTimeString()}`;
}

function inPeriod(ts, period) {
  if (!ts) return false;
  if (period === "all") return true;
  const threshold = periods[period];
  return Date.now() - new Date(ts).getTime() <= threshold;
}

export default function HistoryPage() {
  const [data, setData] = useState({ petEvents: [], commands: [] });
  const [error, setError] = useState("");
  const [period, setPeriod] = useState("7d");
  const [eventType, setEventType] = useState("all");
  const [commandType, setCommandType] = useState("all");

  useEffect(() => {
    async function loadHistory() {
      try {
        const res = await fetch("/api/history", { cache: "no-store" });
        const json = await res.json();
        if (!res.ok) throw new Error(json.error || "Failed to load history");
        setData({ petEvents: json.petEvents || [], commands: json.commands || [] });
        setError("");
      } catch (err) {
        setError(err.message || "Unknown error");
      }
    }

    loadHistory();
    const id = setInterval(loadHistory, 2500);
    return () => clearInterval(id);
  }, []);

  const filteredEvents = useMemo(() => {
    return (data.petEvents || []).filter((ev) => {
      const byTime = inPeriod(ev.ts, period);
      const byType = eventType === "all" || ev.kind === eventType;
      return byTime && byType;
    });
  }, [data.petEvents, period, eventType]);

  const filteredCommands = useMemo(() => {
    return (data.commands || []).filter((cmd) => {
      const ts = cmd.executedAt || cmd.fetchedAt || cmd.createdAt;
      const byTime = inPeriod(ts, period);
      const byType = commandType === "all" || cmd.type === commandType;
      return byTime && byType;
    });
  }, [data.commands, period, commandType]);

  return (
    <main>
      <h1>{process.env.NEXT_PUBLIC_DASHBOARD_TITLE || "PetPal Control Center"}</h1>
      <TopTabs />
      <p className="subtitle">Historical pet data and command execution records.</p>

      {error ? <div className="card" style={{ borderColor: "#d98c6f" }}>Error: {error}</div> : null}

      <section className="filter-row card">
        <label>
          Time Window
          <select value={period} onChange={(e) => setPeriod(e.target.value)}>
            <option value="1h">Last 1 hour</option>
            <option value="24h">Last 24 hours</option>
            <option value="7d">Last 7 days</option>
            <option value="30d">Last 30 days</option>
            <option value="all">All time</option>
          </select>
        </label>

        <label>
          Pet Data Type
          <select value={eventType} onChange={(e) => setEventType(e.target.value)}>
            <option value="all">All</option>
            <option value="pet_around">Pet Around</option>
            <option value="pet_not_around">Pet Not Around</option>
          </select>
        </label>

        <label>
          Command Type
          <select value={commandType} onChange={(e) => setCommandType(e.target.value)}>
            <option value="all">All</option>
            <option value="feed_now">feed_now</option>
            <option value="play_mode_toggle">play_mode_toggle</option>
          </select>
        </label>
      </section>

      <section className="history-grid">
        <article className="table-card">
          <h3>Pet Data History</h3>
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Type</th>
                  <th>Sensor</th>
                  <th>Message</th>
                  <th>Time</th>
                </tr>
              </thead>
              <tbody>
                {filteredEvents.length === 0 ? (
                  <tr><td colSpan="4">No pet data in selected range.</td></tr>
                ) : (
                  filteredEvents.map((ev) => (
                    <tr key={ev.id}>
                      <td>{ev.kind}</td>
                      <td>{ev.sensor || "-"}</td>
                      <td>{ev.message || "-"}</td>
                      <td>{fmtTs(ev.ts)}</td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </article>

        <article className="table-card">
          <h3>Command History</h3>
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Command</th>
                  <th>Status</th>
                  <th>Source</th>
                  <th>Time</th>
                </tr>
              </thead>
              <tbody>
                {filteredCommands.length === 0 ? (
                  <tr><td colSpan="4">No commands in selected range.</td></tr>
                ) : (
                  filteredCommands.map((cmd) => (
                    <tr key={cmd.id}>
                      <td>{cmd.type}</td>
                      <td>{cmd.status}</td>
                      <td>{cmd.source || "-"}</td>
                      <td>{fmtTs(cmd.executedAt || cmd.fetchedAt || cmd.createdAt)}</td>
                    </tr>
                  ))
                )}
              </tbody>
            </table>
          </div>
        </article>
      </section>
    </main>
  );
}
