const deviceId = process.env.FIREBASE_DEVICE_ID || "esp32s2-petpal";

export function firebaseConfigured() {
  return Boolean(process.env.FIREBASE_DATABASE_URL);
}

export function firebaseUrl(path) {
  const base = process.env.FIREBASE_DATABASE_URL.replace(/\/+$/, "");
  const auth = process.env.FIREBASE_AUTH ? `?auth=${process.env.FIREBASE_AUTH}` : "";
  return `${base}${path}.json${auth}`;
}

export function devicePath(child = "") {
  return `/petpal/devices/${deviceId}${child}`;
}

export async function firebaseFetch(path, init = {}) {
  if (!firebaseConfigured()) {
    throw new Error("Set FIREBASE_DATABASE_URL in petpal_dashboard/apps/web/.env.local");
  }

  const res = await fetch(firebaseUrl(path), {
    ...init,
    cache: "no-store"
  });

  const text = await res.text();
  const json = text ? JSON.parse(text) : null;

  if (!res.ok) {
    throw new Error(json?.error || `Firebase HTTP ${res.status}`);
  }

  return json;
}
