import {
    decodeApplyWifiConfigResponse,
    decodeGetWifiStatusResponse,
    decodeScanResultResponse,
    decodeScanStartResponse,
    decodeScanStatusResponse,
    decodeSessionSetup0Response,
    decodeSetWifiConfigResponse,
    encodeApplyWifiConfigRequest,
    encodeGetWifiStatusRequest,
    encodeScanResultRequest,
    encodeScanStartRequest,
    encodeScanStatusRequest,
    encodeSessionSetup0Request,
    encodeSetWifiConfigRequest,
} from "./prov_proto_bundle.js";

const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

const ENDPOINT_PROTO_VER = "/proto-ver";
const ENDPOINT_PROV_SESSION = "/prov-session";
const ENDPOINT_PROV_SCAN = "/prov-scan";
const ENDPOINT_PROV_CONFIG = "/prov-config";
const WIFI_SCAN_GROUP_CHANNELS = 5;
const WIFI_SCAN_PERIOD_MS = 120;
const WIFI_STATUS_POLL_INTERVAL_MS = 2000;
const WIFI_STATUS_POLL_ATTEMPTS = 15;

const DEFAULT_PORTAL_INFO = {
    title: "Device portal is reachable",
    note: "Loading portal state from the provisioning client.",
    apiVersion: "unknown",
    scanSupported: false,
};

/**
 * @brief Normalize portal metadata into the UI shape.
 * @param {object} raw Raw portal payload.
 * @returns {{title: string, note: string, apiVersion: string, scanSupported: boolean}} Stable portal info.
 */
function normalizePortalInfo(raw) {
    const apiVersion = raw.apiVersion ?? raw.api_version ?? DEFAULT_PORTAL_INFO.apiVersion;
    const scanSupported = Boolean(raw.scanSupported ?? raw.scan_supported);

    return {
        title: raw.title ?? DEFAULT_PORTAL_INFO.title,
        note: raw.note ?? `API version: ${apiVersion}. Scan supported: ${scanSupported ? "yes" : "no"}.`,
        apiVersion,
        scanSupported,
    };
}

/**
 * @brief Normalize a Wi-Fi network entry into the UI shape.
 * @param {object} raw Raw network payload.
 * @returns {{ssid: string, security: string, rssi: number}} Stable network entry.
 */
function normalizeWifiNetwork(raw) {
    return {
        ssid: raw.ssid ?? raw.name ?? "Unknown network",
        security: raw.security ?? raw.auth ?? "Security unknown",
        rssi: raw.rssi ?? raw.signal ?? -127,
    };
}

/**
 * @brief Normalize a Wi-Fi network list into the UI shape.
 * @param {object|Array<object>} raw Raw scan payload.
 * @returns {{ssid: string, security: string, rssi: number}[]} Stable network list.
 */
function normalizeWifiList(raw) {
    const networks = Array.isArray(raw)
        ? raw
        : Array.isArray(raw.networks)
            ? raw.networks
            : Array.isArray(raw.items)
                ? raw.items
                : [];

    return networks.map((network) => normalizeWifiNetwork(network));
}

/**
 * @brief 异步等待一段时间，避免 UI 轮询阻塞主线程。
 * @param {number} delayMs 等待时长，单位毫秒。
 * @returns {Promise<void>} 到时后 resolve。
 */
function sleep(delayMs) {
    return new Promise((resolve) => {
        setTimeout(resolve, delayMs);
    });
}

function ensureBytes(value) {
    if (value instanceof Uint8Array) {
        return value;
    }

    if (value instanceof ArrayBuffer) {
        return new Uint8Array(value);
    }

    if (ArrayBuffer.isView(value)) {
        return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    }

    if (typeof value === "string") {
        return TEXT_ENCODER.encode(value);
    }

    if (value == null) {
        return new Uint8Array(0);
    }

    throw new TypeError("unsupported binary body");
}

function decodeResponseText(buffer) {
    return TEXT_DECODER.decode(buffer).replace(/\u0000+$/u, "");
}

function resolveEndpoint(baseUrl, endpoint) {
    if (!baseUrl) {
        return endpoint;
    }

    const trimmedBase = baseUrl.endsWith("/") ? baseUrl.slice(0, -1) : baseUrl;
    return `${trimmedBase}${endpoint}`;
}

function parsePortalInfoResponse(text) {
    const trimmed = text.trim();

    if (!trimmed) {
        return normalizePortalInfo({});
    }

    try {
        const parsed = JSON.parse(trimmed);
        const provInfo = parsed.prov ?? {};
        const capabilities = Array.isArray(provInfo.cap) ? provInfo.cap : [];
        const apiVersion = typeof provInfo.ver === "string" ? provInfo.ver : trimmed;
        const securityNote = capabilities.includes("no_sec") ? "security0 available" : "secure session required";

        return normalizePortalInfo({
            apiVersion,
            scanSupported: capabilities.includes("wifi_scan"),
            note: `API version: ${apiVersion}. Scan supported: ${capabilities.includes("wifi_scan") ? "yes" : "no"}. ${securityNote}.`,
        });
    } catch {
        return normalizePortalInfo({
            apiVersion: trimmed,
            note: `API version: ${trimmed}. Scan supported: no.`,
        });
    }
}

