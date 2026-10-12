// One Durable Object instance ("van") holds the panel's WebSocket, its latest status and the commands in flight.
// The panel connects through the vandeck-link Worker; the website reads and commands through the vandeck Worker.
// Uses the hibernation API: the socket stays open while the object sleeps between messages.

const ACK_TIMEOUT_MS = 8000;

export class Van {
  constructor(ctx) {
    this.ctx = ctx;
    this.pending = new Map(); // command id -> resolve
  }

  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === "/panel") return this.panel(req);
    if (url.pathname === "/status") return Response.json(await this.snapshot());
    if (url.pathname === "/cmd" && req.method === "POST") return this.command(await req.json());
    return new Response("not found", { status: 404 });
  }

  // A new connection replaces the old one (the panel reconnected before the old socket timed out).
  panel(req) {
    if (req.headers.get("Upgrade") !== "websocket") return new Response("expected a WebSocket", { status: 426 });
    for (const old of this.ctx.getWebSockets("panel")) old.close(1000, "replaced");
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, ["panel"]);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, msg) {
    let m;
    try { m = JSON.parse(msg); } catch { return; }
    if (m.type === "status" && m.data && typeof m.data === "object") {
      await this.ctx.storage.put("last", { at: Date.now(), data: m.data });
    } else if (m.type === "ack" && typeof m.id === "string") {
      const done = this.pending.get(m.id);
      if (done) { this.pending.delete(m.id); done({ ok: !!m.ok, error: m.error ?? null }); }
    }
  }

  webSocketClose(ws, code) { ws.close(code, "closing"); }
  webSocketError() {}

  async snapshot() {
    const last = await this.ctx.storage.get("last");
    return {
      connected: this.ctx.getWebSockets("panel").length > 0,
      at: last?.at ?? null,
      status: last?.data ?? null,
    };
  }

  // body: { heater_on?, setpoint_c?, water?, fan? }, already validated by the site Worker.
  async command(set) {
    const ws = this.ctx.getWebSockets("panel")[0];
    if (!ws) return Response.json({ ok: false, error: "the panel isn't connected" }, { status: 503 });
    const id = crypto.randomUUID();
    const reply = new Promise((resolve) => {
      this.pending.set(id, resolve);
      setTimeout(() => {
        if (this.pending.delete(id)) resolve({ ok: false, error: "the panel didn't answer" });
      }, ACK_TIMEOUT_MS);
    });
    ws.send(JSON.stringify({ type: "cmd", id, set }));
    const r = await reply;
    return Response.json(r, { status: r.ok ? 200 : 502 });
  }
}
