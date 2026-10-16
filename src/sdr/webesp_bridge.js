if (typeof Module === "undefined") var Module = {};

// Web Serial host for Kelsidavis/esp-sdr-s2-range protocol 6. ESP32-S3 on
// native USB uses framed IQS1 streaming; UART-connected boards use repeated
// receive-only CAP16 snapshots and therefore contain gaps between blocks.
Module.cellscopeEsp = (() => {
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();
  const crcTable = (() => {
    const table = new Uint32Array(256);
    for (let n = 0; n < 256; ++n) {
      let c = n;
      for (let k = 0; k < 8; ++k) c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
      table[n] = c >>> 0;
    }
    return table;
  })();
  const state = {
    port: null, reader: null, writer: null, buffer: new Uint8Array(0),
    opening: false, cancelled: false, streaming: false, continuous: false,
    centerFreq: 1960000000, actualCenter: 1960000000,
    sampleRate: 250000, gainIndex: -1, ppm: 0,
    minFreq: 100000000, maxFreq: 6000000000, maxGain: 82,
    firmware: "ESP-SDR protocol 6", transport: "UART", generation: 0,
    pumpPromise: null, operations: Promise.resolve(),
  };

  function report(ok, message, rate = 0, center = 0) {
    const ptr = stringToNewUTF8(String(message || ""));
    Module._cellscope_web_esp_prepare_result(ok ? 1 : 0, ptr, rate, center,
      state.minFreq, state.maxFreq, state.maxGain, state.continuous ? 1 : 0);
    _free(ptr);
  }

  function append(chunk) {
    if (!chunk || !chunk.byteLength) return;
    const joined = new Uint8Array(state.buffer.byteLength + chunk.byteLength);
    joined.set(state.buffer); joined.set(chunk, state.buffer.byteLength);
    state.buffer = joined;
  }

  async function fill(count) {
    while (state.buffer.byteLength < count) {
      const result = await readChunk();
      if (result.done) throw new Error("ESP-SDR serial port closed");
      append(result.value);
    }
  }

  async function readExact(count) {
    await fill(count);
    const result = state.buffer.slice(0, count);
    state.buffer = state.buffer.slice(count);
    return result;
  }

  async function readLine() {
    for (;;) {
      const newline = state.buffer.indexOf(10);
      if (newline >= 0) {
        const line = decoder.decode(state.buffer.slice(0, newline)).replace(/\r$/, "");
        state.buffer = state.buffer.slice(newline + 1);
        return line;
      }
      const result = await readChunk();
      if (result.done) throw new Error("ESP-SDR serial port closed");
      append(result.value);
    }
  }

  async function readChunk(timeoutMs = 5000) {
    let timer;
    try {
      return await Promise.race([
        state.reader.read(),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error("ESP-SDR serial response timed out")), timeoutMs);
        }),
      ]);
    } finally {
      if (timer) clearTimeout(timer);
    }
  }

  async function write(value) {
    await state.writer.write(typeof value === "string" ? encoder.encode(value) : value);
  }

  async function command(text, prefix = null) {
    await write(text + "\n");
    for (let i = 0; i < 40; ++i) {
      const line = await readLine();
      if (line.startsWith("ERR ")) throw new Error(`${text}: ${line}`);
      if (!prefix || line.startsWith(prefix)) return line;
    }
    throw new Error(`No response to ${text}`);
  }

  async function synchronize() {
    const nonce = `${Date.now()}${Math.floor(Math.random() * 1000)}`;
    state.buffer = new Uint8Array(0);
    await write(`SYNC ${nonce}\n`);
    for (let i = 0; i < 80; ++i) {
      const line = await readLine();
      if (line === `SYNC ${nonce}`) return;
    }
    throw new Error("Could not synchronize with ESP-SDR firmware");
  }

  function nearestDecimation(rate) {
    const wanted = Math.max(15625, Math.min(2000000, rate));
    const choices = [8, 32, 64, 128, 256, 512, 1024];
    return choices.reduce((best, value) =>
      Math.abs(16000000 / value - wanted) < Math.abs(16000000 / best - wanted) ? value : best);
  }

  async function applyConfiguration() {
    const corrected = state.centerFreq * (1.0 + state.ppm * 1e-6);
    const mhz = Math.max(100, Math.min(6000, Math.round(corrected / 1e6)));
    await command(`FREQ ${mhz}`, "OK");
    if (state.firmware.startsWith("S3SDR")) {
      const residualKhz = Math.round((corrected - mhz * 1e6) / 1000);
      await command(`FOFS ${residualKhz}`, "OK");
    }
    await command(state.gainIndex < 0 ? "GAIN HARDWARE" :
      `GAIN MANUAL ${Math.max(0, Math.min(state.maxGain, Math.round(state.gainIndex)))}`, "OK");
    state.actualCenter = mhz * 1e6 + (state.firmware.startsWith("S3SDR") ?
      Math.round((corrected - mhz * 1e6) / 1000) * 1000 : 0);
    if (!state.continuous) state.sampleRate = 16000000;
  }

  async function beginPrepare() {
    try {
      if (!navigator.serial) throw new Error("Web Serial is not available; use Chrome or Edge on desktop");
      if (state.opening || state.port) return;
      state.opening = true; state.cancelled = false;
      const port = await navigator.serial.requestPort({filters: [
        {usbVendorId: 0x303a}, // Espressif native USB Serial/JTAG
        {usbVendorId: 0x10c4}, // CP210x, including Heltec V3
        {usbVendorId: 0x1a86}, // CH340, common on CYD boards
      ]});
      if (state.cancelled) return;
      const usbInfo = port.getInfo ? port.getInfo() : {};
      // Board-specific conservative rates: Heltec's CP2102 is reliable at
      // 921.6 kbaud, while the tested CYD CH340 needs 115.2 kbaud. Native
      // Espressif USB retains the firmware's 2 Mbaud default.
      const baudRate = usbInfo.usbVendorId === 0x303a ? 2000000 :
        usbInfo.usbVendorId === 0x10c4 ? 921600 : 115200;
      await port.open({baudRate, bufferSize: 65536});
      state.port = port;
      state.reader = port.readable.getReader();
      state.writer = port.writable.getWriter();
      await synchronize();

      state.firmware = await command("INFO");
      if (!/^(?:ESP32|S3)SDR 6\b/.test(state.firmware))
        throw new Error(`Unsupported firmware: ${state.firmware}`);
      const transport = await command("TRANSPORT?", "TRANSPORT ");
      state.transport = transport.split(/\s+/)[1] || "UART";
      state.continuous = state.firmware.startsWith("S3SDR") && state.transport === "USB";
      const range = await command("RANGE?", "RANGE ");
      const rangeFields = range.split(/\s+/).map(Number);
      if (rangeFields.length >= 3) {
        state.minFreq = rangeFields[1] * 1e6;
        state.maxFreq = rangeFields[2] * 1e6;
      }
      const gain = await command("GAIN?", "GAIN ");
      const gainFields = gain.split(/\s+/);
      state.maxGain = Number(gainFields[4]) || 82;
      await applyConfiguration();
      state.opening = false;
      const mode = state.continuous ? "native USB IQS" : `${state.transport} snapshot RX`;
      report(true, `${state.firmware} · ${mode}`, state.sampleRate, state.actualCenter);
    } catch (e) {
      state.opening = false;
      await closePort();
      report(false, e && e.message ? e.message : String(e));
    }
  }

  function deliver(payload, bits) {
    const ptr = _malloc(payload.byteLength); HEAPU8.set(payload, ptr);
    Module._cellscope_web_esp_sample_block(ptr, payload.byteLength, bits,
      bits === 16 ? 32768 : bits === 8 ? 128 : bits === 4 ? 8 : bits === 2 ? 2 : 1);
    _free(ptr);
  }

  function crc32(parts) {
    let crc = 0xffffffff;
    for (const bytes of parts)
      for (let i = 0; i < bytes.byteLength; ++i)
        crc = crcTable[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
  }

  async function pumpContinuous(generation) {
    while (state.streaming && generation === state.generation) {
      const header = await readExact(24);
      if (!state.streaming || generation !== state.generation) break;
      if (header[0] !== 0x49 || header[1] !== 0x51 || header[2] !== 0x53 || header[3] !== 0x31)
        throw new Error("Lost synchronization with IQS1 stream");
      const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
      const samples = view.getUint16(16, true), bits = header[18];
      const payloadBytes = bits === 1 ? Math.ceil(samples / 4) :
        bits === 2 ? Math.ceil(samples / 2) :
        bits === 4 ? samples : samples * 2 * (bits / 8);
      const payloadAndCrc = await readExact(payloadBytes + 4);
      const payload = payloadAndCrc.slice(0, payloadBytes);
      const expected = new DataView(payloadAndCrc.buffer, payloadAndCrc.byteOffset + payloadBytes, 4)
        .getUint32(0, true);
      if (crc32([header, payload]) !== expected) {
        console.warn("ESP-SDR IQS1 CRC mismatch");
        continue;
      }
      if (bits === 1 || bits === 2 || bits === 4 || bits === 8 || bits === 16) deliver(payload, bits);
    }
  }

  async function pumpSnapshots(generation) {
    while (state.streaming && generation === state.generation) {
      await write("CAP16 16380 6\n");
      const line = await readLine();
      if (line.startsWith("ERR ")) throw new Error(line);
      const fields = line.split(/\s+/);
      if (fields[0] !== "DATA") continue;
      const samples = Number(fields[1]);
      if (!Number.isFinite(samples) || samples <= 0 || samples > 16380)
        throw new Error("Invalid ESP-SDR snapshot header");
      const payload = await readExact(samples * 2);
      const expected = Number.parseInt(fields[2], 16) >>> 0;
      if (crc32([payload]) !== expected) {
        console.warn("ESP-SDR snapshot CRC mismatch");
        continue;
      }
      if (state.streaming && generation === state.generation) deliver(payload, 8);
    }
  }

  async function startStreamInternal() {
    if (!state.port || state.streaming) return;
    state.streaming = true;
    const generation = ++state.generation;
    if (state.continuous) {
      const decimation = nearestDecimation(state.sampleRate);
      const bits = decimation === 8 ? 1 : decimation === 32 ? 4 : 8;
      const shift = decimation === 8 ? " 0" : "";
      const line = await command(`IQS 0 ${decimation} ${bits} 6${shift}`, "IQS ");
      state.sampleRate = Number(line.split(/\s+/)[1]) / decimation;
      state.pumpPromise = pumpContinuous(generation);
    } else {
      state.pumpPromise = pumpSnapshots(generation);
    }
    state.pumpPromise.catch(e => {
      if (state.streaming && generation === state.generation) console.error("ESP-SDR receive:", e);
    });
  }

  async function stopStreamForControl() {
    if (!state.streaming) return;
    state.streaming = false; ++state.generation;
    if (state.continuous) await write(new Uint8Array([10]));
    try { await state.pumpPromise; } catch (_) {}
    state.pumpPromise = null;
    await synchronize();
  }

  async function closePort() {
    state.streaming = false; ++state.generation;
    if (state.reader) { try { await state.reader.cancel(); } catch (_) {} try { state.reader.releaseLock(); } catch (_) {} }
    if (state.writer) { try { state.writer.releaseLock(); } catch (_) {} }
    if (state.port) { try { await state.port.close(); } catch (_) {} }
    state.port = null; state.reader = null; state.writer = null;
    state.buffer = new Uint8Array(0); state.pumpPromise = null;
  }

  async function stop() {
    state.cancelled = true;
    try { await stopStreamForControl(); } catch (_) {}
    await closePort(); state.opening = false;
  }

  function configure(cfg) { Object.assign(state, cfg); }
  function enqueue(fn) {
    state.operations = state.operations.then(fn).catch(e => console.error("ESP-SDR control:", e));
    return state.operations;
  }
  function reconfigure(values) {
    Object.assign(state, values);
    if (!state.port) return;
    return enqueue(async () => {
      const resume = state.streaming;
      if (resume) await stopStreamForControl();
      await applyConfiguration();
      if (resume) await startStreamInternal();
    });
  }

  return {
    configure, beginPrepare,
    startStream: () => enqueue(startStreamInternal), stop,
    setCenterFreq: hz => reconfigure({centerFreq: hz}),
    setSampleRate: hz => reconfigure({sampleRate: hz}),
    setGain: gainIndex => reconfigure({gainIndex}),
    setPpm: ppm => reconfigure({ppm}),
  };
})();
