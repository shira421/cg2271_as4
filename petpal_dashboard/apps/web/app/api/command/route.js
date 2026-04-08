export async function POST(request) {
  try {
    const body = await request.json();
    const base = process.env.RAILWAY_API_BASE_URL || "http://localhost:4000";
    const key = process.env.SERVER_DASHBOARD_API_KEY || "dashboard_dev_key";

    const res = await fetch(`${base}/dashboard/commands`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "x-api-key": key
      },
      body: JSON.stringify({ type: body.type, source: "web-dashboard" }),
      cache: "no-store"
    });

    const json = await res.json();
    return Response.json(json, { status: res.status });
  } catch (error) {
    return Response.json({ ok: false, error: error.message || "Proxy error" }, { status: 500 });
  }
}
