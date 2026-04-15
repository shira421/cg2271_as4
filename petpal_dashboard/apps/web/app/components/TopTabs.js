"use client";

import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";

const tabs = [
  { href: "/history", label: "History" },
  { href: "/", label: "Dashboard" }
];

export default function TopTabs({ onAiSummary }) {
  const pathname = usePathname();
  const router = useRouter();

  return (
    <nav className="top-tabs" aria-label="Main tabs">
      {tabs.map((tab) => {
        const active = pathname === tab.href;
        return (
          <Link key={tab.href} href={tab.href} className={`top-tab ${active ? "active" : ""}`}>
            {tab.label}
          </Link>
        );
      })}
      <button
        className="top-tab"
        onClick={() => {
          if (onAiSummary) {
            onAiSummary();
          } else {
            router.push("/?ai=1");
          }
        }}
        type="button"
      >
        AI Summary
      </button>
    </nav>
  );
}