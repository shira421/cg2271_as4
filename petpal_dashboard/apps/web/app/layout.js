import "./globals.css";

export const metadata = {
  title: "PetPal Control Center",
  description: "Standalone dashboard for feed and play controls"
};

export default function RootLayout({ children }) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
