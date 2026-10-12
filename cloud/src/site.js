// vandeck: the website and its API. Cloudflare Access (one-time code by email) sits in front of the whole Worker;
// this code checks Access's signed token as well, so if Access is ever switched off the site fails closed instead of
// opening the heater to anyone. Secrets: ACCESS_TEAM (the Zero Trust team name), ACCESS_AUD (the application's
// audience tag).
import PAGE from "./page.html";
export { Van } from "./van.js";

let certs = { at: 0, keys: [] };

async function accessKeys(team) {
  if (Date.now() - certs.at < 3600_000 && certs.keys.length) return certs.keys;
  const r = await fetch(`https://${team}.cloudflareaccess.com/cdn-cgi/access/certs`);
  if (!r.ok) throw new Error("certs");
  certs = { at: Date.now(), keys: (await r.json()).keys || [] };
  return certs.keys;
}

function b64url(s) {
  s = s.replace(/-/g, "+").replace(/_/g, "/");
  return Uint8Array.from(atob(s + "===".slice((s.length + 3) % 4)), (c) => c.charCodeAt(0));
}

// Returns the signed-in email, or null.
async function accessUser(req, env) {
  if (!env.ACCESS_TEAM || !env.ACCESS_AUD) return null;
  const jwt = req.headers.get("Cf-Access-Jwt-Assertion");
  if (!jwt) return null;
  const [h, p, sig] = jwt.split(".");
  if (!h || !p || !sig) return null;
  try {
    const head = JSON.parse(new TextDecoder().decode(b64url(h)));
    const claims = JSON.parse(new TextDecoder().decode(b64url(p)));
    if (head.alg !== "RS256") return null;
    const jwk = (await accessKeys(env.ACCESS_TEAM)).find((k) => k.kid === head.kid);
    if (!jwk) return null;
    const key = await crypto.subtle.importKey("jwk", jwk, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["verify"]);
    const ok = await crypto.subtle.verify("RSASSA-PKCS1-v1_5", key, b64url(sig), new TextEncoder().encode(`${h}.${p}`));
    const aud = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
    if (!ok || !aud.includes(env.ACCESS_AUD) || claims.iss !== `https://${env.ACCESS_TEAM}.cloudflareaccess.com`) return null;
    if (!claims.exp || claims.exp * 1000 < Date.now()) return null;
    return claims.email || "signed in";
  } catch {
    return null;
  }
}

// Only these fields, in these ranges, ever reach the panel (which checks them again).
function validCommand(b) {
  if (!b || typeof b !== "object") return null;
  const set = {};
  if ("heater_on" in b) { if (typeof b.heater_on !== "boolean") return null; set.heater_on = b.heater_on; }
  if ("setpoint_c" in b) {
    const v = Number(b.setpoint_c);
    if (!Number.isFinite(v) || v < 5 || v > 30) return null;
    set.setpoint_c = Math.round(v * 2) / 2;
  }
  if ("water" in b) { if (!["off", "eco", "hot"].includes(b.water)) return null; set.water = b.water; }
  if ("fan" in b) { if (!["eco", "high"].includes(b.fan)) return null; set.fan = b.fan; }
  return Object.keys(set).length ? set : null;
}

const SECURITY = {
  "Cache-Control": "no-store",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
  "X-Frame-Options": "DENY",
  "Content-Security-Policy": "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'",
};

export default {
  async fetch(req, env) {
    const user = await accessUser(req, env);
    if (!user) {
      return new Response("Sign-in required. This site is protected by Cloudflare Access.\n", { status: 403, headers: SECURITY });
    }
    const url = new URL(req.url);
    const van = env.VAN.get(env.VAN.idFromName("van"));

    if (url.pathname === "/") return new Response(PAGE, { headers: { ...SECURITY, "Content-Type": "text/html; charset=utf-8" } });
    if (url.pathname === "/api/status" && req.method === "GET") {
      const r = await van.fetch("https://van/status");
      return new Response(r.body, { status: r.status, headers: { ...SECURITY, "Content-Type": "application/json" } });
    }
    if (url.pathname === "/api/cmd" && req.method === "POST") {
      // Same-origin only: Access's cookie would otherwise ride along on a forged cross-site POST.
      if (req.headers.get("Origin") !== url.origin) return new Response("bad origin", { status: 403, headers: SECURITY });
      let body;
      try { body = await req.json(); } catch { body = null; }
      const set = validCommand(body);
      if (!set) return Response.json({ ok: false, error: "invalid command" }, { status: 400, headers: SECURITY });
      const r = await van.fetch("https://van/cmd", { method: "POST", body: JSON.stringify(set) });
      return new Response(r.body, { status: r.status, headers: { ...SECURITY, "Content-Type": "application/json" } });
    }
    return new Response("not found", { status: 404, headers: SECURITY });
  },
};
