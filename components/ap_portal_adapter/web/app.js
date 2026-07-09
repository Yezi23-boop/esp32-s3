import { createProvisioningClient } from "./prov_client.js";

const provClient = createProvisioningClient();

const apiDot = document.getElementById("api-dot");
const apiText = document.getElementById("api-text");
const apiNote = document.getElementById("api-note");
const scanCapability = document.getElementById("scan-capability");
const wifiList = document.getElementById("wifi-list");
const feedback = document.getElementById("feedback");
const scanBtn = document.getElementById("scan-btn");
const refreshBtn = document.getElementById("refresh-btn");
const configureBtn = document.getElementById("configure-btn");
const togglePasswordBtn = document.getElementById("toggle-password-btn");
const ssidInput = document.getElementById("ssid");
const passwordInput = document.getElementById("password");
const currentStepText = document.getElementById("current-step-text");
const scanStep = document.getElementById("scan-step");
const sendStep = document.getElementById("send-step");

const hermesToggle = document.getElementById("hermes-toggle");
const hermesForm = document.getElementById("hermes-form");
const hermesStatus = document.getElementById("hermes-status");
const hermesBaseUrl = document.getElementById("hermes-base-url");
const hermesDeviceId = document.getElementById("hermes-device-id");
const hermesDeviceToken = document.getElementById("hermes-device-token");
const hermesAllowHttp = document.getElementById("hermes-allow-http");
const hermesSaveBtn = document.getElementById("hermes-save-btn");
const toggleTokenBtn = document.getElementById("toggle-token-btn");

let selectedNetworkButton = null;

function setCurrentStep(message) {
    currentStepText.textContent = message;
}

function setFeedback(type, message) {
    feedback.className = `feedback ${type}`;
    feedback.textContent = message;
}

function clearFeedback() {
    feedback.className = "feedback hidden";
    feedback.textContent = "";
}

function setBusyState(isBusy) {
    scanBtn.disabled = isBusy;
    refreshBtn.disabled = isBusy;
    configureBtn.disabled = isBusy;
}

function setPortalCapability(isReady) {
    scanCapability.textContent = isReady ? "\u53ef\u626b\u63cf" : "\u7b49\u5f85\u63a5\u53e3";
    scanCapability.className = `capability-chip ${isReady ? "ready" : "pending"}`;
}

function setPortalLoadingState() {
    apiDot.className = "status-dot";
    apiText.textContent = "\u6b63\u5728\u68c0\u6d4b\u8bbe\u5907\u95e8\u6237...";
    apiNote.textContent = "\u8bf7\u4fdd\u6301\u624b\u673a\u6216\u7535\u8111\u8fde\u63a5\u5728\u624b\u8868\u70ed\u70b9\u4e0b\u3002";
    setCurrentStep("\u6b63\u5728\u68c0\u6d4b\u8bbe\u5907\u95e8\u6237");
    setPortalCapability(false);
    clearFeedback();
}

function setPortalReadyState(info) {
    apiDot.className = "status-dot online";
    apiText.textContent = "\u8bbe\u5907\u95e8\u6237\u5df2\u8fde\u63a5";
    apiNote.textContent = info.scanSupported ? "\u8fde\u63a5\u6b63\u5e38\uff0c\u53ef\u4ee5\u626b\u63cf\u9644\u8fd1 Wi-Fi\u3002" : "\u8fde\u63a5\u6b63\u5e38\uff0c\u53ef\u4ee5\u624b\u52a8\u8f93\u5165\u7f51\u7edc\u540d\u79f0\u3002";
    setCurrentStep(info.scanSupported ? "\u53ef\u4ee5\u5f00\u59cb\u626b\u63cf Wi-Fi" : "\u8bf7\u624b\u52a8\u8f93\u5165\u7f51\u7edc\u540d\u79f0");
    setPortalCapability(info.scanSupported);
}

function setPortalErrorState(message) {
    apiDot.className = "status-dot error";
    apiText.textContent = "\u8bbe\u5907\u95e8\u6237\u6682\u4e0d\u53ef\u7528";
    apiNote.textContent = "\u8bf7\u786e\u8ba4\u624b\u673a\u6216\u7535\u8111\u4ecd\u8fde\u63a5\u5728\u8bbe\u5907\u70ed\u70b9\u4e0b\uff0c\u7136\u540e\u5237\u65b0\u72b6\u6001\u3002";
    setCurrentStep("\u8bf7\u5148\u8fde\u63a5\u8bbe\u5907\u70ed\u70b9");
    setPortalCapability(false);
    setFeedback("error", `\u95e8\u6237\u68c0\u6d4b\u5931\u8d25\uff1a${message}`);
}

