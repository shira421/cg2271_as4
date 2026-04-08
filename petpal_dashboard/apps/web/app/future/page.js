"use client";

import TopTabs from "../components/TopTabs";

export default function FuturePage() {
  return (
    <main>
      <h1>{process.env.NEXT_PUBLIC_DASHBOARD_TITLE || "PetPal Control Center"}</h1>
      <TopTabs />
      <section className="card">
        <h3>Future</h3>
        <p>This tab is reserved for future features.</p>
      </section>
    </main>
  );
}
