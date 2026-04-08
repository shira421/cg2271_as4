"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";

const tabs = [
  { href: "/history", label: "History" },
  { href: "/", label: "Dashboard" },
  { href: "/future", label: "Future" }
];

export default function TopTabs() {
  const pathname = usePathname();

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
    </nav>
  );
}