/**
 * @brief 判断 fetch 失败是否更像“SoftAP 门户已主动关闭”而不是业务响应错误。
 *
 * SoftAP 配网成功后，设备会停止 provisioning 并关闭 AP/HTTPD。浏览器随后继续请求
 * `/prov-config` 时，通常拿到的是宿主环境抛出的网络错误，而不是标准 HTTP 状态码。
 *
 * @param {unknown} error fetch 或 transport 抛出的异常对象。
 * @returns {boolean} true 表示更像是门户被主动关闭。
 */
function isLikelyPortalClosedError(error) {
    const message = error instanceof Error ? error.message : String(error ?? "");

    return [
        "Failed to fetch",
        "NetworkError",
        "Load failed",
        "fetch failed",
        "The network connection was lost",
    ].some((token) => message.includes(token));
}

function createHttpBinaryTransport(fetchImpl, baseUrl = "") {
    const state = {
        cookie: "",
    };

    return {
        /**
         * @brief Send binary payload to an official provisioning endpoint.
         * @param {string} endpoint Provisioning endpoint path.
         * @param {Uint8Array|string|ArrayBuffer} body Serialized request body.
         * @returns {Promise<Uint8Array>} Raw response payload.
         */
        async post(endpoint, body) {
            const headers = {
                Accept: "application/octet-stream, text/plain, application/json",
                "Content-Type": "application/octet-stream",
            };

            // Browsers send same-origin cookies automatically. Keep a manual copy
            // as a fallback for environments that expose Set-Cookie to JS.
            if (state.cookie) {
                headers.Cookie = state.cookie;
            }

            const response = await fetchImpl(resolveEndpoint(baseUrl, endpoint), {
                method: "POST",
                headers,
                body: ensureBytes(body),
                credentials: "same-origin",
                cache: "no-store",
            });

            const setCookie = response.headers.get("set-cookie");
            if (setCookie) {
                state.cookie = setCookie.split(";")[0];
            }

            if (!response.ok) {
                throw new Error(`provisioning endpoint ${endpoint} failed with HTTP ${response.status}`);
            }

            return new Uint8Array(await response.arrayBuffer());
        },
    };
}

/**
 * @brief Create the provisioning client used by the AP portal UI.
 * @param {{baseUrl?: string, fetchImpl?: typeof fetch}=} options Optional transport overrides.
 * @returns {{getPortalInfo: function(): Promise<object>, initSession: function(): Promise<object>, getWifiStatus: function(): Promise<object>, applyWifiConfig: function(): Promise<object>, waitForWifiConnection: function(Object=): Promise<object>, scanWifi: function(): Promise<Array<object>>, sendWifiConfig: function(string, string): Promise<void>}} Provisioning client facade.
 */
