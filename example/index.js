const { app, BrowserWindow, ipcMain } = require("electron");
const path = require("path");

const Mpv = require("../index.js"); 

let mainWindow;
let player;

app.commandLine.appendSwitch('use-angle', 'd3d11');
app.commandLine.appendSwitch('ignore-gpu-blocklist');
app.commandLine.appendSwitch('enable-zero-copy');
app.commandLine.appendSwitch('enable-gpu-rasterization');
app.commandLine.appendSwitch('enable-accelerated-video-decode');

function createWindow() {
	mainWindow = new BrowserWindow({
		width: 1200,
		height: 800,
		webPreferences: {
			nodeIntegration: true,
			contextIsolation: false
		}
	});
	
	player = new Mpv({
		onEvent: () => {
			if (!mainWindow.isDestroyed()) {
				mainWindow.webContents.send("mpv-event");
			}
		}
	});
	
	ipcMain.on("mpv-attach", (event, x, y, width, height) => {
		const handle = mainWindow.getNativeWindowHandle();
		event.returnValue = player.attach(handle, x, y, width, height);
	});
	
	ipcMain.on("mpv-resize", (event, x, y, width, height) => {
		player.resize(x, y, width, height);
	});
	
	ipcMain.on("mpv-command", (event, ...args) => {
		player.command(...args);
	});
	
	ipcMain.on("mpv-set-property", (event, name, value) => {
		player.property(name, value);
	});
	
	ipcMain.on("mpv-get-property", (event, name) => {
		event.returnValue = player.getRawProperty(name);
	});
	
	mainWindow.loadFile(path.join(__dirname, "index.html"));
}

app.whenReady().then(createWindow);

app.on("window-all-closed", () => { 
	if (process.platform !== "darwin") app.quit(); 
});