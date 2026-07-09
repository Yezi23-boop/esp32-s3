const TEXT_ENCODER = new TextEncoder();
const TEXT_DECODER = new TextDecoder();

const WIRE_VARINT = 0;
const WIRE_LEN = 2;

const SCAN_AUTH_MODE_NAMES = Object.freeze([
    "Open",
    "WEP",
    "WPA_PSK",
    "WPA2_PSK",
    "WPA_WPA2_PSK",
    "WPA2_ENTERPRISE",
    "WPA3_PSK",
    "WPA2_WPA3_PSK",
]);

const WIFI_STATE_NAMES = Object.freeze([
    "connected",
    "connecting",
    "disconnected",
    "failed",
]);

const WIFI_FAIL_REASON_NAMES = Object.freeze([
    "auth_error",
    "network_not_found",
]);

function ensureUint8Array(value) {
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

    throw new TypeError("expected Uint8Array, ArrayBuffer, TypedArray, or string");
}

function concatBytes(parts) {
    const normalizedParts = parts.filter((part) => part.length > 0);
    const totalLength = normalizedParts.reduce((sum, part) => sum + part.length, 0);
    const merged = new Uint8Array(totalLength);
    let offset = 0;

    for (const part of normalizedParts) {
        merged.set(part, offset);
        offset += part.length;
    }

    return merged;
}

function encodeVarint(value) {
    let remaining = BigInt(value);

    if (remaining < 0n) {
        throw new RangeError("varint encoder only supports non-negative values");
    }

    const bytes = [];
    do {
        let nextByte = Number(remaining & 0x7fn);
        remaining >>= 7n;
        if (remaining !== 0n) {
            nextByte |= 0x80;
        }
        bytes.push(nextByte);
    } while (remaining !== 0n);

    return Uint8Array.from(bytes);
}

function decodeVarint(buffer, startOffset) {
    let value = 0n;
    let shift = 0n;
    let offset = startOffset;

    while (offset < buffer.length) {
        const byte = BigInt(buffer[offset]);
        value |= (byte & 0x7fn) << shift;
        offset += 1;

        if ((byte & 0x80n) === 0n) {
            return { value, nextOffset: offset };
        }

        shift += 7n;
        if (shift > 63n) {
            throw new Error("protobuf varint is too large");
        }
    }

    throw new Error("truncated protobuf varint");
}

function encodeFieldHeader(fieldNumber, wireType) {
    return encodeVarint((BigInt(fieldNumber) << 3n) | BigInt(wireType));
}

function encodeVarintField(fieldNumber, value, options = {}) {
    const normalizedValue = BigInt(value);

    if (!options.emitDefault && normalizedValue === 0n) {
        return new Uint8Array(0);
    }

    return concatBytes([
        encodeFieldHeader(fieldNumber, WIRE_VARINT),
        encodeVarint(normalizedValue),
    ]);
}

function encodeBoolField(fieldNumber, value) {
    return encodeVarintField(fieldNumber, value ? 1 : 0);
}

function encodeBytesField(fieldNumber, value, options = {}) {
    const bytes = ensureUint8Array(value);

    if (!options.emitDefault && bytes.length === 0) {
        return new Uint8Array(0);
    }

    return concatBytes([
        encodeFieldHeader(fieldNumber, WIRE_LEN),
        encodeVarint(bytes.length),
        bytes,
    ]);
}

function encodeMessageField(fieldNumber, value, options = {}) {
    return encodeBytesField(fieldNumber, value, options);
}

function decodeFields(bufferLike) {
    const buffer = ensureUint8Array(bufferLike);
    const fields = [];
    let offset = 0;

    while (offset < buffer.length) {
        const header = decodeVarint(buffer, offset);
        const fieldNumber = Number(header.value >> 3n);
        const wireType = Number(header.value & 0x07n);
        offset = header.nextOffset;

        if (wireType === WIRE_VARINT) {
            const value = decodeVarint(buffer, offset);
            fields.push({ fieldNumber, wireType, value: value.value });
            offset = value.nextOffset;
            continue;
        }

        if (wireType === WIRE_LEN) {
            const lengthField = decodeVarint(buffer, offset);
            const length = Number(lengthField.value);
            const valueStart = lengthField.nextOffset;
            const valueEnd = valueStart + length;

            if (valueEnd > buffer.length) {
                throw new Error("truncated protobuf length-delimited field");
            }

            fields.push({
                fieldNumber,
                wireType,
                value: buffer.slice(valueStart, valueEnd),
            });
            offset = valueEnd;
            continue;
        }

        if (wireType === 1) {
            offset += 8;
            continue;
        }

        if (wireType === 5) {
            offset += 4;
            continue;
        }

        throw new Error(`unsupported protobuf wire type: ${wireType}`);
    }

    return fields;
}

