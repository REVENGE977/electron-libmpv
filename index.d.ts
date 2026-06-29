declare module 'electron-libmpv' {
	interface MpvOptions {
		onEvent?: (event: any) => void;
	}
	
	class Mpv {
		constructor(options?: MpvOptions);
		
		attach(
			windowBuffer: Buffer, 
			x: number, 
			y: number, 
			width: number, 
			height: number
		): boolean;
		
		resize(
			x: number, 
			y: number, 
			width: number, 
			height: number
		): void;
		
		command(...args: string[]): number;
		property(name: string, value: string | number | boolean): number;
		getRawProperty(name: string): string | null;
	}
	
	export = Mpv;
}