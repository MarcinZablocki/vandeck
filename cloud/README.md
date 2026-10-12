# VanDeck remote (Cloudflare)

A website for the van's current values and Truma controls, with nothing listening on the panel. The panel keeps one
outbound WebSocket to Cloudflare, so it works behind Starlink and campsite NAT.

```
phone ──HTTPS──▶ vandeck (Worker, behind Cloudflare Access)
                    │  /api/status, /api/cmd
                    ▼
                 Van (Durable Object: the panel's socket and its latest status)
                    ▲
panel ──WSS────▶ vandeck-link (Worker, panel key)
```

- `vandeck` (`wrangler.toml`, `src/site.js`, `src/page.html`) serves the page and the API. The page has two tabs:
  **Van** (batteries, solar, heating, tanks) and **GPS** (the panel's GPS fix, or its last known location, with map
  links; open it directly with `#gps`). It only reads what the panel sends. Cloudflare Access sits in
  front of it, and the Worker also verifies Access's signed token (`Cf-Access-Jwt-Assertion`) against your team's
  keys and the application's AUD. If Access is ever switched off, it fails closed with 403. Commands must be
  same-origin, and only `heater_on`, `setpoint_c` (5–30), `water` (`off|eco|hot`) and `fan` (`eco|high`) get through.
- `vandeck-link` (`wrangler.link.toml`, `src/link.js`) only accepts a WebSocket upgrade on `/panel` with
  `Authorization: Bearer <PANEL_KEY>` and returns 404 for anything else. It is **not** behind Access, because the
  panel can't do an email login.
- `Van` (`src/van.js`) holds one panel connection (a new one replaces the old) and stores the last status. A command
  waits up to 8 s for the panel's acknowledgement.
- The panel side is `firmware/main/net/relay_svc.c`. It sends the `GET /status` JSON every 10 s and right after a
  command, and applies commands through the same `truma_set_*` calls as the touch screen.

## Secrets

Nothing secret is in this repository. `.gitignore` keeps `.dev.vars`, `.env*` and `.wrangler/` out.

| Secret | Where | What |
|---|---|---|
| `PANEL_KEY` | `vandeck-link` Worker secret, and the panel's NVS (`vd_relay`) | 64 hex chars, the panel's bearer key. Never printed or logged. |
| `ACCESS_TEAM` | `vandeck` Worker secret | Zero Trust team name, the `<team>` in `<team>.cloudflareaccess.com` |
| `ACCESS_AUD` | `vandeck` Worker secret | Audience tag of the Access application |

No account ID is needed in `wrangler.toml`; Wrangler uses the logged-in account.

## Setup

```bash
cd cloud
npx wrangler login
npx wrangler deploy                          # vandeck: defines the Durable Object, so first
npx wrangler deploy -c wrangler.link.toml    # vandeck-link
```

Then:

1. Dashboard: Workers & Pages, then vandeck, then Settings, then Domains & Routes. Enable Cloudflare Access on
   `workers.dev`. In Zero Trust, Access, Applications, give its policy an **Emails** include rule with your own
   address (not "Emails ending in"). Leave `vandeck-link` without Access.
2. Store the team name and AUD:
   ```bash
   printf '%s' '<team>' | npx wrangler secret put ACCESS_TEAM
   printf '%s' '<aud>'  | npx wrangler secret put ACCESS_AUD
   ```
3. Create the panel key and give it to the panel in one go, so it never lands in a file. You need the panel's IP
   and its OTA key:
   ```bash
   KEY=$(openssl rand -hex 32)
   printf '%s' "$KEY" | npx wrangler secret put PANEL_KEY -c wrangler.link.toml
   { printf '%s\n' "<ota key>"; sleep 3
     printf 'relay url wss://vandeck-link.<subdomain>.workers.dev/panel\n'; sleep 1
     printf 'relay key %s\n' "$KEY"; sleep 1; printf 'relay on\nquit\n'; } | nc -w 10 <panel ip> 23
   unset KEY
   ```
   `relay` on the console shows the state (`Connected` within a few seconds).

To rotate the key, repeat step 3. The old key stops working as soon as the secret is replaced. Preview URLs are off
(`preview_urls = false`), so each Worker has exactly one address.
