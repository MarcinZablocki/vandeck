// vandeck-link: the panel's way in. Only a WebSocket upgrade on /panel with the right bearer key gets through to the
// Durable Object; everything else is a 404, so the URL reveals nothing. PANEL_KEY is a Worker secret.

function sameKey(a, b) {
  const x = new TextEncoder().encode(a), y = new TextEncoder().encode(b);
  if (x.length !== y.length) return false;
  return crypto.subtle.timingSafeEqual(x, y);
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname !== "/panel" || req.headers.get("Upgrade") !== "websocket") return new Response("not found", { status: 404 });
    const auth = req.headers.get("Authorization") || "";
    if (!env.PANEL_KEY || env.PANEL_KEY.length < 32 || !sameKey(auth, `Bearer ${env.PANEL_KEY}`)) {
      return new Response("not found", { status: 404 });
    }
    const van = env.VAN.get(env.VAN.idFromName("van"));
    return van.fetch(new Request("https://van/panel", req));
  },
};
