import { devicePath, firebaseFetch } from "../firebase";

const GROQ_API_KEY = "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";
const GROQ_URL = "https://api.groq.com/openai/v1/chat/completions";

export async function GET() {
  try {
    const telemetry = await firebaseFetch(devicePath("/telemetry"));
    if (!telemetry) {
      return Response.json({ ok: true, summary: "No device data yet." });
    }

    const ultrasonicDetected = Number(telemetry.distanceCm) <= 30;
    const shockDetected = telemetry.shockDetected === true;
    const petIsAround = ultrasonicDetected || shockDetected;

    const prompt = `You are a caring pet monitor. Give a 1-2 sentence warm, natural summary of how the pet is doing. Start with one emoji that reflects the mood. Say "pet" not any specific animal. Do NOT mention specific numbers, sensor names, or statistics. Just give a general sense of wellbeing, whether the pet has been around recently, and if anything needs attention (like low water). Keep it conversational, like a friend giving you an update.

Current situation:
- Pet nearby: ${petIsAround ? "Yes" : "No"}
- Temperature comfort: ${telemetry.temperatureC > 20 && telemetry.temperatureC < 32 ? "comfortable" : "uncomfortable"}
- Water level: ${telemetry.waterLevel}
- Shock/activity detected: ${shockDetected ? "Yes" : "No"}
- Play mode: ${telemetry.laserOn ? "Active" : "Off"}
- Last event: ${telemetry.lastEvent}
- Last feed: ${telemetry.lastFeedTs || "Not yet today"}
- Last play session: ${telemetry.lastPlayTs || "Not yet today"}

Examples:
"😊 Your pet is hanging around and seems comfortable! Water is topped up and everything looks good."
"😴 Your pet hasn't been by in a while. Everything else looks fine, but the water could use a refill."
"🎉 Your pet just stopped by! They were recently fed and the environment is cozy."`;

    const res = await fetch(GROQ_URL, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${GROQ_API_KEY}`
      },
      body: JSON.stringify({
        model: "llama-3.1-8b-instant",
        messages: [{ role: "user", content: prompt }],
        max_tokens: 200
      })
    });

    const json = await res.json();
    console.log("[GROQ]", JSON.stringify(json, null, 2));
    const summary =
      json?.choices?.[0]?.message?.content || "Unable to generate summary.";

    return Response.json({ ok: true, summary });
  } catch (error) {
    return Response.json(
      { ok: false, summary: "Summary unavailable.", error: error.message },
      { status: 500 }
    );
  }
}