import { createServer } from "node:http";
import { createApp } from "./app.js";

const app = createApp();
const server = createServer((request, response) => {
  app(request, response).catch(() => {
    if (!response.headersSent) {
      response.statusCode = 500;
      response.setHeader("Content-Type", "application/json; charset=utf-8");
      response.end(JSON.stringify({ state: "error", error_code: "internal_error" }));
    } else {
      response.destroy();
    }
  });
});

server.listen(app.config.port, app.config.host, () => {
  console.log(`music-service listening on ${app.config.host}:${app.config.port}`);
});

function shutdown() {
  server.close(() => {
    app.closeMusic();
    process.exit(0);
  });
}

process.on("SIGTERM", shutdown);
process.on("SIGINT", shutdown);
