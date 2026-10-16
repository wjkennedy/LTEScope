if (typeof Module === "undefined") {
  var Module = {};
}

Module.cellscopeRtl = (() => {
  const TUNERS = [
    { vendorId: 0x0bda, productId: 0x2832 },
    { vendorId: 0x0bda, productId: 0x2838 },
  ];

  class RtlCom {
    constructor(device) {
      this.device = device;
    }

    static WRITE_FLAG = 0x10;

    async claimInterface() {
      await this.device.claimInterface(0);
    }

    async releaseInterface() {
      await this.device.releaseInterface(0);
    }

    getBranding() {
      return {
        manufacturer: this.device.manufacturerName,
        model: this.device.productName,
      };
    }

    async setUsbReg(address, value, length) {
      await this._setReg(0x100, address, value, length);
    }

    async setSysReg(address, value) {
      await this._setReg(0x200, address, value, 1);
    }

    async getSysReg(address) {
      return this._getReg(0x200, address, 1);
    }

    async setDemodReg(page, addr, value, len) {
      await this._setRegBuffer(page, (addr << 8) | 0x20, this._numberToBuffer(value, len, true));
      return this._getReg(0x0a, 0x0120, 1);
    }

    async getI2CReg(addr, reg) {
      await this._setRegBuffer(0x600, addr, new Uint8Array([reg]).buffer);
      return this._getReg(0x600, addr, 1);
    }

    async setI2CReg(addr, reg, value) {
      await this._setRegBuffer(0x600, addr, new Uint8Array([reg, value]).buffer);
    }

    async getI2CRegBuffer(addr, reg, len) {
      await this._setRegBuffer(0x600, addr, new Uint8Array([reg]).buffer);
      return this._getRegBuffer(0x600, addr, len);
    }

    async setGpioOutput(gpio) {
      gpio = 1 << gpio;
      let r = await this.getSysReg(0x3004);
      await this.setSysReg(0x3004, r & ~gpio);
      r = await this.getSysReg(0x3003);
      await this.setSysReg(0x3003, r | gpio);
    }

    async setGpioBit(gpio, val) {
      gpio = 1 << gpio;
      let r = await this.getSysReg(0x3001);
      r = val ? r | gpio : r & ~gpio;
      await this.setSysReg(0x3001, r);
    }

    async getSamples(length) {
      const result = await this.device.transferIn(1, length);
      if (result.status === "ok") {
        const data = result.data;
        return data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
      }
      if (result.status === "stall") {
        await this.device.clearHalt("in", 1);
        return new ArrayBuffer(length);
      }
      throw new Error(`USB bulk read failed length=0x${length.toString(16)} status=${result.status}`);
    }

    async openI2C() {
      await this.setDemodReg(1, 1, 0x18, 1);
    }

    async closeI2C() {
      await this.setDemodReg(1, 1, 0x10, 1);
    }

    async close() {
      await this.device.close();
    }

    async _setReg(block, reg, value, length) {
      await this._writeCtrlMsg(reg, block | RtlCom.WRITE_FLAG, this._numberToBuffer(value, length));
    }

    async _getReg(block, reg, length) {
      return this._bufferToNumber(await this._readCtrlMsg(reg, block, length));
    }

    async _setRegBuffer(block, reg, buffer) {
      await this._writeCtrlMsg(reg, block | RtlCom.WRITE_FLAG, buffer);
    }

    async _getRegBuffer(block, reg, length) {
      return this._readCtrlMsg(reg, block, length);
    }

    _bufferToNumber(buffer) {
      const len = buffer.byteLength;
      const dv = new DataView(buffer);
      if (len === 0) return 0;
      if (len === 1) return dv.getUint8(0);
      if (len === 2) return dv.getUint16(0, true);
      if (len === 4) return dv.getUint32(0, true);
      throw new Error(`Cannot parse ${len}-byte number`);
    }

    _numberToBuffer(value, len, opt_bigEndian) {
      const buffer = new ArrayBuffer(len);
      const dv = new DataView(buffer);
      if (len === 1) {
        dv.setUint8(0, value);
      } else if (len === 2) {
        dv.setUint16(0, value, !opt_bigEndian);
      } else if (len === 4) {
        dv.setUint32(0, value, !opt_bigEndian);
      } else {
        throw new Error(`Cannot write ${len}-byte number`);
      }
      return buffer;
    }

    async _readCtrlMsg(value, index, length) {
      const ti = {
        requestType: "vendor",
        recipient: "device",
        request: 0,
        value,
        index,
      };
      const result = await this.device.controlTransferIn(ti, Math.max(8, length));
      if (result.status === "ok") {
        const data = result.data;
        return data.buffer.slice(data.byteOffset, data.byteOffset + length);
      }
      throw new Error(`USB read failed value=0x${value.toString(16)} index=0x${index.toString(16)} status=${result.status}`);
    }

    async _writeCtrlMsg(value, index, buffer) {
      const ti = {
        requestType: "vendor",
        recipient: "device",
        request: 0,
        value,
        index,
      };
      const result = await this.device.controlTransferOut(ti, buffer);
      if (result.status !== "ok") {
        throw new Error(`USB write failed value=0x${value.toString(16)} index=0x${index.toString(16)} status=${result.status}`);
      }
    }
  }

  const STD_MUX_CFGS = [
    [0, 0b1000, 0b00000010, 0b11011111],
    [50, 0b1000, 0b00000010, 0b10111110],
    [55, 0b1000, 0b00000010, 0b10001011],
    [60, 0b1000, 0b00000010, 0b01111011],
    [65, 0b1000, 0b00000010, 0b01101001],
    [70, 0b1000, 0b00000010, 0b01011000],
    [75, 0b0000, 0b00000010, 0b01000100],
    [90, 0b0000, 0b00000010, 0b00110100],
    [110, 0b0000, 0b00000010, 0b00100100],
    [140, 0b0000, 0b00000010, 0b00010100],
    [180, 0b0000, 0b00000010, 0b00010011],
    [250, 0b0000, 0b00000010, 0b00010001],
    [280, 0b0000, 0b00000010, 0b00000000],
    [310, 0b0000, 0b01000001, 0b00000000],
    [588, 0b0000, 0b01000000, 0b00000000],
  ];

  class R8xx {
    static XTAL_FREQ = 28800000;
    static REGISTERS = [
      0b10000011, 0b00110010, 0b01110101, 0b11000000,
      0b01000000, 0b11010110, 0b01101100, 0b11110101,
      0b01100011, 0b01110101, 0b01101000, 0b01101100,
      0b10000011, 0b10000000, 0b00001111, 0b00000000,
      0b11000000, 0b00110000, 0b01001000, 0b11001100,
      0b01100000, 0b00000000, 0b01010100, 0b10101110,
      0b01001010, 0b11000000,
    ];
    static BIT_REVS = [0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe, 0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf];
    static IF_FREQ = 3570000;

    constructor(com, i2c, muxCfgs, vcoPowerRef) {
      this.com = com;
      this.i2c = i2c;
      this.muxCfgs = muxCfgs;
      this.vcoPowerRef = vcoPowerRef;
      this.xtalFreq = R8xx.XTAL_FREQ;
      this.hasPllLock = false;
      this.shadowRegs = new Uint8Array();
    }

    static async check(com, i2c) {
      await com.openI2C();
      let found = false;
      try {
        const data = await com.getI2CReg(i2c, 0);
        found = data === 0x69;
      } catch (_e) {
      }
      await com.closeI2C();
      return found;
    }

    async setFrequency(freq) {
      await this._setMux(freq + R8xx.IF_FREQ);
      const actual = await this._setPll(freq + R8xx.IF_FREQ);
      return actual - R8xx.IF_FREQ;
    }

    async open() {
      await this.com.setDemodReg(1, 0xb1, 0b00011010, 1);
      await this.com.setDemodReg(0, 0x08, 0b01001101, 1);
      await this.com.setDemodReg(1, 0x15, 0b00000001, 1);
      await this.com.openI2C();
      this.shadowRegs = new Uint8Array(R8xx.REGISTERS);
      for (let i = 0; i < this.shadowRegs.length; ++i) {
        await this.com.setI2CReg(this.i2c, i + 5, this.shadowRegs[i]);
      }
      await this._initElectronics();
      await this.com.closeI2C();
    }

    async close() {
      await this._writeRegMask(0x06, 0b10110001, 0xff);
      await this._writeRegMask(0x05, 0b10110011, 0xff);
      await this._writeRegMask(0x07, 0b00111010, 0xff);
      await this._writeRegMask(0x08, 0b01000000, 0xff);
      await this._writeRegMask(0x09, 0b11000000, 0xff);
      await this._writeRegMask(0x0a, 0b00111010, 0xff);
      await this._writeRegMask(0x0c, 0b00110101, 0xff);
      await this._writeRegMask(0x0f, 0b01101000, 0xff);
      await this._writeRegMask(0x11, 0b00000011, 0xff);
      await this._writeRegMask(0x17, 0b11110100, 0xff);
      await this._writeRegMask(0x19, 0b00001100, 0xff);
    }

    async setAutoGain() {
      await this._writeRegMask(0x05, 0b00000000, 0b00010000);
      await this._writeRegMask(0x07, 0b00010000, 0b00010000);
      await this._writeRegMask(0x0c, 0b00001011, 0b10011111);
    }

    async setManualGain(gain) {
      let fullsteps = Math.floor(gain / 3.5);
      let halfsteps = gain - 3.5 * fullsteps >= 2.3 ? 1 : 0;
      if (fullsteps < 0) fullsteps = 0;
      if (fullsteps > 15) fullsteps = 15;
      if (fullsteps === 15) halfsteps = 0;
      const lnaValue = fullsteps + halfsteps;
      const mixerValue = fullsteps;
      await this._writeRegMask(0x05, 0b00010000, 0b00010000);
      await this._writeRegMask(0x07, 0b00000000, 0b00010000);
      await this._writeRegMask(0x0c, 0b00001000, 0b10011111);
      await this._writeRegMask(0x05, lnaValue, 0b00001111);
      await this._writeRegMask(0x07, mixerValue, 0b00001111);
    }

    setXtalFrequency(xtalFreq) {
      this.xtalFreq = xtalFreq;
    }

    getIntermediateFrequency() {
      return R8xx.IF_FREQ;
    }

    getMinimumFrequency() {
      return R8xx.XTAL_FREQ;
    }

    async _calibrateFilter() {
      let firstTry = true;
      while (true) {
        await this._writeRegMask(0x0b, 0b01100000, 0b01100000);
        await this._writeRegMask(0x0f, 0b00000100, 0b00000100);
        await this._writeRegMask(0x10, 0b00000000, 0b00000011);
        await this._setPll(56000000);
        if (!this.hasPllLock) throw new Error("PLL not locked -- cannot tune to the selected frequency.");
        await this._writeRegMask(0x0b, 0b00010000, 0b00010000);
        await this._writeRegMask(0x0b, 0b00000000, 0b00010000);
        await this._writeRegMask(0x0f, 0b00000000, 0b00000100);
        const data = await this._readRegBuffer(0x00, 5);
        const arr = new Uint8Array(data);
        let filterCap = arr[4] & 0b00001111;
        if (filterCap === 0b00001111) filterCap = 0;
        if (filterCap === 0 || !firstTry) return filterCap;
        firstTry = false;
      }
    }

    async _setMux(freq) {
      const freqMhz = freq / 1000000;
      let i;
      for (i = 0; i < this.muxCfgs.length - 1; ++i) {
        if (freqMhz < this.muxCfgs[i + 1][0]) break;
      }
      const cfg = this.muxCfgs[i];
      await this._writeRegMask(0x17, cfg[1], 0b00001000);
      await this._writeRegMask(0x1a, cfg[2], 0b11000011);
      await this._writeRegMask(0x1b, cfg[3], 0b11111111);
      await this._writeRegMask(0x10, 0b00000000, 0b00001011);
      await this._writeRegMask(0x08, 0b00000000, 0b00111111);
      await this._writeRegMask(0x09, 0b00000000, 0b00111111);
    }

    async _setPll(freq) {
      const pllRef = Math.floor(this.xtalFreq);
      await this._writeRegMask(0x10, 0b00000000, 0b00010000);
      await this._writeRegMask(0x1a, 0b00000000, 0b00001100);
      await this._writeRegMask(0x12, 0b10000000, 0b11100000);
      let divNum = Math.min(6, Math.floor(Math.log(1770000000 / freq) / Math.LN2));
      const mixDiv = 1 << (divNum + 1);
      const data = await this._readRegBuffer(0x00, 5);
      const arr = new Uint8Array(data);
      const vcoFineTune = (arr[4] & 0x30) >> 4;
      if (vcoFineTune > this.vcoPowerRef) {
        --divNum;
      } else if (vcoFineTune < this.vcoPowerRef) {
        ++divNum;
      }
      await this._writeRegMask(0x10, divNum << 5, 0b11100000);
      const vcoFreq = freq * mixDiv;
      const nint = Math.floor(vcoFreq / (2 * pllRef));
      const vcoFra = vcoFreq % (2 * pllRef);
      if (nint > 63) {
        this.hasPllLock = false;
        return 0;
      }
      const ni = Math.floor((nint - 13) / 4);
      const si = (nint - 13) % 4;
      await this._writeRegMask(0x14, ni + (si << 6), 0xff);
      await this._writeRegMask(0x12, vcoFra === 0 ? 0b1000 : 0b0000, 0b00001000);
      const sdm = Math.min(65535, Math.floor((32768 * vcoFra) / pllRef));
      await this._writeRegMask(0x16, sdm >> 8, 0xff);
      await this._writeRegMask(0x15, sdm & 0xff, 0xff);
      await this._getPllLock();
      await this._writeRegMask(0x1a, 0b00001000, 0b00001000);
      return (2 * pllRef * (nint + sdm / 65536)) / mixDiv;
    }

    async _getPllLock() {
      let firstTry = true;
      while (true) {
        const data = await this._readRegBuffer(0x00, 3);
        const arr = new Uint8Array(data);
        if (arr[2] & 0b01000000) {
          this.hasPllLock = true;
          return;
        }
        if (!firstTry) {
          this.hasPllLock = true;
          return;
        }
        await this._writeRegMask(0x12, 0b01100000, 0b11100000);
        firstTry = false;
      }
    }

    async _initElectronics() {
      await this._writeRegMask(0x0c, 0b00000000, 0b00001111);
      await this._writeRegMask(0x13, 0b00110001, 0b00111111);
      await this._writeRegMask(0x1d, 0b00000000, 0b00111000);
      const filterCap = await this._calibrateFilter();
      await this._writeRegMask(0x0a, 0b00010000 | filterCap, 0b00011111);
      await this._writeRegMask(0x0b, 0b01101011, 0b11101111);
      await this._writeRegMask(0x07, 0b00000000, 0b10000000);
      await this._writeRegMask(0x06, 0b00010000, 0b00110000);
      await this._writeRegMask(0x1e, 0b01000000, 0b01100000);
      await this._writeRegMask(0x05, 0b00000000, 0b10000000);
      await this._writeRegMask(0x1f, 0b00000000, 0b10000000);
      await this._writeRegMask(0x0f, 0b00000000, 0b10000000);
      await this._writeRegMask(0x19, 0b01100000, 0b01100000);
      await this._writeRegMask(0x1d, 0b11100101, 0b11000111);
      await this._writeRegMask(0x1c, 0b00100100, 0b11111000);
      await this._writeRegMask(0x0d, 0b01010011, 0b11111111);
      await this._writeRegMask(0x0e, 0b01110101, 0b11111111);
      await this._writeRegMask(0x05, 0b00000000, 0b01100000);
      await this._writeRegMask(0x06, 0b00000000, 0b00001000);
      await this._writeRegMask(0x11, 0b00111000, 0b00001000);
      await this._writeRegMask(0x17, 0b00110000, 0b00110000);
      await this._writeRegMask(0x0a, 0b01000000, 0b01100000);
      await this._writeRegMask(0x1d, 0b00000000, 0b00111000);
      await this._writeRegMask(0x1c, 0b00000000, 0b00000100);
      await this._writeRegMask(0x06, 0b00000000, 0b01000000);
      await this._writeRegMask(0x1a, 0b00110000, 0b00110000);
      await this._writeRegMask(0x1d, 0b00011000, 0b00111000);
      await this._writeRegMask(0x1c, 0b00100100, 0b00000100);
      await this._writeRegMask(0x1e, 0b00001101, 0b00011111);
      await this._writeRegMask(0x1a, 0b00100000, 0b00110000);
    }

    async _readRegBuffer(addr, length) {
      const data = await this.com.getI2CRegBuffer(this.i2c, addr, length);
      const buf = new Uint8Array(data);
      for (let i = 0; i < buf.length; ++i) {
        const b = buf[i];
        buf[i] = (R8xx.BIT_REVS[b & 0xf] << 4) | R8xx.BIT_REVS[b >> 4];
      }
      return buf.buffer;
    }

    async _writeRegMask(addr, value, mask) {
      const rc = this.shadowRegs[addr - 5] || 0;
      const val = (rc & ~mask) | (value & mask);
      this.shadowRegs[addr - 5] = val;
      await this.com.setI2CReg(this.i2c, addr, val);
    }
  }

  class R820T extends R8xx {
    static async maybeInit(com) {
      const found = await R8xx.check(com, 0x34);
      if (!found) return null;
      const tuner = new R820T(com);
      await tuner.open();
      return tuner;
    }
    constructor(com) {
      super(com, 0x34, STD_MUX_CFGS, 2);
    }
  }

  class R828D extends R8xx {
    constructor(com, isBlogV4) {
      super(com, 0x74, isBlogV4 ? MUX_CFGS_RTLSDRBLOGV4 : STD_MUX_CFGS, 1);
      this.input = 0;
      this.isRtlSdrBlogV4 = isBlogV4;
    }

    static async maybeInit(com) {
      const found = await R8xx.check(com, 0x74);
      if (!found) return null;
      const { manufacturer, model } = com.getBranding();
      const isRtlSdrBlogV4 = manufacturer === "RTLSDRBlog" && model === "Blog V4";
      const tuner = new R828D(com, isRtlSdrBlogV4);
      await tuner.open();
      return tuner;
    }

    async setFrequency(freq) {
      let upconvert = 0;
      if (this.isRtlSdrBlogV4 && freq < 28800000) upconvert = 28800000;
      const actual = await super.setFrequency(freq + upconvert);
      if (this.isRtlSdrBlogV4) {
        const input = freq <= 28800000 ? 2 : freq < 250000000 ? 1 : 0;
        if (this.input !== input) {
          this.input = input;
          if (input === 0) {
            await this._writeRegMask(0x06, 0x00, 0x08);
            await this._writeRegMask(0x05, 0x00, 0x60);
          } else if (input === 1) {
            await this._writeRegMask(0x06, 0x00, 0x08);
            await this._writeRegMask(0x05, 0x60, 0x60);
          } else {
            await this._writeRegMask(0x06, 0x08, 0x08);
            await this._writeRegMask(0x05, 0x20, 0x60);
          }
          await this.com.setGpioOutput(5);
          await this.com.setGpioBit(5, input === 2 ? 0 : 1);
        }
      } else {
        const input = freq > 345000000 ? 0 : 1;
        if (this.input !== input) {
          this.input = input;
          await this._writeRegMask(0x05, input === 0 ? 0x00 : 0x60, 0x60);
        }
      }
      return actual - upconvert;
    }

    getMinimumFrequency() {
      return this.isRtlSdrBlogV4 ? 0 : super.getMinimumFrequency();
    }
  }

  const MUX_CFGS_RTLSDRBLOGV4 = [
    [0, 0b0000, 0b00000010, 0b11011111],
    [2.2, 0b1000, 0b00000010, 0b11011111],
    [50, 0b1000, 0b00000010, 0b10111110],
    [55, 0b1000, 0b00000010, 0b10001011],
    [60, 0b1000, 0b00000010, 0b01111011],
    [65, 0b1000, 0b00000010, 0b01101001],
    [70, 0b1000, 0b00000010, 0b01011000],
    [75, 0b1000, 0b00000010, 0b01000100],
    [85, 0b0000, 0b00000010, 0b01000100],
    [90, 0b0000, 0b00000010, 0b00110100],
    [110, 0b0000, 0b00000010, 0b00100100],
    [112, 0b1000, 0b00000010, 0b00100100],
    [140, 0b1000, 0b00000010, 0b00010100],
    [172, 0b0000, 0b00000010, 0b00010100],
    [180, 0b0000, 0b00000010, 0b00010011],
    [242, 0b1000, 0b00000010, 0b00010011],
    [250, 0b1000, 0b00000010, 0b00010001],
    [280, 0b1000, 0b00000010, 0b00000000],
    [310, 0b1000, 0b01000001, 0b00000000],
    [588, 0b1000, 0b01000000, 0b00000000],
  ];

  class RTL2832U {
    static XTAL_FREQ = 28800000;
    static BYTES_PER_SAMPLE = 2;

    constructor(device) {
      this.centerFrequency = 0;
      this.ppm = 0;
      this.gain = null;
      this.directSamplingMethod = 0;
      this.directSampling = 0;
      this.biasTeeEnabled = false;
      this.com = new RtlCom(device);
    }

    static async open(device) {
      const com = new RtlCom(device);
      await com.claimInterface();
      await RTL2832U._init(com);
      const tuner = await RTL2832U._findTuner(com);
      const rtl = new RTL2832U(device);
      rtl.com = com;
      rtl.tuner = tuner;
      await rtl.setGain(rtl.gain);
      await rtl.setFrequencyCorrection(rtl.ppm);
      return rtl;
    }

    static async _init(com) {
      await com.setUsbReg(0x2000, 0b00001001, 1);
      await com.setUsbReg(0x2158, 0x0200, 2);
      await com.setUsbReg(0x2148, 0b0000001000010000, 2);
      await com.setSysReg(0x300b, 0b00100010);
      await com.setSysReg(0x3000, 0b11101000);
      await com.setDemodReg(1, 0x01, 0b00010100, 1);
      await com.setDemodReg(1, 0x01, 0b00010000, 1);
      await com.setDemodReg(1, 0x15, 0b00000000, 1);
      await com.setDemodReg(1, 0x16, 0x00, 1);
      await com.setDemodReg(1, 0x17, 0x00, 1);
      await com.setDemodReg(1, 0x18, 0x00, 1);
      await com.setDemodReg(1, 0x19, 0x00, 1);
      await com.setDemodReg(1, 0x1a, 0x00, 1);
      await com.setDemodReg(1, 0x1b, 0x00, 1);
      await com.setDemodReg(1, 0x1c, 0xca, 1);
      await com.setDemodReg(1, 0x1d, 0xdc, 1);
      await com.setDemodReg(1, 0x1e, 0xd7, 1);
      await com.setDemodReg(1, 0x1f, 0xd8, 1);
      await com.setDemodReg(1, 0x20, 0xe0, 1);
      await com.setDemodReg(1, 0x21, 0xf2, 1);
      await com.setDemodReg(1, 0x22, 0x0e, 1);
      await com.setDemodReg(1, 0x23, 0x35, 1);
      await com.setDemodReg(1, 0x24, 0x06, 1);
      await com.setDemodReg(1, 0x25, 0x50, 1);
      await com.setDemodReg(1, 0x26, 0x9c, 1);
      await com.setDemodReg(1, 0x27, 0x0d, 1);
      await com.setDemodReg(1, 0x28, 0x71, 1);
      await com.setDemodReg(1, 0x29, 0x11, 1);
      await com.setDemodReg(1, 0x2a, 0x14, 1);
      await com.setDemodReg(1, 0x2b, 0x71, 1);
      await com.setDemodReg(1, 0x2c, 0x74, 1);
      await com.setDemodReg(1, 0x2d, 0x19, 1);
      await com.setDemodReg(1, 0x2e, 0x41, 1);
      await com.setDemodReg(1, 0x2f, 0xa5, 1);
      await com.setDemodReg(0, 0x19, 0b00000101, 1);
      await com.setDemodReg(1, 0x93, 0b11110000, 1);
      await com.setDemodReg(1, 0x94, 0b00001111, 1);
      await com.setDemodReg(1, 0x11, 0b00000000, 1);
      await com.setDemodReg(1, 0x04, 0b00000000, 1);
      await com.setDemodReg(0, 0x61, 0b01100000, 1);
      await com.setDemodReg(0, 0x06, 0b10000000, 1);
      await com.setDemodReg(1, 0xb1, 0b00011011, 1);
      await com.setDemodReg(0, 0x0d, 0b10000011, 1);
    }

    static async _findTuner(com) {
      let tuner = await R820T.maybeInit(com);
      if (tuner === null) tuner = await R828D.maybeInit(com);
      if (tuner === null) {
        await com.releaseInterface();
        throw new Error("Unsupported RTL tuner chip");
      }
      return tuner;
    }

    async setSampleRate(rate) {
      const ratio = Math.floor((this._getXtalFrequency() * (1 << 22)) / rate) & 0x0ffffffc;
      const realRate = Math.floor((this._getXtalFrequency() * (1 << 22)) / ratio);
      await this.com.setDemodReg(1, 0x9f, (ratio >> 16) & 0xffff, 2);
      await this.com.setDemodReg(1, 0xa1, ratio & 0xffff, 2);
      await this._resetDemodulator();
      return realRate;
    }

    async setFrequencyCorrection(ppm) {
      this.ppm = ppm;
      const ppmOffset = -1 * Math.floor((this.ppm * (1 << 24)) / 1000000);
      await this.com.setDemodReg(1, 0x3e, (ppmOffset >> 8) & 0x3f, 1);
      await this.com.setDemodReg(1, 0x3f, ppmOffset & 0xff, 1);
      const xtalFrequency = this._getXtalFrequency();
      this.tuner.setXtalFrequency(xtalFrequency);
      const ifFreq = this.tuner.getIntermediateFrequency();
      if (ifFreq !== 0) await this._setIfFrequency(ifFreq);
      if (this.centerFrequency !== 0) await this.setCenterFrequency(this.centerFrequency);
    }

    async _setIfFrequency(ifFreq) {
      const xtalFrequency = this._getXtalFrequency();
      const multiplier = -1 * Math.floor((ifFreq * (1 << 22)) / xtalFrequency);
      await this.com.setDemodReg(1, 0x19, (multiplier >> 16) & 0x3f, 1);
      await this.com.setDemodReg(1, 0x1a, (multiplier >> 8) & 0xff, 1);
      await this.com.setDemodReg(1, 0x1b, multiplier & 0xff, 1);
      return Math.floor((-1 * multiplier * xtalFrequency) / (1 << 22));
    }

    async setGain(gain) {
      this.gain = gain;
      await this.com.openI2C();
      if (this.directSampling) {
        await this._enableRtlAgc(gain === null);
      } else if (this.gain === null) {
        await this.tuner.setAutoGain();
      } else {
        await this.tuner.setManualGain(this.gain);
      }
      await this.com.closeI2C();
    }

    async enableBiasTee(enabled) {
      this.biasTeeEnabled = enabled;
      await this.com.setGpioOutput(0);
      await this.com.setGpioBit(0, enabled ? 1 : 0);
    }

    async setCenterFrequency(freq) {
      await this._maybeSetDirectSampling(freq);
      let actualFreq;
      if (this.directSampling) {
        actualFreq = await this._setIfFrequency(freq);
      } else {
        await this.com.openI2C();
        actualFreq = await this.tuner.setFrequency(freq);
        await this.com.closeI2C();
      }
      this.centerFrequency = freq;
      return actualFreq;
    }

    async setDirectSamplingMethod(method) {
      if (this.directSamplingMethod === method) return;
      this.directSamplingMethod = method;
      if (this.centerFrequency !== 0) await this.setCenterFrequency(this.centerFrequency);
    }

    async _maybeSetDirectSampling(frequency) {
      const lowFrequency = frequency < this.tuner.getMinimumFrequency();
      const method = lowFrequency ? this.directSamplingMethod : 0;
      if (this.directSampling === method) return;
      const tunerWasOn = this.directSampling === 0;
      const useDirectSampling = method !== 0;
      this.directSampling = method;
      if (useDirectSampling) {
        if (tunerWasOn) {
          await this.com.openI2C();
          await this.tuner.close();
          await this.com.closeI2C();
        }
        await this.com.setDemodReg(1, 0xb1, 0b00011010, 1);
        await this.com.setDemodReg(1, 0x15, 0b00000000, 1);
        const regValue = method === 1 ? 0b10000000 : 0b10010000;
        await this.com.setDemodReg(0, 0x06, regValue, 1);
        await this._enableRtlAgc(true);
      } else {
        await this.com.openI2C();
        await this.tuner.open();
        await this.com.closeI2C();
        const ifFreq = this.tuner.getIntermediateFrequency();
        if (ifFreq !== 0) await this._setIfFrequency(ifFreq);
        await this.com.setDemodReg(1, 0x15, 0b00000001, 1);
        await this.com.setDemodReg(0, 0x06, 0b10000000, 1);
        await this._enableRtlAgc(false);
        await this.setGain(this.getGain());
      }
    }

    async resetBuffer() {
      await this.com.setUsbReg(0x2148, 0b0000001000010000, 2);
      await this.com.setUsbReg(0x2148, 0x0000, 2);
    }

    async readSamples(length) {
      const data = await this.com.getSamples(length * RTL2832U.BYTES_PER_SAMPLE);
      return {
        frequency: this.centerFrequency,
        data,
        directSampling: this.directSampling !== 0,
      };
    }

    async close() {
      await this.com.openI2C();
      await this.tuner.close();
      await this.com.closeI2C();
      await this.com.releaseInterface();
      await this.com.close();
    }

    _getXtalFrequency() {
      return Math.floor(RTL2832U.XTAL_FREQ * (1 + this.ppm / 1000000));
    }

    async _resetDemodulator() {
      await this.com.setDemodReg(1, 0x01, 0b00010100, 1);
      await this.com.setDemodReg(1, 0x01, 0b00010000, 1);
    }

    async _enableRtlAgc(enable) {
      await this.com.setDemodReg(0, 0x19, enable ? 0x25 : 0x05, 1);
    }
  }

  const state = {
    device: null,
    rtl: null,
    opening: false,
    openingCancelled: false,
    running: false,
    centerFreq: 1545000000,
    sampleRate: 2400000,
    gainDb: -1,
    ppm: 0,
    biasTee: false,
    directSamplingMethod: 0,
    blockSamples: 16384,
  };

  function failPrepare(message) {
    const msg = String(message || "WebUSB RTL-SDR prepare failed");
    const ptr = stringToNewUTF8(msg);
    Module._cellscope_web_rtl_prepare_result(0, ptr);
    _free(ptr);
  }

  function succeedPrepare() {
    Module._cellscope_web_rtl_prepare_result(1, 0);
  }

  function deliverSamples(samples) {
    if (!state.running || !samples) return;
    const u8 = new Uint8Array(samples.data);
    const ptr = _malloc(u8.byteLength);
    HEAPU8.set(u8, ptr);
    Module._cellscope_web_rtl_sample_block(ptr, u8.byteLength, samples.frequency, samples.directSampling ? 1 : 0);
    _free(ptr);
  }

  async function beginPrepare() {
    try {
      if (!navigator.usb) throw new Error("WebUSB is not available in this browser");
      if (state.opening || state.rtl) return;
      state.opening = true;
      state.openingCancelled = false;
      const device = await navigator.usb.requestDevice({ filters: TUNERS });
      if (state.openingCancelled) {
        try { await device.close(); } catch (_e) {}
        state.opening = false;
        return;
      }
      await device.open();
      const rtl = await RTL2832U.open(device);
      state.device = device;
      state.rtl = rtl;
      state.opening = false;
      await rtl.setSampleRate(state.sampleRate);
      await rtl.setFrequencyCorrection(state.ppm);
      await rtl.setGain(state.gainDb < 0 ? null : state.gainDb);
      await rtl.enableBiasTee(state.biasTee);
      await rtl.setCenterFrequency(state.centerFreq);
      await rtl.resetBuffer();
      succeedPrepare();
    } catch (e) {
      state.opening = false;
      state.device = null;
      state.rtl = null;
      failPrepare(e && e.message ? e.message : String(e));
    }
  }

  async function startStream() {
    if (!state.rtl) throw new Error("RTL-SDR device is not prepared");
    if (state.running) return;
    state.running = true;
    const pump = async () => {
      if (!state.running || !state.rtl) return;
      try {
        const samples = await state.rtl.readSamples(state.blockSamples);
        deliverSamples(samples);
      } catch (e) {
        if (state.running) {
          state.running = false;
          const ptr = stringToNewUTF8(String(e && e.message ? e.message : e));
          Module._cellscope_web_rtl_prepare_result(0, ptr);
          _free(ptr);
        }
        return;
      }
      if (state.running) {
        pump();
        pump();
      }
    };
    pump();
    pump();
  }

  async function stop() {
    state.running = false;
    state.openingCancelled = true;
    if (state.rtl) {
      try {
        await state.rtl.close();
      } catch (_e) {}
      state.rtl = null;
    }
    state.device = null;
    state.opening = false;
  }

  function configure(cfg) {
    if (cfg.centerFreq !== undefined) state.centerFreq = cfg.centerFreq;
    if (cfg.sampleRate !== undefined) state.sampleRate = cfg.sampleRate;
    if (cfg.gainDb !== undefined) state.gainDb = cfg.gainDb;
    if (cfg.ppm !== undefined) state.ppm = cfg.ppm;
    if (cfg.biasTee !== undefined) state.biasTee = !!cfg.biasTee;
    if (cfg.directSamplingMethod !== undefined) state.directSamplingMethod = cfg.directSamplingMethod;
  }

  async function setCenterFreq(hz) {
    state.centerFreq = hz;
    if (state.rtl) await state.rtl.setCenterFrequency(hz);
  }

  async function setSampleRate(hz) {
    state.sampleRate = hz;
    if (state.rtl) await state.rtl.setSampleRate(hz);
  }

  async function setGain(db) {
    state.gainDb = db;
    if (state.rtl) await state.rtl.setGain(db < 0 ? null : db);
  }

  async function setBiasTee(on) {
    state.biasTee = !!on;
    if (state.rtl) await state.rtl.enableBiasTee(state.biasTee);
  }

  async function setPpm(ppm) {
    state.ppm = ppm;
    if (state.rtl) await state.rtl.setFrequencyCorrection(ppm);
  }

  async function setDirectSampling(method) {
    state.directSamplingMethod = method;
    if (state.rtl) await state.rtl.setDirectSamplingMethod(method);
  }

  return {
    configure,
    beginPrepare,
    startStream,
    stop,
    setCenterFreq,
    setSampleRate,
    setGain,
    setBiasTee,
    setPpm,
    setDirectSampling,
  };
})();
