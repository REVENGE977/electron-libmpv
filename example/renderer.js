"use strict";

const { ipcRenderer } = require("electron");
const path = require("path");

const placeholder = document.getElementById("video-placeholder");
const seekBar = document.getElementById("seek-bar");
const timeCurrent = document.getElementById("time-current");
const timeTotal = document.getElementById("time-total");
const togglePlayBtn = document.getElementById("toggle-play-btn");

let isUserDragging = false; 
let isFileLoaded = false;
let isAttached = false;
let resizeDebounceTimeout = null;

ipcRenderer.on("mpv-event", () => updateTimelineMetrics());

const syncVideoGeometry = () => {
	if (!isAttached || !placeholder) return;
	
	clearTimeout(resizeDebounceTimeout);
	resizeDebounceTimeout = setTimeout(() => {
		const rect = placeholder.getBoundingClientRect();
		ipcRenderer.send("mpv-resize", rect.left, rect.top, rect.width, rect.height);
	}, 100);
};

if (placeholder) {
	const resizeObserver = new ResizeObserver(() => syncVideoGeometry());
	resizeObserver.observe(placeholder);
}

function formatTime(seconds) {
	if (isNaN(seconds) || seconds === null) return "00:00";
	const mins = Math.floor(seconds / 60).toString().padStart(2, '0');
	const secs = Math.floor(seconds % 60).toString().padStart(2, '0');
	return `${mins}:${secs}`;
}

function updateTimelineMetrics() {
	if (isUserDragging) return;
	
	const rawCurrent = ipcRenderer.sendSync("mpv-get-property", "time-pos");
	const rawDuration = ipcRenderer.sendSync("mpv-get-property", "duration");
	
	if (rawCurrent && rawDuration) {
		const current = parseFloat(rawCurrent);
		const duration = parseFloat(rawDuration);
		
		timeCurrent.innerText = formatTime(current);
		timeTotal.innerText = formatTime(duration);
		seekBar.value = (current / duration) * 100;
	}

	if (isFileLoaded) {
		const hwdecActive = ipcRenderer.sendSync("mpv-get-property", "hwdec-current");
		const containerFps = ipcRenderer.sendSync("mpv-get-property", "container-fps");
		const estimatedFps = ipcRenderer.sendSync("mpv-get-property", "estimated-vf-fps");

		console.log(`--- MPV Diagnostics ---`);
		console.log(`Hardware Decoding : ${hwdecActive === "no" ? "Software Mode" : `Active (${hwdecActive})`}`);
		console.log(`Container Target  : ${containerFps ? parseFloat(containerFps).toFixed(2) : "Unknown"} FPS`);
		console.log(`Real-time Display : ${estimatedFps ? parseFloat(estimatedFps).toFixed(2) : "Unknown"} FPS`);
  	}
}

setInterval(() => {
	updateTimelineMetrics();
}, 1000);

seekBar.addEventListener("mousedown", () => { isUserDragging = true; });
seekBar.addEventListener("change", () => {
	const rawDuration = ipcRenderer.sendSync("mpv-get-property", "duration");
	if (rawDuration) {
		const duration = parseFloat(rawDuration);
		const targetSeconds = (seekBar.value / 100) * duration;
		ipcRenderer.send("mpv-command", "seek", targetSeconds.toString(), "absolute");
	}
	isUserDragging = false;
});

togglePlayBtn.addEventListener("click", () => {  
	if (!isFileLoaded) {
		const rect = placeholder.getBoundingClientRect();
		
		ipcRenderer.sendSync("mpv-attach", rect.left, rect.top, rect.width, rect.height);
		isAttached = true;
		
		ipcRenderer.send("mpv-command", "loadfile", "http://127.0.0.1:11470/0859a4efa3bf1a93edd68db449f0bb372af51a2b/0?");
		ipcRenderer.send("mpv-set-property", "pause", "no");
		
		togglePlayBtn.innerText = "❚❚ Pause";
		isFileLoaded = true;
		return;
	}
	
	const isCurrentlyPaused = ipcRenderer.sendSync("mpv-get-property", "pause");
	if (isCurrentlyPaused === "yes") {
		ipcRenderer.send("mpv-set-property", "pause", "no");
		togglePlayBtn.innerText = "❚❚ Pause";
	} else {
		ipcRenderer.send("mpv-set-property", "pause", "yes");
		togglePlayBtn.innerText = "▶ Play";
	}
});