export function createProvisioningClient(options = {}) {
    const fetchImpl = options.fetchImpl ?? globalThis.fetch?.bind(globalThis);
    if (typeof fetchImpl !== "function") {
        throw new Error("fetch is required for the provisioning client");
    }

    const transport = createHttpBinaryTransport(fetchImpl, options.baseUrl ?? "");
    const state = {
        sessionReady: false,
        sessionInfo: null,
    };

    /**
     * @brief 建立 security0 session，后续 scan/config/status 都复用同一会话。
     * @returns {Promise<{secVer: number, msg: number, status: number, ok: boolean}>} 会话结果。
     */
    async function initSession() {
        if (state.sessionReady) {
            return state.sessionInfo;
        }

        const responseBuffer = await transport.post(
            ENDPOINT_PROV_SESSION,
            encodeSessionSetup0Request(),
        );
        const response = decodeSessionSetup0Response(responseBuffer);

        if (response.secVer !== 0 || response.status !== 0) {
            throw new Error("security0 session setup failed");
        }

        state.sessionReady = true;
        state.sessionInfo = response;
        return response;
    }

    /**
     * @brief 读取设备当前的 Wi-Fi STA 状态。
     * @returns {Promise<object>} 设备返回的最小状态对象。
     */
    async function getWifiStatus() {
        await initSession();
        const responseBuffer = await transport.post(
            ENDPOINT_PROV_CONFIG,
            encodeGetWifiStatusRequest(),
        );
        return decodeGetWifiStatusResponse(responseBuffer);
    }

    /**
     * @brief 发送 ApplyConfig，让设备开始按刚写入的凭据发起连接。
     * @returns {Promise<object>} ApplyConfig 最小响应。
     */
    async function applyWifiConfig() {
        await initSession();

        const applyResponse = decodeApplyWifiConfigResponse(await transport.post(
            ENDPOINT_PROV_CONFIG,
            encodeApplyWifiConfigRequest(),
        ));
        if (applyResponse.status !== 0) {
            throw new Error("apply wifi config failed");
        }

        return applyResponse;
    }

    /**
     * @brief 轮询 Wi-Fi 连接状态，直到成功、失败或达到轮询上限。
     * @param {{maxAttempts?: number, intervalMs?: number}=} options 轮询次数和间隔配置。
     * @returns {Promise<object>} 包含最终阶段和设备状态的结果。
     */
    async function waitForWifiConnection(options = {}) {
        const maxAttempts = options.maxAttempts ?? WIFI_STATUS_POLL_ATTEMPTS;
        const intervalMs = options.intervalMs ?? WIFI_STATUS_POLL_INTERVAL_MS;
        let lastStatus = null;

        for (let attempt = 0; attempt < maxAttempts; attempt += 1) {
            let status = null;

            try {
                status = await getWifiStatus();
            } catch (error) {
                if (isLikelyPortalClosedError(error)) {
                    return {
                        phase: "portal_closed",
                        settled: true,
                        status: lastStatus,
                        detail: error instanceof Error ? error.message : String(error ?? ""),
                    };
                }

                throw error;
            }

            lastStatus = status;

            if (status.wifiState === "connected") {
                return {
                    phase: "connected",
                    settled: true,
                    status,
                };
            }

            // 设备已经给出失败终态时，立即停止轮询，避免 UI 继续显示“连接中”。
            if (status.wifiState === "failed") {
                return {
                    phase: "failed",
                    settled: true,
                    status,
                };
            }

            if (attempt + 1 < maxAttempts) {
                await sleep(intervalMs);
            }
        }

        return {
            phase: lastStatus?.wifiState === "connecting" ? "connecting" : "timeout",
            settled: false,
            status: lastStatus,
        };
    }

    return {
        /**
         * @brief Return normalized portal metadata for the UI.
         * @returns {Promise<{title: string, note: string, apiVersion: string, scanSupported: boolean}>} Portal metadata.
         */
        async getPortalInfo() {
            const responseBuffer = await transport.post(
                ENDPOINT_PROTO_VER,
                TEXT_ENCODER.encode("---"),
            );
            return parsePortalInfoResponse(decodeResponseText(responseBuffer));
        },

        /**
         * @brief Establish a minimal security0 session.
         * @returns {Promise<{secVer: number, msg: number, status: number, ok: boolean}>} Session result.
         */
        async initSession() {
            return initSession();
        },

        /**
         * @brief Read the current Wi-Fi connection status via prov-config.
         * @returns {Promise<object>} Decoded Wi-Fi status payload.
         */
        async getWifiStatus() {
            return getWifiStatus();
        },

        /**
         * @brief 对外暴露 apply helper，供上层在需要时拆开发送和应用两个步骤。
         * @returns {Promise<object>} ApplyConfig 最小响应。
         */
        async applyWifiConfig() {
            return applyWifiConfig();
        },

        /**
         * @brief 对外暴露连接等待 helper，保持 UI 层不处理底层状态轮询细节。
         * @param {{maxAttempts?: number, intervalMs?: number}=} waitOptions 轮询配置。
         * @returns {Promise<object>} 轮询结果。若 SoftAP 在成功后主动关闭，会返回 `portal_closed`。
         */
        async waitForWifiConnection(waitOptions = {}) {
            return waitForWifiConnection(waitOptions);
        },

        /**
         * @brief 触发官方 prov-scan 流程，并按 status/result 完整取回扫描列表。
         * @returns {Promise<Array<{ssid: string, security: string, rssi: number}>>} 归一化后的扫描结果。
         */
        async scanWifi() {
            await initSession();

            const startResponse = decodeScanStartResponse(await transport.post(
                ENDPOINT_PROV_SCAN,
                encodeScanStartRequest({
                    blocking: true,
                    groupChannels: WIFI_SCAN_GROUP_CHANNELS,
                    periodMs: WIFI_SCAN_PERIOD_MS,
                }),
            ));
            if (startResponse.status !== 0) {
                throw new Error("wifi scan start failed");
            }

            const statusResponse = decodeScanStatusResponse(await transport.post(
                ENDPOINT_PROV_SCAN,
                encodeScanStatusRequest(),
            ));
            if (statusResponse.status !== 0) {
                throw new Error("wifi scan status failed");
            }
            if (!statusResponse.finished || statusResponse.count === 0) {
                return [];
            }

            const resultResponse = decodeScanResultResponse(await transport.post(
                ENDPOINT_PROV_SCAN,
                encodeScanResultRequest(0, statusResponse.count),
            ));
            if (resultResponse.status !== 0) {
                throw new Error("wifi scan result failed");
            }

            return normalizeWifiList(resultResponse.entries);
        },

        /**
         * @brief 写入 Wi-Fi 凭据，并立即触发 ApplyConfig。
         * @param {string} ssid Wi-Fi SSID。
         * @param {string} password Wi-Fi 密码。
         * @returns {Promise<void>} 设备接受 set + apply 后 resolve。
         */
        async sendWifiConfig(ssid, password) {
            await initSession();

            const setResponse = decodeSetWifiConfigResponse(await transport.post(
                ENDPOINT_PROV_CONFIG,
                encodeSetWifiConfigRequest(ssid, password),
            ));
            if (setResponse.status !== 0) {
                throw new Error("set wifi config failed");
            }

            await applyWifiConfig();
        },
    };
}