function formatSignalStrength(rssi) {
    if (rssi >= -55) {
        return "\u4fe1\u53f7\u5f3a";
    }

    if (rssi >= -70) {
        return "\u4fe1\u53f7\u4e2d";
    }

    return "\u4fe1\u53f7\u5f31";
}

function formatSecurity(security) {
    const lowerSecurity = String(security || "").toLowerCase();

    if (lowerSecurity.includes("open") || lowerSecurity.includes("none")) {
        return "\u5f00\u653e\u7f51\u7edc";
    }

    if (lowerSecurity.includes("unknown")) {
        return "\u52a0\u5bc6\u672a\u77e5";
    }

    return "\u9700\u8981\u5bc6\u7801";
}

function resetNetworkList() {
    selectedNetworkButton = null;
    wifiList.replaceChildren();
}

function renderEmptyNetworkList(title, detail) {
    resetNetworkList();
    wifiList.classList.add("empty");

    const empty = document.createElement("article");
    empty.className = "empty-state";

    const titleNode = document.createElement("strong");
    titleNode.textContent = title;

    const detailNode = document.createElement("span");
    detailNode.textContent = detail;

    empty.append(titleNode, detailNode);
    wifiList.append(empty);
}

function createNetworkButton(network) {
    const item = document.createElement("button");
    item.className = "wifi-item";
    item.type = "button";

    const textWrap = document.createElement("span");

    const name = document.createElement("strong");
    name.className = "wifi-name";
    name.textContent = network.ssid;

    const meta = document.createElement("span");
    meta.className = "wifi-meta";
    meta.textContent = `${formatSecurity(network.security)} \u00b7 ${network.rssi} dBm`;

    const signal = document.createElement("span");
    signal.className = "signal-pill";
    signal.textContent = formatSignalStrength(network.rssi);

    textWrap.append(name, meta);
    item.append(textWrap, signal);

    item.addEventListener("click", () => {
        if (selectedNetworkButton) {
            selectedNetworkButton.classList.remove("selected");
        }

        selectedNetworkButton = item;
        item.classList.add("selected");
        ssidInput.value = network.ssid;
        passwordInput.focus();
        sendStep.classList.add("active");
        setCurrentStep(`\u5df2\u9009\u62e9 ${network.ssid}`);
        setFeedback("info", `\u5df2\u9009\u62e9\u300c${network.ssid}\u300d\uff0c\u8bf7\u8f93\u5165\u5bc6\u7801\u540e\u53d1\u9001\u5230\u624b\u8868\u3002`);
    });

    return item;
}

function renderNetworkList(networks) {
    if (networks.length === 0) {
        setCurrentStep("\u6ca1\u6709\u626b\u63cf\u5230\u7f51\u7edc");
        renderEmptyNetworkList("\u6ca1\u6709\u626b\u63cf\u5230 Wi-Fi", "\u53ef\u4ee5\u9760\u8fd1\u8def\u7531\u5668\u540e\u518d\u8bd5\uff0c\u6216\u76f4\u63a5\u624b\u52a8\u8f93\u5165 SSID\u3002");
        return;
    }

    resetNetworkList();
    wifiList.classList.remove("empty");
    scanStep.classList.add("active");
    setCurrentStep("\u8bf7\u9009\u62e9\u8981\u8fde\u63a5\u7684 Wi-Fi");

    networks.forEach((network) => {
        wifiList.append(createNetworkButton(network));
    });
}

async function refreshPortalInfo() {
    setBusyState(true);
    setPortalLoadingState();

    try {
        const info = await provClient.getPortalInfo();
        setPortalReadyState(info);
    } catch (error) {
        setPortalErrorState(error.message);
    } finally {
        setBusyState(false);
    }
}

scanBtn.addEventListener("click", async () => {
    clearFeedback();
    setBusyState(true);
    setCurrentStep("\u6b63\u5728\u626b\u63cf\u9644\u8fd1 Wi-Fi");
    renderEmptyNetworkList("\u6b63\u5728\u626b\u63cf\u9644\u8fd1 Wi-Fi...", "\u626b\u63cf\u671f\u95f4\u8bf7\u4fdd\u6301\u6d4f\u89c8\u5668\u8fde\u63a5\u5728\u8bbe\u5907\u70ed\u70b9\u4e0b\u3002");

    try {
        const networks = await provClient.scanWifi();
        renderNetworkList(networks);
        setFeedback("success", `\u626b\u63cf\u5b8c\u6210\uff0c\u5171\u53d1\u73b0 ${networks.length} \u4e2a\u7f51\u7edc\u3002`);
    } catch (error) {
        setCurrentStep("\u626b\u63cf\u5931\u8d25\uff0c\u53ef\u624b\u52a8\u8f93\u5165");
        renderEmptyNetworkList("Wi-Fi \u626b\u63cf\u5931\u8d25", "\u5f53\u524d\u4ecd\u53ef\u624b\u52a8\u8f93\u5165 SSID\uff1b\u5982\u679c\u591a\u6b21\u5931\u8d25\uff0c\u8bf7\u5237\u65b0\u95e8\u6237\u72b6\u6001\u3002");
        setFeedback("error", `\u626b\u63cf\u5931\u8d25\uff1a${error.message}`);
    } finally {
        setBusyState(false);
    }
});

