/**
 * Test-only device control client. It uses the same Bearer token and poll/ACK
 * contract as the future ESP32 owner task, but never opens an audio stream.
 */
export class MockWatchClient {
  constructor(baseUrl, { deviceId = "watch-001", token = "test-token" } = {}) {
    this.baseUrl = baseUrl;
    this.deviceId = deviceId;
    this.token = token;
  }

  async poll() {
    const response = await fetch(
      `${this.baseUrl}/v1/music/remote-commands/next?device_id=${encodeURIComponent(this.deviceId)}`,
      { headers: { Authorization: `Bearer ${this.token}` } },
    );
    return response.json();
  }

  async ack(commandId, result = { state: "executed" }, snapshot = null) {
    const response = await fetch(
      `${this.baseUrl}/v1/music/remote-commands/${encodeURIComponent(commandId)}/ack?device_id=${encodeURIComponent(this.deviceId)}`,
      {
        method: "POST",
        headers: {
          Authorization: `Bearer ${this.token}`,
          "Content-Type": "application/json",
        },
        body: JSON.stringify({ result, snapshot }),
      },
    );
    return response.json();
  }

  async waitAndAck({ timeoutMs = 1000, result, snapshot } = {}) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const command = await this.poll();
      if (command.state === "claimed") {
        return this.ack(command.command_id, result, snapshot);
      }
      await new Promise((resolve) => setTimeout(resolve, 20));
    }
    throw new Error("mock_watch_command_timeout");
  }
}