function getVarintField(fields, fieldNumber, defaultValue = 0) {
    const field = fields.find((entry) => entry.fieldNumber === fieldNumber && entry.wireType === WIRE_VARINT);
    return field ? Number(field.value) : defaultValue;
}

function getBoolField(fields, fieldNumber, defaultValue = false) {
    return getVarintField(fields, fieldNumber, defaultValue ? 1 : 0) !== 0;
}

function getBytesField(fields, fieldNumber) {
    const field = fields.find((entry) => entry.fieldNumber === fieldNumber && entry.wireType === WIRE_LEN);
    return field ? field.value : new Uint8Array(0);
}

function getRepeatedBytesFields(fields, fieldNumber) {
    return fields
        .filter((entry) => entry.fieldNumber === fieldNumber && entry.wireType === WIRE_LEN)
        .map((entry) => entry.value);
}

function decodeUtf8(value) {
    return TEXT_DECODER.decode(ensureUint8Array(value)).replace(/\u0000+$/u, "");
}

function decodeSignedInt32(value) {
    return Number(BigInt.asIntN(32, value));
}

function bytesToHex(value) {
    return Array.from(ensureUint8Array(value), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

function decodeScanResultEntry(buffer) {
    const fields = decodeFields(buffer);
    const auth = getVarintField(fields, 5, 0);

    return {
        ssid: decodeUtf8(getBytesField(fields, 1)),
        channel: getVarintField(fields, 2, 0),
        rssi: (() => {
            const field = fields.find((entry) => entry.fieldNumber === 3 && entry.wireType === WIRE_VARINT);
            return field ? decodeSignedInt32(field.value) : 0;
        })(),
        bssid: bytesToHex(getBytesField(fields, 4)),
        auth,
        security: SCAN_AUTH_MODE_NAMES[auth] ?? `UNKNOWN_${auth}`,
    };
}

function decodeWifiConnectedState(buffer) {
    const fields = decodeFields(buffer);
    const authMode = getVarintField(fields, 2, 0);

    return {
        ip4Address: decodeUtf8(getBytesField(fields, 1)),
        authMode,
        authModeName: SCAN_AUTH_MODE_NAMES[authMode] ?? `UNKNOWN_${authMode}`,
        ssid: decodeUtf8(getBytesField(fields, 3)),
        bssid: bytesToHex(getBytesField(fields, 4)),
        channel: getVarintField(fields, 5, 0),
    };
}

function decodeWifiAttemptFailed(buffer) {
    const fields = decodeFields(buffer);
    return {
        attemptsRemaining: getVarintField(fields, 1, 0),
    };
}

function decodeSessionEnvelope(buffer) {
    const fields = decodeFields(buffer);
    const sec0Fields = decodeFields(getBytesField(fields, 10));
    const sessionResponseFields = decodeFields(getBytesField(sec0Fields, 21));
    const status = getVarintField(sessionResponseFields, 1, 0);

    return {
        secVer: getVarintField(fields, 2, 0),
        msg: getVarintField(sec0Fields, 1, 0),
        status,
        ok: status === 0,
    };
}

function decodeScanEnvelope(buffer) {
    const fields = decodeFields(buffer);

    return {
        msg: getVarintField(fields, 1, 0),
        status: getVarintField(fields, 2, 0),
        payloadFields: fields,
    };
}

function decodeConfigEnvelope(buffer) {
    const fields = decodeFields(buffer);

    return {
        msg: getVarintField(fields, 1, 0),
        payloadFields: fields,
    };
}

export function encodeSessionSetup0Request() {
    const sec0Payload = encodeMessageField(
        20,
        new Uint8Array(0),
        { emitDefault: true },
    );

    return encodeMessageField(10, sec0Payload);
}

export function decodeSessionSetup0Response(buffer) {
    return decodeSessionEnvelope(buffer);
}

export function encodeScanStartRequest(options = {}) {
    const {
        blocking = true,
        passive = false,
        groupChannels = 5,
        periodMs = 120,
    } = options;

    const startCommand = concatBytes([
        encodeBoolField(1, blocking),
        encodeBoolField(2, passive),
        encodeVarintField(3, groupChannels),
        encodeVarintField(4, periodMs),
    ]);

    return encodeMessageField(10, startCommand, { emitDefault: true });
}

export function encodeScanStatusRequest() {
    return concatBytes([
        encodeVarintField(1, 2, { emitDefault: true }),
        encodeMessageField(12, new Uint8Array(0), { emitDefault: true }),
    ]);
}

export function encodeScanResultRequest(startIndex = 0, count = 0) {
    const resultCommand = concatBytes([
        encodeVarintField(1, startIndex),
        encodeVarintField(2, count),
    ]);

    return concatBytes([
        encodeVarintField(1, 4, { emitDefault: true }),
        encodeMessageField(14, resultCommand, { emitDefault: true }),
    ]);
}

export function decodeScanStartResponse(buffer) {
    return decodeScanEnvelope(buffer);
}

export function decodeScanStatusResponse(buffer) {
    const envelope = decodeScanEnvelope(buffer);
    const statusFields = decodeFields(getBytesField(envelope.payloadFields, 13));

    return {
        msg: envelope.msg,
        status: envelope.status,
        finished: getBoolField(statusFields, 1, false),
        count: getVarintField(statusFields, 2, 0),
    };
}

export function decodeScanResultResponse(buffer) {
    const envelope = decodeScanEnvelope(buffer);
    const resultFields = decodeFields(getBytesField(envelope.payloadFields, 15));
    const entries = getRepeatedBytesFields(resultFields, 1).map((entry) => decodeScanResultEntry(entry));

    return {
        msg: envelope.msg,
        status: envelope.status,
        entries,
    };
}

export function encodeGetWifiStatusRequest() {
    return encodeMessageField(10, new Uint8Array(0), { emitDefault: true });
}

export function encodeSetWifiConfigRequest(ssid, passphrase, options = {}) {
    const {
        bssid = new Uint8Array(0),
        channel = 0,
    } = options;

    const configCommand = concatBytes([
        encodeBytesField(1, ssid),
        encodeBytesField(2, passphrase),
        encodeBytesField(3, bssid),
        encodeVarintField(4, channel),
    ]);

    return concatBytes([
        encodeVarintField(1, 2, { emitDefault: true }),
        encodeMessageField(12, configCommand, { emitDefault: true }),
    ]);
}

export function encodeApplyWifiConfigRequest() {
    return concatBytes([
        encodeVarintField(1, 4, { emitDefault: true }),
        encodeMessageField(14, new Uint8Array(0), { emitDefault: true }),
    ]);
}

export function decodeSetWifiConfigResponse(buffer) {
    const envelope = decodeConfigEnvelope(buffer);
    const responseFields = decodeFields(getBytesField(envelope.payloadFields, 13));

    return {
        msg: envelope.msg,
        status: getVarintField(responseFields, 1, 0),
    };
}

export function decodeApplyWifiConfigResponse(buffer) {
    const envelope = decodeConfigEnvelope(buffer);
    const responseFields = decodeFields(getBytesField(envelope.payloadFields, 15));

    return {
        msg: envelope.msg,
        status: getVarintField(responseFields, 1, 0),
    };
}

export function decodeGetWifiStatusResponse(buffer) {
    const envelope = decodeConfigEnvelope(buffer);
    const statusFields = decodeFields(getBytesField(envelope.payloadFields, 11));
    const status = getVarintField(statusFields, 1, 0);
    const wifiStationState = getVarintField(statusFields, 2, 0);
    const failureReason = getVarintField(statusFields, 10, -1);
    const connectedState = getBytesField(statusFields, 11);
    const attemptFailed = getBytesField(statusFields, 12);

    return {
        msg: envelope.msg,
        status,
        wifiStationState,
        wifiState: WIFI_STATE_NAMES[wifiStationState] ?? `unknown_${wifiStationState}`,
        failureReason,
        failureReasonName: failureReason >= 0
            ? (WIFI_FAIL_REASON_NAMES[failureReason] ?? `unknown_${failureReason}`)
            : null,
        connected: connectedState.length > 0 ? decodeWifiConnectedState(connectedState) : null,
        attemptFailed: attemptFailed.length > 0 ? decodeWifiAttemptFailed(attemptFailed) : null,
    };
}

export const PROV_PROTO_BUNDLE = Object.freeze({
    version: "official-minimal",
    encodeSessionSetup0Request,
    decodeSessionSetup0Response,
    encodeScanStartRequest,
    encodeScanStatusRequest,
    encodeScanResultRequest,
    decodeScanStartResponse,
    decodeScanStatusResponse,
    decodeScanResultResponse,
    encodeGetWifiStatusRequest,
    encodeSetWifiConfigRequest,
    encodeApplyWifiConfigRequest,
    decodeSetWifiConfigResponse,
    decodeApplyWifiConfigResponse,
    decodeGetWifiStatusResponse,
});