refreshBtn.addEventListener("click", () => {
    refreshPortalInfo();
});

togglePasswordBtn.addEventListener("click", () => {
    const shouldShowPassword = passwordInput.type === "password";
    passwordInput.type = shouldShowPassword ? "text" : "password";
    togglePasswordBtn.textContent = shouldShowPassword ? "\u9690" : "\u663e";
    togglePasswordBtn.setAttribute("aria-label", shouldShowPassword ? "\u9690\u85cf\u5bc6\u7801" : "\u663e\u793a\u5bc6\u7801");
});

configureBtn.addEventListener("click", async () => {
    clearFeedback();

    const ssid = ssidInput.value.trim();
    const password = passwordInput.value;

    if (!ssid) {
        setCurrentStep("\u8bf7\u5148\u9009\u62e9\u6216\u8f93\u5165 Wi-Fi");
        setFeedback("warning", "\u8bf7\u5148\u8f93\u5165\u6216\u9009\u62e9 Wi-Fi \u540d\u79f0\u3002");
        ssidInput.focus();
        return;
    }

    setBusyState(true);
    sendStep.classList.add("active");
    setCurrentStep("\u6b63\u5728\u53d1\u9001 Wi-Fi \u4fe1\u606f");

    try {
        await provClient.sendWifiConfig(ssid, password);
        setCurrentStep("\u6b63\u5728\u7b49\u5f85\u624b\u8868\u8fde\u63a5");
        setFeedback("info", "\u51ed\u636e\u5df2\u53d1\u9001\uff0c\u6b63\u5728\u7b49\u5f85\u8bbe\u5907\u5207\u6362\u5230\u76ee\u6807 Wi-Fi...");

        const waitResult = await provClient.waitForWifiConnection();

        if (waitResult.phase === "connected") {
            setCurrentStep("\u624b\u8868\u5df2\u8fde\u63a5 Wi-Fi");
            setFeedback("success", "\u624b\u8868\u5df2\u8fde\u63a5 Wi-Fi\uff0c\u53ef\u4ee5\u56de\u5230\u8bbe\u5907\u67e5\u770b\u8054\u7f51\u72b6\u6001\u3002");
            return;
        }

        if (waitResult.phase === "failed") {
            setCurrentStep("Wi-Fi \u8fde\u63a5\u5931\u8d25");
            setFeedback("error", "\u8bbe\u5907\u62a5\u544a Wi-Fi \u8fde\u63a5\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u5bc6\u7801\u6216\u8def\u7531\u5668\u4fe1\u53f7\u3002");
            return;
        }

        if (waitResult.phase === "portal_closed") {
            setCurrentStep("\u624b\u8868\u6b63\u5728\u5207\u6362\u7f51\u7edc");
            setFeedback("success", "\u8bbe\u5907\u5df2\u63a5\u53d7\u51ed\u636e\u5e76\u53ef\u80fd\u6b63\u5728\u5173\u95ed\u70ed\u70b9\uff1b\u8bf7\u5207\u56de\u76ee\u6807 Wi-Fi \u67e5\u770b\u624b\u8868\u72b6\u6001\u3002");
            return;
        }

        setCurrentStep("\u624b\u8868\u4ecd\u5728\u8fde\u63a5\u4e2d");
        setFeedback("warning", "\u8bbe\u5907\u4ecd\u5728\u8fde\u63a5\u4e2d\u3002\u7a0d\u7b49\u51e0\u79d2\u540e\u5237\u65b0\u72b6\u6001\uff0c\u6216\u68c0\u67e5\u8def\u7531\u5668\u662f\u5426\u5141\u8bb8\u65b0\u8bbe\u5907\u63a5\u5165\u3002");
    } catch (error) {
        setCurrentStep("\u53d1\u9001\u5931\u8d25\uff0c\u8bf7\u91cd\u8bd5");
        setFeedback("error", `\u53d1\u9001\u5931\u8d25\uff1a${error.message}`);
    } finally {
        setBusyState(false);
    }
});

renderEmptyNetworkList("\u8fd8\u6ca1\u6709\u626b\u63cf\u7ed3\u679c", "\u70b9\u51fb\u201c\u626b\u63cf Wi-Fi\u201d\u83b7\u53d6\u9644\u8fd1\u7f51\u7edc\uff0c\u4e5f\u53ef\u4ee5\u76f4\u63a5\u624b\u52a8\u8f93\u5165\u3002");

