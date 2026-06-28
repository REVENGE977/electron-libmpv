"use strict";

const path = require("path");
const binaryPath = path.join(__dirname, "build", "Release", "electron-libmpv.node");
const { MpvPlayer } = require(binaryPath);

class Mpv {
	constructor(options = {}) {
		this.player = new MpvPlayer({
			onEvent: options.onEvent || (() => {})
		});
	}
	
	attach(windowBuffer, x, y, width, height) {
		if (!this.player) return false;
		return this.player.attach(windowBuffer, Math.round(x), Math.round(y), Math.round(width), Math.round(height));
	}
	
	resize(x, y, width, height) {
		if (!this.player) return;
		this.player.resize(Math.round(x), Math.round(y), Math.round(width), Math.round(height));
	}
	
	command(...args) { return this.player ? this.player.command(...args) : -1; }
	property(name, value) { return this.player ? this.player.setProperty(name, value.toString()) : -1; -1; }
	getRawProperty(name) { return this.player ? this.player.getProperty(name) : null; }
}

module.exports = Mpv;