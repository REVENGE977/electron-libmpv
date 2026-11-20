"use strict";

const path = require("path");
const binaryPath = path.join(__dirname, "build", "Release", "electron-libmpv.node");
const { MpvPlayer } = require(binaryPath);

class Mpv {
    constructor(options = {}) {
        this.player = new MpvPlayer({
            libmpvPath: options.libmpvPath || "",
            configPath: options.configPath || "",
            width: options.width || 1280,
            height: options.height || 720,
            onEvent: options.onEvent || (() => {}),
            onFrame: options.onFrame || (() => {}),
        });
    }

    initialize(options = {}) {
        if (!this.player) return { ok: false, error: "Player destroyed" };
        return this.player.initialize({
            width: Math.round(options.width || 1280),
            height: Math.round(options.height || 720),
            hdr: Boolean(options.hdr),
            preferGpu: options.preferGpu !== false,
        });
    }

    resize(width, height, hdr) {
        if (!this.player) return;
        if (typeof hdr === "boolean") {
            this.player.resize(Math.round(width), Math.round(height), hdr);
        } else {
            this.player.resize(Math.round(width), Math.round(height));
        }
    }

    releaseFrame(slotId, generation) {
        if (!this.player) return;
        if (typeof generation === "number") {
            this.player.releaseFrame(slotId, generation);
        } else {
            this.player.releaseFrame(slotId);
        }
    }

    enableSoftwareFallback() {
        return this.player ? this.player.enableSoftwareFallback() : false;
    }

    command(...args) {
        return this.player ? this.player.command(...args.map(a => String(a))) : -1;
    }

    property(name, value) {
        return this.player ? this.player.setProperty(name, value) : -1;
    }

    getRawProperty(name, format = "string") {
        return this.player ? this.player.getProperty(name, format) : null;
    }

    observeProperty(name, format = "string") {
        return this.player ? this.player.observeProperty(name, format) : -1;
    }

    unobserveAll() {
        return this.player ? this.player.unobserveAll() : -1;
    }

    getBackendInfo() {
        return this.player ? this.player.getBackendInfo() : {
            libmpvLoaded: false,
            libmpvPath: "",
            libmpvError: "Player destroyed",
            initialized: false,
            backend: "none",
            sharedTexture: false,
            hdrSupported: false,
        };
    }

    destroy() {
        if (this.player) {
            this.player.destroy();
            this.player = null;
        }
    }
}

module.exports = Mpv;
