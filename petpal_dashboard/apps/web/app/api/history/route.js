export async function GET() {
  try {
    const base = process.env.RAILWAY_API_BASE_URL || "http://localhost:4000";
    const key = process.env.SERVER_DASHBOARD_API_KEY || "dashboard_dev_key";

    const res = await fetch(`${base}/dashboard/history`, {
      method: "GET",
      headers: {
        "x-api-key": key
      },
      cache: "no-store"
    });

    const json = await res.json();
    return Response.json(json, { status: res.status });
  } catch (error) {
    return Response.json({ ok: false, error: error.message || "Proxy error" }, { status: 500 });
  }
}
