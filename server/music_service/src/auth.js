import crypto from "node:crypto";

function bearerToken(request) {
  const header = request.headers.authorization || "";
  const match = /^Bearer\s+(.+)$/i.exec(header);
  return match ? match[1] : "";
}

export function authorize(request, deviceId, config) {
  const expected = config.deviceTokens.get(deviceId);
  if (!expected) return { ok: false, status: 403, errorCode: "device_not_allowed" };
  const actual = bearerToken(request);
  if (!actual) return { ok: false, status: 401, errorCode: "missing_bearer_token" };
  const actualBytes = Buffer.from(actual);
  const expectedBytes = Buffer.from(expected);
  if (
    actualBytes.length !== expectedBytes.length ||
    !crypto.timingSafeEqual(actualBytes, expectedBytes)
  ) {
    return { ok: false, status: 403, errorCode: "invalid_device_token" };
  }
  return { ok: true };
}

export function authorizeMcp(request, config) {
  if (!config.mcpToken) return { ok: false, status: 503, errorCode: "mcp_not_configured" };
  const actual = bearerToken(request);
  if (!actual) return { ok: false, status: 401, errorCode: "missing_mcp_token" };
  const actualBytes = Buffer.from(actual);
  const expectedBytes = Buffer.from(config.mcpToken);
  if (
    actualBytes.length !== expectedBytes.length ||
    !crypto.timingSafeEqual(actualBytes, expectedBytes)
  ) {
    return { ok: false, status: 403, errorCode: "invalid_mcp_token" };
  }
  return { ok: true };
}
