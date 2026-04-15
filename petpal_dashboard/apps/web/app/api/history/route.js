import { devicePath, firebaseFetch } from "../firebase";

export async function GET() {
  try {
    const [events, commands] = await Promise.all([
      firebaseFetch(devicePath("/history/events")),
      firebaseFetch(devicePath("/history/commands"))
    ]);

    const petEvents = events
      ? Object.entries(events).map(([id, ev]) => ({ id, ...ev })).reverse()
      : [];

    const cmdList = commands
      ? Object.entries(commands).map(([id, cmd]) => ({ id, ...cmd })).reverse()
      : [];

    return Response.json({ ok: true, petEvents, commands: cmdList });
  } catch (error) {
    return Response.json({ ok: false, error: error.message || "Firebase history error" }, { status: 500 });
  }
}