hermesToggle.addEventListener("click", () => {
    const isExpanded = hermesToggle.getAttribute("aria-expanded") === "true";
    hermesToggle.setAttribute("aria-expanded", String(!isExpanded));
    hermesForm.classList.toggle("collapsed");
    hermesForm.setAttribute("aria-hidden", String(isExpanded));
    hermesToggle.querySelector(".hermes-toggle-icon").textContent = isExpanded ? "\u25b8" : "\u25be";
});

toggleTokenBtn.addEventListener("click", () => {
    const shouldShow = hermesDeviceToken.type === "password";
    hermesDeviceToken.type = shouldShow ? "text" : "password";
    toggleTokenBtn.textContent = shouldShow ? "\u9690" : "\u663e";
    toggleTokenBtn.setAttribute("aria-label", shouldShow ? "\u9690\u85cf token" : "\u663e\u793a token");
});

function setHermesStatus(configured) {
    hermesStatus.textContent = configured ? "\u5df2\u914d\u7f6e" : "\u672a\u914d\u7f6e";
    hermesStatus.className = `hermes-status ${configured ? "configured" : ""}`;
}

function validateHermesBaseUrl(url) {
    if (!url) {
        return "Hermes \u5730\u5740\u4e0d\u80fd\u4e3a\u7a7a";
    }
    if (/\s/.test(url)) {
        return "Hermes \u5730\u5740\u4e0d\u80fd\u5305\u542b\u7a7a\u683c";
    }
    if (!url.startsWith("https://") && !url.startsWith("http://")) {
        return "Hermes \u5730\u5740\u5fc5\u987b\u4ee5 https:// \u6216 http:// \u5f00\u5934";
    }
    if (url.startsWith("http://") && !hermesAllowHttp.checked) {
        return "\u9ed8\u8ba4\u4e0d\u5141\u8bb8 HTTP\uff1b\u8bf7\u52fe\u9009\u201c\u5141\u8bb8 HTTP \u660e\u6587\u201d\u6216\u4f7f\u7528 https://";
    }
    const forbidden = ["/v1/watch/health", "/v1/watch/voice-command", "/v1/watch/request"];
    for (const path of forbidden) {
        if (url.includes(path)) {
            return `Hermes \u5730\u5740\u4e0d\u80fd\u5305\u542b ${path}`;
        }
    }
    return null;
}

async function loadHermesConfigStatus() {
    try {
        const resp = await fetch("/api/status");
        if (!resp.ok) {
            return;
        }
        const data = await resp.json();
        setHermesStatus(data.memory_watch_endpoint_configured === true);
    } catch (_) {
    }
}

hermesSaveBtn.addEventListener("click", async () => {
    clearFeedback();

    const baseUrl = hermesBaseUrl.value.trim();
    const deviceId = hermesDeviceId.value.trim();
    const deviceToken = hermesDeviceToken.value;

    const urlError = validateHermesBaseUrl(baseUrl);
    if (urlError) {
        setFeedback("warning", urlError);
        hermesBaseUrl.focus();
        return;
    }
    if (!deviceId) {
        setFeedback("warning", "\u8bbe\u5907 ID \u4e0d\u80fd\u4e3a\u7a7a\u3002");
        hermesDeviceId.focus();
        return;
    }
    if (!deviceToken) {
        setFeedback("warning", "\u8bbe\u5907 Token \u4e0d\u80fd\u4e3a\u7a7a\u3002");
        hermesDeviceToken.focus();
        return;
    }

    hermesSaveBtn.disabled = true;

    try {
        const resp = await fetch("/api/memory-watch/config", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({
                base_url: baseUrl,
                device_id: deviceId,
                device_token: deviceToken,
                timeout_ms: 120000,
                allow_http: hermesAllowHttp.checked,
            }),
        });

        if (!resp.ok) {
            const err = await resp.json().catch(() => ({}));
            throw new Error(err.error || `HTTP ${resp.status}`);
        }

        hermesDeviceToken.value = "";
        setHermesStatus(true);
        setFeedback("success", "Hermes \u914d\u7f6e\u5df2\u4fdd\u5b58\u3002\u8054\u7f51\u540e\u624b\u8868\u4f1a\u81ea\u52a8\u68c0\u6d4b\u5728\u7ebf\u72b6\u6001\u3002");
    } catch (error) {
        setFeedback("error", `\u4fdd\u5b58\u5931\u8d25\uff1a${error.message}`);
    } finally {
        hermesSaveBtn.disabled = false;
    }
});

loadHermesConfigStatus();
refreshPortalInfo();
